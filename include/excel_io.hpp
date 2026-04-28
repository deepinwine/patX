/**
 * patX Excel 导入导出模块
 * 
 * 高性能 Excel 处理 - 兼容 Python 版数据格式
 */

#pragma once

#include <string>
#include <vector>
#include <functional>
#include "patent.hpp"

namespace patx {

// Excel 工作表信息
struct SheetInfo {
    std::string name;
    int rows;
    int cols;
};

// Excel 导入结果
struct ImportResult {
    int added;
    int updated;
    int skipped;
    int errors;
    std::vector<std::string> error_messages;
};

// Excel 导出选项
struct ExportOptions {
    bool include_headers = true;
    bool include_notes = true;
    std::string sheet_name = "专利数据";
    int start_row = 1;
    int start_col = 1;
};

class ExcelIO {
public:
    ExcelIO();
    ~ExcelIO();
    
    // 导入专利数据
    ImportResult ImportPatents(
        const std::string& file_path,
        std::function<bool(int, int)> progress_callback = nullptr
    );
    
    // 导出专利数据
    bool ExportPatents(
        const std::string& file_path,
        const std::vector<Patent>& patents,
        const ExportOptions& options = ExportOptions(),
        std::function<bool(int, int)> progress_callback = nullptr
    );
    
    // 获取工作表列表
    std::vector<SheetInfo> GetSheets(const std::string& file_path);
    
    // 检测文件格式
    bool IsExcelFile(const std::string& file_path);
    bool IsCsvFile(const std::string& file_path);
    
    // 错误处理
    std::string GetLastError() const;
    
private:
    std::string last_error_;
    
    // 内部解析方法
    std::vector<Patent> ParseXlsx(const std::string& file_path);
    std::vector<Patent> ParseCsv(const std::string& file_path);
    std::vector<Patent> ParseXls(const std::string& file_path);
    
    // 字段映射 (兼容 Python 版)
    int MapColumnToField(const std::string& column_name);
    Patent ParseRow(const std::vector<std::string>& row, const std::vector<int>& field_map);
};

// 全局实例
ExcelIO& GetExcelIO();

} // namespace patx