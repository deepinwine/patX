// Versioned schema migrations for patX.
//
// Rules:
//  - Existing user data is sacred: no DROP, no recreate, no re-import.
//  - Each step runs inside a transaction and is verified before COMMIT;
//    on failure the transaction rolls back and the error is reported.
//  - Before applying a step to an existing database file, a timestamped
//    backup is written next to it (patents.db.pre_migration_<ts>.bak).
#pragma once

#include <string>

struct sqlite3;
typedef struct sqlite3 sqlite3;

namespace patx {

// Bump when adding a migration step. Fresh databases jump straight to this
// version; existing databases step up one version at a time.
inline constexpr int kSchemaVersionCurrent = 2;

struct SchemaMigrationResult {
    bool ok = true;
    int from_version = 0;
    int to_version = 0;
    bool performed_backup = false;
    std::string backup_path;
    std::string error;
};

// Reads schema_info.version (0 when the table/row is absent, meaning a legacy
// pre-0.4.0 database with no versioning yet).
int ReadSchemaVersion(sqlite3* db);

// Brings the schema up to kSchemaVersionCurrent. For a legacy database this
// applies v1->v2 (structured patent columns, OA external-link columns, notes
// prefix migration, deadline rules, indexes). Safe to call repeatedly.
SchemaMigrationResult RunSchemaMigrations(sqlite3* db, const std::string& db_path);

// Seeds the deadline_rules table with default rules if it is empty. Called on
// fresh databases and as part of v1->v2.
void SeedDeadlineRulesIfEmpty(sqlite3* db);

// Moves "prefix: value" segments previously concatenated into patents.notes
// into the structured columns added by v2. Values are only moved when the
// target column is empty; anything ambiguous stays in notes.
int MigrateNotesPrefixesToColumns(sqlite3* db);

} // namespace patx
