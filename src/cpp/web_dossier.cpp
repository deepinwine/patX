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
    if (days >= 1 && days <= 365) check_interval_days_ = days;
}

bool Manager::EnsureRunning(std::string& error) {
    if (!sidecar_) sidecar_ = new SidecarProcess();
    std::string python = db_.GetConfig("web_dossier_python");
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
                out.documents.push_back(std::move(doc));
            }
        }
        if (parsed.contains("latest_oa") && parsed["latest_oa"].is_object()) {
            const auto& lo = parsed["latest_oa"];
            out.has_latest_oa = true;
            out.latest_oa.document_type = lo.value("document_type", "");
            out.latest_oa.document_title = lo.value("document_title", "");
            out.latest_oa.raw_title = lo.value("raw_title", "");
            out.latest_oa.official_date = lo.value("official_date", "");
            out.latest_oa.direction = lo.value("direction", "");
            out.latest_oa.remote_document_id = lo.value("remote_document_id", "");
            out.latest_oa.source_url = lo.value("source_url", "");
            out.latest_oa.download_url = lo.value("download_url", "");
            out.latest_oa.download_available = lo.value("download_available", false);
            out.latest_oa.fingerprint = lo.value("fingerprint", "");
            out.latest_oa.confidence = lo.value("confidence", "HIGH");
            out.latest_oa.oa_ordinal = lo.value("oa_ordinal", 0);
            out.latest_oa.ds = lo.value("ds", "");
            out.latest_oa.wenjiandm = lo.value("wenjiandm", "");
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
    report.code = ResultCodeFromString(remote.code);

    long long now = static_cast<long long>(time(nullptr));
    long long next = now + static_cast<long long>(check_interval_days_) * 86400;
    db_.UpdatePatentDossierCheck(patent.id, now, next);

    DossierSyncState state;
    state.patent_id = patent.id;
    state.provider = "cnipa";
    state.last_checked_at = now;
    state.auth_state = remote.auth_state;

    // Persist every discovered document (fingerprint dedup happens in SQL).
    std::string app_no = remote.resolved_application_number.empty()
                             ? patent.application_number
                             : remote.resolved_application_number;
    int latest_oa_doc_id = 0;
    for (const auto& d : remote.documents) {
        ProsecutionDocumentRecord rec;
        rec.patent_id = patent.id;
        rec.jurisdiction = "CN";
        rec.application_number = app_no;
        rec.publication_number = patent.publication_number;
        rec.source = "cnipa";
        rec.remote_document_id = d.remote_document_id;
        rec.document_type = d.document_type;
        rec.document_title = d.document_title;
        rec.official_date = d.official_date;
        rec.direction = d.direction;
        rec.source_url = d.source_url;
        rec.download_url = d.download_url;
        rec.download_available = d.download_available;
        rec.fingerprint = d.fingerprint;
        bool created = false;
        int doc_row = db_.UpsertProsecutionDocument(rec, &created);
        if (created) report.documents_new++;
        if (remote.has_latest_oa && !remote.latest_oa.fingerprint.empty() &&
            remote.latest_oa.fingerprint == d.fingerprint) {
            latest_oa_doc_id = doc_row;   // remember for the download step
        }
    }
    report.documents_total = static_cast<int>(remote.documents.size());

    // Latest true Office Action -> OARecord merge rules.
    if (remote.has_latest_oa) {
        const RemoteDocument& oa_doc = remote.latest_oa;
        state.latest_remote_oa_date = oa_doc.official_date;
        state.latest_remote_oa_type = oa_doc.document_title;

        bool is_oa_family = oa_doc.document_type.rfind("OFFICE_ACTION_", 0) == 0;
        if (!is_oa_family) {
            // Sidecar only reports true OAs here; anything else is a bug on
            // its side - record, don't touch OARecords.
            report.message = "latest_oa 不是审查意见类型: " + oa_doc.document_type;
        } else if (oa_doc.confidence != "HIGH") {
            report.code = ResultCode::ManualReviewRequired;
            report.message = "置信度 " + oa_doc.confidence + "，待人工确认: " +
                             oa_doc.document_title + " @ " + oa_doc.official_date;
        } else if (oa_doc.official_date.empty()) {
            report.code = ResultCode::DateParseFailed;
            report.message = "最新审查意见缺少可解析的官文日: " + oa_doc.document_title;
        } else {
            std::string canonical = NormalizeOaTypeCn(oa_doc.document_title);
            auto existing = db_.GetOAsForPatentId(patent.id);
            report.latest_remote_oa_type = canonical;
            report.latest_remote_oa_date = oa_doc.official_date;

            const OARecord* same_type = nullptr;
            const OARecord* same_date = nullptr;
            for (const auto& e : existing) {
                std::string e_canonical = NormalizeOaTypeCn(e.oa_type);
                if (e_canonical == canonical && IsOfficeActionTypeCn(e_canonical)) same_type = &e;
                if (!e.issue_date.empty() && e.issue_date == oa_doc.official_date &&
                    (IsOfficeActionTypeCn(e_canonical) || e.oa_type.empty())) {
                    same_date = &e;
                }
            }

            if (same_date) {
                // Already recorded (matched by exact date).
                if (same_type && same_type->issue_date.empty()) {
                    db_.UpdateOASyncFields(same_type->id, oa_doc.official_date, "auto_filled_date");
                    report.message = "已补入官文日 " + oa_doc.official_date;
                } else {
                    report.code = ResultCode::NoChange;
                }
            } else if (same_type) {
                if (same_type->issue_date.empty()) {
                    db_.UpdateOASyncFields(same_type->id, oa_doc.official_date, "auto_filled_date");
                    report.code = ResultCode::NoChange;
                    report.message = "已补入官文日 " + oa_doc.official_date;
                } else {
                    // Same OA ordinal, different local date: never overwrite.
                    db_.UpdateOASyncFields(same_type->id, "", "date_conflict");
                    report.code = ResultCode::DateConflict;
                    report.date_conflict = true;
                    report.message = "本地 " + canonical + " 官文日 " + same_type->issue_date +
                                     "，官网 " + oa_doc.official_date + "，待人工确认";
                }
            } else {
                OARecord oa;
                oa.patent_id = patent.id;
                oa.geke_code = patent.geke_code;
                oa.patent_title = patent.title;
                oa.oa_type = canonical;
                oa.issue_date = oa_doc.official_date;
                oa.source = "cnipa";
                oa.remote_document_id = oa_doc.remote_document_id;
                oa.sync_flag = "web_new";
                int id = db_.InsertOA(oa, /*log_undo=*/true);
                if (id > 0) {
                    report.code = ResultCode::NewOfficeAction;
                    report.oa_created_id = id;
                    report.message = "发现新的审查意见: " + canonical + " @ " + oa_doc.official_date;

                    // Phase 2: fetch the notice itself. Strictly optional -
                    // the OA is already recorded; a download failure only
                    // annotates the report.
                    if (!oa_doc.remote_document_id.empty()) {
                        std::string folder = db_.GetConfig("web_dossier_folder");
                        if (folder.empty()) folder = "data/dossiers/CN";
                        json dl_args;
                        dl_args["provider"] = "cnipa";
                        dl_args["application_number"] = app_no;
                        dl_args["rid"] = oa_doc.remote_document_id;
                        dl_args["ds"] = oa_doc.ds.empty() ? "TZS" : oa_doc.ds;
                        dl_args["wenjiandm"] = oa_doc.wenjiandm.empty() ? "100000" : oa_doc.wenjiandm;
                        dl_args["official_date"] = oa_doc.official_date;
                        dl_args["title"] = canonical;
                        dl_args["dest_dir"] = folder;
                        std::string dl_resp;
                        if (Rpc("download_document", dl_args.dump(), dl_resp)) {
                            try {
                                auto dl = json::parse(dl_resp);
                                if (dl.value("ok", false)) {
                                    report.downloaded_path = dl.value("saved_path", "");
                                    if (latest_oa_doc_id > 0 && !report.downloaded_path.empty()) {
                                        db_.UpdateProsecutionDocumentDownload(
                                            latest_oa_doc_id, report.downloaded_path);
                                    }
                                    report.message += "，PDF已下载: " + report.downloaded_path;
                                } else {
                                    report.message += "（PDF下载失败: " +
                                                      dl.value("code", "") + " " +
                                                      dl.value("message", "") + "）";
                                }
                            } catch (...) {
                                report.message += "（PDF下载响应解析失败）";
                            }
                        } else {
                            report.message += "（PDF下载未执行：sidecar 通信失败）";
                        }
                    }
                }
            }
        }
    } else if (report.code == ResultCode::Ok || report.code == ResultCode::NoChange) {
        report.code = ResultCode::NoChange;
    }

    if (report.code == ResultCode::Ok || report.code == ResultCode::NewOfficeAction ||
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

    json args;
    args["provider"] = "cnipa";
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

    int interval = atoi(db_.GetConfig("web_dossier_interval_days").c_str());
    if (interval >= 1 && interval <= 365) check_interval_days_ = interval;

    int index = 0;
    for (const auto& p : patents) {
        if (cancel.load()) break;
        if (progress && !progress(index, summary.total, p.geke_code)) break;
        index++;

        CaseSyncReport r = SyncCase(p, cancel);
        summary.checked++;
        switch (r.code) {
            case ResultCode::NewOfficeAction:
            case ResultCode::DateConflict:
                summary.new_oa++;
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
                if (IsBatchAbortingCode(r.code)) return summary;
                break;
        }
    }
    return summary;
}

} // namespace webdossier
