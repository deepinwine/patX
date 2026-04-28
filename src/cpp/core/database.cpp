/**
 * patX 数据库实现
 */

#include "database.hpp"
#include <cstring>

namespace patx {

Database::Database() : db_(nullptr) {}

Database::~Database() {
    Close();
}

bool Database::Open(const std::string& db_path) {
    db_path_ = db_path;
    
    int result = sqlite3_open(db_path.c_str(), &db_);
    if (result != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    
    // 启用 WAL 模式提升性能
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA cache_size=10000;", nullptr, nullptr, nullptr);
    
    return InitTables() && PrepareStatements();
}

void Database::Close() {
    if (db_) {
        // 释放预编译语句
        if (stmt_insert_patent_) sqlite3_finalize(stmt_insert_patent_);
        if (stmt_update_patent_) sqlite3_finalize(stmt_update_patent_);
        if (stmt_find_by_id_) sqlite3_finalize(stmt_find_by_id_);
        if (stmt_delete_by_id_) sqlite3_finalize(stmt_delete_by_id_);
        
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool Database::InitTables() {
    // 创建专利表 (兼容 Python 版结构)
    const char* create_patents_sql = 
        "CREATE TABLE IF NOT EXISTS patents ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "geke_code TEXT UNIQUE,"
        "application_number TEXT,"
        "title TEXT,"
        "applicant TEXT,"
        "application_date TEXT,"
        "patent_type TEXT,"
        "application_status TEXT,"
        "inventor TEXT,"
        "classification_number TEXT,"
        "agent TEXT,"
        "agency TEXT,"
        "publish_number TEXT,"
        "publish_date TEXT,"
        "grant_date TEXT,"
        "patent_level INTEGER DEFAULT 0,"
        "geke_handler TEXT,"
        "notes TEXT,"
        "created_at TEXT,"
        "updated_at TEXT"
        ");";
    
    // 创建索引
    const char* create_indexes_sql = 
        "CREATE INDEX IF NOT EXISTS idx_geke_code ON patents(geke_code);"
        "CREATE INDEX IF NOT EXISTS idx_application ON patents(application_number);"
        "CREATE INDEX IF NOT EXISTS idx_applicant ON patents(applicant);"
        "CREATE INDEX IF NOT EXISTS idx_type ON patents(patent_type);";
    
    char* err_msg = nullptr;
    int result = sqlite3_exec(db_, create_patents_sql, nullptr, nullptr, &err_msg);
    if (result != SQLITE_OK) {
        last_error_ = err_msg;
        sqlite3_free(err_msg);
        return false;
    }
    
    sqlite3_exec(db_, create_indexes_sql, nullptr, nullptr, nullptr);
    
    // 创建 OA 记录表
    const char* create_oa_sql = 
        "CREATE TABLE IF NOT EXISTS oa_records ("
        "id INTEGER PRIMARY KEY,"
        "geke_code TEXT,"
        "patent_title TEXT,"
        "oa_type TEXT,"
        "issue_date TEXT,"
        "official_deadline TEXT,"
        "handler TEXT,"
        "is_completed INTEGER DEFAULT 0,"
        "created_at TEXT,"
        "updated_at TEXT"
        ");";
    
    sqlite3_exec(db_, create_oa_sql, nullptr, nullptr, nullptr);
    
    return true;
}

bool Database::PrepareStatements() {
    const char* insert_sql = 
        "INSERT INTO patents (geke_code, application_number, title, applicant, "
        "application_date, patent_type, application_status, inventor, "
        "classification_number, agent, agency, publish_number, publish_date, "
        "grant_date, patent_level, geke_handler, notes, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    
    int result = sqlite3_prepare_v2(db_, insert_sql, -1, &stmt_insert_patent_, nullptr);
    return result == SQLITE_OK;
}

bool Database::InsertPatent(const Patent& patent) {
    sqlite3_reset(stmt_insert_patent_);
    
    // 绑定参数
    sqlite3_bind_text(stmt_insert_patent_, 1, patent.geke_code, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_patent_, 2, patent.application_no, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_patent_, 3, patent.title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_patent_, 4, patent.applicant, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_patent_, 5, patent.application_date, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_patent_, 6, patent.patent_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_patent_, 7, patent.legal_status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_patent_, 8, patent.inventor, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_patent_, 9, patent.classification, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_patent_, 10, patent.agent, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_patent_, 11, patent.agency, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_patent_, 12, patent.publish_no, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_patent_, 13, patent.publish_date, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_patent_, 14, patent.grant_date, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_insert_patent_, 15, patent.patent_level);
    sqlite3_bind_text(stmt_insert_patent_, 16, patent.handler, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_patent_, 17, patent.notes, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_patent_, 18, patent.created_at, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_insert_patent_, 19, patent.updated_at, -1, SQLITE_TRANSIENT);
    
    int result = sqlite3_step(stmt_insert_patent_);
    return result == SQLITE_DONE;
}

bool Database::InsertPatentsBatch(const std::vector<Patent>& patents) {
    BeginTransaction();
    
    for (const auto& p : patents) {
        if (!InsertPatent(p)) {
            Rollback();
            return false;
        }
    }
    
    return Commit();
}

bool Database::BeginTransaction() {
    return sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool Database::Commit() {
    return sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool Database::Rollback() {
    return sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr) == SQLITE_OK;
}

Database& GetDatabase() {
    static Database instance;
    return instance;
}

} // namespace patx