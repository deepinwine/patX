// patX Database Implementation
//
// All SQL for the core IP modules (patents / OA / PCT / software / IC /
// foreign / deadline rules) lives here; the web dossier OA merge rules live
// in web_dossier_rules.cpp.
#include "database.hpp"
#include "undo_manager.hpp"
#include "patx/schema_migrations.hpp"
#include "patx/log.hpp"

#include <sstream>
#include <iostream>
#include <cstring>
#include <set>

static UndoManager* g_undo_manager = nullptr;

UndoManager& GetUndoManager() {
    return *g_undo_manager;
}

Database::Database(const std::string& db_path) : db_path_(db_path) {
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        last_error_ = db_ ? sqlite3_errmsg(db_) : "sqlite3_open failed";
        PATX_LOG_ERROR(std::string("Cannot open database: ") + last_error_ + " (" + db_path + ")");
        db_ = nullptr;
        return;
    }

    // Enable WAL mode for better performance
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA cache_size=10000;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);

    // Initialize undo manager
    g_undo_manager = new UndoManager();
    g_undo_manager->SetDatabase(db_);

    // Create tables (idempotent, creates the current schema shape)
    InitTables();

    // Bring an existing database up to the current schema version. This is
    // transactional and backs up the file first - see schema_migrations.cpp.
    auto migration = patx::RunSchemaMigrations(db_, db_path_);
    schema_version_ = patx::ReadSchemaVersion(db_);
    if (!migration.ok) {
        last_error_ = migration.error;
        PATX_LOG_ERROR("Schema migration failed: " + migration.error);
    } else if (migration.to_version > migration.from_version) {
        PATX_LOG_INFO("Database schema is now v" + std::to_string(migration.to_version));
    }
}

Database::~Database() {
    delete g_undo_manager;
    g_undo_manager = nullptr;
    if (db_) {
        sqlite3_close(db_);
    }
}

void Database::InitTables() {
    Execute(R"(
        CREATE TABLE IF NOT EXISTS patents (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            geke_code TEXT UNIQUE,
            application_number TEXT,
            title TEXT,
            proposal_name TEXT,
            application_status TEXT,
            patent_type TEXT,
            patent_level TEXT,
            application_date TEXT,
            authorization_date TEXT,
            expiration_date TEXT,
            geke_handler TEXT,
            rd_department TEXT,
            agency_firm TEXT,
            original_applicant TEXT,
            current_applicant TEXT,
            inventor TEXT,
            notes TEXT,
            class_level1 TEXT,
            class_level2 TEXT,
            class_level3 TEXT,
            updated_at INTEGER DEFAULT 0,
            related_case_info TEXT,
            fee_status TEXT,
            rd_project TEXT,
            class_level4 TEXT,
            tags TEXT,
            details TEXT,
            filing_date TEXT,
            disclosure_writer TEXT,
            agent_code TEXT,
            agent_name TEXT,
            intangible_asset_eval TEXT,
            internal_rd_project TEXT,
            technology_route TEXT,
            project_id TEXT,
            oa_reminder_1 TEXT,
            oa_reminder_2 TEXT,
            oa_reminder_3 TEXT,
            oa_reminder_4 TEXT,
            oa_reminder_5 TEXT,
            reexamination TEXT,
            pudong_subsidy TEXT,
            pct_reminder TEXT,
            publication_number TEXT,
            last_dossier_check_at INTEGER DEFAULT 0,
            next_dossier_check_at INTEGER DEFAULT 0
        )
    )");

    Execute(R"(
        CREATE TABLE IF NOT EXISTS oa_records (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            patent_id INTEGER,
            geke_code TEXT,
            patent_title TEXT,
            oa_type TEXT,
            official_deadline TEXT,
            issue_date TEXT,
            response_date TEXT,
            handler TEXT,
            writer TEXT,
            progress TEXT,
            agency TEXT,
            oa_summary TEXT,
            is_completed INTEGER DEFAULT 0,
            is_extendable INTEGER DEFAULT 0,
            extension_requested INTEGER DEFAULT 0,
            extension_months INTEGER,
            extended_deadline TEXT,
            notes TEXT,
            jurisdiction TEXT DEFAULT '',
            source TEXT DEFAULT '',
            deadline_source TEXT DEFAULT '',
            remote_document_id TEXT DEFAULT '',
            sync_flag TEXT DEFAULT ''
        )
    )");

    Execute(R"(
        CREATE TABLE IF NOT EXISTS pct_patents (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            geke_code TEXT UNIQUE,
            domestic_source TEXT,
            application_no TEXT,
            country_app_no TEXT,
            title TEXT,
            application_status TEXT,
            handler TEXT,
            inventor TEXT,
            filing_date TEXT,
            application_date TEXT,
            priority_date TEXT,
            country TEXT,
            notes TEXT
        )
    )");

    Execute(R"(
        CREATE TABLE IF NOT EXISTS software_copyrights (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            case_no TEXT UNIQUE,
            reg_no TEXT,
            title TEXT,
            original_owner TEXT,
            current_owner TEXT,
            application_status TEXT,
            handler TEXT,
            developer TEXT,
            inventor TEXT,
            dev_complete_date TEXT,
            application_date TEXT,
            reg_date TEXT,
            version TEXT,
            notes TEXT
        )
    )");

    Execute(R"(
        CREATE TABLE IF NOT EXISTS ic_layouts (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            case_no TEXT UNIQUE,
            reg_no TEXT,
            title TEXT,
            original_owner TEXT,
            current_owner TEXT,
            application_status TEXT,
            handler TEXT,
            designer TEXT,
            inventor TEXT,
            application_date TEXT,
            creation_date TEXT,
            cert_date TEXT,
            notes TEXT
        )
    )");

    Execute(R"(
        CREATE TABLE IF NOT EXISTS foreign_patents (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            case_no TEXT UNIQUE,
            pct_no TEXT,
            country_app_no TEXT,
            title TEXT,
            owner TEXT,
            patent_status TEXT,
            handler TEXT,
            inventor TEXT,
            application_date TEXT,
            authorization_date TEXT,
            country TEXT,
            application_no TEXT,
            notes TEXT
        )
    )");

    Execute(R"(
        CREATE TABLE IF NOT EXISTS annual_fees (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            patent_id INTEGER,
            geke_code TEXT,
            patent_title TEXT,
            patent_type TEXT,
            fee_year INTEGER,
            fee_amount TEXT,
            fee_period_end TEXT,
            grace_period_end TEXT,
            is_paid INTEGER DEFAULT 0,
            payment_date TEXT,
            notes TEXT,
            jurisdiction TEXT DEFAULT 'CN',
            application_date TEXT DEFAULT ''
        )
    )");

    Execute(R"(
        CREATE TABLE IF NOT EXISTS prosecution_documents (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            patent_id INTEGER,
            jurisdiction TEXT,
            application_number TEXT,
            publication_number TEXT,
            source TEXT,
            remote_document_id TEXT,
            document_type TEXT,
            document_title TEXT,
            official_date TEXT,
            direction TEXT,
            source_url TEXT,
            download_url TEXT,
            download_available INTEGER DEFAULT 0,
            fingerprint TEXT,
            first_seen_at INTEGER,
            last_seen_at INTEGER,
            local_path TEXT,
            downloaded_at INTEGER DEFAULT 0,
            raw_metadata TEXT,
            raw_title TEXT DEFAULT '',
            document_code TEXT DEFAULT '',
            document_version TEXT DEFAULT 'ORIGINAL',
            event_key TEXT DEFAULT '',
            source_trace TEXT DEFAULT '',
            UNIQUE (source, application_number, fingerprint)
        )
    )");
    Execute("CREATE INDEX IF NOT EXISTS idx_prosecution_docs_patent "
            "ON prosecution_documents(patent_id)");
    Execute("CREATE UNIQUE INDEX IF NOT EXISTS idx_prosecution_docs_event_key "
            "ON prosecution_documents(patent_id, event_key) WHERE event_key <> ''");

    Execute(R"(
        CREATE TABLE IF NOT EXISTS dossier_sync_state (
            patent_id INTEGER NOT NULL,
            provider TEXT NOT NULL,
            last_checked_at INTEGER DEFAULT 0,
            last_success_at INTEGER DEFAULT 0,
            last_error_at INTEGER DEFAULT 0,
            last_error_code TEXT,
            last_error_message TEXT,
            latest_remote_oa_date TEXT,
            latest_remote_oa_type TEXT,
            auth_state TEXT DEFAULT 'NOT_INITIALIZED',
            PRIMARY KEY (patent_id, provider)
        )
    )");

    // Legacy upgrade path for databases created before the per-module
    // migrations below existed (column-level ALTER, idempotent).
    MigrateTables();
}

void Database::MigrateTables() {
    // Helper to get existing columns
    auto getColumns = [this](const std::string& table) -> std::set<std::string> {
        std::set<std::string> columns;
        std::string sql = "PRAGMA table_info(" + table + ");";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* col_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (col_name) columns.insert(col_name);
            }
            sqlite3_finalize(stmt);
        }
        return columns;
    };

    // Helper to add column if missing
    auto addColumn = [this](const std::string& table, const std::string& col, const std::string& type) {
        std::string sql = "ALTER TABLE " + table + " ADD COLUMN " + col + " " + type + ";";
        sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr);
    };

    // Historical column additions kept so pre-0.4 databases converge to the
    // same shape regardless of which version they come from. The structured
    // v2 columns and indexes are handled by schema_migrations.cpp.
    auto patent_cols = getColumns("patents");
    for (const auto& col : std::vector<std::string>{"patent_level", "geke_handler", "class_level1",
             "class_level2", "class_level3", "rd_department", "agency_firm", "original_applicant",
             "current_applicant", "proposal_name"}) {
        if (!patent_cols.count(col)) addColumn("patents", col, "TEXT");
    }

    auto oa_cols = getColumns("oa_records");
    for (const auto& col : std::vector<std::string>{"writer", "progress", "agency", "oa_summary"}) {
        if (!oa_cols.count(col)) addColumn("oa_records", col, "TEXT");
    }
    if (!oa_cols.count("is_extendable")) addColumn("oa_records", "is_extendable", "INTEGER DEFAULT 0");
    if (!oa_cols.count("extension_requested")) addColumn("oa_records", "extension_requested", "INTEGER DEFAULT 0");
    if (!oa_cols.count("extension_months")) addColumn("oa_records", "extension_months", "INTEGER");
    if (!oa_cols.count("extended_deadline")) addColumn("oa_records", "extended_deadline", "TEXT");

    auto pct_cols = getColumns("pct_patents");
    for (const auto& col : std::vector<std::string>{"domestic_source", "country_app_no",
             "filing_date", "priority_date", "country"}) {
        if (!pct_cols.count(col)) addColumn("pct_patents", col, "TEXT");
    }

    auto sw_cols = getColumns("software_copyrights");
    for (const auto& col : std::vector<std::string>{"original_owner", "current_owner", "developer",
             "dev_complete_date", "version"}) {
        if (!sw_cols.count(col)) addColumn("software_copyrights", col, "TEXT");
    }

    auto ic_cols = getColumns("ic_layouts");
    for (const auto& col : std::vector<std::string>{"original_owner", "current_owner", "designer",
             "creation_date", "cert_date"}) {
        if (!ic_cols.count(col)) addColumn("ic_layouts", col, "TEXT");
    }

    auto fp_cols = getColumns("foreign_patents");
    for (const auto& col : std::vector<std::string>{"pct_no", "country_app_no", "owner",
             "patent_status", "country", "application_no"}) {
        if (!fp_cols.count(col)) addColumn("foreign_patents", col, "TEXT");
    }
}

bool Database::Execute(const std::string& sql) {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        last_error_ = err_msg ? err_msg : "unknown SQL error";
        PATX_LOG_ERROR(std::string("SQL error: ") + last_error_);
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

std::string Database::EscapeString(const std::string& s) {
    std::string result;
    result.reserve(s.size() * 2);
    for (char c : s) {
        if (c == '\'') result += "''";
        else result += c;
    }
    return result;
}

std::string Database::LikePattern(const std::string& keyword) {
    std::string escaped;
    for (char c : keyword) {
        if (c == '%' || c == '_' || c == '\\') escaped += '\\';
        escaped += c;
    }
    return "'" + EscapeString("%" + escaped + "%") + "' ESCAPE '\\'";
}

std::string Database::GetCurrentDate() {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char buf[11];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Row readers: fixed column order matching the SELECT lists below
// ---------------------------------------------------------------------------

namespace {

std::string Col(sqlite3_stmt* stmt, int col) {
    const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return text ? text : "";
}

// SELECT * is retained only at migration-compatibility boundaries. Resolve
// appended migration fields by name because fresh and upgraded databases can
// legitimately have different physical column orders.
std::string ColByName(sqlite3_stmt* stmt, const char* name) {
    const int count = sqlite3_column_count(stmt);
    for (int i = 0; i < count; ++i) {
        const char* column_name = sqlite3_column_name(stmt, i);
        if (column_name && std::strcmp(column_name, name) == 0) return Col(stmt, i);
    }
    return "";
}

void ReadPatent(sqlite3_stmt* stmt, Patent& p) {
    p.id = sqlite3_column_int(stmt, 0);
    p.geke_code = Col(stmt, 1);
    p.application_number = Col(stmt, 2);
    p.title = Col(stmt, 3);
    p.proposal_name = Col(stmt, 4);
    p.application_status = Col(stmt, 5);
    p.patent_type = Col(stmt, 6);
    p.patent_level = Col(stmt, 7);
    p.application_date = Col(stmt, 8);
    p.authorization_date = Col(stmt, 9);
    p.expiration_date = Col(stmt, 10);
    p.geke_handler = Col(stmt, 11);
    p.rd_department = Col(stmt, 12);
    p.agency_firm = Col(stmt, 13);
    p.original_applicant = Col(stmt, 14);
    p.current_applicant = Col(stmt, 15);
    p.inventor = Col(stmt, 16);
    p.notes = Col(stmt, 17);
    p.class_level1 = Col(stmt, 18);
    p.class_level2 = Col(stmt, 19);
    p.class_level3 = Col(stmt, 20);
    p.updated_at = sqlite3_column_int64(stmt, 21);
    p.related_case_info = Col(stmt, 22);
    p.fee_status = Col(stmt, 23);
    p.rd_project = Col(stmt, 24);
    p.class_level4 = Col(stmt, 25);
    p.tags = Col(stmt, 26);
    p.details = Col(stmt, 27);
    p.filing_date = Col(stmt, 28);
    p.disclosure_writer = Col(stmt, 29);
    p.agent_code = Col(stmt, 30);
    p.agent_name = Col(stmt, 31);
    p.intangible_asset_eval = Col(stmt, 32);
    p.internal_rd_project = Col(stmt, 33);
    p.technology_route = Col(stmt, 34);
    p.project_id = Col(stmt, 35);
    p.oa_reminder_1 = Col(stmt, 36);
    p.oa_reminder_2 = Col(stmt, 37);
    p.oa_reminder_3 = Col(stmt, 38);
    p.oa_reminder_4 = Col(stmt, 39);
    p.oa_reminder_5 = Col(stmt, 40);
    p.reexamination = Col(stmt, 41);
    p.pudong_subsidy = Col(stmt, 42);
    p.pct_reminder = Col(stmt, 43);
    p.publication_number = Col(stmt, 44);
}

const char* kPatentColumns =
    "id, geke_code, application_number, title, proposal_name, application_status, "
    "patent_type, patent_level, application_date, authorization_date, expiration_date, "
    "geke_handler, rd_department, agency_firm, original_applicant, current_applicant, "
    "inventor, notes, class_level1, class_level2, class_level3, updated_at, "
    "related_case_info, fee_status, rd_project, class_level4, tags, details, filing_date, "
    "disclosure_writer, agent_code, agent_name, intangible_asset_eval, internal_rd_project, "
    "technology_route, project_id, oa_reminder_1, oa_reminder_2, oa_reminder_3, oa_reminder_4, "
    "oa_reminder_5, reexamination, pudong_subsidy, pct_reminder, publication_number";

void ReadOA(sqlite3_stmt* stmt, OARecord& oa) {
    oa.id = sqlite3_column_int(stmt, 0);
    oa.patent_id = sqlite3_column_int(stmt, 1);
    oa.geke_code = Col(stmt, 2);
    oa.patent_title = Col(stmt, 3);
    oa.oa_type = Col(stmt, 4);
    oa.official_deadline = Col(stmt, 5);
    oa.issue_date = Col(stmt, 6);
    oa.response_date = Col(stmt, 7);
    oa.handler = Col(stmt, 8);
    oa.writer = Col(stmt, 9);
    oa.progress = Col(stmt, 10);
    oa.agency = Col(stmt, 11);
    oa.oa_summary = Col(stmt, 12);
    oa.is_completed = sqlite3_column_int(stmt, 13) != 0;
    oa.is_extendable = sqlite3_column_int(stmt, 14) != 0;
    oa.extension_requested = sqlite3_column_int(stmt, 15) != 0;
    oa.extension_months = sqlite3_column_int(stmt, 16);
    oa.extended_deadline = Col(stmt, 17);
    oa.notes = Col(stmt, 18);
    oa.jurisdiction = Col(stmt, 19);
    oa.source = Col(stmt, 20);
    oa.deadline_source = Col(stmt, 21);
    oa.remote_document_id = Col(stmt, 22);
    oa.sync_flag = Col(stmt, 23);
}

const char* kOAColumns =
    "id, patent_id, geke_code, patent_title, oa_type, official_deadline, issue_date, "
    "response_date, handler, writer, progress, agency, oa_summary, is_completed, "
    "is_extendable, extension_requested, extension_months, extended_deadline, notes, "
    "jurisdiction, source, deadline_source, "
    "remote_document_id, sync_flag";

void ReadPCT(sqlite3_stmt* stmt, PCTPatent& p) {
    p.id = sqlite3_column_int(stmt, 0);
    p.geke_code = Col(stmt, 1);
    p.domestic_source = Col(stmt, 2);
    p.application_no = Col(stmt, 3);
    p.country_app_no = Col(stmt, 4);
    p.title = Col(stmt, 5);
    p.application_status = Col(stmt, 6);
    p.handler = Col(stmt, 7);
    p.inventor = Col(stmt, 8);
    p.filing_date = Col(stmt, 9);
    p.application_date = Col(stmt, 10);
    p.priority_date = Col(stmt, 11);
    p.country = Col(stmt, 12);
    p.notes = Col(stmt, 13);
}

const char* kPCTColumns =
    "id, geke_code, domestic_source, application_no, country_app_no, title, "
    "application_status, handler, inventor, filing_date, application_date, "
    "priority_date, country, notes";

void ReadSoftware(sqlite3_stmt* stmt, SoftwareCopyright& s) {
    s.id = sqlite3_column_int(stmt, 0);
    s.case_no = Col(stmt, 1);
    s.reg_no = Col(stmt, 2);
    s.title = Col(stmt, 3);
    s.original_owner = Col(stmt, 4);
    s.current_owner = Col(stmt, 5);
    s.application_status = Col(stmt, 6);
    s.handler = Col(stmt, 7);
    s.developer = Col(stmt, 8);
    s.inventor = Col(stmt, 9);
    s.dev_complete_date = Col(stmt, 10);
    s.application_date = Col(stmt, 11);
    s.reg_date = Col(stmt, 12);
    s.version = Col(stmt, 13);
    s.notes = Col(stmt, 14);
}

const char* kSoftwareColumns =
    "id, case_no, reg_no, title, original_owner, current_owner, application_status, "
    "handler, developer, inventor, dev_complete_date, application_date, reg_date, "
    "version, notes";

void ReadIC(sqlite3_stmt* stmt, ICLayout& ic) {
    ic.id = sqlite3_column_int(stmt, 0);
    ic.case_no = Col(stmt, 1);
    ic.reg_no = Col(stmt, 2);
    ic.title = Col(stmt, 3);
    ic.original_owner = Col(stmt, 4);
    ic.current_owner = Col(stmt, 5);
    ic.application_status = Col(stmt, 6);
    ic.handler = Col(stmt, 7);
    ic.designer = Col(stmt, 8);
    ic.inventor = Col(stmt, 9);
    ic.application_date = Col(stmt, 10);
    ic.creation_date = Col(stmt, 11);
    ic.cert_date = Col(stmt, 12);
    ic.notes = Col(stmt, 13);
}

const char* kICColumns =
    "id, case_no, reg_no, title, original_owner, current_owner, application_status, "
    "handler, designer, inventor, application_date, creation_date, cert_date, notes";

void ReadForeign(sqlite3_stmt* stmt, ForeignPatent& f) {
    f.id = sqlite3_column_int(stmt, 0);
    f.case_no = Col(stmt, 1);
    f.pct_no = Col(stmt, 2);
    f.country_app_no = Col(stmt, 3);
    f.title = Col(stmt, 4);
    f.owner = Col(stmt, 5);
    f.patent_status = Col(stmt, 6);
    f.handler = Col(stmt, 7);
    f.inventor = Col(stmt, 8);
    f.application_date = Col(stmt, 9);
    f.authorization_date = Col(stmt, 10);
    f.country = Col(stmt, 11);
    f.application_no = Col(stmt, 12);
    f.notes = Col(stmt, 13);
}

const char* kForeignColumns =
    "id, case_no, pct_no, country_app_no, title, owner, patent_status, handler, "
    "inventor, application_date, authorization_date, country, application_no, notes";

std::vector<Patent> QueryPatents(sqlite3* db, const std::string& where_clause) {
    std::vector<Patent> results;
    std::string sql = std::string("SELECT ") + kPatentColumns + " FROM patents" + where_clause;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Patent p;
            ReadPatent(stmt, p);
            results.push_back(std::move(p));
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

} // namespace

// ============== Patents ==============

std::vector<Patent> Database::GetPatents(const QueryFilter& filter) {
    std::string sql = " WHERE 1=1";

    if (!filter.status.empty())
        sql += " AND application_status = '" + EscapeString(filter.status) + "'";
    if (!filter.level.empty())
        sql += " AND patent_level = '" + EscapeString(filter.level) + "'";
    if (!filter.handler.empty())
        sql += " AND geke_handler = '" + EscapeString(filter.handler) + "'";
    if (!filter.keyword.empty()) {
        sql += " AND (geke_code LIKE " + LikePattern(filter.keyword) +
               " OR application_number LIKE " + LikePattern(filter.keyword) +
               " OR title LIKE " + LikePattern(filter.keyword) +
               " OR proposal_name LIKE " + LikePattern(filter.keyword) +
               " OR inventor LIKE " + LikePattern(filter.keyword) +
               " OR tags LIKE " + LikePattern(filter.keyword) +
               " OR rd_project LIKE " + LikePattern(filter.keyword) + ")";
    }
    sql += " ORDER BY geke_code COLLATE NOCASE";
    return QueryPatents(db_, sql);
}

std::vector<Patent> Database::SearchPatents(const std::string& keyword) {
    QueryFilter filter;
    filter.keyword = keyword;
    return GetPatents(filter);
}

Patent Database::GetPatentById(int id) {
    auto results = QueryPatents(db_, " WHERE id = " + std::to_string(id));
    return results.empty() ? Patent() : results[0];
}

Patent Database::GetPatentByCode(const std::string& geke_code) {
    auto results = QueryPatents(db_,
        " WHERE geke_code = '" + EscapeString(geke_code) + "'");
    return results.empty() ? Patent() : results[0];
}

int Database::InsertPatent(const Patent& p, bool log_undo) {
    std::string sql =
        "INSERT INTO patents (geke_code, application_number, title, proposal_name, "
        "application_status, patent_type, patent_level, application_date, authorization_date, "
        "expiration_date, geke_handler, rd_department, agency_firm, original_applicant, "
        "current_applicant, inventor, notes, class_level1, class_level2, class_level3, "
        "updated_at, related_case_info, fee_status, rd_project, class_level4, tags, details, "
        "filing_date, disclosure_writer, agent_code, agent_name, intangible_asset_eval, "
        "internal_rd_project, technology_route, project_id, oa_reminder_1, oa_reminder_2, "
        "oa_reminder_3, oa_reminder_4, oa_reminder_5, reexamination, pudong_subsidy, pct_reminder, "
        "publication_number) "
        "VALUES ('" +
        EscapeString(p.geke_code) + "','" + EscapeString(p.application_number) + "','" +
        EscapeString(p.title) + "','" + EscapeString(p.proposal_name) + "','" +
        EscapeString(p.application_status) + "','" + EscapeString(p.patent_type) + "','" +
        EscapeString(p.patent_level) + "','" + EscapeString(p.application_date) + "','" +
        EscapeString(p.authorization_date) + "','" + EscapeString(p.expiration_date) + "','" +
        EscapeString(p.geke_handler) + "','" + EscapeString(p.rd_department) + "','" +
        EscapeString(p.agency_firm) + "','" + EscapeString(p.original_applicant) + "','" +
        EscapeString(p.current_applicant) + "','" + EscapeString(p.inventor) + "','" +
        EscapeString(p.notes) + "','" + EscapeString(p.class_level1) + "','" +
        EscapeString(p.class_level2) + "','" + EscapeString(p.class_level3) + "'," +
        std::to_string(time(nullptr)) + ",'" +
        EscapeString(p.related_case_info) + "','" + EscapeString(p.fee_status) + "','" +
        EscapeString(p.rd_project) + "','" + EscapeString(p.class_level4) + "','" +
        EscapeString(p.tags) + "','" + EscapeString(p.details) + "','" +
        EscapeString(p.filing_date) + "','" + EscapeString(p.disclosure_writer) + "','" +
        EscapeString(p.agent_code) + "','" + EscapeString(p.agent_name) + "','" +
        EscapeString(p.intangible_asset_eval) + "','" + EscapeString(p.internal_rd_project) + "','" +
        EscapeString(p.technology_route) + "','" + EscapeString(p.project_id) + "','" +
        EscapeString(p.oa_reminder_1) + "','" + EscapeString(p.oa_reminder_2) + "','" +
        EscapeString(p.oa_reminder_3) + "','" + EscapeString(p.oa_reminder_4) + "','" +
        EscapeString(p.oa_reminder_5) + "','" + EscapeString(p.reexamination) + "','" +
        EscapeString(p.pudong_subsidy) + "','" + EscapeString(p.pct_reminder) + "','" +
        EscapeString(p.publication_number) + "')";

    if (Execute(sql)) {
        int id = static_cast<int>(sqlite3_last_insert_rowid(db_));
        if (log_undo && g_undo_manager) {
            g_undo_manager->LogOperation("delete", "patents", id, PatentToJson(p), "");
        }
        return id;
    }
    return 0;
}

bool Database::UpdatePatent(int id, const Patent& p, bool log_undo) {
    if (log_undo && g_undo_manager) {
        Patent old = GetPatentById(id);
        g_undo_manager->LogOperation("update", "patents", id, PatentToJson(old), PatentToJson(p));
    }

    std::string sql =
        "UPDATE patents SET " +
        std::string("application_number = '") + EscapeString(p.application_number) + "'," +
        "title = '" + EscapeString(p.title) + "'," +
        "proposal_name = '" + EscapeString(p.proposal_name) + "'," +
        "application_status = '" + EscapeString(p.application_status) + "'," +
        "patent_type = '" + EscapeString(p.patent_type) + "'," +
        "patent_level = '" + EscapeString(p.patent_level) + "'," +
        "application_date = '" + EscapeString(p.application_date) + "'," +
        "authorization_date = '" + EscapeString(p.authorization_date) + "'," +
        "expiration_date = '" + EscapeString(p.expiration_date) + "'," +
        "geke_handler = '" + EscapeString(p.geke_handler) + "'," +
        "rd_department = '" + EscapeString(p.rd_department) + "'," +
        "agency_firm = '" + EscapeString(p.agency_firm) + "'," +
        "original_applicant = '" + EscapeString(p.original_applicant) + "'," +
        "current_applicant = '" + EscapeString(p.current_applicant) + "'," +
        "inventor = '" + EscapeString(p.inventor) + "'," +
        "notes = '" + EscapeString(p.notes) + "'," +
        "class_level1 = '" + EscapeString(p.class_level1) + "'," +
        "class_level2 = '" + EscapeString(p.class_level2) + "'," +
        "class_level3 = '" + EscapeString(p.class_level3) + "'," +
        "updated_at = " + std::to_string(time(nullptr)) + "," +
        "related_case_info = '" + EscapeString(p.related_case_info) + "'," +
        "fee_status = '" + EscapeString(p.fee_status) + "'," +
        "rd_project = '" + EscapeString(p.rd_project) + "'," +
        "class_level4 = '" + EscapeString(p.class_level4) + "'," +
        "tags = '" + EscapeString(p.tags) + "'," +
        "details = '" + EscapeString(p.details) + "'," +
        "filing_date = '" + EscapeString(p.filing_date) + "'," +
        "disclosure_writer = '" + EscapeString(p.disclosure_writer) + "'," +
        "agent_code = '" + EscapeString(p.agent_code) + "'," +
        "agent_name = '" + EscapeString(p.agent_name) + "'," +
        "intangible_asset_eval = '" + EscapeString(p.intangible_asset_eval) + "'," +
        "internal_rd_project = '" + EscapeString(p.internal_rd_project) + "'," +
        "technology_route = '" + EscapeString(p.technology_route) + "'," +
        "project_id = '" + EscapeString(p.project_id) + "'," +
        "oa_reminder_1 = '" + EscapeString(p.oa_reminder_1) + "'," +
        "oa_reminder_2 = '" + EscapeString(p.oa_reminder_2) + "'," +
        "oa_reminder_3 = '" + EscapeString(p.oa_reminder_3) + "'," +
        "oa_reminder_4 = '" + EscapeString(p.oa_reminder_4) + "'," +
        "oa_reminder_5 = '" + EscapeString(p.oa_reminder_5) + "'," +
        "reexamination = '" + EscapeString(p.reexamination) + "'," +
        "pudong_subsidy = '" + EscapeString(p.pudong_subsidy) + "'," +
        "pct_reminder = '" + EscapeString(p.pct_reminder) + "'," +
        "publication_number = '" + EscapeString(p.publication_number) + "'" +
        " WHERE id = " + std::to_string(id);
    return Execute(sql);
}

bool Database::DeletePatent(int id, bool log_undo) {
    if (log_undo && g_undo_manager) {
        Patent old = GetPatentById(id);
        g_undo_manager->LogOperation("delete", "patents", id, PatentToJson(old), "");
    }
    return Execute("DELETE FROM patents WHERE id = " + std::to_string(id));
}

// ============== OA Records ==============

std::vector<OARecord> Database::GetOARecords(const QueryFilter& filter) {
    std::vector<OARecord> results;
    std::string sql = std::string("SELECT ") + kOAColumns + " FROM oa_records WHERE 1=1";

    if (filter.deadline_state == "due5") {
        sql += " AND is_completed = 0 AND date(official_deadline) BETWEEN date('now') AND date('now', '+5 days')";
    } else if (filter.deadline_state == "due30") {
        sql += " AND is_completed = 0 AND date(official_deadline) BETWEEN date('now') AND date('now', '+30 days')";
    } else if (filter.deadline_state == "incomplete") {
        sql += " AND is_completed = 0";
    } else if (filter.deadline_state == "completed") {
        sql += " AND is_completed = 1";
    }

    if (!filter.oa_type.empty())
        sql += " AND oa_type = '" + EscapeString(filter.oa_type) + "'";
    if (!filter.handler.empty())
        sql += " AND handler = '" + EscapeString(filter.handler) + "'";
    if (!filter.writer.empty())
        sql += " AND writer = '" + EscapeString(filter.writer) + "'";
    if (!filter.progress.empty())
        sql += " AND progress = '" + EscapeString(filter.progress) + "'";
    if (!filter.keyword.empty()) {
        sql += " AND (geke_code LIKE " + LikePattern(filter.keyword) +
               " OR patent_title LIKE " + LikePattern(filter.keyword) +
               " OR oa_type LIKE " + LikePattern(filter.keyword) +
               " OR oa_summary LIKE " + LikePattern(filter.keyword) +
               " OR notes LIKE " + LikePattern(filter.keyword) + ")";
    }
    sql += " ORDER BY is_completed ASC, official_deadline ASC";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            OARecord oa;
            ReadOA(stmt, oa);
            results.push_back(std::move(oa));
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

OARecord Database::GetOAById(int id) {
    std::string sql = std::string("SELECT ") + kOAColumns + " FROM oa_records WHERE id = " +
                      std::to_string(id);
    sqlite3_stmt* stmt;
    OARecord oa;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) ReadOA(stmt, oa);
        sqlite3_finalize(stmt);
    }
    return oa;
}

std::vector<OARecord> Database::GetOAByPatent(const std::string& geke_code) {
    QueryFilter filter;
    filter.keyword.clear();
    std::vector<OARecord> results;
    std::string sql = std::string("SELECT ") + kOAColumns + " FROM oa_records WHERE geke_code = '" +
                      EscapeString(geke_code) + "' ORDER BY official_deadline ASC";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            OARecord oa;
            ReadOA(stmt, oa);
            results.push_back(std::move(oa));
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

int Database::InsertOA(const OARecord& oa, bool log_undo) {
    std::string sql =
        "INSERT INTO oa_records (patent_id, geke_code, patent_title, oa_type, official_deadline, "
        "issue_date, response_date, handler, writer, progress, agency, oa_summary, is_completed, "
        "is_extendable, extension_requested, extension_months, extended_deadline, notes, "
        "jurisdiction, source, deadline_source, "
        "remote_document_id, sync_flag) VALUES (" +
        std::to_string(oa.patent_id) + ",'" +
        EscapeString(oa.geke_code) + "','" + EscapeString(oa.patent_title) + "','" +
        EscapeString(oa.oa_type) + "','" + EscapeString(oa.official_deadline) + "','" +
        EscapeString(oa.issue_date) + "','" + EscapeString(oa.response_date) + "','" +
        EscapeString(oa.handler) + "','" + EscapeString(oa.writer) + "','" +
        EscapeString(oa.progress) + "','" + EscapeString(oa.agency) + "','" +
        EscapeString(oa.oa_summary) + "'," +
        std::to_string(oa.is_completed ? 1 : 0) + "," +
        std::to_string(oa.is_extendable ? 1 : 0) + "," +
        std::to_string(oa.extension_requested ? 1 : 0) + "," +
        std::to_string(oa.extension_months) + ",'" +
        EscapeString(oa.extended_deadline) + "','" + EscapeString(oa.notes) + "','" +
        EscapeString(oa.jurisdiction) + "','" + EscapeString(oa.source) + "','" +
        EscapeString(oa.deadline_source) + "','" + EscapeString(oa.remote_document_id) + "','" +
        EscapeString(oa.sync_flag) + "')";

    if (Execute(sql)) {
        int id = static_cast<int>(sqlite3_last_insert_rowid(db_));
        if (log_undo && g_undo_manager) {
            g_undo_manager->LogOperation("delete", "oa_records", id, OAToJson(oa), "");
        }
        return id;
    }
    return 0;
}

bool Database::UpdateOA(int id, const OARecord& oa, bool log_undo) {
    if (log_undo && g_undo_manager) {
        OARecord old = GetOAById(id);
        g_undo_manager->LogOperation("update", "oa_records", id, OAToJson(old), OAToJson(oa));
    }

    std::string sql =
        "UPDATE oa_records SET " +
        std::string("oa_type = '") + EscapeString(oa.oa_type) + "'," +
        "official_deadline = '" + EscapeString(oa.official_deadline) + "'," +
        "issue_date = '" + EscapeString(oa.issue_date) + "'," +
        "response_date = '" + EscapeString(oa.response_date) + "'," +
        "handler = '" + EscapeString(oa.handler) + "'," +
        "writer = '" + EscapeString(oa.writer) + "'," +
        "progress = '" + EscapeString(oa.progress) + "'," +
        "agency = '" + EscapeString(oa.agency) + "'," +
        "oa_summary = '" + EscapeString(oa.oa_summary) + "'," +
        "is_completed = " + std::to_string(oa.is_completed ? 1 : 0) + "," +
        "is_extendable = " + std::to_string(oa.is_extendable ? 1 : 0) + "," +
        "extension_requested = " + std::to_string(oa.extension_requested ? 1 : 0) + "," +
        "extension_months = " + std::to_string(oa.extension_months) + "," +
        "extended_deadline = '" + EscapeString(oa.extended_deadline) + "'," +
        "notes = '" + EscapeString(oa.notes) + "'," +
        "jurisdiction = '" + EscapeString(oa.jurisdiction) + "'," +
        "source = '" + EscapeString(oa.source) + "'," +
        "deadline_source = '" + EscapeString(oa.deadline_source) + "'," +
        "remote_document_id = '" + EscapeString(oa.remote_document_id) + "'," +
        "sync_flag = '" + EscapeString(oa.sync_flag) + "'" +
        " WHERE id = " + std::to_string(id);
    return Execute(sql);
}

bool Database::DeleteOA(int id, bool log_undo) {
    if (log_undo && g_undo_manager) {
        OARecord old = GetOAById(id);
        g_undo_manager->LogOperation("delete", "oa_records", id, OAToJson(old), "");
    }
    return Execute("DELETE FROM oa_records WHERE id = " + std::to_string(id));
}

bool Database::MarkOACompleted(int id) {
    return Execute("UPDATE oa_records SET is_completed = 1, progress = 'completed', response_date = '" +
                   GetCurrentDate() + "' WHERE id = " + std::to_string(id));
}

// ============== PCT ==============

std::vector<PCTPatent> Database::GetPCTPatents(const QueryFilter& filter) {
    std::vector<PCTPatent> results;
    std::string sql = std::string("SELECT ") + kPCTColumns + " FROM pct_patents WHERE 1=1";
    if (!filter.status.empty())
        sql += " AND application_status = '" + EscapeString(filter.status) + "'";
    if (!filter.handler.empty())
        sql += " AND handler = '" + EscapeString(filter.handler) + "'";
    if (!filter.country.empty())
        sql += " AND country = '" + EscapeString(filter.country) + "'";
    if (!filter.keyword.empty()) {
        sql += " AND (geke_code LIKE " + LikePattern(filter.keyword) +
               " OR application_no LIKE " + LikePattern(filter.keyword) +
               " OR country_app_no LIKE " + LikePattern(filter.keyword) +
               " OR title LIKE " + LikePattern(filter.keyword) +
               " OR domestic_source LIKE " + LikePattern(filter.keyword) +
               " OR inventor LIKE " + LikePattern(filter.keyword) + ")";
    }
    sql += " ORDER BY geke_code COLLATE NOCASE";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            PCTPatent p;
            ReadPCT(stmt, p);
            results.push_back(std::move(p));
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

PCTPatent Database::GetPCTById(int id) {
    std::string sql = std::string("SELECT ") + kPCTColumns + " FROM pct_patents WHERE id = " +
                      std::to_string(id);
    sqlite3_stmt* stmt;
    PCTPatent p;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) ReadPCT(stmt, p);
        sqlite3_finalize(stmt);
    }
    return p;
}

int Database::InsertPCT(const PCTPatent& p) {
    std::string sql =
        "INSERT INTO pct_patents (geke_code, domestic_source, application_no, country_app_no, "
        "title, application_status, handler, inventor, filing_date, application_date, "
        "priority_date, country, notes) VALUES ('" +
        EscapeString(p.geke_code) + "','" + EscapeString(p.domestic_source) + "','" +
        EscapeString(p.application_no) + "','" + EscapeString(p.country_app_no) + "','" +
        EscapeString(p.title) + "','" + EscapeString(p.application_status) + "','" +
        EscapeString(p.handler) + "','" + EscapeString(p.inventor) + "','" +
        EscapeString(p.filing_date) + "','" + EscapeString(p.application_date) + "','" +
        EscapeString(p.priority_date) + "','" + EscapeString(p.country) + "','" +
        EscapeString(p.notes) + "')";
    if (Execute(sql)) return static_cast<int>(sqlite3_last_insert_rowid(db_));
    return 0;
}

bool Database::UpdatePCT(int id, const PCTPatent& p) {
    std::string sql =
        "UPDATE pct_patents SET " +
        std::string("geke_code = '") + EscapeString(p.geke_code) + "'," +
        "domestic_source = '" + EscapeString(p.domestic_source) + "'," +
        "application_no = '" + EscapeString(p.application_no) + "'," +
        "country_app_no = '" + EscapeString(p.country_app_no) + "'," +
        "title = '" + EscapeString(p.title) + "'," +
        "application_status = '" + EscapeString(p.application_status) + "'," +
        "handler = '" + EscapeString(p.handler) + "'," +
        "inventor = '" + EscapeString(p.inventor) + "'," +
        "filing_date = '" + EscapeString(p.filing_date) + "'," +
        "application_date = '" + EscapeString(p.application_date) + "'," +
        "priority_date = '" + EscapeString(p.priority_date) + "'," +
        "country = '" + EscapeString(p.country) + "'," +
        "notes = '" + EscapeString(p.notes) + "'" +
        " WHERE id = " + std::to_string(id);
    return Execute(sql);
}

bool Database::DeletePCT(int id) {
    return Execute("DELETE FROM pct_patents WHERE id = " + std::to_string(id));
}

// ============== Software ==============

std::vector<SoftwareCopyright> Database::GetSoftwareCopyrights(const QueryFilter& filter) {
    std::vector<SoftwareCopyright> results;
    std::string sql = std::string("SELECT ") + kSoftwareColumns + " FROM software_copyrights WHERE 1=1";
    if (!filter.status.empty())
        sql += " AND application_status = '" + EscapeString(filter.status) + "'";
    if (!filter.handler.empty())
        sql += " AND handler = '" + EscapeString(filter.handler) + "'";
    if (!filter.keyword.empty()) {
        sql += " AND (case_no LIKE " + LikePattern(filter.keyword) +
               " OR reg_no LIKE " + LikePattern(filter.keyword) +
               " OR title LIKE " + LikePattern(filter.keyword) +
               " OR current_owner LIKE " + LikePattern(filter.keyword) +
               " OR notes LIKE " + LikePattern(filter.keyword) + ")";
    }
    sql += " ORDER BY case_no COLLATE NOCASE";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            SoftwareCopyright s;
            ReadSoftware(stmt, s);
            results.push_back(std::move(s));
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

SoftwareCopyright Database::GetSoftwareById(int id) {
    std::string sql = std::string("SELECT ") + kSoftwareColumns + " FROM software_copyrights WHERE id = " +
                      std::to_string(id);
    sqlite3_stmt* stmt;
    SoftwareCopyright s;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) ReadSoftware(stmt, s);
        sqlite3_finalize(stmt);
    }
    return s;
}

int Database::InsertSoftware(const SoftwareCopyright& s) {
    std::string sql =
        "INSERT INTO software_copyrights (case_no, reg_no, title, original_owner, current_owner, "
        "application_status, handler, developer, inventor, dev_complete_date, application_date, "
        "reg_date, version, notes) VALUES ('" +
        EscapeString(s.case_no) + "','" + EscapeString(s.reg_no) + "','" + EscapeString(s.title) + "','" +
        EscapeString(s.original_owner) + "','" + EscapeString(s.current_owner) + "','" +
        EscapeString(s.application_status) + "','" + EscapeString(s.handler) + "','" +
        EscapeString(s.developer) + "','" + EscapeString(s.inventor) + "','" +
        EscapeString(s.dev_complete_date) + "','" + EscapeString(s.application_date) + "','" +
        EscapeString(s.reg_date) + "','" + EscapeString(s.version) + "','" +
        EscapeString(s.notes) + "')";
    if (Execute(sql)) return static_cast<int>(sqlite3_last_insert_rowid(db_));
    return 0;
}

bool Database::UpdateSoftware(int id, const SoftwareCopyright& s) {
    std::string sql =
        "UPDATE software_copyrights SET " +
        std::string("case_no = '") + EscapeString(s.case_no) + "'," +
        "reg_no = '" + EscapeString(s.reg_no) + "'," +
        "title = '" + EscapeString(s.title) + "'," +
        "original_owner = '" + EscapeString(s.original_owner) + "'," +
        "current_owner = '" + EscapeString(s.current_owner) + "'," +
        "application_status = '" + EscapeString(s.application_status) + "'," +
        "handler = '" + EscapeString(s.handler) + "'," +
        "developer = '" + EscapeString(s.developer) + "'," +
        "inventor = '" + EscapeString(s.inventor) + "'," +
        "dev_complete_date = '" + EscapeString(s.dev_complete_date) + "'," +
        "application_date = '" + EscapeString(s.application_date) + "'," +
        "reg_date = '" + EscapeString(s.reg_date) + "'," +
        "version = '" + EscapeString(s.version) + "'," +
        "notes = '" + EscapeString(s.notes) + "'" +
        " WHERE id = " + std::to_string(id);
    return Execute(sql);
}

bool Database::DeleteSoftware(int id) {
    return Execute("DELETE FROM software_copyrights WHERE id = " + std::to_string(id));
}

// ============== IC Layouts ==============

std::vector<ICLayout> Database::GetICLayouts(const QueryFilter& filter) {
    std::vector<ICLayout> results;
    std::string sql = std::string("SELECT ") + kICColumns + " FROM ic_layouts WHERE 1=1";
    if (!filter.status.empty())
        sql += " AND application_status = '" + EscapeString(filter.status) + "'";
    if (!filter.handler.empty())
        sql += " AND handler = '" + EscapeString(filter.handler) + "'";
    if (!filter.keyword.empty()) {
        sql += " AND (case_no LIKE " + LikePattern(filter.keyword) +
               " OR reg_no LIKE " + LikePattern(filter.keyword) +
               " OR title LIKE " + LikePattern(filter.keyword) +
               " OR current_owner LIKE " + LikePattern(filter.keyword) +
               " OR notes LIKE " + LikePattern(filter.keyword) + ")";
    }
    sql += " ORDER BY case_no COLLATE NOCASE";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ICLayout ic;
            ReadIC(stmt, ic);
            results.push_back(std::move(ic));
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

ICLayout Database::GetICById(int id) {
    std::string sql = std::string("SELECT ") + kICColumns + " FROM ic_layouts WHERE id = " +
                      std::to_string(id);
    sqlite3_stmt* stmt;
    ICLayout ic;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) ReadIC(stmt, ic);
        sqlite3_finalize(stmt);
    }
    return ic;
}

int Database::InsertIC(const ICLayout& ic) {
    std::string sql =
        "INSERT INTO ic_layouts (case_no, reg_no, title, original_owner, current_owner, "
        "application_status, handler, designer, inventor, application_date, creation_date, "
        "cert_date, notes) VALUES ('" +
        EscapeString(ic.case_no) + "','" + EscapeString(ic.reg_no) + "','" + EscapeString(ic.title) + "','" +
        EscapeString(ic.original_owner) + "','" + EscapeString(ic.current_owner) + "','" +
        EscapeString(ic.application_status) + "','" + EscapeString(ic.handler) + "','" +
        EscapeString(ic.designer) + "','" + EscapeString(ic.inventor) + "','" +
        EscapeString(ic.application_date) + "','" + EscapeString(ic.creation_date) + "','" +
        EscapeString(ic.cert_date) + "','" + EscapeString(ic.notes) + "')";
    if (Execute(sql)) return static_cast<int>(sqlite3_last_insert_rowid(db_));
    return 0;
}

bool Database::UpdateIC(int id, const ICLayout& ic) {
    std::string sql =
        "UPDATE ic_layouts SET " +
        std::string("case_no = '") + EscapeString(ic.case_no) + "'," +
        "reg_no = '" + EscapeString(ic.reg_no) + "'," +
        "title = '" + EscapeString(ic.title) + "'," +
        "original_owner = '" + EscapeString(ic.original_owner) + "'," +
        "current_owner = '" + EscapeString(ic.current_owner) + "'," +
        "application_status = '" + EscapeString(ic.application_status) + "'," +
        "handler = '" + EscapeString(ic.handler) + "'," +
        "designer = '" + EscapeString(ic.designer) + "'," +
        "inventor = '" + EscapeString(ic.inventor) + "'," +
        "application_date = '" + EscapeString(ic.application_date) + "'," +
        "creation_date = '" + EscapeString(ic.creation_date) + "'," +
        "cert_date = '" + EscapeString(ic.cert_date) + "'," +
        "notes = '" + EscapeString(ic.notes) + "'" +
        " WHERE id = " + std::to_string(id);
    return Execute(sql);
}

bool Database::DeleteIC(int id) {
    return Execute("DELETE FROM ic_layouts WHERE id = " + std::to_string(id));
}

// ============== Foreign Patents ==============

std::vector<ForeignPatent> Database::GetForeignPatents(const QueryFilter& filter) {
    std::vector<ForeignPatent> results;
    std::string sql = std::string("SELECT ") + kForeignColumns + " FROM foreign_patents WHERE 1=1";
    if (!filter.status.empty())
        sql += " AND patent_status = '" + EscapeString(filter.status) + "'";
    if (!filter.handler.empty())
        sql += " AND handler = '" + EscapeString(filter.handler) + "'";
    if (!filter.country.empty())
        sql += " AND country = '" + EscapeString(filter.country) + "'";
    if (!filter.keyword.empty()) {
        sql += " AND (case_no LIKE " + LikePattern(filter.keyword) +
               " OR pct_no LIKE " + LikePattern(filter.keyword) +
               " OR country_app_no LIKE " + LikePattern(filter.keyword) +
               " OR application_no LIKE " + LikePattern(filter.keyword) +
               " OR title LIKE " + LikePattern(filter.keyword) +
               " OR owner LIKE " + LikePattern(filter.keyword) + ")";
    }
    sql += " ORDER BY case_no COLLATE NOCASE";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ForeignPatent f;
            ReadForeign(stmt, f);
            results.push_back(std::move(f));
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

ForeignPatent Database::GetForeignById(int id) {
    std::string sql = std::string("SELECT ") + kForeignColumns + " FROM foreign_patents WHERE id = " +
                      std::to_string(id);
    sqlite3_stmt* stmt;
    ForeignPatent f;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) ReadForeign(stmt, f);
        sqlite3_finalize(stmt);
    }
    return f;
}

int Database::InsertForeign(const ForeignPatent& f) {
    std::string sql =
        "INSERT INTO foreign_patents (case_no, pct_no, country_app_no, title, owner, "
        "patent_status, handler, inventor, application_date, authorization_date, country, "
        "application_no, notes) VALUES ('" +
        EscapeString(f.case_no) + "','" + EscapeString(f.pct_no) + "','" +
        EscapeString(f.country_app_no) + "','" + EscapeString(f.title) + "','" +
        EscapeString(f.owner) + "','" + EscapeString(f.patent_status) + "','" +
        EscapeString(f.handler) + "','" + EscapeString(f.inventor) + "','" +
        EscapeString(f.application_date) + "','" + EscapeString(f.authorization_date) + "','" +
        EscapeString(f.country) + "','" + EscapeString(f.application_no) + "','" +
        EscapeString(f.notes) + "')";
    if (Execute(sql)) return static_cast<int>(sqlite3_last_insert_rowid(db_));
    return 0;
}

bool Database::UpdateForeign(int id, const ForeignPatent& f) {
    std::string sql =
        "UPDATE foreign_patents SET " +
        std::string("case_no = '") + EscapeString(f.case_no) + "'," +
        "pct_no = '" + EscapeString(f.pct_no) + "'," +
        "country_app_no = '" + EscapeString(f.country_app_no) + "'," +
        "title = '" + EscapeString(f.title) + "'," +
        "owner = '" + EscapeString(f.owner) + "'," +
        "patent_status = '" + EscapeString(f.patent_status) + "'," +
        "handler = '" + EscapeString(f.handler) + "'," +
        "inventor = '" + EscapeString(f.inventor) + "'," +
        "application_date = '" + EscapeString(f.application_date) + "'," +
        "authorization_date = '" + EscapeString(f.authorization_date) + "'," +
        "country = '" + EscapeString(f.country) + "'," +
        "application_no = '" + EscapeString(f.application_no) + "'," +
        "notes = '" + EscapeString(f.notes) + "'" +
        " WHERE id = " + std::to_string(id);
    return Execute(sql);
}

bool Database::DeleteForeign(int id) {
    return Execute("DELETE FROM foreign_patents WHERE id = " + std::to_string(id));
}

namespace {

// Normalizes a US application number: strips "US", slashes ("17/248024" ->
// "17248024"), whitespace and leading zeros (ODP expects the series+serial
// form without the slash, e.g. "17248024").
std::string NormalizeUSApplicationNumber(const std::string& input) {
    std::string out;
    for (char c : input) {
        if (c == ' ' || c == '\t' || c == '/' || c == '-') continue;
        if ((c == 'U' || c == 'u') && out.empty()) continue;
        out += static_cast<char>(::toupper(c));
    }
    // ODP stores numbers without leading zeros after the series digits are
    // joined; compare digit-only tails conservatively (no stripping here -
    // caller also compares with LIKE).
    return out;
}

} // namespace

// ============== Deadline Rules ==============

std::vector<DeadlineRule> Database::GetDeadlineRules(bool enabled_only) {
    std::vector<DeadlineRule> results;
    std::string sql = std::string(
        "SELECT id, jurisdiction, event_type, rule_description, base_months, base_days, "
        "extendable, max_extension_months, effective_from, effective_to, enabled, notes "
        "FROM deadline_rules");
    if (enabled_only) sql += " WHERE enabled = 1";
    sql += " ORDER BY jurisdiction, event_type";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            DeadlineRule r;
            r.id = sqlite3_column_int(stmt, 0);
            r.jurisdiction = Col(stmt, 1);
            r.event_type = Col(stmt, 2);
            r.rule_description = Col(stmt, 3);
            r.base_months = sqlite3_column_int(stmt, 4);
            r.base_days = sqlite3_column_int(stmt, 5);
            r.extendable = sqlite3_column_int(stmt, 6) != 0;
            r.max_extension_months = sqlite3_column_int(stmt, 7);
            r.effective_from = Col(stmt, 8);
            r.effective_to = Col(stmt, 9);
            r.enabled = sqlite3_column_int(stmt, 10) != 0;
            r.notes = Col(stmt, 11);
            results.push_back(std::move(r));
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

bool Database::InsertDeadlineRule(const DeadlineRule& rule, int* new_id) {
    std::string sql = std::string(
        "INSERT INTO deadline_rules (jurisdiction, event_type, rule_description, base_months, "
        "base_days, extendable, max_extension_months, effective_from, effective_to, enabled, notes) "
        "VALUES ('") +
        EscapeString(rule.jurisdiction) + "','" + EscapeString(rule.event_type) + "','" +
        EscapeString(rule.rule_description) + "'," +
        std::to_string(rule.base_months) + "," + std::to_string(rule.base_days) + "," +
        std::to_string(rule.extendable ? 1 : 0) + "," + std::to_string(rule.max_extension_months) + ",'" +
        EscapeString(rule.effective_from) + "','" + EscapeString(rule.effective_to) + "'," +
        std::to_string(rule.enabled ? 1 : 0) + ",'" + EscapeString(rule.notes) + "')";
    if (Execute(sql)) {
        if (new_id) *new_id = static_cast<int>(sqlite3_last_insert_rowid(db_));
        return true;
    }
    return false;
}

bool Database::UpdateDeadlineRule(const DeadlineRule& rule) {
    if (rule.id <= 0) return false;
    std::string sql = std::string(
        "UPDATE deadline_rules SET ") +
        "jurisdiction = '" + EscapeString(rule.jurisdiction) + "'," +
        "event_type = '" + EscapeString(rule.event_type) + "'," +
        "rule_description = '" + EscapeString(rule.rule_description) + "'," +
        "base_months = " + std::to_string(rule.base_months) + "," +
        "base_days = " + std::to_string(rule.base_days) + "," +
        "extendable = " + std::to_string(rule.extendable ? 1 : 0) + "," +
        "max_extension_months = " + std::to_string(rule.max_extension_months) + "," +
        "effective_from = '" + EscapeString(rule.effective_from) + "'," +
        "effective_to = '" + EscapeString(rule.effective_to) + "'," +
        "enabled = " + std::to_string(rule.enabled ? 1 : 0) + "," +
        "notes = '" + EscapeString(rule.notes) + "'" +
        " WHERE id = " + std::to_string(rule.id);
    return Execute(sql);
}

bool Database::DeleteDeadlineRule(int id) {
    return Execute("DELETE FROM deadline_rules WHERE id = " + std::to_string(id));
}

std::string Database::CalculateDeadline(const std::string& jurisdiction,
                                        const std::string& event_type,
                                        const std::string& base_date) const {
    if (base_date.empty()) return "";
    sqlite3_stmt* stmt;
    std::string sql =
        "SELECT date(?, '+' || base_months || ' months', '+' || base_days || ' days') "
        "FROM deadline_rules WHERE jurisdiction = ? AND event_type = ? AND enabled = 1 "
        "ORDER BY id LIMIT 1";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, base_date.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, jurisdiction.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, event_type.c_str(), -1, SQLITE_TRANSIENT);
        std::string result;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            result = Col(stmt, 0);
        }
        sqlite3_finalize(stmt);
        return result;
    }
    return "";
}

// ============== Utility ==============

std::vector<std::string> Database::GetDistinctValues(const std::string& table, const std::string& column) {
    std::vector<std::string> results;
    // Whitelist: table.column comes from internal call sites only, but keep
    // the guard so future callers can't inject through these two fields.
    static const std::set<std::string> allowed_tables = {
        "patents", "oa_records", "pct_patents", "software_copyrights",
        "ic_layouts", "foreign_patents"};
    if (!allowed_tables.count(table)) return results;

    std::string sql = "SELECT DISTINCT " + column + " FROM " + table +
                      " WHERE " + column + " IS NOT NULL AND " + column + " != '' ORDER BY " + column;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            results.push_back(Col(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

void Database::SetConfig(const std::string& key, const std::string& value) {
    Execute(R"(CREATE TABLE IF NOT EXISTS config (key TEXT PRIMARY KEY, value TEXT))");
    Execute("INSERT OR REPLACE INTO config (key, value) VALUES ('" +
            EscapeString(key) + "','" + EscapeString(value) + "')");
}

std::string Database::GetConfig(const std::string& key) {
    Execute(R"(CREATE TABLE IF NOT EXISTS config (key TEXT PRIMARY KEY, value TEXT))");
    std::string sql = "SELECT value FROM config WHERE key = '" + EscapeString(key) + "'";
    sqlite3_stmt* stmt;
    std::string result;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            result = Col(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return result;
}

bool Database::BackupTo(const std::string& dest_path) {
    sqlite3* dest = nullptr;
    if (sqlite3_open(dest_path.c_str(), &dest) != SQLITE_OK) {
        last_error_ = "cannot open backup destination";
        sqlite3_close(dest);
        return false;
    }
    sqlite3_backup* backup = sqlite3_backup_init(dest, "main", db_, "main");
    if (!backup) {
        last_error_ = "backup init failed";
        sqlite3_close(dest);
        return false;
    }
    sqlite3_backup_step(backup, -1);
    int rc = sqlite3_backup_finish(backup);
    sqlite3_close(dest);
    if (rc != SQLITE_OK) {
        last_error_ = "backup failed";
        return false;
    }
    return true;
}

// ============== Undo support ==============

std::string Database::PatentToJson(const Patent& p) {
    // Lossless snapshot of every column so undo restores the full record.
    std::ostringstream json;
    auto field = [&](const char* name, const std::string& value, bool last = false) {
        json << "\"" << name << "\":\"" << EscapeString(value) << "\"" << (last ? "" : ",");
    };
    json << "{";
    json << "\"id\":" << p.id << ",";
    field("geke_code", p.geke_code);
    field("application_number", p.application_number);
    field("title", p.title);
    field("proposal_name", p.proposal_name);
    field("application_status", p.application_status);
    field("patent_type", p.patent_type);
    field("patent_level", p.patent_level);
    field("application_date", p.application_date);
    field("authorization_date", p.authorization_date);
    field("expiration_date", p.expiration_date);
    field("geke_handler", p.geke_handler);
    field("rd_department", p.rd_department);
    field("agency_firm", p.agency_firm);
    field("original_applicant", p.original_applicant);
    field("current_applicant", p.current_applicant);
    field("inventor", p.inventor);
    field("notes", p.notes);
    field("class_level1", p.class_level1);
    field("class_level2", p.class_level2);
    field("class_level3", p.class_level3);
    field("related_case_info", p.related_case_info);
    field("fee_status", p.fee_status);
    field("rd_project", p.rd_project);
    field("class_level4", p.class_level4);
    field("tags", p.tags);
    field("details", p.details);
    field("filing_date", p.filing_date);
    field("disclosure_writer", p.disclosure_writer);
    field("agent_code", p.agent_code);
    field("agent_name", p.agent_name);
    field("intangible_asset_eval", p.intangible_asset_eval);
    field("internal_rd_project", p.internal_rd_project);
    field("technology_route", p.technology_route);
    field("project_id", p.project_id);
    field("oa_reminder_1", p.oa_reminder_1);
    field("oa_reminder_2", p.oa_reminder_2);
    field("oa_reminder_3", p.oa_reminder_3);
    field("oa_reminder_4", p.oa_reminder_4);
    field("oa_reminder_5", p.oa_reminder_5);
    field("reexamination", p.reexamination);
    field("pudong_subsidy", p.pudong_subsidy);
    field("pct_reminder", p.pct_reminder);
    field("publication_number", p.publication_number, true);
    json << "}";
    return json.str();
}

std::string Database::OAToJson(const OARecord& oa) {
    std::ostringstream json;
    json << "{";
    json << "\"id\":" << oa.id << ",";
    json << "\"patent_id\":" << oa.patent_id << ",";
    json << "\"geke_code\":\"" << EscapeString(oa.geke_code) << "\",";
    json << "\"patent_title\":\"" << EscapeString(oa.patent_title) << "\",";
    json << "\"oa_type\":\"" << EscapeString(oa.oa_type) << "\",";
    json << "\"official_deadline\":\"" << EscapeString(oa.official_deadline) << "\",";
    json << "\"issue_date\":\"" << EscapeString(oa.issue_date) << "\",";
    json << "\"response_date\":\"" << EscapeString(oa.response_date) << "\",";
    json << "\"handler\":\"" << EscapeString(oa.handler) << "\",";
    json << "\"writer\":\"" << EscapeString(oa.writer) << "\",";
    json << "\"progress\":\"" << EscapeString(oa.progress) << "\",";
    json << "\"agency\":\"" << EscapeString(oa.agency) << "\",";
    json << "\"oa_summary\":\"" << EscapeString(oa.oa_summary) << "\",";
    json << "\"is_completed\":" << (oa.is_completed ? 1 : 0) << ",";
    json << "\"is_extendable\":" << (oa.is_extendable ? 1 : 0) << ",";
    json << "\"extension_requested\":" << (oa.extension_requested ? 1 : 0) << ",";
    json << "\"extension_months\":" << oa.extension_months << ",";
    json << "\"extended_deadline\":\"" << EscapeString(oa.extended_deadline) << "\",";
    json << "\"notes\":\"" << EscapeString(oa.notes) << "\",";
    json << "\"jurisdiction\":\"" << EscapeString(oa.jurisdiction) << "\",";
    json << "\"source\":\"" << EscapeString(oa.source) << "\",";
    json << "\"deadline_source\":\"" << EscapeString(oa.deadline_source) << "\",";
    json << "\"remote_document_id\":\"" << EscapeString(oa.remote_document_id) << "\",";
    json << "\"sync_flag\":\"" << EscapeString(oa.sync_flag) << "\"";
    json << "}";
    return json.str();
}

void Database::BeginBatch() {
    if (g_undo_manager) g_undo_manager->BeginBatch();
}

int Database::Undo() {
    if (g_undo_manager) return g_undo_manager->Undo();
    return 0;
}

bool Database::CanUndo() const {
    if (g_undo_manager) return g_undo_manager->CanUndo();
    return false;
}

// ============== Web Dossier Sync ==============

std::vector<Patent> Database::GetPatentsForDossierCheck(bool include_granted, int limit) {
    std::vector<Patent> results;
    // Terminal statuses are never checked; granted cases only on the slow
    // cycle (include_granted). Everything else (审查中/等待审查/答复/复审...)
    // counts as active prosecution.
    std::string sql =
        "SELECT * FROM patents WHERE "
        "(application_status IS NULL OR application_status NOT LIKE '%放弃%') AND "
        "application_status NOT LIKE '%失效%' AND "
        "application_status NOT LIKE '%撤回%' AND "
        "application_status NOT LIKE '%视撤%' AND "
        "application_status NOT LIKE '%终止%' AND "
        "application_status NOT LIKE '%已获证书%'";
    if (!include_granted) {
        sql += " AND (application_status IS NULL OR (application_status NOT LIKE '%授权%' "
               "AND application_status NOT LIKE '%Granted%'))";
    }
    sql += " ORDER BY next_dossier_check_at ASC, id ASC";
    if (limit > 0) sql += " LIMIT " + std::to_string(limit);

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Patent p;
            auto cs = [&](int col) -> std::string {
                return (const char*)sqlite3_column_text(stmt, col) ? (const char*)sqlite3_column_text(stmt, col) : "";
            };
            p.id = sqlite3_column_int(stmt, 0);
            p.geke_code = cs(1);
            p.application_number = cs(2);
            p.title = cs(3);
            p.proposal_name = cs(4);
            p.application_status = cs(5);
            p.patent_type = cs(6);
            p.patent_level = cs(7);
            p.application_date = cs(8);
            p.authorization_date = cs(9);
            p.expiration_date = cs(10);
            p.geke_handler = cs(11);
            p.rd_department = cs(12);
            p.agency_firm = cs(13);
            p.original_applicant = cs(14);
            p.current_applicant = cs(15);
            p.inventor = cs(16);
            p.notes = cs(17);
            p.class_level1 = cs(18);
            p.class_level2 = cs(19);
            p.class_level3 = cs(20);
            p.publication_number = ColByName(stmt, "publication_number");
            results.push_back(p);
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

int Database::UpsertProsecutionDocument(ProsecutionDocumentRecord& doc, bool* created) {
    if (created) *created = false;
    long long now = static_cast<long long>(time(nullptr));

    // Cross-source identity first: one row per official event, whatever the
    // provider. Falls back to the legacy (source, app, fingerprint) rule for
    // rows that carry no event_key (older data / sites without stable ids).
    std::string find = "SELECT id, source_trace FROM prosecution_documents WHERE ";
    if (!doc.event_key.empty()) {
        find += "patent_id = ? AND event_key = ?";
    } else {
        find += "source = ? AND application_number = ? AND fingerprint = ?";
    }
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, find.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (!doc.event_key.empty()) {
            sqlite3_bind_int(stmt, 1, doc.patent_id);
            sqlite3_bind_text(stmt, 2, doc.event_key.c_str(), -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_text(stmt, 1, doc.source.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, doc.application_number.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, doc.fingerprint.c_str(), -1, SQLITE_TRANSIENT);
        }
        int existing = 0;
        std::string existing_trace;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            existing = sqlite3_column_int(stmt, 0);
            const char* trace = (const char*)sqlite3_column_text(stmt, 1);
            if (trace) existing_trace = trace;
        }
        sqlite3_finalize(stmt);
        if (existing > 0) {
            std::string merged_trace = existing_trace;
            auto append_if_new = [&merged_trace](const std::string& item) {
                if (item.empty()) return;
                size_t start = 0;
                while (start <= merged_trace.size()) {
                    size_t end = merged_trace.find(',', start);
                    std::string token = merged_trace.substr(
                        start, end == std::string::npos ? std::string::npos : end - start);
                    if (token == item) return;
                    if (end == std::string::npos) break;
                    start = end + 1;
                }
                merged_trace += merged_trace.empty() ? item : "," + item;
            };
            append_if_new(doc.source);
            if (!doc.source_trace.empty()) {
                size_t start = 0;
                while (start <= doc.source_trace.size()) {
                    size_t end = doc.source_trace.find(',', start);
                    std::string token = doc.source_trace.substr(
                        start, end == std::string::npos ? std::string::npos : end - start);
                    append_if_new(token);
                    if (end == std::string::npos) break;
                    start = end + 1;
                }
            }
            Execute("UPDATE prosecution_documents SET last_seen_at = " + std::to_string(now) +
                    ", raw_metadata = '" + EscapeString(doc.raw_metadata) + "'" +
                    ", source_trace = '" + EscapeString(merged_trace) + "'" +
                    " WHERE id = " + std::to_string(existing));
            doc.id = existing;
            doc.last_seen_at = now;
            doc.source_trace = merged_trace;
            return existing;
        }
    }

    if (doc.source_trace.empty()) doc.source_trace = doc.source;
    doc.first_seen_at = now;
    doc.last_seen_at = now;
    std::string sql =
        "INSERT INTO prosecution_documents (patent_id, jurisdiction, application_number, "
        "publication_number, source, remote_document_id, document_type, document_title, "
        "official_date, direction, source_url, download_url, download_available, fingerprint, "
        "first_seen_at, last_seen_at, raw_metadata, raw_title, document_code, "
        "document_version, event_key, source_trace) VALUES (" +
        std::to_string(doc.patent_id) + ",'" +
        EscapeString(doc.jurisdiction) + "','" +
        EscapeString(doc.application_number) + "','" +
        EscapeString(doc.publication_number) + "','" +
        EscapeString(doc.source) + "','" +
        EscapeString(doc.remote_document_id) + "','" +
        EscapeString(doc.document_type) + "','" +
        EscapeString(doc.document_title) + "','" +
        EscapeString(doc.official_date) + "','" +
        EscapeString(doc.direction) + "','" +
        EscapeString(doc.source_url) + "','" +
        EscapeString(doc.download_url) + "'," +
        std::to_string(doc.download_available ? 1 : 0) + ",'" +
        EscapeString(doc.fingerprint) + "'," +
        std::to_string(doc.first_seen_at) + "," +
        std::to_string(doc.last_seen_at) + ",'" +
        EscapeString(doc.raw_metadata) + "','" +
        EscapeString(doc.raw_title) + "','" +
        EscapeString(doc.document_code) + "','" +
        EscapeString(doc.document_version) + "','" +
        EscapeString(doc.event_key) + "','" +
        EscapeString(doc.source_trace) + "')";
    if (Execute(sql)) {
        doc.id = sqlite3_last_insert_rowid(db_);
        if (created) *created = true;
        return doc.id;
    }
    return 0;
}

ProsecutionDocumentRecord Database::GetProsecutionDocumentById(int id) {
    ProsecutionDocumentRecord doc;
    if (id <= 0) return doc;
    sqlite3_stmt* stmt;
    std::string sql = "SELECT id, patent_id, jurisdiction, application_number, "
                      "publication_number, source, remote_document_id, document_type, "
                      "document_title, official_date, direction, source_url, download_url, "
                      "download_available, fingerprint, first_seen_at, last_seen_at, "
                      "raw_metadata, raw_title, document_code, document_version, "
                      "event_key, source_trace FROM prosecution_documents WHERE id = " +
                      std::to_string(id);
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            auto text = [&stmt](int i) -> std::string {
                const char* v = (const char*)sqlite3_column_text(stmt, i);
                return v ? v : "";
            };
            doc.id = sqlite3_column_int(stmt, 0);
            doc.patent_id = sqlite3_column_int(stmt, 1);
            doc.jurisdiction = text(2);
            doc.application_number = text(3);
            doc.publication_number = text(4);
            doc.source = text(5);
            doc.remote_document_id = text(6);
            doc.document_type = text(7);
            doc.document_title = text(8);
            doc.official_date = text(9);
            doc.direction = text(10);
            doc.source_url = text(11);
            doc.download_url = text(12);
            doc.download_available = sqlite3_column_int(stmt, 13) != 0;
            doc.fingerprint = text(14);
            doc.first_seen_at = sqlite3_column_int64(stmt, 15);
            doc.last_seen_at = sqlite3_column_int64(stmt, 16);
            doc.raw_metadata = text(17);
            doc.raw_title = text(18);
            doc.document_code = text(19);
            doc.document_version = text(20);
            doc.event_key = text(21);
            doc.source_trace = text(22);
        }
        sqlite3_finalize(stmt);
    }
    return doc;
}

std::vector<Patent> Database::GetPatentsDueForDossierCheck(bool include_granted,
                                                           long long now, int limit) {
    // Same status window as GetPatentsForDossierCheck, narrowed to cases the
    // CN dossier chain can actually query and whose check slot has arrived.
    std::vector<Patent> results;
    std::string sql =
        "SELECT * FROM patents WHERE "
        "(application_status IS NULL OR application_status NOT LIKE '%放弃%') AND "
        "application_status NOT LIKE '%失效%' AND "
        "application_status NOT LIKE '%撤回%' AND "
        "application_status NOT LIKE '%视撤%' AND "
        "application_status NOT LIKE '%终止%' AND "
        "application_status NOT LIKE '%已获证书%'";
    if (!include_granted) {
        sql += " AND (application_status IS NULL OR (application_status NOT LIKE '%授权%' "
               "AND application_status NOT LIKE '%Granted%'))";
    }
    sql += " AND (next_dossier_check_at IS NULL OR next_dossier_check_at = 0"
           " OR next_dossier_check_at <= " + std::to_string(now) + ")";
    sql += " AND (application_number LIKE 'CN%' OR application_number GLOB '[0-9]*'"
           " OR publication_number LIKE 'CN%')";
    sql += " ORDER BY next_dossier_check_at ASC, id ASC";
    if (limit > 0) sql += " LIMIT " + std::to_string(limit);

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Patent p;
            auto cs = [&](int col) -> std::string {
                return (const char*)sqlite3_column_text(stmt, col) ? (const char*)sqlite3_column_text(stmt, col) : "";
            };
            p.id = sqlite3_column_int(stmt, 0);
            p.geke_code = cs(1);
            p.application_number = cs(2);
            p.title = cs(3);
            p.proposal_name = cs(4);
            p.application_status = cs(5);
            p.patent_type = cs(6);
            p.patent_level = cs(7);
            p.application_date = cs(8);
            p.authorization_date = cs(9);
            p.expiration_date = cs(10);
            p.geke_handler = cs(11);
            p.rd_department = cs(12);
            p.agency_firm = cs(13);
            p.original_applicant = cs(14);
            p.current_applicant = cs(15);
            p.inventor = cs(16);
            p.notes = cs(17);
            p.class_level1 = cs(18);
            p.class_level2 = cs(19);
            p.class_level3 = cs(20);
            p.publication_number = ColByName(stmt, "publication_number");
            results.push_back(p);
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

bool Database::UpdatePatentDossierCheck(int patent_id, long long last_at, long long next_at) {
    return Execute("UPDATE patents SET last_dossier_check_at = " + std::to_string(last_at) +
                   ", next_dossier_check_at = " + std::to_string(next_at) +
                   " WHERE id = " + std::to_string(patent_id));
}

bool Database::UpsertDossierSyncState(const DossierSyncState& s) {
    std::string sql =
        "INSERT INTO dossier_sync_state (patent_id, provider, last_checked_at, last_success_at, "
        "last_error_at, last_error_code, last_error_message, latest_remote_oa_date, "
        "latest_remote_oa_type, auth_state) VALUES (" +
        std::to_string(s.patent_id) + ",'" +
        EscapeString(s.provider) + "'," +
        std::to_string(s.last_checked_at) + "," +
        std::to_string(s.last_success_at) + "," +
        std::to_string(s.last_error_at) + ",'" +
        EscapeString(s.last_error_code) + "','" +
        EscapeString(s.last_error_message) + "','" +
        EscapeString(s.latest_remote_oa_date) + "','" +
        EscapeString(s.latest_remote_oa_type) + "','" +
        EscapeString(s.auth_state) + "') "
        "ON CONFLICT(patent_id, provider) DO UPDATE SET "
        "last_checked_at = excluded.last_checked_at, "
        "last_success_at = excluded.last_success_at, "
        "last_error_at = excluded.last_error_at, "
        "last_error_code = excluded.last_error_code, "
        "last_error_message = excluded.last_error_message, "
        "latest_remote_oa_date = excluded.latest_remote_oa_date, "
        "latest_remote_oa_type = excluded.latest_remote_oa_type, "
        "auth_state = excluded.auth_state";
    return Execute(sql);
}

std::vector<DossierSyncState> Database::GetDossierSyncStates(int limit) {
    std::vector<DossierSyncState> results;
    sqlite3_stmt* stmt;
    std::string sql = "SELECT patent_id, provider, last_checked_at, last_success_at, "
                      "last_error_at, last_error_code, last_error_message, "
                      "latest_remote_oa_date, latest_remote_oa_type, auth_state "
                      "FROM dossier_sync_state ORDER BY last_checked_at DESC LIMIT " +
                      std::to_string(limit);
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            DossierSyncState s;
            s.patent_id = sqlite3_column_int(stmt, 0);
            auto text = [&stmt](int i) -> std::string {
                const char* v = (const char*)sqlite3_column_text(stmt, i);
                return v ? v : "";
            };
            s.provider = text(1);
            s.last_checked_at = sqlite3_column_int64(stmt, 2);
            s.last_success_at = sqlite3_column_int64(stmt, 3);
            s.last_error_at = sqlite3_column_int64(stmt, 4);
            s.last_error_code = text(5);
            s.last_error_message = text(6);
            s.latest_remote_oa_date = text(7);
            s.latest_remote_oa_type = text(8);
            s.auth_state = text(9);
            results.push_back(s);
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

std::vector<OARecord> Database::GetOAsForPatentId(int patent_id) {
    std::vector<OARecord> results;
    sqlite3_stmt* stmt;
    std::string sql = "SELECT * FROM oa_records WHERE patent_id = " + std::to_string(patent_id);
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            OARecord oa;
            oa.id = sqlite3_column_int(stmt, 0);
            oa.patent_id = sqlite3_column_int(stmt, 1);
            auto text = [&stmt](int i) -> std::string {
                const char* v = (const char*)sqlite3_column_text(stmt, i);
                return v ? v : "";
            };
            oa.geke_code = text(2);
            oa.patent_title = text(3);
            oa.oa_type = text(4);
            oa.official_deadline = text(5);
            oa.issue_date = text(6);
            oa.response_date = text(7);
            oa.handler = text(8);
            oa.writer = text(9);
            oa.progress = text(10);
            oa.agency = text(11);
            oa.oa_summary = text(12);
            oa.is_completed = sqlite3_column_int(stmt, 13) != 0;
            // columns 19-21 exist after MigrateTables
            oa.source = ColByName(stmt, "source");
            oa.remote_document_id = ColByName(stmt, "remote_document_id");
            oa.sync_flag = ColByName(stmt, "sync_flag");
            results.push_back(oa);
        }
        sqlite3_finalize(stmt);
    }
    return results;
}

bool Database::UpdateOASyncFields(int oa_id, const std::string& issue_date_if_empty,
                                  const std::string& sync_flag) {
    std::string sql = "UPDATE oa_records SET sync_flag = '" + EscapeString(sync_flag) + "'";
    if (!issue_date_if_empty.empty()) {
        sql += ", issue_date = CASE WHEN issue_date IS NULL OR issue_date = '' THEN '" +
               EscapeString(issue_date_if_empty) + "' ELSE issue_date END";
    }
    sql += " WHERE id = " + std::to_string(oa_id);
    return Execute(sql);
}

bool Database::UpdateProsecutionDocumentDownload(int document_id, const std::string& local_path) {
    return Execute("UPDATE prosecution_documents SET local_path = '" +
                   EscapeString(local_path) + "', downloaded_at = " +
                   std::to_string(static_cast<long long>(time(nullptr))) +
                   " WHERE id = " + std::to_string(document_id));
}
