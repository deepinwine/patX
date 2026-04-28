/**
 * patX 数据库实现
 */

#include "database.hpp"
#include <cstring>
#include <chrono>
#include <iomanip>
#include <sstream>

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
        
        stmt_insert_patent_ = nullptr;
        stmt_update_patent_ = nullptr;
        stmt_find_by_id_ = nullptr;
        stmt_delete_by_id_ = nullptr;
        
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool Database::IsOpen() const {
    return db_ != nullptr;
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
        "CREATE INDEX IF NOT EXISTS idx_type ON patents(patent_type);"
        "CREATE INDEX IF NOT EXISTS idx_status ON patents(application_status);";
    
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
    if (result != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    
    const char* update_sql = 
        "UPDATE patents SET geke_code=?, application_number=?, title=?, applicant=?, "
        "application_date=?, patent_type=?, application_status=?, inventor=?, "
        "classification_number=?, agent=?, agency=?, publish_number=?, publish_date=?, "
        "grant_date=?, patent_level=?, geke_handler=?, notes=?, updated_at=? "
        "WHERE id=?;";
    
    result = sqlite3_prepare_v2(db_, update_sql, -1, &stmt_update_patent_, nullptr);
    if (result != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    
    const char* find_sql = "SELECT * FROM patents WHERE id = ?;";
    result = sqlite3_prepare_v2(db_, find_sql, -1, &stmt_find_by_id_, nullptr);
    if (result != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    
    const char* delete_sql = "DELETE FROM patents WHERE id = ?;";
    result = sqlite3_prepare_v2(db_, delete_sql, -1, &stmt_delete_by_id_, nullptr);
    if (result != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    
    return true;
}

Patent Database::ParsePatent(sqlite3_stmt* stmt) {
    Patent p;
    memset(&p, 0, sizeof(p));
    
    p.id = sqlite3_column_int(stmt, 0);
    
    const char* geke_code = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    if (geke_code) strncpy(p.geke_code, geke_code, sizeof(p.geke_code) - 1);
    
    const char* app_no = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    if (app_no) strncpy(p.application_no, app_no, sizeof(p.application_no) - 1);
    
    const char* title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    if (title) strncpy(p.title, title, sizeof(p.title) - 1);
    
    const char* applicant = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    if (applicant) strncpy(p.applicant, applicant, sizeof(p.applicant) - 1);
    
    const char* app_date = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    if (app_date) strncpy(p.application_date, app_date, sizeof(p.application_date) - 1);
    
    const char* patent_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
    if (patent_type) strncpy(p.patent_type, patent_type, sizeof(p.patent_type) - 1);
    
    const char* legal_status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
    if (legal_status) strncpy(p.legal_status, legal_status, sizeof(p.legal_status) - 1);
    
    const char* inventor = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
    if (inventor) strncpy(p.inventor, inventor, sizeof(p.inventor) - 1);
    
    const char* classification = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
    if (classification) strncpy(p.classification, classification, sizeof(p.classification) - 1);
    
    const char* agent = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
    if (agent) strncpy(p.agent, agent, sizeof(p.agent) - 1);
    
    const char* agency = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
    if (agency) strncpy(p.agency, agency, sizeof(p.agency) - 1);
    
    const char* publish_no = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
    if (publish_no) strncpy(p.publish_no, publish_no, sizeof(p.publish_no) - 1);
    
    const char* publish_date = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
    if (publish_date) strncpy(p.publish_date, publish_date, sizeof(p.publish_date) - 1);
    
    const char* grant_date = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 14));
    if (grant_date) strncpy(p.grant_date, grant_date, sizeof(p.grant_date) - 1);
    
    p.patent_level = sqlite3_column_int(stmt, 15);
    
    const char* handler = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 16));
    if (handler) strncpy(p.handler, handler, sizeof(p.handler) - 1);
    
    const char* notes = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 17));
    if (notes) strncpy(p.notes, notes, sizeof(p.notes) - 1);
    
    const char* created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 18));
    if (created_at) strncpy(p.created_at, created_at, sizeof(p.created_at) - 1);
    
    const char* updated_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 19));
    if (updated_at) strncpy(p.updated_at, updated_at, sizeof(p.updated_at) - 1);
    
    p.is_valid = 1;
    
    return p;
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
    if (result != SQLITE_DONE) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

bool Database::UpdatePatent(const Patent& patent) {
    sqlite3_reset(stmt_update_patent_);
    
    // 绑定参数
    sqlite3_bind_text(stmt_update_patent_, 1, patent.geke_code, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_update_patent_, 2, patent.application_no, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_update_patent_, 3, patent.title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_update_patent_, 4, patent.applicant, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_update_patent_, 5, patent.application_date, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_update_patent_, 6, patent.patent_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_update_patent_, 7, patent.legal_status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_update_patent_, 8, patent.inventor, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_update_patent_, 9, patent.classification, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_update_patent_, 10, patent.agent, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_update_patent_, 11, patent.agency, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_update_patent_, 12, patent.publish_no, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_update_patent_, 13, patent.publish_date, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_update_patent_, 14, patent.grant_date, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_update_patent_, 15, patent.patent_level);
    sqlite3_bind_text(stmt_update_patent_, 16, patent.handler, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_update_patent_, 17, patent.notes, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_update_patent_, 18, patent.updated_at, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt_update_patent_, 19, patent.id);
    
    int result = sqlite3_step(stmt_update_patent_);
    if (result != SQLITE_DONE) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

bool Database::DeletePatent(int32_t id) {
    sqlite3_reset(stmt_delete_by_id_);
    sqlite3_bind_int(stmt_delete_by_id_, 1, id);
    
    int result = sqlite3_step(stmt_delete_by_id_);
    if (result != SQLITE_DONE) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

Patent* Database::FindPatentById(int32_t id) {
    sqlite3_reset(stmt_find_by_id_);
    sqlite3_bind_int(stmt_find_by_id_, 1, id);
    
    int result = sqlite3_step(stmt_find_by_id_);
    if (result == SQLITE_ROW) {
        static thread_local Patent p;
        p = ParsePatent(stmt_find_by_id_);
        return &p;
    }
    return nullptr;
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

std::vector<Patent> Database::LoadAllPatents() {
    std::vector<Patent> patents;
    
    const char* sql = "SELECT * FROM patents ORDER BY id;";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return patents;
    }
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        patents.push_back(ParsePatent(stmt));
    }
    
    sqlite3_finalize(stmt);
    return patents;
}

std::vector<Patent> Database::SearchPatents(const std::string& query) {
    std::vector<Patent> results;
    
    const char* sql = 
        "SELECT * FROM patents WHERE "
        "title LIKE ? OR applicant LIKE ? OR geke_code LIKE ? "
        "ORDER BY id LIMIT 1000;";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }
    
    std::string pattern = "%" + query + "%";
    sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, pattern.c_str(), -1, SQLITE_TRANSIENT);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(ParsePatent(stmt));
    }
    
    sqlite3_finalize(stmt);
    return results;
}

std::vector<Patent> Database::SearchPatentsMulti(
    const std::string& applicant,
    const std::string& patent_type,
    const std::string& date_from,
    const std::string& date_to,
    const std::string& legal_status
) {
    std::vector<Patent> results;
    
    std::string sql = "SELECT * FROM patents WHERE 1=1";
    std::vector<std::string> params;
    
    if (!applicant.empty()) {
        sql += " AND applicant LIKE ?";
        params.push_back("%" + applicant + "%");
    }
    if (!patent_type.empty()) {
        sql += " AND patent_type = ?";
        params.push_back(patent_type);
    }
    if (!date_from.empty()) {
        sql += " AND application_date >= ?";
        params.push_back(date_from);
    }
    if (!date_to.empty()) {
        sql += " AND application_date <= ?";
        params.push_back(date_to);
    }
    if (!legal_status.empty()) {
        sql += " AND application_status = ?";
        params.push_back(legal_status);
    }
    
    sql += " ORDER BY id LIMIT 1000;";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }
    
    for (size_t i = 0; i < params.size(); ++i) {
        sqlite3_bind_text(stmt, static_cast<int>(i + 1), params[i].c_str(), -1, SQLITE_TRANSIENT);
    }
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(ParsePatent(stmt));
    }
    
    sqlite3_finalize(stmt);
    return results;
}

Statistics Database::GetStatistics() {
    Statistics stats;
    memset(&stats, 0, sizeof(stats));
    
    // 统计总数
    const char* count_sql = "SELECT COUNT(*) FROM patents;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, count_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            stats.total_patents = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    
    // 按类型统计
    const char* type_sql = "SELECT patent_type, COUNT(*) FROM patents GROUP BY patent_type;";
    if (sqlite3_prepare_v2(db_, type_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            int count = sqlite3_column_int(stmt, 1);
            if (type && strcmp(type, "发明") == 0) stats.by_type[0] = count;
            else if (type && strcmp(type, "实用新型") == 0) stats.by_type[1] = count;
            else if (type && strcmp(type, "外观设计") == 0) stats.by_type[2] = count;
        }
        sqlite3_finalize(stmt);
    }
    
    // 按状态统计
    const char* status_sql = "SELECT application_status, COUNT(*) FROM patents GROUP BY application_status;";
    if (sqlite3_prepare_v2(db_, status_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            int count = sqlite3_column_int(stmt, 1);
            if (status && strcmp(status, "有效") == 0) stats.by_status[0] = count;
            else if (status && strcmp(status, "失效") == 0) stats.by_status[1] = count;
            else if (status && strcmp(status, " pending") == 0) stats.by_status[2] = count;
        }
        sqlite3_finalize(stmt);
    }
    
    // 统计待处理 OA
    const char* oa_sql = "SELECT COUNT(*) FROM oa_records WHERE is_completed = 0;";
    if (sqlite3_prepare_v2(db_, oa_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            stats.pending_oa = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    
    // 统计紧急 OA (截止日期在7天内)
    const char* urgent_sql = 
        "SELECT COUNT(*) FROM oa_records WHERE is_completed = 0 "
        "AND date(official_deadline) <= date('now', '+7 days');";
    if (sqlite3_prepare_v2(db_, urgent_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            stats.urgent_oa = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    
    return stats;
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

std::string Database::GetLastError() const {
    return last_error_;
}

Database& GetDatabase() {
    static Database instance;
    return instance;
}

} // namespace patx
