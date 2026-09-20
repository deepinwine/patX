// 审查意见网页自动同步 / Web Dossier Sync - C++ core.
//
// Owns the Python sidecar process (tools/web_dossier/service.py) over a
// stdin/stdout JSON-RPC channel, builds the per-case sync queue, and applies
// the remote results to the local database under strict merge rules:
//
//   - new Office Actions are INSERTed only from HIGH-confidence parses
//   - an existing OA with an empty issue_date gets the date filled in
//   - an existing OA with a DIFFERENT date is flagged DATE_CONFLICT and
//     never overwritten
//   - handler / writer / response_date / oa_summary / notes / manual
//     deadlines are never touched by the sync
//
// The sidecar does the browser work (Playwright, user-logged-in persistent
// profile, human-paced queries). All database writes happen here, on the
// GUI's Database connection - one writer, no sidecar DB access.
#pragma once

#include "database.hpp"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace webdossier {

// Result codes shared with the Python sidecar (keep in sync with
// tools/web_dossier/models.py).
enum class ResultCode {
    Ok = 0,
    NoChange,
    NewOfficeAction,
    AuthRequired,
    SessionExpired,
    CaseNotFound,
    PublicationNotAvailable,
    AccessDenied,
    ResolveFailed,
    PageStructureChanged,
    RateLimited,
    TemporaryError,
    NetworkError,
    DateParseFailed,
    DateConflict,
    UnsupportedJurisdiction,
    ManualReviewRequired,
    SidecarError,        // sidecar missing / crashed / bad JSON
    Cancelled,
};

const char* ToString(ResultCode c);
ResultCode ResultCodeFromString(const std::string& name);

// One discovered remote document, as reported by the sidecar.
struct RemoteDocument {
    std::string document_type;       // OFFICE_ACTION_SECOND / ...
    std::string document_title;      // normalized
    std::string raw_title;
    std::string official_date;       // YYYY-MM-DD or ""
    std::string direction;           // official / applicant
    std::string remote_document_id;
    std::string source_url;
    std::string download_url;
    bool download_available = false;
    std::string fingerprint;
    std::string confidence;          // HIGH / MEDIUM / LOW
    int oa_ordinal = 0;              // 1/2/3.. for 第N次审查意见通知书
};

struct CaseSyncReport {
    int patent_id = 0;
    std::string geke_code;
    std::string identifier_used;     // application or publication number
    ResultCode code = ResultCode::Ok;
    std::string message;             // human-readable, for the sync log UI
    std::string latest_remote_oa_type;
    std::string latest_remote_oa_date;
    int documents_total = 0;
    int documents_new = 0;           // newly stored prosecution_documents
    int oa_created_id = 0;           // oa_records.id when a new OA was inserted
    bool date_conflict = false;      // existing OA flagged, needs the user
};

struct BatchSummary {
    int total = 0;
    int checked = 0;
    int new_oa = 0;
    int no_change = 0;
    int auth_required = 0;
    int failed = 0;
    int manual_review = 0;           // MEDIUM/LOW confidence, not applied
    std::vector<CaseSyncReport> findings;   // new OA + conflicts
    std::vector<CaseSyncReport> failures;
};

// Chinese OA type normalization: "二通"/"第二次审查意见通知书"/"第2次审查意见
// 通知书" -> "第二次审查意见通知书"; returns the input unchanged when it is
// not an OA-family type.
std::string NormalizeOaTypeCn(const std::string& raw);
bool IsOfficeActionTypeCn(const std::string& normalized);
// OA ordinal (第一次->1, 第二次->2, 第3次->3); 0 when unknown.
int OaTypeOrdinalCn(const std::string& raw);

class SidecarProcess;   // pimpl - hides wxProcess from this header

class Manager {
public:
    // db must outlive the manager. script_dir is the ABSOLUTE path of the
    // tools/web_dossier directory (the GUI resolves it next to the
    // executable or the cwd).
    Manager(Database& db, const std::string& script_dir);
    ~Manager();

    // Spawns the sidecar if needed and checks it answers "ping".
    // error gets a user-actionable message on failure (python missing,
    // playwright missing, script not found, ...).
    bool EnsureRunning(std::string& error);

    // Opens the headful browser for the user to log in and waits (minutes)
    // until the provider reports an authenticated session. cancel flag lets
    // the GUI abort the wait.
    ResultCode Login(const std::string& provider, std::atomic<bool>& cancel);

    // Syncs one case end-to-end (resolve -> query -> apply rules).
    CaseSyncReport SyncCase(const Patent& patent, std::atomic<bool>& cancel);

    // Syncs the active-case queue. progress(case_index, total, geke_code) is
    // called before each case; returning false (or cancel) stops the batch.
    BatchSummary SyncAll(bool include_granted, int limit,
                         const std::function<bool(int, int, const std::string&)>& progress,
                         std::atomic<bool>& cancel);

    int check_interval_days() const;
    void set_check_interval_days(int days);

    bool sidecar_running() const;

private:
    // Performs one JSON-RPC round trip. Returns false on transport trouble.
    bool Rpc(const std::string& op, const std::string& json_payload, std::string& response);

    struct RemoteCaseResult {
        bool ok = false;
        std::string code;                    // result-code name from sidecar
        std::string message;
        std::string resolved_application_number;
        std::string auth_state;
        std::vector<RemoteDocument> documents;
        // Latest true Office Action (only OFFICE_ACTION_* types):
        bool has_latest_oa = false;
        RemoteDocument latest_oa;
    };

    CaseSyncReport ApplyRemoteResult(const Patent& patent, const RemoteCaseResult& remote);
    bool ParseCaseResult(const std::string& json_body, RemoteCaseResult& out);

    Database& db_;
    std::string script_dir_;
    SidecarProcess* sidecar_ = nullptr;
    int check_interval_days_ = 7;
};

} // namespace webdossier
