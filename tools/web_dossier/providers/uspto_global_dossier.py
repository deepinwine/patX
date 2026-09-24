"""USPTO Global Dossier metadata provider for CN prosecution events.

The parser is deliberately independent of Playwright so saved, sanitised HTML
can cover structure drift and interception handling without network access.
Only official ORIGINAL metadata is retained; document bodies are never
requested or downloaded.
"""
from __future__ import annotations

import re
import time
import urllib.parse
from html import escape
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
SPA_WAIT_TIMEOUT_SECONDS = 30.0
SPA_POLL_INTERVAL_MS = 250

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

_VISIBLE_DOCUMENT_SNAPSHOT_SCRIPT = r"""
(targetToken) => {
  const normalize = (value) => (value || "").toUpperCase().replace(/[^A-Z0-9]/g, "");
  const isVisible = (element) => {
    if (!(element instanceof Element)) return false;
    for (let node = element; node instanceof Element; node = node.parentElement) {
      const tag = node.tagName.toLowerCase();
      if (["script", "style", "template"].includes(tag)) return false;
      if (node.hidden || node.getAttribute("aria-hidden") === "true") return false;
      const style = window.getComputedStyle(node);
      if (style.display === "none" || style.visibility === "hidden") return false;
    }
    return true;
  };
  const text = (element) => isVisible(element) ? (element.innerText || "").trim() : "";
  const body = document.body;
  const visibleText = body ? (body.innerText || "").trim() : "";
  const lowered = visibleText.toLowerCase();
  const blocked = [
    "access denied", "request rejected", "forbidden",
    "sign in to global dossier", "login required", "security verification",
    "verify you are human", "captcha"
  ].some((marker) => lowered.includes(marker));
  const targetMatched = Boolean(targetToken) && normalize(visibleText).includes(targetToken);
  const semanticTable = Array.from(document.querySelectorAll("table"))
    .filter(isVisible)
    .find((table) => {
      const headers = Array.from(table.querySelectorAll("th")).map((cell) => text(cell).toLowerCase());
      const has = (patterns) => headers.some((header) => patterns.some((pattern) => pattern.test(header)));
      return has([/^(document )?date$/, /publication date/]) &&
        has([/description/, /document title/, /^title$/]) &&
        has([/document code/, /^code$/]) &&
        has([/document id/, /remote id/, /^id$/]) &&
        has([/direction/, /category/, /party/]);
    });
  const emptyState = Array.from(document.querySelectorAll(
    '[role="status"], [role="alert"], [class*="empty" i], [id*="empty" i], ' +
    '[class*="no-document" i], [id*="no-document" i], ' +
    '[class*="no-record" i], [id*="no-record" i]'
  )).filter(isVisible).find((element) => /\bno (documents|records)\b/i.test(text(element)));
  let state = "PENDING";
  if (blocked) state = "BLOCKED";
  else if (targetMatched && semanticTable) state = "READY";
  else if (targetMatched && emptyState) state = "EMPTY";
  return {
    state,
    targetMatched,
    tableHtml: semanticTable ? semanticTable.outerHTML : "",
    visibleText,
    emptyText: emptyState ? text(emptyState) : ""
  };
}
"""

class _DocumentListParser(HTMLParser):
    """Collect semantic tables plus visible empty-state containers."""

    _VOID_TAGS = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr",
    }

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.page_text: list[str] = []
        self.tables: list[dict] = []
        self.empty_state_texts: list[dict] = []
        self._table: Optional[dict] = None
        self._row: Optional[list] = None
        self._cell: Optional[dict] = None
        self._visibility_stack: list[tuple[str, bool]] = []
        self._empty_capture: Optional[tuple[int, str]] = None
        self._empty_text: list[str] = []

    def handle_starttag(self, tag, attrs):
        attributes = dict(attrs)
        parent_visible = (
            self._visibility_stack[-1][1] if self._visibility_stack else True
        )
        style = re.sub(r"\s+", "", attributes.get("style", "").lower())
        hidden = (
            tag in ("script", "style", "template")
            or "hidden" in attributes
            or attributes.get("aria-hidden", "").strip().lower() == "true"
            or "d-none" in attributes.get("class", "").lower().split()
            or "display:none" in style
            or "visibility:hidden" in style
        )
        visible = parent_visible and not hidden
        self._visibility_stack.append((tag, visible))
        if not visible:
            if tag in self._VOID_TAGS:
                self._visibility_stack.pop()
            return

        identity = " ".join(
            (attributes.get("id", ""), attributes.get("class", ""))
        ).lower()
        if self._empty_capture is None and re.search(
            r"empty|no[-_ ]?(?:documents|records)", identity
        ):
            self._empty_capture = (len(self._visibility_stack), tag)
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
        if tag in self._VOID_TAGS:
            self._visibility_stack.pop()

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)
        self.handle_endtag(tag)

    def handle_data(self, data):
        if self._visibility_stack and not self._visibility_stack[-1][1]:
            return
        self.page_text.append(data)
        if self._cell is not None:
            self._cell["text"] += data
        if self._empty_capture is not None:
            self._empty_text.append(data)

    def handle_endtag(self, tag):
        visible = (
            self._visibility_stack[-1][1] if self._visibility_stack else True
        )
        if visible and tag in ("th", "td") and self._cell is not None:
            self._cell["text"] = re.sub(r"\s+", " ", self._cell["text"]).strip()
            self._row.append(self._cell)
            self._cell = None
        elif visible and tag == "tr" and self._row is not None:
            if self._table is not None and self._row:
                self._table["rows"].append(self._row)
            self._row = None
        elif visible and tag == "table" and self._table is not None:
            self.tables.append(self._table)
            self._table = None

        if self._empty_capture is not None and self._empty_capture[:2] == (
            len(self._visibility_stack), tag
        ):
            text = re.sub(r"\s+", " ", "".join(self._empty_text)).strip()
            if text:
                self.empty_state_texts.append({"text": text})
            self._empty_capture = None
            self._empty_text = []

        for index in range(len(self._visibility_stack) - 1, -1, -1):
            if self._visibility_stack[index][0] == tag:
                del self._visibility_stack[index:]
                break


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


def _identifier_token(raw: str) -> str:
    return re.sub(r"[^A-Z0-9]", "", (raw or "").upper())


def _visible_cn_number_tokens(text: str) -> set[str]:
    tokens = set()
    for raw_number in re.findall(
        r"\bCN\s*\d{8,14}(?:\.\d)?[A-Z]?\b",
        text or "",
        re.IGNORECASE,
    ):
        identifier = normalize_cn_identifier(raw_number)
        if identifier.number_type in ("application", "publication"):
            tokens.add(_identifier_token(identifier.normalized_number))
    return tokens


def _is_official_direction(raw: str) -> bool:
    normalized = re.sub(r"[^a-z]+", " ", (raw or "").lower()).strip()
    return normalized in _OFFICIAL_DIRECTIONS


def inspect_uspto_document_state(html: str, target_number: str) -> str:
    """Return READY/BLOCKED/PENDING for saved, already rendered HTML."""
    if not html or not html.strip():
        return "PENDING"
    parser = _DocumentListParser()
    try:
        parser.feed(html)
        parser.close()
    except Exception:
        return "PENDING"
    visible_text = re.sub(r"\s+", " ", " ".join(parser.page_text)).strip()
    if any(marker in visible_text.lower() for marker in _BLOCK_MARKERS):
        return "BLOCKED"
    target_token = _identifier_token(target_number)
    if target_token and target_token not in _identifier_token(visible_text):
        return "PENDING"
    for table in parser.tables:
        if any(_header_indexes(row) is not None for row in table["rows"]):
            return "READY"
    for empty_state in parser.empty_state_texts:
        if re.search(
            r"\bno (?:documents|records)\b",
            empty_state["text"],
            re.IGNORECASE,
        ):
            return "READY"
    return "PENDING"


def parse_uspto_document_list(
    html: str, application_number: str = "", publication_number: str = ""
) -> SyncOutcome:
    """Parse a Global Dossier document list without touching the network."""
    if not html or not html.strip():
        return SyncOutcome(
            code=ResultCode.TEMPORARY_ERROR,
            message="USPTO Global Dossier 返回空响应",
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
    lowered = page_text.lower()
    if any(marker in lowered for marker in _BLOCK_MARKERS):
        return SyncOutcome(
            code=ResultCode.ACCESS_DENIED,
            message="USPTO Global Dossier 返回拦截或登录页面",
        )
    if "global dossier" not in page_text.lower():
        return SyncOutcome(
            code=ResultCode.PAGE_STRUCTURE_CHANGED,
            message="未识别 USPTO Global Dossier 页面标志",
        )

    normalized_application = _normal_number(application_number, "application")
    normalized_publication = _normal_number(publication_number, "publication")
    target_number = normalized_application or normalized_publication
    target_token = _identifier_token(target_number)
    visible_case_tokens = _visible_cn_number_tokens(page_text)
    if target_number and visible_case_tokens and target_token not in visible_case_tokens:
        return SyncOutcome(
            code=ResultCode.PAGE_STRUCTURE_CHANGED,
            message="Global Dossier 页面案件号与查询目标不一致",
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
        re.search(
            r"\bno (?:documents|records)\b",
            empty_state["text"],
            re.IGNORECASE,
        )
        for empty_state in parser.empty_state_texts
    )
    if not target_rows:
        if explicit_empty:
            return SyncOutcome(code=ResultCode.OK, documents=[])
        return SyncOutcome(
            code=ResultCode.PAGE_STRUCTURE_CHANGED,
            message="文档表格为空且未显示明确的无文档状态",
        )

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


def _snapshot_html(snapshot: dict, target_number: str) -> str:
    """Convert a browser-filtered visible snapshot into parser input."""
    target = escape(target_number, quote=True)
    state = snapshot.get("state")
    if state == "READY":
        table_html = snapshot.get("tableHtml")
        if not isinstance(table_html, str) or not table_html.strip():
            return ""
        return (
            "<html><body><h1>Global Dossier</h1>"
            f"<p class=\"case-number\">{target}</p>{table_html}</body></html>"
        )
    if state == "EMPTY":
        return (
            "<html><body><h1>Global Dossier</h1>"
            f"<p class=\"case-number\">{target}</p>"
            "<table><thead><tr><th>Date</th><th>Document Description</th>"
            "<th>Document Code</th><th>Document ID</th><th>Direction</th>"
            "</tr></thead><tbody></tbody></table>"
            "<div class=\"empty-state\">No documents found</div>"
            "</body></html>"
        )
    return ""


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

    def _target_token(
        self, application_number: str, publication_number: str
    ) -> str:
        normalized_application = _normal_number(application_number, "application")
        normalized_publication = _normal_number(publication_number, "publication")
        return _identifier_token(normalized_application or normalized_publication)

    @staticmethod
    def _is_dossier_response(response, target_token: str) -> bool:
        try:
            url = response.url or ""
            request = response.request
            post_data = getattr(request, "post_data", "") or ""
        except Exception:
            return False
        path = urllib.parse.urlparse(url).path.lower()
        is_dossier_endpoint = bool(
            re.search(
                r"(?:^|/)(?:dossier|documents?|files?)(?:$|[/_.-])",
                path,
            )
        )
        request_identity = _identifier_token(url + " " + str(post_data))
        return is_dossier_endpoint and bool(
            target_token and target_token in request_identity
        )

    def _http_error_outcome(self) -> Optional[SyncOutcome]:
        statuses = []
        navigation_status = getattr(self._manager, "last_navigation_status", None)
        if isinstance(navigation_status, int):
            statuses.append(navigation_status)
        statuses.extend(
            status
            for status in getattr(self._manager, "last_spa_statuses", [])
            if isinstance(status, int)
        )
        if any(status in (401, 403) for status in statuses):
            return SyncOutcome(
                code=ResultCode.ACCESS_DENIED,
                message="USPTO Global Dossier 请求被拒绝",
            )
        if 429 in statuses:
            return SyncOutcome(
                code=ResultCode.RATE_LIMITED,
                message="USPTO Global Dossier 请求受限（HTTP 429）",
            )
        if any(status >= 500 for status in statuses):
            return SyncOutcome(
                code=ResultCode.NETWORK_ERROR,
                message="USPTO Global Dossier 服务暂时不可用",
            )
        if 404 in statuses:
            return SyncOutcome(
                code=ResultCode.CASE_NOT_FOUND,
                message="USPTO Global Dossier 未找到案件数据",
            )
        return None

    def _wait_for_document_state(
        self, page, target_token: str, cancel
    ) -> tuple[Optional[SyncOutcome], Optional[dict]]:
        deadline = time.monotonic() + SPA_WAIT_TIMEOUT_SECONDS
        while True:
            if cancel is not None and cancel.is_set():
                return SyncOutcome(
                    code=ResultCode.TEMPORARY_ERROR,
                    message="USPTO Global Dossier 查询已取消",
                ), None
            status_outcome = self._http_error_outcome()
            if status_outcome is not None:
                return status_outcome, None
            try:
                snapshot = page.evaluate(
                    _VISIBLE_DOCUMENT_SNAPSHOT_SCRIPT,
                    target_token,
                )
            except Exception:
                return SyncOutcome(
                    code=ResultCode.NETWORK_ERROR,
                    message="读取 Global Dossier 文档状态失败",
                ), None
            if not isinstance(snapshot, dict):
                snapshot = {}
            state = snapshot.get("state")
            if state == "BLOCKED":
                return SyncOutcome(
                    code=ResultCode.ACCESS_DENIED,
                    message="USPTO Global Dossier 返回可见拦截或登录页面",
                ), None
            if state in ("READY", "EMPTY") and snapshot.get(
                "targetMatched"
            ) is True:
                return self._http_error_outcome(), snapshot
            if time.monotonic() >= deadline:
                return SyncOutcome(
                    code=ResultCode.PAGE_STRUCTURE_CHANGED,
                    message="等待 Global Dossier 文档状态超时或页面结构已变化",
                ), None
            try:
                page.wait_for_timeout(SPA_POLL_INTERVAL_MS)
            except Exception:
                return SyncOutcome(
                    code=ResultCode.PAGE_STRUCTURE_CHANGED,
                    message="等待 Global Dossier 文档状态失败",
                ), None

    def list_documents(
        self, application_number: str, publication_number: str, cancel
    ) -> SyncOutcome:
        url = self._case_url(application_number, publication_number)
        if not url:
            return SyncOutcome(
                code=ResultCode.RESOLVE_FAILED,
                message="缺少可识别的 CN 申请号或公开号",
            )
        target_token = self._target_token(application_number, publication_number)
        try:
            page = self._manager.open_page(
                url,
                cancel,
                fresh=True,
                response_filter=lambda response: self._is_dossier_response(
                    response, target_token
                ),
            )
        except Exception:
            page = None
        if page is None:
            return SyncOutcome(
                code=ResultCode.NETWORK_ERROR,
                message="无法打开 USPTO Global Dossier 页面",
            )

        wait_outcome, snapshot = self._wait_for_document_state(
            page,
            target_token,
            cancel,
        )
        if wait_outcome is not None:
            return wait_outcome
        normalized_application = _normal_number(application_number, "application")
        normalized_publication = _normal_number(publication_number, "publication")
        target_number = normalized_application or normalized_publication
        html = _snapshot_html(snapshot or {}, target_number)
        if not html:
            return SyncOutcome(
                code=ResultCode.PAGE_STRUCTURE_CHANGED,
                message="Global Dossier 可见文档快照不完整",
            )
        return parse_uspto_document_list(html, application_number, publication_number)

    def download_document(
        self, document: ProsecutionDocument, dest_path: str, cancel
    ) -> ResultCode:
        return ResultCode.UNSUPPORTED_JURISDICTION
