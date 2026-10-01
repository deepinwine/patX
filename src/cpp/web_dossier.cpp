// 审查意见网页自动同步 - C++ core implementation. See web_dossier.hpp.
#include "web_dossier.hpp"
#include "patx/sidecar_process_lifecycle.hpp"

#include <nlohmann/json.hpp>

#include <wx/process.h>
#include <wx/utils.h>
#include <wx/filename.h>
#include <wx/log.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <csignal>
#include <memory>
#include <mutex>

using nlohmann::json;

namespace webdossier {

// Codes that mean "the whole site/provider is unusable right now" - the batch
// must stop instead of marking every remaining case as failed.
static bool IsBatchAbortingCode(ResultCode c) {
    return c == ResultCode::AuthRequired || c == ResultCode::SessionExpired ||
           c == ResultCode::RateLimited || c == ResultCode::AccessDenied ||
           c == ResultCode::PageStructureChanged || c == ResultCode::NetworkError;
}

// ---------------------------------------------------------------------------
// Sidecar process (stdin/stdout line JSON-RPC)
// ---------------------------------------------------------------------------

class SidecarProcess : public RpcTransport {
private:
    // Both the transport and the self-deleting wxProcess keep this state alive.
    // The mutex protects every stream access against OnTerminate deleting pipes.
    struct ProcessState {
        std::mutex mutex;
        wxProcess* process = nullptr; // Non-owning.
        wxInputStream* in = nullptr;
        wxOutputStream* out = nullptr;
        long pid = 0;
        std::uint64_t generation = 0;
        std::string buffer;

        void Clear() {
            process = nullptr;
            in = nullptr;
            out = nullptr;
            pid = 0;
            generation = 0;
            buffer.clear();
        }

        void ClearIfCurrent(wxProcess* child, std::uint64_t child_generation) {
            if (process == child && generation == child_generation) Clear();
        }
    };

    class PatxProcess : public wxProcess {
    public:
        PatxProcess(std::shared_ptr<ProcessState> state,
                    std::shared_ptr<SidecarProcessLifecycle> lifecycle,
                    std::uint64_t generation)
            : wxProcess(wxPROCESS_REDIRECT), state_(std::move(state)),
              lifecycle_(std::move(lifecycle)), generation_(generation) {}

        void OnTerminate(int, int) override {
            lifecycle_->NotifyChildExit(generation_, [] {});
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                // Cleanup is safe even after DetachOwner/StopChild: state is
                // shared, and no callback dereferences the former transport.
                state_->ClearIfCurrent(this, generation_);
            }
            delete this;
        }

    private:
        std::shared_ptr<ProcessState> state_;
        std::shared_ptr<SidecarProcessLifecycle> lifecycle_;
        std::uint64_t generation_;
    };

public:
    SidecarProcess()
        : lifecycle_(std::make_shared<SidecarProcessLifecycle>()),
          state_(std::make_shared<ProcessState>()) {
#ifndef _WIN32
        // A pipe can close between checking the child and writing. Let wx
        // report EPIPE as a stream error instead of terminating the GUI.
        static std::once_flag sigpipe_once;
        std::call_once(sigpipe_once, [] { std::signal(SIGPIPE, SIG_IGN); });
#endif
    }

    ~SidecarProcess() override {
        lifecycle_->DetachOwner();
        Shutdown();
    }

    // Host operations are serialized by Manager; termination callbacks use
    // only shared state and can run concurrently or inside wxExecute itself.
    bool Start(const std::string& command, std::string& error) override {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->process) return true;
        }
        const auto generation = lifecycle_->BeginChild();
        auto* process = new PatxProcess(state_, lifecycle_, generation);
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->process = process;
            state_->generation = generation;
        }
        // Do not hold state_->mutex: wxExecute may call OnTerminate here.
        const long pid = wxExecute(wxString::FromUTF8(command.c_str()), wxEXEC_ASYNC, process);
        if (pid == 0) {
            lifecycle_->StopChild(generation);
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                state_->ClearIfCurrent(process, generation);
            }
            delete process; // No child was started, so no exit callback owns it.
            error = "无法启动 sidecar 进程: " + command;
            return false;
        }
        bool pipes_ready = false;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->process != process || state_->generation != generation) {
                error = "sidecar 在启动时已退出";
                return false;
            }
            // The exit callback must acquire this lock before deleting process.
            state_->pid = pid;
            state_->in = process->GetInputStream();
            state_->out = process->GetOutputStream();
            pipes_ready = state_->in && state_->out;
        }
        if (!pipes_ready) {
            error = "sidecar 管道初始化失败";
            Shutdown();
            return false;
        }
        return true;
    }

    bool Call(const std::string& line_request, int timeout_ms, std::string& line_response,
              std::atomic<bool>& cancel) override {
        std::uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->process || !state_->out || !state_->in) return false;
            generation = state_->generation;
            if (!state_->out->Write(line_request.c_str(), line_request.size()).IsOk() ||
                !state_->out->Write("\n", 1).IsOk()) return false;
            state_->out->Sync();
            if (!state_->out->IsOk()) return false;
            state_->buffer.clear();
        }

        const auto start = std::chrono::steady_clock::now();
        while (true) {
            if (cancel.load()) return false;
            bool can_read = false;
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                if (state_->generation != generation || !state_->in) return false;
                can_read = state_->in->CanRead();
                if (can_read) {
                    char chunk[4096];
                    const size_t got = state_->in->Read(chunk, sizeof(chunk)).LastRead();
                    if (got == 0 && state_->in->Eof()) return false;
                    state_->buffer.append(chunk, got);
                    const size_t nl = state_->buffer.find('\n');
                    if (nl != std::string::npos) {
                        line_response = state_->buffer.substr(0, nl);
                        state_->buffer.erase(0, nl + 1);
                        return !line_response.empty();
                    }
                }
            }
            if (!can_read) wxMilliSleep(25);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            if (elapsed.count() > timeout_ms) return false;
        }
    }

    void Shutdown() override {
        std::uint64_t generation = 0;
        long pid = 0;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->process) {
                state_->Clear();
                return;
            }
            generation = state_->generation;
            pid = state_->pid;
            if (state_->out) {
                static constexpr char request[] = "{\"op\":\"shutdown\"}\n";
                if (state_->out->Write(request, sizeof(request) - 1).IsOk()) {
                    state_->out->Sync();
                    // Stream failure is best effort only; the bounded kill path
                    // below handles an unresponsive or already closed pipe.
                }
            }
            state_->in = nullptr;
            state_->out = nullptr;
            state_->buffer.clear();
        }

        if (!WaitForChildExit(pid, generation, 10, 100)) {
#ifdef _WIN32
            KillChild(pid, wxSIGKILL);
#else
            const auto result = KillChild(pid, wxSIGTERM);
            if (result != wxKILL_NO_PROCESS &&
                !WaitForChildExit(pid, generation, 10, 20)) {
                KillChild(pid, wxSIGKILL);
            }
#endif
        }
        lifecycle_->StopChild(generation);
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->generation == generation) state_->Clear();
        }
    }

private:
    bool WaitForChildExit(long pid, std::uint64_t generation, int attempts, int sleep_ms) {
        for (int attempt = 0; attempt <= attempts; ++attempt) {
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                if (state_->generation != generation || pid <= 0 ||
                    !wxProcess::Exists(pid)) return true;
            }
            if (attempt < attempts) wxMilliSleep(sleep_ms);
        }
        return false;
    }

    static wxKillError KillChild(long pid, wxSignal signal) {
        const auto result = wxProcess::Kill(pid, signal);
        if (result != wxKILL_OK && result != wxKILL_NO_PROCESS) {
            wxLogWarning("无法结束 sidecar 进程 %ld (错误 %d)", pid, static_cast<int>(result));
        }
        return result;
    }

    std::shared_ptr<SidecarProcessLifecycle> lifecycle_;
    std::shared_ptr<ProcessState> state_;
};

std::unique_ptr<RpcTransport> CreateDefaultRpcTransport() {
    return std::make_unique<SidecarProcess>();
}

// ---------------------------------------------------------------------------
// Manager
// ---------------------------------------------------------------------------

Manager::Manager(Database& db, const std::string& script_dir)
    : Manager(db, script_dir, RpcTransportFactory{CreateDefaultRpcTransport}) {}

Manager::Manager(Database& db, const std::string& script_dir,
                 RpcTransportFactory transport_factory)
    : db_(db), script_dir_(script_dir),
      transport_factory_(std::move(transport_factory)) {
    set_check_interval_days(atoi(db_.GetConfig("web_dossier_interval_days").c_str()));
}

Manager::Manager(Database& db, const std::string& script_dir,
                 std::unique_ptr<RpcTransport> transport)
    : Manager(db, script_dir, RpcTransportFactory{CreateDefaultRpcTransport}) {
    sidecar_ = std::move(transport);
}

Manager::~Manager() {
    std::lock_guard<std::mutex> lock(rpc_mutex_);
    sidecar_.reset();
}

bool Manager::sidecar_running() const {
    std::lock_guard<std::mutex> lock(rpc_mutex_);
    return sidecar_ != nullptr;
}

int Manager::check_interval_days() const { return check_interval_days_.load(); }
void Manager::set_check_interval_days(int days) {
    check_interval_days_ = NormalizeCheckIntervalDays(days);
}

bool Manager::EnsureRunning(std::string& error) {
    std::lock_guard<std::mutex> lock(rpc_mutex_);
    if (!sidecar_ && transport_factory_) sidecar_ = transport_factory_();
    if (!sidecar_) {
        error = "无法创建 sidecar transport，请重启 patX 后重试";
        return false;
    }
    const auto discard_transport = [this] {
        sidecar_->Shutdown();
        sidecar_.reset();
    };
    std::string python = db_.GetConfig("web_dossier_python");
    if (python.empty()) python = "python3";
    // script_dir_ is the absolute path of tools/web_dossier (resolved by the
    // GUI next to the executable, falling back to the working directory).
    std::string command = python + " \"" + script_dir_ + "/service.py\"";
    if (!sidecar_->Start(command, error)) {
        discard_transport();
        return false;
    }
    std::atomic<bool> no_cancel{false};
    std::string response;
    if (!sidecar_->Call("{\"op\":\"ping\"}", 15000, response, no_cancel)) {
        error = "sidecar 无响应（需要 Python 3.10+ 与 playwright：pip install playwright && "
                "playwright install chromium）";
        discard_transport();
        return false;
    }
    try {
        auto parsed = json::parse(response);
        if (parsed.value("op", "") != "pong") {
            error = "sidecar 响应异常: " + response.substr(0, 200);
            discard_transport();
            return false;
        }
        if (parsed.value("python_ok", false) == false) {
            error = "Python 环境缺少依赖: " + parsed.value("missing", "");
            discard_transport();
            return false;
        }
    } catch (const std::exception&) {
        error = "sidecar 响应不是 JSON: " + response.substr(0, 200);
        discard_transport();
        return false;
    }
    return true;
}

bool Manager::Rpc(const std::string& op, const std::string& json_payload,
                  std::string& response, std::atomic<bool>* cancel) {
    std::lock_guard<std::mutex> lock(rpc_mutex_);
    if (cancel && cancel->load()) return false;
    if (!sidecar_) return false;
    std::atomic<bool> no_cancel{false};
    json req;
    req["op"] = op;
    if (!json_payload.empty()) {
        try {
            req["args"] = json::parse(json_payload);
        } catch (...) {
            return false;
        }
    }
    // Login waits for the user (minutes); queries are slow but bounded.
    int timeout = op == "login" ? 1200000 : 300000;
    const bool ok = sidecar_->Call(req.dump(), timeout, response,
                                   cancel ? *cancel : no_cancel);
    if (!ok) {
        // A failed response may still arrive later. Drop the sidecar so
        // a future RPC cannot consume that stale line as its own response.
        sidecar_->Shutdown();
        sidecar_.reset();
    }
    return ok;
}

bool Manager::EpoCall(const std::string& epo_op, const std::string& publication_number,
                      std::string& response) {
    json args;
    args["epo_op"] = epo_op;
    args["publication_number"] = publication_number;
    args["consumer_key"] = db_.GetConfig("epo_consumer_key");
    args["consumer_secret"] = db_.GetConfig("epo_consumer_secret");
    return Rpc("epo", args.dump(), response, nullptr);
}

ResultCode Manager::Login(const std::string& provider, std::atomic<bool>& cancel) {
    std::string response;
    if (!Rpc("login", "{\"provider\":\"" + provider + "\"}", response, &cancel)) {
        return cancel.load() ? ResultCode::Cancelled : ResultCode::SidecarError;
    }
    try {
        auto parsed = json::parse(response);
        return ResultCodeFromString(parsed.value("code", "SIDECAR_ERROR"));
    } catch (...) {
        return ResultCode::SidecarError;
    }
}

bool Manager::ParseCaseResult(const std::string& json_body, RemoteCaseResult& out) {
    return ParseRemoteCaseResultJson(json_body, out);
}

CaseSyncReport Manager::ApplyRemoteResult(const Patent& patent, const RemoteCaseResult& remote) {
    return ApplyRemoteCaseResult(db_, patent, remote, check_interval_days_.load(),
                                 static_cast<long long>(time(nullptr)));
}

CaseSyncReport Manager::SyncCase(const Patent& patent, std::atomic<bool>& cancel) {
    CaseSyncReport report;
    report.patent_id = patent.id;
    report.geke_code = patent.geke_code;

    if (patent.application_number.empty() && patent.publication_number.empty()) {
        report.code = ResultCode::ResolveFailed;
        report.message = patent.geke_code + " 没有申请号也没有公开号，无法查询";
        return report;
    }
    report.identifier_used = patent.application_number.empty() ? patent.publication_number
                                                               : patent.application_number;
    if (cancel.load()) {
        report.code = ResultCode::Cancelled;
        return report;
    }

    json args;
    args["provider"] = "cnipa";
    args["application_number"] = patent.application_number;
    args["publication_number"] = patent.publication_number;
    std::string response;
    if (!Rpc("sync_case", args.dump(), response, &cancel)) {
        report.code = cancel.load() ? ResultCode::Cancelled : ResultCode::SidecarError;
        report.message = "sidecar 通信失败";
        return report;
    }
    RemoteCaseResult remote;
    if (!ParseCaseResult(response, remote)) {
        report.code = ResultCode::SidecarError;
        report.message = "sidecar 响应解析失败: " + response.substr(0, 200);
        return report;
    }
    if (!remote.ok && remote.code == "AUTH_REQUIRED") {
        // Persist the auth state, do not mark the case as failed.
        report.code = ResultCode::AuthRequired;
        report.message = "CNIPA 需要登录";
        DossierSyncState state;
        state.patent_id = patent.id;
        state.provider = "cnipa";
        state.last_checked_at = static_cast<long long>(time(nullptr));
        state.last_error_at = state.last_checked_at;
        state.last_error_code = "AUTH_REQUIRED";
        state.auth_state = "AUTH_REQUIRED";
        db_.UpsertDossierSyncState(state);
        return report;
    }
    return ApplyRemoteResult(patent, remote);
}

BatchSummary Manager::SyncAll(bool include_granted, int limit,
                              const std::function<bool(int, int, const std::string&)>& progress,
                              std::atomic<bool>& cancel) {
    BatchSummary summary;
    auto patents = db_.GetPatentsForDossierCheck(include_granted, limit);
    summary.total = static_cast<int>(patents.size());

    set_check_interval_days(atoi(db_.GetConfig("web_dossier_interval_days").c_str()));

    int index = 0;
    for (const auto& p : patents) {
        if (cancel.load()) break;
        if (progress && !progress(index, summary.total, p.geke_code)) break;
        index++;

        CaseSyncReport r = SyncCase(p, cancel);
        AccumulateBatchSummary(summary, r);
        if (r.code == ResultCode::Cancelled || IsBatchAbortingCode(r.code)) return summary;
    }
    return summary;
}

} // namespace webdossier
