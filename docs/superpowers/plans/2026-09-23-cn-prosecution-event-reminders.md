# CN 专利审查事件后台提醒实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 让 patX 针对已导入的 CN 申请，按 USPTO Global Dossier → EPO Global Dossier → CNIPA 的顺序后台检查官方发文元数据，自动建立去重的 OA/提醒记录，并区分第几次审查意见、驳回、授权、补正和其他官方通知。

**架构：** Python sidecar 负责号码规范化、网页获取、`ORIGINAL` 过滤、事件分类和数据源降级；C++ 核心负责跨来源去重、OA 安全合并、查询节奏与同步状态；wxWidgets 控制器负责手动进度、静默后台任务、设置和桌面通知。保留 CNIPA 的持久化浏览器 Profile，但 USPTO/EPO 公共数据源不要求用户配置 API Key。

**技术栈：** Python 3.10+、Playwright、`urllib.request`、`html.parser`、C++17、wxWidgets、SQLite、nlohmann/json、CMake/CTest、pytest。

---

## 文件结构

### 新建文件

- `tools/web_dossier/providers/uspto_global_dossier.py`：USPTO Global Dossier 公共页面 Provider 与纯 HTML 解析器。
- `tools/web_dossier/providers/epo_global_dossier.py`：EPO Global Dossier 公共页面 Provider 与纯 HTML 解析器。
- `tools/web_dossier/providers/chain.py`：按固定顺序执行 Provider，并记录实际来源和失败轨迹。
- `tools/web_dossier/fixtures/uspto_global_dossier/cn202510469601_documents.html`：已脱敏的 USPTO 文档列表样本。
- `tools/web_dossier/fixtures/epo_global_dossier/cn202510469601_documents.html`：EPO 文档列表样本，包含 `ORIGINAL` 与 `TRANSLATED`。
- `tools/web_dossier/tests/test_global_dossier_classifier.py`：英文标题、文档代码和官方事件筛选测试。
- `tools/web_dossier/tests/test_uspto_global_dossier.py`：USPTO 页面解析与异常响应测试。
- `tools/web_dossier/tests/test_epo_global_dossier.py`：EPO 页面解析与异常响应测试。
- `tools/web_dossier/tests/test_provider_chain.py`：三层降级、冲突和 CNIPA 登录态测试。
- `tools/web_dossier/tests/test_service_sync.py`：sidecar JSON 协议测试。

### 修改文件

- `tools/web_dossier/models.py`：增加文档代码、版本、事件指纹、来源轨迹和 `latest_event`。
- `tools/web_dossier/document_classifier.py`：统一处理中英文标题、稳定文档代码和事件中文显示名。
- `tools/web_dossier/providers/base.py`：让 `SyncOutcome` 输出最新官方事件和 Provider 尝试轨迹。
- `tools/web_dossier/providers/cnipa.py`：将 CNIPA 结果适配统一官方事件模型，不改变登录安全边界。
- `tools/web_dossier/browser/manager.py`：支持公共 Provider 使用无登录的 headless 上下文，CNIPA 仍保持可见持久化 Profile。
- `tools/web_dossier/service.py`：`sync_case` 改由 Provider 链执行；保留显式 `login` 仅供 CNIPA 使用。
- `include/patx/schema_migrations.hpp`：数据库版本从 v4 升至 v5。
- `src/cpp/database/schema_migrations.cpp`：新增跨来源事件键、来源轨迹、原始标题和文档代码列。
- `include/database.hpp`、`src/cpp/database.cpp`：扩展审查事件模型、跨来源 upsert 和到期队列查询。
- `include/web_dossier.hpp`、`src/cpp/web_dossier_rules.cpp`：增加官方事件中文名与可单测的安全合并规则。
- `src/cpp/web_dossier.cpp`：解析 `latest_event`、写入真实来源、停止下载正文、处理所有官方事件。
- `src/cpp/ui/web_dossier_dialogs.hpp`、`src/cpp/ui/web_dossier_dialogs.cpp`：支持静默后台批次、设置、通知和登录提示。
- `src/cpp/main_gui.cpp`：启动后 30 秒首次检查、每 30 分钟调度，以及设置菜单。
- `tests/test_database.cpp`：v4 → v5 迁移和到期队列测试。
- `tests/test_web_dossier_rules.cpp`：官方事件合并、跨来源去重和人工字段保护测试。
- `README.md`、`tools/web_dossier/README.md`：更新数据源、配置、隐私和故障处理说明。

## 任务 1：统一官方事件模型与分类

**文件：**
- 修改：`tools/web_dossier/models.py:15-186`
- 修改：`tools/web_dossier/document_classifier.py:13-106`
- 创建：`tools/web_dossier/tests/test_global_dossier_classifier.py`

- [ ] **步骤 1：编写失败的分类与指纹测试**

```python
from web_dossier.document_classifier import classify_official_document, event_title_cn
from web_dossier.models import Confidence, DocumentType, ProsecutionDocument


def test_global_dossier_titles_and_codes():
    assert classify_official_document(
        "First notice of examination opinions (ORIGINAL)", "210401-CN"
    ) == (DocumentType.OFFICE_ACTION_FIRST, 1, Confidence.HIGH)
    assert classify_official_document(
        "Second notice of examination opinions (ORIGINAL)", ""
    )[:2] == (DocumentType.OFFICE_ACTION_SECOND, 2)
    assert classify_official_document("Decision to reject (ORIGINAL)", "")[:2] == (
        DocumentType.REJECTION_DECISION, 0
    )
    assert classify_official_document("Notification to grant patent right (ORIGINAL)", "")[:2] == (
        DocumentType.GRANT_NOTICE, 0
    )
    assert classify_official_document("Notification to make rectification (ORIGINAL)", "")[:2] == (
        DocumentType.CORRECTION_NOTICE, 0
    )


def test_final_is_not_an_oa_ordinal():
    doc_type, ordinal, confidence = classify_official_document(
        "Final rejection decision (ORIGINAL)", ""
    )
    assert (doc_type, ordinal, confidence) == (
        DocumentType.REJECTION_DECISION, 0, Confidence.HIGH
    )


def test_cross_provider_event_key_ignores_provider_specific_id():
    uspto = ProsecutionDocument(
        source="uspto_global_dossier", application_number="CN202510469601.5",
        document_type=DocumentType.OFFICE_ACTION_FIRST,
        document_title="第一次审查意见通知书", official_date="2026-05-23",
        remote_document_id="uspto-123",
    )
    epo = ProsecutionDocument(
        source="epo_global_dossier", application_number="CN202510469601.5",
        document_type=DocumentType.OFFICE_ACTION_FIRST,
        document_title="第一次审查意见通知书", official_date="2026-05-23",
        remote_document_id="epo-456",
    )
    assert uspto.event_key() == epo.event_key()
    assert event_title_cn(DocumentType.REJECTION_DECISION, 0, "") == "驳回决定"
```

- [ ] **步骤 2：运行测试，确认新接口尚不存在**

运行：

```bash
cd tools
/Users/jiajia/Documents/test/.patx-cn-reminders-venv/bin/python -m pytest \
  web_dossier/tests/test_global_dossier_classifier.py -q
```

预期：FAIL，提示无法导入 `classify_official_document` 或缺少 `event_key`。

- [ ] **步骤 3：实现统一字段、分类入口和事件中文名**

在 `ProsecutionDocument` 中增加以下字段，并让 `to_dict()` 原样输出：

```python
document_code: str = ""
document_version: str = "ORIGINAL"
source_trace: list[str] = field(default_factory=list)

def event_key(self) -> str:
    basis = "|".join([
        self.jurisdiction,
        self.application_number,
        self.document_type.value,
        str(self.oa_ordinal),
        self.official_date,
        normalize_title(self.document_title),
    ])
    return hashlib.sha256(basis.encode("utf-8")).hexdigest()
```

同时在 Python `ResultCode` 中新增 `NEW_OFFICIAL_EVENT`；保留
`NEW_OFFICE_ACTION` 仅用于读取旧 sidecar 响应，不再生成旧值。

在 `document_classifier.py` 中保留 `classify_cn_title()` 兼容入口，并新增：

```python
_CODE_TYPES = {
    "210401-CN": (DocumentType.OFFICE_ACTION_FIRST, 1),
}

def classify_official_document(raw_title: str, document_code: str = ""):
    code = (document_code or "").strip().upper()
    if code in _CODE_TYPES:
        doc_type, ordinal = _CODE_TYPES[code]
        return doc_type, ordinal, Confidence.HIGH

    compact = re.sub(r"\s+", " ", (raw_title or "").strip()).lower()
    if re.search(r"final rejection|decision to reject", compact):
        return DocumentType.REJECTION_DECISION, 0, Confidence.HIGH
    ordinal_words = {"first": 1, "second": 2, "third": 3, "fourth": 4,
                     "fifth": 5, "sixth": 6, "seventh": 7, "eighth": 8,
                     "ninth": 9, "tenth": 10}
    for word, ordinal in ordinal_words.items():
        if re.search(rf"\b{word}\b.*(?:examination opinions|office action)", compact):
            doc_type = (DocumentType.OFFICE_ACTION_FIRST if ordinal == 1 else
                        DocumentType.OFFICE_ACTION_SECOND if ordinal == 2 else
                        DocumentType.OFFICE_ACTION_NTH)
            return doc_type, ordinal, Confidence.HIGH
    if re.search(r"grant patent right|notification to grant|registration formalities", compact):
        return DocumentType.GRANT_NOTICE, 0, Confidence.HIGH
    if re.search(r"rectification|correction notice", compact):
        return DocumentType.CORRECTION_NOTICE, 0, Confidence.HIGH
    return classify_cn_title(raw_title)

def event_title_cn(document_type: DocumentType, ordinal: int, fallback_title: str) -> str:
    if document_type.is_office_action:
        return oa_title_cn(document_type, ordinal)
    return {
        DocumentType.REJECTION_DECISION: "驳回决定",
        DocumentType.GRANT_NOTICE: "授权通知",
        DocumentType.CORRECTION_NOTICE: "补正通知",
    }.get(document_type, fallback_title or "其他官方通知")
```

代码与标题得到不同分类时，返回 `UNKNOWN/LOW`，让调用方生成 `MANUAL_REVIEW_REQUIRED`，不得按任一方猜测。

- [ ] **步骤 4：运行 Python 分类测试与原有分类回归测试**

运行：

```bash
cd tools
/Users/jiajia/Documents/test/.patx-cn-reminders-venv/bin/python -m pytest \
  web_dossier/tests/test_global_dossier_classifier.py \
  web_dossier/tests/test_document_classifier.py -q
```

预期：全部 PASS。

- [ ] **步骤 5：提交统一事件模型**

```bash
git add tools/web_dossier/models.py tools/web_dossier/document_classifier.py \
  tools/web_dossier/tests/test_global_dossier_classifier.py
git commit -m "feat(审查提醒): 统一 CN 官方事件分类模型"
```

## 任务 2：实现 USPTO Global Dossier Provider

**文件：**
- 创建：`tools/web_dossier/providers/uspto_global_dossier.py`
- 创建：`tools/web_dossier/fixtures/uspto_global_dossier/cn202510469601_documents.html`
- 创建：`tools/web_dossier/tests/test_uspto_global_dossier.py`
- 修改：`tools/web_dossier/browser/manager.py:36-201`

- [ ] **步骤 1：保存已验证页面的最小 fixture，并编写失败的纯解析测试**

fixture 至少包含 2026-05-23 的 `First notice of examination opinions (ORIGINAL)`、对应 `TRANSLATED` 行、申请人提交文件和稳定文档标识。测试固定为：

```python
from pathlib import Path
from web_dossier.models import DocumentType, ResultCode
from web_dossier.providers.uspto_global_dossier import parse_uspto_document_list

FIXTURE = Path(__file__).parents[1] / "fixtures/uspto_global_dossier/cn202510469601_documents.html"


def test_parse_uspto_original_official_events_only():
    outcome = parse_uspto_document_list(
        FIXTURE.read_text(encoding="utf-8"), "CN202510469601.5", "CN120134203A"
    )
    assert outcome.code == ResultCode.OK
    assert [(d.document_type, d.official_date) for d in outcome.documents] == [
        (DocumentType.OFFICE_ACTION_FIRST, "2026-05-23")
    ]
    assert outcome.documents[0].document_version == "ORIGINAL"
    assert outcome.documents[0].source == "uspto_global_dossier"


def test_uspto_intercept_is_not_no_change():
    outcome = parse_uspto_document_list("<html>Access Denied</html>", "CN202510469601.5", "")
    assert outcome.code == ResultCode.ACCESS_DENIED
```

- [ ] **步骤 2：运行测试，确认 Provider 尚不存在**

运行：

```bash
cd tools
/Users/jiajia/Documents/test/.patx-cn-reminders-venv/bin/python -m pytest \
  web_dossier/tests/test_uspto_global_dossier.py -q
```

预期：FAIL，提示无法导入 `uspto_global_dossier`。

- [ ] **步骤 3：实现纯解析器和公共页面 Provider**

Provider 使用无登录、headless 的浏览器上下文访问：

```python
APP_URL = "https://globaldossier.uspto.gov/#/result/application/CN/{application}/0"
PUB_URL = "https://globaldossier.uspto.gov/#/result/publication/CN/{publication}/1"

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

    def list_documents(self, application_number, publication_number, cancel):
        ident = normalize_cn_identifier(application_number)
        if ident.number_type != "application":
            return SyncOutcome(code=ResultCode.RESOLVE_FAILED,
                               message="USPTO Global Dossier 需要可识别的 CN 申请号")
        digits = ident.number
        url = APP_URL.format(application=digits)
        page = self._manager.open_page(url, cancel)
        if page is None:
            return SyncOutcome(code=ResultCode.NETWORK_ERROR,
                               message="USPTO Global Dossier 无法访问")
        page.wait_for_timeout(1500)
        return parse_uspto_document_list(page.content(), ident.normalized_number,
                                         publication_number)

    def download_document(self, document, dest_path, cancel):
        return ResultCode.UNSUPPORTED_JURISDICTION
```

解析器必须同时验证页面身份和表头；只有确认页面表示“无文档”时才返回空的 `OK`。HTML 空白、拦截页、登录页和结构缺失分别返回 `TEMPORARY_ERROR`、`ACCESS_DENIED` 或 `PAGE_STRUCTURE_CHANGED`。`BrowserManager` 增加 `prefer_system_browser` 参数，使公共 Provider 使用 Playwright headless 浏览器，CNIPA 继续使用真实 Chrome + 持久化 Profile。

- [ ] **步骤 4：运行 USPTO Provider 测试与全量 Python 测试**

运行：

```bash
cd tools
/Users/jiajia/Documents/test/.patx-cn-reminders-venv/bin/python -m pytest \
  web_dossier/tests/test_uspto_global_dossier.py -q
/Users/jiajia/Documents/test/.patx-cn-reminders-venv/bin/python -m pytest web_dossier/tests -q
```

预期：全部 PASS，且测试不访问网络。

- [ ] **步骤 5：提交 USPTO Provider**

```bash
git add tools/web_dossier/browser/manager.py \
  tools/web_dossier/providers/uspto_global_dossier.py \
  tools/web_dossier/fixtures/uspto_global_dossier \
  tools/web_dossier/tests/test_uspto_global_dossier.py
git commit -m "feat(审查提醒): 接入 USPTO Global Dossier 元数据"
```

## 任务 3：实现 EPO Global Dossier Provider 与三层降级链

**文件：**
- 创建：`tools/web_dossier/providers/epo_global_dossier.py`
- 创建：`tools/web_dossier/providers/chain.py`
- 创建：`tools/web_dossier/fixtures/epo_global_dossier/cn202510469601_documents.html`
- 创建：`tools/web_dossier/tests/test_epo_global_dossier.py`
- 创建：`tools/web_dossier/tests/test_provider_chain.py`
- 修改：`tools/web_dossier/providers/cnipa.py`

- [ ] **步骤 1：编写 EPO 解析和 Provider 降级的失败测试**

```python
def test_epo_keeps_original_and_drops_translation():
    outcome = parse_epo_document_list(FIXTURE.read_text(encoding="utf-8"),
                                      "CN202510469601.5", "CN120134203A")
    assert [(d.document_title, d.official_date) for d in outcome.documents] == [
        ("第一次审查意见通知书", "2026-05-23")
    ]
    assert outcome.documents[0].remote_document_id == (
        "20251046960152104012026052310110680664683123_CN"
    )


def test_chain_falls_back_uspto_then_epo_without_cnipa_login():
    chain = ProviderChain([
        FakeProvider("uspto_global_dossier", ResultCode.NETWORK_ERROR),
        FakeProvider("epo_global_dossier", ResultCode.OK, [FIRST_OA]),
        FakeProvider("cnipa", ResultCode.AUTH_REQUIRED),
    ])
    outcome = chain.list_documents("CN202510469601.5", "CN120134203A", CancelFalse())
    assert outcome.code == ResultCode.OK
    assert outcome.provider_used == "epo_global_dossier"
    assert [a.provider for a in outcome.attempts] == [
        "uspto_global_dossier", "epo_global_dossier"
    ]


def test_chain_reaches_cnipa_and_surfaces_login_requirement():
    chain = ProviderChain([
        FakeProvider("uspto_global_dossier", ResultCode.ACCESS_DENIED),
        FakeProvider("epo_global_dossier", ResultCode.NETWORK_ERROR),
        FakeProvider("cnipa", ResultCode.AUTH_REQUIRED),
    ])
    outcome = chain.list_documents("CN202510469601.5", "", CancelFalse())
    assert outcome.code == ResultCode.AUTH_REQUIRED
    assert outcome.provider_used == "cnipa"
```

- [ ] **步骤 2：运行测试，确认 EPO Provider 和链尚不存在**

运行：

```bash
cd tools
/Users/jiajia/Documents/test/.patx-cn-reminders-venv/bin/python -m pytest \
  web_dossier/tests/test_epo_global_dossier.py \
  web_dossier/tests/test_provider_chain.py -q
```

预期：FAIL，提示缺少对应模块。

- [ ] **步骤 3：实现 EPO HTML 解析和固定顺序降级**

EPO 使用已验证的公开入口，不使用 OPS Key：

```python
EPO_DOSSIER_URL = "https://register.epo.org/ipfwretrieve?apn=CN.{application}.A&lng=en"

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

    def list_documents(self, application_number, publication_number, cancel):
        ident = normalize_cn_identifier(application_number)
        if ident.number_type != "application":
            return SyncOutcome(code=ResultCode.RESOLVE_FAILED,
                               message="EPO Global Dossier 需要可识别的 CN 申请号")
        status, html = self._fetch_html(
            EPO_DOSSIER_URL.format(application=ident.number)
        )
        if status == 429:
            return SyncOutcome(code=ResultCode.RATE_LIMITED, message="EPO 限流")
        if status != 200:
            return SyncOutcome(code=ResultCode.NETWORK_ERROR,
                               message=f"EPO Global Dossier HTTP {status}")
        return parse_epo_document_list(html, ident.normalized_number, publication_number)

    def download_document(self, document, dest_path, cancel):
        return ResultCode.UNSUPPORTED_JURISDICTION
```

`ProviderChain` 只对 `NETWORK_ERROR`、`TEMPORARY_ERROR`、`RATE_LIMITED`、`ACCESS_DENIED`、`PAGE_STRUCTURE_CHANGED`、`CASE_NOT_FOUND` 和 `RESOLVE_FAILED` 降级。任一 Provider 成功验证页面结构后，即使官方事件列表为空，也停止降级。CNIPA 返回 `AUTH_REQUIRED/SESSION_EXPIRED` 时直接返回，后台不得调用 `ensure_login()`。

Provider 尝试轨迹使用以下稳定结构，供 sidecar 和同步历史审计：

```python
@dataclass
class ProviderAttempt:
    provider: str
    code: ResultCode
    message: str = ""

    def to_dict(self) -> dict:
        return {"provider": self.provider, "code": self.code.value,
                "message": self.message}

class ProviderChain:
    def __init__(self, providers):
        self.providers = list(providers)

    def list_documents(self, application_number, publication_number, cancel):
        attempts = []
        for provider in self.providers:
            outcome = provider.list_documents(application_number, publication_number, cancel)
            attempts.append(ProviderAttempt(provider.provider_id, outcome.code,
                                            outcome.message))
            outcome.provider_used = provider.provider_id
            outcome.attempts = list(attempts)
            if outcome.ok or outcome.code in (ResultCode.AUTH_REQUIRED,
                                               ResultCode.SESSION_EXPIRED):
                return outcome
        return outcome
```

CNIPA 生成的 `ProsecutionDocument` 明确设置 `source="cnipa"`、`document_version="ORIGINAL"` 和 `document_code=wenjiandm`，以便统一持久化。

- [ ] **步骤 4：运行 EPO、降级链和 CNIPA 回归测试**

运行：

```bash
cd tools
/Users/jiajia/Documents/test/.patx-cn-reminders-venv/bin/python -m pytest \
  web_dossier/tests/test_epo_global_dossier.py \
  web_dossier/tests/test_provider_chain.py \
  web_dossier/tests/test_cnipa_parser.py \
  web_dossier/tests/test_cpquery_api_parser.py -q
```

预期：全部 PASS。

- [ ] **步骤 5：提交 EPO Provider 和降级链**

```bash
git add tools/web_dossier/providers/epo_global_dossier.py \
  tools/web_dossier/providers/chain.py tools/web_dossier/providers/cnipa.py \
  tools/web_dossier/fixtures/epo_global_dossier \
  tools/web_dossier/tests/test_epo_global_dossier.py \
  tools/web_dossier/tests/test_provider_chain.py
git commit -m "feat(审查提醒): 添加 EPO 与 CNIPA 降级查询"
```

## 任务 4：升级 sidecar 协议为“最新官方事件”

**文件：**
- 修改：`tools/web_dossier/providers/base.py:15-37`
- 修改：`tools/web_dossier/models.py:176-186`
- 修改：`tools/web_dossier/service.py:37-149`
- 创建：`tools/web_dossier/tests/test_service_sync.py`

- [ ] **步骤 1：编写失败的 sidecar 响应测试**

```python
def test_sync_response_reports_provider_and_latest_official_event(monkeypatch):
    monkeypatch.setattr(service, "_get_chain", lambda: FakeSuccessfulChain())
    response = service._op_sync_case({
        "application_number": "CN202510469601.5",
        "publication_number": "CN120134203A",
    }, CancelFalse())
    assert response["ok"] is True
    assert response["provider_used"] == "uspto_global_dossier"
    assert response["latest_event"]["document_type"] == "OFFICE_ACTION_FIRST"
    assert response["latest_event"]["official_date"] == "2026-05-23"
    assert response["latest_oa"] is None


def test_background_sync_never_opens_cnipa_login(monkeypatch):
    chain = FakeAuthRequiredChain()
    monkeypatch.setattr(service, "_get_chain", lambda: chain)
    response = service._op_sync_case({"application_number": "CN202510469601.5"},
                                     CancelFalse())
    assert response["code"] == "AUTH_REQUIRED"
    assert chain.ensure_login_called is False
```

- [ ] **步骤 2：运行测试，确认旧协议只有 `latest_oa`**

运行：

```bash
cd tools
/Users/jiajia/Documents/test/.patx-cn-reminders-venv/bin/python -m pytest \
  web_dossier/tests/test_service_sync.py -q
```

预期：FAIL，缺少 `provider_used` 或 `latest_event`。

- [ ] **步骤 3：实现新响应，同时保留兼容字段**

`SyncOutcome` 增加 `provider_used`、`attempts`，并将序列化改为：

```python
def to_dict(self, latest_event=None):
    return {
        "ok": self.ok,
        "code": self.code.value,
        "message": self.message,
        "auth_state": self.auth_state,
        "provider_used": self.provider_used,
        "attempts": [attempt.to_dict() for attempt in self.attempts],
        "resolved_application_number": self.resolved_application_number,
        "documents": [doc.to_dict() for doc in self.documents],
        "latest_event": latest_event.to_dict() if latest_event else None,
        "latest_oa": None,
}
```

`service.py` 延迟构造并缓存整条 Provider 链，构造 Provider 本身不得打开浏览器：

```python
_CHAIN = None

def _get_chain():
    global _CHAIN
    if _CHAIN is None:
        from web_dossier.browser.manager import BrowserManager
        from web_dossier.providers.chain import ProviderChain
        from web_dossier.providers.epo_global_dossier import EpoGlobalDossierProvider
        from web_dossier.providers.uspto_global_dossier import UsptoGlobalDossierProvider
        uspto = UsptoGlobalDossierProvider(
            BrowserManager("uspto_global_dossier", headless=True,
                           prefer_system_browser=False)
        )
        epo = EpoGlobalDossierProvider()
        cnipa = CNIPAWebProvider(browser_manager=BrowserManager("cnipa"))
        _CHAIN = ProviderChain([uspto, epo, cnipa])
    return _CHAIN
```

因此 `EpoGlobalDossierProvider.__init__()` 的 `fetch_html` 参数默认使用模块内
`urllib.request` 实现；测试再注入 fixture fetcher。

`pick_latest_official_event()` 只接受 `direction == "official"`、`document_version == "ORIGINAL"`、有合法日期且类型属于 OA/驳回/授权/补正/其他官方的文档；按 `(official_date, oa_ordinal, remote_document_id)` 选最新。`_op_sync_case()` 调用 `_get_chain()`，不再预先检查 CNIPA 登录，只有显式 `_op_login()` 才创建可见 CNIPA 浏览器。

筛选函数使用明确白名单：

```python
_REMINDABLE_TYPES = {
    DocumentType.OFFICE_ACTION_FIRST,
    DocumentType.OFFICE_ACTION_SECOND,
    DocumentType.OFFICE_ACTION_NTH,
    DocumentType.REJECTION_DECISION,
    DocumentType.GRANT_NOTICE,
    DocumentType.CORRECTION_NOTICE,
    DocumentType.OTHER_OFFICIAL,
}

def pick_latest_official_event(documents):
    candidates = [
        doc for doc in documents
        if doc.direction == "official"
        and doc.document_version == "ORIGINAL"
        and doc.document_type in _REMINDABLE_TYPES
        and doc.official_date
    ]
    return max(candidates,
               key=lambda doc: (doc.official_date, doc.oa_ordinal,
                                doc.remote_document_id),
               default=None)
```

- [ ] **步骤 4：运行 sidecar 测试和完整 Python 测试**

运行：

```bash
cd tools
/Users/jiajia/Documents/test/.patx-cn-reminders-venv/bin/python -m pytest \
  web_dossier/tests/test_service_sync.py -q
/Users/jiajia/Documents/test/.patx-cn-reminders-venv/bin/python -m pytest web_dossier/tests -q
```

预期：全部 PASS。

- [ ] **步骤 5：提交 sidecar 协议升级**

```bash
git add tools/web_dossier/models.py tools/web_dossier/providers/base.py \
  tools/web_dossier/service.py tools/web_dossier/tests/test_service_sync.py
git commit -m "feat(审查提醒): 输出最新官方事件与实际数据源"
```

## 任务 5：升级数据库并实现跨来源事件去重

**文件：**
- 修改：`include/patx/schema_migrations.hpp:18-38`
- 修改：`src/cpp/database/schema_migrations.cpp:425-609`
- 修改：`include/database.hpp:207-348`
- 修改：`src/cpp/database.cpp:1541-1716`
- 修改：`tests/test_database.cpp`
- 修改：`tests/test_web_dossier_rules.cpp:71-169`

- [ ] **步骤 1：编写失败的 v5 迁移、到期队列和跨来源 upsert 测试**

测试必须验证以下断言：

```cpp
CHECK_EQ(db.SchemaVersion(), 5);
CHECK(HasColumn(db.GetHandle(), "prosecution_documents", "event_key"));
CHECK(HasColumn(db.GetHandle(), "prosecution_documents", "source_trace"));

auto due = db.GetPatentsDueForDossierCheck(false, 1'000, 0);
CHECK(due.size() == 1);
CHECK_STR_EQ(due[0].geke_code, "DUE-CN");

ProsecutionDocumentRecord uspto;
uspto.patent_id = 1;
uspto.application_number = "CN202510469601.5";
uspto.source = "uspto_global_dossier";
uspto.event_key = "same-event";
uspto.source_trace = "uspto_global_dossier";
bool created = false;
int id1 = db.UpsertProsecutionDocument(uspto, &created);
CHECK(created);

ProsecutionDocumentRecord epo = uspto;
epo.source = "epo_global_dossier";
epo.source_trace = "epo_global_dossier";
int id2 = db.UpsertProsecutionDocument(epo, &created);
CHECK(id2 == id1);
CHECK(!created);
CHECK(db.GetProsecutionDocumentById(id1).source_trace.find("epo_global_dossier") !=
      std::string::npos);
```

- [ ] **步骤 2：运行 C++ 测试，确认 schema 和 API 尚未升级**

运行：

```bash
cmake --build build -j8
ctest --test-dir build -R 'patx_tests|patx_web_dossier_tests' --output-on-failure
```

预期：编译失败，提示缺少 `GetPatentsDueForDossierCheck`、`event_key` 等成员。

- [ ] **步骤 3：实现 v4 → v5 增量迁移和数据库 API**

迁移只允许 `ALTER TABLE ... ADD COLUMN` 和新索引：

```sql
ALTER TABLE prosecution_documents ADD COLUMN raw_title TEXT DEFAULT '';
ALTER TABLE prosecution_documents ADD COLUMN document_code TEXT DEFAULT '';
ALTER TABLE prosecution_documents ADD COLUMN document_version TEXT DEFAULT 'ORIGINAL';
ALTER TABLE prosecution_documents ADD COLUMN event_key TEXT DEFAULT '';
ALTER TABLE prosecution_documents ADD COLUMN source_trace TEXT DEFAULT '';
CREATE UNIQUE INDEX IF NOT EXISTS idx_prosecution_docs_event_key
ON prosecution_documents(patent_id, event_key)
WHERE event_key <> '';
```

`GetPatentsDueForDossierCheck(include_granted, now, limit)` 复用原状态排除条件，并追加：

```sql
AND (next_dossier_check_at IS NULL OR next_dossier_check_at = 0
     OR next_dossier_check_at <= ?)
AND (application_number LIKE 'CN%' OR application_number GLOB '[0-9]*'
     OR publication_number LIKE 'CN%')
```

`UpsertProsecutionDocument()` 在 `event_key` 非空时先按 `(patent_id, event_key)` 查询；命中后只刷新 `last_seen_at/raw_metadata`，并把新来源追加到逗号分隔的 `source_trace`，不得创建第二行。旧数据的 `event_key` 为空时继续使用原 `(source, application_number, fingerprint)` 规则。

新增 `GetProsecutionDocumentById(int id)`，完整读取 v5 字段，专供同步历史展示和单元测试使用；不存在时返回 `id == 0` 的空记录。

- [ ] **步骤 4：运行数据库与迁移测试**

运行：

```bash
cmake --build build -j8
ctest --test-dir build -R 'patx_tests|patx_web_dossier_tests' --output-on-failure
```

预期：`2/2` 测试通过。

- [ ] **步骤 5：提交数据库升级**

```bash
git add include/patx/schema_migrations.hpp src/cpp/database/schema_migrations.cpp \
  include/database.hpp src/cpp/database.cpp tests/test_database.cpp \
  tests/test_web_dossier_rules.cpp
git commit -m "feat(审查提醒): 增加跨来源事件去重与到期队列"
```

## 任务 6：实现 C++ 官方事件合并并停止下载正文

**文件：**
- 修改：`include/web_dossier.hpp:28-170`
- 修改：`src/cpp/web_dossier_rules.cpp:8-158`
- 修改：`src/cpp/web_dossier.cpp:197-522`
- 修改：`tests/test_web_dossier_rules.cpp`

- [ ] **步骤 1：编写失败的事件合并测试**

新增测试覆盖：第一次/第二次/Nth OA、驳回、授权、补正、其他官方通知；相同事件不重复；空日期补入；日期冲突不覆盖；授权/驳回不改 `Patent.application_status`；不产生下载路径。

```cpp
RemoteDocument grant;
grant.source = "epo_global_dossier";
grant.document_type = "GRANT_NOTICE";
grant.document_title = "授权通知";
grant.raw_title = "Notification to grant patent right (ORIGINAL)";
grant.official_date = "2026-08-10";
grant.remote_document_id = "epo-grant-1";
grant.event_key = "grant-event";
grant.confidence = "HIGH";

auto merged = MergeOfficialEvent(db, patent, grant);
CHECK(merged.code == ResultCode::NewOfficialEvent);
CHECK_STR_EQ(db.GetOAsForPatentId(patent.id).back().oa_type, "授权通知");
CHECK_STR_EQ(db.GetPatentById(patent.id).application_status, "实质审查中");
```

- [ ] **步骤 2：运行测试，确认通用事件合并函数尚不存在**

运行：

```bash
cmake --build build -j8
ctest --test-dir build -R patx_web_dossier_tests --output-on-failure
```

预期：编译失败，缺少 `NewOfficialEvent` 或 `MergeOfficialEvent`。

- [ ] **步骤 3：实现可单测合并函数，并让 Manager 使用它**

头文件定义保持一致：

```cpp
struct EventMergeResult {
    ResultCode code = ResultCode::NoChange;
    int oa_created_id = 0;
    bool date_conflict = false;
    std::string canonical_title;
    std::string message;
};

std::string OfficialEventTitleCn(const RemoteDocument& document);
EventMergeResult MergeOfficialEvent(Database& db, const Patent& patent,
                                    const RemoteDocument& document);
```

`ParseCaseResult()` 读取 `provider_used`、每个文档的 `source/document_code/document_version/event_key/source_trace` 和 `latest_event`。`ApplyRemoteResult()` 对所有合格官方事件逐条持久化，但只把 `latest_event` 合并到 `oa_records`；`OARecord.source` 使用真实 Provider，`remote_document_id` 保存来源标识。删除 `download_document` RPC 调用和自动下载分支，不删除旧数据库列及既有本地文件。

在 C++ `ResultCode` 中新增 `NewOfficialEvent` 并映射
`NEW_OFFICIAL_EVENT`；解析旧值 `NEW_OFFICE_ACTION` 时也映射到该枚举。
`BatchSummary.new_oa` 重命名为 `new_events`，所有界面文字统一为“新官方事件”。

成功、无变化、新事件、日期冲突时，下一次查询为 `now + interval_days * 86400`；网络、限流、页面变化时为 `now + 1800`，确保下一轮调度可重试。默认间隔改为 1 天，配置只接受 1、3、7。

- [ ] **步骤 4：运行 C++ 规则、数据库和全量测试**

运行：

```bash
cmake --build build -j8
ctest --test-dir build --output-on-failure
```

预期：`2/2` 测试通过，构建日志中不存在新增编译错误。

- [ ] **步骤 5：提交 C++ 合并规则**

```bash
git add include/web_dossier.hpp src/cpp/web_dossier_rules.cpp \
  src/cpp/web_dossier.cpp tests/test_web_dossier_rules.cpp
git commit -m "feat(审查提醒): 合并全部 CN 官方发文事件"
```

## 任务 7：实现静默后台调度、设置和桌面提醒

**文件：**
- 修改：`src/cpp/ui/web_dossier_dialogs.hpp:23-58`
- 修改：`src/cpp/ui/web_dossier_dialogs.cpp:18-503`
- 修改：`src/cpp/main_gui.cpp:6-245,304-354,2316-2351`

- [ ] **步骤 1：先定义可验证的控制器状态接口并编译失败**

在头文件测试友好的接口中声明：

```cpp
void SyncDueInBackground();
void ShowSettings();
bool background_run() const { return background_run_; }

void StartWorker(const std::vector<Patent>& queue, bool is_login_only,
                 bool show_dialog, bool background_run);
void ShowBatchNotification(const webdossier::BatchSummary& summary);

bool background_run_ = false;
webdossier::BatchSummary last_summary_;
std::unique_ptr<wxNotificationMessage> notification_;
```

运行构建，预期因实现缺失而链接失败：

```bash
cmake --build build -j8
```

- [ ] **步骤 2：实现后台控制器，不显示进度窗口**

`SyncDueInBackground()` 在 `busy()` 时直接跳过；否则调用：

```cpp
auto queue = db_.GetPatentsDueForDossierCheck(false,
    static_cast<long long>(std::time(nullptr)), 0);
if (!queue.empty()) StartWorker(queue, false, false, true);
```

后台批次期间仍通过线程事件累计 `BatchSummary`，但不创建或显示 `WebDossierSyncDialog`。完成后：

- 无变化：不显示任何 UI。
- 新事件：刷新 OA 列表并显示桌面通知。
- 多件新事件：通知显示案件数，并在点击后打开同步结果列表。
- 日期冲突：显示“待人工确认”通知。
- CNIPA 登录失效：显示“需要登录 CNIPA”通知；点击后打开带“登录 CNIPA”按钮的同步窗口，不自动打开浏览器。

通知对象由控制器持有，并绑定：

```cpp
notification_->Bind(wxEVT_NOTIFICATION_MESSAGE_CLICK,
    [this](wxCommandEvent&) {
        if (last_summary_.auth_required > 0) ShowLoginRequired();
        else ShowLastFindings();
    });
```

- [ ] **步骤 3：实现 1/3/7 天设置和主窗口定时器**

`ShowSettings()` 使用 `wxSingleChoiceDialog` 提供“每天、每 3 天、每 7 天”，保存到 `web_dossier_interval_days`。主窗口增加“审查提醒设置”菜单，并在构造函数中：

```cpp
auto_sync_timer_ = new wxTimer(this);
Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
    dossier_controller->SyncDueInBackground();
    if (auto_sync_timer_->IsOneShot()) auto_sync_timer_->Start(30 * 60 * 1000);
}, auto_sync_timer_->GetId());
auto_sync_timer_->StartOnce(30 * 1000);
```

切换数据库后重新绑定 `set_on_finished([this] { LoadOA(); })`，确保后台新事件立即刷新 OA 页。

- [ ] **步骤 4：构建 GUI 并执行手动验收**

运行：

```bash
cmake --build build -j8
ctest --test-dir build --output-on-failure
./build/patx
```

手动验收：

1. 设置为 1 天并重启，确认约 30 秒后出现一次后台查询，且无变化时没有弹窗。
2. 将 fixture 指向新事件，确认 OA 页刷新并只出现一次桌面通知。
3. 再次运行同一 fixture，确认不重复建 OA、不重复通知。
4. 让前两层 Provider 失败且 CNIPA 未登录，确认只出现登录提示；点击提示后才显示“登录 CNIPA”按钮。
5. 完成 CNIPA 登录并重启，确认 Profile 被复用。

- [ ] **步骤 5：提交调度与提醒界面**

```bash
git add src/cpp/ui/web_dossier_dialogs.hpp src/cpp/ui/web_dossier_dialogs.cpp \
  src/cpp/main_gui.cpp
git commit -m "feat(审查提醒): 添加静默调度与桌面通知"
```

## 任务 8：更新文档并完成端到端验证

**文件：**
- 修改：`README.md`
- 修改：`tools/web_dossier/README.md`

- [ ] **步骤 1：更新用户文档**

明确记录：

- 首期仅检查 CN 案件。
- 数据源顺序为 USPTO Global Dossier → EPO Global Dossier → CNIPA。
- USPTO/EPO 公共案卷元数据不使用 USPTO ODP API Key；EPO OPS Key 只用于现有同族/法律状态功能。
- 只处理 `ORIGINAL` 官方发文，不下载正文。
- CNIPA 登录由用户完成，Profile 持久化，密码和 Cookie 不写日志。
- 默认每天检查，可选 1/3/7 天；启动约 30 秒检查，运行中每 30 分钟调度。
- 授权和驳回只建立提醒，不改变专利主档状态。

- [ ] **步骤 2：运行全部离线自动测试**

运行：

```bash
cd tools
/Users/jiajia/Documents/test/.patx-cn-reminders-venv/bin/python -m pytest web_dossier/tests -q
cd ..
cmake --build build -j8
ctest --test-dir build --output-on-failure
git diff --check
```

预期：Python 全部 PASS；CTest `2/2` PASS；`git diff --check` 无输出。

- [ ] **步骤 3：使用验收样例验证真实元数据**

在手动同步中选择 `CN202510469601.5`，预期结果：

```text
数据源：uspto_global_dossier（不可用时为 epo_global_dossier）
事件：第一次审查意见通知书
官方日期：2026-05-23
正文下载：未请求
OA 新增：首次 1 条，重复检查 0 条
```

临时断开 USPTO 数据源后重新检查，确认 EPO 结果命中同一 `event_key`，不创建第二条 OA，并在 `source_trace` 中保留两个来源。

- [ ] **步骤 4：执行安全日志检查**

运行：

```bash
rg -n -i 'cookie|authorization|consumer_secret|api[_ -]?key|password' \
  patx.log patx_sidecar_stderr.log debug/web_dossier 2>/dev/null
```

预期：没有凭据值；允许出现不含值的字段名或安全说明。若调试目录不存在，同样视为通过。

- [ ] **步骤 5：提交文档与最终验证记录**

```bash
git add README.md tools/web_dossier/README.md
git commit -m "docs(审查提醒): 补充数据源与后台提醒说明"
git status --short --branch
```

预期：工作区干净，分支为 `codex/cn-prosecution-reminders`。

## 最终规格覆盖检查

- 数据源优先级：任务 2、3、4。
- CNIPA 持久登录和后台不主动弹登录：任务 3、4、7。
- `ORIGINAL` 与官方发文筛选：任务 1、2、3、4。
- First/Second/Nth、Final/Rejection、Granted、Correction、Other：任务 1、6。
- 自动创建、去重、来源轨迹和人工字段保护：任务 5、6。
- 授权/驳回不改主档状态：任务 6。
- 启动 30 秒、每 30 分钟、1/3/7 天：任务 5、7。
- 无变化静默、新事件桌面通知、登录提示：任务 7。
- `CN202510469601.5` 验收：任务 8。
- 不下载正文、不记录凭据：任务 6、8。
