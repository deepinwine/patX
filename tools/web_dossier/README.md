# patX Web Dossier Sidecar（中国申请官方发文提醒）

Python sidecar 由 patX C++ GUI 通过 stdin/stdout 行 JSON-RPC 驱动。首期只检查中国申请，按以下顺序获取官方发文元数据：

1. USPTO Global Dossier；
2. EPO Global Dossier；
3. CNIPA。

前两层不可用或没有可用结果时，才进入下一层。USPTO 和 EPO 的公共案卷元数据不需要 USPTO ODP API Key 或 EPO OPS Key；EPO OPS Key 只用于 patX 原有的同族和法律状态功能。

sidecar 只返回 `ORIGINAL` 官方发文的类型、官方日期和来源，不下载正文。C++ 侧把第一次、第二次和后续审查意见，以及驳回、授权、补正和其他官方通知合并为提醒，并按稳定事件键跨来源去重。

## 设计红线（有意为之，不接受“优化”掉）

- 不破解或自动识别验证码；登录、验证码和人机验证全部由用户在可见浏览器中完成。
- 不绕过登录、权限或 WAF；不使用 stealth 插件、代理池或指纹伪装。
- 同一时刻只运行 1 个页面查询；案件间保留最小间隔（默认 8 秒，由 `PATX_DOSSIER_MIN_INTERVAL` 控制）。
- 页面出现「系统繁忙」「访问频繁」或验证提示时，本批立即停止并返回 `RATE_LIMITED`。
- 不把密码、Cookie 或 Authorization 头写入日志；调试工件只包含截图、HTML 和脱敏后的 `error.json`。
- 公开号不等于申请号。无法可靠解析时返回 `RESOLVE_FAILED`，不猜测。
- 只有高置信度的官方事件才允许 C++ 侧自动新增提醒。
- 授权和驳回只新增提醒，不修改专利主档状态。

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

patX GUI 通过显式重定向的 stdin/stdout 管道与 sidecar 通信。取消、超时或 RPC／管道错误会使当前 sidecar 实例关闭并丢弃；子进程异常退出后，下一次同步需要时会重新启动 sidecar。正常关闭时先发送 `shutdown`，再进行有界等待，必要时终止由 patX 启动的子进程，不会无限阻塞。

该通信机制不会保存或打印 Cookie、密码、API Key 或 RPC 请求正文。

## 后台调度与通知

- 默认检查间隔为 1 天，用户可以在 GUI 中选择 1、3 或 7 天。
- 软件启动约 30 秒后执行首次调度，随后每 30 分钟检查一次到期队列。
- 空队列或没有新事件时不启动多余查询，也不显示 UI。
- 新事件和日期冲突会刷新 OA 列表并显示桌面通知。
- CNIPA 登录失效时只显示登录提示；用户点击后才打开包含登录按钮的结果窗口，不自动启动登录流程。
- 同步线程在控制器销毁时会收到取消信号并完成 join，避免数据库切换或程序退出时留下后台 RPC。

## 离线/CI 模式

设置 `PATX_DOSSIER_FIXTURE_DIR=tools/web_dossier/fixtures/cnipa` 后，CNIPA Provider 只读取本地 fixture，不访问网络。USPTO 和 EPO Provider 的解析测试会直接加载各自的 fixture。pytest 全部基于这些离线数据：

```bash
cd tools
python3 -m pytest web_dossier/tests -q
```

fixture 分别位于：

- `fixtures/uspto_global_dossier`；
- `fixtures/epo_global_dossier`；
- `fixtures/cnipa`。

验收样例 `CN202510469601.5` 的期望事件为「第一次审查意见通知书」，官方日期为 `2026-05-23`。相同事件从不同来源返回时应合并为同一条记录，并把来源追加到 `source_trace`。

## CNIPA 真实站点流程

cpquery（`cpquery.cponline.cnipa.gov.cn`）是带访问防护的 Vue SPA，不能依赖裸 HTTP 或固定 DOM 表格。CNIPA Provider 的真实流程如下：

1. 用户通过专利业务办理 App 扫码登录；程序只等待登录完成。
2. 浏览器 Profile 保存在本机，后续启动时优先复用；密码和 Cookie 不写入日志。
3. 登录页检索申请号并进入案件。
4. 在已登录页面上下文中请求站点自身的通知书清单和中间文件清单接口。
5. 响应进入与 Global Dossier 相同的分类、事件键和合并流程。
6. 瞬时拦截会在间隔后重试 1 次；再次失败则按 `RATE_LIMITED` 停止本批。

申请号会规范为站点 API 需要的 13 位形式。HTML 表格解析器保留用于 fixture、离线测试和兼容性兜底。

## 真实 CNIPA 页面改版后

`providers/cnipa.py`、`providers/uspto_global_dossier.py` 和 `providers/epo_global_dossier.py` 的解析层可离线测试。页面结构变化时返回 `PAGE_STRUCTURE_CHANGED`，并在 `debug/web_dossier/<时间戳>_<案号>/` 保存 `screenshot.png`、`page.html` 和脱敏后的 `error.json`，用于更新解析器和 fixture。
