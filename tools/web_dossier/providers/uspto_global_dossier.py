"""USPTO Global Dossier metadata provider for CN prosecution events.

The parser is deliberately independent of Playwright so saved, sanitised HTML
can cover structure drift and interception handling without network access.
Only official ORIGINAL metadata is retained; document bodies are never
requested or downloaded.
"""
from __future__ import annotations

import re
import urllib.parse
from html.parser import HTMLParser
from typing import Optional

from ..document_classifier import classify_official_document, event_title_cn
from ..models import (
    Confidence,
    DocumentType,
    ProsecutionDocument,
    ResultCode,
    normalize_date,
)
from ..number_resolver import normalize_cn_identifier
from .base import DossierProvider, SyncOutcome


APP_URL = "https://globaldossier.uspto.gov/#/result/application/CN/{application}/0"
PUB_URL = "https://globaldossier.uspto.gov/#/result/publication/CN/{publication}/1"

_BLOCK_MARKERS = (
    "access denied",
    "request rejected",
    "forbidden",
    "sign in to global dossier",
    "login required",
    "security verification",
    "verify you are human",
    "captcha",
)

_REMINDABLE_TYPES = {
    DocumentType.OFFICE_ACTION_FIRST,
    DocumentType.OFFICE_ACTION_SECOND,
    DocumentType.OFFICE_ACTION_NTH,
    DocumentType.REJECTION_DECISION,
    DocumentType.GRANT_NOTICE,
    DocumentType.CORRECTION_NOTICE,
}

_OFFICIAL_DIRECTIONS = {
    "official",
    "office",
    "patent office",
    "office to applicant",
    "patent office to applicant",
}

_DOCUMENT_STATE_SCRIPT = r"""
() => {
  const bodyText = (document.body?.innerText || '').toLowerCase();
  const hasDocumentTable = Array.from(document.querySelectorAll('table')).some(
    (table) => {
      const firstRow = table.querySelector('thead') || table.querySelector('tr');
      const header = (firstRow?.innerText || '').toLowerCase();
      return /\bdate\b/.test(header) &&
             /(description|document title)/.test(header) &&
             /(document code|\bcode\b)/.test(header);
    }
  );
  const hasEmptyState = /\b(?:no documents|no records)\b/.test(bodyText);
  const hasBlockedState = /(access denied|request rejected|forbidden|sign in to global dossier|login required|security verification|verify you are human|captcha)/.test(bodyText);
  return hasDocumentTable || hasEmptyState || hasBlockedState;
}
"""


class _DocumentListParser(HTMLParser):
    """Collect semantic tables plus visible empty-state containers."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.page_text: list[str] = []
        self.tables: list[dict] = []
        self.empty_state_texts: list[str] = []
        self._table: Optional[dict] = None
        self._row: Optional[list] = None
        self._cell: Optional[dict] = None
        self._empty_depth = 0
        self._empty_text: list[str] = []

    def handle_starttag(self, tag, attrs):
        attributes = dict(attrs)
        if self._empty_depth:
            self._empty_depth += 1
        else:
            identity = " ".join(
                (attributes.get("id", ""), attributes.get("class", ""))
            ).lower()
            hidden = "hidden" in attributes or (
                attributes.get("aria-hidden", "").lower() == "true"
            )
            if not hidden and re.search(r"empty|no[-_ ]?(?:documents|records)", identity):
                self._empty_depth = 1
                self._empty_text = []

        if tag == "table" and self._table is None:
            self._table = {"rows": []}
        elif tag == "tr" and self._table is not None:
            self._row = []
        elif tag in ("th", "td") and self._row is not None:
            self._cell = {
                "text": "",
                "href": "",
                "header": tag == "th",
            }
        elif tag == "a" and self._cell is not None:
            self._cell["href"] = attributes.get("href", "")

    def handle_data(self, data):
        self.page_text.append(data)
        if self._cell is not None:
            self._cell["text"] += data
        if self._empty_depth:
            self._empty_text.append(data)

    def handle_endtag(self, tag):
        if tag in ("th", "td") and self._cell is not None:
            self._cell["text"] = re.sub(r"\s+", " ", self._cell["text"]).strip()
            self._row.append(self._cell)
            self._cell = None
        elif tag == "tr" and self._row is not None:
            if self._table is not None and self._row:
                self._table["rows"].append(self._row)
            self._row = None
        elif tag == "table" and self._table is not None:
            self.tables.append(self._table)
            self._table = None

        if self._empty_depth:
            self._empty_depth -= 1
            if self._empty_depth == 0:
                text = re.sub(r"\s+", " ", "".join(self._empty_text)).strip()
                if text:
                    self.empty_state_texts.append(text)
                self._empty_text = []


def _header_indexes(row: list[dict]) -> Optional[dict[str, int]]:
    if not row or not all(cell["header"] for cell in row):
        return None
    headers = [re.sub(r"\s+", " ", cell["text"]).strip().lower() for cell in row]

    def find(*patterns: str) -> int:
        for index, header in enumerate(headers):
            if any(re.search(pattern, header) for pattern in patterns):
                return index
        return -1

    indexes = {
        "date": find(r"^(?:document )?date$", r"publication date"),
        "title": find(r"description", r"document title", r"^title$"),
        "code": find(r"document code", r"^code$"),
        "remote_id": find(r"document id", r"remote id", r"^id$"),
        "direction": find(r"direction", r"category", r"party"),
    }
    return indexes if all(index >= 0 for index in indexes.values()) else None


def _cell(row: list[dict], index: int) -> dict:
    return row[index] if 0 <= index < len(row) else {"text": "", "href": ""}


def _normal_number(raw: str, number_type: str) -> str:
    identifier = normalize_cn_identifier(raw)
    if identifier.number_type == number_type:
        return identifier.normalized_number
    return ""


def _is_official_direction(raw: str) -> bool:
    normalized = re.sub(r"[^a-z]+", " ", (raw or "").lower()).strip()
    return normalized in _OFFICIAL_DIRECTIONS


def parse_uspto_document_list(
    html: str, application_number: str = "", publication_number: str = ""
) -> SyncOutcome:
    """Parse a Global Dossier document list without touching the network."""
    if not html or not html.strip():
        return SyncOutcome(
            code=ResultCode.TEMPORARY_ERROR,
            message="USPTO Global Dossier 返回空响应",
        )

    lowered = re.sub(r"\s+", " ", html).lower()
    if any(marker in lowered for marker in _BLOCK_MARKERS):
        return SyncOutcome(
            code=ResultCode.ACCESS_DENIED,
            message="USPTO Global Dossier 返回拦截或登录页面",
        )

    parser = _DocumentListParser()
    try:
        parser.feed(html)
        parser.close()
    except Exception:
        return SyncOutcome(
            code=ResultCode.PAGE_STRUCTURE_CHANGED,
            message="USPTO Global Dossier 页面解析异常",
        )

    page_text = re.sub(r"\s+", " ", " ".join(parser.page_text)).strip()
    if "global dossier" not in page_text.lower():
        return SyncOutcome(
            code=ResultCode.PAGE_STRUCTURE_CHANGED,
            message="未识别 USPTO Global Dossier 页面标志",
        )

    target_rows = None
    indexes = None
    for table in parser.tables:
        for row_index, row in enumerate(table["rows"]):
            candidate_indexes = _header_indexes(row)
            if candidate_indexes is not None:
                target_rows = table["rows"][row_index + 1 :]
                indexes = candidate_indexes
                break
        if indexes is not None:
            break
    if target_rows is None or indexes is None:
        return SyncOutcome(
            code=ResultCode.PAGE_STRUCTURE_CHANGED,
            message="未找到具有语义表头的 Global Dossier 文档表格",
        )

    explicit_empty = any(
        re.search(r"\bno (?:documents|records)\b", text, re.IGNORECASE)
        for text in parser.empty_state_texts
    )
    if not target_rows:
        if explicit_empty:
            return SyncOutcome(code=ResultCode.OK, documents=[])
        return SyncOutcome(
            code=ResultCode.PAGE_STRUCTURE_CHANGED,
            message="文档表格为空且未显示明确的无文档状态",
        )

    normalized_application = _normal_number(application_number, "application")
    normalized_publication = _normal_number(publication_number, "publication")
    documents: list[ProsecutionDocument] = []
    official_candidates = 0
    date_failures = 0

    for row in target_rows:
        title_cell = _cell(row, indexes["title"])
        raw_title = title_cell["text"]
        direction_text = _cell(row, indexes["direction"])["text"]
        if not re.search(r"\(\s*original\s*\)\s*$", raw_title, re.IGNORECASE):
            continue
        if not _is_official_direction(direction_text):
            continue

        document_code = _cell(row, indexes["code"])["text"].strip()
        document_type, ordinal, confidence = classify_official_document(
            raw_title, document_code
        )
        if confidence != Confidence.HIGH or document_type not in _REMINDABLE_TYPES:
            continue

        official_candidates += 1
        official_date = normalize_date(_cell(row, indexes["date"])["text"])
        if not official_date:
            date_failures += 1
            continue

        remote_cell = _cell(row, indexes["remote_id"])
        remote_document_id = remote_cell["text"].strip() or remote_cell["href"].strip()
        source_url = remote_cell["href"].strip() or title_cell["href"].strip()
        canonical_title = event_title_cn(document_type, ordinal, raw_title)
        documents.append(
            ProsecutionDocument(
                jurisdiction="CN",
                application_number=normalized_application,
                publication_number=normalized_publication,
                source="uspto_global_dossier",
                remote_document_id=remote_document_id,
                document_type=document_type,
                document_title=canonical_title,
                raw_title=raw_title,
                official_date=official_date,
                direction="official",
                source_url=source_url,
                download_available=False,
                confidence=confidence,
                oa_ordinal=ordinal,
                document_code=document_code,
                document_version="ORIGINAL",
                source_trace=["uspto_global_dossier"],
            )
        )

    if official_candidates and date_failures == official_candidates:
        return SyncOutcome(
            code=ResultCode.DATE_PARSE_FAILED,
            message="所有候选官方文件的日期均无法解析",
        )
    return SyncOutcome(
        code=ResultCode.OK,
        documents=documents,
        resolved_application_number=normalized_application,
    )


class UsptoGlobalDossierProvider(DossierProvider):
    provider_id = "uspto_global_dossier"
    jurisdiction = "CN"

    def __init__(self, browser_manager=None):
        if browser_manager is None:
            from ..browser.manager import BrowserManager

            browser_manager = BrowserManager(
                self.provider_id,
                prefer_system_browser=False,
                headless=True,
            )
        self._manager = browser_manager

    def health_check(self) -> bool:
        return self._manager is not None

    def check_auth(self, cancel) -> ResultCode:
        return ResultCode.OK

    def ensure_login(self, cancel) -> ResultCode:
        return ResultCode.OK

    def resolve_case(
        self, application_number: str, publication_number: str, cancel
    ) -> SyncOutcome:
        normalized_application = _normal_number(application_number, "application")
        if normalized_application:
            return SyncOutcome(
                code=ResultCode.OK,
                resolved_application_number=normalized_application,
            )
        normalized_publication = _normal_number(publication_number, "publication")
        if normalized_publication:
            return SyncOutcome(code=ResultCode.OK)
        return SyncOutcome(
            code=ResultCode.RESOLVE_FAILED,
            message="缺少可识别的 CN 申请号或公开号",
        )

    def _case_url(self, application_number: str, publication_number: str) -> str:
        normalized_application = _normal_number(application_number, "application")
        if normalized_application:
            route_number = normalized_application.removeprefix("CN")
            return APP_URL.format(
                application=urllib.parse.quote(route_number, safe=".")
            )
        normalized_publication = _normal_number(publication_number, "publication")
        if normalized_publication:
            route_number = normalized_publication.removeprefix("CN")
            return PUB_URL.format(publication=urllib.parse.quote(route_number, safe=""))
        return ""

    def list_documents(
        self, application_number: str, publication_number: str, cancel
    ) -> SyncOutcome:
        url = self._case_url(application_number, publication_number)
        if not url:
            return SyncOutcome(
                code=ResultCode.RESOLVE_FAILED,
                message="缺少可识别的 CN 申请号或公开号",
            )
        try:
            page = self._manager.open_page(url, cancel)
        except Exception:
            page = None
        if page is None:
            return SyncOutcome(
                code=ResultCode.NETWORK_ERROR,
                message="无法打开 USPTO Global Dossier 页面",
            )

        navigation_status = getattr(self._manager, "last_navigation_status", None)
        if navigation_status in (401, 403):
            return SyncOutcome(
                code=ResultCode.ACCESS_DENIED,
                message=f"USPTO Global Dossier 导航被拒绝（HTTP {navigation_status}）",
            )
        if navigation_status == 429:
            return SyncOutcome(
                code=ResultCode.RATE_LIMITED,
                message="USPTO Global Dossier 请求受限（HTTP 429）",
            )

        try:
            page.wait_for_load_state("domcontentloaded", timeout=30_000)
            page.wait_for_function(_DOCUMENT_STATE_SCRIPT, timeout=30_000)
        except Exception:
            return SyncOutcome(
                code=ResultCode.PAGE_STRUCTURE_CHANGED,
                message="等待 Global Dossier 文档状态超时或页面结构已变化",
            )
        try:
            html = page.content()
        except Exception:
            return SyncOutcome(
                code=ResultCode.NETWORK_ERROR,
                message="无法读取 USPTO Global Dossier 页面",
            )
        return parse_uspto_document_list(html, application_number, publication_number)

    def download_document(
        self, document: ProsecutionDocument, dest_path: str, cancel
    ) -> ResultCode:
        return ResultCode.UNSUPPORTED_JURISDICTION
