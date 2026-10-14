// Database layer tests: schema migration, full-field CRUD for every module,
// unified query filters, deadline rules.
#include "test_registry.hpp"

#include "database.hpp"
#include "patx/schema_migrations.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <sqlite3.h>
#include <thread>
#include <utility>

using namespace testutil;

namespace {

Patent MakePatent() {
    Patent p;
    p.geke_code = "GK-TEST-001";
    p.application_number = "202510000001.X";
    p.title = "测试专利";
    p.proposal_name = "测试提案";
    p.application_status = "pending";
    p.patent_type = "invention";
    p.patent_level = "core";
    p.application_date = "2025-01-15";
    p.geke_handler = "张三";
    p.inventor = "李四; 王五";
    p.technology_route = "AI芯片";
    p.rd_project = "智算平台";
    p.tags = "核心;AI";
    p.agent_name = "赵代理";
    p.agent_code = "AG001";
    p.disclosure_writer = "钱撰写";
    p.fee_status = "已缴费";
    p.project_id = "PRJ-9";
    p.oa_reminder_1 = "2025-06-01";
    p.pct_reminder = "2027-06-01";
    p.notes = "手工备注";
    return p;
}

class ScopedSqliteStatement {
public:
    ScopedSqliteStatement() = default;
    ~ScopedSqliteStatement() {
        if (value_) sqlite3_finalize(value_);
    }

    ScopedSqliteStatement(const ScopedSqliteStatement&) = delete;
    ScopedSqliteStatement& operator=(const ScopedSqliteStatement&) = delete;

    sqlite3_stmt** out() { return &value_; }
    sqlite3_stmt* get() const { return value_; }

private:
    sqlite3_stmt* value_ = nullptr;
};

bool SqliteHasColumn(sqlite3* db, const char* table, const char* column) {
    ScopedSqliteStatement stmt;
    std::string sql = std::string("PRAGMA table_info(") + table + ")";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, stmt.out(), nullptr) != SQLITE_OK) return false;
    bool found = false;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        if (name && std::string(name) == column) {
            found = true;
            break;
        }
    }
    return found;
}

bool SqliteHasTable(sqlite3* db, const char* table) {
    ScopedSqliteStatement stmt;
    const char* sql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?";
    if (sqlite3_prepare_v2(db, sql, -1, stmt.out(), nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt.get(), 1, table, -1, SQLITE_TRANSIENT);
    return sqlite3_step(stmt.get()) == SQLITE_ROW;
}

bool SqliteHasIndex(sqlite3* db, const char* index) {
    ScopedSqliteStatement stmt;
    const char* sql = "SELECT 1 FROM sqlite_master WHERE type='index' AND name=?";
    if (sqlite3_prepare_v2(db, sql, -1, stmt.out(), nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt.get(), 1, index, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(stmt.get()) == SQLITE_ROW;
    return found;
}

std::vector<std::string> SqliteIndexColumns(sqlite3* db, const char* index) {
    std::vector<std::string> columns;
    ScopedSqliteStatement stmt;
    std::string sql = std::string("PRAGMA index_info(") + index + ")";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, stmt.out(), nullptr) != SQLITE_OK) return columns;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        if (name) columns.emplace_back(name);
    }
    return columns;
}

std::string SqliteIndexSql(sqlite3* db, const char* index) {
    ScopedSqliteStatement stmt;
    const char* query = "SELECT sql FROM sqlite_master WHERE type='index' AND name=?";
    if (sqlite3_prepare_v2(db, query, -1, stmt.out(), nullptr) != SQLITE_OK) return "";
    sqlite3_bind_text(stmt.get(), 1, index, -1, SQLITE_TRANSIENT);
    std::string sql;
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        if (text) sql = text;
    }
    return sql;
}

std::string CompactIndexSql(std::string sql) {
    sql.erase(std::remove_if(sql.begin(), sql.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }), sql.end());
    std::transform(sql.begin(), sql.end(), sql.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (!sql.empty() && sql.back() == ';') sql.pop_back();
    return sql;
}

void RemoveDatabaseAndMigrationBackups(const std::string& path) noexcept {
    std::error_code error;
    const std::filesystem::path db_path(path);
    const std::string backup_prefix = db_path.filename().string() + ".pre_migration_";
    std::filesystem::directory_iterator iterator(db_path.parent_path(), error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end) {
        if (iterator->path().filename().string().find(backup_prefix) == 0) {
            std::filesystem::remove(iterator->path(), error);
        }
        iterator.increment(error);
    }
    error.clear();
    std::filesystem::remove(path + "-wal", error);
    error.clear();
    std::filesystem::remove(path + "-shm", error);
    error.clear();
    std::filesystem::remove(path, error);
}

struct ScopedMigrationDatabase {
    explicit ScopedMigrationDatabase(std::string database_path)
        : path(std::move(database_path)) {
        RemoveDatabaseAndMigrationBackups(path);
    }

    ~ScopedMigrationDatabase() { RemoveDatabaseAndMigrationBackups(path); }

    std::string path;
};

struct ScopedSqliteHandle {
    ScopedSqliteHandle() = default;
    ~ScopedSqliteHandle() {
        if (value) sqlite3_close_v2(value);
    }

    ScopedSqliteHandle(const ScopedSqliteHandle&) = delete;
    ScopedSqliteHandle& operator=(const ScopedSqliteHandle&) = delete;

    sqlite3* value = nullptr;
};

std::string UnifiedLegacySchemaSql(int version, bool include_due_index) {
    std::string sql =
        "CREATE TABLE schema_info(version INTEGER NOT NULL);"
        "INSERT INTO schema_info VALUES(" + std::to_string(version) + ");";
    sql += R"sql(
        CREATE TABLE patents (
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
        );
        CREATE TABLE oa_records (
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
        );
        CREATE TABLE prosecution_documents (
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
        );
        CREATE INDEX idx_prosecution_docs_patent ON prosecution_documents(patent_id);
        CREATE UNIQUE INDEX idx_prosecution_docs_event_key
            ON prosecution_documents(patent_id,event_key) WHERE event_key <> '';
        CREATE TABLE dossier_sync_state (
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
        );
    )sql";
    if (include_due_index) {
        sql += "CREATE INDEX idx_patents_next_dossier_check "
               "ON patents(next_dossier_check_at);";
    }
    return sql;
}

void CheckUnifiedV6Shape(sqlite3* db) {
    for (const char* column : {
             "geke_code", "application_number", "title", "proposal_name",
             "application_status", "patent_type", "geke_handler", "technology_route",
             "project_id", "publication_number", "last_dossier_check_at",
             "next_dossier_check_at"}) {
        CHECK(SqliteHasColumn(db, "patents", column));
    }
    for (const char* column : {
             "patent_id", "geke_code", "patent_title", "oa_type", "official_deadline",
             "issue_date", "response_date", "handler", "writer", "progress", "agency",
             "oa_summary", "notes", "jurisdiction", "source", "deadline_source",
             "remote_document_id", "sync_flag"}) {
        CHECK(SqliteHasColumn(db, "oa_records", column));
    }
    for (const char* column : {
             "patent_id", "jurisdiction", "application_number", "publication_number",
             "source", "remote_document_id", "document_type", "document_title",
             "official_date", "direction", "source_url", "download_url",
             "download_available", "fingerprint", "first_seen_at", "last_seen_at",
             "local_path", "downloaded_at", "raw_metadata", "raw_title", "document_code",
             "document_version", "event_key", "source_trace"}) {
        CHECK(SqliteHasColumn(db, "prosecution_documents", column));
    }
    for (const char* column : {
             "patent_id", "provider", "last_checked_at", "last_success_at", "last_error_at",
             "last_error_code", "last_error_message", "latest_remote_oa_date",
             "latest_remote_oa_type", "auth_state", "latest_applicant_activity",
             "terminal_state", "reexamination_state"}) {
        CHECK(SqliteHasColumn(db, "dossier_sync_state", column));
    }
    CHECK(SqliteHasIndex(db, "idx_patents_next_dossier_check"));
    CHECK(SqliteHasIndex(db, "idx_prosecution_docs_event_key"));
}

struct AlterTableAuthorizerContext {
    int alter_table_calls = 0;
    int deny_on_call = 2;
};

int DenySecondAlterTable(void* raw_context, int action, const char*, const char*,
                         const char*, const char*) {
    if (action != SQLITE_ALTER_TABLE) return SQLITE_OK;
    auto* context = static_cast<AlterTableAuthorizerContext*>(raw_context);
    ++context->alter_table_calls;
    return context->alter_table_calls == context->deny_on_call ? SQLITE_DENY : SQLITE_OK;
}

class ScopedAlterTableAuthorizer {
public:
    ScopedAlterTableAuthorizer(sqlite3* db, AlterTableAuthorizerContext* context)
        : db_(db), status_(sqlite3_set_authorizer(db, DenySecondAlterTable, context)) {}

    ~ScopedAlterTableAuthorizer() {
        if (db_) sqlite3_set_authorizer(db_, nullptr, nullptr);
    }

    ScopedAlterTableAuthorizer(const ScopedAlterTableAuthorizer&) = delete;
    ScopedAlterTableAuthorizer& operator=(const ScopedAlterTableAuthorizer&) = delete;

    int status() const { return status_; }

private:
    sqlite3* db_;
    int status_;
};

struct InterruptStatementContext {
    sqlite3* db = nullptr;
    const char* statement = nullptr;
    bool interrupted = false;
};

int InterruptMatchingStatement(unsigned trace, void* raw_context, void*, void* raw_sql) {
    auto* context = static_cast<InterruptStatementContext*>(raw_context);
    if (trace != SQLITE_TRACE_STMT || context->interrupted || !raw_sql) return 0;
    const auto* sql = static_cast<const char*>(raw_sql);
    if (std::string(sql).find(context->statement) == std::string::npos) return 0;
    context->interrupted = true;
    sqlite3_interrupt(context->db);
    return 0;
}

} // namespace

TEST(database_fresh_schema_is_versioned) {
    ScopedMigrationDatabase fixture(TempDbPath("fresh"));
    const std::string& path = fixture.path;
    std::filesystem::remove(path);
    {
        Database db(path);
        CHECK(db.IsOpen());
        CHECK_EQ(db.SchemaVersion(), patx::kSchemaVersionCurrent);
        CHECK_EQ(db.SchemaVersion(), 6);
        CHECK(SqliteHasColumn(db.GetHandle(), "prosecution_documents", "raw_title"));
        CHECK(SqliteHasColumn(db.GetHandle(), "prosecution_documents", "document_code"));
        CHECK(SqliteHasColumn(db.GetHandle(), "prosecution_documents", "document_version"));
        CHECK(SqliteHasColumn(db.GetHandle(), "prosecution_documents", "event_key"));
        CHECK(SqliteHasColumn(db.GetHandle(), "prosecution_documents", "source_trace"));
        CHECK(SqliteHasIndex(db.GetHandle(), "idx_prosecution_docs_event_key"));
        CHECK(SqliteHasIndex(db.GetHandle(), "idx_patents_next_dossier_check"));
        CHECK(SqliteHasColumn(db.GetHandle(), "dossier_sync_state", "latest_applicant_activity"));
        CHECK(SqliteHasColumn(db.GetHandle(), "dossier_sync_state", "terminal_state"));
        CHECK(SqliteHasColumn(db.GetHandle(), "dossier_sync_state", "reexamination_state"));

        auto repeated = patx::RunSchemaMigrations(db.GetHandle(), path);
        CHECK(repeated.ok);
        CHECK_EQ(repeated.from_version, 6);
        CHECK_EQ(repeated.to_version, 6);
    }
    const std::string backup_prefix = std::filesystem::path(path).filename().string() +
                                      ".pre_migration_";
    for (const auto& entry : std::filesystem::directory_iterator(
             std::filesystem::path(path).parent_path())) {
        if (entry.path().filename().string().find(backup_prefix) == 0) {
            std::filesystem::remove(entry.path());
        }
    }
    std::filesystem::remove(path);
}

TEST(database_v4_to_current_migration_is_incremental_and_preserves_data) {
    ScopedMigrationDatabase fixture(TempDbPath("v4_to_current"));
    const std::string& path = fixture.path;
    std::filesystem::remove(path);
    {
        sqlite3* raw = nullptr;
        CHECK_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
        const char* sql =
            "CREATE TABLE schema_info (version INTEGER NOT NULL);"
            "INSERT INTO schema_info VALUES (4);"
            "CREATE TABLE patents(id INTEGER PRIMARY KEY, next_dossier_check_at INTEGER DEFAULT 0);"
            "CREATE TABLE prosecution_documents ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT, patent_id INTEGER, jurisdiction TEXT,"
            " application_number TEXT, publication_number TEXT, source TEXT,"
            " remote_document_id TEXT, document_type TEXT, document_title TEXT,"
            " official_date TEXT, direction TEXT, source_url TEXT, download_url TEXT,"
            " download_available INTEGER DEFAULT 0, fingerprint TEXT,"
            " first_seen_at INTEGER, last_seen_at INTEGER, local_path TEXT,"
            " downloaded_at INTEGER DEFAULT 0, raw_metadata TEXT,"
            " UNIQUE(source, application_number, fingerprint));"
            "CREATE TABLE dossier_sync_state("
            "patent_id INTEGER NOT NULL, provider TEXT NOT NULL,"
            "last_checked_at INTEGER DEFAULT 0, last_success_at INTEGER DEFAULT 0,"
            "last_error_at INTEGER DEFAULT 0, last_error_code TEXT, last_error_message TEXT,"
            "latest_remote_oa_date TEXT, latest_remote_oa_type TEXT,"
            "auth_state TEXT DEFAULT 'NOT_INITIALIZED', PRIMARY KEY(patent_id,provider));"
            "INSERT INTO prosecution_documents (patent_id, application_number, source,"
            " remote_document_id, document_title, official_date, fingerprint, raw_metadata)"
            " VALUES (7, '202410000001.1', 'cnipa', 'remote-old', '旧通知书',"
            " '2025-01-02', 'fp-old', '{\"old\":true}');";
        char* error = nullptr;
        CHECK_EQ(sqlite3_exec(raw, sql, nullptr, nullptr, &error), SQLITE_OK);
        if (error) sqlite3_free(error);
        sqlite3_close(raw);
    }

    {
        Database db(path);
        CHECK(db.IsOpen());
        CHECK_EQ(db.SchemaVersion(), 6);
        CHECK(SqliteHasColumn(db.GetHandle(), "prosecution_documents", "raw_title"));
        CHECK(SqliteHasColumn(db.GetHandle(), "prosecution_documents", "document_code"));
        CHECK(SqliteHasColumn(db.GetHandle(), "prosecution_documents", "document_version"));
        CHECK(SqliteHasColumn(db.GetHandle(), "prosecution_documents", "event_key"));
        CHECK(SqliteHasColumn(db.GetHandle(), "prosecution_documents", "source_trace"));
        CHECK(SqliteHasIndex(db.GetHandle(), "idx_prosecution_docs_event_key"));
        CHECK(SqliteHasIndex(db.GetHandle(), "idx_patents_next_dossier_check"));
        CHECK(SqliteHasColumn(db.GetHandle(), "dossier_sync_state", "latest_applicant_activity"));
        CHECK(SqliteHasColumn(db.GetHandle(), "dossier_sync_state", "terminal_state"));
        CHECK(SqliteHasColumn(db.GetHandle(), "dossier_sync_state", "reexamination_state"));

        ProsecutionDocumentRecord migrated = db.GetProsecutionDocumentById(1);
        CHECK_EQ(migrated.id, 1);
        CHECK_EQ(migrated.patent_id, 7);
        CHECK_STR_EQ(migrated.source, "cnipa");
        CHECK_STR_EQ(migrated.remote_document_id, "remote-old");
        CHECK_STR_EQ(migrated.document_title, "旧通知书");
        CHECK_STR_EQ(migrated.raw_metadata, "{\"old\":true}");
        CHECK_STR_EQ(migrated.raw_title, "");
        CHECK_STR_EQ(migrated.document_code, "");
        CHECK_STR_EQ(migrated.document_version, "ORIGINAL");
        CHECK_STR_EQ(migrated.event_key, "");
        CHECK_STR_EQ(migrated.source_trace, "");

        auto repeated = patx::RunSchemaMigrations(db.GetHandle(), path);
        CHECK(repeated.ok);
        CHECK_EQ(repeated.from_version, 6);
        CHECK_EQ(repeated.to_version, 6);
    }
    const std::string v4_backup_prefix = std::filesystem::path(path).filename().string() +
                                         ".pre_migration_";
    for (const auto& entry : std::filesystem::directory_iterator(
             std::filesystem::path(path).parent_path())) {
        if (entry.path().filename().string().find(v4_backup_prefix) == 0) {
            std::filesystem::remove(entry.path());
        }
    }
    std::filesystem::remove(path);
}

TEST(database_constructor_backs_up_v4_before_initializing_current_tables) {
    ScopedMigrationDatabase fixture(TempDbPath("constructor_v4_backup_order"));
    const std::string& path = fixture.path;
    std::filesystem::remove(path);
    const std::string backup_prefix = std::filesystem::path(path).filename().string() +
                                      ".pre_migration_";
    for (const auto& entry : std::filesystem::directory_iterator(
             std::filesystem::path(path).parent_path())) {
        if (entry.path().filename().string().find(backup_prefix) == 0) {
            std::filesystem::remove(entry.path());
        }
    }
    sqlite3* raw = nullptr;
    CHECK_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    const char* schema =
        "CREATE TABLE schema_info(version INTEGER NOT NULL);"
        "INSERT INTO schema_info VALUES(4);"
        "CREATE TABLE patents(id INTEGER PRIMARY KEY, next_dossier_check_at INTEGER DEFAULT 0);"
        "CREATE TABLE prosecution_documents("
        "id INTEGER PRIMARY KEY, patent_id INTEGER, source TEXT, application_number TEXT,"
        "fingerprint TEXT, UNIQUE(source,application_number,fingerprint));"
        "CREATE TABLE dossier_sync_state("
        "patent_id INTEGER NOT NULL, provider TEXT NOT NULL,"
        "last_checked_at INTEGER DEFAULT 0, last_success_at INTEGER DEFAULT 0,"
        "last_error_at INTEGER DEFAULT 0, last_error_code TEXT, last_error_message TEXT,"
        "latest_remote_oa_date TEXT, latest_remote_oa_type TEXT,"
        "auth_state TEXT DEFAULT 'NOT_INITIALIZED', PRIMARY KEY(patent_id,provider));"
        "CREATE TABLE v4_marker(value TEXT);"
        "INSERT INTO v4_marker VALUES('before-init');";
    CHECK_EQ(sqlite3_exec(raw, schema, nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(raw);

    {
        Database db(path);
        CHECK(db.IsOpen());
        CHECK_EQ(db.SchemaVersion(), 6);
        CHECK(SqliteHasIndex(db.GetHandle(), "idx_patents_next_dossier_check"));
        CHECK(SqliteHasColumn(db.GetHandle(), "dossier_sync_state", "latest_applicant_activity"));
        CHECK(SqliteHasColumn(db.GetHandle(), "dossier_sync_state", "terminal_state"));
        CHECK(SqliteHasColumn(db.GetHandle(), "dossier_sync_state", "reexamination_state"));
    }

    std::filesystem::path backup_path;
    for (const auto& entry : std::filesystem::directory_iterator(
             std::filesystem::path(path).parent_path())) {
        if (entry.path().filename().string().find(backup_prefix) == 0) {
            backup_path = entry.path();
            break;
        }
    }
    CHECK(!backup_path.empty());
    sqlite3* backup = nullptr;
    CHECK_EQ(sqlite3_open_v2(backup_path.string().c_str(), &backup,
                             SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    CHECK_EQ(patx::ReadSchemaVersion(backup), 4);
    CHECK(!SqliteHasColumn(backup, "prosecution_documents", "event_key"));
    sqlite3_stmt* added_table = nullptr;
    CHECK_EQ(sqlite3_prepare_v2(backup,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='oa_records'",
        -1, &added_table, nullptr), SQLITE_OK);
    CHECK(sqlite3_step(added_table) == SQLITE_DONE);
    sqlite3_finalize(added_table);
    sqlite3_close(backup);

    if (!backup_path.empty()) std::filesystem::remove(backup_path);
    std::filesystem::remove(path);
}

TEST(database_constructor_backup_failure_does_not_initialize_or_migrate_v4) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("patx_constructor_backup_failure_" + std::to_string(
            static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
    std::filesystem::create_directories(directory);
    const std::string path = (directory / "patents.db").string();
    sqlite3* raw = nullptr;
    CHECK_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    const char* schema =
        "CREATE TABLE schema_info(version INTEGER NOT NULL);"
        "INSERT INTO schema_info VALUES(4);"
        "CREATE TABLE patents(id INTEGER PRIMARY KEY, next_dossier_check_at INTEGER DEFAULT 0);"
        "CREATE TABLE prosecution_documents("
        "id INTEGER PRIMARY KEY, patent_id INTEGER, source TEXT, application_number TEXT,"
        "fingerprint TEXT, UNIQUE(source,application_number,fingerprint));";
    CHECK_EQ(sqlite3_exec(raw, schema, nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(raw);

    auto read_file = [](const std::string& file_path) {
        std::ifstream stream(file_path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream),
                           std::istreambuf_iterator<char>());
    };
    const std::string bytes_before = read_file(path);

    std::filesystem::permissions(directory,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace);
    {
        Database db(path);
        CHECK(!db.IsOpen());
        CHECK(db.GetHandle() == nullptr);
        CHECK_EQ(db.SchemaVersion(), 4);
        CHECK(!db.LastError().empty());
    }
    CHECK_STR_EQ(read_file(path), bytes_before);
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);

    sqlite3* unchanged = nullptr;
    CHECK_EQ(sqlite3_open_v2(path.c_str(), &unchanged, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    CHECK_EQ(patx::ReadSchemaVersion(unchanged), 4);
    CHECK(!SqliteHasColumn(unchanged, "prosecution_documents", "event_key"));
    sqlite3_stmt* added_table = nullptr;
    CHECK_EQ(sqlite3_prepare_v2(unchanged,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='oa_records'",
        -1, &added_table, nullptr), SQLITE_OK);
    CHECK(sqlite3_step(added_table) == SQLITE_DONE);
    sqlite3_finalize(added_table);
    sqlite3_close(unchanged);
    std::filesystem::remove_all(directory);
}

TEST(database_reopens_v6_and_repairs_missing_due_index) {
    ScopedMigrationDatabase fixture(TempDbPath("v6_due_index_repair"));
    const std::string& path = fixture.path;
    std::filesystem::remove(path);
    {
        Database db(path);
        CHECK_EQ(db.SchemaVersion(), 6);
        CHECK(db.Execute("DROP INDEX idx_patents_next_dossier_check"));
        CHECK(!SqliteHasIndex(db.GetHandle(), "idx_patents_next_dossier_check"));
    }
    {
        Database reopened(path);
        CHECK_EQ(reopened.SchemaVersion(), 6);
        CHECK(SqliteHasIndex(reopened.GetHandle(), "idx_patents_next_dossier_check"));
    }
    std::filesystem::remove(path);
}

TEST(database_v6_repairs_indexes_with_extra_partial_predicates) {
    ScopedMigrationDatabase fixture(TempDbPath("v6_partial_index_repair"));
    Database db(fixture.path);
    CHECK(db.IsOpen());
    CHECK_EQ(db.SchemaVersion(), 6);
    CHECK(db.Execute("DROP INDEX idx_prosecution_docs_event_key;"
                     "CREATE UNIQUE INDEX idx_prosecution_docs_event_key "
                     "ON prosecution_documents(patent_id,event_key) "
                     "WHERE event_key <> '' AND source='cnipa';"
                     "DROP INDEX idx_patents_next_dossier_check;"
                     "CREATE INDEX idx_patents_next_dossier_check "
                     "ON patents(next_dossier_check_at) "
                     "WHERE next_dossier_check_at > 0;"));

    const auto migration = patx::RunSchemaMigrations(db.GetHandle(), fixture.path);
    CHECK(migration.ok);
    CHECK(migration.performed_backup);
    CHECK(std::filesystem::exists(migration.backup_path));
    CHECK_EQ(patx::ReadSchemaVersion(db.GetHandle()), 6);
    CHECK(SqliteIndexColumns(db.GetHandle(), "idx_prosecution_docs_event_key") ==
          std::vector<std::string>({"patent_id", "event_key"}));
    CHECK_STR_EQ(
        CompactIndexSql(SqliteIndexSql(db.GetHandle(), "idx_prosecution_docs_event_key")),
        "createuniqueindexidx_prosecution_docs_event_keyonprosecution_documents"
        "(patent_id,event_key)whereevent_key<>''");
    CHECK(SqliteIndexColumns(db.GetHandle(), "idx_patents_next_dossier_check") ==
          std::vector<std::string>({"next_dossier_check_at"}));
    CHECK_STR_EQ(
        CompactIndexSql(SqliteIndexSql(db.GetHandle(), "idx_patents_next_dossier_check")),
        "createindexidx_patents_next_dossier_checkonpatents(next_dossier_check_at)");
}

TEST(database_v6_repairs_event_index_with_expression_and_nocase_collation) {
    ScopedMigrationDatabase fixture(TempDbPath("v6_expression_index_repair"));
    Database db(fixture.path);
    CHECK(db.IsOpen());
    CHECK_EQ(db.SchemaVersion(), 6);
    CHECK(db.Execute("DROP INDEX idx_prosecution_docs_event_key;"
                     "CREATE UNIQUE INDEX idx_prosecution_docs_event_key "
                     "ON prosecution_documents(patent_id COLLATE NOCASE,event_key,lower(source)) "
                     "WHERE event_key <> '';"));

    const auto migration = patx::RunSchemaMigrations(db.GetHandle(), fixture.path);
    CHECK(migration.ok);
    CHECK(migration.performed_backup);
    CHECK(std::filesystem::exists(migration.backup_path));
    CHECK_EQ(patx::ReadSchemaVersion(db.GetHandle()), 6);
    CHECK_STR_EQ(
        CompactIndexSql(SqliteIndexSql(db.GetHandle(), "idx_prosecution_docs_event_key")),
        "createuniqueindexidx_prosecution_docs_event_keyonprosecution_documents"
        "(patent_id,event_key)whereevent_key<>''");
}

TEST(database_codex_v5_to_v6_preserves_data_and_is_idempotent) {
    ScopedMigrationDatabase fixture(TempDbPath("codex_v5_to_v6"));
    ScopedSqliteHandle database;
    CHECK_EQ(sqlite3_open(fixture.path.c_str(), &database.value), SQLITE_OK);
    const std::string schema = UnifiedLegacySchemaSql(5, true) + R"sql(
        INSERT INTO patents(
            id,geke_code,application_number,title,application_status,patent_type,geke_handler,
            technology_route,project_id,publication_number,last_dossier_check_at,
            next_dossier_check_at)
        VALUES(41,'GK-CODEX-V5','202610000001.2','Codex保留案卷','pending','invention',
               '王五','芯片封装','PROJECT-CODEX','CN119900001A',1700000000,1700000900);
        INSERT INTO oa_records(
            id,patent_id,geke_code,patent_title,oa_type,official_deadline,issue_date,response_date,
            handler,writer,progress,agency,oa_summary,notes,jurisdiction,source,deadline_source,
            remote_document_id,sync_flag)
        VALUES(51,41,'GK-CODEX-V5','Codex保留案卷','第一次审查意见通知书','2027-01-01',
               '2026-09-01','','张三','赵六','drafting','测试代理所','创造性','人工备注',
               'CN','cnipa','manual','oa-codex-51','manual_reviewed');
        INSERT INTO prosecution_documents(
            id,patent_id,jurisdiction,application_number,publication_number,source,
            remote_document_id,document_type,document_title,raw_title,document_code,
            document_version,official_date,direction,source_url,download_url,download_available,
            fingerprint,event_key,source_trace,first_seen_at,last_seen_at,local_path,downloaded_at,
            raw_metadata)
        VALUES(61,41,'CN','202610000001.2','CN119900001A','cnipa','doc-codex-61',
               'office_action','第一次审查意见通知书','第一次审查意见通知书（原始）','OA-1',
               'ORIGINAL','2026-09-01','incoming','https://example.test/source/61',
               'https://example.test/download/61',1,'fp-codex-v5',
               'cn:202610000001.2:office-action:2026-09-01','epo_global_dossier,cnipa',
               1700000010,1700000020,'/tmp/codex-oa-61.pdf',1700000030,
               '{"fixture":"codex-v5"}');
        INSERT INTO dossier_sync_state(
            patent_id,provider,last_checked_at,last_success_at,last_error_at,last_error_code,
            last_error_message,latest_remote_oa_date,latest_remote_oa_type,auth_state)
        VALUES(41,'cnipa',1700000100,1700000050,1700000001,'OLD_WARNING','保留同步状态',
               '2026-09-01','第一次审查意见通知书','AUTHENTICATED');
    )sql";
    CHECK_EQ(sqlite3_exec(database.value, schema.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    CHECK(SqliteHasIndex(database.value, "idx_patents_next_dossier_check"));

    auto migration = patx::RunSchemaMigrations(database.value, fixture.path);
    CHECK(migration.ok);
    CHECK_EQ(migration.from_version, 5);
    CHECK_EQ(migration.to_version, 6);
    CHECK(migration.performed_backup);
    CHECK(!migration.backup_path.empty());
    CHECK(std::filesystem::exists(migration.backup_path));
    CHECK_EQ(patx::ReadSchemaVersion(database.value), 6);
    CheckUnifiedV6Shape(database.value);

    ScopedSqliteHandle backup;
    CHECK_EQ(sqlite3_open_v2(migration.backup_path.c_str(), &backup.value,
                             SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    CHECK_EQ(patx::ReadSchemaVersion(backup.value), 5);
    CHECK(!SqliteHasColumn(backup.value, "dossier_sync_state", "terminal_state"));

    auto repeated = patx::RunSchemaMigrations(database.value, fixture.path);
    CHECK(repeated.ok);
    CHECK_EQ(repeated.from_version, 6);
    CHECK_EQ(repeated.to_version, 6);
    CHECK(!repeated.performed_backup);
    CHECK_EQ(patx::ReadSchemaVersion(database.value), 6);
    CheckUnifiedV6Shape(database.value);

    ScopedSqliteStatement preserved;
    CHECK_EQ(sqlite3_prepare_v2(database.value,
        "SELECT p.geke_code,p.title,p.next_dossier_check_at,"
        "o.handler,o.writer,o.deadline_source,o.source,o.remote_document_id,o.sync_flag,"
        "d.local_path,d.downloaded_at,d.raw_title,d.document_code,d.document_version,"
        "d.event_key,d.source_trace,s.last_checked_at,s.last_error_message,s.auth_state "
        "FROM patents p JOIN oa_records o ON o.patent_id=p.id "
        "JOIN prosecution_documents d ON d.patent_id=p.id "
        "JOIN dossier_sync_state s ON s.patent_id=p.id WHERE p.id=41",
        -1, preserved.out(), nullptr), SQLITE_OK);
    CHECK_EQ(sqlite3_step(preserved.get()), SQLITE_ROW);
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 0)),
                 "GK-CODEX-V5");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 1)),
                 "Codex保留案卷");
    CHECK_EQ(sqlite3_column_int64(preserved.get(), 2), 1700000900);
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 3)), "张三");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 4)), "赵六");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 5)), "manual");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 6)), "cnipa");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 7)),
                 "oa-codex-51");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 8)),
                 "manual_reviewed");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 9)),
                 "/tmp/codex-oa-61.pdf");
    CHECK_EQ(sqlite3_column_int64(preserved.get(), 10), 1700000030);
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 11)),
                 "第一次审查意见通知书（原始）");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 12)), "OA-1");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 13)),
                 "ORIGINAL");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 14)),
                 "cn:202610000001.2:office-action:2026-09-01");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 15)),
                 "epo_global_dossier,cnipa");
    CHECK_EQ(sqlite3_column_int64(preserved.get(), 16), 1700000100);
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 17)),
                 "保留同步状态");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 18)),
                 "AUTHENTICATED");
}

TEST(database_glm_v5_to_v6_repairs_missing_codex_shape) {
    ScopedMigrationDatabase fixture(TempDbPath("glm_v5_to_v6"));
    ScopedSqliteHandle database;
    CHECK_EQ(sqlite3_open(fixture.path.c_str(), &database.value), SQLITE_OK);
    const std::string schema = UnifiedLegacySchemaSql(5, false) + R"sql(
        INSERT INTO patents(
            id,geke_code,application_number,title,application_status,patent_type,geke_handler,
            technology_route,project_id,publication_number,last_dossier_check_at,
            next_dossier_check_at)
        VALUES(71,'GK-GLM-V5','US18990001','GLM保留案卷','pending','invention','钱七',
               '电池材料','PROJECT-GLM','US2026990001A1',1701000000,1701000900);
        INSERT INTO oa_records(
            id,patent_id,geke_code,patent_title,oa_type,official_deadline,issue_date,response_date,
            handler,writer,progress,agency,oa_summary,notes,jurisdiction,source,deadline_source,
            remote_document_id,sync_flag)
        VALUES(72,71,'GK-GLM-V5','GLM保留案卷','Non-Final Rejection','2026-11-20',
               '2026-08-20','','李四','周八','review','GLM Agency','102 rejection','manual note',
               'US','uspto','manual','oa-glm-72','confirmed');
        INSERT INTO prosecution_documents(
            id,patent_id,jurisdiction,application_number,publication_number,source,
            remote_document_id,document_type,document_title,raw_title,document_code,
            document_version,official_date,direction,source_url,download_url,download_available,
            fingerprint,event_key,source_trace,first_seen_at,last_seen_at,local_path,downloaded_at,
            raw_metadata)
        VALUES(73,71,'US','US18990001','US2026990001A1','uspto','doc-glm-73',
               'non_final_rejection','Non-Final Rejection','NON-FINAL REJECTION','CTNF',
               'ORIGINAL','2026-08-20','incoming','https://example.test/source/73',
               'https://example.test/download/73',1,'fp-glm-v5',
               'us:18990001:non-final:2026-08-20','uspto',1701000010,1701000020,
               '/tmp/glm-oa-73.pdf',1701000030,'{"fixture":"glm-v5"}');
        INSERT INTO dossier_sync_state(
            patent_id,provider,last_checked_at,last_success_at,last_error_at,last_error_code,
            last_error_message,latest_remote_oa_date,latest_remote_oa_type,auth_state)
        VALUES(71,'uspto',1701000100,1701000050,1701000001,'STALE_TOKEN','保留GLM同步状态',
               '2026-08-20','Non-Final Rejection','AUTHENTICATED');
    )sql";
    CHECK_EQ(sqlite3_exec(database.value, schema.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    CHECK(!SqliteHasIndex(database.value, "idx_patents_next_dossier_check"));
    CHECK(SqliteHasIndex(database.value, "idx_prosecution_docs_event_key"));

    auto migration = patx::RunSchemaMigrations(database.value, fixture.path);
    CHECK(migration.ok);
    CHECK_EQ(migration.from_version, 5);
    CHECK_EQ(migration.to_version, 6);
    CHECK(migration.performed_backup);
    CHECK(std::filesystem::exists(migration.backup_path));
    CHECK_EQ(patx::ReadSchemaVersion(database.value), 6);
    CheckUnifiedV6Shape(database.value);

    auto repeated = patx::RunSchemaMigrations(database.value, fixture.path);
    CHECK(repeated.ok);
    CHECK_EQ(repeated.from_version, 6);
    CHECK_EQ(repeated.to_version, 6);
    CHECK(!repeated.performed_backup);
    CHECK_EQ(patx::ReadSchemaVersion(database.value), 6);
    CheckUnifiedV6Shape(database.value);

    ScopedSqliteStatement preserved;
    CHECK_EQ(sqlite3_prepare_v2(database.value,
        "SELECT p.geke_code,p.title,p.next_dossier_check_at,"
        "o.handler,o.writer,o.deadline_source,o.source,o.remote_document_id,o.sync_flag,"
        "d.local_path,d.downloaded_at,d.raw_title,d.document_code,d.document_version,"
        "d.event_key,d.source_trace,s.last_checked_at,s.last_error_message,s.auth_state "
        "FROM patents p JOIN oa_records o ON o.patent_id=p.id "
        "JOIN prosecution_documents d ON d.patent_id=p.id "
        "JOIN dossier_sync_state s ON s.patent_id=p.id WHERE p.id=71",
        -1, preserved.out(), nullptr), SQLITE_OK);
    CHECK_EQ(sqlite3_step(preserved.get()), SQLITE_ROW);
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 0)),
                 "GK-GLM-V5");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 1)),
                 "GLM保留案卷");
    CHECK_EQ(sqlite3_column_int64(preserved.get(), 2), 1701000900);
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 3)), "李四");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 4)), "周八");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 5)), "manual");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 6)), "uspto");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 7)),
                 "oa-glm-72");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 8)),
                 "confirmed");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 9)),
                 "/tmp/glm-oa-73.pdf");
    CHECK_EQ(sqlite3_column_int64(preserved.get(), 10), 1701000030);
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 11)),
                 "NON-FINAL REJECTION");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 12)), "CTNF");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 13)),
                 "ORIGINAL");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 14)),
                 "us:18990001:non-final:2026-08-20");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 15)), "uspto");
    CHECK_EQ(sqlite3_column_int64(preserved.get(), 16), 1701000100);
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 17)),
                 "保留GLM同步状态");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 18)),
                 "AUTHENTICATED");
}

TEST(database_reopens_v6_and_repairs_missing_required_shape) {
    ScopedMigrationDatabase fixture(TempDbPath("v6_required_shape_repair"));
    {
        ScopedSqliteHandle created;
        CHECK_EQ(sqlite3_open(fixture.path.c_str(), &created.value), SQLITE_OK);
        const std::string schema = UnifiedLegacySchemaSql(6, true) +
            "ALTER TABLE dossier_sync_state ADD COLUMN latest_applicant_activity TEXT DEFAULT '';"
            "ALTER TABLE dossier_sync_state ADD COLUMN reexamination_state TEXT DEFAULT '';"
            "INSERT INTO dossier_sync_state "
            "(patent_id,provider,latest_applicant_activity,reexamination_state) "
            "VALUES(81,'cnipa','2026-09-20','pending');";
        CHECK_EQ(sqlite3_exec(created.value, schema.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
        CHECK(!SqliteHasColumn(created.value, "dossier_sync_state", "terminal_state"));
    }

    ScopedSqliteHandle reopened;
    CHECK_EQ(sqlite3_open(fixture.path.c_str(), &reopened.value), SQLITE_OK);
    auto migration = patx::RunSchemaMigrations(reopened.value, fixture.path);
    CHECK(migration.ok);
    CHECK_EQ(migration.from_version, 6);
    CHECK_EQ(migration.to_version, 6);
    CHECK(SqliteHasColumn(reopened.value, "dossier_sync_state", "terminal_state"));
    CHECK(migration.performed_backup);
    CHECK(!migration.backup_path.empty());
    CHECK(std::filesystem::exists(migration.backup_path));
    CHECK_EQ(patx::ReadSchemaVersion(reopened.value), 6);
    CheckUnifiedV6Shape(reopened.value);

    ScopedSqliteStatement preserved;
    CHECK_EQ(sqlite3_prepare_v2(reopened.value,
        "SELECT latest_applicant_activity,reexamination_state FROM dossier_sync_state "
        "WHERE patent_id=81 AND provider='cnipa'", -1, preserved.out(), nullptr), SQLITE_OK);
    CHECK_EQ(sqlite3_step(preserved.get()), SQLITE_ROW);
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 0)),
                 "2026-09-20");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 1)),
                 "pending");

    ScopedSqliteHandle backup;
    CHECK_EQ(sqlite3_open_v2(migration.backup_path.c_str(), &backup.value,
                             SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    CHECK_EQ(patx::ReadSchemaVersion(backup.value), 6);
    CHECK(!SqliteHasColumn(backup.value, "dossier_sync_state", "terminal_state"));
}

TEST(database_v6_missing_dossier_unique_key_fails_without_rebuilding_table) {
    ScopedMigrationDatabase fixture(TempDbPath("v6_missing_dossier_unique"));
    ScopedSqliteHandle database;
    CHECK_EQ(sqlite3_open(fixture.path.c_str(), &database.value), SQLITE_OK);
    std::string schema = UnifiedLegacySchemaSql(6, true);
    const std::string primary_key = "PRIMARY KEY (patent_id, provider)";
    const size_t primary_key_pos = schema.find(primary_key);
    CHECK(primary_key_pos != std::string::npos);
    schema.replace(primary_key_pos, primary_key.size(), "CHECK (patent_id >= 0)");
    schema +=
        "ALTER TABLE dossier_sync_state ADD COLUMN latest_applicant_activity TEXT DEFAULT '';"
        "ALTER TABLE dossier_sync_state ADD COLUMN terminal_state TEXT DEFAULT '';"
        "ALTER TABLE dossier_sync_state ADD COLUMN reexamination_state TEXT DEFAULT '';"
        "INSERT INTO dossier_sync_state("
        "patent_id,provider,last_error_message,latest_applicant_activity,terminal_state,"
        "reexamination_state) VALUES(86,'cnipa','preserve-without-key','2026-10-01',"
        "'PENDING','REQUEST_PERIOD');";
    CHECK_EQ(sqlite3_exec(database.value, schema.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);

    const auto migration = patx::RunSchemaMigrations(database.value, fixture.path);
    CHECK(!migration.ok);
    CHECK(migration.performed_backup);
    CHECK(std::filesystem::exists(migration.backup_path));
    CHECK(migration.error.find("dossier_sync_state") != std::string::npos);
    CHECK_EQ(patx::ReadSchemaVersion(database.value), 6);
    CHECK(sqlite3_get_autocommit(database.value) != 0);

    ScopedSqliteStatement preserved;
    CHECK_EQ(sqlite3_prepare_v2(database.value,
        "SELECT last_error_message,terminal_state FROM dossier_sync_state "
        "WHERE patent_id=86 AND provider='cnipa'", -1, preserved.out(), nullptr), SQLITE_OK);
    CHECK_EQ(sqlite3_step(preserved.get()), SQLITE_ROW);
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 0)),
                 "preserve-without-key");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 1)),
                 "PENDING");
}

TEST(database_v6_rejects_non_rowid_id_primary_keys_without_rebuilding_tables) {
    struct InvalidPrimaryKeyCase {
        const char* table;
        const char* declaration;
        const char* id_value;
        const char* marker_column;
        bool interrupt_index_list;
    };
    const InvalidPrimaryKeyCase cases[] = {
        {"patents", "id TEXT PRIMARY KEY", "patents-text-id", "title", false},
        {"oa_records", "id TEXT PRIMARY KEY", "oa-text-id", "notes", false},
        {"prosecution_documents", "id TEXT PRIMARY KEY", "document-text-id",
         "document_title", false},
        {"patents", "id INTEGER PRIMARY KEY DESC", "606", "title", false},
        {"patents", "id INTEGER PRIMARY KEY DESC", "607", "title", true},
    };

    for (const auto& test_case : cases) {
        const std::string fixture_name =
            std::string("v6_non_rowid_pk_") + test_case.table + "_" + test_case.id_value;
        ScopedMigrationDatabase fixture(TempDbPath(fixture_name.c_str()));
        ScopedSqliteHandle database;
        CHECK_EQ(sqlite3_open(fixture.path.c_str(), &database.value), SQLITE_OK);

        std::string schema = UnifiedLegacySchemaSql(6, true);
        const std::string table_prefix =
            std::string("CREATE TABLE ") + test_case.table + " (";
        const size_t table_pos = schema.find(table_prefix);
        CHECK(table_pos != std::string::npos);
        const std::string valid_declaration = "id INTEGER PRIMARY KEY AUTOINCREMENT";
        const size_t id_pos = schema.find(valid_declaration, table_pos);
        CHECK(id_pos != std::string::npos);
        schema.replace(id_pos, valid_declaration.size(), test_case.declaration);
        schema +=
            "ALTER TABLE dossier_sync_state ADD COLUMN latest_applicant_activity "
            "TEXT DEFAULT '';"
            "ALTER TABLE dossier_sync_state ADD COLUMN terminal_state TEXT DEFAULT '';"
            "ALTER TABLE dossier_sync_state ADD COLUMN reexamination_state TEXT DEFAULT '';";
        const std::string quoted_id =
            std::string(test_case.declaration).find("TEXT") == std::string::npos
                ? test_case.id_value
                : std::string("'") + test_case.id_value + "'";
        schema += "INSERT INTO " + std::string(test_case.table) + "(id," +
                  test_case.marker_column + ") VALUES(" + quoted_id +
                  ",'preserve-non-rowid-pk');";
        CHECK_EQ(sqlite3_exec(database.value, schema.c_str(), nullptr, nullptr, nullptr),
                 SQLITE_OK);

        InterruptStatementContext interrupt_context{
            database.value, "PRAGMA index_list(patents)", false};
        if (test_case.interrupt_index_list) {
            CHECK_EQ(sqlite3_trace_v2(database.value, SQLITE_TRACE_STMT,
                                      InterruptMatchingStatement, &interrupt_context), SQLITE_OK);
        }
        const auto migration = patx::RunSchemaMigrations(database.value, fixture.path);
        if (test_case.interrupt_index_list) {
            CHECK_EQ(sqlite3_trace_v2(database.value, 0, nullptr, nullptr), SQLITE_OK);
            CHECK(interrupt_context.interrupted);
        }
        CHECK(!migration.ok);
        CHECK(migration.performed_backup);
        CHECK(std::filesystem::exists(migration.backup_path));
        CHECK(migration.error.find("PRIMARY KEY(id)") != std::string::npos);
        CHECK_EQ(patx::ReadSchemaVersion(database.value), 6);
        CHECK(sqlite3_get_autocommit(database.value) != 0);

        ScopedSqliteStatement preserved;
        const std::string select = "SELECT CAST(id AS TEXT) FROM " +
            std::string(test_case.table) + " WHERE " + test_case.marker_column +
            "='preserve-non-rowid-pk'";
        CHECK_EQ(sqlite3_prepare_v2(database.value, select.c_str(), -1, preserved.out(),
                                    nullptr), SQLITE_OK);
        CHECK_EQ(sqlite3_step(preserved.get()), SQLITE_ROW);
        CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 0)),
                     test_case.id_value);
    }
}

TEST(database_v6_repair_failure_rolls_back_and_keeps_version) {
    ScopedMigrationDatabase fixture(TempDbPath("v6_repair_rollback"));
    ScopedSqliteHandle database;
    CHECK_EQ(sqlite3_open(fixture.path.c_str(), &database.value), SQLITE_OK);
    const std::string schema = UnifiedLegacySchemaSql(6, true) + R"sql(
        INSERT INTO dossier_sync_state(
            patent_id,provider,last_checked_at,last_success_at,last_error_at,last_error_code,
            last_error_message,latest_remote_oa_date,latest_remote_oa_type,auth_state)
        VALUES(91,'cnipa',1702000100,1702000050,1702000001,'KEEP_CODE',
               'keep-after-failed-repair','2026-09-30','驳回决定','AUTHENTICATED');
    )sql";
    CHECK_EQ(sqlite3_exec(database.value, schema.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    CHECK(!SqliteHasColumn(database.value, "dossier_sync_state", "latest_applicant_activity"));
    CHECK(!SqliteHasColumn(database.value, "dossier_sync_state", "terminal_state"));

    AlterTableAuthorizerContext authorizer_context;
    patx::SchemaMigrationResult migration;
    {
        ScopedAlterTableAuthorizer authorizer(database.value, &authorizer_context);
        CHECK_EQ(authorizer.status(), SQLITE_OK);
        migration = patx::RunSchemaMigrations(database.value, fixture.path);
    }
    CHECK(!migration.ok);
    CHECK(!migration.error.empty());
    CHECK_EQ(authorizer_context.alter_table_calls, 2);
    CHECK_EQ(migration.from_version, 6);
    CHECK_EQ(patx::ReadSchemaVersion(database.value), 6);
    CHECK(!SqliteHasColumn(database.value, "dossier_sync_state", "latest_applicant_activity"));
    CHECK(!SqliteHasColumn(database.value, "dossier_sync_state", "terminal_state"));
    CHECK(!SqliteHasColumn(database.value, "dossier_sync_state", "reexamination_state"));
    CHECK(sqlite3_get_autocommit(database.value) != 0);

    ScopedSqliteStatement marker;
    CHECK_EQ(sqlite3_prepare_v2(database.value,
        "SELECT last_error_message FROM dossier_sync_state WHERE patent_id=91 AND provider='cnipa'",
        -1, marker.out(), nullptr), SQLITE_OK);
    CHECK_EQ(sqlite3_step(marker.get()), SQLITE_ROW);
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(marker.get(), 0)),
                 "keep-after-failed-repair");
}

TEST(database_v6_unique_event_index_rebuild_failure_restores_original_index_and_rows) {
    ScopedMigrationDatabase fixture(TempDbPath("v6_event_index_rebuild_rollback"));
    Database db(fixture.path);
    CHECK(db.IsOpen());
    CHECK(db.Execute("DROP INDEX idx_prosecution_docs_event_key;"
                     "CREATE INDEX idx_prosecution_docs_event_key "
                     "ON prosecution_documents(patent_id,event_key) WHERE event_key <> '';"
                     "INSERT INTO prosecution_documents("
                     "patent_id,event_key,source,application_number,fingerprint) "
                     "VALUES(96,'duplicate-event','source-a','app-a','fp-a');"
                     "INSERT INTO prosecution_documents("
                     "patent_id,event_key,source,application_number,fingerprint) "
                     "VALUES(96,'duplicate-event','source-b','app-b','fp-b');"));
    const std::string original_index_sql = CompactIndexSql(
        SqliteIndexSql(db.GetHandle(), "idx_prosecution_docs_event_key"));

    const auto migration = patx::RunSchemaMigrations(db.GetHandle(), fixture.path);
    CHECK(!migration.ok);
    CHECK(migration.performed_backup);
    CHECK_EQ(patx::ReadSchemaVersion(db.GetHandle()), 6);
    CHECK(sqlite3_get_autocommit(db.GetHandle()) != 0);
    CHECK_STR_EQ(
        CompactIndexSql(SqliteIndexSql(db.GetHandle(), "idx_prosecution_docs_event_key")),
        original_index_sql);

    ScopedSqliteStatement rows;
    CHECK_EQ(sqlite3_prepare_v2(db.GetHandle(),
        "SELECT COUNT(*) FROM prosecution_documents "
        "WHERE patent_id=96 AND event_key='duplicate-event'",
        -1, rows.out(), nullptr), SQLITE_OK);
    CHECK_EQ(sqlite3_step(rows.get()), SQLITE_ROW);
    CHECK_EQ(sqlite3_column_int(rows.get(), 0), 2);
}

TEST(database_unversioned_v2_failure_rolls_back_schema_version_marker) {
    ScopedMigrationDatabase fixture(TempDbPath("unversioned_v2_marker_rollback"));
    ScopedSqliteHandle database;
    CHECK_EQ(sqlite3_open(fixture.path.c_str(), &database.value), SQLITE_OK);
    CHECK_EQ(sqlite3_exec(database.value, R"sql(
        CREATE TABLE patents (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            geke_code TEXT UNIQUE,
            title TEXT,
            technology_route TEXT
        );
        INSERT INTO patents(geke_code,title,technology_route)
        VALUES('GK-UNVERSIONED-V2','保留未版本化数据','legacy-v2-route');
    )sql", nullptr, nullptr, nullptr), SQLITE_OK);
    CHECK(!SqliteHasTable(database.value, "schema_info"));
    AlterTableAuthorizerContext authorizer_context;
    authorizer_context.deny_on_call = 1;
    patx::SchemaMigrationResult migration;
    {
        ScopedAlterTableAuthorizer authorizer(database.value, &authorizer_context);
        CHECK_EQ(authorizer.status(), SQLITE_OK);
        migration = patx::RunSchemaMigrations(database.value, fixture.path);
    }
    CHECK(!migration.ok);
    CHECK(migration.performed_backup);
    CHECK_EQ(migration.from_version, 0);
    CHECK_EQ(authorizer_context.alter_table_calls, 1);
    CHECK(!SqliteHasTable(database.value, "schema_info"));
    CHECK(!SqliteHasTable(database.value, "oa_records"));
    CHECK(!SqliteHasColumn(database.value, "patents", "publication_number"));
    CHECK_EQ(patx::ReadSchemaVersion(database.value), 0);
    CHECK(sqlite3_get_autocommit(database.value) != 0);

    ScopedSqliteStatement preserved;
    CHECK_EQ(sqlite3_prepare_v2(database.value,
        "SELECT title FROM patents WHERE geke_code='GK-UNVERSIONED-V2'",
        -1, preserved.out(), nullptr), SQLITE_OK);
    CHECK_EQ(sqlite3_step(preserved.get()), SQLITE_ROW);
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved.get(), 0)),
                 "保留未版本化数据");
}

TEST(database_constructor_migration_failure_closes_database) {
    std::string path = TempDbPath("constructor_migration_failure");
    std::filesystem::remove(path);
    sqlite3* raw = nullptr;
    CHECK_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    const char* broken_v4 =
        "CREATE TABLE schema_info(version INTEGER NOT NULL);"
        "INSERT INTO schema_info VALUES(4);"
        "CREATE TABLE patents(id INTEGER PRIMARY KEY, next_dossier_check_at INTEGER DEFAULT 0);";
    CHECK_EQ(sqlite3_exec(raw, broken_v4, nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(raw);

    {
        Database db(path);
        CHECK(!db.IsOpen());
        CHECK(db.GetHandle() == nullptr);
        CHECK_EQ(db.SchemaVersion(), 4);
        CHECK(!db.LastError().empty());
    }

    sqlite3* unchanged = nullptr;
    CHECK_EQ(sqlite3_open_v2(path.c_str(), &unchanged, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    CHECK_EQ(patx::ReadSchemaVersion(unchanged), 4);
    CHECK(!SqliteHasColumn(unchanged, "patents", "publication_number"));
    sqlite3_close(unchanged);
    const std::string backup_prefix = std::filesystem::path(path).filename().string() +
                                      ".pre_migration_";
    for (const auto& entry : std::filesystem::directory_iterator(
             std::filesystem::path(path).parent_path())) {
        if (entry.path().filename().string().find(backup_prefix) == 0) {
            std::filesystem::remove(entry.path());
        }
    }
    std::filesystem::remove(path);
}

TEST(database_v4_file_migration_creates_one_openable_pre_migration_backup) {
    ScopedMigrationDatabase fixture(TempDbPath("v4_backup"));
    const std::string& path = fixture.path;
    std::filesystem::remove(path);
    ScopedSqliteHandle database;
    CHECK_EQ(sqlite3_open(path.c_str(), &database.value), SQLITE_OK);
    sqlite3* raw = database.value;
    const char* schema =
        "CREATE TABLE schema_info(version INTEGER NOT NULL);"
        "INSERT INTO schema_info VALUES(4);"
        "CREATE TABLE patents(id INTEGER PRIMARY KEY, next_dossier_check_at INTEGER DEFAULT 0);"
        "CREATE TABLE prosecution_documents("
        "id INTEGER PRIMARY KEY, patent_id INTEGER, source TEXT, application_number TEXT,"
        "fingerprint TEXT, document_title TEXT, local_path TEXT, downloaded_at INTEGER DEFAULT 0,"
        "raw_metadata TEXT, UNIQUE(source,application_number,fingerprint));"
        "CREATE TABLE dossier_sync_state("
        "patent_id INTEGER NOT NULL, provider TEXT NOT NULL,"
        "last_checked_at INTEGER DEFAULT 0, last_success_at INTEGER DEFAULT 0,"
        "last_error_at INTEGER DEFAULT 0, last_error_code TEXT, last_error_message TEXT,"
        "latest_remote_oa_date TEXT, latest_remote_oa_type TEXT,"
        "auth_state TEXT DEFAULT 'NOT_INITIALIZED', PRIMARY KEY(patent_id,provider));"
        "INSERT INTO prosecution_documents VALUES(1,9,'cnipa','202410000001.1','fp-backup',"
        "'迁移前数据','',0,'old');";
    CHECK_EQ(sqlite3_exec(raw, schema, nullptr, nullptr, nullptr), SQLITE_OK);

    auto migration = patx::RunSchemaMigrations(raw, path);
    CHECK(migration.ok);
    CHECK(migration.performed_backup);
    CHECK(!migration.backup_path.empty());
    CHECK(std::filesystem::exists(migration.backup_path));
    CHECK_EQ(patx::ReadSchemaVersion(raw), 6);
    CHECK(SqliteHasIndex(raw, "idx_patents_next_dossier_check"));
    CHECK(SqliteHasColumn(raw, "dossier_sync_state", "latest_applicant_activity"));
    CHECK(SqliteHasColumn(raw, "dossier_sync_state", "terminal_state"));
    CHECK(SqliteHasColumn(raw, "dossier_sync_state", "reexamination_state"));
    int backup_count = 0;
    const std::string backup_prefix = std::filesystem::path(path).filename().string() +
                                      ".pre_migration_";
    for (const auto& entry : std::filesystem::directory_iterator(
             std::filesystem::path(path).parent_path())) {
        if (entry.path().filename().string().find(backup_prefix) == 0) ++backup_count;
    }
    CHECK_EQ(backup_count, 1);

    sqlite3* backup = nullptr;
    CHECK_EQ(sqlite3_open_v2(migration.backup_path.c_str(), &backup, SQLITE_OPEN_READONLY, nullptr),
             SQLITE_OK);
    CHECK_EQ(patx::ReadSchemaVersion(backup), 4);
    CHECK(!SqliteHasColumn(backup, "prosecution_documents", "event_key"));
    sqlite3_stmt* row = nullptr;
    CHECK_EQ(sqlite3_prepare_v2(backup,
        "SELECT document_title,raw_metadata FROM prosecution_documents WHERE id=1",
        -1, &row, nullptr), SQLITE_OK);
    CHECK_EQ(sqlite3_step(row), SQLITE_ROW);
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(row, 0)), "迁移前数据");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(row, 1)), "old");
    sqlite3_finalize(row);
    sqlite3_close(backup);
    sqlite3_close(raw);
    database.value = nullptr;
    std::filesystem::remove(migration.backup_path);
    std::filesystem::remove(path);
}

TEST(database_backup_failure_aborts_v4_migration_without_changes) {
    std::string path = TempDbPath("v4_backup_failure");
    std::filesystem::remove(path);
    sqlite3* raw = nullptr;
    CHECK_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    const char* schema =
        "CREATE TABLE schema_info(version INTEGER NOT NULL);"
        "INSERT INTO schema_info VALUES(4);"
        "CREATE TABLE prosecution_documents("
        "id INTEGER PRIMARY KEY, patent_id INTEGER, source TEXT, application_number TEXT,"
        "fingerprint TEXT, raw_metadata TEXT, UNIQUE(source,application_number,fingerprint));"
        "INSERT INTO prosecution_documents VALUES(1,9,'cnipa','202410000001.1','fp','keep');";
    CHECK_EQ(sqlite3_exec(raw, schema, nullptr, nullptr, nullptr), SQLITE_OK);

    std::string impossible = path + "/missing-parent/patents.db";
    auto migration = patx::RunSchemaMigrations(raw, impossible);
    CHECK(!migration.ok);
    CHECK(!migration.performed_backup);
    CHECK_EQ(patx::ReadSchemaVersion(raw), 4);
    CHECK(!SqliteHasColumn(raw, "prosecution_documents", "event_key"));
    sqlite3_stmt* row = nullptr;
    CHECK_EQ(sqlite3_prepare_v2(raw,
        "SELECT raw_metadata FROM prosecution_documents WHERE id=1", -1, &row, nullptr),
        SQLITE_OK);
    CHECK_EQ(sqlite3_step(row), SQLITE_ROW);
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(row, 0)), "keep");
    sqlite3_finalize(row);
    sqlite3_close(raw);
    std::filesystem::remove(path);
}

TEST(database_v5_event_key_index_is_partial_and_scoped_by_patent) {
    std::string path = TempDbPath("event_index");
    std::filesystem::remove(path);
    {
        Database db(path);
        auto columns = SqliteIndexColumns(db.GetHandle(), "idx_prosecution_docs_event_key");
        CHECK_EQ(columns.size(), 2u);
        CHECK_STR_EQ(columns[0], "patent_id");
        CHECK_STR_EQ(columns[1], "event_key");
        std::string index_sql = SqliteIndexSql(db.GetHandle(), "idx_prosecution_docs_event_key");
        CHECK(index_sql.find("WHERE event_key <> ''") != std::string::npos);

        const char* first =
            "INSERT INTO prosecution_documents "
            "(patent_id,event_key,source,application_number,fingerprint) "
            "VALUES (41,'event-1','source-a','app-a','fp-a')";
        CHECK_EQ(sqlite3_exec(db.GetHandle(), first, nullptr, nullptr, nullptr), SQLITE_OK);

        char* error = nullptr;
        const char* duplicate =
            "INSERT INTO prosecution_documents "
            "(patent_id,event_key,source,application_number,fingerprint) "
            "VALUES (41,'event-1','source-b','app-b','fp-b')";
        CHECK_EQ(sqlite3_exec(db.GetHandle(), duplicate, nullptr, nullptr, &error),
                 SQLITE_CONSTRAINT);
        sqlite3_free(error);

        const char* other_patent =
            "INSERT INTO prosecution_documents "
            "(patent_id,event_key,source,application_number,fingerprint) "
            "VALUES (42,'event-1','source-c','app-c','fp-c')";
        CHECK_EQ(sqlite3_exec(db.GetHandle(), other_patent, nullptr, nullptr, nullptr), SQLITE_OK);

        const char* empty_keys =
            "INSERT INTO prosecution_documents "
            "(patent_id,event_key,source,application_number,fingerprint) "
            "VALUES (41,'','source-d','app-d','fp-d');"
            "INSERT INTO prosecution_documents "
            "(patent_id,event_key,source,application_number,fingerprint) "
            "VALUES (41,'','source-e','app-e','fp-e');";
        CHECK_EQ(sqlite3_exec(db.GetHandle(), empty_keys, nullptr, nullptr, nullptr), SQLITE_OK);
    }
    std::filesystem::remove(path);
}

TEST(database_patent_full_field_roundtrip) {
    std::string path = TempDbPath("patent");
    std::filesystem::remove(path);
    {
        Database db(path);
        Patent in = MakePatent();
        int id = db.InsertPatent(in);
        CHECK(id > 0);

        Patent out = db.GetPatentById(id);
        CHECK_STR_EQ(out.geke_code, "GK-TEST-001");
        CHECK_STR_EQ(out.technology_route, "AI芯片");
        CHECK_STR_EQ(out.rd_project, "智算平台");
        CHECK_STR_EQ(out.tags, "核心;AI");
        CHECK_STR_EQ(out.agent_name, "赵代理");
        CHECK_STR_EQ(out.agent_code, "AG001");
        CHECK_STR_EQ(out.disclosure_writer, "钱撰写");
        CHECK_STR_EQ(out.fee_status, "已缴费");
        CHECK_STR_EQ(out.project_id, "PRJ-9");
        CHECK_STR_EQ(out.oa_reminder_1, "2025-06-01");
        CHECK_STR_EQ(out.pct_reminder, "2027-06-01");
        CHECK_STR_EQ(out.notes, "手工备注");
        CHECK(out.updated_at > 0);

        // Update must persist structured fields too
        out.technology_route = "存算一体";
        out.rd_project = "新项目";
        CHECK(db.UpdatePatent(id, out));
        Patent after = db.GetPatentById(id);
        CHECK_STR_EQ(after.technology_route, "存算一体");
        CHECK_STR_EQ(after.rd_project, "新项目");
        CHECK(after.updated_at >= out.updated_at);
    }
    std::filesystem::remove(path);
}

TEST(database_dossier_sync_state_v6_fields_roundtrip) {
    Database db(":memory:");
    CHECK(db.IsOpen());

    DossierSyncState state;
    state.patent_id = 1;
    state.provider = "cn'ipa";
    state.last_checked_at = 11;
    state.last_success_at = 12;
    state.last_error_at = 13;
    state.last_error_code = "AUTH'REQUIRED";
    state.last_error_message = "first ' error";
    state.latest_remote_oa_date = "2026-02-17";
    state.latest_remote_oa_type = "applicant's response";
    state.auth_state = "AUTH'ENTICATED";
    state.latest_applicant_activity = "2026-02-'18";
    state.terminal_state = "REEXAMINATION_'PENDING";
    state.reexamination_state = "REQUEST_'PERIOD";
    CHECK(db.UpsertDossierSyncState(state));

    auto check_state = [&db](const DossierSyncState& expected) {
        const auto states = db.GetDossierSyncStates();
        CHECK_EQ(states.size(), 1u);
        const auto& actual = states[0];
        CHECK_EQ(actual.patent_id, expected.patent_id);
        CHECK_STR_EQ(actual.provider, expected.provider);
        CHECK_EQ(actual.last_checked_at, expected.last_checked_at);
        CHECK_EQ(actual.last_success_at, expected.last_success_at);
        CHECK_EQ(actual.last_error_at, expected.last_error_at);
        CHECK_STR_EQ(actual.last_error_code, expected.last_error_code);
        CHECK_STR_EQ(actual.last_error_message, expected.last_error_message);
        CHECK_STR_EQ(actual.latest_remote_oa_date, expected.latest_remote_oa_date);
        CHECK_STR_EQ(actual.latest_remote_oa_type, expected.latest_remote_oa_type);
        CHECK_STR_EQ(actual.auth_state, expected.auth_state);
        CHECK_STR_EQ(actual.latest_applicant_activity, expected.latest_applicant_activity);
        CHECK_STR_EQ(actual.terminal_state, expected.terminal_state);
        CHECK_STR_EQ(actual.reexamination_state, expected.reexamination_state);
    };
    check_state(state);

    state.last_checked_at = 21;
    state.last_success_at = 22;
    state.last_error_at = 23;
    state.last_error_code = "UPDATED'CODE";
    state.last_error_message = "second ' error";
    state.latest_remote_oa_date = "2026-03-17";
    state.latest_remote_oa_type = "updated applicant's response";
    state.auth_state = "AUTH'RENEWED";
    state.latest_applicant_activity = "2026-03-'18";
    state.terminal_state = "REEXAMINATION_'ACTIVE";
    state.reexamination_state = "HEARING_'PERIOD";
    CHECK(db.UpsertDossierSyncState(state));
    check_state(state);
}

TEST(database_failed_peer_destruction_preserves_primary_undo_manager) {
    Database primary(":memory:");
    CHECK(primary.IsOpen());
    Patent patent = MakePatent();
    patent.geke_code = "UNDO-PRIMARY";
    int id = primary.InsertPatent(patent);
    CHECK(id > 0);
    CHECK(primary.CanUndo());

    const std::filesystem::path missing_parent =
        std::filesystem::temp_directory_path() /
        ("patx_missing_peer_" + std::to_string(
            static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
    std::filesystem::remove_all(missing_parent);
    {
        Database failed_peer((missing_parent / "patents.db").string());
        CHECK(!failed_peer.IsOpen());
        CHECK(!failed_peer.LastError().empty());
    }

    CHECK(primary.CanUndo());
    CHECK(primary.GetPatentById(id).id == id);
}

TEST(database_successful_peers_keep_instance_owned_undo_in_both_destruction_orders) {
    {
        Database primary(":memory:");
        Patent primary_patent = MakePatent();
        primary_patent.geke_code = "UNDO-NESTED-PRIMARY";
        int primary_id = primary.InsertPatent(primary_patent);
        CHECK(primary_id > 0 && primary.CanUndo());
        {
            Database secondary(":memory:");
            Patent secondary_patent = MakePatent();
            secondary_patent.geke_code = "UNDO-NESTED-SECONDARY";
            int secondary_id = secondary.InsertPatent(secondary_patent);
            CHECK(secondary_id > 0 && secondary.CanUndo());
        }
        CHECK(primary.CanUndo());
        CHECK(primary.GetPatentById(primary_id).id == primary_id);
    }

    auto first = std::make_unique<Database>(":memory:");
    auto second = std::make_unique<Database>(":memory:");
    Patent first_patent = MakePatent();
    first_patent.geke_code = "UNDO-FIRST-DESTROYED";
    Patent second_patent = MakePatent();
    second_patent.geke_code = "UNDO-SECOND-SURVIVES";
    int first_id = first->InsertPatent(first_patent);
    int second_id = second->InsertPatent(second_patent);
    CHECK(first_id > 0 && second_id > 0);
    CHECK(first->CanUndo());
    CHECK(second->CanUndo());

    first.reset();
    CHECK(second->CanUndo());
    CHECK(second->GetPatentById(second_id).id == second_id);
}

TEST(database_legacy_migration_preserves_data) {
    // Build a legacy (v1-shaped) database by hand: patents with notes that
    // carry the old "<prefix>: value" segments, no schema_info.
    std::string path = TempDbPath("legacy");
    std::filesystem::remove(path);
    {
        sqlite3* raw = nullptr;
        if (sqlite3_open(path.c_str(), &raw) != SQLITE_OK) {
            sqlite3_close(raw);
            Fail("cannot create legacy test db");
        }
        const char* legacy_schema =
            "CREATE TABLE patents ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " geke_code TEXT UNIQUE,"
            " application_number TEXT,"
            " title TEXT,"
            " proposal_name TEXT,"
            " application_status TEXT,"
            " patent_type TEXT,"
            " patent_level TEXT,"
            " application_date TEXT,"
            " authorization_date TEXT,"
            " expiration_date TEXT,"
            " geke_handler TEXT,"
            " rd_department TEXT,"
            " agency_firm TEXT,"
            " original_applicant TEXT,"
            " current_applicant TEXT,"
            " inventor TEXT,"
            " notes TEXT,"
            " class_level1 TEXT,"
            " class_level2 TEXT,"
            " class_level3 TEXT,"
            " updated_at INTEGER DEFAULT 0);"
            "INSERT INTO patents (geke_code, title, notes) VALUES ("
            " 'GK-OLD-1', '旧数据', "
            " '技术路线: 图像传感器; 代理人: 刘代理; 1st OA: 2025-03-01; 纯文本备注');";
        char* err = nullptr;
        if (sqlite3_exec(raw, legacy_schema, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "legacy setup failed";
            sqlite3_free(err);
            sqlite3_close(raw);
            Fail(msg);
        }
        sqlite3_close(raw);
    }

    {
        Database db(path);   // constructor runs the migration
        CHECK(db.IsOpen());
        CHECK_EQ(db.SchemaVersion(), patx::kSchemaVersionCurrent);

        Patent p = db.GetPatentByCode("GK-OLD-1");
        CHECK_STR_EQ(p.title, "旧数据");
        // Prefixes migrated into structured columns
        CHECK_STR_EQ(p.technology_route, "图像传感器");
        CHECK_STR_EQ(p.agent_name, "刘代理");
        CHECK_STR_EQ(p.oa_reminder_1, "2025-03-01");
        // Non-prefix text stays in notes
        CHECK(p.notes.find("纯文本备注") != std::string::npos);
        CHECK(p.notes.find("技术路线") == std::string::npos);

        // A pre-migration backup must exist next to the db
        bool backup_found = false;
        for (auto& entry : std::filesystem::directory_iterator(
                 std::filesystem::path(path).parent_path())) {
            std::string name = entry.path().filename().string();
            if (name.find("pre_migration") != std::string::npos &&
                name.find("legacy") != std::string::npos) {
                backup_found = true;
                std::filesystem::remove(entry.path());
            }
        }
        CHECK(backup_found);
    }
    std::filesystem::remove(path);
}

TEST(database_v1_migration_fills_historical_columns_before_creating_indexes) {
    ScopedMigrationDatabase fixture(TempDbPath("v1_missing_historical_columns"));
    const std::string& path = fixture.path;
    std::filesystem::remove(path);
    ScopedSqliteHandle database;
    CHECK_EQ(sqlite3_open(path.c_str(), &database.value), SQLITE_OK);
    sqlite3* raw = database.value;
    const char* legacy_schema =
        "CREATE TABLE patents("
        "id INTEGER PRIMARY KEY, geke_code TEXT UNIQUE, application_number TEXT, title TEXT,"
        "application_status TEXT, patent_type TEXT, application_date TEXT,"
        "authorization_date TEXT, expiration_date TEXT, inventor TEXT, notes TEXT,"
        "updated_at INTEGER DEFAULT 0);"
        "INSERT INTO patents(id,geke_code,application_number,title,application_status,notes)"
        "VALUES(1,'GK-V1-MIN','202410123456.7','最小旧库','pending','保留数据');"
        "CREATE TABLE oa_records(id INTEGER PRIMARY KEY, geke_code TEXT, official_deadline TEXT);"
        "CREATE TABLE pct_patents(id INTEGER PRIMARY KEY, geke_code TEXT, application_no TEXT);"
        "CREATE TABLE software_copyrights(id INTEGER PRIMARY KEY, case_no TEXT);"
        "CREATE TABLE ic_layouts(id INTEGER PRIMARY KEY, case_no TEXT);"
        "CREATE TABLE foreign_patents(id INTEGER PRIMARY KEY, case_no TEXT);"
        "CREATE TABLE annual_fees(id INTEGER PRIMARY KEY, patent_id INTEGER);";
    CHECK_EQ(sqlite3_exec(raw, legacy_schema, nullptr, nullptr, nullptr), SQLITE_OK);

    auto migration = patx::RunSchemaMigrations(raw, path);
    CHECK(migration.ok);
    CHECK(migration.performed_backup);
    CHECK_EQ(migration.from_version, 0);
    CHECK_EQ(migration.to_version, 6);
    CHECK_EQ(patx::ReadSchemaVersion(raw), 6);
    CHECK(SqliteHasColumn(raw, "dossier_sync_state", "latest_applicant_activity"));
    CHECK(SqliteHasColumn(raw, "dossier_sync_state", "terminal_state"));
    CHECK(SqliteHasColumn(raw, "dossier_sync_state", "reexamination_state"));

    for (const char* column : {"patent_level", "geke_handler", "class_level1", "class_level2",
                               "class_level3", "rd_department", "agency_firm",
                               "original_applicant", "current_applicant", "proposal_name"}) {
        CHECK(SqliteHasColumn(raw, "patents", column));
    }
    for (const char* column : {"writer", "progress", "agency", "oa_summary", "is_extendable",
                               "extension_requested", "extension_months", "extended_deadline"}) {
        CHECK(SqliteHasColumn(raw, "oa_records", column));
    }
    for (const char* column : {"domestic_source", "country_app_no", "filing_date",
                               "priority_date", "country"}) {
        CHECK(SqliteHasColumn(raw, "pct_patents", column));
    }
    for (const char* column : {"original_owner", "current_owner", "developer",
                               "dev_complete_date", "version"}) {
        CHECK(SqliteHasColumn(raw, "software_copyrights", column));
    }
    for (const char* column : {"original_owner", "current_owner", "designer",
                               "creation_date", "cert_date"}) {
        CHECK(SqliteHasColumn(raw, "ic_layouts", column));
    }
    for (const char* column : {"pct_no", "country_app_no", "owner", "patent_status",
                               "country", "application_no"}) {
        CHECK(SqliteHasColumn(raw, "foreign_patents", column));
    }
    sqlite3_stmt* preserved = nullptr;
    CHECK_EQ(sqlite3_prepare_v2(raw,
        "SELECT title,notes FROM patents WHERE id=1", -1, &preserved, nullptr), SQLITE_OK);
    CHECK_EQ(sqlite3_step(preserved), SQLITE_ROW);
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved, 0)), "最小旧库");
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(preserved, 1)), "保留数据");
    sqlite3_finalize(preserved);
    sqlite3_close(raw);
    database.value = nullptr;

    sqlite3* backup = nullptr;
    CHECK_EQ(sqlite3_open_v2(migration.backup_path.c_str(), &backup,
                             SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    CHECK_EQ(patx::ReadSchemaVersion(backup), 0);
    CHECK(!SqliteHasColumn(backup, "patents", "geke_handler"));
    sqlite3_close(backup);
    std::filesystem::remove(migration.backup_path);
    std::filesystem::remove(path);
}

TEST(database_query_filters) {
    std::string path = TempDbPath("filter");
    std::filesystem::remove(path);
    {
        Database db(path);
        Patent a = MakePatent();
        a.geke_code = "GK-F-1";
        a.title = "图像处理装置";
        a.geke_handler = "张三";
        a.application_status = "pending";
        db.InsertPatent(a);

        Patent b = MakePatent();
        b.geke_code = "GK-F-2";
        b.title = "通信方法";
        b.geke_handler = "李四";
        b.application_status = "granted";
        b.tags = "通信";
        db.InsertPatent(b);

        QueryFilter f;
        CHECK_EQ(db.GetPatents(f).size(), 2u);

        f.handler = "张三";
        CHECK_EQ(db.GetPatents(f).size(), 1u);

        f = QueryFilter{};
        f.keyword = "通信";
        auto hits = db.GetPatents(f);
        CHECK_EQ(hits.size(), 1u);
        CHECK_STR_EQ(hits[0].geke_code, "GK-F-2");

        // LIKE wildcards in the keyword must not widen the query
        f = QueryFilter{};
        f.keyword = "%";
        CHECK_EQ(db.GetPatents(f).size(), 0u);
    }
    std::filesystem::remove(path);
}

TEST(database_pct_software_ic_foreign_full_crud) {
    std::string path = TempDbPath("crud");
    std::filesystem::remove(path);
    {
        Database db(path);

        // PCT: insert + full update roundtrip
        PCTPatent pct;
        pct.geke_code = "PCT-1";
        pct.domestic_source = "GK-1";
        pct.application_no = "PCT/CN2025/123456";
        pct.country_app_no = "US17/999888";
        pct.title = "PCT标题";
        pct.application_status = "national phase";
        pct.handler = "张三";
        pct.inventor = "李四";
        pct.filing_date = "2025-02-01";
        pct.application_date = "2025-02-02";
        pct.priority_date = "2024-02-02";
        pct.country = "US";
        pct.notes = "note";
        int pct_id = db.InsertPCT(pct);
        CHECK(pct_id > 0);
        pct.title = "改后PCT";
        pct.inventor = "王五";
        pct.country_app_no = "EP2500011.1";
        CHECK(db.UpdatePCT(pct_id, pct));
        auto pct_out = db.GetPCTById(pct_id);
        CHECK_STR_EQ(pct_out.title, "改后PCT");
        CHECK_STR_EQ(pct_out.inventor, "王五");
        CHECK_STR_EQ(pct_out.country_app_no, "EP2500011.1");
        CHECK_STR_EQ(pct_out.priority_date, "2024-02-02");

        // Software: update (previously missing entirely)
        SoftwareCopyright sw;
        sw.case_no = "SW-1";
        sw.title = "软件A";
        sw.developer = "开发部";
        sw.version = "V2.0";
        sw.notes = "sw-note";
        int sw_id = db.InsertSoftware(sw);
        CHECK(sw_id > 0);
        sw.title = "软件A改";
        sw.version = "V3.0";
        sw.notes = "sw-note-2";
        CHECK(db.UpdateSoftware(sw_id, sw));
        auto sw_out = db.GetSoftwareById(sw_id);
        CHECK_STR_EQ(sw_out.title, "软件A改");
        CHECK_STR_EQ(sw_out.version, "V3.0");
        CHECK_STR_EQ(sw_out.notes, "sw-note-2");

        // IC: update roundtrip
        ICLayout ic;
        ic.case_no = "IC-1";
        ic.title = "布图A";
        ic.designer = "设计部";
        ic.creation_date = "2025-01-01";
        int ic_id = db.InsertIC(ic);
        CHECK(ic_id > 0);
        ic.title = "布图A改";
        ic.cert_date = "2025-08-01";
        CHECK(db.UpdateIC(ic_id, ic));
        auto ic_out = db.GetICById(ic_id);
        CHECK_STR_EQ(ic_out.title, "布图A改");
        CHECK_STR_EQ(ic_out.cert_date, "2025-08-01");

        // Foreign: full insert + update (previously partial insert only)
        ForeignPatent fp;
        fp.case_no = "FP-1";
        fp.country = "US";
        fp.application_no = "17248024";
        fp.title = "US case";
        fp.inventor = "John Doe";
        fp.application_date = "2021-01-05";
        fp.authorization_date = "2023-05-09";
        fp.notes = "fp-note";
        int fp_id = db.InsertForeign(fp);
        CHECK(fp_id > 0);
        fp.title = "US case amended";
        fp.inventor = "Jane Roe";
        fp.notes = "fp-note-2";
        CHECK(db.UpdateForeign(fp_id, fp));
        auto fp_out = db.GetForeignById(fp_id);
        CHECK_STR_EQ(fp_out.title, "US case amended");
        CHECK_STR_EQ(fp_out.inventor, "Jane Roe");
        CHECK_STR_EQ(fp_out.application_no, "17248024");
        CHECK_STR_EQ(fp_out.notes, "fp-note-2");

    }
    std::filesystem::remove(path);
}

TEST(database_oa_crud_and_external_link) {
    std::string path = TempDbPath("oa");
    std::filesystem::remove(path);
    {
        Database db(path);
        OARecord oa;
        oa.geke_code = "GK-1";
        oa.oa_type = "1-OA";
        oa.official_deadline = "2025-12-01";
        oa.issue_date = "2025-08-01";
        oa.handler = "张三";
        oa.writer = "李四";
        oa.progress = "drafting";
        oa.oa_summary = "创造性";
        oa.jurisdiction = "US";       // legacy value from the removed USPTO sync
        oa.source = "cnipa";
        oa.remote_document_id = "row-1";
        oa.sync_flag = "web_new";
        oa.deadline_source = "calculated";
        int id = db.InsertOA(oa);
        CHECK(id > 0);

        auto out = db.GetOAById(id);
        CHECK_STR_EQ(out.oa_summary, "创造性");
        CHECK_STR_EQ(out.jurisdiction, "US");
        CHECK_STR_EQ(out.remote_document_id, "row-1");
        CHECK_STR_EQ(out.sync_flag, "web_new");
        CHECK_STR_EQ(out.deadline_source, "calculated");

        // Filter by deadline state
        QueryFilter f;
        f.deadline_state = "incomplete";
        CHECK_EQ(db.GetOARecords(f).size(), 1u);
        db.MarkOACompleted(id);
        f.deadline_state = "completed";
        CHECK_EQ(db.GetOARecords(f).size(), 1u);
    }
    std::filesystem::remove(path);
}

TEST(database_oa_exact_merge_serializes_alias_inserts) {
    const std::string path = TempDbPath("oa_exact_merge_insert_race");
    std::filesystem::remove(path);
    {
        Database first(path);
        Database second(path);

        OARecord canonical;
        canonical.geke_code = "GK-OA-ATOMIC-INSERT";
        canonical.oa_type = "第一次审查意见通知书";
        canonical.issue_date = "2026-09-10";
        canonical.handler = "李四";

        OARecord alias = canonical;
        alias.oa_type = "一通";
        alias.handler = "王五";

        std::promise<void> start_promise;
        std::shared_future<void> start = start_promise.get_future().share();
        OAExactMergeResult first_result;
        OAExactMergeResult second_result;
        std::thread first_thread([&] {
            start.wait();
            first_result = first.MergeOAExact(canonical, true);
        });
        std::thread second_thread([&] {
            start.wait();
            second_result = second.MergeOAExact(alias, true);
        });
        start_promise.set_value();
        first_thread.join();
        second_thread.join();

        const auto records = first.GetOAByPatent(canonical.geke_code);
        CHECK_EQ(records.size(), 1u);
        CHECK((first_result.status == OAExactMergeStatus::Inserted &&
               second_result.status == OAExactMergeStatus::HandlerConflict) ||
              (second_result.status == OAExactMergeStatus::Inserted &&
               first_result.status == OAExactMergeStatus::HandlerConflict));
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

TEST(database_oa_exact_merge_fills_handler_with_compare_and_set) {
    const std::string path = TempDbPath("oa_exact_merge_handler_race");
    std::filesystem::remove(path);
    {
        Database seed(path);
        OARecord existing;
        existing.geke_code = "GK-OA-ATOMIC-HANDLER";
        existing.oa_type = "一通";
        existing.issue_date = "2026-09-11";
        existing.writer = "人工撰写人";
        CHECK(seed.InsertOA(existing) > 0);

        Database first(path);
        Database second(path);
        OARecord first_incoming = existing;
        first_incoming.oa_type = "第一次审查意见通知书";
        first_incoming.handler = "李四";
        OARecord second_incoming = first_incoming;
        second_incoming.handler = "王五";

        std::promise<void> start_promise;
        std::shared_future<void> start = start_promise.get_future().share();
        OAExactMergeResult first_result;
        OAExactMergeResult second_result;
        std::thread first_thread([&] {
            start.wait();
            first_result = first.MergeOAExact(first_incoming, true);
        });
        std::thread second_thread([&] {
            start.wait();
            second_result = second.MergeOAExact(second_incoming, true);
        });
        start_promise.set_value();
        first_thread.join();
        second_thread.join();

        const auto records = seed.GetOAByPatent(existing.geke_code);
        CHECK_EQ(records.size(), 1u);
        CHECK_STR_EQ(records.front().writer, "人工撰写人");
        CHECK(records.front().handler == "李四" || records.front().handler == "王五");
        CHECK((first_result.status == OAExactMergeStatus::HandlerUpdated &&
               second_result.status == OAExactMergeStatus::HandlerConflict) ||
              (second_result.status == OAExactMergeStatus::HandlerUpdated &&
               first_result.status == OAExactMergeStatus::HandlerConflict));
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

TEST(database_connection_write_lock_serializes_same_instance_transactions) {
    const std::string path = TempDbPath("connection_write_lock");
    std::filesystem::remove(path);
    {
        Database db(path);
        OARecord oa;
        oa.geke_code = "GK-OA-SAME-CONNECTION";
        oa.oa_type = "一通";
        oa.issue_date = "2026-09-13";

        ProsecutionDocumentRecord document;
        document.patent_id = 1;
        document.source = "cnipa";
        document.application_number = "202610000001.1";
        document.fingerprint = "same-connection-fingerprint";
        document.event_key = "same-connection-event";

        auto connection_lock = db.AcquireConnectionWriteLock();
        std::promise<void> worker_started;
        auto worker = std::async(std::launch::async, [&] {
            worker_started.set_value();
            bool created = false;
            return db.UpsertProsecutionDocument(document, &created);
        });
        worker_started.get_future().wait();
        CHECK(worker.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);

        const OAExactMergeResult merged = db.MergeOAExact(oa, true);
        CHECK(merged.status == OAExactMergeStatus::Inserted);
        connection_lock.unlock();

        CHECK(worker.get() > 0);
        const auto records = db.GetOAByPatent(oa.geke_code);
        CHECK_EQ(records.size(), 1u);
        CHECK(db.GetProsecutionDocumentById(document.id).id > 0);
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

TEST(database_deadline_rules) {
    std::string path = TempDbPath("rules");
    std::filesystem::remove(path);
    {
        Database db(path);
        auto rules = db.GetDeadlineRules();
        CHECK(rules.size() >= 5);   // seeded defaults

        // CN invention OA: 4 months from issue date
        std::string d1 = db.CalculateDeadline("CN", "oa_response_invention", "2025-01-31");
        CHECK_STR_EQ(d1, "2025-05-31");

        // US OA: 3 months
        std::string d2 = db.CalculateDeadline("US", "oa_response", "2025-03-10");
        CHECK_STR_EQ(d2, "2025-06-10");

        // Unknown jurisdiction: no suggestion
        CHECK_STR_EQ(db.CalculateDeadline("XX", "whatever", "2025-01-01"), "");

        // User-defined rule CRUD
        DeadlineRule custom;
        custom.jurisdiction = "EP";
        custom.event_type = "oa_response";
        custom.base_months = 4;
        int rule_id = 0;
        CHECK(db.InsertDeadlineRule(custom, &rule_id));
        CHECK(rule_id > 0);
        std::string d3 = db.CalculateDeadline("EP", "oa_response", "2025-01-01");
        CHECK_STR_EQ(d3, "2025-05-01");

        custom.id = rule_id;
        custom.base_months = 6;
        CHECK(db.UpdateDeadlineRule(custom));
        std::string d4 = db.CalculateDeadline("EP", "oa_response", "2025-01-01");
        CHECK_STR_EQ(d4, "2025-07-01");

        CHECK(db.DeleteDeadlineRule(rule_id));
        CHECK_STR_EQ(db.CalculateDeadline("EP", "oa_response", "2025-01-01"), "");
    }
    std::filesystem::remove(path);
}

TEST(database_backup_api) {
    std::string path = TempDbPath("backup");
    std::string dest = TempDbPath("backup_dest");
    std::filesystem::remove(path);
    std::filesystem::remove(dest);
    {
        Database db(path);
        Patent p = MakePatent();
        db.InsertPatent(p);
        CHECK(db.BackupTo(dest));
    }
    {
        Database restored(dest);
        Patent p = restored.GetPatentByCode("GK-TEST-001");
        CHECK_STR_EQ(p.title, "测试专利");
    }
    std::filesystem::remove(path);
    std::filesystem::remove(dest);
}

TEST(database_consistent_snapshot_includes_committed_wal_pages) {
    const std::string source = TempDbPath("live_wal_source");
    const std::string snapshot = TempDbPath("live_wal_snapshot");
    std::filesystem::remove(source);
    std::filesystem::remove(snapshot);

    sqlite3* live = nullptr;
    CHECK_EQ(sqlite3_open(source.c_str(), &live), SQLITE_OK);
    CHECK_EQ(sqlite3_exec(live,
        "PRAGMA journal_mode=WAL;"
        "CREATE TABLE live_rows(value TEXT);"
        "PRAGMA wal_checkpoint(TRUNCATE);"
        "INSERT INTO live_rows VALUES('committed-in-wal');",
        nullptr, nullptr, nullptr), SQLITE_OK);

    std::string error;
    CHECK(Database::CopyConsistentSnapshot(source, snapshot, &error));
    CHECK(error.empty());

    sqlite3* copied = nullptr;
    CHECK_EQ(sqlite3_open(snapshot.c_str(), &copied), SQLITE_OK);
    sqlite3_stmt* row = nullptr;
    const int prepare_rc = sqlite3_prepare_v2(copied,
        "SELECT value FROM live_rows", -1, &row, nullptr);
    if (prepare_rc != SQLITE_OK) Fail(sqlite3_errmsg(copied));
    CHECK_EQ(sqlite3_step(row), SQLITE_ROW);
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(row, 0)),
                 "committed-in-wal");
    sqlite3_finalize(row);
    sqlite3_close(copied);
    sqlite3_close(live);

    std::filesystem::remove(snapshot);
    std::filesystem::remove(source);
}

TEST(database_consistent_snapshot_rejects_aliases_without_deleting_source) {
    const std::string source = TempDbPath("snapshot_alias_source");
    const std::string alias = TempDbPath("snapshot_alias_link");
    std::filesystem::remove(source);
    std::filesystem::remove(alias);
    sqlite3* db = nullptr;
    CHECK_EQ(sqlite3_open(source.c_str(), &db), SQLITE_OK);
    CHECK_EQ(sqlite3_exec(db,
        "CREATE TABLE keep_me(value TEXT); INSERT INTO keep_me VALUES('safe');",
        nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(db);

    std::string error;
    CHECK(!Database::CopyConsistentSnapshot(source, source, &error));
    CHECK(std::filesystem::exists(source));

    std::filesystem::create_hard_link(source, alias);
    error.clear();
    CHECK(!Database::CopyConsistentSnapshot(source, alias, &error));
    CHECK(std::filesystem::exists(source));
    CHECK(std::filesystem::exists(alias));

    sqlite3* intact = nullptr;
    CHECK_EQ(sqlite3_open_v2(source.c_str(), &intact, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    sqlite3_stmt* row = nullptr;
    CHECK_EQ(sqlite3_prepare_v2(intact,
        "SELECT value FROM keep_me", -1, &row, nullptr), SQLITE_OK);
    CHECK_EQ(sqlite3_step(row), SQLITE_ROW);
    CHECK_STR_EQ(reinterpret_cast<const char*>(sqlite3_column_text(row, 0)), "safe");
    sqlite3_finalize(row);
    sqlite3_close(intact);
    std::filesystem::remove(alias);
    std::filesystem::remove(source);
}

TEST(database_consistent_snapshot_missing_source_preserves_existing_destination) {
    const std::string missing = TempDbPath("snapshot_missing_source");
    const std::string destination = TempDbPath("snapshot_existing_destination");
    std::filesystem::remove(missing);
    std::filesystem::remove(destination);
    {
        std::ofstream out(destination, std::ios::binary);
        out << "do-not-delete";
    }

    std::string error;
    CHECK(!Database::CopyConsistentSnapshot(missing, destination, &error));
    CHECK(std::filesystem::exists(destination));
    std::ifstream in(destination, std::ios::binary);
    CHECK_STR_EQ(std::string(std::istreambuf_iterator<char>(in),
                             std::istreambuf_iterator<char>()),
                 "do-not-delete");
    std::filesystem::remove(destination);
}
