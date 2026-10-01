# Sidecar 进程生命周期稳定性设计

## 目标

修复 patX 审查信息 sidecar 在启动、正常退出、取消、超时和程序关闭时的进程所有权问题，避免管道不可用、旧响应污染下一次 RPC，以及 `wxProcess` 被重复释放导致的崩溃。

本设计参考 `glm/cn-prosecution-reminders` 中 `ef67631` 和 `f678ee2` 的处理方式，但保留当前分支已有的 `RpcTransport` 抽象、取消机制、线程互斥和测试注入接口。

## 当前问题

当前 `SidecarProcess` 通过 `wxProcess::Open()` 启动子进程，并在 `Shutdown()` 中直接 `delete process_`：

- `wxProcess` 的退出通知与 patX 主动释放可能交错，存在重复释放风险；
- 管道重定向依赖 `wxProcess::Open()` 的隐含行为，在不同平台上的可控性不足；
- 子进程异常退出后，输入输出流指针不能由专门的退出回调统一清理；
- 取消或超时后如果继续复用 transport，下一次 RPC 可能读到上一请求迟到的响应；
- 当前测试可以注入假 transport，但没有覆盖真实进程所有权的状态迁移。

## 方案比较

### 方案 A：直接 cherry-pick GLM 提交

直接采用 GLM 的 `SidecarProcess` 实现。

优点是改动快。缺点是 GLM 分支没有当前分支完整的 `RpcTransport`、取消路径和 manager 并发保护，直接移植会造成接口回退和大面积冲突，因此不采用。

### 方案 B：在当前 transport 架构内移植生命周期模型（采用）

保留 `RpcTransport` 和 `Manager` 公共接口，只替换默认 wxWidgets transport 的进程启动、退出通知和关闭逻辑。将生命周期状态迁移提取为无 GUI 依赖的小型状态对象，供自动测试验证。

优点是改动范围小，能同时保留当前分支的测试能力和 GLM 的进程安全修复。缺点是需要增加少量内部状态代码。

### 方案 C：重写异步进程监督器

使用新的事件队列和全异步 RPC 状态机管理 sidecar。

长期扩展性更好，但会同时改变线程模型、超时行为和 GUI 回调，超出本轮稳定性修复范围，因此不采用。

## 所有权模型

### wxWidgets 进程对象

默认 transport 创建 `PatxSidecarProcess`，并使用：

```text
wxExecute(command, wxEXEC_ASYNC, process)
```

`PatxSidecarProcess` 以 `wxPROCESS_REDIRECT` 构造，显式启用标准输入、标准输出和错误流管道。

启动成功后：

- wxWidgets 负责在子进程退出通知完成后回收 `PatxSidecarProcess`；
- `SidecarProcess` 只保存非拥有的 `wxProcess*`；
- `Shutdown()`、析构函数和取消路径均不得直接 `delete process_`。

启动失败时，wxWidgets 尚未接管对象，由启动函数删除刚创建的进程对象。

### 宿主与退出回调

`SidecarProcess` 与 `PatxSidecarProcess` 共享一个生命周期状态对象：

```text
owner_alive
child_running
generation
```

行为如下：

- 子进程退出：`OnTerminate()` 仅在 `owner_alive` 且 generation 匹配时通知宿主；
- 宿主收到通知：清空 `process_`、输入流和输出流，不释放进程对象；
- 宿主析构：先标记 `owner_alive = false`，再执行幂等关闭；
- 宿主销毁后的迟到回调只更新共享状态，不访问已经析构的 `SidecarProcess`。

generation 用于防止旧子进程的退出回调清除后来重启的新进程指针。

## 启动与关闭流程

### 启动

1. transport 已有运行中的子进程时直接复用；
2. 创建新的 generation 和 `PatxSidecarProcess`；
3. 通过 `wxExecute(..., wxEXEC_ASYNC, process)` 启动；
4. 启动失败时删除尚未被 wxWidgets 接管的对象并返回可操作错误；
5. 启动成功后取得重定向管道，并验证输入、输出流均可用；
6. 管道不可用时终止该 generation，返回启动失败，不进入 ping。

### 正常关闭

1. `Shutdown()` 可以被调用多次；
2. 首次关闭向 sidecar 写入 `shutdown` 请求；
3. 仅给予最多 1 秒的有界优雅退出窗口，不允许无限等待；
4. 有界窗口结束后子进程仍存在时发送 `wxSIGTERM`；
5. 立即使当前 transport 不再接受新 RPC，并清空宿主持有的非拥有指针；
6. 真实进程对象仍由 wxWidgets 在退出通知后回收。

### 取消和超时

取消或 RPC 失败后，当前 transport 不能继续使用：

- 关闭其子进程和管道；
- `Manager` 丢弃该 transport；
- 下一次请求重新创建 sidecar；
- 不保留尚未消费的缓冲区，避免旧响应被下一次 RPC 读取。

登录等待仍使用现有长超时；普通同步仍使用现有查询超时。本批不调整超时数值。

## 组件边界

### 生命周期状态单元

职责：记录 owner、child 和 generation 状态，决定退出回调是否可以访问宿主。它不依赖 wxWidgets，可由核心测试直接验证。

### `SidecarProcess`

职责：默认 wxWidgets transport，实现启动、单次 RPC、关闭和退出通知。它不处理案件同步业务，也不访问数据库。

### `Manager`

继续负责：

- 用 `rpc_mutex_` 串行化 transport 调用；
- 通过 `RpcTransport` 支持生产 transport 和测试假对象；
- ping 失败、取消或通信失败后丢弃失效 transport；
- 后续请求按需重建 sidecar。

## 错误处理

- 启动失败：返回「无法启动 sidecar 进程」并保留原命令信息；
- 重定向管道不可用：返回「sidecar 管道初始化失败」；
- ping 失败：关闭并丢弃 transport，保留现有依赖安装提示；
- 子进程异常退出：当前 RPC 返回失败，下一次请求重新启动；
- 重复 `Shutdown()`：无副作用；
- 宿主析构后的退出回调：不得访问宿主，不得崩溃。

错误信息和日志不得包含 API Key、Cookie、密码或 RPC 请求正文。

## 测试设计

自动测试至少覆盖：

1. generation 匹配的退出事件可以通知存活宿主；
2. 旧 generation 的迟到退出不能清除新子进程；
3. 宿主销毁后退出回调不能访问宿主；
4. 生命周期关闭操作可重复调用；
5. transport 取消后 manager 丢弃实例，下一次调用使用新实例；
6. RPC 超时或通信失败后不会复用旧缓冲区；
7. sidecar 正常 ping 后关闭不会触发崩溃；
8. GUI 全量构建和现有后台同步测试继续通过。

真实进程 smoke test 使用项目现有 sidecar 执行有限次数的「启动 → ping → 关闭」，不访问官网、不触发 CNIPA 登录，也不依赖网络。

## 兼容性

- 不修改数据库结构；
- 不修改 OA 合并、处理人保护或后台调度规则；
- 不修改 Python sidecar 协议；
- 不修改配置键和用户界面；
- macOS、Windows 和 Linux 共用同一所有权规则。

## 不在本次范围

- 原生 C++ USPTO 数据源；
- CNIPA 登录流程改造；
- 后台查询间隔和限流策略；
- 国内申请页 OA 集成；
- 期限计算引擎；
- sidecar RPC 改为全异步协议。
