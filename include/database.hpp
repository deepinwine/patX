// patX Database - SQLite wrapper for patent/IP data.
//
// All modules (Domestic/OA/PCT/Software/IC/Foreign + USPTO repositories) share
// this connection. Schema changes go through the versioned migration system in
// migration.cpp - never ALTER/DROP ad hoc from feature code.
#pragma once

#include <string>
#include <vector>
#include <memory>
#include <map>
#include <ctime>
#include <sqlite3.h>

// ---------------------------------------------------------------------------
// Data models
// ---------------------------------------------------------------------------

struct Patent {
    int id = 0;
    std::string geke_code;
    std::string application_number;
    // CN 公开号 (e.g. CN119870049A); used to resolve the application number
    // on the dossier site when application_number is empty.
    std::string publication_number;
    std::string title;
    std::string proposal_name;
    std::string application_status;
    std::string patent_type;
    std::string patent_level;
    std::string application_date;
    std::string authorization_date;
    std::string expiration_date;
    std::string geke_handler;
    std::string rd_department;
    std::string agency_firm;
    std::string original_applicant;
    std::string current_applicant;
    std::string inventor;
    std::string notes;
    std::string class_level1;
    std::string class_level2;
    std::string class_level3;
    // Structured fields migrated out of notes (schema v2). Excel columns that
    // users filter/sort on live here instead of being concatenated into notes.
    std::string related_case_info;
    std::string fee_status;
    std::string rd_project;
    std::string class_level4;
    std::string tags;
    std::string details;
    std::string filing_date;
    std::string disclosure_writer;
    std::string agent_code;
    std::string agent_name;
    std::string intangible_asset_eval;
    std::string internal_rd_project;
    std::string technology_route;
    std::string project_id;
    std::string oa_reminder_1;
    std::string oa_reminder_2;
    std::string oa_reminder_3;
    std::string oa_reminder_4;
    std::string oa_reminder_5;
    std::string reexamination;
    std::string pudong_subsidy;
    std::string pct_reminder;
    long long updated_at = 0; // unix seconds, maintained on every write (NAS sync conflict detection)
};

struct OARecord {
    int id = 0;
    int patent_id = 0;
    std::string geke_code;
    std::string patent_title;
    std::string oa_type;
    std::string official_deadline;
    std::string issue_date;
    std::string response_date;
    std::string handler;
    std::string writer;
    std::string progress;
    std::string agency;
    std::string oa_summary;
    bool is_completed = false;
    bool is_extendable = false;
    bool extension_requested = false;
    int extension_months = 0;
    std::string extended_deadline;
    std::string notes;
    // External linkage (USPTO sync). jurisdiction='US', source='USPTO' for
    // records created by the sync service; empty for manually created records.
    std::string jurisdiction;
    std::string source;
    std::string external_case_id;      // uspto_cases.id
    std::string external_document_id;  // uspto_documents.document_identifier
    std::string deadline_source;       // official / calculated / manual / ''
    // Web dossier sync linkage (source='cnipa'). Filled by the sync layer
    // only; the manual edit dialogs never touch these.
    std::string remote_document_id;
    std::string sync_flag;              // "" / web_new / date_conflict / auto_filled_date
};

struct PCTPatent {
    int id = 0;
    std::string geke_code;
    std::string domestic_source;
    std::string application_no;
    std::string country_app_no;
    std::string title;
    std::string application_status;
    std::string handler;
    std::string inventor;
    std::string filing_date;
    std::string application_date;
    std::string priority_date;
    std::string country;
    std::string notes;
};

struct SoftwareCopyright {
    int id = 0;
    std::string case_no;
    std::string reg_no;
    std::string title;
    std::string original_owner;
    std::string current_owner;
    std::string application_status;
    std::string handler;
    std::string developer;
    std::string inventor;
    std::string dev_complete_date;
    std::string application_date;
    std::string reg_date;
    std::string version;
    std::string notes;
};

struct ICLayout {
    int id = 0;
    std::string case_no;
    std::string reg_no;
    std::string title;
    std::string original_owner;
    std::string current_owner;
    std::string application_status;
    std::string handler;
    std::string designer;
    std::string inventor;
    std::string application_date;
    std::string creation_date;
    std::string cert_date;
    std::string notes;
};

struct ForeignPatent {
    int id = 0;
    std::string case_no;
    std::string pct_no;
    std::string country_app_no;
    std::string title;
    std::string owner;
    std::string patent_status;
    std::string handler;
    std::string inventor;
    std::string application_date;
    std::string authorization_date;
    std::string country;
    std::string application_no;
    std::string notes;
};

// Deadline calculation rule. Rows live in the deadline_rules table and are
// fully editable at runtime; the seeded rows are just defaults.
struct DeadlineRule {
    int id = 0;
    std::string jurisdiction;      // CN / US / PCT / ...
    std::string event_type;        // oa_response / national_phase_entry / ...
    std::string rule_description;
    int base_months = 0;
    int base_days = 0;
    bool extendable = false;
    int max_extension_months = 0;
    std::string effective_from;
    std::string effective_to;
    bool enabled = true;
    std::string notes;
};

// Unified query filter shared by every module listing. Fields that don't apply
// to a module are ignored by that module's query. The old per-module filter
// signatures (status/handler strings) remain as thin wrappers for compatibility.
struct QueryFilter {
    std::string keyword;    // LIKE across the module's searchable columns
    std::string status;
    std::string handler;
    std::string level;      // patents only
    std::string oa_type;    // OA only
    std::string writer;     // OA only
    std::string progress;   // OA only
    std::string country;    // PCT / Foreign
    std::string examiner;   // US prosecution
    std::string art_unit;   // US prosecution
    std::string sync_status;// US prosecution
    // OA deadline state: "" all / "incomplete" / "completed" / "due5" / "due30"
    std::string deadline_state;
};

// One document discovered on an official prosecution-dossier website
// (CNIPA etc.). Populated by the web dossier sync; the Python sidecar finds
// them, this C++ side persists them. Fingerprint dedup keeps re-syncs cheap.
struct ProsecutionDocumentRecord {
    int id = 0;
    int patent_id = 0;
    std::string jurisdiction;         // CN / US / ...
    std::string application_number;
    std::string publication_number;
    std::string source;               // provider id, e.g. "cnipa"
    std::string remote_document_id;   // stable id when the site offers one
    std::string document_type;        // OFFICE_ACTION_SECOND / GRANT_NOTICE / ...
    std::string document_title;       // normalized title
    std::string official_date;        // YYYY-MM-DD ("" when unknown)
    std::string direction;            // official / applicant
    std::string source_url;
    std::string download_url;
    bool download_available = false;
    std::string fingerprint;          // sha256(jurisdiction+app+title+date+id)
    long long first_seen_at = 0;      // unix epoch
    long long last_seen_at = 0;
    std::string raw_metadata;         // small JSON blob, never credentials
};

// Per-patent sync bookkeeping for one provider.
struct DossierSyncState {
    int patent_id = 0;
    std::string provider;             // "cnipa"
    long long last_checked_at = 0;
    long long last_success_at = 0;
    long long last_error_at = 0;
    std::string last_error_code;      // AUTH_REQUIRED / PAGE_STRUCTURE_CHANGED / ...
    std::string last_error_message;
    std::string latest_remote_oa_date;   // YYYY-MM-DD
    std::string latest_remote_oa_type;
    std::string auth_state;           // NOT_INITIALIZED / AUTHENTICATED / ...
};

class Database {
public:
    explicit Database(const std::string& db_path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool IsOpen() const { return db_ != nullptr; }
    sqlite3* GetHandle() { return db_; }
    const std::string& path() const { return db_path_; }
    int SchemaVersion() const { return schema_version_; }
    std::string LastError() const { return last_error_; }

    // ---------- Patents ----------
    std::vector<Patent> GetPatents(const QueryFilter& filter = {});
    Patent GetPatentById(int id);
    Patent GetPatentByCode(const std::string& geke_code);
    int InsertPatent(const Patent& p, bool log_undo = true);
    bool UpdatePatent(int id, const Patent& p, bool log_undo = true);
    bool DeletePatent(int id, bool log_undo = true);
    std::vector<Patent> SearchPatents(const std::string& keyword);

    // ---------- OA Records ----------
    std::vector<OARecord> GetOARecords(const QueryFilter& filter = {});
    OARecord GetOAById(int id);
    std::vector<OARecord> GetOAByPatent(const std::string& geke_code);
    int InsertOA(const OARecord& oa, bool log_undo = true);
    bool UpdateOA(int id, const OARecord& oa, bool log_undo = true);
    bool DeleteOA(int id, bool log_undo = true);
    bool MarkOACompleted(int id);
    // Finds a synced OA record by its USPTO document identifier (0 if absent).
    int FindOAByExternalDocument(const std::string& external_document_id);

    // ---------- PCT ----------
    std::vector<PCTPatent> GetPCTPatents(const QueryFilter& filter = {});
    PCTPatent GetPCTById(int id);
    int InsertPCT(const PCTPatent& p);
    bool UpdatePCT(int id, const PCTPatent& p);
    bool DeletePCT(int id);

    // ---------- Software ----------
    std::vector<SoftwareCopyright> GetSoftwareCopyrights(const QueryFilter& filter = {});
    SoftwareCopyright GetSoftwareById(int id);
    int InsertSoftware(const SoftwareCopyright& s);
    bool UpdateSoftware(int id, const SoftwareCopyright& s);
    bool DeleteSoftware(int id);

    // ---------- IC Layouts ----------
    std::vector<ICLayout> GetICLayouts(const QueryFilter& filter = {});
    ICLayout GetICById(int id);
    int InsertIC(const ICLayout& ic);
    bool UpdateIC(int id, const ICLayout& ic);
    bool DeleteIC(int id);

    // ---------- Foreign ----------
    std::vector<ForeignPatent> GetForeignPatents(const QueryFilter& filter = {});
    ForeignPatent GetForeignById(int id);
    int InsertForeign(const ForeignPatent& f);
    bool UpdateForeign(int id, const ForeignPatent& f);
    bool DeleteForeign(int id);
    // Find a US-case candidate: country in (US/USA/United States) matching by
    // application number. Returns 0 when there is no unambiguous candidate.
    int FindUSCaseCandidate(const std::string& application_number);

    // ---------- Deadline rules ----------
    std::vector<DeadlineRule> GetDeadlineRules(bool enabled_only = false);
    bool InsertDeadlineRule(const DeadlineRule& rule, int* new_id = nullptr);
    bool UpdateDeadlineRule(const DeadlineRule& rule);
    bool DeleteDeadlineRule(int id);
    // Computes a suggested deadline from a rule (base date + months/days).
    // Returns "" when no enabled rule matches the jurisdiction/event type.
    std::string CalculateDeadline(const std::string& jurisdiction,
                                  const std::string& event_type,
                                  const std::string& base_date) const;

    // ---------- Utility ----------
    std::vector<std::string> GetDistinctValues(const std::string& table, const std::string& column);
    void SetConfig(const std::string& key, const std::string& value);
    std::string GetConfig(const std::string& key);
    std::string GetCurrentDate();
    // Copy the database file (safe point-in-time backup using SQLite's
    // backup API; works while the connection is open).
    bool BackupTo(const std::string& dest_path);

    // ---------- Web dossier sync (CNIPA 网页审查信息同步) ----------
    // Cases worth checking: active prosecution statuses first. Terminal
    // statuses (放弃/失效/撤回/视撤/终止) are skipped; granted cases only
    // when include_granted (they are checked on the slow cycle).
    std::vector<Patent> GetPatentsForDossierCheck(bool include_granted, int limit = 0);
    // Dedup key: (source, application_number, fingerprint). Sets *created
    // when the row is new; otherwise only last_seen_at is refreshed.
    int UpsertProsecutionDocument(ProsecutionDocumentRecord& doc, bool* created = nullptr);
    bool UpdatePatentDossierCheck(int patent_id, long long last_at, long long next_at);
    bool UpsertDossierSyncState(const DossierSyncState& state);
    std::vector<DossierSyncState> GetDossierSyncStates(int limit = 200);
    // OA matching helper for the sync rules: existing OA of this patent whose
    // canonical type equals canonical_type and issue_date is empty (date can
    // be filled in), or whose date differs (DATE_CONFLICT candidate).
    std::vector<OARecord> GetOAsForPatentId(int patent_id);
    // Targeted sync update: fills issue_date only when currently empty and
    // sets sync_flag - never touches handler/writer/deadline/notes fields.
    bool UpdateOASyncFields(int oa_id, const std::string& issue_date_if_empty,
                            const std::string& sync_flag);

    // ---------- Undo ----------
    void BeginBatch();
    int Undo();
    bool CanUndo() const;

    // SQL helpers used by the USPTO repositories as well
    bool Execute(const std::string& sql);
    std::string EscapeString(const std::string& s);
    // Escapes LIKE wildcards in a user keyword and wraps it for LIKE.
    std::string LikePattern(const std::string& keyword);

private:
    sqlite3* db_ = nullptr;
    std::string db_path_;
    int schema_version_ = 0;
    std::string last_error_;

    void InitTables();
    void MigrateTables();

    // JSON serialization for undo snapshots (full-field, so undo is lossless)
    std::string PatentToJson(const Patent& p);
    std::string OAToJson(const OARecord& oa);
};

// Global undo manager accessor
class UndoManager;
UndoManager& GetUndoManager();
