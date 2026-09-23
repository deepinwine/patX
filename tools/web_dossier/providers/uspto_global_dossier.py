"""USPTO Global Dossier provider - public JSON API, no key, no login.

globaldossier.uspto.gov is an Angular SPA whose data comes from a public
CloudFront JSON service (observed live, Sept 2026):

    GET  {API}/patent-family/svc/family/application/CN/{12-digit app}
         -> {"list": [{"countryCode": "CN", "docList": {"docs": [...]}}]}
    POST {API}/doc-list/svc/doclist/process
         body {"request": [{"docNumber", "country": "CN", "kindCode": "A"}]}
         -> SSE lines "data:{...}" with the same doc payload

Doc rows carry docCode ("210401-CN"), docDesc ("First notice of examination
opinions (ORIGINAL)"), legalDateStr (MM/DD/YYYY) and the OneDOC docId - the
same identifiers the EPO register serves, so event_keys collapse across
sources. Only ORIGINAL official outgoing documents are kept; TRANSLATED twins
and applicant submissions are dropped. Requests are paced - the service rate
limits aggressively (429).
"""
from __future__ import annotations

import json
import re
import time
from typing import Optional, Tuple

from ..document_classifier import (classify_official_document, direction_for,
                                   event_title_cn)
from ..models import (REMINDABLE_TYPES, ProsecutionDocument, ResultCode,
                      normalize_date)
from ..number_resolver import normalize_cn_identifier
from .base import DossierProvider, SyncOutcome

API_BASE = "https://d1kazzu6rbodne.cloudfront.net"
FAMILY_URL = API_BASE + "/patent-family/svc/family/application/CN/{application}"
DOCLIST_URL = API_BASE + "/doc-list/svc/doclist/process"

_HEADERS = {
    "User-Agent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                  "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0 Safari/537.36",
    "Accept": "application/json, text/plain, */*",
    "Origin": "https://globaldossier.uspto.gov",
    "Referer": "https://globaldossier.uspto.gov/",
}

_MIN_INTERVAL_SECONDS = 2.0     # polite pacing; the service 429s bursts
_last_request_at = 0.0

_US_DATE_RE = re.compile(r"^(\d{1,2})/(\d{1,2})/(\d{4})$")


def fetch_json_public(url: str, post_body: Optional[str] = None) -> Tuple[int, str]:
    """Plain urllib GET/POST with browser-ish headers; (status, body)."""
    import urllib.request
    import urllib.error
    global _last_request_at
    wait = _MIN_INTERVAL_SECONDS - (time.monotonic() - _last_request_at)
    if wait > 0:
        time.sleep(wait)
    _last_request_at = time.monotonic()

    data = post_body.encode("utf-8") if post_body is not None else None
    headers = dict(_HEADERS)
    if data is not None:
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return resp.getcode(), resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as exc:
        try:
            body = exc.read().decode("utf-8", "replace")
        except Exception:
            body = ""
        return exc.code, body
    except (urllib.error.URLError, TimeoutError, OSError):
        return 0, ""     # transport trouble: NETWORK_ERROR, never a crash


def _parse_us_date(text: str) -> str:
    m = _US_DATE_RE.fullmatch((text or "").strip())
    if not m:
        return normalize_date(text)
    return normalize_date(f"{m.group(3)}-{m.group(1)}-{m.group(2)}")


def _strip_version(title: str):
    m = re.search(r"\((ORIGINAL|TRANSLATED)\)\s*$", title.strip(), re.IGNORECASE)
    if not m:
        return title.strip(), ""
    return title[:m.start()].strip(), m.group(1).upper()


def parse_uspto_doc_payload(body: str, application_number: str,
                            publication_number: str) -> SyncOutcome:
    """Parse the family JSON (or the SSE doclist fallback) into documents."""
    app_no = (application_number or "").strip()
    if not body or not body.strip():
        return SyncOutcome(code=ResultCode.TEMPORARY_ERROR,
                           message="USPTO Global Dossier 返回空白响应")
    if "429" in body[:200] and "RATE LIMITED" in body[:200].upper():
        return SyncOutcome(code=ResultCode.RATE_LIMITED,
                           message="USPTO Global Dossier 限流（429），稍后重试")

    # SSE fallback: keep only the data:{...} event lines
    if body.lstrip().startswith("data:"):
        merged_docs = []
        for line in body.splitlines():
            line = line.strip()
            if not line.startswith("data:"):
                continue
            try:
                event = json.loads(line[5:])
            except ValueError:
                return SyncOutcome(code=ResultCode.PAGE_STRUCTURE_CHANGED,
                                   message="USPTO doclist SSE 解析失败")
            merged_docs.extend(event.get("docs", []))
        payload_docs, cn_member = merged_docs, True
    else:
        try:
            payload = json.loads(body)
        except ValueError:
            return SyncOutcome(code=ResultCode.PAGE_STRUCTURE_CHANGED,
                               message="USPTO Global Dossier 响应不是 JSON")
        cn_member = False
        payload_docs = []
        for member in payload.get("list", []):
            if member.get("countryCode") == "CN":
                cn_member = True
                payload_docs = member.get("docList", {}).get("docs", []) or []
                break
        if not cn_member:
            return SyncOutcome(code=ResultCode.CASE_NOT_FOUND,
                               message="USPTO Global Dossier 未找到该 CN 案件")

    documents = []
    for doc in payload_docs:
        raw_title, version = _strip_version(str(doc.get("docDesc") or ""))
        if not raw_title:
            continue
        if version == "TRANSLATED":
            continue                      # machine translation twin
        code = str(doc.get("docCode") or "")
        doc_type, ordinal, confidence = classify_official_document(raw_title, code)
        if direction_for(doc_type) != "official":
            continue                      # applicant submission
        if doc_type not in REMINDABLE_TYPES:
            continue                      # search reports etc.: not tracked events
        documents.append(ProsecutionDocument(
            jurisdiction="CN",
            application_number=app_no,
            publication_number=(publication_number or "").strip(),
            source="uspto_global_dossier",
            remote_document_id=str(doc.get("docId") or ""),
            document_type=doc_type,
            document_title=event_title_cn(doc_type, ordinal, raw_title),
            raw_title=raw_title,
            official_date=_parse_us_date(str(doc.get("legalDateStr") or "")),
            direction="official",
            confidence=confidence,
            oa_ordinal=ordinal,
            document_code=code,
            document_version=version or "ORIGINAL",
            source_trace=["uspto_global_dossier"],
        ))
    return SyncOutcome(code=ResultCode.OK, documents=documents,
                       resolved_application_number=app_no)


class UsptoGlobalDossierProvider(DossierProvider):
    provider_id = "uspto_global_dossier"
    jurisdiction = "CN"

    def __init__(self, fetch_json=None):
        self._fetch_json = fetch_json or fetch_json_public

    def health_check(self) -> bool:
        return True

    def check_auth(self, cancel) -> ResultCode:
        return ResultCode.OK

    def ensure_login(self, cancel) -> ResultCode:
        return ResultCode.OK

    def resolve_case(self, application_number, publication_number, cancel):
        return SyncOutcome(code=ResultCode.OK,
                           resolved_application_number=application_number)

    def list_documents(self, application_number: str, publication_number: str,
                       cancel) -> SyncOutcome:
        ident = normalize_cn_identifier(application_number)
        if not ident.valid or ident.number_type != "application":
            return SyncOutcome(code=ResultCode.RESOLVE_FAILED,
                               message="USPTO Global Dossier 需要可识别的 CN 申请号")
        if cancel is not None and cancel.is_set():
            return SyncOutcome(code=ResultCode.TEMPORARY_ERROR, message="已取消")

        status, body = self._fetch_json(FAMILY_URL.format(application=ident.number))
        if status == 429:
            return SyncOutcome(code=ResultCode.RATE_LIMITED,
                               message="USPTO Global Dossier 限流（429）")
        if status == 404:
            return SyncOutcome(code=ResultCode.CASE_NOT_FOUND,
                               message="USPTO Global Dossier 未收录该案件（可能尚未公开）")
        if status != 200:
            return SyncOutcome(code=ResultCode.NETWORK_ERROR,
                               message=f"USPTO Global Dossier HTTP {status}"
                               if status else "USPTO Global Dossier 连接失败")
        outcome = parse_uspto_doc_payload(body, ident.normalized_number,
                                          publication_number)
        if outcome.code == ResultCode.CASE_NOT_FOUND:
            return outcome
        # family payload without a usable docList -> SSE refresh endpoint
        if outcome.code == ResultCode.PAGE_STRUCTURE_CHANGED:
            post = json.dumps({"request": [{"docNumber": ident.number,
                                            "country": "CN", "kindCode": "A"}]})
            status, body = self._fetch_json(DOCLIST_URL, post_body=post)
            if status == 429:
                return SyncOutcome(code=ResultCode.RATE_LIMITED,
                                   message="USPTO Global Dossier 限流（429）")
            if status != 200:
                return SyncOutcome(code=ResultCode.NETWORK_ERROR,
                                   message=f"USPTO Global Dossier HTTP {status}")
            return parse_uspto_doc_payload(body, ident.normalized_number,
                                           publication_number)
        return outcome

    def download_document(self, document, dest_path, cancel):
        return ResultCode.UNSUPPORTED_JURISDICTION
