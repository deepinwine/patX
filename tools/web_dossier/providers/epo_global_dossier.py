"""Public EPO Global Dossier metadata provider for CN applications.

The pure parser accepts saved HTML and never downloads document bodies.  The
provider uses the public European Patent Register entry and requires a real CN
application number; a publication number is never guessed to be one.
"""
from __future__ import annotations

import re
import urllib.error
import urllib.request
from html.parser import HTMLParser
from typing import Optional

from ..document_classifier import (
    classify_official_document,
    direction_for,
    event_title_cn,
)
from ..models import Confidence, DocumentType, ProsecutionDocument, ResultCode, normalize_date
from ..number_resolver import normalize_cn_identifier
from .base import DossierProvider, SyncOutcome


EPO_DOSSIER_URL = (
    "https://register.epo.org/ipfwretrieve?apn=CN.{application}.A&lng=en"
)

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
    return (
        "hidden" in attributes
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
    identifier = normalize_cn_identifier(raw)
    return identifier.normalized_number if identifier.number_type == expected else ""


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


def _version_from_title(raw_title: str) -> str:
    match = re.search(r"\((ORIGINAL|TRANSLATED)\)\s*$", raw_title or "", re.IGNORECASE)
    return match.group(1).upper() if match else ""


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
    dated_candidates = 0
    date_failures = 0
    structural_failure = ""
    normalized_application = (
        expected_application
        if "." in expected_application
        else displayed_application
    )
    normalized_publication = displayed_publication or expected_publication
    application_core = normalize_cn_identifier(normalized_application).number
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
        version = (
            _cell(row, indexes["version"])["text"].strip().upper()
            if "version" in indexes
            else _version_from_title(raw_title)
        )
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
        dated_candidates += 1
        official_date = _normalize_epo_date(_cell(row, indexes["date"])["text"])
        if not official_date:
            date_failures += 1
            continue
        if not remote_document_id:
            structural_failure = "EPO 官方文件行缺少文档标识"
            break
        remote_application = re.match(
            r"^(?:CN[-_.]?)?(\d{12})", remote_document_id.upper()
        )
        if remote_application and remote_application.group(1) != application_core:
            structural_failure = "EPO 文档标识与页面申请号不一致"
            break
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
    if dated_candidates and date_failures == dated_candidates:
        return SyncOutcome(
            code=ResultCode.DATE_PARSE_FAILED,
            message="所有候选 EPO 官方文件的日期均无法解析",
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
            return int(response.status), response.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        return int(exc.code), exc.read().decode("utf-8", errors="replace")
    except (OSError, urllib.error.URLError):
        return 0, ""


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

    def resolve_case(
        self, application_number: str, publication_number: str, cancel
    ) -> SyncOutcome:
        identifier = normalize_cn_identifier(application_number)
        if identifier.number_type != "application":
            return SyncOutcome(
                code=ResultCode.RESOLVE_FAILED,
                message="EPO Global Dossier 需要可识别的 CN 申请号",
            )
        return SyncOutcome(
            code=ResultCode.OK,
            resolved_application_number=identifier.normalized_number,
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
        identifier = normalize_cn_identifier(resolved.resolved_application_number)
        url = EPO_DOSSIER_URL.format(application=identifier.number)
        try:
            status, html = self._fetch_html(url)
        except Exception as exc:  # noqa: BLE001 - provider boundary
            return SyncOutcome(
                code=ResultCode.NETWORK_ERROR,
                message=f"EPO Global Dossier 请求失败: {type(exc).__name__}",
            )
        if status == 429:
            return SyncOutcome(code=ResultCode.RATE_LIMITED, message="EPO 限流")
        if status != 200:
            return SyncOutcome(
                code=ResultCode.NETWORK_ERROR,
                message=f"EPO Global Dossier HTTP {status}",
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
