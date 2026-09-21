"""EPO OPS provider - pure REST (no browser).

Powers the two things the CNIPA web sync cannot do itself:
  - number service: publication number -> application number (any country)
  - DOCDB family: which applications/publications belong to the same patent
    family (CN case -> US/EP/WO counterparts)

Auth: OAuth2 client_credentials with the consumer key/secret of a registered
application (https://developers.epo.org). Credentials arrive per request
from the C++ side (stored in the local, git-ignored config table) and are
never logged. Non-paying quota (2 GB/month, ~4 req/s) is ample for periodic
case management.
"""
from __future__ import annotations

import base64
import json
import re
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from typing import List, Optional

from .base import DossierProvider, SyncOutcome

from ..models import ResultCode
from ..number_resolver import api_application_number, normalize_cn_identifier

OPS_BASE = "https://ops.epo.org/3.2"
_TOKEN_CACHE: dict = {}
# Data endpoints live under /rest-services on the current gateway (auth does
# not); verified live 2026-09 and cross-checked against patent-dev/epo-ops.
DATA_BASE = OPS_BASE + "/rest-services"


@dataclass
class FamilyMember:
    authority: str = ""            # CN / US / EP / WO ...
    application_number: str = ""   # docdb form, e.g. US202410123456A? raw doc-number
    application_date: str = ""
    publication_number: str = ""   # e.g. US20240270309A1 (country+number+kind)
    kind: str = ""

    def to_dict(self) -> dict:
        return {"authority": self.authority,
                "application_number": self.application_number,
                "application_date": self.application_date,
                "publication_number": self.publication_number,
                "kind": self.kind}


def _epodoc_publication(raw: str) -> str:
    """Best-effort epodoc publication id (CCnnnnnnnnnK). Returns '' when the
    input does not look like a publication."""
    s = re.sub(r"\s+", "", (raw or "").upper()).removeprefix("CN")
    ident = normalize_cn_identifier(raw)
    if ident.number_type == "publication" and ident.country_code == "CN":
        return "CN" + ident.number + ident.kind_code.upper()
    m = re.match(r"^([A-Z]{2})(\d{6,12})([A-Z]\d?)$", s)
    if m:
        return "".join(m.groups())
    m = re.match(r"^([A-Z]{2})(\d{6,12})$", s)
    return "".join(m.groups()) if m else ""


class EpoOpsProvider(DossierProvider):
    provider_id = "epo_ops"
    jurisdiction = "MULTI"

    def __init__(self, consumer_key: str = "", consumer_secret: str = "",
                 base_url: str = OPS_BASE):
        self.key = consumer_key or ""
        self.secret = consumer_secret or ""
        self.base = base_url.rstrip("/")           # auth base
        self.data_base = self.base + "/rest-services"
        self._token = ""
        self._token_expiry = 0.0
        self.last_family: List[FamilyMember] = []
        self.last_events: List[dict] = []
        self.last_citations: List[str] = []

    # ---- auth ----------------------------------------------------------
    def health_check(self) -> bool:
        return bool(self.key and self.secret)

    def check_auth(self, cancel) -> ResultCode:
        if not self.health_check():
            return ResultCode.RESOLVE_FAILED
        if self._fetch_token() is None:
            return ResultCode.AUTH_REQUIRED
        return ResultCode.OK

    def ensure_login(self, cancel) -> ResultCode:
        # No browser involved: the OAuth token IS the login.
        return ResultCode.OK if self._fetch_token() is not None else ResultCode.AUTH_REQUIRED

    def _fetch_token(self) -> Optional[str]:
        if self._token and time.monotonic() < self._token_expiry:
            return self._token
        # Module-level cache: the sidecar builds a fresh provider per request,
        # and the token endpoint throttles frequent fetches - reuse the token
        # across instances for the same credentials.
        cached = _TOKEN_CACHE.get(self.key)
        if cached and time.monotonic() < cached[1]:
            self._token, self._token_expiry = cached
            return self._token
        basic = base64.b64encode(f"{self.key}:{self.secret}".encode()).decode()
        body = urllib.parse.urlencode({"grant_type": "client_credentials"}).encode()
        for attempt in range(3):
            req = urllib.request.Request(
                self.base + "/auth/accesstoken", data=body,
                headers={"Authorization": "Basic " + basic,
                         "Content-Type": "application/x-www-form-urlencoded"})
            try:
                with urllib.request.urlopen(req, timeout=30) as resp:
                    payload = json.loads(resp.read().decode("utf-8", "replace"))
                token = payload.get("access_token", "")
                ttl = int(payload.get("expires_in", "1200") or 1200)
                expiry = time.monotonic() + max(60, ttl - 120)
                if token:
                    self._token, self._token_expiry = token, expiry
                    _TOKEN_CACHE[self.key] = (token, expiry)
                return token or None
            except urllib.error.HTTPError as exc:
                if exc.code in (429, 500, 502, 503):
                    time.sleep(2 ** attempt)
                    continue
                return None
            except Exception:
                time.sleep(1)
                continue
        return None

    def _get_json(self, path: str):
        """GET with bearer token, JSON accepted; (status, obj-or-None, raw)."""
        token = self._fetch_token()
        if not token:
            return 401, None, ""
        url = self.data_base + path
        last_status = 0
        for attempt in range(3):
            req = urllib.request.Request(
                url, headers={"Authorization": "Bearer " + token,
                              "Accept": "application/json"})
            try:
                with urllib.request.urlopen(req, timeout=45) as resp:
                    raw = resp.read().decode("utf-8", "replace")
                    return resp.status, json.loads(raw), raw
            except urllib.error.HTTPError as exc:
                last_status = exc.code
                if exc.code in (429, 500, 502, 503):   # back off and retry
                    time.sleep(2 ** attempt)
                    continue
                return exc.code, None, ""
            except (urllib.error.URLError, TimeoutError, OSError):
                time.sleep(2 ** attempt)
                last_status = 0
        return last_status or 0, None, ""

    # ---- number service -------------------------------------------------
    def resolve_publication(self, publication_number: str) -> SyncOutcome:
        epodoc = _epodoc_publication(publication_number)
        if not epodoc:
            return SyncOutcome(code=ResultCode.RESOLVE_FAILED,
                               message=f"无法识别公开号 {publication_number}")
        # number-service only accepts POST batches on the current gateway;
        # the family response carries the same information (each member's
        # application-reference), so resolve through it and skip the flaky
        # service entirely.
        out = self.family(publication_number)
        if not out.ok:
            return out
        wanted = epodoc.upper()
        for m in self.last_family:
            if m.publication_number.upper() == wanted and m.application_number:
                return SyncOutcome(code=ResultCode.OK,
                                   resolved_application_number=m.application_number)
        return SyncOutcome(code=ResultCode.RESOLVE_FAILED,
                           message=f"{epodoc} 未在同族中解析出申请号")

    def resolve_case(self, application_number: str, publication_number: str,
                     cancel) -> SyncOutcome:
        return self.resolve_publication(publication_number or application_number)

    # ---- family ----------------------------------------------------------
    def family(self, publication_number: str) -> SyncOutcome:
        epodoc = _epodoc_publication(publication_number)
        if not epodoc:
            return SyncOutcome(code=ResultCode.RESOLVE_FAILED,
                               message=f"无法识别公开号 {publication_number}")
        docdb = epodoc_to_docdb(epodoc)
        status, obj, _ = self._get_json(
            "/family/publication/docdb/" + urllib.parse.quote(docdb))
        if status == 404:
            return SyncOutcome(code=ResultCode.CASE_NOT_FOUND,
                               message=f"EPO 无 {epodoc} 同族记录")
        if status != 200 or obj is None:
            return SyncOutcome(code=ResultCode.NETWORK_ERROR,
                               message=f"EPO 同族服务 HTTP {status}")
        members = parse_family(obj)
        if not members:
            return SyncOutcome(code=ResultCode.CASE_NOT_FOUND,
                               message=f"{epodoc} 同族为空或结构未识别")
        self.last_family = members
        return SyncOutcome(code=ResultCode.OK,
                           message=f"同族 {len(members)} 件")


    # ---- legal status (INPADOC) ------------------------------------------
    def legal_status(self, application_number: str, publication_number: str = "") -> SyncOutcome:
        """INPADOC events for the case (CN events included: 实审进入/驳回/
        授权/年费等)。Any event newer than `since` (YYYY-MM-DD) counts as an
        update signal. Results land in self.last_events."""
        app13 = api_application_number(application_number)
        if not app13:
            epodoc = _epodoc_publication(publication_number)
            if not epodoc:
                return SyncOutcome(code=ResultCode.RESOLVE_FAILED,
                                   message="缺少可识别的申请号/公开号")
            docdb = epodoc_to_docdb(epodoc)
            path = "/legal/publication/docdb/" + urllib.parse.quote(docdb)
        else:
            path = "/legal/application/docdb/" + \
                urllib.parse.quote(f"CN.{app13[:12]}.A")
        status, obj, _ = self._get_json(path)
        if status == 404:
            self.last_events = []
            return SyncOutcome(code=ResultCode.CASE_NOT_FOUND, message="EPO 无法律状态记录")
        if status != 200 or obj is None:
            return SyncOutcome(code=ResultCode.NETWORK_ERROR,
                               message=f"EPO 法律状态 HTTP {status}")
        events = parse_legal_events(obj)
        self.last_events = events
        if not events:
            return SyncOutcome(code=ResultCode.OK, message="法律状态事件为空")
        return SyncOutcome(code=ResultCode.OK,
                           message=f"{len(events)} 条法律事件，最新 {events[0]['date']}")

    # ---- cited documents (对比文件) ---------------------------------------
    def citations(self, application_number: str, publication_number: str = "") -> SyncOutcome:
        """DOCDB 引用文献（审查检索/对比文件）。Results in self.last_citations."""
        app13 = api_application_number(application_number)
        if app13:
            path = "/published-data/application/docdb/" + \
                urllib.parse.quote(f"CN.{app13[:12]}.A") + "/biblio"
        else:
            epodoc = _epodoc_publication(publication_number)
            if not epodoc:
                return SyncOutcome(code=ResultCode.RESOLVE_FAILED,
                                   message="缺少可识别的申请号/公开号")
            path = "/published-data/publication/docdb/" + \
                urllib.parse.quote(epodoc_to_docdb(epodoc)) + "/biblio"
        status, obj, _ = self._get_json(path)
        if status == 404:
            self.last_citations = []
            return SyncOutcome(code=ResultCode.CASE_NOT_FOUND, message="EPO 无书目记录")
        if status != 200 or obj is None:
            return SyncOutcome(code=ResultCode.NETWORK_ERROR,
                               message=f"EPO 书目 HTTP {status}")
        cites = parse_citations(obj)
        self.last_citations = cites
        return SyncOutcome(code=ResultCode.OK,
                           message=f"{len(cites)} 件引用文献" if cites else "无引用文献记录")


    # ---- DossierProvider plumbing (CNIPA does the OA dossier) ------------
    def list_documents(self, application_number: str, publication_number: str,
                       cancel) -> SyncOutcome:
        return SyncOutcome(code=ResultCode.UNSUPPORTED_JURISDICTION,
                           message="EPO provider 只做号码解析与同族，审查文件由 CNIPA provider 提供")

    def download_document(self, document, dest_path: str, cancel):
        return ResultCode.UNSUPPORTED_JURISDICTION, {"message": "EPO 不提供文档下载"}


def epodoc_to_docdb(epodoc: str) -> str:
    """CN119870049A -> CN.119870049.A (docdb path form)."""
    m = re.match(r"^([A-Z]{2})(\d+)([A-Z]\d?)?$", epodoc)
    if not m:
        return epodoc
    cc, num, kind = m.group(1), m.group(2), m.group(3) or ""
    return f"{cc}.{num}.{kind}" if kind else f"{cc}.{num}"


# ---------------------------------------------------------------------------
# Pure parsers (fixture-tested offline)
# ---------------------------------------------------------------------------

def _walk(obj, key):
    """Yield every value stored under `key` at any depth of a nested
    dict/list structure (OPS JSON nests shapes vary by result count)."""
    if isinstance(obj, dict):
        for k, v in obj.items():
            if k == key:
                yield v
            yield from _walk(v, key)
    elif isinstance(obj, list):
        for item in obj:
            yield from _walk(item, key)


def _walk_keys(obj, keys):
    """Like _walk but matches local names ignoring the XML namespace prefix
    (live responses carry ops:family-member, fixtures may not)."""
    suffixes = tuple(":" + k for k in keys)
    if isinstance(obj, dict):
        for k, v in obj.items():
            if k in keys or k.endswith(suffixes):
                yield v
            yield from _walk_keys(v, keys)
    elif isinstance(obj, list):
        for item in obj:
            yield from _walk_keys(item, keys)


def _doc_ids(entry) -> List[dict]:
    ids = []
    for v in _walk(entry, "document-id"):
        if isinstance(v, dict):
            ids.append(v)
        elif isinstance(v, list):
            ids.extend(x for x in v if isinstance(x, dict))
    return ids


def _id_text(doc_id: dict) -> str:
    def g(name):
        v = doc_id.get(name)
        if isinstance(v, dict):
            v = v.get("$")
        return str(v) if v is not None else ""
    return f"{g('country')}{g('doc-number')}{g('kind')}"


def _is_docdb(doc_id: dict) -> bool:
    """The docdb discriminator moved between generations: fixtures use
    @format, live responses use @document-id-type."""
    return doc_id.get("@format") == "docdb" or \
        doc_id.get("@document-id-type") == "docdb"


def parse_number_service(obj) -> List[str]:
    """Extracts application numbers from a number-service response."""
    apps = []
    for doc in _walk(obj, "exchange-document"):
        for doc_id in _doc_ids(doc):
            if (doc_id.get("@format") == "epodoc" or
                    doc_id.get("@document-id-type") == "epodoc") and str(
                    doc_id.get("type", "")) != "publication":
                text = _id_text(doc_id)
                if text and text not in apps:
                    apps.append(text)
    return apps


def parse_family(obj) -> List[FamilyMember]:
    """Extracts family members from a docdb family response. Each member
    contributes its earliest publication and its application reference."""
    members: List[FamilyMember] = []
    raw_members: List = []
    for value in _walk_keys(obj, ("family-member",)):
        if isinstance(value, list):
            raw_members.extend(value)
        else:
            raw_members.append(value)
    for fm in raw_members:
        member = FamilyMember()
        refs = _walk(fm, "application-reference")
        for ref in refs:
            for doc_id in _doc_ids(ref):
                if _is_docdb(doc_id):
                    def g(name, d=doc_id):
                        v = d.get(name)
                        if isinstance(v, dict):
                            v = v.get("$")
                        return str(v) if v is not None else ""
                    member.authority = g("country")
                    member.application_number = g("country") + g("doc-number")
                    date = doc_id.get("date") or ref.get("date")
                    if isinstance(date, dict):
                        date = date.get("$")
                    date = str(date or "")[:10]
                    if len(date) == 8 and date.isdigit():   # 19991108 -> ISO
                        date = date[:4] + "-" + date[4:6] + "-" + date[6:]
                    member.application_date = date
                    break
            if member.application_number:
                break
        pubs = []
        for doc_id in _doc_ids(fm):
            if _is_docdb(doc_id):
                pubs.append(_id_text(doc_id))
        if pubs:
            member.publication_number = pubs[0]
            m = re.match(r"^[A-Z]{2}\d+([A-Z]\d?)$", pubs[0])
            member.kind = m.group(1) if m else ""
        if member.application_number or member.publication_number:
            members.append(member)
    return members


def parse_legal_events(obj) -> List[dict]:
    """INPADOC events from a /legal response. Each event dict carries
    date/code/description; sorted newest first."""
    events: List[dict] = []
    for value in _walk_keys(obj, ("legal",)):
        items = value if isinstance(value, list) else [value]
        for e in items:
            if not isinstance(e, dict) or "@code" not in e:
                continue
            date = ""
            gazette = e.get("ops:L007EP")
            if isinstance(gazette, dict):
                date = str(gazette.get("$", ""))[:10]
            if not date or date.startswith("0001"):
                date = str(e.get("@dateMigr", ""))[:10]
            events.append({
                "date": date,
                "code": str(e.get("@code", "")).strip(),
                "description": str(e.get("@desc", "")).strip(),
            })
    events.sort(key=lambda e: e["date"], reverse=True)
    return events


def parse_citations(obj) -> List[str]:
    """Docdb cited documents (对比文件) from a biblio response."""
    cited: List[str] = []
    for rc in _walk_keys(obj, ("references-cited",)):
        for cid in _walk_keys(rc, ("citation",)):
            items = cid if isinstance(cid, list) else [cid]
            for c in items:
                if not isinstance(c, dict):
                    continue
                for did_list in _walk_keys(c, ("document-id",)):
                    dis = did_list if isinstance(did_list, list) else [did_list]
                    for d in dis:
                        if isinstance(d, dict) and _is_docdb(d):
                            text = _id_text(d)
                            if text and text not in cited:
                                cited.append(text)
    return cited
