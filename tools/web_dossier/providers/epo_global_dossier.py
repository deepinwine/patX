"""Public EPO Global Dossier metadata provider for CN applications.

The pure parser accepts saved HTML and never downloads document bodies.  The
provider uses the public European Patent Register entry and requires a real CN
application number; a publication number is never guessed to be one.
"""
from __future__ import annotations

import re
import urllib.error
import urllib.parse
import urllib.request
from html.parser import HTMLParser
from typing import Optional

from ..document_classifier import (
    classify_official_document,
    direction_for,
    event_title_cn,
)
from ..models import Confidence, DocumentType, ProsecutionDocument, ResultCode, normalize_date
from ..number_resolver import cn_check_digit, normalize_cn_identifier
from .base import DossierProvider, SyncOutcome


EPO_DOSSIER_URL = (
    "https://register.epo.org/ipfwretrieve?apn=CN.{application}.A&lng=en"
)
MAX_HTML_BYTES = 5 * 1024 * 1024

_BLOCK_MARKERS = (
    "access denied",
    "request rejected",
    "forbidden",
    "login required",
    "sign in to global dossier",
    "authentication required",
    "security verification",
    "verify you are human",
    "captcha",
    "just a moment",
    "enable javascript and cookies",
    "checking your browser",
    "cloudflare ray id",
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


def _hidden(attributes: dict[str, str]) -> bool:
    style = re.sub(r"\s+", "", attributes.get("style", "").lower())
    classes = set(attributes.get("class", "").lower().split())
    return (
        "hidden" in attributes
        or bool(
            classes.intersection({"d-none", "hidden", "invisible", "is-hidden"})
        )
        or attributes.get("aria-hidden", "").lower() == "true"
        or "display:none" in style
        or "visibility:hidden" in style
    )


class _EpoDocumentParser(HTMLParser):
    """Collect visible page text, semantic table rows, and empty states."""

    _NON_RENDERED = {"script", "style", "template", "noscript"}
    _VOID_TAGS = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr",
    }

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.page_text: list[str] = []
        self.tables: list[list[list[dict]]] = []
        self.empty_states: list[dict] = []
        self._visibility: list[bool] = []
        self._table: Optional[list[list[dict]]] = None
        self._row: Optional[list[dict]] = None
        self._row_id = ""
        self._cell: Optional[dict] = None
        self._empty_depth: Optional[int] = None
        self._empty_parts: list[str] = []
        self._empty_table: Optional[list[list[dict]]] = None

    @property
    def _visible(self) -> bool:
        return all(self._visibility)

    def handle_starttag(self, tag, attrs):
        attributes = {key: value or "" for key, value in attrs}
        parent_visible = self._visible
        visible = parent_visible and tag not in self._NON_RENDERED and not _hidden(attributes)
        self._visibility.append(visible)
        if not visible:
            if tag in self._VOID_TAGS:
                self._visibility.pop()
            return
        if tag == "table" and self._table is None:
            self._table = []
        elif tag == "tr" and self._table is not None:
            self._row = []
            self._row_id = attributes.get("id", "")
        elif tag in ("th", "td") and self._row is not None:
            self._cell = {
                "text": "",
                "href": "",
                "header": tag == "th",
                "row_id": self._row_id,
            }
        elif tag == "a" and self._cell is not None:
            self._cell["href"] = attributes.get("href", "")

        marker = " ".join((attributes.get("class", ""), attributes.get("id", "")))
        if self._empty_depth is None and re.search(
            r"(?:empty|no[-_ ]?(?:document|record))", marker, re.IGNORECASE
        ):
            self._empty_depth = len(self._visibility)
            self._empty_parts = []
            self._empty_table = self._table
        if tag in self._VOID_TAGS:
            self._visibility.pop()

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)
        if tag not in self._VOID_TAGS:
            self.handle_endtag(tag)

    def handle_data(self, data):
        if not self._visible:
            return
        text = re.sub(r"\s+", " ", data).strip()
        if not text:
            return
        self.page_text.append(text)
        if self._cell is not None:
            self._cell["text"] += (" " if self._cell["text"] else "") + text
        if self._empty_depth is not None:
            self._empty_parts.append(text)

    def handle_endtag(self, tag):
        if self._visible:
            if tag in ("th", "td") and self._cell is not None:
                self._cell["text"] = re.sub(
                    r"\s+", " ", self._cell["text"]
                ).strip()
                if self._row is not None:
                    self._row.append(self._cell)
                self._cell = None
            elif tag == "tr" and self._row is not None:
                if self._table is not None and self._row:
                    self._table.append(self._row)
                self._row = None
                self._row_id = ""
            elif tag == "table" and self._table is not None:
                self.tables.append(self._table)
                self._table = None

        if self._empty_depth == len(self._visibility):
            text = re.sub(r"\s+", " ", " ".join(self._empty_parts)).strip()
            if text:
                self.empty_states.append({
                    "text": text,
                    "table": self._empty_table,
                })
            self._empty_depth = None
            self._empty_parts = []
            self._empty_table = None
        if self._visibility:
            self._visibility.pop()


def _header_indexes(row: list[dict]) -> Optional[dict[str, int]]:
    if not row or not all(cell.get("header") for cell in row):
        return None
    indexes: dict[str, int] = {}
    patterns = {
        "date": (r"^(?:document |dispatch |mailing )?date$",),
        "title": (r"^(?:document )?(?:description|title)$",),
        "code": (r"^(?:document )?code$",),
        "id": (r"^(?:document |remote )?id$", r"^identifier$"),
        "direction": (r"^(?:direction|category|party)$",),
        "version": (r"^(?:document )?(?:version|language version)$",),
        "pages": (r"^pages?$",),
    }
    for index, cell in enumerate(row):
        header = re.sub(r"\s+", " ", cell.get("text", "")).strip().lower()
        for name, candidates in patterns.items():
            if name not in indexes and any(re.fullmatch(pattern, header) for pattern in candidates):
                indexes[name] = index
    required = {"date", "title"}
    has_version_semantics = "version" in indexes or "pages" in indexes
    return indexes if required.issubset(indexes) and has_version_semantics else None


def _cell(row: list[dict], index: int) -> dict:
    return row[index] if 0 <= index < len(row) else {"text": "", "href": ""}


def _displayed_identifiers(page_text: str) -> tuple[set[str], set[str]]:
    upper = page_text.upper()
    applications = {
        f"CN{match.group(1)}.{match.group(2)}"
        for match in re.finditer(r"\bCN\s*(\d{12})\s*\.\s*([0-9X])\b", upper)
    }
    applications.update(
        f"CN{match.group(1)}"
        for match in re.finditer(r"\bCN\s*(\d{12})(?!\s*\.)\b", upper)
    )
    publications = {
        f"CN{match.group(1)}{match.group(2)}"
        for match in re.finditer(r"\bCN\s*(\d{9})\s*([ABU](?:1)?)\b", upper)
    }
    return applications, publications


def _normalized(raw: str, expected: str) -> str:
    if expected == "application":
        return _normalize_epo_application(raw)
    identifier = normalize_cn_identifier(raw)
    return identifier.normalized_number if identifier.number_type == expected else ""


def _normalize_epo_application(raw: str) -> str:
    """Normalize CN application inputs, including a validated X check digit."""
    if not raw:
        return ""
    normalized = raw.translate(
        str.maketrans("０１２３４５６７８９．Ｘｘ", "0123456789.XX")
    )
    normalized = re.sub(r"\s+", "", normalized).upper().removeprefix("CN")
    match = re.fullmatch(r"(\d{12})(?:\.?([0-9X]))?", normalized)
    if not match:
        return ""
    number, supplied_check = match.groups()
    if supplied_check and supplied_check != cn_check_digit(number):
        return ""
    return f"CN{number}" + (f".{supplied_check}" if supplied_check else "")


def _application_core(raw: str) -> str:
    normalized = _normalize_epo_application(raw)
    match = re.fullmatch(r"CN(\d{12})(?:\.[0-9X])?", normalized)
    return match.group(1) if match else ""


def _same_application(expected: str, displayed: str) -> bool:
    if expected.split(".", 1)[0] != displayed.split(".", 1)[0]:
        return False
    return "." not in expected or "." not in displayed or expected == displayed


def _normalize_epo_date(raw: str) -> str:
    european = re.fullmatch(
        r"\s*(\d{1,2})\.(\d{1,2})\.(\d{4})\s*", raw or ""
    )
    if european:
        return normalize_date(
            f"{european.group(3)}-{european.group(2)}-{european.group(1)}"
        )
    return normalize_date(raw)


def _versions_from_title(raw_title: str) -> set[str]:
    return {
        match.upper()
        for match in re.findall(
            r"\((ORIGINAL|TRANSLATED)\)", raw_title or "", re.IGNORECASE
        )
    }


def _document_id_from_cells(id_cell: dict, title_cell: dict) -> str:
    text = (id_cell.get("text") or "").strip()
    if text and not re.fullmatch(r"(?:view|open|document)", text, re.IGNORECASE):
        return text
    for cell in (id_cell, title_cell):
        href = cell.get("href") or ""
        match = re.search(r"[?&]documentId=([A-Z0-9_-]+)", href, re.IGNORECASE)
        if match:
            return match.group(1)
    return ""


def _document_code_from_id(remote_document_id: str, application_core: str) -> str:
    match = re.match(
        rf"^{re.escape(application_core)}[0-9X](\d{{6}})",
        remote_document_id or "",
        re.IGNORECASE,
    )
    return f"{match.group(1)}-CN" if match else ""


def _application_cores_from_href(href: str) -> tuple[set[str], bool]:
    """Extract CN application identities embedded in EPO links/JavaScript."""
    decoded = urllib.parse.unquote((href or "").replace("&amp;", "&"))
    cores: set[str] = set()
    invalid_check = False
    for match in re.finditer(
        r"(?:[?&]|\b)(?:number|apn|application(?:number)?)\s*=\s*"
        r"(?:CN[._-]?)?(\d{12})(?:[._-]?([0-9X]))?(?:[._-]?A\d?)?"
        r"(?=[^0-9]|$)",
        decoded,
        re.IGNORECASE,
    ):
        core = match.group(1)
        supplied_check = (match.group(2) or "").upper()
        cores.add(core)
        if supplied_check and supplied_check != cn_check_digit(core):
            invalid_check = True
    return cores, invalid_check


def _row_application_cores(
    remote_document_id: str, id_cell: dict, title_cell: dict
) -> tuple[set[str], bool]:
    cores: set[str] = set()
    remote_match = re.match(
        r"^(?:CN[-_.]?)?(\d{12})([0-9X])?",
        remote_document_id or "",
        re.IGNORECASE,
    )
    invalid_check = False
    if remote_match:
        core = remote_match.group(1)
        supplied_check = (remote_match.group(2) or "").upper()
        cores.add(core)
        if supplied_check and supplied_check != cn_check_digit(core):
            invalid_check = True
    for cell in (id_cell, title_cell):
        href_cores, href_invalid_check = _application_cores_from_href(
            cell.get("href") or ""
        )
        cores.update(href_cores)
        invalid_check = invalid_check or href_invalid_check
    return cores, invalid_check


def _same_publication(expected: str, displayed: str) -> bool:
    expected_identifier = normalize_cn_identifier(expected)
    displayed_identifier = normalize_cn_identifier(displayed)
    return (
        expected_identifier.number == displayed_identifier.number
        and (
            not expected_identifier.kind_code
            or expected_identifier.kind_code == displayed_identifier.kind_code
        )
    )


def parse_epo_document_list(
    html: str, application_number: str = "", publication_number: str = ""
) -> SyncOutcome:
    """Parse saved EPO Global Dossier HTML with strict case/table binding."""
    if not html or not html.strip():
        return SyncOutcome(
            code=ResultCode.TEMPORARY_ERROR,
            message="EPO Global Dossier 返回空响应",
        )

    parser = _EpoDocumentParser()
    try:
        parser.feed(html)
        parser.close()
    except Exception:
        return SyncOutcome(
            code=ResultCode.PAGE_STRUCTURE_CHANGED,
            message="EPO Global Dossier 页面解析异常",
        )

    page_text = re.sub(r"\s+", " ", " ".join(parser.page_text)).strip()
    lowered = page_text.lower()
    blocked = any(marker in lowered for marker in _BLOCK_MARKERS) or (
        "sign in" in lowered and not parser.tables
    )
    if blocked:
        return SyncOutcome(
            code=ResultCode.ACCESS_DENIED,
            message="EPO Global Dossier 返回拦截、登录或安全验证页面",
        )
    if not (
        "global dossier" in lowered
        and ("european patent register" in lowered or "epo register" in lowered)
    ):
        return SyncOutcome(
            code=ResultCode.PAGE_STRUCTURE_CHANGED,
            message="未识别 EPO Global Dossier 页面标志",
        )

    displayed_applications, displayed_publications = _displayed_identifiers(page_text)
    expected_application = _normalized(application_number, "application")
    expected_publication = _normalized(publication_number, "publication")
    if len(displayed_applications) != 1 or len(displayed_publications) > 1:
        return SyncOutcome(
            code=ResultCode.PAGE_STRUCTURE_CHANGED,
            message="EPO 页面缺少唯一的案件申请号或混入多个案件",
        )
    displayed_application = next(iter(displayed_applications))
    displayed_publication = next(iter(displayed_publications), "")
    if expected_application and not _same_application(
        expected_application, displayed_application
    ):
        return SyncOutcome(
            code=ResultCode.PAGE_STRUCTURE_CHANGED,
            message="EPO 页面申请号与查询案件不一致",
        )
    if (
        expected_publication
        and displayed_publication
        and not _same_publication(expected_publication, displayed_publication)
    ):
        return SyncOutcome(
            code=ResultCode.PAGE_STRUCTURE_CHANGED,
            message="EPO 页面公开号与查询案件不一致",
        )
    if expected_publication and not displayed_publication and not expected_application:
        return SyncOutcome(
            code=ResultCode.PAGE_STRUCTURE_CHANGED,
            message="EPO 页面未显示可绑定的查询公开号",
        )

    selected_rows = None
    selected_table = None
    indexes = None
    for rows in parser.tables:
        for row_index, row in enumerate(rows):
            candidate_indexes = _header_indexes(row)
            if candidate_indexes is not None:
                if selected_rows is not None:
                    return SyncOutcome(
                        code=ResultCode.PAGE_STRUCTURE_CHANGED,
                        message="EPO 页面存在多个候选文档表，无法安全绑定案件",
                    )
                selected_rows = rows[row_index + 1 :]
                selected_table = rows
                indexes = candidate_indexes
                break
    if selected_rows is None or indexes is None:
        return SyncOutcome(
            code=ResultCode.PAGE_STRUCTURE_CHANGED,
            message="未找到语义完整的 EPO Global Dossier 文档表",
        )

    bound_empty = any(
        state["table"] is selected_table
        and re.search(
            r"\bno (?:documents|records)\b", state["text"], re.IGNORECASE
        )
        for state in parser.empty_states
    )
    placeholder_rows_only = bool(selected_rows) and all(
        re.search(
            r"\bno (?:documents|records)\b",
            " ".join(cell.get("text", "") for cell in row),
            re.IGNORECASE,
        )
        for row in selected_rows
    )
    if bound_empty and (not selected_rows or placeholder_rows_only):
        return SyncOutcome(
            code=ResultCode.OK,
            documents=[],
            resolved_application_number=displayed_application,
        )
    if not selected_rows:
        return SyncOutcome(
            code=ResultCode.PAGE_STRUCTURE_CHANGED,
            message="EPO 文档表为空但页面没有可信的无文档状态",
        )

    documents: list[ProsecutionDocument] = []
    date_failures = 0
    structural_failure = ""
    normalized_application = (
        expected_application
        if "." in expected_application
        else displayed_application
    )
    normalized_publication = displayed_publication or expected_publication
    application_core = _application_core(normalized_application)
    for row in selected_rows:
        semantic_version_index = indexes.get("version", indexes.get("pages", -1))
        required_indexes = (indexes["date"], indexes["title"], semantic_version_index)
        if any(index >= len(row) for index in required_indexes):
            structural_failure = "EPO 文档行缺少必需列"
            break
        title_cell = _cell(row, indexes["title"])
        raw_title = title_cell["text"].strip()
        document_code = _cell(row, indexes.get("code", -1))["text"].strip()
        direction = re.sub(
            r"\s+", " ", _cell(row, indexes.get("direction", -1))["text"]
        ).strip().lower()
        title_versions = _versions_from_title(raw_title)
        column_version = (
            _cell(row, indexes["version"])["text"].strip().upper()
            if "version" in indexes
            else ""
        )
        if len(title_versions) > 1:
            structural_failure = "EPO 文档标题包含冲突的版本标记"
            break
        title_version = next(iter(title_versions), "")
        if title_version and column_version and title_version != column_version:
            structural_failure = "EPO 文档标题版本与版本列冲突"
            break
        version = column_version or title_version
        if not raw_title or not version:
            structural_failure = "EPO 文档行的标题或版本为空"
            break
        if version != "ORIGINAL":
            continue
        id_cell = _cell(row, indexes.get("id", -1))
        remote_document_id = (
            _document_id_from_cells(id_cell, title_cell)
            or row[0].get("row_id", "")
        )
        if not document_code:
            document_code = _document_code_from_id(
                remote_document_id, application_core
            )
        document_type, ordinal, confidence = classify_official_document(
            raw_title, document_code
        )
        inferred_direction = direction_for(document_type)
        if (
            (direction and direction not in _OFFICIAL_DIRECTIONS)
            or inferred_direction != "official"
        ):
            continue
        if document_type not in _REMINDABLE_TYPES or confidence != Confidence.HIGH:
            continue
        if not remote_document_id:
            structural_failure = "EPO 官方文件行缺少文档标识"
            break
        row_application_cores, invalid_row_check = _row_application_cores(
            remote_document_id, id_cell, title_cell
        )
        if invalid_row_check:
            structural_failure = "EPO 文档行案件校验位无效"
            break
        if not row_application_cores:
            structural_failure = "EPO 官方文件行缺少可验证的案件标识"
            break
        if row_application_cores != {application_core}:
            structural_failure = "EPO 文档行案件标识与页面申请号不一致"
            break
        official_date = _normalize_epo_date(_cell(row, indexes["date"])["text"])
        if not official_date:
            date_failures += 1
            continue
        documents.append(
            ProsecutionDocument(
                jurisdiction="CN",
                application_number=normalized_application,
                publication_number=normalized_publication,
                source="epo_global_dossier",
                remote_document_id=remote_document_id,
                document_type=document_type,
                document_title=event_title_cn(document_type, ordinal, raw_title),
                raw_title=raw_title,
                official_date=official_date,
                direction="official",
                source_url=EPO_DOSSIER_URL.format(application=application_core),
                download_available=False,
                confidence=confidence,
                oa_ordinal=ordinal,
                document_code=document_code,
                document_version="ORIGINAL",
                source_trace=["epo_global_dossier"],
            )
        )

    if structural_failure:
        return SyncOutcome(
            code=ResultCode.PAGE_STRUCTURE_CHANGED,
            message=structural_failure,
        )
    if date_failures:
        return SyncOutcome(
            code=ResultCode.DATE_PARSE_FAILED,
            message="EPO 官方文件中存在无法解析的日期",
        )
    return SyncOutcome(
        code=ResultCode.OK,
        documents=documents,
        resolved_application_number=normalized_application,
    )


def fetch_html_public(url: str) -> tuple[int, str]:
    """Fetch public metadata HTML without credentials, cookies, or body files."""
    request = urllib.request.Request(
        url,
        headers={
            "User-Agent": (
                "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                "AppleWebKit/537.36 (KHTML, like Gecko) "
                "Chrome/128.0 Safari/537.36"
            ),
            "Accept-Language": "en",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            body = response.read(MAX_HTML_BYTES + 1)
            return int(response.status), body.decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        body = exc.read(MAX_HTML_BYTES + 1)
        return int(exc.code), body.decode("utf-8", errors="replace")
    except (OSError, urllib.error.URLError):
        return 0, ""


def _playwright_available() -> bool:
    try:
        from pathlib import Path

        from playwright.sync_api import sync_playwright

        with sync_playwright() as playwright:
            return Path(playwright.chromium.executable_path).is_file()
    except Exception:  # noqa: BLE001 - optional runtime dependency probe
        return False


def _http_error(status: int) -> Optional[SyncOutcome]:
    if status == 200:
        return None
    if status in (401, 403):
        return SyncOutcome(
            code=ResultCode.ACCESS_DENIED,
            message=f"EPO Global Dossier 请求被拒绝（HTTP {status}）",
        )
    if status == 429:
        return SyncOutcome(
            code=ResultCode.RATE_LIMITED,
            message="EPO Global Dossier 请求受限（HTTP 429）",
        )
    return SyncOutcome(
        code=ResultCode.NETWORK_ERROR,
        message=f"EPO Global Dossier HTTP {status}",
    )


def _html_too_large(html: str) -> bool:
    return len((html or "").encode("utf-8")) > MAX_HTML_BYTES


class EpoGlobalDossierProvider(DossierProvider):
    provider_id = "epo_global_dossier"
    jurisdiction = "CN"

    def __init__(self, fetch_html=None, browser_manager=None):
        self._fetch_html = fetch_html
        self._manager_injected = browser_manager is not None
        if fetch_html is not None:
            self._manager = browser_manager
        else:
            if browser_manager is None:
                from ..browser.manager import BrowserManager

                browser_manager = BrowserManager(
                    self.provider_id,
                    prefer_system_browser=False,
                    headless=True,
                )
            self._manager = browser_manager

    def health_check(self) -> bool:
        if self._fetch_html is not None:
            return callable(self._fetch_html)
        if self._manager_injected:
            return self._manager is not None
        return self._manager is not None and _playwright_available()

    def check_auth(self, cancel) -> ResultCode:
        return ResultCode.OK

    def ensure_login(self, cancel) -> ResultCode:
        return ResultCode.OK

    def resolve_case(
        self, application_number: str, publication_number: str, cancel
    ) -> SyncOutcome:
        normalized = _normalize_epo_application(application_number)
        if not normalized:
            return SyncOutcome(
                code=ResultCode.RESOLVE_FAILED,
                message="EPO Global Dossier 需要可识别的 CN 申请号",
            )
        return SyncOutcome(
            code=ResultCode.OK,
            resolved_application_number=normalized,
        )

    def list_documents(
        self, application_number: str, publication_number: str, cancel
    ) -> SyncOutcome:
        resolved = self.resolve_case(application_number, publication_number, cancel)
        if not resolved.ok:
            return resolved
        if cancel is not None and cancel.is_set():
            return SyncOutcome(
                code=ResultCode.TEMPORARY_ERROR,
                message="EPO Global Dossier 查询已取消",
            )
        application_core = _application_core(resolved.resolved_application_number)
        url = EPO_DOSSIER_URL.format(application=application_core)
        try:
            if self._fetch_html is not None:
                status, html = self._fetch_html(url)
            else:
                page = self._manager.open_page(url, cancel, fresh=True)
                status = getattr(self._manager, "last_navigation_status", None)
                if page is None:
                    if cancel is not None and cancel.is_set():
                        return SyncOutcome(
                            code=ResultCode.TEMPORARY_ERROR,
                            message="EPO Global Dossier 查询已取消",
                        )
                    return SyncOutcome(
                        code=ResultCode.NETWORK_ERROR,
                        message="EPO Global Dossier 浏览器导航失败",
                    )
                navigation_error = _http_error(
                    status if isinstance(status, int) else 0
                )
                if navigation_error is not None:
                    return navigation_error
                html = page.content()
        except Exception as exc:  # noqa: BLE001 - provider boundary
            return SyncOutcome(
                code=ResultCode.NETWORK_ERROR,
                message=f"EPO Global Dossier 请求失败: {type(exc).__name__}",
            )
        status_error = _http_error(status if isinstance(status, int) else 0)
        if status_error is not None:
            return status_error
        if _html_too_large(html):
            return SyncOutcome(
                code=ResultCode.TEMPORARY_ERROR,
                message="EPO Global Dossier 响应超过安全大小限制",
            )
        if cancel is not None and cancel.is_set():
            return SyncOutcome(
                code=ResultCode.TEMPORARY_ERROR,
                message="EPO Global Dossier 查询已取消",
            )
        return parse_epo_document_list(
            html, resolved.resolved_application_number, publication_number
        )

    def download_document(
        self, document: ProsecutionDocument, dest_path: str, cancel
    ) -> ResultCode:
        return ResultCode.UNSUPPORTED_JURISDICTION
