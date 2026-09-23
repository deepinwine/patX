"""EPO Patent Register public "Global Dossier" provider (no OPS key).

register.epo.org serves the Five-Office shared dossier metadata as a plain
HTML page; we fetch it with urllib (no browser) and keep only ORIGINAL
official outgoing documents, mirroring the USPTO Global Dossier provider so
both collapse to the same event_key.
"""
from __future__ import annotations

import re
from html.parser import HTMLParser
from typing import Optional, Tuple

from ..document_classifier import (classify_official_document, direction_for,
                                   event_title_cn)
from ..models import Confidence, ProsecutionDocument, ResultCode, normalize_date
from ..number_resolver import normalize_cn_identifier
from .base import DossierProvider, SyncOutcome

EPO_DOSSIER_URL = "https://register.epo.org/ipfwretrieve?apn=CN.{application}.A&lng=en"

_IDENTITY_MARKERS = ("european patent register", "global dossier", "epo register")
_INTERCEPT_MARKERS = ("access denied", "request blocked", "captcha", "forbidden")
_NO_DOC_MARKERS = ("no documents", "no data available", "no hits", "0 documents")
_DATE_RE = re.compile(r"^\d{4}-\d{1,2}-\d{1,2}$")
_EU_DATE_RE = re.compile(r"^(\d{1,2})\.(\d{1,2})\.(\d{4})$")


def fetch_html_public(url: str) -> Tuple[int, str]:
    """Plain urllib GET with a browser-ish UA; returns (status, body)."""
    import urllib.request
    import urllib.error
    req = urllib.request.Request(url, headers={
        "User-Agent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)",
        "Accept-Language": "en",
    })
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return resp.getcode(), resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as exc:
        try:
            body = exc.read().decode("utf-8", "replace")
        except Exception:
            body = ""
        return exc.code, body


class _RegisterDocParser(HTMLParser):
    """Collects <tr> rows (date / title / version) from the dossier table."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.rows = []
        self._in_row = False
        self._in_cell = False
        self._row = None
        self._cell = []

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "tr":
            self._in_row = True
            self._row = {"cells": [], "doc_id": attrs.get("id", "")}
        elif tag == "td" and self._in_row:
            self._in_cell = True
            self._cell = []

    def handle_endtag(self, tag):
        if tag == "td" and self._in_cell:
            self._in_cell = False
            self._row["cells"].append(" ".join("".join(self._cell).split()))
        elif tag == "tr" and self._in_row:
            self._in_row = False
            if self._row["cells"]:
                self.rows.append(self._row)
            self._row = None

    def handle_data(self, data):
        if self._in_cell:
            self._cell.append(data)


def _normalize_row_date(cell: str) -> str:
    if _DATE_RE.fullmatch(cell or ""):
        return normalize_date(cell)
    m = _EU_DATE_RE.fullmatch((cell or "").strip())
    if m:
        return normalize_date(f"{m.group(3)}-{m.group(2)}-{m.group(1)}")
    return ""


def parse_epo_document_list(html: str, application_number: str,
                            publication_number: str) -> SyncOutcome:
    """Pure parser over the EPO register Global Dossier table."""
    if not html or not html.strip():
        return SyncOutcome(code=ResultCode.TEMPORARY_ERROR,
                           message="EPO register 返回空白页面")
    low = html.lower()
    if any(m in low for m in _INTERCEPT_MARKERS):
        return SyncOutcome(code=ResultCode.ACCESS_DENIED,
                           message="EPO register 拒绝访问")
    if not any(m in low for m in _IDENTITY_MARKERS):
        return SyncOutcome(code=ResultCode.PAGE_STRUCTURE_CHANGED,
                           message="EPO register 页面身份无法确认")

    parser = _RegisterDocParser()
    try:
        parser.feed(html)
    except Exception:
        return SyncOutcome(code=ResultCode.PAGE_STRUCTURE_CHANGED,
                           message="EPO register HTML 解析失败")

    app_no = (application_number or "").strip()
    documents = []
    for row in parser.rows:
        date = ""
        version = ""
        title = ""
        for cell in row["cells"]:
            if not date and _normalize_row_date(cell):
                date = _normalize_row_date(cell)
            elif cell.upper() in ("ORIGINAL", "TRANSLATED"):
                version = cell.upper()
            elif cell != date and len(cell) > len(title):
                title = cell
        if not title:
            continue
        doc_type, ordinal, confidence = classify_official_document(title, "")
        if direction_for(doc_type) != "official":
            continue                      # applicant submission
        if version == "TRANSLATED":
            continue                      # machine translation twin
        documents.append(ProsecutionDocument(
            jurisdiction="CN",
            application_number=app_no,
            publication_number=(publication_number or "").strip(),
            source="epo_global_dossier",
            remote_document_id=row["doc_id"],
            document_type=doc_type,
            document_title=event_title_cn(doc_type, ordinal, title),
            raw_title=title,
            official_date=date,
            direction="official",
            confidence=Confidence.HIGH if (date and confidence == Confidence.HIGH)
            else Confidence.MEDIUM,
            oa_ordinal=ordinal,
            document_version=version or "ORIGINAL",
            source_trace=["epo_global_dossier"],
        ))
    if not parser.rows and not any(m in low for m in _NO_DOC_MARKERS):
        return SyncOutcome(code=ResultCode.PAGE_STRUCTURE_CHANGED,
                           message="EPO register 文档表格缺失")
    return SyncOutcome(code=ResultCode.OK, documents=documents,
                       resolved_application_number=app_no)


class EpoGlobalDossierProvider(DossierProvider):
    provider_id = "epo_global_dossier"
    jurisdiction = "CN"

    def __init__(self, fetch_html=None):
        self._fetch_html = fetch_html or fetch_html_public

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
                               message="EPO Global Dossier 需要可识别的 CN 申请号")
        if cancel is not None and cancel.is_set():
            return SyncOutcome(code=ResultCode.TEMPORARY_ERROR, message="已取消")
        status, html = self._fetch_html(
            EPO_DOSSIER_URL.format(application=ident.number))
        if status == 429:
            return SyncOutcome(code=ResultCode.RATE_LIMITED, message="EPO 限流")
        if status != 200:
            return SyncOutcome(code=ResultCode.NETWORK_ERROR,
                               message=f"EPO Global Dossier HTTP {status}")
        return parse_epo_document_list(html, ident.normalized_number,
                                       publication_number)

    def download_document(self, document, dest_path, cancel):
        return ResultCode.UNSUPPORTED_JURISDICTION
