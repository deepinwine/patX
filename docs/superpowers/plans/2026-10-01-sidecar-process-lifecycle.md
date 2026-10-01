# Sidecar 进程生命周期稳定性实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 在保留 `RpcTransport`、取消机制和测试注入能力的前提下，消除 sidecar 启停过程中的 `wxProcess` 重复释放、迟到回调和旧响应复用风险。

**架构：** 新增一个不依赖 wxWidgets 的代际生命周期状态单元，负责 owner 存活状态和 active generation 判定。默认 wxWidgets transport 使用显式重定向的 `wxExecute()` 与非拥有进程指针；`Manager` 使用可注入工厂重建失效 transport，并在任何 RPC 通信失败后立即丢弃旧实例。

**技术栈：** C++17、wxWidgets `wxProcess`/`wxExecute`、CMake/CTest、自有 manager 测试、离线回显子进程。

---

## 文件结构

- 创建 `include/patx/sidecar_process_lifecycle.hpp`：无 wxWidgets 依赖的 owner/generation 状态单元。
- 修改 `include/web_dossier.hpp`：声明 transport 工厂类型、默认 transport 工厂及测试工厂构造函数。
- 修改 `src/cpp/web_dossier.cpp`：实现安全的 wxWidgets sidecar transport，并统一 RPC 失败后的实例失效处理。
- 修改 `tests/test_web_dossier_manager.cpp`：覆盖代际状态、重复关闭、取消、通信失败和 transport 重建。
- 创建 `tests/sidecar_echo_helper.cpp`：只处理 `ping` 和 `shutdown` 的离线子进程。
- 创建 `tests/test_sidecar_transport.cpp`：真实 wxWidgets 管道启动/关闭 smoke test。
- 修改 `CMakeLists.txt`：注册离线 helper 和 transport 生命周期测试。

## 任务 1：定义可测试的代际生命周期状态

**文件：**
- 创建：`include/patx/sidecar_process_lifecycle.hpp`
- 修改：`tests/test_web_dossier_manager.cpp`

- [ ] **步骤 1：编写 generation 状态失败测试**

在 `tests/test_web_dossier_manager.cpp` 增加：

```cpp
#include "patx/sidecar_process_lifecycle.hpp"

static bool TestSidecarProcessLifecycleState() {
    SidecarProcessLifecycle state;

    const auto first = state.BeginChild();
    int notifications = 0;
    CHECK(state.NotifyChildExit(first, [&] { notifications++; }));
    CHECK_EQ(notifications, 1);

    const auto stale = state.BeginChild();
    const auto current = state.BeginChild();
    CHECK(!state.NotifyChildExit(stale, [&] { notifications++; }));
    CHECK(state.NotifyChildExit(current, [&] { notifications++; }));
    CHECK_EQ(notifications, 2);

    const auto stopped = state.BeginChild();
    CHECK(state.StopChild(stopped));
    CHECK(!state.StopChild(stopped));
    CHECK(!state.NotifyChildExit(stopped, [&] { notifications++; }));

    const auto detached = state.BeginChild();
    state.DetachOwner();
    CHECK(!state.NotifyChildExit(detached, [&] { notifications++; }));
    CHECK_EQ(notifications, 2);
    return true;
}
```

在匿名命名空间前增加本地断言宏（当前文件没有测试框架断言）：

```cpp
#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << __FILE__ << ':' << __LINE__                           \
                      << ": check failed: " #condition << std::endl;           \
            return false;                                                      \
        }                                                                      \
    } while (false)

#define CHECK_EQ(actual, expected) CHECK((actual) == (expected))
```

把原 `main()` 的取消测试主体提取为 `static bool TestCancellationDropsTransport()`，并让 `main()` 顺序调用两个 `bool` 测试函数；任一返回 `false` 时返回 `1`。

- [ ] **步骤 2：运行构建并确认因头文件缺失而失败**

运行：

```bash
cmake --build build -j4 --target patx_web_dossier_manager_tests
```

预期：编译失败，提示找不到 `patx/sidecar_process_lifecycle.hpp`。

- [ ] **步骤 3：实现最小生命周期状态单元**

创建 `include/patx/sidecar_process_lifecycle.hpp`：

```cpp
#pragma once

#include <cstdint>
#include <mutex>
#include <utility>

namespace webdossier {

class SidecarProcessLifecycle {
public:
    std::uint64_t BeginChild() {
        std::lock_guard<std::mutex> lock(mutex_);
        active_generation_ = ++next_generation_;
        return active_generation_;
    }

    template <typename Callback>
    bool NotifyChildExit(std::uint64_t generation, Callback&& callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!owner_alive_ || generation == 0 || generation != active_generation_) {
            return false;
        }
        active_generation_ = 0;
        std::forward<Callback>(callback)();
        return true;
    }

    bool StopChild(std::uint64_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation == 0 || generation != active_generation_) return false;
        active_generation_ = 0;
        return true;
    }

    void DetachOwner() {
        std::lock_guard<std::mutex> lock(mutex_);
        owner_alive_ = false;
    }

private:
    std::mutex mutex_;
    bool owner_alive_ = true;
    std::uint64_t next_generation_ = 0;
    std::uint64_t active_generation_ = 0;
};

}  // namespace webdossier
```

回调在状态互斥锁内执行，使 `DetachOwner()` 必须等待正在执行的退出回调结束，从而避免宿主析构与回调解引用交错。回调不得反向调用该状态对象的方法。

- [ ] **步骤 4：运行 manager 测试并确认通过**

运行：

```bash
cmake --build build -j4 --target patx_web_dossier_manager_tests
ctest --test-dir build -R '^patx_web_dossier_manager_tests$' --output-on-failure
```

预期：目标构建成功，`1/1` 测试通过。

- [ ] **步骤 5：提交状态单元**

```bash
git add include/patx/sidecar_process_lifecycle.hpp tests/test_web_dossier_manager.cpp
git commit -m "test(sidecar): 定义进程代际状态"
```

## 任务 2：通信失败后丢弃并重建 transport

**文件：**
- 修改：`include/web_dossier.hpp:244-304`
- 修改：`src/cpp/web_dossier.cpp:103-192`
- 修改：`tests/test_web_dossier_manager.cpp`

- [ ] **步骤 1：编写 transport 重建失败测试**

在 `tests/test_web_dossier_manager.cpp` 增加一个脚本化 transport：

```cpp
struct ScriptedTransportState {
    int created = 0;
    int shutdown = 0;
    int destroyed = 0;
};

class ScriptedTransport : public RpcTransport {
public:
    ScriptedTransport(std::shared_ptr<ScriptedTransportState> state,
                      bool fail_sync)
        : state_(std::move(state)), fail_sync_(fail_sync) {
        state_->created++;
    }
    ~ScriptedTransport() override { state_->destroyed++; }
    bool Start(const std::string&, std::string&) override { return true; }
    bool Call(const std::string& request, int, std::string& response,
              std::atomic<bool>&) override {
        if (request.find("\"op\":\"ping\"") != std::string::npos) {
            response = R"({"op":"pong","python_ok":true})";
            return true;
        }
        if (fail_sync_) return false;
        response = R"({"ok":true,"code":"NO_CHANGE","documents":[],"latest_event":null})";
        return true;
    }
    void Shutdown() override { state_->shutdown++; }
private:
    std::shared_ptr<ScriptedTransportState> state_;
    bool fail_sync_ = false;
};
```

测试使用工厂第一次返回 `fail_sync=true`，第二次返回正常 transport：

```cpp
auto state = std::make_shared<ScriptedTransportState>();
int sequence = 0;
Manager manager(db, ".", [state, &sequence] {
    return std::make_unique<ScriptedTransport>(state, sequence++ == 0);
});
std::string error;
CHECK(manager.EnsureRunning(error));
auto failed = manager.SyncCase(patent, cancel);
CHECK(failed.code == ResultCode::SidecarError);
CHECK(!manager.sidecar_running());
CHECK_EQ(state->shutdown, 1);
CHECK_EQ(state->destroyed, 1);
CHECK(manager.EnsureRunning(error));
CHECK(manager.sidecar_running());
CHECK_EQ(state->created, 2);
```

- [ ] **步骤 2：运行构建并确认构造函数缺失**

运行：

```bash
cmake --build build -j4 --target patx_web_dossier_manager_tests
```

预期：编译失败，提示 `Manager` 没有接收 `RpcTransportFactory` 的构造函数。

- [ ] **步骤 3：声明 transport 工厂**

在 `include/web_dossier.hpp` 增加：

```cpp
using RpcTransportFactory = std::function<std::unique_ptr<RpcTransport>()>;

std::unique_ptr<RpcTransport> CreateDefaultRpcTransport();
```

为 `Manager` 增加工厂构造函数并保存工厂：

```cpp
Manager(Database& db, const std::string& script_dir,
        RpcTransportFactory transport_factory);

RpcTransportFactory transport_factory_;
```

保留现有接收 `std::unique_ptr<RpcTransport>` 的构造函数，保证已有测试和调用方兼容。

- [ ] **步骤 4：实现工厂创建与统一失效处理**

在 `src/cpp/web_dossier.cpp`：

```cpp
Manager::Manager(Database& db, const std::string& script_dir)
    : Manager(db, script_dir, [] { return CreateDefaultRpcTransport(); }) {}

Manager::Manager(Database& db, const std::string& script_dir,
                 RpcTransportFactory transport_factory)
    : db_(db), script_dir_(script_dir),
      transport_factory_(std::move(transport_factory)) {
    set_check_interval_days(atoi(db_.GetConfig("web_dossier_interval_days").c_str()));
}
```

`EnsureRunning()` 在 `sidecar_` 为空时调用工厂；工厂返回空指针时返回「无法创建 sidecar transport」。现有 `unique_ptr` 构造函数保留传入实例，同时把后续工厂设置为默认工厂。

将 `Rpc()` 的失败路径统一改为：

```cpp
if (!ok) {
    sidecar_->Shutdown();
    sidecar_.reset();
}
return ok;
```

这条规则同时适用于取消、超时、子进程退出和管道错误；下一次调用只能创建新实例。

- [ ] **步骤 5：运行 manager 测试**

运行：

```bash
cmake --build build -j4 --target patx_web_dossier_manager_tests
ctest --test-dir build -R '^patx_web_dossier_manager_tests$' --output-on-failure
```

预期：`1/1` 通过；原取消测试仍确认 `saw_cancel`、`shutdown` 和 `destroyed`。

- [ ] **步骤 6：提交 manager 恢复逻辑**

```bash
git add include/web_dossier.hpp src/cpp/web_dossier.cpp tests/test_web_dossier_manager.cpp
git commit -m "fix(sidecar): 通信失败后重建传输实例"
```

## 任务 3：替换真实 wxWidgets 进程所有权

**文件：**
- 修改：`src/cpp/web_dossier.cpp:1-101`
- 创建：`tests/sidecar_echo_helper.cpp`
- 创建：`tests/test_sidecar_transport.cpp`
- 修改：`CMakeLists.txt:108-183`

- [ ] **步骤 1：创建离线回显 helper**

创建 `tests/sidecar_echo_helper.cpp`：

```cpp
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--exit-immediately") return 0;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.find("\"op\":\"ping\"") != std::string::npos) {
            std::cout << R"({"op":"pong","python_ok":true})" << std::endl;
        } else if (line.find("\"op\":\"shutdown\"") != std::string::npos) {
            return 0;
        } else {
            std::cout << R"({"ok":true})" << std::endl;
        }
    }
    return 0;
}
```

- [ ] **步骤 2：编写真实 transport 生命周期测试**

创建 `tests/test_sidecar_transport.cpp`。测试接收 helper 绝对路径，使用 `CreateDefaultRpcTransport()` 执行：

```cpp
wxInitializer initializer;
if (!initializer.IsOk()) {
    std::cout << "SKIP: wxWidgets unavailable in this environment\n";
    return 0;
}

for (int i = 0; i < 10; ++i) {
    auto transport = CreateDefaultRpcTransport();
    std::string error;
    CHECK(transport->Start(QuoteCommand(argv[1]), error));
    std::atomic<bool> cancel{false};
    std::string response;
    CHECK(transport->Call(R"({"op":"ping"})", 5000, response, cancel));
    CHECK(response.find("\"op\":\"pong\"") != std::string::npos);
    transport->Shutdown();
    transport->Shutdown();
    transport.reset();
    PumpWxEventsFor(100);
}
```

再增加两个确定性场景：

1. 用同一个 transport 启动 `helper --exit-immediately`，循环泵送 wx 事件直到子进程退出；随后对同一个 transport 调用 `Start()` 启动正常 helper，并要求 `ping` 成功。旧实现会保留非空的 `process_`，第二次 `Start()` 错误地直接返回，之后 `ping` 失败。
2. 启动 `helper --exit-immediately` 后立即销毁 transport，再泵送 wx 事件 500 ms；进程不得崩溃，用于覆盖宿主已析构后的迟到退出通知。

测试文件增加以下公共辅助函数，避免固定休眠掩盖事件循环问题：

```cpp
static void PumpWxEventsFor(int milliseconds) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (wxTheApp) wxTheApp->Yield(true);
        wxMilliSleep(10);
    }
}

static std::string QuoteCommand(const std::string& executable,
                                const std::string& argument = {}) {
    std::string command = "\"" + executable + "\"";
    if (!argument.empty()) command += " " + argument;
    return command;
}
```

Windows、macOS 和 Linux 的命令路径统一由 `QuoteCommand()` 使用双引号包裹；helper 不访问网络和数据库。

- [ ] **步骤 3：注册测试并确认链接或行为失败**

在 `CMakeLists.txt` 的 `BUILD_TESTING AND PATX_BUILD_GUI` 分支增加：

```cmake
add_executable(patx_sidecar_echo_helper tests/sidecar_echo_helper.cpp)
add_executable(patx_sidecar_transport_tests
    tests/test_sidecar_transport.cpp
    src/cpp/web_dossier.cpp)
target_link_libraries(patx_sidecar_transport_tests PRIVATE
    patx_core ${wxWidgets_LIBRARIES})
target_include_directories(patx_sidecar_transport_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${wxWidgets_INCLUDE_DIRS})
add_test(NAME patx_sidecar_transport_tests
    COMMAND patx_sidecar_transport_tests
            $<TARGET_FILE:patx_sidecar_echo_helper>)
```

运行：

```bash
cmake -S . -B build
cmake --build build -j4 --target patx_sidecar_transport_tests
ctest --test-dir build -R '^patx_sidecar_transport_tests$' --output-on-failure
```

预期：测试在“子进程自行退出后重新启动”场景失败，具体表现为第二次启动后的 `ping` 返回 `false`；如果旧实现因平台事件时序在更早位置崩溃，也属于有效红灯，但必须保留完整 CTest 输出。不得用测试代码中的强制失败制造红灯。

- [ ] **步骤 4：实现显式重定向进程类**

在 `src/cpp/web_dossier.cpp` 中将当前 `SidecarProcess` 替换为：

```cpp
class SidecarProcess : public RpcTransport {
public:
    SidecarProcess()
        : lifecycle_(std::make_shared<SidecarProcessLifecycle>()) {}
    ~SidecarProcess() override {
        lifecycle_->DetachOwner();
        Shutdown();
    }

    bool Start(const std::string& command, std::string& error) override;
    bool Call(const std::string& request, int timeout_ms,
              std::string& response, std::atomic<bool>& cancel) override;
    void Shutdown() override;

private:
    class PatxProcess : public wxProcess {
    public:
        PatxProcess(SidecarProcess* owner,
                    std::shared_ptr<SidecarProcessLifecycle> lifecycle,
                    std::uint64_t generation)
            : wxProcess(wxPROCESS_REDIRECT), owner_(owner),
              lifecycle_(std::move(lifecycle)), generation_(generation) {}
        void OnTerminate(int, int) override {
            lifecycle_->NotifyChildExit(generation_, [this] {
                owner_->OnChildExited(generation_);
            });
            // 覆盖默认 OnTerminate 后必须显式自回收。此时宿主已清除
            // 非拥有指针，或者生命周期状态已禁止访问宿主。
            delete this;
        }
    private:
        SidecarProcess* owner_;
        std::shared_ptr<SidecarProcessLifecycle> lifecycle_;
        std::uint64_t generation_ = 0;
    };

    void OnChildExited(std::uint64_t generation);
    std::shared_ptr<SidecarProcessLifecycle> lifecycle_;
    wxProcess* process_ = nullptr;  // non-owning; PatxProcess 退出时自回收
    wxInputStream* in_ = nullptr;
    wxOutputStream* out_ = nullptr;
    long pid_ = 0;
    std::uint64_t generation_ = 0;
    std::string buffer_;
};
```

`Start()` 必须使用：

```cpp
auto* process = new PatxProcess(this, lifecycle_, generation);
const long pid = wxExecute(wxString::FromUTF8(command.c_str()),
                           wxEXEC_ASYNC, process);
```

只有 `pid == 0` 时才能由启动函数删除尚未运行的 `process`。启动成功后，宿主禁止手动删除；`PatxProcess::OnTerminate()` 在通知生命周期状态和宿主之后执行 `delete this`。这与 wxWidgets 官方 `samples/exec/exec.cpp` 对自定义 async process handler 的回收规则一致，也修复了 GLM 实现覆盖 `OnTerminate()` 后没有自回收造成的泄漏。

`Shutdown()`：

1. 保存当前 generation 和 PID；
2. 向可用输出流写入 `shutdown`；
3. 最多等待 10 次、每次 100 ms，并使用 `wxProcess::Exists(pid)` 判断；
4. 仍存在时调用 `wxProcess::Kill(pid, wxSIGTERM)`；
5. 调用 `lifecycle_->StopChild(generation)`；
6. 清空非拥有指针、PID、generation 和缓冲区；
7. 不调用 `delete process_`；迟到退出回调负责自回收。

`OnChildExited()` 仅在 generation 等于成员当前 generation 时清空指针和缓冲区。

实现：

```cpp
std::unique_ptr<RpcTransport> CreateDefaultRpcTransport() {
    return std::make_unique<SidecarProcess>();
}
```

- [ ] **步骤 5：验证真实启停和现有 manager 行为**

运行：

```bash
cmake --build build -j4 --target \
  patx_sidecar_transport_tests patx_web_dossier_manager_tests patx
ctest --test-dir build \
  -R 'patx_sidecar_transport_tests|patx_web_dossier_manager_tests' \
  --output-on-failure
```

预期：两个测试均通过，`patx` GUI 完整链接成功。运行结束后不得残留 `patx_sidecar_echo_helper` 进程。

- [ ] **步骤 6：提交真实 transport 修复**

```bash
git add src/cpp/web_dossier.cpp tests/sidecar_echo_helper.cpp \
  tests/test_sidecar_transport.cpp CMakeLists.txt
git commit -m "fix(sidecar): 安全管理 wxProcess 生命周期"
```

## 任务 4：回归验证、文档同步与 GitHub 备份

**文件：**
- 修改：`tools/web_dossier/README.md`

- [ ] **步骤 1：补充运行时说明**

在 `tools/web_dossier/README.md` 的运行说明中增加：

- sidecar 通过显式重定向管道与 GUI 通信；
- 取消、超时或异常退出后当前进程会被丢弃，下一次同步自动重建；
- 正常关闭会先发送 `shutdown`，随后执行有界终止；
- 该机制不会保存或打印 Cookie、密码和 API Key。

- [ ] **步骤 2：运行完整 C++ 构建和测试**

运行：

```bash
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

预期：GUI `patx` 构建成功，所有 CTest 通过，包括新增的 sidecar transport 测试。

- [ ] **步骤 3：运行 Python sidecar 回归测试**

运行：

```bash
cd tools
/opt/anaconda3/bin/python -m pytest web_dossier/tests -q
```

预期：现有 278 项 Python 测试全部通过；本批不修改 Python 协议。

- [ ] **步骤 4：检查进程、差异和工作树**

运行：

```bash
pgrep -af patx_sidecar_echo_helper || true
git diff --check
git status --short --branch
```

预期：没有回显 helper 残留；`git diff --check` 无输出；仅 README 存在预期修改。

- [ ] **步骤 5：提交文档**

```bash
git add tools/web_dossier/README.md
git commit -m "docs(sidecar): 补充异常恢复与关闭规则"
```

- [ ] **步骤 6：最终审查与备份**

按规格逐项审查以下内容：

- 启动成功后宿主没有手动 `delete process_`，退出处理器只在 `OnTerminate()` 末尾自回收一次；
- generation 阻止迟到回调清理新进程；
- owner 析构后回调不会解引用宿主；
- 任意 RPC 失败都会丢弃失效 transport；
- 取消测试、真实管道测试、GUI 构建和 Python 测试均有新鲜证据。

审查通过后运行：

```bash
git push origin codex/cn-prosecution-reminders
git rev-parse HEAD
git ls-remote origin refs/heads/codex/cn-prosecution-reminders
```

预期：本地与远端 SHA 完全一致，工作树干净。
