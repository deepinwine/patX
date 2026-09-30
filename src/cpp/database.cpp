// patX Database Implementation
//
// All SQL for the core IP modules (patents / OA / PCT / software / IC /
// foreign / deadline rules) lives here; the web dossier OA merge rules live
// in web_dossier_rules.cpp.
#include "database.hpp"
#include "undo_manager.hpp"
#include "patx/schema_migrations.hpp"
#include "patx/log.hpp"
#include "web_dossier.hpp"

#include <sstream>
#include <iostream>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <filesystem>
#include <set>
#include <stdexcept>

static UndoManager* g_undo_manager = nullptr;
static std::vector<UndoManager*> g_live_undo_managers;

namespace {

class SqliteTransaction {
public:
    SqliteTransaction(sqlite3* db, std::string& error) : db_(db), error_(error) {}

    bool BeginImmediate() {
        if (!Exec("BEGIN IMMEDIATE TRANSACTION")) return false;
        active_ = true;
        return true;
    }

    bool Commit() {
        if (!active_ || !Exec("COMMIT")) return false;
        active_ = false;
        return true;
    }

    ~SqliteTransaction() {
        if (active_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }

private:
    bool Exec(const char* sql) {
        char* message = nullptr;
        const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &message);
        if (rc == SQLITE_OK) return true;
        error_ = message ? message : sqlite3_errmsg(db_);
        sqlite3_free(message);
        return false;
    }

    sqlite3* db_ = nullptr;
    std::string& error_;
    bool active_ = false;
};

void RegisterUndoManager(UndoManager* manager) {
    if (!manager) return;
    g_live_undo_managers.push_back(manager);
    g_undo_manager = manager;
}

void UnregisterUndoManager(UndoManager* manager) {
    if (!manager) return;
    g_live_undo_managers.erase(
        std::remove(g_live_undo_managers.begin(), g_live_undo_managers.end(), manager),
        g_live_undo_managers.end());
    g_undo_manager = g_live_undo_managers.empty() ? nullptr : g_live_undo_managers.back();
}

} // namespace

Database::ConnectionWriteLock Database::AcquireConnectionWriteLock() {
    return ConnectionWriteLock(connection_write_mutex_);
}

UndoManager& GetUndoManager() {
    if (!g_undo_manager) throw std::logic_error("no live database undo manager");
    return *g_undo_manager;
}

Database::Database(const std::string& db_path) : db_path_(db_path) {
    auto close_database = [&]() {
        if (undo_manager_) {
            UnregisterUndoManager(undo_manager_.get());
            undo_manager_.reset();
        }
        if (db_) {
            sqlite3_close_v2(db_);
            db_ = nullptr;
        }
    };

    bool existing_nonempty_file = false;
    std::string path_inspection_error;
    if (db_path != ":memory:") {
        std::error_code error;
        const bool exists = std::filesystem::exists(db_path, error);
        if (error) {
            path_inspection_error = "cannot inspect database path: " + error.message();
        } else if (exists) {
            const auto size = std::filesystem::file_size(db_path, error);
            if (error) {
                path_inspection_error = "cannot inspect database size: " + error.message();
            } else {
                existing_nonempty_file = size > 0;
            }
        }
    }

    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        last_error_ = db_ ? sqlite3_errmsg(db_) : "sqlite3_open failed";
        PATX_LOG_ERROR(std::string("Cannot open database: ") + last_error_ + " (" + db_path + ")");
        close_database();
        return;
    }

    if (!path_inspection_error.empty()) {
        last_error_ = path_inspection_error;
        PATX_LOG_ERROR(last_error_);
        close_database();
        return;
    }

    sqlite3_busy_timeout(db_, 5000);

    // Existing file databases must be backed up and migrated before any
    // CREATE/ALTER statements or write-capable PRAGMAs run.
    if (existing_nonempty_file) {
        auto migration = patx::RunSchemaMigrations(db_, db_path_);
        schema_version_ = patx::ReadSchemaVersion(db_);
        if (!migration.ok) {
            last_error_ = migration.error;
            PATX_LOG_ERROR("Schema migration failed: " + migration.error);
            close_database();
            return;
        }
        if (migration.to_version > migration.from_version) {
            PATX_LOG_INFO("Database schema is now v" +
                          std::to_string(migration.to_version));
        }
    }

    // Enable WAL mode for better performance
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA cache_size=10000;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
    // Initialize undo manager
    undo_manager_ = std::make_unique<UndoManager>();
    undo_manager_->SetDatabase(db_);
    RegisterUndoManager(undo_manager_.get());

    // Fill in the idempotent current shape after an existing database has
    // safely migrated, or initialize a new/empty database before stamping it.
    InitTables();

    if (existing_nonempty_file) {
        schema_version_ = patx::ReadSchemaVersion(db_);
        return;
    }

    auto migration = patx::RunSchemaMigrations(db_, db_path_, false);
    schema_version_ = patx::ReadSchemaVersion(db_);
    if (!migration.ok) {
        last_error_ = migration.error;
        PATX_LOG_ERROR("Schema migration failed: " + migration.error);
        close_database();
    } else if (migration.to_version > migration.from_version) {
        PATX_LOG_INFO("Database schema is now v" + std::to_string(migration.to_version));
    }
}

Database::~Database() {
    UnregisterUndoManager(undo_manager_.get());
    undo_manager_.reset();
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
    Execute("CREATE INDEX IF NOT EXISTS idx_patents_next_dossier_check "
            "ON patents(next_dossier_check_at)");

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
            raw_title TEXT DEFAULT '',
            document_code TEXT DEFAULT '',
            document_version TEXT DEFAULT 'ORIGINAL',
            official_date TEXT,
            direction TEXT,
            source_url TEXT,
            download_url TEXT,
            download_available INTEGER DEFAULT 0,
            fingerprint TEXT,
            event_key TEXT DEFAULT '',
            source_trace TEXT DEFAULT '',
            first_seen_at INTEGER,
            last_seen_at INTEGER,
            local_path TEXT,
            downloaded_at INTEGER DEFAULT 0,
            raw_metadata TEXT,
            UNIQUE (source, application_number, fingerprint)
        )
    )");
    Execute("CREATE INDEX IF NOT EXISTS idx_prosecution_docs_patent "
            "ON prosecution_documents(patent_id)");
    sqlite3_stmt* event_key_column = nullptr;
    bool has_event_key = false;
    if (sqlite3_prepare_v2(db_, "PRAGMA table_info(prosecution_documents)", -1,
                           &event_key_column, nullptr) == SQLITE_OK) {
        while (sqlite3_step(event_key_column) == SQLITE_ROW) {
            const char* name = reinterpret_cast<const char*>(
                sqlite3_column_text(event_key_column, 1));
            if (name && std::strcmp(name, "event_key") == 0) {
                has_event_key = true;
                break;
            }
        }
    }
    sqlite3_finalize(event_key_column);
    if (has_event_key) {
        Execute("CREATE UNIQUE INDEX IF NOT EXISTS idx_prosecution_docs_event_key "
                "ON prosecution_documents(patent_id, event_key) WHERE event_key <> ''");
    }

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
    auto lock = AcquireConnectionWriteLock();
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

std::string TrimToken(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

std::string MergeSourceTrace(const std::string& existing, const std::string& source,
                             const std::string& incoming) {
    std::vector<std::string> tokens;
    std::set<std::string> seen;
    auto append_token = [&](const std::string& value) {
        std::string token = TrimToken(value);
        std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (token.empty()) return;
        for (unsigned char c : token) {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '.' || c == '-')) {
                return;
            }
        }
        if (seen.insert(token).second) tokens.push_back(std::move(token));
    };
    auto append_trace = [&](const std::string& value) {
        size_t start = 0;
        while (start <= value.size()) {
            size_t comma = value.find(',', start);
            append_token(value.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    };
    append_trace(existing);
    append_token(source);
    append_trace(incoming);

    std::string merged;
    for (const auto& token : tokens) {
        if (!merged.empty()) merged += ',';
        merged += token;
    }
    return merged;
}

bool IsAsciiDigits(const std::string& value) {
    if (value.empty()) return false;
    for (unsigned char c : value) {
        if (!std::isdigit(c)) return false;
    }
    return true;
}

bool IsCnApplicationBody(const std::string& body) {
    const size_t dot = body.find('.');
    if (dot == std::string::npos) {
        if (body.size() == 12) return IsAsciiDigits(body);
        if (body.size() != 13 || !IsAsciiDigits(body.substr(0, 12))) return false;
        const char check = body.back();
        return std::isdigit(static_cast<unsigned char>(check)) || check == 'X' || check == 'x';
    }
    if (dot != 12 || body.size() != 14 ||
        body.find('.', dot + 1) != std::string::npos ||
        !IsAsciiDigits(body.substr(0, 12))) {
        return false;
    }
    const char check = body.back();
    return std::isdigit(static_cast<unsigned char>(check)) || check == 'X' || check == 'x';
}

bool IsCnPublicationIdentifier(const std::string& identifier) {
    if (identifier.size() < 2 || std::toupper(static_cast<unsigned char>(identifier[0])) != 'C' ||
        std::toupper(static_cast<unsigned char>(identifier[1])) != 'N') {
        return false;
    }
    const std::string body = identifier.substr(2);
    size_t digits_end = 0;
    while (digits_end < body.size() &&
           std::isdigit(static_cast<unsigned char>(body[digits_end]))) {
        ++digits_end;
    }
    if (digits_end != 9) return false;
    const std::string suffix = body.substr(digits_end);
    std::string normalized_suffix = suffix;
    std::transform(normalized_suffix.begin(), normalized_suffix.end(),
                   normalized_suffix.begin(), [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                   });
    return normalized_suffix.empty() || normalized_suffix == "A" ||
           normalized_suffix == "A1" || normalized_suffix == "B" ||
           normalized_suffix == "B1" || normalized_suffix == "U";
}

bool IsCnDossierIdentifier(const std::string& application_number,
                           const std::string& publication_number) {
    const std::string application = TrimToken(application_number);
    if (!application.empty()) {
        if (application.size() >= 2 &&
            std::toupper(static_cast<unsigned char>(application[0])) == 'C' &&
            std::toupper(static_cast<unsigned char>(application[1])) == 'N') {
            const std::string body = application.substr(2);
            if (IsCnApplicationBody(body) || IsCnPublicationIdentifier(application)) return true;
        } else if (IsCnApplicationBody(application)) {
            return true;
        }
    }
    return IsCnPublicationIdentifier(TrimToken(publication_number));
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
        if (log_undo && undo_manager_) {
            undo_manager_->LogOperation("delete", "patents", id, PatentToJson(p), "");
        }
        return id;
    }
    return 0;
}

bool Database::UpdatePatent(int id, const Patent& p, bool log_undo) {
    if (log_undo && undo_manager_) {
        Patent old = GetPatentById(id);
        undo_manager_->LogOperation("update", "patents", id, PatentToJson(old), PatentToJson(p));
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
    if (log_undo && undo_manager_) {
        Patent old = GetPatentById(id);
        undo_manager_->LogOperation("delete", "patents", id, PatentToJson(old), "");
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
    auto lock = AcquireConnectionWriteLock();
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
        if (log_undo && undo_manager_) {
            undo_manager_->LogOperation("delete", "oa_records", id, OAToJson(oa), "");
        }
        return id;
    }
    return 0;
}

bool Database::UpdateOA(int id, const OARecord& oa, bool log_undo) {
    auto lock = AcquireConnectionWriteLock();
    if (log_undo && undo_manager_) {
        OARecord old = GetOAById(id);
        undo_manager_->LogOperation("update", "oa_records", id, OAToJson(old), OAToJson(oa));
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

OAExactMergeResult Database::MergeOAExact(const OARecord& incoming,
                                          bool fill_empty_handler,
                                          bool log_undo,
                                          bool overwrite_handler_conflict) {
    auto lock = AcquireConnectionWriteLock();
    OAExactMergeResult result;
    if (incoming.geke_code.empty() || incoming.oa_type.empty() || incoming.issue_date.empty()) {
        last_error_ = "OA exact merge requires code, type and issue date";
        return result;
    }

    // The connection lock serializes writers sharing this Database instance;
    // BEGIN IMMEDIATE additionally serializes other connections to the file.
    last_error_.clear();
    SqliteTransaction transaction(db_, last_error_);
    if (!transaction.BeginImmediate()) return result;

    auto commit = [&](OAExactMergeResult value) {
        if (!transaction.Commit()) return OAExactMergeResult{};
        return value;
    };

    const std::string canonical_type = webdossier::NormalizeOaTypeCn(incoming.oa_type);
    std::vector<OARecord> matches;
    for (const auto& existing : GetOAByPatent(incoming.geke_code)) {
        if (existing.issue_date == incoming.issue_date &&
            webdossier::NormalizeOaTypeCn(existing.oa_type) == canonical_type) {
            matches.push_back(existing);
        }
    }

    if (matches.size() > 1) {
        result.status = OAExactMergeStatus::MatchConflict;
        return commit(result);
    }

    if (matches.size() == 1) {
        const OARecord& existing = matches.front();
        result.record_id = existing.id;
        const bool fills_empty_handler =
            fill_empty_handler && existing.handler.empty() && !incoming.handler.empty();
        const bool overwrites_handler_conflict =
            overwrite_handler_conflict && !existing.handler.empty() &&
            !incoming.handler.empty() && existing.handler != incoming.handler;
        if (fills_empty_handler || overwrites_handler_conflict) {
            sqlite3_stmt* stmt = nullptr;
            const char* sql = fills_empty_handler
                ? "UPDATE oa_records SET handler = ? "
                  "WHERE id = ? AND (handler IS NULL OR handler = '')"
                : "UPDATE oa_records SET handler = ? WHERE id = ?";
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
                last_error_ = sqlite3_errmsg(db_);
                return result;
            }
            sqlite3_bind_text(stmt, 1, incoming.handler.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 2, existing.id);
            const int rc = sqlite3_step(stmt);
            if (rc != SQLITE_DONE) last_error_ = sqlite3_errmsg(db_);
            sqlite3_finalize(stmt);
            if (rc != SQLITE_DONE) {
                return result;
            }
            if (sqlite3_changes(db_) != 1) {
                result.status = OAExactMergeStatus::HandlerConflict;
                return commit(result);
            }
            if (log_undo && undo_manager_) {
                OARecord updated = existing;
                updated.handler = incoming.handler;
                undo_manager_->LogOperation(
                    "update", "oa_records", existing.id,
                    OAToJson(existing), OAToJson(updated));
            }
            result.status = OAExactMergeStatus::HandlerUpdated;
            result.overwrote_handler_conflict = overwrites_handler_conflict;
            return commit(result);
        }

        if (incoming.handler.empty() || existing.handler == incoming.handler ||
            !fill_empty_handler) {
            result.status = OAExactMergeStatus::Unchanged;
        } else {
            result.status = OAExactMergeStatus::HandlerConflict;
        }
        return commit(result);
    }

    result.record_id = InsertOA(incoming, log_undo);
    if (result.record_id <= 0) return OAExactMergeResult{};
    result.status = OAExactMergeStatus::Inserted;
    return commit(result);
}

bool Database::DeleteOA(int id, bool log_undo) {
    auto lock = AcquireConnectionWriteLock();
    if (log_undo && undo_manager_) {
        OARecord old = GetOAById(id);
        undo_manager_->LogOperation("delete", "oa_records", id, OAToJson(old), "");
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

bool Database::CopyConsistentSnapshot(const std::string& source_path,
                                      const std::string& dest_path,
                                      std::string* error) {
    if (error) error->clear();
    auto reject = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    std::error_code path_error;
    const bool source_exists = std::filesystem::exists(source_path, path_error);
    if (path_error || !source_exists) {
        return reject(path_error ? path_error.message() : "snapshot source does not exist");
    }
    path_error.clear();
    const bool destination_exists = std::filesystem::exists(dest_path, path_error);
    if (path_error) return reject(path_error.message());
    if (destination_exists) {
        path_error.clear();
        const bool same_file = std::filesystem::equivalent(source_path, dest_path, path_error);
        if (!path_error && same_file) {
            return reject("snapshot source and destination are the same file");
        }
        return reject("snapshot destination already exists");
    }

    sqlite3* source = nullptr;
    sqlite3* destination = nullptr;
    bool destination_created = false;
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        if (destination) sqlite3_close(destination);
        if (source) sqlite3_close(source);
        if (destination_created) {
            std::error_code remove_error;
            std::filesystem::remove(dest_path, remove_error);
        }
        return false;
    };

    int rc = sqlite3_open_v2(source_path.c_str(), &source, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        return fail(source ? sqlite3_errmsg(source) : "cannot open snapshot source");
    }
    sqlite3_busy_timeout(source, 5000);
    rc = sqlite3_open(dest_path.c_str(), &destination);
    path_error.clear();
    destination_created = std::filesystem::exists(dest_path, path_error) && !path_error;
    if (rc != SQLITE_OK) {
        return fail(destination ? sqlite3_errmsg(destination)
                                : "cannot open snapshot destination");
    }
    sqlite3_busy_timeout(destination, 5000);

    sqlite3_backup* backup = sqlite3_backup_init(destination, "main", source, "main");
    if (!backup) return fail(sqlite3_errmsg(destination));

    int attempts = 0;
    do {
        rc = sqlite3_backup_step(backup, -1);
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) sqlite3_sleep(10);
    } while ((rc == SQLITE_BUSY || rc == SQLITE_LOCKED) && ++attempts < 500);
    const int finish_rc = sqlite3_backup_finish(backup);
    if (rc != SQLITE_DONE || finish_rc != SQLITE_OK) {
        return fail(sqlite3_errmsg(destination));
    }

    const int destination_close_rc = sqlite3_close(destination);
    destination = nullptr;
    const int source_close_rc = sqlite3_close(source);
    source = nullptr;
    if (destination_close_rc != SQLITE_OK || source_close_rc != SQLITE_OK) {
        return fail("cannot close SQLite snapshot cleanly");
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
    auto lock = AcquireConnectionWriteLock();
    if (undo_manager_) undo_manager_->BeginBatch();
}

int Database::Undo() {
    auto lock = AcquireConnectionWriteLock();
    if (undo_manager_) return undo_manager_->Undo();
    return 0;
}

bool Database::CanUndo() const {
    ConnectionWriteLock lock(connection_write_mutex_);
    if (undo_manager_) return undo_manager_->CanUndo();
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
        "application_status NOT LIKE '%终止%'";
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

std::vector<Patent> Database::GetPatentsDueForDossierCheck(bool include_granted,
                                                            long long now, int limit) {
    std::string sql =
        " WHERE COALESCE(application_status, '') NOT LIKE '%放弃%'"
        " AND COALESCE(application_status, '') NOT LIKE '%失效%'"
        " AND COALESCE(application_status, '') NOT LIKE '%撤回%'"
        " AND COALESCE(application_status, '') NOT LIKE '%视撤%'"
        " AND COALESCE(application_status, '') NOT LIKE '%终止%'";
    if (!include_granted) {
        sql += " AND COALESCE(application_status, '') NOT LIKE '%授权%'"
               " AND COALESCE(application_status, '') NOT LIKE '%Granted%'";
    }
    sql += " AND (next_dossier_check_at IS NULL OR next_dossier_check_at = 0"
           " OR next_dossier_check_at <= " + std::to_string(now) + ")"
           " ORDER BY COALESCE(next_dossier_check_at, 0) ASC, id ASC";
    auto candidates = QueryPatents(db_, sql);
    std::vector<Patent> results;
    for (auto& patent : candidates) {
        if (!IsCnDossierIdentifier(patent.application_number, patent.publication_number)) continue;
        results.push_back(std::move(patent));
        if (limit > 0 && static_cast<int>(results.size()) >= limit) break;
    }
    return results;
}

int Database::UpsertProsecutionDocument(ProsecutionDocumentRecord& doc, bool* created) {
    auto lock = AcquireConnectionWriteLock();
    if (created) *created = false;
    const long long now = static_cast<long long>(time(nullptr));
    last_error_.clear();
    if (!Execute("BEGIN IMMEDIATE TRANSACTION")) return 0;

    auto rollback = [&]() {
        std::string cause = last_error_;
        if (cause.empty()) cause = sqlite3_errmsg(db_);
        if (cause.empty() || cause == "not an error") {
            cause = "prosecution document upsert failed";
        }
        Execute("ROLLBACK");
        last_error_ = std::move(cause);
        return 0;
    };

    struct ExistingDocument {
        int id = 0;
        int patent_id = 0;
        std::string source_trace;
        long long first_seen_at = 0;
        long long last_seen_at = 0;
        std::string source;
        std::string event_key;
        std::string local_path;
        std::string download_url;
        std::string source_url;
        long long downloaded_at = 0;
        bool download_available = false;
        std::string raw_metadata;
    };
    auto read_existing = [](sqlite3_stmt* stmt) {
        ExistingDocument existing;
        existing.id = sqlite3_column_int(stmt, 0);
        existing.patent_id = sqlite3_column_int(stmt, 1);
        auto text = [stmt](int column) {
            const char* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
            return std::string(value ? value : "");
        };
        existing.source_trace = text(2);
        existing.first_seen_at = sqlite3_column_int64(stmt, 3);
        existing.last_seen_at = sqlite3_column_int64(stmt, 4);
        existing.source = text(5);
        existing.event_key = text(6);
        existing.local_path = text(7);
        existing.download_url = text(8);
        existing.source_url = text(9);
        existing.downloaded_at = sqlite3_column_int64(stmt, 10);
        existing.download_available = sqlite3_column_int(stmt, 11) != 0;
        existing.raw_metadata = text(12);
        return existing;
    };
    auto find_event = [&]() {
        ExistingDocument existing;
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "SELECT id,patent_id,source_trace,first_seen_at,last_seen_at,source,event_key,"
            "local_path,download_url,source_url,downloaded_at,download_available,raw_metadata "
            "FROM prosecution_documents "
            "WHERE patent_id = ? AND event_key = ? LIMIT 1";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return existing;
        sqlite3_bind_int(stmt, 1, doc.patent_id);
        sqlite3_bind_text(stmt, 2, doc.event_key.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) existing = read_existing(stmt);
        sqlite3_finalize(stmt);
        return existing;
    };
    auto find_legacy = [&](bool empty_event_key_only) {
        ExistingDocument existing;
        sqlite3_stmt* stmt = nullptr;
        std::string sql =
            "SELECT id,patent_id,source_trace,first_seen_at,last_seen_at,source,event_key,"
            "local_path,download_url,source_url,downloaded_at,download_available,raw_metadata "
            "FROM prosecution_documents "
            "WHERE source = ? AND application_number = ? AND fingerprint = ? LIMIT 1";
        if (empty_event_key_only) {
            sql =
                "SELECT id,patent_id,source_trace,first_seen_at,last_seen_at,source,event_key,"
                "local_path,download_url,source_url,downloaded_at,download_available,raw_metadata "
                "FROM prosecution_documents "
                "WHERE source = ? AND application_number = ? AND fingerprint = ? "
                "AND (event_key IS NULL OR event_key = '') LIMIT 1";
        }
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return existing;
        sqlite3_bind_text(stmt, 1, doc.source.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, doc.application_number.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, doc.fingerprint.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) existing = read_existing(stmt);
        sqlite3_finalize(stmt);
        return existing;
    };
    auto refresh_existing = [&](const ExistingDocument& existing) {
        const std::string merged_trace =
            MergeSourceTrace(existing.source_trace, doc.source, doc.source_trace);
        sqlite3_stmt* update = nullptr;
        const char* sql =
            "UPDATE prosecution_documents SET last_seen_at = ?, raw_metadata = ?, "
            "source_trace = ? WHERE id = ?";
        if (sqlite3_prepare_v2(db_, sql, -1, &update, nullptr) != SQLITE_OK) return rollback();
        sqlite3_bind_int64(update, 1, now);
        sqlite3_bind_text(update, 2, doc.raw_metadata.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(update, 3, merged_trace.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(update, 4, existing.id);
        const int rc = sqlite3_step(update);
        if (rc != SQLITE_DONE) last_error_ = sqlite3_errmsg(db_);
        sqlite3_finalize(update);
        if (rc != SQLITE_DONE || sqlite3_changes(db_) != 1 || !Execute("COMMIT")) {
            return rollback();
        }
        doc.id = existing.id;
        doc.first_seen_at = existing.first_seen_at;
        doc.last_seen_at = now;
        doc.source_trace = merged_trace;
        return existing.id;
    };
    auto merge_legacy_into_canonical = [&](const ExistingDocument& canonical,
                                            const ExistingDocument& legacy) {
        if (legacy.id == canonical.id) return refresh_existing(canonical);
        if (legacy.patent_id != canonical.patent_id || !legacy.event_key.empty()) {
            return rollback();
        }

        std::string merged_trace =
            MergeSourceTrace(canonical.source_trace, canonical.source, legacy.source_trace);
        merged_trace = MergeSourceTrace(merged_trace, legacy.source, doc.source_trace);
        merged_trace = MergeSourceTrace(merged_trace, doc.source, "");
        long long first_seen = canonical.first_seen_at;
        if (first_seen == 0 || (legacy.first_seen_at > 0 && legacy.first_seen_at < first_seen)) {
            first_seen = legacy.first_seen_at;
        }
        const long long last_seen = std::max({canonical.last_seen_at, legacy.last_seen_at, now});
        auto prefer_canonical = [](const std::string& canonical_value,
                                   const std::string& legacy_value) {
            return canonical_value.empty() ? legacy_value : canonical_value;
        };
        const std::string local_path =
            prefer_canonical(canonical.local_path, legacy.local_path);
        const std::string download_url =
            prefer_canonical(canonical.download_url, legacy.download_url);
        const std::string source_url =
            prefer_canonical(canonical.source_url, legacy.source_url);
        const long long downloaded_at = canonical.downloaded_at > 0
            ? canonical.downloaded_at : legacy.downloaded_at;
        const bool download_available =
            canonical.download_available || legacy.download_available;
        const std::string& raw_metadata = doc.raw_metadata;

        sqlite3_stmt* update = nullptr;
        const char* update_sql =
            "UPDATE prosecution_documents SET source_trace=?, first_seen_at=?, last_seen_at=?, "
            "local_path=?, download_url=?, source_url=?, downloaded_at=?, "
            "download_available=?, raw_metadata=? WHERE id=?";
        if (sqlite3_prepare_v2(db_, update_sql, -1, &update, nullptr) != SQLITE_OK) {
            return rollback();
        }
        int parameter = 1;
        sqlite3_bind_text(update, parameter++, merged_trace.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(update, parameter++, first_seen);
        sqlite3_bind_int64(update, parameter++, last_seen);
        sqlite3_bind_text(update, parameter++, local_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(update, parameter++, download_url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(update, parameter++, source_url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(update, parameter++, downloaded_at);
        sqlite3_bind_int(update, parameter++, download_available ? 1 : 0);
        sqlite3_bind_text(update, parameter++, raw_metadata.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(update, parameter++, canonical.id);
        const int update_rc = sqlite3_step(update);
        if (update_rc != SQLITE_DONE) last_error_ = sqlite3_errmsg(db_);
        const int updated = sqlite3_changes(db_);
        sqlite3_finalize(update);
        if (update_rc != SQLITE_DONE || updated != 1) return rollback();

        sqlite3_stmt* remove = nullptr;
        const char* delete_sql =
            "DELETE FROM prosecution_documents WHERE id=? "
            "AND (event_key IS NULL OR event_key='')";
        if (sqlite3_prepare_v2(db_, delete_sql, -1, &remove, nullptr) != SQLITE_OK) {
            return rollback();
        }
        sqlite3_bind_int(remove, 1, legacy.id);
        const int delete_rc = sqlite3_step(remove);
        if (delete_rc != SQLITE_DONE) last_error_ = sqlite3_errmsg(db_);
        const int deleted = sqlite3_changes(db_);
        sqlite3_finalize(remove);
        if (delete_rc != SQLITE_DONE || deleted != 1 || !Execute("COMMIT")) return rollback();

        doc.id = canonical.id;
        doc.first_seen_at = first_seen;
        doc.last_seen_at = last_seen;
        doc.source_trace = merged_trace;
        return canonical.id;
    };
    auto promote_legacy = [&](const ExistingDocument& legacy) {
        const std::string merged_trace =
            MergeSourceTrace(legacy.source_trace, doc.source, doc.source_trace);
        sqlite3_stmt* promote = nullptr;
        const char* sql =
            "UPDATE prosecution_documents SET event_key = ?, last_seen_at = ?, "
            "raw_metadata = ?, source_trace = ? WHERE id = ? "
            "AND (event_key IS NULL OR event_key = '')";
        if (sqlite3_prepare_v2(db_, sql, -1, &promote, nullptr) != SQLITE_OK) {
            return rollback();
        }
        sqlite3_bind_text(promote, 1, doc.event_key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(promote, 2, now);
        sqlite3_bind_text(promote, 3, doc.raw_metadata.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(promote, 4, merged_trace.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(promote, 5, legacy.id);
        const int rc = sqlite3_step(promote);
        if (rc != SQLITE_DONE) last_error_ = sqlite3_errmsg(db_);
        const int changed = sqlite3_changes(db_);
        sqlite3_finalize(promote);
        if (rc == SQLITE_DONE && changed == 1) {
            if (!Execute("COMMIT")) return rollback();
            doc.id = legacy.id;
            doc.first_seen_at = legacy.first_seen_at;
            doc.last_seen_at = now;
            doc.source_trace = merged_trace;
            return legacy.id;
        }

        // If another compatible writer already claimed the partial key,
        // merge into that canonical event row instead of violating it.
        if ((rc & 0xff) == SQLITE_CONSTRAINT || changed == 0) {
            ExistingDocument event = find_event();
            if (event.id > 0) return merge_legacy_into_canonical(event, legacy);
        }
        return rollback();
    };

    if (!doc.event_key.empty()) {
        ExistingDocument event = find_event();
        if (event.id > 0) {
            ExistingDocument legacy = find_legacy(false);
            if (legacy.id > 0 && legacy.id != event.id) {
                return merge_legacy_into_canonical(event, legacy);
            }
            return refresh_existing(event);
        }

        ExistingDocument legacy = find_legacy(true);
        if (legacy.id > 0) return promote_legacy(legacy);
    } else {
        ExistingDocument legacy = find_legacy(false);
        if (legacy.id > 0) return refresh_existing(legacy);
    }

    doc.first_seen_at = now;
    doc.last_seen_at = now;
    doc.source_trace = MergeSourceTrace("", doc.source, doc.source_trace);
    sqlite3_stmt* insert = nullptr;
    const char* insert_sql =
        "INSERT INTO prosecution_documents ("
        "patent_id, jurisdiction, application_number, publication_number, source, "
        "remote_document_id, document_type, document_title, raw_title, document_code, "
        "document_version, official_date, direction, source_url, download_url, "
        "download_available, fingerprint, event_key, source_trace, first_seen_at, "
        "last_seen_at, raw_metadata) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db_, insert_sql, -1, &insert, nullptr) != SQLITE_OK) return rollback();
    int bind = 1;
    sqlite3_bind_int(insert, bind++, doc.patent_id);
    auto bind_text = [&](const std::string& value) {
        sqlite3_bind_text(insert, bind++, value.c_str(), -1, SQLITE_TRANSIENT);
    };
    bind_text(doc.jurisdiction);
    bind_text(doc.application_number);
    bind_text(doc.publication_number);
    bind_text(doc.source);
    bind_text(doc.remote_document_id);
    bind_text(doc.document_type);
    bind_text(doc.document_title);
    bind_text(doc.raw_title);
    bind_text(doc.document_code);
    bind_text(doc.document_version);
    bind_text(doc.official_date);
    bind_text(doc.direction);
    bind_text(doc.source_url);
    bind_text(doc.download_url);
    sqlite3_bind_int(insert, bind++, doc.download_available ? 1 : 0);
    bind_text(doc.fingerprint);
    bind_text(doc.event_key);
    bind_text(doc.source_trace);
    sqlite3_bind_int64(insert, bind++, doc.first_seen_at);
    sqlite3_bind_int64(insert, bind++, doc.last_seen_at);
    bind_text(doc.raw_metadata);
    const int insert_rc = sqlite3_step(insert);
    if (insert_rc != SQLITE_DONE) last_error_ = sqlite3_errmsg(db_);
    sqlite3_finalize(insert);
    if (insert_rc == SQLITE_DONE && sqlite3_changes(db_) == 1) {
        doc.id = static_cast<int>(sqlite3_last_insert_rowid(db_));
        if (!Execute("COMMIT")) return rollback();
        if (created) *created = true;
        return doc.id;
    }

    if ((insert_rc & 0xff) == SQLITE_CONSTRAINT) {
        ExistingDocument existing = doc.event_key.empty() ? find_legacy(false) : find_event();
        if (existing.id > 0) {
            if (!doc.event_key.empty()) {
                ExistingDocument legacy = find_legacy(false);
                if (legacy.id > 0 && legacy.id != existing.id) {
                    return merge_legacy_into_canonical(existing, legacy);
                }
            }
            return refresh_existing(existing);
        }
        if (!doc.event_key.empty()) {
            ExistingDocument legacy = find_legacy(true);
            if (legacy.id > 0) return promote_legacy(legacy);
        }
    }
    return rollback();
}

ProsecutionDocumentRecord Database::GetProsecutionDocumentById(int id) {
    ProsecutionDocumentRecord doc;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, patent_id, jurisdiction, application_number, publication_number, source, "
        "remote_document_id, document_type, document_title, raw_title, document_code, "
        "document_version, official_date, direction, source_url, download_url, "
        "download_available, fingerprint, event_key, source_trace, first_seen_at, "
        "last_seen_at, raw_metadata FROM prosecution_documents WHERE id = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return doc;
    sqlite3_bind_int(stmt, 1, id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        doc.id = sqlite3_column_int(stmt, 0);
        doc.patent_id = sqlite3_column_int(stmt, 1);
        doc.jurisdiction = Col(stmt, 2);
        doc.application_number = Col(stmt, 3);
        doc.publication_number = Col(stmt, 4);
        doc.source = Col(stmt, 5);
        doc.remote_document_id = Col(stmt, 6);
        doc.document_type = Col(stmt, 7);
        doc.document_title = Col(stmt, 8);
        doc.raw_title = Col(stmt, 9);
        doc.document_code = Col(stmt, 10);
        doc.document_version = Col(stmt, 11);
        doc.official_date = Col(stmt, 12);
        doc.direction = Col(stmt, 13);
        doc.source_url = Col(stmt, 14);
        doc.download_url = Col(stmt, 15);
        doc.download_available = sqlite3_column_int(stmt, 16) != 0;
        doc.fingerprint = Col(stmt, 17);
        doc.event_key = Col(stmt, 18);
        doc.source_trace = Col(stmt, 19);
        doc.first_seen_at = sqlite3_column_int64(stmt, 20);
        doc.last_seen_at = sqlite3_column_int64(stmt, 21);
        doc.raw_metadata = Col(stmt, 22);
    }
    sqlite3_finalize(stmt);
    return doc;
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
                                  const std::string& sync_flag,
                                  const std::string& source_if_empty,
                                  const std::string& remote_document_id_if_empty) {
    auto lock = AcquireConnectionWriteLock();
    std::string sql = "UPDATE oa_records SET sync_flag = '" + EscapeString(sync_flag) + "'";
    if (!issue_date_if_empty.empty()) {
        sql += ", issue_date = CASE WHEN issue_date IS NULL OR issue_date = '' THEN '" +
               EscapeString(issue_date_if_empty) + "' ELSE issue_date END";
    }
    if (!source_if_empty.empty()) {
        sql += ", source = CASE WHEN source IS NULL OR source = '' THEN '" +
               EscapeString(source_if_empty) + "' ELSE source END";
    }
    if (!remote_document_id_if_empty.empty()) {
        sql += ", remote_document_id = CASE WHEN remote_document_id IS NULL OR "
               "remote_document_id = '' THEN '" +
               EscapeString(remote_document_id_if_empty) +
               "' ELSE remote_document_id END";
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
