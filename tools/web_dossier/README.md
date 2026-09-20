# patX Web Dossier Sidecar（审查意见网页同步）

Python sidecar，由 patX C++ GUI 通过 stdin/stdout 行 JSON-RPC 驱动。
用 Playwright 持久化 Chromium profile 访问官方审查信息网站（Phase 1: CNIPA），
人工登录一次后按人工节奏逐件查询，发现新审查意见后由 C++ 侧按保护规则写入数据库。

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
