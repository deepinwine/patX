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
(query) => {
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
  const visibleTextFor = (root) => {
    if (!root || !isVisible(root)) return "";
    const parts = [];
    const walker = document.createTreeWalker(
      root,
      NodeFilter.SHOW_TEXT,
      {
        acceptNode(node) {
          const parent = node.parentElement;
          return parent && isVisible(parent) && node.nodeValue.trim()
            ? NodeFilter.FILTER_ACCEPT
            : NodeFilter.FILTER_REJECT;
        }
      }
    );
    while (walker.nextNode()) parts.push(walker.currentNode.nodeValue);
    return parts.join(" ").replace(/\s+/g, " ").trim();
  };
  const text = (element) => visibleTextFor(element);
  const rowsFor = (table) => Array.from(table.querySelectorAll("tr"))
    .filter(isVisible)
    .map((row) => Array.from(row.children)
      .filter((cell) => ["TH", "TD"].includes(cell.tagName))
      .map((cell, index) => {
        const visible = isVisible(cell);
        const link = visible
          ? Array.from(cell.querySelectorAll("a[href]")).find(isVisible)
          : undefined;
        return {
          index,
          visible,
          text: visible ? text(cell) : "",
          href: link ? (link.getAttribute("href") || "") : "",
          header: cell.tagName === "TH"
        };
      }))
    .filter((row) => row.length > 0);
  const caseTokensFor = (rows) => {
    const tokens = new Set();
    for (const row of rows) for (const cell of row) {
      const source = `${cell.text || ""} ${cell.href || ""}`.toUpperCase();
      for (const match of source.matchAll(/CN[-_\s]*(\d{9,14})(?=[.\/_\-\sA-Z]|$)/g)) {
        tokens.add(`CN${match[1]}`);
      }
    }
    return Array.from(tokens);
  };
  const displayedCaseAliases = (value) => {
    const aliases = new Set();
    const upper = (value || "").toUpperCase();
    for (const match of upper.matchAll(/CN\s*(\d{9,14})\.\d\b/g)) {
      aliases.add(`CN${match[1]}`);
    }
    for (const match of upper.matchAll(/CN\s*(\d{9,14})[A-Z]\b/g)) {
      aliases.add(`CN${match[1]}`);
    }
    return aliases;
  };
  const visibleText = visibleTextFor(document.body);
  const lowered = visibleText.toLowerCase();
  const pageMarker = lowered.includes("global dossier");
  const blocked = [
    "access denied", "request rejected", "forbidden",
    "sign in to global dossier", "login required", "security verification",
    "verify you are human", "captcha"
  ].some((marker) => lowered.includes(marker));
  const targetMatched = Boolean(query.targetToken) &&
    normalize(visibleText).includes(query.targetToken);
  const caseAliases = new Set([query.targetCore]);
  if (targetMatched) {
    for (const alias of displayedCaseAliases(visibleText)) caseAliases.add(alias);
  }
  const candidates = Array.from(document.querySelectorAll("table"))
    .filter(isVisible)
    .map((table) => ({table, rows: rowsFor(table)}))
    .filter(({rows}) => {
      const headerRow = rows.find((row) =>
        row.length > 0 && row.every((cell) => cell.header));
      const headers = (headerRow || [])
        .filter((cell) => cell.visible)
        .map((cell) => cell.text.toLowerCase());
      const has = (patterns) => headers.some((header) => patterns.some((pattern) => pattern.test(header)));
      return has([/^(document )?date$/, /publication date/]) &&
        has([/description/, /document title/, /^title$/]) &&
        has([/document code/, /^code$/]) &&
        has([/document id/, /remote id/, /^id$/]) &&
        has([/direction/, /category/, /party/]);
    })
    .map((candidate) => ({
      ...candidate,
      dataRows: candidate.rows.filter((row) => row.some((cell) => !cell.header)),
      tableCaseTokens: caseTokensFor(candidate.rows)
    }));
  const emptyState = Array.from(document.querySelectorAll(
    '[role="status"], [role="alert"], [class*="empty" i], [id*="empty" i], ' +
    '[class*="no-document" i], [id*="no-document" i], ' +
    '[class*="no-record" i], [id*="no-record" i]'
  )).filter(isVisible).find((element) => /\bno (documents|records)\b/i.test(text(element)));
  const loading = Array.from(document.querySelectorAll(
    '[aria-busy="true"], [role="progressbar"], [class*="loading" i], [class*="spinner" i]'
  )).some(isVisible);
  const hashMatched = window.location.hash === query.expectedHash;
  const safeFallback = Boolean(query.freshGeneration) && hashMatched &&
    targetMatched && !loading;
  const rowTokensFor = (candidate) => candidate.dataRows.map((row) =>
    caseTokensFor([row]));
  const mismatch = candidates.some((candidate) =>
    rowTokensFor(candidate).some((tokens) =>
      tokens.some((token) => !caseAliases.has(token))));
  const selected = candidates.find((candidate) => {
    const rowTokens = rowTokensFor(candidate);
    return rowTokens.length > 0
      ? rowTokens.every((tokens) =>
          tokens.length > 0
            ? tokens.every((token) => caseAliases.has(token))
            : safeFallback)
      : safeFallback;
  });
  let state = "PENDING";
  if (blocked) state = "BLOCKED";
  else if (mismatch) state = "MISMATCH";
  else if (pageMarker && selected && selected.dataRows.length > 0) state = "READY";
  else if (pageMarker && selected && selected.dataRows.length === 0 && emptyState) state = "EMPTY";
  return {
    state,
    targetMatched,
    rows: selected ? selected.rows : [],
    tableCaseTokens: selected ? selected.tableCaseTokens :
      candidates.flatMap((candidate) => candidate.tableCaseTokens),
    caseAliases: Array.from(caseAliases),
    safeFallback,
    hashMatched,
    loading,
    freshGeneration: Boolean(query.freshGeneration),
    pageMarker,
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
        self._row_cell_index = 0
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
            if tag in ("th", "td") and self._row is not None:
                self._cell = {
                    "text": "",
                    "href": "",
                    "header": tag == "th",
                    "index": self._row_cell_index,
                    "visible": False,
                }
                self._row_cell_index += 1
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
            self._row_cell_index = 0
        elif tag in ("th", "td") and self._row is not None:
            self._cell = {
                "text": "",
                "href": "",
                "header": tag == "th",
                "index": self._row_cell_index,
                "visible": True,
            }
            self._row_cell_index += 1
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
        if tag in ("th", "td") and self._cell is not None:
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
    visible_headers = [
        (position, cell)
        for position, cell in enumerate(row)
        if cell.get("visible", True)
    ]
    headers = [
        re.sub(r"\s+", " ", cell["text"]).strip().lower()
        for _, cell in visible_headers
    ]

    def find(*patterns: str) -> int:
        for (position, cell), header in zip(visible_headers, headers):
            if any(re.search(pattern, header) for pattern in patterns):
                return cell.get("index", position)
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
    for position, cell in enumerate(row):
        if cell.get("index", position) == index and cell.get(
            "visible", True
        ):
            return cell
    return {"text": "", "href": "", "visible": False, "index": index}


def _normal_number(raw: str, number_type: str) -> str:
    identifier = normalize_cn_identifier(raw)
    if identifier.number_type == number_type:
        return identifier.normalized_number
    return ""


def _identifier_token(raw: str) -> str:
    return re.sub(r"[^A-Z0-9]", "", (raw or "").upper())


_CASE_CORE_PATTERN = re.compile(
    r"CN[-_\s]*(\d{9,14})(?=[./_\-\sA-Z]|$)",
    re.IGNORECASE,
)


def _case_core(raw: str) -> str:
    identifier = normalize_cn_identifier(raw)
    if identifier.number_type == "application":
        return identifier.normalized_number.split(".", 1)[0]
    if identifier.number_type == "publication":
        return re.sub(r"[A-Z]$", "", identifier.normalized_number)
    match = _CASE_CORE_PATTERN.search(raw or "")
    return f"CN{match.group(1)}" if match else ""


def _case_tokens_from_text(text: str) -> set[str]:
    return {f"CN{match}" for match in _CASE_CORE_PATTERN.findall(text or "")}


def _displayed_case_aliases(text: str) -> set[str]:
    aliases = {
        f"CN{digits}"
        for digits in re.findall(
            r"\bCN\s*(\d{9,14})\.\d\b", text or "", re.IGNORECASE
        )
    }
    aliases.update(
        f"CN{digits}"
        for digits in re.findall(
            r"\bCN\s*(\d{9,14})[A-Z]\b", text or "", re.IGNORECASE
        )
    )
    return aliases


def _table_case_tokens(rows: list[list[dict]]) -> set[str]:
    tokens = set()
    for row in rows:
        for cell in row:
            tokens.update(
                _case_tokens_from_text(
                    f"{cell.get('text', '')} {cell.get('href', '')}"
                )
            )
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
    target_core = _case_core(target_number)
    page_aliases = _displayed_case_aliases(visible_text)
    target_matched = (
        not target_number
        or _identifier_token(target_number) in _identifier_token(visible_text)
    )
    case_aliases = set(page_aliases) if target_matched else set()
    if target_core:
        case_aliases.add(target_core)
    semantic_tables = []
    for table in parser.tables:
        if any(_header_indexes(row) is not None for row in table["rows"]):
            semantic_tables.append(table["rows"])
    for rows in semantic_tables:
        header_index = next(
            index
            for index, row in enumerate(rows)
            if _header_indexes(row) is not None
        )
        data_rows = rows[header_index + 1 :]
        if data_rows:
            row_tokens = [_table_case_tokens([row]) for row in data_rows]
            if any(tokens - case_aliases for tokens in row_tokens):
                return "PENDING"
            if all(row_tokens):
                return "READY"
    for empty_state in parser.empty_state_texts:
        if re.search(
            r"\bno (?:documents|records)\b",
            empty_state["text"],
            re.IGNORECASE,
        ) and semantic_tables and target_matched:
            return "READY"
    return "PENDING"


def parse_uspto_document_rows(
    rows: list[list[dict]],
    application_number: str = "",
    publication_number: str = "",
    *,
    explicit_empty: bool = False,
    case_aliases: Optional[set[str]] = None,
    binding_verified: bool = False,
) -> SyncOutcome:
    """Parse visible structured cells from either HTML or browser snapshots."""
    normalized_application = _normal_number(application_number, "application")
    normalized_publication = _normal_number(publication_number, "publication")
    indexes = None
    target_rows = None
    for row_index, row in enumerate(rows):
        candidate_indexes = _header_indexes(row)
        if candidate_indexes is not None:
            indexes = candidate_indexes
            target_rows = rows[row_index + 1 :]
            break
    if target_rows is None or indexes is None:
        return SyncOutcome(
            code=ResultCode.PAGE_STRUCTURE_CHANGED,
            message="未找到具有语义表头的 Global Dossier 文档表格",
        )

    if not target_rows:
        if explicit_empty:
            return SyncOutcome(
                code=ResultCode.OK,
                documents=[],
                resolved_application_number=normalized_application,
            )
        return SyncOutcome(
            code=ResultCode.PAGE_STRUCTURE_CHANGED,
            message="文档表格为空且未显示明确的无文档状态",
        )

    target_core = _case_core(normalized_application or normalized_publication)
    allowed_aliases = {
        core
        for raw_alias in (case_aliases or set())
        if (core := _case_core(raw_alias))
    }
    if target_core:
        allowed_aliases.add(target_core)
    required_indexes = set(indexes.values())
    for row in target_rows:
        if any(not _cell(row, index).get("visible", False) for index in required_indexes):
            return SyncOutcome(
                code=ResultCode.PAGE_STRUCTURE_CHANGED,
                message="Global Dossier 文档行缺少可见必需列",
            )
        row_tokens = _table_case_tokens([row])
        if row_tokens and not row_tokens.issubset(allowed_aliases):
            return SyncOutcome(
                code=ResultCode.PAGE_STRUCTURE_CHANGED,
                message="Global Dossier 文档表格混入其他案件记录",
            )
        if not row_tokens and not binding_verified:
            return SyncOutcome(
                code=ResultCode.PAGE_STRUCTURE_CHANGED,
                message="Global Dossier 文档行无法绑定到目标案件",
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


def parse_uspto_document_list(
    html: str, application_number: str = "", publication_number: str = ""
) -> SyncOutcome:
    """Parse a saved Global Dossier document list without network access."""
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
    if "global dossier" not in lowered:
        return SyncOutcome(
            code=ResultCode.PAGE_STRUCTURE_CHANGED,
            message="未识别 USPTO Global Dossier 页面标志",
        )

    target_number = (
        _normal_number(application_number, "application")
        or _normal_number(publication_number, "publication")
    )
    target_core = _case_core(target_number)
    target_matched = (
        not target_number
        or _identifier_token(target_number) in _identifier_token(page_text)
    )
    case_aliases = {target_core} if target_core else set()
    if target_matched:
        case_aliases.update(_displayed_case_aliases(page_text))
    semantic_tables = [
        table["rows"]
        for table in parser.tables
        if any(_header_indexes(row) is not None for row in table["rows"])
    ]
    if not semantic_tables:
        return SyncOutcome(
            code=ResultCode.PAGE_STRUCTURE_CHANGED,
            message="未找到与目标案件匹配的 Global Dossier 文档表格",
        )
    for rows in semantic_tables:
        if _table_case_tokens(rows) - case_aliases:
            return SyncOutcome(
                code=ResultCode.PAGE_STRUCTURE_CHANGED,
                message="Global Dossier 文档表格案件号与查询目标不一致",
            )
    selected_rows = semantic_tables[0]

    explicit_empty = any(
        re.search(
            r"\bno (?:documents|records)\b",
            empty_state["text"],
            re.IGNORECASE,
        )
        for empty_state in parser.empty_state_texts
    )
    header_index = next(
        index
        for index, row in enumerate(selected_rows)
        if _header_indexes(row) is not None
    )
    if (
        explicit_empty
        and not selected_rows[header_index + 1 :]
        and target_number
        and not target_matched
    ):
        return SyncOutcome(
            code=ResultCode.PAGE_STRUCTURE_CHANGED,
            message="Global Dossier 无文档状态无法绑定到目标案件",
        )
    return parse_uspto_document_rows(
        selected_rows,
        application_number,
        publication_number,
        explicit_empty=explicit_empty,
        case_aliases=case_aliases,
        binding_verified=False,
    )


def _snapshot_rows(snapshot: dict) -> Optional[list[list[dict]]]:
    raw_rows = snapshot.get("rows")
    if not isinstance(raw_rows, list):
        return None
    rows = []
    for raw_row in raw_rows:
        if not isinstance(raw_row, list):
            return None
        row = []
        for raw_cell in raw_row:
            if not isinstance(raw_cell, dict):
                return None
            text = raw_cell.get("text", "")
            href = raw_cell.get("href", "")
            header = raw_cell.get("header")
            index = raw_cell.get("index")
            visible = raw_cell.get("visible")
            if not isinstance(text, str) or not isinstance(href, str):
                return None
            if not isinstance(header, bool) or not isinstance(visible, bool):
                return None
            if not isinstance(index, int) or index < 0:
                return None
            row.append({
                "text": text,
                "href": href,
                "header": header,
                "index": index,
                "visible": visible,
            })
        if row:
            rows.append(row)
    return rows


def _snapshot_is_bound(
    snapshot: dict, rows: list[list[dict]], target_core: str
) -> bool:
    visible_text = snapshot.get("visibleText", "")
    if not isinstance(visible_text, str) or "global dossier" not in visible_text.lower():
        return False
    case_aliases = {target_core} if target_core else set()
    if snapshot.get("targetMatched") is True:
        case_aliases.update(_displayed_case_aliases(visible_text))
    safe_fallback = all((
        snapshot.get("freshGeneration") is True,
        snapshot.get("hashMatched") is True,
        snapshot.get("targetMatched") is True,
        snapshot.get("loading") is False,
        snapshot.get("safeFallback") is True,
    ))
    header_index = next(
        (
            index
            for index, row in enumerate(rows)
            if _header_indexes(row) is not None
        ),
        None,
    )
    if header_index is None:
        return False
    data_rows = rows[header_index + 1 :]
    if not data_rows:
        return safe_fallback
    for row in data_rows:
        row_tokens = _table_case_tokens([row])
        if row_tokens:
            if not row_tokens.issubset(case_aliases):
                return False
        elif not safe_fallback:
            return False
    return True


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
        self,
        page,
        target_token: str,
        target_core: str,
        expected_hash: str,
        cancel,
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
                    {
                        "targetToken": target_token,
                        "targetCore": target_core,
                        "expectedHash": expected_hash,
                        "freshGeneration": True,
                    },
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
            if state == "MISMATCH":
                return SyncOutcome(
                    code=ResultCode.PAGE_STRUCTURE_CHANGED,
                    message="Global Dossier 文档表格混入其他案件记录",
                ), None
            if state in ("READY", "EMPTY"):
                rows = _snapshot_rows(snapshot)
                if rows is not None:
                    if not any(_header_indexes(row) is not None for row in rows):
                        return SyncOutcome(
                            code=ResultCode.PAGE_STRUCTURE_CHANGED,
                            message="Global Dossier 可见文档表头不完整",
                        ), None
                    if _snapshot_is_bound(snapshot, rows, target_core):
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
        target_number = (
            _normal_number(application_number, "application")
            or _normal_number(publication_number, "publication")
        )
        target_core = _case_core(target_number)
        expected_hash = f"#{urllib.parse.urlparse(url).fragment}"
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
            target_core,
            expected_hash,
            cancel,
        )
        if wait_outcome is not None:
            return wait_outcome
        rows = _snapshot_rows(snapshot or {})
        if rows is None:
            return SyncOutcome(
                code=ResultCode.PAGE_STRUCTURE_CHANGED,
                message="Global Dossier 可见文档快照不完整",
            )
        snapshot_data = snapshot or {}
        visible_text = snapshot_data.get("visibleText", "")
        case_aliases = {target_core} if target_core else set()
        if snapshot_data.get("targetMatched") is True and isinstance(
            visible_text, str
        ):
            case_aliases.update(_displayed_case_aliases(visible_text))
        binding_verified = all((
            snapshot_data.get("freshGeneration") is True,
            snapshot_data.get("hashMatched") is True,
            snapshot_data.get("targetMatched") is True,
            snapshot_data.get("loading") is False,
            snapshot_data.get("safeFallback") is True,
        ))
        header_index = next(
            (
                index
                for index, row in enumerate(rows)
                if _header_indexes(row) is not None
            ),
            None,
        )
        data_rows = rows[header_index + 1 :] if header_index is not None else rows
        explicit_empty = bool(
            not data_rows
            and re.search(
                r"\bno (?:documents|records)\b",
                str(snapshot_data.get("emptyText", "")),
                re.IGNORECASE,
            )
        )
        return parse_uspto_document_rows(
            rows,
            application_number,
            publication_number,
            explicit_empty=explicit_empty,
            case_aliases=case_aliases,
            binding_verified=binding_verified,
        )

    def download_document(
        self, document: ProsecutionDocument, dest_path: str, cancel
    ) -> ResultCode:
        return ResultCode.UNSUPPORTED_JURISDICTION
