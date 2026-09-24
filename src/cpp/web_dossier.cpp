// 审查意见网页自动同步 - C++ core implementation. See web_dossier.hpp.
#include "web_dossier.hpp"
#include "web_datasource.hpp"

#include <nlohmann/json.hpp>

#include <wx/process.h>
#include <wx/utils.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/log.h>

#include <chrono>
#include <cstdio>
#include <set>
#include <tuple>

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

class SidecarProcess {
public:
    ~SidecarProcess() { Shutdown(); }

    // command must be fully resolved (absolute script path) - no shell, no
    // cwd change (that would break the GUI's relative database paths).
    bool Start(const std::string& command, std::string& error) {
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
              std::atomic<bool>& cancel) {
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

    void Shutdown() {
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
    : db_(db), script_dir_(script_dir) {}

Manager::~Manager() {
    delete sidecar_;
}

bool Manager::sidecar_running() const { return sidecar_ != nullptr; }

int Manager::check_interval_days() const { return check_interval_days_; }
void Manager::set_check_interval_days(int days) {
    if (days == 1 || days == 3 || days == 7) check_interval_days_ = days;
}

void Manager::PickLatestOfficialEvent(RemoteCaseResult& remote) {
    // 与 Python pick_latest_official_event 对齐：可提醒 ORIGINAL 官方事件中
    // 按（官文日, 次数, 远端ID）取最新。
    static const std::set<std::string> remindable = {
        "OFFICE_ACTION_FIRST", "OFFICE_ACTION_SECOND", "OFFICE_ACTION_NTH",
        "REJECTION_DECISION", "GRANT_NOTICE", "CORRECTION_NOTICE", "OTHER_OFFICIAL"};
    const RemoteDocument* best = nullptr;
    for (const auto& d : remote.documents) {
        if (d.direction != "official" || d.official_date.empty()) continue;
        if (d.document_version != "ORIGINAL" && !d.document_version.empty()) continue;
        if (!remindable.count(d.document_type)) continue;
        if (!best ||
            std::tie(d.official_date, d.oa_ordinal, d.remote_document_id) >
            std::tie(best->official_date, best->oa_ordinal, best->remote_document_id)) {
            best = &d;
        }
    }
    if (best) {
        remote.has_latest_event = true;
        remote.latest_event = *best;
    }
}

bool Manager::EnsureRunning(std::string& error) {
    if (!sidecar_) sidecar_ = new SidecarProcess();
    std::string python = db_.GetConfig("web_dossier_python");
    if (python.empty()) {
        // 便携版布局：<exe 目录>/python/python(.exe) 随包携带、免安装
        wxString exe = wxStandardPaths::Get().GetExecutablePath();
        wxFileName fn(exe);
        std::string dir = fn.GetPath().ToStdString();
        const char* candidates[] = {
#ifdef _WIN32
            "python/python.exe", "python/pythonw.exe",
#else
            "python/bin/python3", "python/bin/python", "python/python3",
#endif
        };
        for (const char* c : candidates) {
            wxFileName full(wxString(dir) + "/" + c);
            if (wxFileExists(full.GetFullPath())) {
                python = full.GetFullPath().ToStdString();
                break;
            }
        }
    }
    if (python.empty()) python = "python3";
    // script_dir_ is the absolute path of tools/web_dossier (resolved by the
    // GUI next to the executable, falling back to the working directory).
    std::string command = python + " \"" + script_dir_ + "/service.py\"";
    if (!sidecar_->Start(command, error)) {
        delete sidecar_;
        sidecar_ = nullptr;
        return false;
    }
    std::atomic<bool> no_cancel{false};
    std::string response;
    if (!sidecar_->Call("{\"op\":\"ping\"}", 15000, response, no_cancel)) {
        error = "sidecar 无响应（需要 Python 3.10+ 与 playwright：pip install playwright && "
                "playwright install chromium）";
        sidecar_->Shutdown();
        delete sidecar_;
        sidecar_ = nullptr;
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
                  std::string& response) {
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
    return sidecar_->Call(req.dump(), timeout, response, no_cancel);
}

bool Manager::EpoCall(const std::string& epo_op, const std::string& publication_number,
                      std::string& response) {
    json args;
    args["epo_op"] = epo_op;
    args["publication_number"] = publication_number;
    args["consumer_key"] = db_.GetConfig("epo_consumer_key");
    args["consumer_secret"] = db_.GetConfig("epo_consumer_secret");
    return Rpc("epo", args.dump(), response);
}

ResultCode Manager::Login(const std::string& provider, std::atomic<bool>& cancel) {
    std::string response;
    if (!Rpc("login", "{\"provider\":\"" + provider + "\"}", response)) {
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
    try {
        auto parsed = json::parse(json_body);
        out.ok = parsed.value("ok", false);
        out.code = parsed.value("code", "SIDECAR_ERROR");
        out.message = parsed.value("message", "");
        out.resolved_application_number = parsed.value("resolved_application_number", "");
        out.auth_state = parsed.value("auth_state", "");
        out.provider_used = parsed.value("provider_used", "");
        if (parsed.contains("attempts") && parsed["attempts"].is_array()) {
            for (const auto& a : parsed["attempts"]) {
                out.attempts.emplace_back(a.value("provider", ""),
                                          a.value("code", ""));
            }
        }
        if (parsed.contains("documents") && parsed["documents"].is_array()) {
            for (const auto& d : parsed["documents"]) {
                RemoteDocument doc;
                doc.document_type = d.value("document_type", "UNKNOWN");
                doc.document_title = d.value("document_title", "");
                doc.raw_title = d.value("raw_title", "");
                doc.official_date = d.value("official_date", "");
                doc.direction = d.value("direction", "");
                doc.remote_document_id = d.value("remote_document_id", "");
                doc.source_url = d.value("source_url", "");
                doc.download_url = d.value("download_url", "");
                doc.download_available = d.value("download_available", false);
                doc.fingerprint = d.value("fingerprint", "");
                doc.confidence = d.value("confidence", "HIGH");
                doc.oa_ordinal = d.value("oa_ordinal", 0);
                doc.ds = d.value("ds", "");
                doc.wenjiandm = d.value("wenjiandm", "");
                doc.source = d.value("source", "");
                doc.document_code = d.value("document_code", "");
                doc.document_version = d.value("document_version", "ORIGINAL");
                doc.event_key = d.value("event_key", "");
                if (d.contains("source_trace") && d["source_trace"].is_array()) {
                    std::string joined;
                    for (const auto& t : d["source_trace"]) {
                        std::string item = t.get<std::string>();
                        if (item.empty()) continue;
                        joined += joined.empty() ? item : "," + item;
                    }
                    doc.source_trace = joined;
                }
                out.documents.push_back(std::move(doc));
            }
        }
        // latest_event is the current field; a legacy sidecar only offered
        // latest_oa (true Office Actions), which still merges as an event
        const char* latest_fields[] = {"latest_event", "latest_oa"};
        for (const char* field : latest_fields) {
            if (parsed.contains(field) && parsed[field].is_object()) {
                const auto& lo = parsed[field];
                out.has_latest_event = true;
                out.latest_event.document_type = lo.value("document_type", "");
                out.latest_event.document_title = lo.value("document_title", "");
                out.latest_event.raw_title = lo.value("raw_title", "");
                out.latest_event.official_date = lo.value("official_date", "");
                out.latest_event.direction = lo.value("direction", "");
                out.latest_event.remote_document_id = lo.value("remote_document_id", "");
                out.latest_event.source_url = lo.value("source_url", "");
                out.latest_event.download_url = lo.value("download_url", "");
                out.latest_event.download_available = lo.value("download_available", false);
                out.latest_event.fingerprint = lo.value("fingerprint", "");
                out.latest_event.confidence = lo.value("confidence", "HIGH");
                out.latest_event.oa_ordinal = lo.value("oa_ordinal", 0);
                out.latest_event.ds = lo.value("ds", "");
                out.latest_event.wenjiandm = lo.value("wenjiandm", "");
                out.latest_event.source = lo.value("source", "");
                out.latest_event.document_code = lo.value("document_code", "");
                out.latest_event.document_version = lo.value("document_version", "ORIGINAL");
                out.latest_event.event_key = lo.value("event_key", "");
                if (lo.contains("source_trace") && lo["source_trace"].is_array()) {
                    std::string joined;
                    for (const auto& t : lo["source_trace"]) {
                        std::string item = t.get<std::string>();
                        if (item.empty()) continue;
                        joined += joined.empty() ? item : "," + item;
                    }
                    out.latest_event.source_trace = joined;
                }
                break;   // latest_event wins; latest_oa is only the fallback
            }
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

CaseSyncReport Manager::ApplyRemoteResult(const Patent& patent, const RemoteCaseResult& remote) {
    CaseSyncReport report;
    report.patent_id = patent.id;
    report.geke_code = patent.geke_code;
    report.provider_used = remote.provider_used.empty() ? "cnipa" : remote.provider_used;
    report.code = ResultCodeFromString(remote.code);
    for (const auto& [provider, code] : remote.attempts) {
        if (code == "RATE_LIMITED") {
            report.rate_limited_upstream = true;
            break;
        }
    }

    long long now = static_cast<long long>(time(nullptr));
    std::string provider = report.provider_used;

    DossierSyncState state;
    state.patent_id = patent.id;
    state.provider = provider;
    state.last_checked_at = now;
    state.auth_state = remote.auth_state;

    // Persist every discovered document with its real source and event_key
    // (dedup happens in SQL: event_key first, legacy fingerprint otherwise).
    std::string app_no = remote.resolved_application_number.empty()
                             ? patent.application_number
                             : remote.resolved_application_number;
    for (const auto& d : remote.documents) {
        ProsecutionDocumentRecord rec;
        rec.patent_id = patent.id;
        rec.jurisdiction = "CN";
        rec.application_number = app_no;
        rec.publication_number = patent.publication_number;
        rec.source = d.source.empty() ? provider : d.source;
        rec.remote_document_id = d.remote_document_id;
        rec.document_type = d.document_type;
        rec.document_title = d.document_title;
        rec.raw_title = d.raw_title;
        rec.official_date = d.official_date;
        rec.direction = d.direction;
        rec.source_url = d.source_url;
        rec.download_url = d.download_url;
        rec.download_available = d.download_available;
        rec.fingerprint = d.fingerprint;
        rec.document_code = d.document_code.empty() ? d.wenjiandm : d.document_code;
        rec.document_version = d.document_version.empty() ? "ORIGINAL" : d.document_version;
        rec.event_key = d.event_key;
        rec.source_trace = d.source_trace;
        bool created = false;
        db_.UpsertProsecutionDocument(rec, &created);
        if (created) report.documents_new++;
    }
    report.documents_total = static_cast<int>(remote.documents.size());

    // Latest official event -> OARecord merge rules (never downloads bodies).
    if (remote.has_latest_event) {
        state.latest_remote_oa_date = remote.latest_event.official_date;
        state.latest_remote_oa_type = remote.latest_event.document_title;
        auto merged = MergeOfficialEvent(db_, patent, remote.latest_event);
        report.code = merged.code;
        report.message = merged.message;
        report.oa_created_id = merged.oa_created_id;
        report.date_conflict = merged.date_conflict;
        report.latest_remote_oa_type = merged.canonical_title;
        report.latest_remote_oa_date = remote.latest_event.official_date;
    } else if (report.code == ResultCode::Ok || report.code == ResultCode::NoChange) {
        report.code = ResultCode::NoChange;
    }

    // Scheduling: verified pages re-check at the configured interval;
    // transient transport trouble retries in 30 minutes so the next
    // background sweep picks the case up again.
    long long next = now + static_cast<long long>(check_interval_days_) * 86400;
    if (report.code == ResultCode::NetworkError || report.code == ResultCode::RateLimited ||
        report.code == ResultCode::PageStructureChanged) {
        next = now + 1800;
    }
    db_.UpdatePatentDossierCheck(patent.id, now, next);

    if (report.code == ResultCode::Ok || report.code == ResultCode::NewOfficialEvent ||
        report.code == ResultCode::NoChange || report.code == ResultCode::DateConflict) {
        state.last_success_at = now;
    } else {
        state.last_error_at = now;
        state.last_error_code = ToString(report.code);
        state.last_error_message = report.message.empty() ? remote.message : report.message;
    }
    db_.UpsertDossierSyncState(state);
    return report;
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

    // 第一层：原生 C++ USPTO Global Dossier（纯 HTTP+JSON，免 Python/免浏览器）。
    // 日常巡检在这一层完成后即返回；只有网络/限流/未收录才走 sidecar 链
    //（EPO/CNIPA，需要便携 Python 时由附加包提供）。
    {
        std::string digits;
        for (char c : patent.application_number) {
            if (c == '.') break;
            if (c >= '0' && c <= '9') digits += c;
        }
        if (digits.size() == 13) digits = digits.substr(0, 12);
        if (digits.size() == 12) {
            uspto::NativeUsptoClient client;
            auto parsed = client.FetchDocuments(digits, patent.application_number,
                                                patent.publication_number);
            if (parsed.code == "OK") {
                RemoteCaseResult remote;
                remote.ok = true;
                remote.code = "OK";
                remote.provider_used = "uspto_global_dossier";
                remote.attempts.emplace_back("uspto_global_dossier", "OK");
                remote.resolved_application_number = patent.application_number;
                remote.documents = std::move(parsed.documents);
                PickLatestOfficialEvent(remote);
                return ApplyRemoteResult(patent, remote);
            }
            // 原生层失败：sidecar 可用则降级，否则如实上报原生结果
            std::string err;
            if (!sidecar_ && !EnsureRunning(err)) {
                RemoteCaseResult remote;
                remote.ok = false;
                remote.code = parsed.code.empty() ? "NETWORK_ERROR" : parsed.code;
                remote.provider_used = "uspto_global_dossier";
                remote.attempts.emplace_back("uspto_global_dossier", remote.code);
                remote.resolved_application_number = patent.application_number;
                return ApplyRemoteResult(patent, remote);
            }
        }
    }

    json args;
    args["application_number"] = patent.application_number;
    args["publication_number"] = patent.publication_number;
    std::string response;
    if (!Rpc("sync_case", args.dump(), response)) {
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
        // Persist the auth state, do not mark the case as failed. The chain
        // already stopped at CNIPA - the login browser only opens when the
        // user explicitly asks for it.
        report.code = ResultCode::AuthRequired;
        for (const auto& [provider, code] : remote.attempts) {
            if (code == "RATE_LIMITED") report.rate_limited_upstream = true;
        }
        report.provider_used = remote.provider_used.empty() ? "cnipa" : remote.provider_used;
        report.message = remote.provider_used == "cnipa" || remote.provider_used.empty()
                             ? "CNIPA 需要登录"
                             : "数据源需要登录";
        DossierSyncState state;
        state.patent_id = patent.id;
        state.provider = report.provider_used;
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

    // the settings dialog only offers these three; anything else falls back
    // to the daily default
    int interval = atoi(db_.GetConfig("web_dossier_interval_days").c_str());
    if (interval == 1 || interval == 3 || interval == 7) check_interval_days_ = interval;
    else check_interval_days_ = 1;

    int index = 0;
    for (const auto& p : patents) {
        if (cancel.load()) break;
        if (progress && !progress(index, summary.total, p.geke_code)) break;
        index++;

        CaseSyncReport r = SyncCase(p, cancel);
        summary.checked++;
        switch (r.code) {
            case ResultCode::NewOfficialEvent:
            case ResultCode::DateConflict:
                summary.new_events++;
                summary.findings.push_back(r);
                break;
            case ResultCode::NoChange:
            case ResultCode::Ok:
                summary.no_change++;
                break;
            case ResultCode::ManualReviewRequired:
                summary.manual_review++;
                summary.findings.push_back(r);
                break;
            case ResultCode::AuthRequired:
            case ResultCode::SessionExpired:
                summary.auth_required++;
                summary.findings.push_back(r);
                return summary;   // stop the whole batch, don't fail the rest
            case ResultCode::Cancelled:
                return summary;
            default:
                summary.failed++;
                summary.failures.push_back(r);
                if (r.rate_limited_upstream || IsBatchAbortingCode(r.code)) return summary;
                break;
        }
    }
    return summary;
}

} // namespace webdossier
