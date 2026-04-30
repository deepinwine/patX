/**
 * patX Data Migration Tool
 *
 * Migrates data from Python version SQLite database
 */

#pragma once

#include <string>
#include "database.hpp"

struct MigrationStatus {
    int patents_migrated = 0;
    int oa_records_migrated = 0;
    int pct_patents_migrated = 0;
    int software_migrated = 0;
    int ic_layouts_migrated = 0;
    int foreign_migrated = 0;
    int errors = 0;
    std::string source_db;
    std::string target_db;
    bool success = false;
};

class Migration {
public:
    MigrationStatus MigrateFromPython(
        const std::string& python_db_path,
        Database& target_db,
        bool overwrite = false
    );

    std::string DetectPythonDbVersion(const std::string& db_path);
    std::string GetLastError() const;

private:
    std::string last_error_;
};

Migration& GetMigration();
