#include "web_dossier.hpp"

#include <wx/app.h>
#include <wx/apptrait.h>
#include <wx/evtloop.h>
#include <wx/init.h>
#include <wx/utils.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>

#define CHECK(condition) do { if (!(condition)) { std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: " #condition << std::endl; return false; } } while (false)

namespace {

std::string QuoteCommand(const std::string& executable, bool exit_immediately = false) {
    return "\"" + executable + "\"" + (exit_immediately ? " --exit-immediately" : "");
}

void PumpWxEventsFor(int milliseconds) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (wxTheApp) wxTheApp->ProcessPendingEvents();
        if (auto* loop = wxEventLoopBase::GetActive()) loop->DispatchTimeout(0);
        wxMilliSleep(10);
    }
}

bool TestRepeatedShutdown(const std::string& helper) {
    for (int iteration = 0; iteration < 10; ++iteration) {
        auto transport = webdossier::CreateDefaultRpcTransport();
        std::string error;
        CHECK(transport->Start(QuoteCommand(helper), error));
        std::string response;
        std::atomic<bool> cancel{false};
        CHECK(transport->Call(R"({"op":"ping"})", 5000, response, cancel));
        CHECK(response.find("pong") != std::string::npos);
        transport->Shutdown();
        transport->Shutdown();
        transport.reset();
        PumpWxEventsFor(100);
    }
    return true;
}

bool TestRestartAfterChildExit(const std::string& helper) {
    auto transport = webdossier::CreateDefaultRpcTransport();
    std::string error;
    CHECK(transport->Start(QuoteCommand(helper, true), error));
    PumpWxEventsFor(500);
    CHECK(transport->Start(QuoteCommand(helper), error));
    std::string response;
    std::atomic<bool> cancel{false};
    CHECK(transport->Call(R"({"op":"ping"})", 5000, response, cancel));
    CHECK(response.find("pong") != std::string::npos);
    transport->Shutdown();
    return true;
}

bool TestDestroyBeforeChildExitNotification(const std::string& helper) {
    auto transport = webdossier::CreateDefaultRpcTransport();
    std::string error;
    CHECK(transport->Start(QuoteCommand(helper, true), error));
    transport.reset();
    PumpWxEventsFor(500);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    wxInitializer initializer;
    if (!initializer.IsOk()) {
        std::cout << "SKIP: wxWidgets initialization failed" << std::endl;
        return 0;
    }
    std::unique_ptr<wxEventLoopBase> event_loop(wxTheApp->GetTraits()->CreateEventLoop());
    wxEventLoopActivator activate_loop(event_loop.get());
    if (argc != 2) {
        std::cerr << "usage: patx_sidecar_transport_tests <absolute-helper-path>" << std::endl;
        return 1;
    }
    const std::string helper = argv[1];
    std::cout << "RUN: restart after child exit" << std::endl;
    if (!TestRestartAfterChildExit(helper)) return 1;
    std::cout << "RUN: repeated shutdown" << std::endl;
    if (!TestRepeatedShutdown(helper)) return 1;
    std::cout << "RUN: destroy before exit notification" << std::endl;
    if (!TestDestroyBeforeChildExitNotification(helper)) return 1;
    std::cout << "PASS: sidecar transport lifecycle" << std::endl;
    return 0;
}
