#include "web_dossier.hpp"

#include <nlohmann/json.hpp>
#include <wx/app.h>
#include <wx/apptrait.h>
#include <wx/evtloop.h>
#include <wx/filename.h>
#include <wx/init.h>
#include <wx/process.h>
#include <wx/utils.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#define CHECK(condition) do { if (!(condition)) { std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: " #condition << std::endl; return false; } } while (false)

namespace {

std::string QuoteCommand(const std::string& executable, const std::string& mode = "") {
    return "\"" + executable + "\"" + (mode.empty() ? "" : " " + mode);
}

void PumpWxEvents() {
    if (wxTheApp) wxTheApp->ProcessPendingEvents();
    if (auto* loop = wxEventLoopBase::GetActive()) loop->DispatchTimeout(0);
}

bool WaitUntil(const std::function<bool()>& condition, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    do {
        PumpWxEvents();
        if (condition()) return true;
        wxMilliSleep(10);
    } while (std::chrono::steady_clock::now() < deadline);
    PumpWxEvents();
    return condition();
}

bool ChildHasExited(long pid) {
    return pid > 0 && WaitUntil([pid] { return !wxProcess::Exists(pid); }, 3000);
}

bool RequestPid(webdossier::RpcTransport& transport, const std::string& op, long& pid) {
    std::atomic<bool> cancel{false};
    std::string response;
    CHECK(transport.Call("{\"op\":\"" + op + "\"}", 5000, response, cancel));
    const auto parsed = nlohmann::json::parse(response, nullptr, false);
    CHECK(!parsed.is_discarded());
    pid = parsed.value("pid", 0L);
    CHECK(pid > 0);
    if (op == "ping") CHECK(parsed.value("op", "") == "pong");
    return true;
}

bool TestRepeatedShutdown(const std::string& helper) {
    for (int iteration = 0; iteration < 10; ++iteration) {
        auto transport = webdossier::CreateDefaultRpcTransport();
        std::string error;
        CHECK(transport->Start(QuoteCommand(helper), error));
        long pid = 0;
        CHECK(RequestPid(*transport, "ping", pid));
        transport->Shutdown();
        transport->Shutdown();
        transport.reset();
        CHECK(ChildHasExited(pid));
    }
    return true;
}

bool TestRestartAfterChildExit(const std::string& helper) {
    auto transport = webdossier::CreateDefaultRpcTransport();
    std::string error;
    CHECK(transport->Start(QuoteCommand(helper), error));
    long pid = 0;
    CHECK(RequestPid(*transport, "exit", pid));
    CHECK(ChildHasExited(pid));
    CHECK(transport->Start(QuoteCommand(helper), error));
    CHECK(RequestPid(*transport, "ping", pid));
    transport->Shutdown();
    CHECK(ChildHasExited(pid));
    return true;
}

bool TestRestartAfterImmediateChildExit(const std::string& helper) {
    auto transport = webdossier::CreateDefaultRpcTransport();
    std::string error;
    // wxExecute can deliver termination before returning; either Start result
    // is valid for the immediately exiting child, but restart must succeed.
    if (!transport->Start(QuoteCommand(helper, "--exit-immediately"), error)) {
        CHECK(!error.empty());
    }
    long pid = 0;
    std::atomic<bool> cancel{false};
    CHECK(WaitUntil([&] {
        if (!transport->Start(QuoteCommand(helper), error)) return false;
        std::string response;
        if (!transport->Call(R"({"op":"ping"})", 500, response, cancel)) return false;
        const auto parsed = nlohmann::json::parse(response, nullptr, false);
        if (parsed.is_discarded() || parsed.value("op", "") != "pong") return false;
        pid = parsed.value("pid", 0L);
        return pid > 0;
    }, 5000));
    transport->Shutdown();
    CHECK(ChildHasExited(pid));
    return true;
}

bool TestDestroyImmediatelyAfterStartingExitingChild(const std::string& helper) {
    auto transport = webdossier::CreateDefaultRpcTransport();
    std::string error;
    const bool started = transport->Start(QuoteCommand(helper, "--exit-immediately"), error);
    transport.reset(); // No RPC, event pumping, or wait before destruction.
    CHECK(started || !error.empty());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    do {
        PumpWxEvents();
        wxMilliSleep(10);
    } while (std::chrono::steady_clock::now() < deadline);
    return true;
}

bool TestDestroyBeforeChildExitNotification(const std::string& helper) {
    auto transport = webdossier::CreateDefaultRpcTransport();
    std::string error;
    CHECK(transport->Start(QuoteCommand(helper), error));
    long pid = 0;
    CHECK(RequestPid(*transport, "exit", pid));
    transport.reset(); // No event pumping between exit request and destruction.
    CHECK(ChildHasExited(pid));
    return true;
}

bool TestClosedInputWithoutExitNotification(const std::string& helper) {
    auto transport = webdossier::CreateDefaultRpcTransport();
    std::string error;
    CHECK(transport->Start(QuoteCommand(helper, "--close-input-on-ping"), error));
    long pid = 0;
    // The helper closes stdin before flushing pong, so this is a deterministic
    // closed pipe without sleeping or processing the pending exit notification.
    CHECK(RequestPid(*transport, "ping", pid));
    std::atomic<bool> cancel{false};
    std::string response;
    CHECK(!transport->Call(R"({"op":"ping"})", 500, response, cancel));
    transport->Shutdown();
    CHECK(ChildHasExited(pid));
    return true;
}

bool TestIgnoreShutdown(const std::string& helper) {
    auto transport = webdossier::CreateDefaultRpcTransport();
    std::string error;
    CHECK(transport->Start(QuoteCommand(helper, "--ignore-shutdown"), error));
    long pid = 0;
    CHECK(RequestPid(*transport, "ping", pid));
    transport->Shutdown();
    const bool exited = ChildHasExited(pid);
    if (!exited) {
        std::cerr << "helper ignored shutdown, pid=" << pid << std::endl;
        wxProcess::Kill(pid, wxSIGKILL); // Clean up only this test's child on failure.
        ChildHasExited(pid);
    }
    CHECK(exited);
    return true;
}

bool TestExitDuringConcurrentCall(const std::string& helper) {
    for (int iteration = 0; iteration < 30; ++iteration) {
        auto transport = webdossier::CreateDefaultRpcTransport();
        std::string error;
        CHECK(transport->Start(QuoteCommand(helper, "--exit-on-ping"), error));
        long pid = 0;
        CHECK(RequestPid(*transport, "ready", pid));
        std::atomic<bool> cancel{false};
        std::atomic<bool> completed{false};
        bool call_succeeded = true;
        std::thread caller([&] {
            std::string response;
            call_succeeded = transport->Call(R"({"op":"ping"})", 5000, response, cancel);
            completed = true;
        });
        const bool returned = WaitUntil([&] { return completed.load(); }, 3000);
        if (!returned) cancel = true;
        caller.join();
        CHECK(returned);
        CHECK(!call_succeeded);
        transport->Shutdown();
        CHECK(ChildHasExited(pid));
    }
    return true;
}

bool TestRapidExitDuringStart(const std::string& helper) {
    for (int iteration = 0; iteration < 100; ++iteration) {
        struct PidFile {
            wxString path = wxFileName::CreateTempFileName("patx-sidecar-pid-");
            ~PidFile() { if (!path.empty()) wxRemoveFile(path); }
        } pid_file;
        CHECK(!pid_file.path.empty());
        auto transport = webdossier::CreateDefaultRpcTransport();
        std::string error;
        // A child exiting inside wxExecute may make Start fail normally.
        const std::string mode = "--exit-with-pid-file \"" +
                                 pid_file.path.ToStdString() + "\"";
        if (!transport->Start(QuoteCommand(helper, mode), error)) {
            CHECK(!error.empty());
        }
        long pid = 0;
        CHECK(WaitUntil([&] {
            std::ifstream input(pid_file.path.ToStdString());
            input >> pid;
            return pid > 0 && !wxProcess::Exists(pid);
        }, 3000));
        transport.reset();
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    wxInitializer initializer;
    if (!initializer.IsOk()) {
        std::cout << "SKIP: wxWidgets initialization failed" << std::endl;
        return 77;
    }
    std::unique_ptr<wxEventLoopBase> event_loop(wxTheApp->GetTraits()->CreateEventLoop());
    if (!event_loop || !event_loop->IsOk()) {
        std::cout << "SKIP: wxWidgets event loop unavailable" << std::endl;
        return 77;
    }
    wxEventLoopActivator activate_loop(event_loop.get());
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: patx_sidecar_transport_tests <absolute-helper-path> [scenario]"
                  << std::endl;
        return 1;
    }
    const std::string helper = argv[1];
    const std::string selected = argc == 3 ? argv[2] : "";
    struct Scenario { const char* name; bool (*run)(const std::string&); };
    const Scenario scenarios[] = {
        {"closed-input", TestClosedInputWithoutExitNotification},
        {"ignore-shutdown", TestIgnoreShutdown},
        {"concurrent-exit", TestExitDuringConcurrentCall},
        {"rapid-exit", TestRapidExitDuringStart},
        {"restart-immediate", TestRestartAfterImmediateChildExit},
        {"restart", TestRestartAfterChildExit},
        {"repeated-shutdown", TestRepeatedShutdown},
        {"destroy-before-notification", TestDestroyBeforeChildExitNotification},
        {"destroy-immediate", TestDestroyImmediatelyAfterStartingExitingChild},
    };
    bool matched = false;
    for (const auto& scenario : scenarios) {
        if (!selected.empty() && selected != scenario.name) continue;
        matched = true;
        std::cout << "RUN: " << scenario.name << std::endl;
        if (!scenario.run(helper)) return 1;
    }
    if (!matched) return 1;
    std::cout << "PASS: sidecar transport lifecycle" << std::endl;
    return 0;
}
