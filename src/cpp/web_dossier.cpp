// 审查意见网页自动同步 - C++ core implementation. See web_dossier.hpp.
#include "web_dossier.hpp"

#include <nlohmann/json.hpp>

#include <wx/process.h>
#include <wx/utils.h>
#include <wx/filename.h>
#include <wx/log.h>

#include <chrono>
#include <cstdio>

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
public:
    ~SidecarProcess() override { Shutdown(); }

    // command must be fully resolved (absolute script path) - no shell, no
    // cwd change (that would break the GUI's relative database paths).
    bool Start(const std::string& command, std::string& error) override {
        if (process_) return true;
        process_ = wxProcess::Open(wxString::FromUTF8(command.c_str()));
        if (!process_) {
            error = "无法启动 sidecar 进程: " + command;
            return false;
        }
        in_ = process_->GetInputStream();     // sidecar stdout
        out_ = process_->GetOutputStream();   // sidecar stdin
        return true;
    }

    bool Call(const std::string& line_request, int timeout_ms, std::string& line_response,
              std::atomic<bool>& cancel) override {
        if (!process_ || !out_ || !in_) return false;
        if (!out_->Write(line_request.c_str(), line_request.size()).IsOk()) return false;
        out_->Write("\n", 1);

        buffer_.clear();
        auto start = std::chrono::steady_clock::now();
        while (true) {
            if (cancel.load()) return false;
            if (in_->CanRead()) {
                char chunk[4096];
                size_t got = in_->Read(chunk, sizeof(chunk)).LastRead();
                if (got == 0 && in_->Eof()) return false;   // sidecar died
                buffer_.append(chunk, got);
                size_t nl = buffer_.find('\n');
                if (nl != std::string::npos) {
                    line_response = buffer_.substr(0, nl);
                    buffer_.erase(0, nl + 1);
                    return !line_response.empty();
                }
            } else {
                wxMilliSleep(25);
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            if (elapsed.count() > timeout_ms) return false;
        }
    }

    void Shutdown() override {
        if (process_) {
            // best effort graceful stop, then detach
            if (out_) {
                out_->Write("{\"op\":\"shutdown\"}\n", 19);
                wxMilliSleep(300);
            }
            delete process_;   // closes pipes; child gets EOF
            process_ = nullptr;
            in_ = nullptr;
            out_ = nullptr;
        }
    }

private:
    wxProcess* process_ = nullptr;
    wxInputStream* in_ = nullptr;
    wxOutputStream* out_ = nullptr;
    std::string buffer_;
};

// ---------------------------------------------------------------------------
// Manager
// ---------------------------------------------------------------------------

Manager::Manager(Database& db, const std::string& script_dir)
    : db_(db), script_dir_(script_dir) {
    set_check_interval_days(atoi(db_.GetConfig("web_dossier_interval_days").c_str()));
}

Manager::Manager(Database& db, const std::string& script_dir,
                 std::unique_ptr<RpcTransport> transport)
    : db_(db), script_dir_(script_dir), sidecar_(std::move(transport)) {
    set_check_interval_days(atoi(db_.GetConfig("web_dossier_interval_days").c_str()));
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
    if (!sidecar_) sidecar_ = std::make_unique<SidecarProcess>();
    std::string python = db_.GetConfig("web_dossier_python");
    if (python.empty()) python = "python3";
    // script_dir_ is the absolute path of tools/web_dossier (resolved by the
    // GUI next to the executable, falling back to the working directory).
    std::string command = python + " \"" + script_dir_ + "/service.py\"";
    if (!sidecar_->Start(command, error)) {
        sidecar_.reset();
        return false;
    }
    std::atomic<bool> no_cancel{false};
    std::string response;
    if (!sidecar_->Call("{\"op\":\"ping\"}", 15000, response, no_cancel)) {
        error = "sidecar 无响应（需要 Python 3.10+ 与 playwright：pip install playwright && "
                "playwright install chromium）";
        sidecar_->Shutdown();
        sidecar_.reset();
        return false;
    }
    try {
        auto parsed = json::parse(response);
        if (parsed.value("op", "") != "pong") {
            error = "sidecar 响应异常: " + response.substr(0, 200);
            return false;
        }
        if (parsed.value("python_ok", false) == false) {
            error = "Python 环境缺少依赖: " + parsed.value("missing", "");
            return false;
        }
    } catch (const std::exception&) {
        error = "sidecar 响应不是 JSON: " + response.substr(0, 200);
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
    if (!ok && cancel && cancel->load()) {
        // The cancelled response may still arrive later. Drop the sidecar so
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
