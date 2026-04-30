#include "undo_manager.hpp"
#include "database.hpp"
#include <chrono>
#include <sstream>
#include <cstring>

UndoManager::UndoManager() = default;
UndoManager::~UndoManager() = default;

void UndoManager::SetDatabase(sqlite3* db) {
    db_ = db;

    // Create undo_log table
    if (db_) {
        const char* sql =
            "CREATE TABLE IF NOT EXISTS undo_log ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "operation TEXT NOT NULL,"
            "table_name TEXT NOT NULL,"
            "record_id INTEGER,"
            "old_data TEXT,"
            "new_data TEXT,"
            "batch_id INTEGER,"
            "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
            ");";
        sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
    }
}

void UndoManager::BeginBatch() {
    using namespace std::chrono;
    auto ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    current_batch_id_ = ms;
}

void UndoManager::LogOperation(const std::string& operation,
                                const std::string& table_name,
                                int record_id,
                                const std::string& old_data,
                                const std::string& new_data) {
    if (!db_) return;

    if (current_batch_id_ == 0) BeginBatch();

    const char* sql =
        "INSERT INTO undo_log (operation, table_name, record_id, old_data, new_data, batch_id) "
        "VALUES (?, ?, ?, ?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return;

    sqlite3_bind_text(stmt, 1, operation.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, table_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, record_id);
    sqlite3_bind_text(stmt, 4, old_data.c_str(), -1, SQLITE_TRANSIENT);
    if (!new_data.empty()) {
        sqlite3_bind_text(stmt, 5, new_data.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 5);
    }
    sqlite3_bind_int64(stmt, 6, current_batch_id_);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

bool UndoManager::CanUndo() const {
    if (!db_) return false;

    const char* sql = "SELECT COUNT(*) FROM undo_log;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    bool can = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        can = sqlite3_column_int(stmt, 0) > 0;
    }
    sqlite3_finalize(stmt);
    return can;
}

int UndoManager::Undo() {
    if (!db_) {
        last_error_ = "Database not connected";
        return 0;
    }

    // Find the most recent batch
    const char* batch_sql = "SELECT DISTINCT batch_id FROM undo_log ORDER BY batch_id DESC LIMIT 1;";
    sqlite3_stmt* batch_stmt = nullptr;
    if (sqlite3_prepare_v2(db_, batch_sql, -1, &batch_stmt, nullptr) != SQLITE_OK) {
        last_error_ = "Failed to query undo log";
        return 0;
    }

    if (sqlite3_step(batch_stmt) != SQLITE_ROW) {
        sqlite3_finalize(batch_stmt);
        last_error_ = "No undoable operations";
        return 0;
    }

    int64_t batch_id = sqlite3_column_int64(batch_stmt, 0);
    sqlite3_finalize(batch_stmt);

    // Get all logs in this batch (reverse order)
    const char* logs_sql =
        "SELECT operation, table_name, record_id, old_data FROM undo_log "
        "WHERE batch_id = ? ORDER BY id DESC;";
    sqlite3_stmt* logs_stmt = nullptr;
    if (sqlite3_prepare_v2(db_, logs_sql, -1, &logs_stmt, nullptr) != SQLITE_OK) {
        last_error_ = "Failed to read undo log";
        return 0;
    }

    int count = 0;
    sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

    while (sqlite3_step(logs_stmt) == SQLITE_ROW) {
        const char* operation = reinterpret_cast<const char*>(sqlite3_column_text(logs_stmt, 0));
        const char* table_name = reinterpret_cast<const char*>(sqlite3_column_text(logs_stmt, 1));
        int record_id = sqlite3_column_int(logs_stmt, 2);
        const char* old_data = reinterpret_cast<const char*>(sqlite3_column_text(logs_stmt, 3));

        if (!operation || !table_name || !old_data) continue;

        std::string op(operation);
        std::string table(table_name);
        std::string data(old_data);

        if (op == "delete") {
            if (RestoreDeletedRecord(table, data)) count++;
        } else if (op == "update") {
            if (RestoreUpdatedRecord(table, record_id, data)) count++;
        }
    }

    sqlite3_finalize(logs_stmt);

    // Delete the batch
    sqlite3_stmt* del_stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM undo_log WHERE batch_id = ?;", -1, &del_stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(del_stmt, 1, batch_id);
        sqlite3_step(del_stmt);
        sqlite3_finalize(del_stmt);
    }

    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);

    return count;
}

std::string UndoManager::GetLastError() const {
    return last_error_;
}

bool UndoManager::RestoreDeletedRecord(const std::string& table_name, const std::string& old_data) {
    std::string sql;

    if (table_name == "patents") {
        sql = SerializePatentToInsert(old_data);
    } else if (table_name == "oa_records") {
        sql = SerializeOARecordToInsert(old_data);
    } else {
        sql = SerializeGenericToInsert(table_name, old_data);
    }

    if (sql.empty()) return false;

    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        if (err) {
            last_error_ = "Restore failed: " + std::string(err);
            sqlite3_free(err);
        }
        return false;
    }
    return true;
}

bool UndoManager::RestoreUpdatedRecord(const std::string& table_name, int record_id, const std::string& old_data) {
    std::string sql;

    if (table_name == "patents") {
        sql = SerializeGenericToUpdate("patents", old_data, record_id);
    } else if (table_name == "oa_records") {
        sql = SerializeGenericToUpdate("oa_records", old_data, record_id);
    } else {
        sql = SerializeGenericToUpdate(table_name, old_data, record_id);
    }

    if (sql.empty()) return false;

    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        if (err) {
            last_error_ = "Restore update failed: " + std::string(err);
            sqlite3_free(err);
        }
        return false;
    }
    return true;
}

// JSON helpers - minimal parsing without external deps

std::string UndoManager::JsonGetString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    pos++;

    // Skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

    if (pos >= json.size()) return "";

    if (json[pos] != '"') return "";

    pos++;
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            pos++;
            result += json[pos];
        } else {
            result += json[pos];
        }
        pos++;
    }
    return result;
}

int UndoManager::JsonGetInt(const std::string& json, const std::string& key, int default_val) {
    std::string val = JsonGetString(json, key);
    if (val.empty()) return default_val;
    try {
        return std::stoi(val);
    } catch (...) {
        return default_val;
    }
}

// Serialization helpers

std::string UndoManager::SerializePatentToInsert(const std::string& json) {
    auto q = [](const std::string& s) -> std::string {
        std::string r;
        for (char c : s) {
            if (c == '\'') r += "''";
            else r += c;
        }
        return r;
    };

    std::ostringstream sql;
    sql << "INSERT INTO patents (geke_code, application_number, title, proposal_name, "
        << "application_status, patent_type, patent_level, application_date, authorization_date, "
        << "expiration_date, geke_handler, rd_department, agency_firm, "
        << "original_applicant, current_applicant, inventor, notes, "
        << "class_level1, class_level2, class_level3) VALUES (";

    sql << "'" << q(JsonGetString(json, "geke_code")) << "',";
    sql << "'" << q(JsonGetString(json, "application_number")) << "',";
    sql << "'" << q(JsonGetString(json, "title")) << "',";
    sql << "'" << q(JsonGetString(json, "proposal_name")) << "',";
    sql << "'" << q(JsonGetString(json, "application_status")) << "',";
    sql << "'" << q(JsonGetString(json, "patent_type")) << "',";
    sql << "'" << q(JsonGetString(json, "patent_level")) << "',";
    sql << "'" << q(JsonGetString(json, "application_date")) << "',";
    sql << "'" << q(JsonGetString(json, "authorization_date")) << "',";
    sql << "'" << q(JsonGetString(json, "expiration_date")) << "',";
    sql << "'" << q(JsonGetString(json, "geke_handler")) << "',";
    sql << "'" << q(JsonGetString(json, "rd_department")) << "',";
    sql << "'" << q(JsonGetString(json, "agency_firm")) << "',";
    sql << "'" << q(JsonGetString(json, "original_applicant")) << "',";
    sql << "'" << q(JsonGetString(json, "current_applicant")) << "',";
    sql << "'" << q(JsonGetString(json, "inventor")) << "',";
    sql << "'" << q(JsonGetString(json, "notes")) << "',";
    sql << "'" << q(JsonGetString(json, "class_level1")) << "',";
    sql << "'" << q(JsonGetString(json, "class_level2")) << "',";
    sql << "'" << q(JsonGetString(json, "class_level3")) << "');";

    return sql.str();
}

std::string UndoManager::SerializeOARecordToInsert(const std::string& json) {
    auto q = [](const std::string& s) -> std::string {
        std::string r;
        for (char c : s) { if (c == '\'') r += "''"; else r += c; }
        return r;
    };

    std::ostringstream sql;
    sql << "INSERT INTO oa_records (patent_id, geke_code, patent_title, oa_type, "
        << "official_deadline, issue_date, response_date, handler, writer, progress, "
        << "agency, oa_summary, is_completed, is_extendable, extension_requested, "
        << "extension_months, extended_deadline, notes) VALUES (";

    sql << JsonGetInt(json, "patent_id") << ",";
    sql << "'" << q(JsonGetString(json, "geke_code")) << "',";
    sql << "'" << q(JsonGetString(json, "patent_title")) << "',";
    sql << "'" << q(JsonGetString(json, "oa_type")) << "',";
    sql << "'" << q(JsonGetString(json, "official_deadline")) << "',";
    sql << "'" << q(JsonGetString(json, "issue_date")) << "',";
    sql << "'" << q(JsonGetString(json, "response_date")) << "',";
    sql << "'" << q(JsonGetString(json, "handler")) << "',";
    sql << "'" << q(JsonGetString(json, "writer")) << "',";
    sql << "'" << q(JsonGetString(json, "progress")) << "',";
    sql << "'" << q(JsonGetString(json, "agency")) << "',";
    sql << "'" << q(JsonGetString(json, "oa_summary")) << "',";
    sql << JsonGetInt(json, "is_completed") << ",";
    sql << JsonGetInt(json, "is_extendable") << ",";
    sql << JsonGetInt(json, "extension_requested") << ",";
    sql << JsonGetInt(json, "extension_months") << ",";
    sql << "'" << q(JsonGetString(json, "extended_deadline")) << "',";
    sql << "'" << q(JsonGetString(json, "notes")) << "');";

    return sql.str();
}

std::string UndoManager::SerializeGenericToInsert(const std::string& table, const std::string& json) {
    // For pct_patents, software_copyrights, ic_layouts, foreign_patents
    auto q = [](const std::string& s) -> std::string {
        std::string r;
        for (char c : s) { if (c == '\'') r += "''"; else r += c; }
        return r;
    };

    if (table == "pct_patents") {
        std::ostringstream sql;
        sql << "INSERT INTO pct_patents (geke_code, domestic_source, application_no, "
            << "country_app_no, title, application_status, handler, inventor, "
            << "filing_date, application_date, priority_date, country, notes) VALUES ('"
            << q(JsonGetString(json, "geke_code")) << "','"
            << q(JsonGetString(json, "domestic_source")) << "','"
            << q(JsonGetString(json, "application_no")) << "','"
            << q(JsonGetString(json, "country_app_no")) << "','"
            << q(JsonGetString(json, "title")) << "','"
            << q(JsonGetString(json, "application_status")) << "','"
            << q(JsonGetString(json, "handler")) << "','"
            << q(JsonGetString(json, "inventor")) << "','"
            << q(JsonGetString(json, "filing_date")) << "','"
            << q(JsonGetString(json, "application_date")) << "','"
            << q(JsonGetString(json, "priority_date")) << "','"
            << q(JsonGetString(json, "country")) << "','"
            << q(JsonGetString(json, "notes")) << "');";
        return sql.str();
    }

    if (table == "software_copyrights") {
        std::ostringstream sql;
        sql << "INSERT INTO software_copyrights (case_no, reg_no, title, original_owner, "
            << "current_owner, application_status, handler, developer, inventor, "
            << "dev_complete_date, application_date, reg_date, version, notes) VALUES ('"
            << q(JsonGetString(json, "case_no")) << "','"
            << q(JsonGetString(json, "reg_no")) << "','"
            << q(JsonGetString(json, "title")) << "','"
            << q(JsonGetString(json, "original_owner")) << "','"
            << q(JsonGetString(json, "current_owner")) << "','"
            << q(JsonGetString(json, "application_status")) << "','"
            << q(JsonGetString(json, "handler")) << "','"
            << q(JsonGetString(json, "developer")) << "','"
            << q(JsonGetString(json, "inventor")) << "','"
            << q(JsonGetString(json, "dev_complete_date")) << "','"
            << q(JsonGetString(json, "application_date")) << "','"
            << q(JsonGetString(json, "reg_date")) << "','"
            << q(JsonGetString(json, "version")) << "','"
            << q(JsonGetString(json, "notes")) << "');";
        return sql.str();
    }

    if (table == "ic_layouts") {
        std::ostringstream sql;
        sql << "INSERT INTO ic_layouts (case_no, reg_no, title, original_owner, "
            << "current_owner, application_status, handler, designer, inventor, "
            << "application_date, creation_date, cert_date, notes) VALUES ('"
            << q(JsonGetString(json, "case_no")) << "','"
            << q(JsonGetString(json, "reg_no")) << "','"
            << q(JsonGetString(json, "title")) << "','"
            << q(JsonGetString(json, "original_owner")) << "','"
            << q(JsonGetString(json, "current_owner")) << "','"
            << q(JsonGetString(json, "application_status")) << "','"
            << q(JsonGetString(json, "handler")) << "','"
            << q(JsonGetString(json, "designer")) << "','"
            << q(JsonGetString(json, "inventor")) << "','"
            << q(JsonGetString(json, "application_date")) << "','"
            << q(JsonGetString(json, "creation_date")) << "','"
            << q(JsonGetString(json, "cert_date")) << "','"
            << q(JsonGetString(json, "notes")) << "');";
        return sql.str();
    }

    if (table == "foreign_patents") {
        std::ostringstream sql;
        sql << "INSERT INTO foreign_patents (case_no, pct_no, country_app_no, title, "
            << "owner, patent_status, handler, inventor, application_date, "
            << "authorization_date, country, application_no, notes) VALUES ('"
            << q(JsonGetString(json, "case_no")) << "','"
            << q(JsonGetString(json, "pct_no")) << "','"
            << q(JsonGetString(json, "country_app_no")) << "','"
            << q(JsonGetString(json, "title")) << "','"
            << q(JsonGetString(json, "owner")) << "','"
            << q(JsonGetString(json, "patent_status")) << "','"
            << q(JsonGetString(json, "handler")) << "','"
            << q(JsonGetString(json, "inventor")) << "','"
            << q(JsonGetString(json, "application_date")) << "','"
            << q(JsonGetString(json, "authorization_date")) << "','"
            << q(JsonGetString(json, "country")) << "','"
            << q(JsonGetString(json, "application_no")) << "','"
            << q(JsonGetString(json, "notes")) << "');";
        return sql.str();
    }

    return "";
}

std::string UndoManager::SerializeGenericToUpdate(const std::string& table, const std::string& json, int id) {
    auto q = [](const std::string& s) -> std::string {
        std::string r;
        for (char c : s) { if (c == '\'') r += "''"; else r += c; }
        return r;
    };

    if (table == "patents") {
        std::ostringstream sql;
        sql << "UPDATE patents SET application_number='" << q(JsonGetString(json, "application_number")) << "',"
            << "title='" << q(JsonGetString(json, "title")) << "',"
            << "proposal_name='" << q(JsonGetString(json, "proposal_name")) << "',"
            << "application_status='" << q(JsonGetString(json, "application_status")) << "',"
            << "patent_type='" << q(JsonGetString(json, "patent_type")) << "',"
            << "patent_level='" << q(JsonGetString(json, "patent_level")) << "',"
            << "application_date='" << q(JsonGetString(json, "application_date")) << "',"
            << "authorization_date='" << q(JsonGetString(json, "authorization_date")) << "',"
            << "expiration_date='" << q(JsonGetString(json, "expiration_date")) << "',"
            << "geke_handler='" << q(JsonGetString(json, "geke_handler")) << "',"
            << "rd_department='" << q(JsonGetString(json, "rd_department")) << "',"
            << "agency_firm='" << q(JsonGetString(json, "agency_firm")) << "',"
            << "current_applicant='" << q(JsonGetString(json, "current_applicant")) << "',"
            << "inventor='" << q(JsonGetString(json, "inventor")) << "',"
            << "notes='" << q(JsonGetString(json, "notes")) << "',"
            << "class_level1='" << q(JsonGetString(json, "class_level1")) << "',"
            << "class_level2='" << q(JsonGetString(json, "class_level2")) << "',"
            << "class_level3='" << q(JsonGetString(json, "class_level3")) << "'"
            << " WHERE id=" << id << ";";
        return sql.str();
    }

    if (table == "oa_records") {
        std::ostringstream sql;
        sql << "UPDATE oa_records SET oa_type='" << q(JsonGetString(json, "oa_type")) << "',"
            << "issue_date='" << q(JsonGetString(json, "issue_date")) << "',"
            << "official_deadline='" << q(JsonGetString(json, "official_deadline")) << "',"
            << "handler='" << q(JsonGetString(json, "handler")) << "',"
            << "writer='" << q(JsonGetString(json, "writer")) << "',"
            << "progress='" << q(JsonGetString(json, "progress")) << "',"
            << "agency='" << q(JsonGetString(json, "agency")) << "',"
            << "notes='" << q(JsonGetString(json, "notes")) << "',"
            << "is_completed=" << JsonGetInt(json, "is_completed")
            << " WHERE id=" << id << ";";
        return sql.str();
    }

    return "";
}
