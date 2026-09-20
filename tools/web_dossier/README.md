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

## 真实 CNIPA 页面改版后

`providers/cnipa.py` 的解析层是纯函数（`parse_dossier_documents`），
页面结构变化返回 PAGE_STRUCTURE_CHANGED 并在 `debug/web_dossier/<时间戳>_<案号>/`
保存 screenshot.png + page.html + error.json，用于修正选择器后补 fixture。
