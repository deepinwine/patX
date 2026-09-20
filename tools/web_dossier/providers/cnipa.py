"""CNIPA provider - browser-assisted dossier queries.

Two layers:

1. `parse_dossier_documents(html, ...)` - PURE parsing of the 审查信息
   document table (stdlib html.parser, no network). Unit-tested against
   checked-in fixtures under fixtures/cnipa/. Page-structure drift raises
   ResultCode.PAGE_STRUCTURE_CHANGED, never a silent mis-parse.

2. `CNIPAWebProvider` - the Playwright flow. Queries at human pace (one
   page, minimum interval between queries), reuses a persistent browser
   profile the USER logged into (we never read passwords or automate
   captchas), and captures debug artifacts on failure. When the environment
   variable PATX_DOSSIER_FIXTURE_DIR points at a fixture directory, the
   provider serves those saved pages instead of touching the network -
   that is how CI and offline development run.

Selector strategy: semantic fallbacks (role/text/label) first, positional
CSS only as the last step; every miss escalates to PAGE_STRUCTURE_CHANGED.

The real-site flow follows the field-tested knowledge documented in
github.com/teamilkman/cnipa-cpquery (same guardrails: own account, real
browser, page-context requests, human pacing): cpquery is a Ruishu-protected
Vue SPA, so the document list comes from the site's own JSON APIs fetched
INSIDE the logged-in page (the anti-bot layer only signs page-issued
requests) instead of DOM scraping:

    POST /api/view/gn/scxx/tzs  通知书清单  {zhuanlisqh, nodeId:'aj_gk_scxx_tzs',  anjianbh:''}
    POST /api/view/gn/scxx/zjwj 中间文件清单 {zhuanlisqh, nodeId:'aj_gk_scxx_zjwj', anjianbh:''}

List rows: {name: 'YYYY-MM-DD  文件名', ds: TZS|ZJWJ|SQWJ, additionalData:{rid,...}}.
Auth: Authorization: Bearer <localStorage.ACCESS_TOKEN> + userType header.
Empty-body 200s and HTML intercepts are transient: one retry after a pause,
never conclude "no access" from them.
"""
from __future__ import annotations

import os
import re
import threading
import time
from html.parser import HTMLParser
from pathlib import Path
from typing import List, Optional

from ..document_classifier import classify_cn_title, direction_for
from ..models import (Confidence, DocumentType, ProsecutionDocument, ResultCode,
                      normalize_date)
from ..number_resolver import api_application_number, normalize_cn_identifier
from .base import DossierProvider, SyncOutcome

BASE_URL = os.environ.get("PATX_CNIPA_BASE_URL",
                           "https://cpquery.cponline.cnipa.gov.cn")

# cpquery JSON list APIs (page-context fetch; see module docstring)
API_TZS = "/api/view/gn/scxx/tzs"      # 通知书（审查意见/授权/驳回…）
API_ZJWJ = "/api/view/gn/scxx/zjwj"    # 中间文件（意见陈述/补正/替换文件…）
API_FILE_INFOS = "/api/view/gn/fetch-file-infos"        # 案卷页清单
API_FILE_PAGE = "/api/pcshoss/view/fetch-file"          # 页本体（OSS 签名 URL）
LIST_RETRY_PAUSE_SECONDS = 7           # field-tested: transient blocks clear

DS_LABELS = {"TZS": "通知书", "ZJWJ": "中间文件", "SQWJ": "申请文件"}

# Markers of the login wall vs an authenticated page (multi-fallback; the
# QR-login page exposes label.title-item-btn and button.qrImg per the
# field-tested DOM notes).
LOGIN_MARKERS = ("用户登录", "请登录", "统一身份认证", "title-item-btn", "qrImg")
LOGGED_IN_MARKERS = ("退出", "欢迎您", "您好")

# Same-day throttling: minimum pause between dossier queries. Human-paced on
# purpose; this is case management for one's own portfolio, not crawling.
MIN_QUERY_INTERVAL_SECONDS = float(os.environ.get("PATX_DOSSIER_MIN_INTERVAL", "8"))
BLOCK_MARKERS = ("系统繁忙", "访问过于频繁", "验证码错误", "安全验证")
STEP_WAIT_MS = 20_000


# ---------------------------------------------------------------------------
# Pure HTML parsing
# ---------------------------------------------------------------------------

class _TableCollector(HTMLParser):
    """Collects <tr> cell texts + first href per row for every table."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.tables: List[dict] = []      # {"headers": [...], "rows": [[{text,href}...]]}
        self._table_stack: List[dict] = []
        self._row: Optional[list] = None
        self._cell: Optional[dict] = None

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "table":
            self._table_stack.append({"headers": [], "rows": []})
        elif tag == "tr" and self._table_stack:
            self._row = []
        elif tag in ("td", "th") and self._row is not None:
            self._cell = {"text": "", "href": ""}
        elif tag == "a" and self._cell is not None:
            self._cell["href"] = a.get("href", "")

    def handle_data(self, data):
        if self._cell is not None:
            self._cell["text"] += data

    def handle_endtag(self, tag):
        if tag in ("td", "th") and self._cell is not None:
            text = re.sub(r"\s+", " ", self._cell["text"]).strip()
            self._row.append({"text": text, "href": self._cell["href"]})
            self._cell = None
        elif tag == "tr" and self._row is not None:
            if self._table_stack:
                if self._looks_like_header(self._row):
                    self._table_stack[-1]["headers"].append(
                        [c["text"] for c in self._row])
                else:
                    self._table_stack[-1]["rows"].append(self._row)
            self._row = None
        elif tag == "table" and self._table_stack:
            t = self._table_stack.pop()
            if not self._table_stack and (t["rows"] or t["headers"]):
                self.tables.append(t)
            elif self._table_stack:
                self._table_stack[-1]["rows"].extend(t["rows"])

    @staticmethod
    def _looks_like_header(row) -> bool:
        texts = "".join(c["text"] for c in row)
        return bool(re.search(r"序号|发文日|文件类型|名称|类型|日期", texts)) and \
            not re.search(r"\d{4}", texts)


def parse_dossier_documents(html: str, application_number: str = "",
                            publication_number: str = "") -> SyncOutcome:
    """Parses a 审查信息 page into ProsecutionDocuments.

    The dossier table is identified by header keywords (发文日 + 文件类型/
    名称), not by DOM position. Any parse failure is PAGE_STRUCTURE_CHANGED.
    """
    parser = _TableCollector()
    try:
        parser.feed(html)
    except Exception:
        return SyncOutcome(code=ResultCode.PAGE_STRUCTURE_CHANGED,
                           message="审查信息页面解析异常")

    # 1) The middle-document table: header mentions 发文日 and a type column
    target = None
    for table in parser.tables:
        flat = " ".join(" ".join(h) for h in table["headers"]) + \
               " " + " ".join(c["text"] for r in table["rows"][:2] for c in r)
        if re.search(r"发文日|发文日期", flat) and re.search(r"文件类型|中间文件|名称", flat):
            target = table
            break
    if target is None:
        return SyncOutcome(code=ResultCode.PAGE_STRUCTURE_CHANGED,
                           message="未找到审查文件列表表格（页面结构可能已改版）")

    header_strong = bool(target["headers"])
    docs: List[ProsecutionDocument] = []
    for i, row in enumerate(target["rows"]):
        if not row:
            continue
        cells = [c["text"] for c in row]
        date = ""
        for cell in cells:
            date = normalize_date(cell)
            if date:
                break
        # The title cell: the longest CJK text that is not the date itself
        title = ""
        href = ""
        for c in row:
            t = c["text"]
            if t and t != date and not re.fullmatch(r"[\d\s./年月日-]+", t) and \
                    len(t) > len(title):
                title = t
                href = c["href"]
        if not title:
            continue
        doc_type, ordinal, confidence = classify_cn_title(title)
        docs.append(ProsecutionDocument(
            jurisdiction="CN",
            application_number=application_number,
            publication_number=publication_number,
            source="cnipa",
            remote_document_id=href or f"row-{i+1}",
            document_type=doc_type,
            document_title=title,
            raw_title=title,
            official_date=date,
            direction=direction_for(doc_type),
            source_url=BASE_URL,
            download_url=href if href.startswith("http") else "",
            download_available=bool(href),
            confidence=Confidence.HIGH if (header_strong and date) else
                       (Confidence.MEDIUM if date else Confidence.LOW),
            oa_ordinal=ordinal,
        ))

    if not docs:
        return SyncOutcome(code=ResultCode.PAGE_STRUCTURE_CHANGED,
                           message="审查文件表格无有效行（页面结构可能已改版）")
    outcome = SyncOutcome(code=ResultCode.OK, documents=docs,
                          resolved_application_number=application_number)
    return outcome




# ---------------------------------------------------------------------------
# Pure parsing of the cpquery JSON list APIs
# ---------------------------------------------------------------------------

def looks_like_intercept(body: str) -> bool:
    """Ruishu intercepts show up as HTML (instead of JSON) or as an empty
    200 body - both transient per field testing, never 'no access'."""
    t = (body or "").lstrip()
    if not t:
        return True
    return t.startswith("<")


def parse_cpquery_list_response(bodies, application_number="", publication_number=""):
    """Parses the concatenated tzs + zjwj JSON list responses.

    bodies: list of (api_path, body_text). Rows carry name
    'YYYY-MM-DD  <文件名>' (two spaces) and additionalData.rid.
    Returns SyncOutcome; intercept-looking bodies -> TEMPORARY_ERROR.
    """
    docs: List[ProsecutionDocument] = []
    for api_path, body in bodies:
        if looks_like_intercept(body):
            return SyncOutcome(code=ResultCode.TEMPORARY_ERROR,
                               message="清单接口返回拦截页/空响应（瞬时），稍后重试")
        try:
            import json as _json
            payload = _json.loads(body)
        except ValueError:
            return SyncOutcome(code=ResultCode.PAGE_STRUCTURE_CHANGED,
                               message="清单响应不是 JSON（页面结构可能已改版）")
        if payload.get("code") != 200 or not isinstance(payload.get("data"), list):
            return SyncOutcome(code=ResultCode.PAGE_STRUCTURE_CHANGED,
                               message=f"清单响应结构异常 code={payload.get('code')}")
        for i, row in enumerate(payload["data"]):
            name = (row.get("name") or "").strip()
            if not name:
                continue
            m = re.match(r"^(\d{4}-\d{2}-\d{2})\s+(.+)$", name)
            date = normalize_date(m.group(1)) if m else ""
            title = (m.group(2) if m else name).strip()
            add = row.get("additionalData") or {}
            rid = str(add.get("rid") or row.get("nodeId") or f"row-{i+1}")
            doc_type, ordinal, confidence = classify_cn_title(title)
            docs.append(ProsecutionDocument(
                jurisdiction="CN",
                application_number=application_number,
                publication_number=publication_number,
                source="cnipa",
                remote_document_id=rid,
                document_type=doc_type,
                document_title=title,
                raw_title=title,
                official_date=date,
                direction=direction_for(doc_type),
                source_url=BASE_URL + api_path,
                download_available=bool(add.get("rid")),
                confidence=Confidence.HIGH if date else Confidence.MEDIUM,
                oa_ordinal=ordinal,
                ds=str(row.get("ds") or add.get("ds") or ""),
                wenjiandm=str(add.get("wenjiandm") or ""),
            ))
    if not docs:
        return SyncOutcome(code=ResultCode.PAGE_STRUCTURE_CHANGED,
                           message="清单为空且无有效行（结构可能已改版）")
    return SyncOutcome(code=ResultCode.OK, documents=docs,
                       resolved_application_number=application_number)


# JS executed INSIDE the logged-in cpquery page: the anti-bot layer signs
# page-issued requests automatically, so the JSON APIs must be fetched here -
# a bare replay from outside the page gets blocked by design.
_PAGE_FETCH_JS = """async (payload) => {
    const token = localStorage.getItem('ACCESS_TOKEN');
    const userType = localStorage.getItem('USER_TYPE');
    const resp = await fetch(payload.path, {
        method: 'POST',
        headers: Object.assign(
            {'Content-Type': 'application/json'},
            token ? {'Authorization': 'Bearer ' + token} : {},
            userType ? {'userType': userType} : {}),
        body: JSON.stringify(payload.body),
    });
    const text = await resp.text();
    return {status: resp.status, text: text.slice(0, 500000)};
}"""



# ---------------------------------------------------------------------------
# Document download (pure helpers, unit-tested)
# ---------------------------------------------------------------------------

def build_fetch_file_url(params: dict) -> str:
    """Manual concatenation on purpose: URLSearchParams would encode the '/'
    characters inside osslujing as %2F, which 404s on the OSS endpoint
    (field-tested pitfall). Values here contain no reserved characters."""
    parts = []
    for key in ("osslujing", "wenjianhzm", "timestamp", "sign", "isDN",
                "ds", "wenjiandm"):
        value = params.get(key)
        if value is not None:
            parts.append(f"{key}={value}")
    return API_FILE_PAGE + "?" + "&".join(parts)


def sniff_format(data: bytes) -> str:
    """The manifest's wenjianhzm lies (says PNG, body is PDF): trust the
    magic bytes instead."""
    if data[:4] == b"%PDF":
        return "pdf"
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        return "png"
    return "unknown"


def build_document_filename(official_date: str, ds: str, title: str,
                            suffix: str, exists_fn, pages=None) -> str:
    """'<日期>_<类别>_<名称>_<N>页.ext', _2/_3 on duplicate display names so
    every dossier record keeps its own file."""
    date = official_date or "无日期"
    label = DS_LABELS.get(ds, ds or "文件")
    safe = re.sub(r'[\\/:*?"<>| ]+', "_", title).strip("_")[:60] or "文档"
    base = f"{date}_{label}_{safe}" + (f"_{pages}页" if pages else "")
    name = base + suffix
    n = 2
    while exists_fn(name):
        name = f"{base}_{n}{suffix}"
        n += 1
    return name


def parse_file_infos(body: str):
    """Parses a fetch-file-infos response into (page_params, wenjianhzm).
    page_params: list of dicts for build_fetch_file_url. Errors return
    (None, error_message)."""
    if looks_like_intercept(body):
        return None, "页清单返回拦截页/空响应（瞬时）"
    try:
        import json as _json
        payload = _json.loads(body)
    except ValueError:
        return None, "页清单响应不是 JSON"
    data = payload.get("data") or {}
    paths = data.get("ossLujingList") or []
    if not paths:
        return None, "页清单为空"
    pages = []
    for p in paths:
        pages.append({
            "osslujing": p.get("osslujing", ""),
            "timestamp": p.get("timestamp", ""),
            "sign": p.get("sign", ""),
            "isDN": "true" if p.get("isDN") else "false",
            "wenjianhzm": data.get("wenjianhzm", ""),
            "ds": data.get("ds", ""),
            "wenjiandm": data.get("wenjiandm", ""),
        })
    return pages, data.get("wenjianhzm", "")


# Binary in-page fetch: returns base64 (page bodies are PDF/PNG streams).
_PAGE_BINARY_JS = """async (payload) => {
    const resp = await fetch(payload.url, {method: 'GET'});
    const buf = await resp.arrayBuffer();
    const bytes = new Uint8Array(buf);
    let binary = '';
    const chunk = 0x8000;
    for (let i = 0; i < bytes.length; i += chunk) {
        binary += String.fromCharCode.apply(null, bytes.subarray(i, i + chunk));
    }
    return {status: resp.status, b64: btoa(binary)};
}"""

def looks_like_login_page(html: str) -> bool:
    hit = sum(1 for m in LOGIN_MARKERS if m in html)
    return hit >= 2 or ("登录" in html and "密码" in html)


def looks_logged_in(html: str) -> bool:
    return any(m in html for m in LOGGED_IN_MARKERS) and not looks_like_login_page(html)


def looks_blocked(html: str) -> bool:
    return any(m in html for m in BLOCK_MARKERS)


# ---------------------------------------------------------------------------
# Browser-assisted provider
# ---------------------------------------------------------------------------

class CNIPAWebProvider(DossierProvider):
    provider_id = "cnipa"
    jurisdiction = "CN"

    def __init__(self, browser_manager=None, fixture_dir: Optional[str] = None):
        self._manager = browser_manager
        self._fixture_dir = Path(fixture_dir or os.environ.get("PATX_DOSSIER_FIXTURE_DIR", ""))
        self._last_query_at = 0.0
        self._lock = threading.Lock()

    # ---- infrastructure -------------------------------------------------
    def health_check(self) -> bool:
        if self._fixture_dir.name:
            return (self._fixture_dir / "dossier_list.html").exists()
        try:
            from ..browser.manager import BrowserManager   # noqa: F401
            return True
        except ImportError:
            return False

    def _pace(self):
        """Sleep so consecutive queries keep a human pace (per process)."""
        wait = self._last_query_at + MIN_QUERY_INTERVAL_SECONDS - time.monotonic()
        if wait > 0:
            time.sleep(wait)
        self._last_query_at = time.monotonic()

    # ---- auth ------------------------------------------------------------
    def check_auth(self, cancel) -> ResultCode:
        if self._fixture_dir.name:
            html = (self._fixture_dir / "dossier_list.html").read_text("utf-8")
            return ResultCode.OK if looks_logged_in(html) else ResultCode.AUTH_REQUIRED
        page = self._manager.open_page(BASE_URL, cancel)
        if page is None:
            return ResultCode.NETWORK_ERROR
        html = page.content()
        if looks_blocked(html):
            return ResultCode.RATE_LIMITED
        if looks_like_login_page(html) and not looks_logged_in(html):
            return ResultCode.AUTH_REQUIRED
        return ResultCode.OK

    def ensure_login(self, cancel) -> ResultCode:
        """Visible browser; the user does their own login/captcha."""
        if self._fixture_dir.name:
            return ResultCode.OK   # fixture mode is always "logged in"
        return self._manager.ensure_login(BASE_URL, looks_logged_in, cancel)

    # ---- case resolution --------------------------------------------------
    def resolve_case(self, application_number: str, publication_number: str,
                     cancel) -> SyncOutcome:
        # Normalize what we got. A CN publication number is NOT an application
        # number - when only the publication is known, the site's publication
        # search resolves it; failure is RESOLVE_FAILED, never a guess.
        app_ident = normalize_cn_identifier(application_number or "")
        pub_ident = normalize_cn_identifier(publication_number or "")
        if app_ident.number_type != "application":
            app_ident = None
        if not app_ident and pub_ident.number_type != "publication":
            return SyncOutcome(code=ResultCode.RESOLVE_FAILED,
                               message="申请号与公开号均无法识别（需要 202410123457.5 或 CN119870049A 形式）")
        if app_ident:
            return SyncOutcome(code=ResultCode.OK,
                               resolved_application_number=app_ident.normalized_number)
        return self._resolve_publication_via_site(pub_ident.normalized_number, cancel)

    def _resolve_publication_via_site(self, pub_no: str, cancel) -> SyncOutcome:
        if self._fixture_dir.name:
            # Fixture flow: dossier fixture embeds the application number line
            html = (self._fixture_dir / "dossier_list.html").read_text("utf-8")
            m = re.search(r"申请号[：:]\s*([0-9.]+)", html)
            if m:
                return SyncOutcome(code=ResultCode.OK,
                                   resolved_application_number=m.group(1))
            return SyncOutcome(code=ResultCode.RESOLVE_FAILED,
                               message="fixture 中未找到申请号")
        # Live: the 公布公告 query by publication number. If the site does
        # not resolve it, say so instead of guessing.
        page = self._manager.open_page(f"{BASE_URL}/pubquery", cancel)
        if page is None:
            return SyncOutcome(code=ResultCode.NETWORK_ERROR, message="无法打开公布公告查询")
        html = page.content()
        if looks_like_login_page(html):
            return SyncOutcome(code=ResultCode.AUTH_REQUIRED, message="CNIPA 需要登录")
        m = re.search(r"申请号[：:]\s*([0-9]{12}(?:\.[0-9])?)", html)
        if not m:
            return SyncOutcome(code=ResultCode.RESOLVE_FAILED,
                               message=f"公开号 {pub_no} 未能解析出申请号，请人工确认后补录申请号")
        return SyncOutcome(code=ResultCode.OK, resolved_application_number=m.group(1))

    # ---- documents ----------------------------------------------------------
    def list_documents(self, application_number: str, publication_number: str,
                       cancel) -> SyncOutcome:
        resolve = self.resolve_case(application_number, publication_number, cancel)
        if not resolve.ok and resolve.code != ResultCode.NO_CHANGE:
            return resolve

        app_no = resolve.resolved_application_number
        if self._fixture_dir.name:
            name = "dossier_list_no_oa.html" if application_number.endswith("X") \
                else "dossier_list.html"
            path = self._fixture_dir / name
            if not path.exists():
                path = self._fixture_dir / "dossier_list.html"
            html = path.read_text("utf-8")
            if looks_like_login_page(html):
                return SyncOutcome(code=ResultCode.AUTH_REQUIRED, message="CNIPA 需要登录")
            return parse_dossier_documents(html, app_no, publication_number)

        app13 = api_application_number(application_number) or \
                 api_application_number(resolve.resolved_application_number)
        if not app13:
            return SyncOutcome(code=ResultCode.RESOLVE_FAILED,
                               message=f"申请号 {app_no} 无法规范为 13 位 API 形式")

        with self._lock:          # exactly one page doing queries at a time
            self._pace()
            page = self._manager.open_page(BASE_URL, cancel)
            if page is None:
                return SyncOutcome(code=ResultCode.NETWORK_ERROR, message="站点打开失败")
            try:
                html = page.content()
                if looks_like_login_page(html) and not looks_logged_in(html):
                    return SyncOutcome(code=ResultCode.AUTH_REQUIRED,
                                       message="CNIPA 登录状态已失效")
                if not self._search_case(page, app13, cancel):
                    self._manager.capture_debug_artifacts(
                        page, "PAGE_STRUCTURE_CHANGED", app13)
                    return SyncOutcome(code=ResultCode.PAGE_STRUCTURE_CHANGED,
                                       message="检索/进入案件失败（页面结构可能已改版）")

                bodies = []
                for api in (API_TZS, API_ZJWJ):
                    body = self._page_fetch_list(page, api, app13, cancel)
                    if body is None:
                        if cancel is not None and cancel.is_set():
                            return SyncOutcome(code=ResultCode.TEMPORARY_ERROR,
                                               message="已取消")
                        # transient intercept/empty body: one paced retry,
                        # never conclude "no access" from it
                        time.sleep(LIST_RETRY_PAUSE_SECONDS)
                        body = self._page_fetch_list(page, api, app13, cancel)
                    if body is None:
                        self._manager.capture_debug_artifacts(
                            page, "RATE_LIMITED", app13)
                        return SyncOutcome(
                            code=ResultCode.RATE_LIMITED,
                            message="清单请求被拦截（瞬时），本批已停止，请稍后再试")
                    bodies.append((api, body))
            except Exception as exc:   # noqa: BLE001 - mapped to a result code
                self._manager.capture_debug_artifacts(page, "TEMPORARY_ERROR", app13)
                return SyncOutcome(code=ResultCode.TEMPORARY_ERROR,
                                   message=f"页面交互失败: {type(exc).__name__}")

        outcome = parse_cpquery_list_response(bodies, app_no, publication_number)
        if outcome.code == ResultCode.PAGE_STRUCTURE_CHANGED:
            self._manager.capture_debug_artifacts(page, "PAGE_STRUCTURE_CHANGED", app13)
        return outcome

    def _search_case(self, page, app13: str, cancel) -> bool:
        """Search by application number and open the case, using the
        field-tested selectors (placeholder text / 查询 button / result link)."""
        try:
            box = page.locator('input[placeholder*="例如"]')
            if box.count() == 0:
                box = page.get_by_placeholder("申请号")
            if box.count() == 0:
                return False
            box.first.click()
            box.first.fill(app13)
            search = page.locator('button.q-btn--st',
                                  has_text="查").first
            if search.count() == 0:
                search = page.get_by_role("button", name="查询").first
            if search.count() == 0:
                return False
            search.click()
            page.wait_for_load_state("networkidle", timeout=STEP_WAIT_MS)
            link = page.locator("span.hover_active", has_text=app13[:10]).first
            if link.count() == 0:
                link = page.get_by_text(app13, exact=False).first
            if link.count() == 0:
                return False
            link.click()
            page.wait_for_load_state("networkidle", timeout=STEP_WAIT_MS)
            return True
        except Exception:
            return False

    def _page_fetch_list(self, page, api: str, app13: str, cancel, extra=None):
        """One in-page fetch of a list API; None on intercept/empty/failure."""
        if cancel is not None and cancel.is_set():
            return None
        if api == API_FILE_INFOS:
            body = {"zhuanlisqh": app13, "anjianbh": ""}
            body.update(extra or {})
        else:
            body = {"zhuanlisqh": app13, "anjianbh": "",
                    "nodeId": "aj_gk_scxx_" + ("tzs" if api == API_TZS else "zjwj")}
        try:
            result = page.evaluate(
                _PAGE_FETCH_JS,
                {"path": api, "body": body})
        except Exception:
            return None
        body = (result or {}).get("text", "")
        if looks_like_intercept(body):
            return None
        return body

    def download_document(self, document: ProsecutionDocument, dest_path: str,
                          cancel):
        """Downloads one dossier document into dest_path (a directory).

        Metadata sync never depends on this. Returns (ResultCode, info dict)
        where info carries saved_path / page_count / message for the caller.
        """
        info = {"saved_path": "", "page_count": 0, "message": ""}
        app13 = api_application_number(document.application_number)
        rid = document.remote_document_id
        if not app13 or not rid:
            return ResultCode.RESOLVE_FAILED, {**info, "message": "缺少申请号或文档 rid"}

        if self._fixture_dir.name:
            # Fixtures carry no binary payloads; live-only by design.
            return ResultCode.UNSUPPORTED_JURISDICTION, {**info, "message": "fixture 模式不提供下载"}

        import base64
        from pathlib import Path as _Path
        ds = document.ds or "TZS"
        wenjiandm = document.wenjiandm or "100000"
        with self._lock:
            self._pace()
            page = self._manager.open_page(BASE_URL, cancel)
            if page is None:
                return ResultCode.NETWORK_ERROR, {**info, "message": "站点打开失败"}
            try:
                body = None
                for attempt in (1, 2):   # same transient-intercept discipline
                    body = self._page_fetch_list(page, API_FILE_INFOS, app13, cancel,
                                                 extra={"rid": rid, "ds": ds,
                                                        "wenjiandm": wenjiandm})
                    if body is not None:
                        break
                    if attempt == 1:
                        time.sleep(LIST_RETRY_PAUSE_SECONDS)
                if body is None:
                    return ResultCode.RATE_LIMITED, {**info, "message": "页清单请求被拦截"}
                pages, err = parse_file_infos(body)
                if pages is None:
                    return ResultCode.PAGE_STRUCTURE_CHANGED, {**info, "message": err}

                blobs = []
                for p in pages:
                    if cancel is not None and cancel.is_set():
                        return ResultCode.TEMPORARY_ERROR, {**info, "message": "已取消"}
                    url = BASE_URL + build_fetch_file_url(p)
                    try:
                        result = page.evaluate(_PAGE_BINARY_JS, {"url": url})
                        data = base64.b64decode((result or {}).get("b64", "") or "")
                    except Exception:
                        data = b""
                    if not data:
                        return ResultCode.TEMPORARY_ERROR, \
                            {**info, "message": "文档页下载失败（瞬时），可重试"}
                    blobs.append(data)
            except Exception as exc:   # noqa: BLE001
                return ResultCode.TEMPORARY_ERROR, \
                    {**info, "message": f"下载交互失败: {type(exc).__name__}"}

        dest = _Path(dest_path)
        dest.mkdir(parents=True, exist_ok=True)
        fmt = sniff_format(blobs[0])
        if fmt == "pdf":
            name = build_document_filename(
                document.official_date, ds, document.document_title, ".pdf",
                lambda n: (dest / n).exists(), pages=len(blobs))
            out = dest / name
            out.write_bytes(blobs[0] if len(blobs) == 1 else b"".join(blobs))
            return ResultCode.OK, {**info, "saved_path": str(out),
                                   "page_count": len(blobs), "message": ""}
        if fmt == "png":
            try:
                import img2pdf
                name = build_document_filename(
                    document.official_date, ds, document.document_title, ".pdf",
                    lambda n: (dest / n).exists(), pages=len(blobs))
                out = dest / name
                out.write_bytes(img2pdf.convert(blobs))
                return ResultCode.OK, {**info, "saved_path": str(out),
                                       "page_count": len(blobs), "message": ""}
            except ImportError:
                # Without img2pdf keep the PNG pages rather than failing the
                # whole download; the caller reports what was saved.
                saved = []
                for i, blob in enumerate(blobs, 1):
                    name = build_document_filename(
                        document.official_date, ds,
                        f"{document.document_title}_{i:02d}", ".png",
                        lambda n: (dest / n).exists())
                    out = dest / name
                    out.write_bytes(blob)
                    saved.append(out.name)
                return ResultCode.OK, {**info, "saved_path": str(dest / saved[0]),
                                       "page_count": len(blobs),
                                       "message": "img2pdf 未安装，PNG 分页保存"}
        return ResultCode.PAGE_STRUCTURE_CHANGED, \
            {**info, "message": f"未知文件格式（魔数不匹配），共 {len(blobs)} 页"}
