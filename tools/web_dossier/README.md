# patX Web Dossier Sidecar（审查意见网页同步）

Python sidecar，由 patX C++ GUI 通过 stdin/stdout 行 JSON-RPC 驱动。
按 **USPTO Global Dossier → EPO Patent Register → CNIPA** 的固定顺序查询 CN 案件的
官方发文元数据，发现新官方事件后由 C++ 侧按保护规则写入数据库。

## 数据源与降级链（providers/chain.py）

| 顺序 | Provider | 认证 | 方式 |
|---|---|---|---|
| 1 | `uspto_global_dossier` | 无 | 公开 JSON API（见下），浏览器 UA 的 urllib 直取 |
| 2 | `epo_global_dossier` | 无 | urllib 直取 register.epo.org 公共页面 |
| 3 | `cnipa` | 用户扫码登录 | CDP 接管真实 Chrome，页面内 JSON API |

### 第 1 层实测情报（2026-09 实机验证）

globaldossier.uspto.gov 是 Angular SPA，深链 hash 不会自动渲染，但底层数据服务是
公开 CloudFront JSON（`app-config.json` 的 `externalapiURL`）：

```
GET  https://d1kazzu6rbodne.cloudfront.net/patent-family/svc/family/application/CN/{12位申请号}
POST https://d1kazzu6rbodne.cloudfront.net/doc-list/svc/doclist/process
     body {"request":[{"docNumber":"202510469601","country":"CN","kindCode":"A"}]}  # SSE
```

- 请求头需浏览器 UA + `Origin/Referer: https://globaldossier.uspto.gov`，否则 403
- **限流凶**：连发几次就 `429 CLOUDFRONT RATE LIMITED`——provider 内置最小 2 秒间隔，
  429 上抛 RATE_LIMITED 由降级链兜底
- 文书字段：`docCode`（如 `210401-CN`=第一次审查意见）、`docDesc`（英文标题 +
  `(ORIGINAL)/(TRANSLATED)` 后缀）、`legalDateStr`（`MM/DD/YYYY`）、`docId`
  （OneDOC 标识，与 EPO register 同源）
- 同一文书 ORIGINAL 与机翻 TRANSLATED 成对出现，解析器只留 ORIGINAL

### 第 2 层现状（诚实说明）

register.epo.org 部署了 Cloudflare Turnstile 人机验证：headless 与裸 HTTP 均 403，
headful 真浏览器也会触发交互式挑战。按红线**不做任何验证码绕过**，因此该层在无人
值守巡检中通常返回 ACCESS_DENIED 并降级到 CNIPA；代码与 fixture 保留（站点不挑战
时即可工作）。Global Dossier 数据本身第 1 层已完整覆盖（同为 OneDOC 五局共享）。

### 降级规则

- 只在瞬时/结构性错误（NETWORK_ERROR、TEMPORARY_ERROR、RATE_LIMITED、ACCESS_DENIED、
  PAGE_STRUCTURE_CHANGED、CASE_NOT_FOUND、RESOLVE_FAILED）时降级；任一来源确认数据
  可读后即使事件列表为空也停止降级
- CNIPA 返回 AUTH_REQUIRED 时直接上抛——后台巡检**绝不**自动打开登录浏览器
- 只保留 `ORIGINAL` 官方发文且类型属于可提醒事件（OA/驳回/授权/补正/其他官方）：
  TRANSLATED 机翻副本、申请人提交文件、检索报告均被解析器丢弃
- 三个来源的官方事件都规范为同一 event_key（jurisdiction+申请号+类型+次数+官文日+
  规范化标题 的 sha256），同一事件跨来源只落一行（`source_trace` 记录谁报告过）

## 设计红线（有意为之，不接受“优化”掉）

- 不破解/自动识别验证码；登录、验证码、人机验证全部由用户在可见浏览器中人工完成
- 不绕过登录/权限/WAF；不用 stealth 插件、代理池、指纹伪装
- 同一时刻只有 1 个页面查询；案件间有最小间隔（默认 8 秒，PATX_DOSSIER_MIN_INTERVAL）
- 页面出现 系统繁忙/访问频繁/验证 提示 → 本批立即停止（RATE_LIMITED）
- 不保存、不记录任何密码/Cookie/Authorization 头；调试工件仅 截图+HTML+error.json
- 公开号≠申请号：解析不了就 RESOLVE_FAILED，绝不猜测
- 只有 HIGH 置信度的解析才允许 C++ 侧自动新增 OARecord

## 运行

需要 Python 3.10+：

```bash
pip install -r requirements.txt
playwright install chromium
```

patX 会自动启动 `service.py`；手工调试：

```bash
echo '{"op":"ping"}' | python3 tools/web_dossier/service.py
```

## 离线/CI 模式

设置 `PATX_DOSSIER_FIXTURE_DIR=tools/web_dossier/fixtures/cnipa` 后 provider
只读取本地 fixture 页面，不访问网络。pytest 全部基于 fixtures：

```bash
cd tools && python3 -m pytest web_dossier/tests -q
```

## 真实站点流程（借鉴 teamilkman/cnipa-cpquery 的实测情报）

cpquery（`cpquery.cponline.cnipa.gov.cn`）是瑞数防护的 Vue SPA：裸 HTTP 与
带 webdriver 标记的请求会被拦截，DOM 表格也不可依赖。本 provider 的真实模式：

1. 用户登录一次（扫码，专利业务办理 APP）——人完成，程序只等待
2. 检索：`input[placeholder*="例如"]` 填申请号 → 查询按钮 → 结果 `span.hover_active`
3. 在已登录页面上下文内 `fetch` 站点自身 JSON API：
   - `/api/view/gn/scxx/tzs`（通知书清单，审查意见在列）
   - `/api/view/gn/scxx/zjwj`（中间文件清单）
   认证自动带 `Authorization: Bearer <localStorage.ACCESS_TOKEN>`
4. 响应 `{code:200,data:[{name:"YYYY-MM-DD  文件名",additionalData:{rid}}]}`
   → 复用同一套分类器/指纹/最新OA选择逻辑
5. 瞬时拦截（HTML/空响应体）→ 隔 7 秒重试一次；再失败按 RATE_LIMITED 停整批

申请号规范为 API 需要的 13 位形式（权重 2–9、2–5 加权 mod 11 补校验位）。
HTML 表格解析器保留为 fixture/离线模式与兜底。

## 真实 CNIPA 页面改版后

`providers/cnipa.py` 的解析层是纯函数（`parse_dossier_documents`），
页面结构变化返回 PAGE_STRUCTURE_CHANGED 并在 `debug/web_dossier/<时间戳>_<案号>/`
保存 screenshot.png + page.html + error.json，用于修正选择器后补 fixture。
