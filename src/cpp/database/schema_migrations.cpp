#include "patx/schema_migrations.hpp"
#include "patx/log.hpp"

#include <sqlite3.h>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

namespace patx {

namespace {

bool Exec(sqlite3* db, const std::string& sql, std::string* error = nullptr) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        if (error) *error = err ? err : "unknown SQL error";
        PATX_LOG_ERROR("SQL failed in migration: " + std::string(err ? err : "?") + " | " + sql.substr(0, 200));
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool HasTable(sqlite3* db, const std::string& table) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
                           -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, table.c_str(), -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
}

std::vector<std::string> GetColumns(sqlite3* db, const std::string& table) {
    std::vector<std::string> columns;
    sqlite3_stmt* stmt = nullptr;
    std::string sql = "PRAGMA table_info(" + table + ");";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (name) columns.push_back(name);
        }
        sqlite3_finalize(stmt);
    }
    return columns;
}

bool HasColumn(sqlite3* db, const std::string& table, const std::string& column) {
    for (const auto& c : GetColumns(db, table)) {
        if (c == column) return true;
    }
    return false;
}

std::string Quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "''";
        else out += c;
    }
    out += "'";
    return out;
}

// ---------------------------------------------------------------------------
// v1 -> v2
// ---------------------------------------------------------------------------

// Columns appended to patents in v2, mapped to the notes prefix that the
// Excel importer used to concatenate (see excel_io.cpp field ids 21-42).
const std::vector<std::pair<std::string, std::string>>& PatentV2Columns() {
    static const std::vector<std::pair<std::string, std::string>> cols = {
        {"related_case_info", "关联案信息"},
        {"fee_status", "缴费状态"},
        {"rd_project", "研发项目"},
        {"class_level4", "四级（新）"},
        {"tags", "标签"},
        {"details", "具体内容"},
        {"filing_date", "立案日"},
        {"disclosure_writer", "技术交底书撰写人"},
        {"agent_code", "代理人编码"},
        {"agent_name", "代理人"},
        {"intangible_asset_eval", "无形资产评估"},
        {"internal_rd_project", "内部研发项目"},
        {"technology_route", "技术路线"},
        {"project_id", "项目ID"},
        {"oa_reminder_1", "1st OA"},
        {"oa_reminder_2", "2nd OA"},
        {"oa_reminder_3", "3rd OA"},
        {"oa_reminder_4", "4th OA"},
        {"oa_reminder_5", "5OA"},
        {"reexamination", "复审"},
        {"pudong_subsidy", "浦东资助情况"},
        {"pct_reminder", "PCT提醒"},
    };
    return cols;
}

bool ApplyV1ToV2(sqlite3* db) {
    // 1. Structured patent columns
    for (const auto& [column, prefix] : PatentV2Columns()) {
        if (!HasColumn(db, "patents", column)) {
            if (!Exec(db, "ALTER TABLE patents ADD COLUMN " + column + " TEXT;")) return false;
        }
    }
    if (!HasColumn(db, "patents", "updated_at")) {
        if (!Exec(db, "ALTER TABLE patents ADD COLUMN updated_at INTEGER DEFAULT 0;")) return false;
    }

    // 2. OA external linkage columns (USPTO integration)
    const std::vector<std::pair<std::string, std::string>> oa_cols = {
        {"jurisdiction", "TEXT DEFAULT ''"},
        {"source", "TEXT DEFAULT ''"},
        {"external_case_id", "TEXT DEFAULT ''"},
        {"external_document_id", "TEXT DEFAULT ''"},
        {"deadline_source", "TEXT DEFAULT ''"},
    };
    for (const auto& [column, type] : oa_cols) {
        if (!HasColumn(db, "oa_records", column)) {
            if (!Exec(db, "ALTER TABLE oa_records ADD COLUMN " + column + " " + type + ";")) return false;
        }
    }

    // 3. Annual fee jurisdiction/identifier columns
    const std::vector<std::pair<std::string, std::string>> fee_cols = {
        {"jurisdiction", "TEXT DEFAULT 'CN'"},
        {"application_date", "TEXT DEFAULT ''"},
    };
    for (const auto& [column, type] : fee_cols) {
        if (!HasColumn(db, "annual_fees", column)) {
            if (!Exec(db, "ALTER TABLE annual_fees ADD COLUMN " + column + " " + type + ";")) return false;
        }
    }

    // 4. Deadline rules table
    if (!Exec(db, R"(
        CREATE TABLE IF NOT EXISTS deadline_rules (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            jurisdiction TEXT NOT NULL,
            event_type TEXT NOT NULL,
            rule_description TEXT,
            base_months INTEGER DEFAULT 0,
            base_days INTEGER DEFAULT 0,
            extendable INTEGER DEFAULT 0,
            max_extension_months INTEGER DEFAULT 0,
            effective_from TEXT,
            effective_to TEXT,
            enabled INTEGER DEFAULT 1,
            notes TEXT
        );
    )")) return false;

    // 5. Indexes for the hot query paths
    const std::vector<std::string> indexes = {
        "CREATE INDEX IF NOT EXISTS idx_patents_application_number ON patents(application_number);",
        "CREATE INDEX IF NOT EXISTS idx_patents_handler ON patents(geke_handler);",
        "CREATE INDEX IF NOT EXISTS idx_patents_status ON patents(application_status);",
        "CREATE INDEX IF NOT EXISTS idx_oa_geke_code ON oa_records(geke_code);",
        "CREATE INDEX IF NOT EXISTS idx_oa_deadline ON oa_records(official_deadline);",
        "CREATE INDEX IF NOT EXISTS idx_oa_external_doc ON oa_records(external_document_id);",
        "CREATE INDEX IF NOT EXISTS idx_pct_geke_code ON pct_patents(geke_code);",
        "CREATE INDEX IF NOT EXISTS idx_pct_application_no ON pct_patents(application_no);",
        "CREATE INDEX IF NOT EXISTS idx_foreign_application_no ON foreign_patents(application_no);",
        "CREATE INDEX IF NOT EXISTS idx_foreign_country_app_no ON foreign_patents(country_app_no);",
        "CREATE INDEX IF NOT EXISTS idx_software_case_no ON software_copyrights(case_no);",
        "CREATE INDEX IF NOT EXISTS idx_ic_case_no ON ic_layouts(case_no);",
        "CREATE INDEX IF NOT EXISTS idx_annual_fees_patent ON annual_fees(patent_id);",
    };
    for (const auto& sql : indexes) {
        if (!Exec(db, sql)) return false;
    }

    // 6. Move legacy notes-prefix data into the new columns
    MigrateNotesPrefixesToColumns(db);

    // 7. Seed deadline rules for existing installations
    SeedDeadlineRulesIfEmpty(db);

    return true;
}

bool ApplyV2ToV3(sqlite3* db) {
    const std::vector<std::pair<std::string, std::string>> patent_cols = {
        {"publication_number", "TEXT DEFAULT ''"},
        {"last_dossier_check_at", "INTEGER DEFAULT 0"},
        {"next_dossier_check_at", "INTEGER DEFAULT 0"},
    };
    for (const auto& [column, type] : patent_cols) {
        if (!HasColumn(db, "patents", column) &&
            !Exec(db, "ALTER TABLE patents ADD COLUMN " + column + " " + type + ";")) {
            return false;
        }
    }

    const std::vector<std::pair<std::string, std::string>> oa_cols = {
        {"remote_document_id", "TEXT DEFAULT ''"},
        {"sync_flag", "TEXT DEFAULT ''"},
    };
    for (const auto& [column, type] : oa_cols) {
        if (!HasColumn(db, "oa_records", column) &&
            !Exec(db, "ALTER TABLE oa_records ADD COLUMN " + column + " " + type + ";")) {
            return false;
        }
    }

    if (!Exec(db, R"(
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
            raw_metadata TEXT,
            UNIQUE (source, application_number, fingerprint)
        );
        CREATE INDEX IF NOT EXISTS idx_prosecution_docs_patent
            ON prosecution_documents(patent_id);
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
        );
    )")) return false;

    return true;
}

} // namespace

int ReadSchemaVersion(sqlite3* db) {
    if (!db) return 0;
    if (!HasTable(db, "schema_info")) return 0;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT version FROM schema_info LIMIT 1", -1, &stmt, nullptr) != SQLITE_OK)
        return 0;
    int version = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        version = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return version;
}

void SeedDeadlineRulesIfEmpty(sqlite3* db) {
    // Fresh databases reach this before ApplyV1ToV2 ever ran, so make sure
    // the table exists here (idempotent).
    Exec(db, R"(
        CREATE TABLE IF NOT EXISTS deadline_rules (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            jurisdiction TEXT NOT NULL,
            event_type TEXT NOT NULL,
            rule_description TEXT,
            base_months INTEGER DEFAULT 0,
            base_days INTEGER DEFAULT 0,
            extendable INTEGER DEFAULT 0,
            max_extension_months INTEGER DEFAULT 0,
            effective_from TEXT,
            effective_to TEXT,
            enabled INTEGER DEFAULT 1,
            notes TEXT
        );
    )");
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM deadline_rules", -1, &stmt, nullptr) != SQLITE_OK)
        return;
    bool empty = true;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        empty = sqlite3_column_int(stmt, 0) == 0;
    }
    sqlite3_finalize(stmt);
    if (!empty) return;

    struct SeedRule {
        const char* jurisdiction;
        const char* event_type;
        const char* description;
        int months;
        int days;
        bool extendable;
        int max_ext;
        const char* notes;
    };
    // Suggested deadlines only - the UI marks calculated dates as
    // "Needs confirmation" and users can override them (deadline_source=manual).
    const SeedRule seeds[] = {
        {"CN", "oa_response_invention", "发明OA答复期限（发文日起）", 4, 0, true, 1,
         "专利法实施细则：发文日起4个月，可请求延长1个月"},
        {"CN", "oa_response_utility", "实用新型/外观OA答复期限（发文日起）", 2, 0, true, 1,
         "专利法实施细则：发文日起2个月，可请求延长1个月"},
        {"PCT", "national_phase_entry", "PCT进入国家阶段期限（优先权日起）", 30, 0, false, 0,
         "PCT细则：自优先权日起30个月"},
        {"US", "oa_response", "US OA response statutory period (from mail date)", 3, 0, true, 3,
         "37 CFR 1.136: 3-month statutory period, extensions of time available (fees apply)"},
        {"US", "notice_of_allowance_issue_fee", "US Issue Fee due (from NOA mail date)", 3, 0, true, 3,
         "37 CFR 1.136; extensions available"},
    };

    for (const auto& r : seeds) {
        std::string sql = std::string(
            "INSERT INTO deadline_rules (jurisdiction, event_type, rule_description, "
            "base_months, base_days, extendable, max_extension_months, enabled, notes) VALUES ('") +
            r.jurisdiction + "','" + r.event_type + "','" + r.description + "'," +
            std::to_string(r.months) + "," + std::to_string(r.days) + "," +
            (r.extendable ? "1" : "0") + "," + std::to_string(r.max_ext) + ",1,'" + r.notes + "');";
        Exec(db, sql);
    }
    PATX_LOG_INFO("Seeded deadline rules with default CN/PCT/US entries");
}

int MigrateNotesPrefixesToColumns(sqlite3* db) {
    // Build prefix -> column lookup
    std::map<std::string, std::string> prefix_to_column;
    for (const auto& [column, prefix] : PatentV2Columns()) {
        prefix_to_column[prefix] = column;
    }

    struct Row {
        int id;
        std::string notes;
        std::map<std::string, std::string> extracted;
        std::string remaining;
    };
    std::vector<Row> rows;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT id, notes FROM patents WHERE notes IS NOT NULL AND notes != ''",
                           -1, &stmt, nullptr) != SQLITE_OK)
        return 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Row row;
        row.id = sqlite3_column_int(stmt, 0);
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (!text) continue;
        row.notes = text;

        // Split on "; " and try "<prefix>: value" per segment
        std::vector<std::string> segments;
        std::string current;
        for (size_t i = 0; i < row.notes.size(); i++) {
            if (i + 1 < row.notes.size() && row.notes[i] == ';' && row.notes[i + 1] == ' ') {
                segments.push_back(current);
                current.clear();
                i++; // skip the space
            } else {
                current += row.notes[i];
            }
        }
        segments.push_back(current);

        std::vector<std::string> kept;
        for (auto& segment : segments) {
            bool matched = false;
            for (const auto& [prefix, column] : prefix_to_column) {
                std::string tag = prefix + ": ";
                if (segment.compare(0, tag.size(), tag) == 0 && segment.size() > tag.size()) {
                    if (row.extracted.find(column) == row.extracted.end()) {
                        row.extracted[column] = segment.substr(tag.size());
                        matched = true;
                    }
                    // Duplicate segment with the same prefix: drop the duplicate
                    break;
                }
            }
            if (!matched) kept.push_back(segment);
        }

        // Rebuild notes from unmatched segments
        std::string remaining;
        for (size_t i = 0; i < kept.size(); i++) {
            if (i > 0) remaining += "; ";
            remaining += kept[i];
        }
        row.remaining = remaining;
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);

    int migrated_fields = 0;
    for (const auto& row : rows) {
        if (row.extracted.empty()) continue;

        std::string sql = "UPDATE patents SET notes=" + Quote(row.remaining);
        for (const auto& [column, value] : row.extracted) {
            // Only fill empty columns - never overwrite real structured data
            sql += ", " + column + "=COALESCE(NULLIF(" + column + ", ''), " + Quote(value) + ")";
            migrated_fields++;
        }
        sql += " WHERE id=" + std::to_string(row.id) + ";";
        Exec(db, sql);
    }

    if (migrated_fields > 0) {
        PATX_LOG_INFO("Migrated " + std::to_string(migrated_fields) +
                      " structured fields out of patents.notes");
    }
    return migrated_fields;
}

bool ApplyV3ToV4(sqlite3* db) {
    // Phase-2 dossier download bookkeeping: where the PDF/PNG landed.
    if (!HasColumn(db, "prosecution_documents", "local_path") &&
        !Exec(db, "ALTER TABLE prosecution_documents ADD COLUMN local_path TEXT;")) {
        return false;
    }
    if (!HasColumn(db, "prosecution_documents", "downloaded_at") &&
        !Exec(db, "ALTER TABLE prosecution_documents ADD COLUMN downloaded_at INTEGER DEFAULT 0;")) {
        return false;
    }
    return true;
}

SchemaMigrationResult RunSchemaMigrations(sqlite3* db, const std::string& db_path) {
    SchemaMigrationResult result;
    if (!db) {
        result.ok = false;
        result.error = "database handle is null";
        return result;
    }

    // InitTables() creates the full current shape with CREATE TABLE IF NOT
    // EXISTS, so both fresh and legacy databases have the core tables by now.
    // They are distinguished by shape: a legacy (v1) patents table lacks the
    // structured columns that v2 adds and has no schema_info version row.
    int version = ReadSchemaVersion(db);
    result.from_version = version;

    if (version == 0) {
        const bool has_v2_shape =
            HasTable(db, "patents") && HasColumn(db, "patents", "technology_route");
        const bool has_current_shape = has_v2_shape &&
            HasColumn(db, "patents", "publication_number") &&
            HasColumn(db, "oa_records", "sync_flag") &&
            HasTable(db, "prosecution_documents") && HasTable(db, "dossier_sync_state");
        if (has_current_shape || !HasTable(db, "patents")) {
            // Fresh database (or one without business tables): nothing to
            // migrate, stamp the current version.
            if (!Exec(db, "CREATE TABLE IF NOT EXISTS schema_info (version INTEGER NOT NULL);",
                      &result.error) ||
                !Exec(db, "INSERT INTO schema_info (version) VALUES (" +
                              std::to_string(kSchemaVersionCurrent) + ");", &result.error)) {
                result.ok = false;
                return result;
            }
            SeedDeadlineRulesIfEmpty(db);
            result.to_version = kSchemaVersionCurrent;
            return result;
        }
        if (has_v2_shape) {
            if (!Exec(db, "CREATE TABLE IF NOT EXISTS schema_info (version INTEGER NOT NULL);",
                      &result.error) ||
                !Exec(db, "INSERT INTO schema_info (version) VALUES (2);", &result.error)) {
                result.ok = false;
                return result;
            }
            version = 2;
        }
        // Legacy v1 database with real user data - back it up before touching
        // anything. The backup is a byte-for-byte copy taken before the
        // migration transaction starts.
        if (db_path != ":memory:") {
            time_t now = time(nullptr);
            struct tm tm_buf;
#ifdef _WIN32
            localtime_s(&tm_buf, &now);
#else
            localtime_r(&now, &tm_buf);
#endif
            char ts[64];
            snprintf(ts, sizeof(ts), "%04d%02d%02d_%02d%02d%02d",
                     tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                     tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
            std::string backup = db_path + ".pre_migration_" + ts + ".bak";
            std::ifstream src(db_path, std::ios::binary);
            if (src.is_open()) {
                std::ofstream dst(backup, std::ios::binary);
                dst << src.rdbuf();
                dst.close();
                src.close();
                result.performed_backup = true;
                result.backup_path = backup;
                PATX_LOG_INFO("Pre-migration backup written: " + backup);
            }
        }
        if (!has_v2_shape) version = 1;
    }

    while (version < kSchemaVersionCurrent) {
        if (version == 1) {
            PATX_LOG_INFO("Applying schema migration v1 -> v2");
            if (!Exec(db, "BEGIN TRANSACTION;", &result.error)) {
                result.ok = false;
                return result;
            }
            if (!ApplyV1ToV2(db)) {
                Exec(db, "ROLLBACK;");
                result.ok = false;
                result.error = "v1->v2 migration failed (rolled back); backup: " + result.backup_path;
                return result;
            }
            // Verify the step landed before committing
            if (!HasColumn(db, "patents", "technology_route") || !HasTable(db, "deadline_rules")) {
                Exec(db, "ROLLBACK;");
                result.ok = false;
                result.error = "v1->v2 verification failed (rolled back)";
                return result;
            }
            if (!Exec(db, "CREATE TABLE IF NOT EXISTS schema_info (version INTEGER NOT NULL);",
                      &result.error) ||
                !Exec(db, "DELETE FROM schema_info;", &result.error) ||
                !Exec(db, "INSERT INTO schema_info (version) VALUES (2);", &result.error) ||
                !Exec(db, "COMMIT;", &result.error)) {
                Exec(db, "ROLLBACK;");
                result.ok = false;
                return result;
            }
            version = 2;
            PATX_LOG_INFO("Schema migration v1 -> v2 committed");
        } else if (version == 2) {
            PATX_LOG_INFO("Applying schema migration v2 -> v3");
            if (!Exec(db, "BEGIN TRANSACTION;", &result.error)) {
                result.ok = false;
                return result;
            }
            if (!ApplyV2ToV3(db)) {
                Exec(db, "ROLLBACK;");
                result.ok = false;
                result.error = "v2->v3 migration failed (rolled back)";
                return result;
            }
            if (!HasColumn(db, "patents", "publication_number") ||
                !HasColumn(db, "oa_records", "sync_flag") ||
                !HasTable(db, "prosecution_documents") || !HasTable(db, "dossier_sync_state")) {
                Exec(db, "ROLLBACK;");
                result.ok = false;
                result.error = "v2->v3 verification failed (rolled back)";
                return result;
            }
            if (!Exec(db, "DELETE FROM schema_info;", &result.error) ||
                !Exec(db, "INSERT INTO schema_info (version) VALUES (3);", &result.error) ||
                !Exec(db, "COMMIT;", &result.error)) {
                Exec(db, "ROLLBACK;");
                result.ok = false;
                return result;
            }
            version = 3;
            PATX_LOG_INFO("Schema migration v2 -> v3 committed");
        } else if (version == 3) {
            PATX_LOG_INFO("Applying schema migration v3 -> v4");
            if (!Exec(db, "BEGIN TRANSACTION;", &result.error)) {
                result.ok = false;
                return result;
            }
            if (!ApplyV3ToV4(db)) {
                Exec(db, "ROLLBACK;");
                result.ok = false;
                result.error = "v3->v4 migration failed (rolled back)";
                return result;
            }
            if (!HasColumn(db, "prosecution_documents", "local_path")) {
                Exec(db, "ROLLBACK;");
                result.ok = false;
                result.error = "v3->v4 verification failed (rolled back)";
                return result;
            }
            if (!Exec(db, "DELETE FROM schema_info;", &result.error) ||
                !Exec(db, "INSERT INTO schema_info (version) VALUES (4);", &result.error) ||
                !Exec(db, "COMMIT;", &result.error)) {
                Exec(db, "ROLLBACK;");
                result.ok = false;
                return result;
            }
            version = 4;
            PATX_LOG_INFO("Schema migration v3 -> v4 committed");
        } else {
            result.ok = false;
            result.error = "unknown schema version " + std::to_string(version);
            return result;
        }
    }

    result.to_version = version;
    return result;
}

} // namespace patx
