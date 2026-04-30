/**
 * patX Data Migration Tool Implementation
 */

#include "migration.hpp"
#include <sqlite3.h>
#include <iostream>

MigrationStatus Migration::MigrateFromPython(
    const std::string& python_db_path,
    Database& target_db,
    bool overwrite)
{
    MigrationStatus status;
    status.source_db = python_db_path;
    status.success = false;

    sqlite3* source_db = nullptr;
    int result = sqlite3_open_v2(python_db_path.c_str(), &source_db, SQLITE_OPEN_READONLY, nullptr);
    if (result != SQLITE_OK) {
        last_error_ = "Cannot open source database";
        return status;
    }

    const char* sql = "SELECT * FROM patents;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(source_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Patent p;

            const char* geke = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (geke) p.geke_code = geke;

            const char* app_no = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            if (app_no) p.application_number = app_no;

            const char* title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            if (title) p.title = title;

            const char* app_date = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            if (app_date) p.application_date = app_date;

            const char* ptype = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
            if (ptype) p.patent_type = ptype;

            const char* status_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
            if (status_str) p.application_status = status_str;

            const char* inventor = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
            if (inventor) p.inventor = inventor;

            p.patent_type = p.patent_type.empty() ? "invention" : p.patent_type;
            p.patent_level = "normal";
            p.application_status = p.application_status.empty() ? "pending" : p.application_status;

            if (!p.geke_code.empty() && !p.title.empty()) {
                target_db.InsertPatent(p);
                status.patents_migrated++;
            }
        }
        sqlite3_finalize(stmt);
    }

    sqlite3_close(source_db);
    status.success = true;

    std::cout << "[Migration] Migrated " << status.patents_migrated << " patents" << std::endl;
    return status;
}

std::string Migration::DetectPythonDbVersion(const std::string& db_path) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        return "unknown";
    }

    std::string version = "1.0";
    const char* sql = "SELECT name FROM sqlite_master WHERE type='table';";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (name && std::string(name) == "software_copyrights") {
                version = "2.5+";
            }
        }
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);
    return version;
}

std::string Migration::GetLastError() const {
    return last_error_;
}

Migration& GetMigration() {
    static Migration instance;
    return instance;
}
