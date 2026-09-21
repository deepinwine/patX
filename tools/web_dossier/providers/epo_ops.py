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
from ..number_resolver import normalize_cn_identifier

OPS_BASE = "https://ops.epo.org/3.2"


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
        self.base = base_url.rstrip("/")
        self._token = ""
        self._token_expiry = 0.0
        self.last_family: List[FamilyMember] = []

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
        basic = base64.b64encode(f"{self.key}:{self.secret}".encode()).decode()
        body = urllib.parse.urlencode({"grant_type": "client_credentials"}).encode()
        req = urllib.request.Request(
            self.base + "/auth/accesstoken", data=body,
            headers={"Authorization": "Basic " + basic,
                     "Content-Type": "application/x-www-form-urlencoded"})
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                payload = json.loads(resp.read().decode("utf-8", "replace"))
            self._token = payload.get("access_token", "")
            ttl = int(payload.get("expires_in", "1200") or 1200)
            self._token_expiry = time.monotonic() + max(60, ttl - 120)
            return self._token or None
        except urllib.error.HTTPError as exc:
            if exc.code in (401, 403):
                return None
            return None
        except Exception:
            return None

    def _get_json(self, path: str):
        """GET with bearer token, JSON accepted; (status, obj-or-None, raw)."""
        token = self._fetch_token()
        if not token:
            return 401, None, ""
        url = self.base + path
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
        status, obj, _ = self._get_json(
            "/number-service/application/epodoc/" + urllib.parse.quote(epodoc))
        if status == 404:
            # Token OK but every service 404s = the app has no OPS product
            # attached on the developer portal (only auth is routed).
            return SyncOutcome(code=ResultCode.RESOLVE_FAILED,
                               message=f"EPO 数据服务 404：应用可能未订阅 OPS 产品"
                                       f"（developers.epo.org → 你的应用 → Products 添加），"
                                       f"或 {epodoc} 无记录")
        if status != 200 or obj is None:
            return SyncOutcome(code=ResultCode.NETWORK_ERROR,
                               message=f"EPO 号码服务 HTTP {status}")
        apps = parse_number_service(obj)
        if not apps:
            return SyncOutcome(code=ResultCode.RESOLVE_FAILED,
                               message=f"{epodoc} 未解析出申请号")
        return SyncOutcome(code=ResultCode.OK,
                           resolved_application_number=apps[0],
                           message="; ".join(apps))

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


def parse_number_service(obj) -> List[str]:
    """Extracts application numbers from a number-service response."""
    apps = []
    for doc in _walk(obj, "exchange-document"):
        for doc_id in _doc_ids(doc):
            if doc_id.get("@format") == "epodoc" and str(
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
    for value in _walk(obj, "family-member"):
        if isinstance(value, list):
            raw_members.extend(value)
        else:
            raw_members.append(value)
    for fm in raw_members:
        member = FamilyMember()
        refs = _walk(fm, "application-reference")
        for ref in refs:
            for doc_id in _doc_ids(ref):
                if doc_id.get("@format") == "docdb":
                    def g(name, d=doc_id):
                        v = d.get(name)
                        if isinstance(v, dict):
                            v = v.get("$")
                        return str(v) if v is not None else ""
                    member.authority = g("country")
                    member.application_number = g("country") + g("doc-number")
                    date = ref.get("date") or doc_id.get("date")
                    if isinstance(date, dict):
                        date = date.get("$")
                    member.application_date = str(date or "")[:10]
                    break
            if member.application_number:
                break
        pubs = []
        for doc_id in _doc_ids(fm):
            if doc_id.get("@format") == "docdb":
                pubs.append(_id_text(doc_id))
        if pubs:
            member.publication_number = pubs[0]
            m = re.match(r"^[A-Z]{2}\d+([A-Z]\d?)$", pubs[0])
            member.kind = m.group(1) if m else ""
        if member.application_number or member.publication_number:
            members.append(member)
    return members
