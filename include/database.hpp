/**
 * patX 数据库管理模块
 * 
 * 基于 SQLite3 原生 C 接口的高性能数据库操作
 */

#pragma once

#include <sqlite3.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include "patent.hpp"

namespace patx {

class Database {
public:
    Database();
    ~Database();
    
    // 打开/关闭数据库
    bool Open(const std::string& db_path);
    void Close();
    bool IsOpen() const;
    
    // 表结构初始化 (兼容 Python 版)
    bool InitTables();
    
    // 专利 CRUD
    bool InsertPatent(const Patent& patent);
    bool UpdatePatent(const Patent& patent);
    bool DeletePatent(int32_t id);
    Patent* FindPatentById(int32_t id);
    
    // 批量操作
    bool InsertPatentsBatch(const std::vector<Patent>& patents);
    std::vector<Patent> LoadAllPatents();
    
    // 检索
    std::vector<Patent> SearchPatents(const std::string& query);
    std::vector<Patent> SearchPatentsMulti(
        const std::string& applicant,
        const std::string& patent_type,
        const std::string& date_from,
        const std::string& date_to,
        const std::string& legal_status
    );
    
    // 统计
    Statistics GetStatistics();
    
    // 事务
    bool BeginTransaction();
    bool Commit();
    bool Rollback();
    
    // 错误处理
    std::string GetLastError() const;
    
private:
    sqlite3* db_;
    std::string db_path_;
    std::string last_error_;
    
    // 预编译语句缓存
    sqlite3_stmt* stmt_insert_patent_ = nullptr;
    sqlite3_stmt* stmt_update_patent_ = nullptr;
    sqlite3_stmt* stmt_find_by_id_ = nullptr;
    sqlite3_stmt* stmt_delete_by_id_ = nullptr;
    
    bool PrepareStatements();
    Patent ParsePatent(sqlite3_stmt* stmt);
};

// 全局数据库实例
Database& GetDatabase();

} // namespace patx