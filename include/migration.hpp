/**
 * patX 数据迁移工具
 * 
 * 从 Python 版 SQLite 数据库迁移数据
 */

#pragma once

#include <string>
#include <vector>
#include "patent.hpp"

namespace patx {

// 迁移状态
struct MigrationStatus {
    int patents_migrated;
    int oa_records_migrated;
    int pct_patents_migrated;
    int software_migrated;
    int ic_layouts_migrated;
    int foreign_migrated;
    int errors;
    std::string source_db;
    std::string target_db;
    bool success;
};

class Migration {
public:
    Migration();
    ~Migration();
    
    // 从 Python 版数据库迁移
    MigrationStatus MigrateFromPython(
        const std::string& python_db_path,
        const std::string& target_db_path,
        bool overwrite = false
    );
    
    // 检测 Python 版数据库版本
    std::string DetectPythonDbVersion(const std::string& db_path);
    
    // 验证数据完整性
    bool ValidateMigration(const std::string& source_db, const std::string& target_db);
    
    // 获取迁移进度
    int GetProgress() const;
    
    // 错误处理
    std::string GetLastError() const;
    
private:
    std::string last_error_;
    int progress_;
    
    // 内部迁移方法
    bool MigratePatents(void* source_db, void* target_db);
    bool MigrateOaRecords(void* source_db, void* target_db);
    bool MigratePctPatents(void* source_db, void* target_db);
    bool MigrateSoftware(void* source_db, void* target_db);
    bool MigrateIcLayouts(void* source_db, void* target_db);
    bool MigrateForeignPatents(void* source_db, void* target_db);
    
    // 辅助方法
    int GetMigratedCount(void* db, const std::string& table);
};

// 全局实例
Migration& GetMigration();

} // namespace patx
