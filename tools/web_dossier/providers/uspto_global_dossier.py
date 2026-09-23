"""USPTO Global Dossier public page provider (no API key, no login).

The site is a public Angular SPA: we render the page in a headless browser
context and parse the DOCUMENTED list out of the rendered DOM. Only ORIGINAL
official outgoing documents are kept; TRANSLATED twins and applicant
submissions are dropped so every provider emits the same event shape as CNIPA.
"""
from __future__ import annotations

import re
from html.parser import HTMLParser

from ..document_classifier import (classify_official_document, direction_for,
                                   event_title_cn)
from ..models import ProsecutionDocument, ResultCode, normalize_date
from ..number_resolver import normalize_cn_identifier
from .base import DossierProvider, SyncOutcome

APP_URL = "https://globaldossier.uspto.gov/#/result/application/CN/{application}/0"
PUB_URL = "https://globaldossier.uspto.gov/#/result/publication/CN/{publication}/1"

_IDENTITY_MARKERS = ("global dossier", "globaldossier")
_INTERCEPT_MARKERS = ("access denied", "request blocked", "captcha", "challenge")
_LOGIN_MARKERS = ("sign in", "log in", "please login")
_NO_DOC_MARKERS = ("no documents", "no data available", "no results")
_DATE_RE = re.compile(r"\d{4}-\d{1,2}-\d{1,2}")
_CODE_RE = re.compile(r"^\d{6}$")


class _DocumentTableParser(HTMLParser):
    """Collects <tr> rows (date / title / code cells + data-doc-id attrs)."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.identity_seen = False
        self.marker_texts = []
        self.rows = []            # list of {"cells": [...], "doc_id": ""}
        self._in_row = False
        self._in_cell = False
        self._row = None
        self._cell = []

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "tr":
            self._in_row = True
            self._row = {"cells": [], "doc_id": attrs.get("data-doc-id", "")}
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
        else:
            text = data.strip().lower()
            if text:
                self.marker_texts.append(text)

    # text inside <title>/<h1>/... is enough for identity/intercept detection
    def _page_text(self) -> str:
        return " ".join(self.marker_texts)

    page_text = property(_page_text)


def _strip_version(title: str):
    """'First notice ... (ORIGINAL)' -> ('First notice ...', 'ORIGINAL')."""
    m = re.search(r"\((ORIGINAL|TRANSLATED)\)\s*$", title.strip(), re.IGNORECASE)
    if not m:
        return title.strip(), ""
    return title[:m.start()].strip(), m.group(1).upper()


def parse_uspto_document_list(html: str, application_number: str,
                              publication_number: str) -> SyncOutcome:
    """Pure parser over the rendered Global Dossier document table."""
    if not html or not html.strip():
        return SyncOutcome(code=ResultCode.TEMPORARY_ERROR,
                           message="USPTO Global Dossier 返回空白页面")
    low = html.lower()
    parser = _DocumentTableParser()
    try:
        parser.feed(html)
    except Exception:
        return SyncOutcome(code=ResultCode.PAGE_STRUCTURE_CHANGED,
                           message="USPTO Global Dossier HTML 解析失败")
    text = parser.page_text
    if any(m in low for m in _INTERCEPT_MARKERS):
        return SyncOutcome(code=ResultCode.ACCESS_DENIED,
                           message="USPTO Global Dossier 拒绝访问")
    if not any(m in low for m in _IDENTITY_MARKERS):
        if any(m in low for m in _LOGIN_MARKERS):
            return SyncOutcome(code=ResultCode.ACCESS_DENIED,
                               message="USPTO Global Dossier 要求登录")
        return SyncOutcome(code=ResultCode.PAGE_STRUCTURE_CHANGED,
                           message="USPTO Global Dossier 页面身份无法确认")

    app_no = (application_number or "").strip()
    documents = []
    for row in parser.rows:
        date = next((c for c in row["cells"] if _DATE_RE.fullmatch(c)), "")
        code = next((c for c in row["cells"] if _CODE_RE.fullmatch(c)), "")
        title = ""
        for cell in row["cells"]:
            if cell != date and cell != code and len(cell) > len(title):
                title = cell
        if not title:
            continue
        raw_title, version = _strip_version(title)
        doc_type, ordinal, confidence = classify_official_document(
            raw_title, f"{code}-CN" if code else "")
        if direction_for(doc_type) != "official":
            continue                      # applicant submission
        if version == "TRANSLATED":
            continue                      # machine translation twin
        documents.append(ProsecutionDocument(
            jurisdiction="CN",
            application_number=app_no,
            publication_number=(publication_number or "").strip(),
            source="uspto_global_dossier",
            remote_document_id=row["doc_id"],
            document_type=doc_type,
            document_title=event_title_cn(doc_type, ordinal, raw_title),
            raw_title=raw_title,
            official_date=normalize_date(date),
            direction="official",
            confidence=confidence,
            oa_ordinal=ordinal,
            document_code=f"{code}-CN" if code else "",
            document_version=version or "ORIGINAL",
            source_trace=["uspto_global_dossier"],
        ))
    if not parser.rows and not any(m in text for m in _NO_DOC_MARKERS):
        # identity confirmed but no table AND no explicit "no documents"
        # statement: structure changed, never assume an empty case
        return SyncOutcome(code=ResultCode.PAGE_STRUCTURE_CHANGED,
                           message="USPTO Global Dossier 文档表格缺失")
    return SyncOutcome(code=ResultCode.OK, documents=documents,
                       resolved_application_number=app_no)


class UsptoGlobalDossierProvider(DossierProvider):
    provider_id = "uspto_global_dossier"
    jurisdiction = "CN"

    def __init__(self, browser_manager):
        self._manager = browser_manager

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
        digits = ident.number
        url = APP_URL.format(application=digits)
        page = self._manager.open_page(url, cancel)
        if page is None:
            return SyncOutcome(code=ResultCode.NETWORK_ERROR,
                               message="USPTO Global Dossier 无法访问")
        # the SPA renders asynchronously: wait for the table, then a grace gap
        try:
            page.wait_for_selector("table, .no-data, .doc-list", timeout=8000)
        except Exception:
            pass
        page.wait_for_timeout(1500)
        return parse_uspto_document_list(page.content(), ident.normalized_number,
                                         publication_number)

    def download_document(self, document, dest_path, cancel):
        return ResultCode.UNSUPPORTED_JURISDICTION
