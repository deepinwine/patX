// Database layer tests: schema migration, full-field CRUD for every module,
// unified query filters, deadline rules.
#include "test_registry.hpp"

#include "database.hpp"
#include "patx/schema_migrations.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>

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

bool SqliteHasColumn(sqlite3* db, const char* table, const char* column) {
    sqlite3_stmt* stmt = nullptr;
    std::string sql = std::string("PRAGMA table_info(") + table + ")";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (name && std::string(name) == column) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

bool SqliteHasIndex(sqlite3* db, const char* index) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT 1 FROM sqlite_master WHERE type='index' AND name=?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, index, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
}

std::vector<std::string> SqliteIndexColumns(sqlite3* db, const char* index) {
    std::vector<std::string> columns;
    sqlite3_stmt* stmt = nullptr;
    std::string sql = std::string("PRAGMA index_info(") + index + ")";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return columns;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        if (name) columns.emplace_back(name);
    }
    sqlite3_finalize(stmt);
    return columns;
}

std::string SqliteIndexSql(sqlite3* db, const char* index) {
    sqlite3_stmt* stmt = nullptr;
    const char* query = "SELECT sql FROM sqlite_master WHERE type='index' AND name=?";
    if (sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) != SQLITE_OK) return "";
    sqlite3_bind_text(stmt, 1, index, -1, SQLITE_TRANSIENT);
    std::string sql;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (text) sql = text;
    }
    sqlite3_finalize(stmt);
    return sql;
}

} // namespace

TEST(database_fresh_schema_is_versioned) {
    std::string path = TempDbPath("fresh");
    std::filesystem::remove(path);
    {
        Database db(path);
        CHECK(db.IsOpen());
        CHECK_EQ(db.SchemaVersion(), patx::kSchemaVersionCurrent);
        CHECK_EQ(db.SchemaVersion(), 5);
        CHECK(SqliteHasColumn(db.GetHandle(), "prosecution_documents", "raw_title"));
        CHECK(SqliteHasColumn(db.GetHandle(), "prosecution_documents", "document_code"));
        CHECK(SqliteHasColumn(db.GetHandle(), "prosecution_documents", "document_version"));
        CHECK(SqliteHasColumn(db.GetHandle(), "prosecution_documents", "event_key"));
        CHECK(SqliteHasColumn(db.GetHandle(), "prosecution_documents", "source_trace"));
        CHECK(SqliteHasIndex(db.GetHandle(), "idx_prosecution_docs_event_key"));
        CHECK(SqliteHasIndex(db.GetHandle(), "idx_patents_next_dossier_check"));

        auto repeated = patx::RunSchemaMigrations(db.GetHandle(), path);
        CHECK(repeated.ok);
        CHECK_EQ(repeated.from_version, 5);
        CHECK_EQ(repeated.to_version, 5);
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

TEST(database_v4_to_v5_migration_is_incremental_and_preserves_data) {
    std::string path = TempDbPath("v4_to_v5");
    std::filesystem::remove(path);
    {
        sqlite3* raw = nullptr;
        CHECK_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
        const char* sql =
            "CREATE TABLE schema_info (version INTEGER NOT NULL);"
            "INSERT INTO schema_info VALUES (4);"
            "CREATE TABLE prosecution_documents ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT, patent_id INTEGER, jurisdiction TEXT,"
            " application_number TEXT, publication_number TEXT, source TEXT,"
            " remote_document_id TEXT, document_type TEXT, document_title TEXT,"
            " official_date TEXT, direction TEXT, source_url TEXT, download_url TEXT,"
            " download_available INTEGER DEFAULT 0, fingerprint TEXT,"
            " first_seen_at INTEGER, last_seen_at INTEGER, local_path TEXT,"
            " downloaded_at INTEGER DEFAULT 0, raw_metadata TEXT,"
            " UNIQUE(source, application_number, fingerprint));"
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
        CHECK_EQ(db.SchemaVersion(), 5);
        CHECK(SqliteHasColumn(db.GetHandle(), "prosecution_documents", "raw_title"));
        CHECK(SqliteHasColumn(db.GetHandle(), "prosecution_documents", "document_code"));
        CHECK(SqliteHasColumn(db.GetHandle(), "prosecution_documents", "document_version"));
        CHECK(SqliteHasColumn(db.GetHandle(), "prosecution_documents", "event_key"));
        CHECK(SqliteHasColumn(db.GetHandle(), "prosecution_documents", "source_trace"));
        CHECK(SqliteHasIndex(db.GetHandle(), "idx_prosecution_docs_event_key"));
        CHECK(SqliteHasIndex(db.GetHandle(), "idx_patents_next_dossier_check"));

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
        CHECK_EQ(repeated.from_version, 5);
        CHECK_EQ(repeated.to_version, 5);
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
    std::string path = TempDbPath("constructor_v4_backup_order");
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
        "CREATE TABLE v4_marker(value TEXT);"
        "INSERT INTO v4_marker VALUES('before-init');";
    CHECK_EQ(sqlite3_exec(raw, schema, nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(raw);

    {
        Database db(path);
        CHECK(db.IsOpen());
        CHECK_EQ(db.SchemaVersion(), 5);
        CHECK(SqliteHasIndex(db.GetHandle(), "idx_patents_next_dossier_check"));
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

TEST(database_reopens_v5_and_repairs_missing_due_index) {
    std::string path = TempDbPath("v5_due_index_repair");
    std::filesystem::remove(path);
    {
        Database db(path);
        CHECK_EQ(db.SchemaVersion(), 5);
        CHECK(db.Execute("DROP INDEX idx_patents_next_dossier_check"));
        CHECK(!SqliteHasIndex(db.GetHandle(), "idx_patents_next_dossier_check"));
    }
    {
        Database reopened(path);
        CHECK_EQ(reopened.SchemaVersion(), 5);
        CHECK(SqliteHasIndex(reopened.GetHandle(), "idx_patents_next_dossier_check"));
    }
    std::filesystem::remove(path);
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
    std::string path = TempDbPath("v4_backup");
    std::filesystem::remove(path);
    sqlite3* raw = nullptr;
    CHECK_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    const char* schema =
        "CREATE TABLE schema_info(version INTEGER NOT NULL);"
        "INSERT INTO schema_info VALUES(4);"
        "CREATE TABLE patents(id INTEGER PRIMARY KEY, next_dossier_check_at INTEGER DEFAULT 0);"
        "CREATE TABLE prosecution_documents("
        "id INTEGER PRIMARY KEY, patent_id INTEGER, source TEXT, application_number TEXT,"
        "fingerprint TEXT, document_title TEXT, local_path TEXT, downloaded_at INTEGER DEFAULT 0,"
        "raw_metadata TEXT, UNIQUE(source,application_number,fingerprint));"
        "INSERT INTO prosecution_documents VALUES(1,9,'cnipa','202410000001.1','fp-backup',"
        "'迁移前数据','',0,'old');";
    CHECK_EQ(sqlite3_exec(raw, schema, nullptr, nullptr, nullptr), SQLITE_OK);

    auto migration = patx::RunSchemaMigrations(raw, path);
    CHECK(migration.ok);
    CHECK(migration.performed_backup);
    CHECK(!migration.backup_path.empty());
    CHECK(std::filesystem::exists(migration.backup_path));
    CHECK_EQ(patx::ReadSchemaVersion(raw), 5);
    CHECK(!SqliteHasIndex(raw, "idx_patents_next_dossier_check"));
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
    std::string path = TempDbPath("v1_missing_historical_columns");
    std::filesystem::remove(path);
    sqlite3* raw = nullptr;
    CHECK_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
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
    CHECK_EQ(migration.to_version, 5);
    CHECK_EQ(patx::ReadSchemaVersion(raw), 5);

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
