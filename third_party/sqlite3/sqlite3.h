#ifndef SQLITE3_H
#define SQLITE3_H

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef long long int sqlite3_int64;

#define SQLITE_OK           0
#define SQLITE_ERROR        1
#define SQLITE_BUSY         5
#define SQLITE_LOCKED       6
#define SQLITE_NOMEM        7
#define SQLITE_READONLY     8
#define SQLITE_INTERRUPT    9
#define SQLITE_IOERR       10
#define SQLITE_CORRUPT     11
#define SQLITE_NOTFOUND    12
#define SQLITE_FULL        13
#define SQLITE_CANTOPEN    14
#define SQLITE_PROTOCOL    15
#define SQLITE_EMPTY       16
#define SQLITE_SCHEMA      17
#define SQLITE_TOOBIG      18
#define SQLITE_CONSTRAINT  19
#define SQLITE_MISMATCH    20
#define SQLITE_MISUSE      21
#define SQLITE_NOLFS       22
#define SQLITE_AUTH        23
#define SQLITE_FORMAT      24
#define SQLITE_RANGE       25
#define SQLITE_NOTADB      26
#define SQLITE_ROW         100
#define SQLITE_DONE        101

#define SQLITE_TRANSIENT   ((void(*)(void*))-1)

int sqlite3_open(const char *filename, sqlite3 **ppDb);
int sqlite3_close(sqlite3 *db);
int sqlite3_exec(sqlite3 *db, const char *sql, int (*callback)(void*,int,char**,char**), void *arg, char **errmsg);
int sqlite3_prepare_v2(sqlite3 *db, const char *sql, int nByte, sqlite3_stmt **ppStmt, const char **pzTail);
int sqlite3_step(sqlite3_stmt *stmt);
int sqlite3_finalize(sqlite3_stmt *stmt);
int sqlite3_bind_text(sqlite3_stmt *stmt, int i, const char *zData, int nData, void(*xDel)(void*));
int sqlite3_bind_int(sqlite3_stmt *stmt, int i, int iValue);
int sqlite3_column_int(sqlite3_stmt *stmt, int iCol);
const char *sqlite3_column_text(sqlite3_stmt *stmt, int iCol);
int sqlite3_errcode(sqlite3 *db);
const char *sqlite3_errmsg(sqlite3 *db);
void *sqlite3_malloc(int n);
void sqlite3_free(void *p);
int sqlite3_changes(sqlite3 *db);
sqlite3_int64 sqlite3_last_insert_rowid(sqlite3 *db);

#endif
