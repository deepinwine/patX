#include "sqlite3.h"

/* Stub implementation - real SQLite should be installed */
int sqlite3_open(const char *filename, sqlite3 **ppDb) { return SQLITE_ERROR; }
int sqlite3_close(sqlite3 *db) { return SQLITE_OK; }
int sqlite3_exec(sqlite3 *db, const char *sql, int (*callback)(void*,int,char**,char**), void *arg, char **errmsg) { return SQLITE_ERROR; }
int sqlite3_prepare_v2(sqlite3 *db, const char *sql, int nByte, sqlite3_stmt **ppStmt, const char **pzTail) { return SQLITE_ERROR; }
int sqlite3_step(sqlite3_stmt *stmt) { return SQLITE_DONE; }
int sqlite3_finalize(sqlite3_stmt *stmt) { return SQLITE_OK; }
int sqlite3_bind_text(sqlite3_stmt *stmt, int i, const char *zData, int nData, void(*xDel)(void*)) { return SQLITE_OK; }
int sqlite3_bind_int(sqlite3_stmt *stmt, int i, int iValue) { return SQLITE_OK; }
int sqlite3_column_int(sqlite3_stmt *stmt, int iCol) { return 0; }
const char *sqlite3_column_text(sqlite3_stmt *stmt, int iCol) { return ""; }
int sqlite3_errcode(sqlite3 *db) { return SQLITE_OK; }
const char *sqlite3_errmsg(sqlite3 *db) { return ""; }
void *sqlite3_malloc(int n) { return 0; }
void sqlite3_free(void *p) {}
int sqlite3_changes(sqlite3 *db) { return 0; }
sqlite3_int64 sqlite3_last_insert_rowid(sqlite3 *db) { return 0; }
