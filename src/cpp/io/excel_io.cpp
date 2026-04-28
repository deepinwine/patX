/**
 * patX Excel 导入导出实现
 * 
 * 使用 OpenXLSX 库进行高性能处理
 */

#include "excel_io.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cstring>

#ifdef USE_OPENXLSX
#include <OpenXLSX.hpp>
#endif

namespace patx {

ExcelIO::ExcelIO() = default;
ExcelIO::~ExcelIO() = default;

bool ExcelIO::IsExcelFile(const std::string& file_path) {
    std::string ext = file_path.substr(file_path.find_last_of('.'));
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".xlsx" || ext == ".xls";
}

bool ExcelIO::IsCsvFile(const std::string& file_path) {
    std::string ext = file_path.substr(file_path.find_last_of('.'));
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".csv" || ext == ".tsv";
}

// 字段名到字段的映射 (兼容 Python 版)
int ExcelIO::MapColumnToField(const std::string& column_name) {
    static const std::vector<std::pair<std::string, int>> field_map = {
        {"格科编码", 1},
        {"申请号", 2},
        {"发明名称", 3},
        {"申请人", 4},
        {"申请日期", 5},
        {"专利类型", 6},
        {"法律状态", 7},
        {"发明人", 8},
        {"分类号", 9},
        {"代理人", 10},
        {"代理机构", 11},
        {"公开号", 12},
        {"公开日期", 13},
        {"授权日期", 14},
        {"专利等级", 15},
        {"处理人", 16},
        {"备注", 17},
        // 英文名称兼容
        {"geke_code", 1},
        {"application_number", 2},
        {"title", 3},
        {"applicant", 4},
        {"application_date", 5},
        {"patent_type", 6},
        {"application_status", 7},
    };
    
    for (const auto& [name, field] : field_map) {
        if (column_name.find(name) != std::string::npos) {
            return field;
        }
    }
    return 0; // 未知字段
}

ImportResult ExcelIO::ImportPatents(
    const std::string& file_path,
    std::function<bool(int, int)> progress_callback
) {
    ImportResult result = {0, 0, 0, 0};
    
    std::vector<Patent> patents;
    
    if (IsCsvFile(file_path)) {
        patents = ParseCsv(file_path);
    } else if (IsExcelFile(file_path)) {
#ifdef USE_OPENXLSX
        patents = ParseXlsx(file_path);
#else
        patents = ParseCsv(file_path); // 回退到 CSV 解析
#endif
    } else {
        last_error_ = "不支持的文件格式";
        return result;
    }
    
    // TODO: 批量插入数据库
    result.added = patents.size();
    
    return result;
}

std::vector<Patent> ExcelIO::ParseCsv(const std::string& file_path) {
    std::vector<Patent> patents;
    std::ifstream file(file_path);
    
    if (!file.is_open()) {
        last_error_ = "无法打开文件: " + file_path;
        return patents;
    }
    
    std::string line;
    std::vector<int> field_map;
    int row_num = 0;
    
    // 读取标题行
    if (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, ',')) {
            field_map.push_back(MapColumnToField(cell));
        }
    }
    
    // 读取数据行
    while (std::getline(file, line)) {
        row_num++;
        std::vector<std::string> row;
        std::stringstream ss(line);
        std::string cell;
        
        while (std::getline(ss, cell, ',')) {
            // 去除引号
            if (!cell.empty() && cell.front() == '"') {
                cell = cell.substr(1);
            }
            if (!cell.empty() && cell.back() == '"') {
                cell = cell.substr(0, cell.size() - 1);
            }
            row.push_back(cell);
        }
        
        Patent p;
        memset(&p, 0, sizeof(p));
        p.id = row_num;
        
        for (size_t i = 0; i < row.size() && i < field_map.size(); i++) {
            const std::string& value = row[i];
            int field = field_map[i];
            
            switch (field) {
                case 1: strncpy(p.geke_code, value.c_str(), 15); break;
                case 2: strncpy(p.application_no, value.c_str(), 31); break;
                case 3: strncpy(p.title, value.c_str(), 255); break;
                case 4: strncpy(p.applicant, value.c_str(), 127); break;
                case 5: strncpy(p.application_date, value.c_str(), 15); break;
                case 6: strncpy(p.patent_type, value.c_str(), 31); break;
                case 7: strncpy(p.legal_status, value.c_str(), 31); break;
                case 8: strncpy(p.inventor, value.c_str(), 255); break;
                case 9: strncpy(p.classification, value.c_str(), 63); break;
                case 10: strncpy(p.agent, value.c_str(), 63); break;
                case 11: strncpy(p.agency, value.c_str(), 127); break;
                case 12: strncpy(p.publish_no, value.c_str(), 31); break;
                case 13: strncpy(p.publish_date, value.c_str(), 15); break;
                case 14: strncpy(p.grant_date, value.c_str(), 15); break;
                case 15: p.patent_level = std::stoi(value); break;
                case 16: strncpy(p.handler, value.c_str(), 31); break;
                case 17: strncpy(p.notes, value.c_str(), 511); break;
            }
        }
        
        patents.push_back(p);
    }
    
    return patents;
}

bool ExcelIO::ExportPatents(
    const std::string& file_path,
    const std::vector<Patent>& patents,
    const ExportOptions& options,
    std::function<bool(int, int)> progress_callback
) {
    std::ofstream file(file_path);
    
    if (!file.is_open()) {
        last_error_ = "无法创建文件: " + file_path;
        return false;
    }
    
    // 写入标题行
    if (options.include_headers) {
        file << "格科编码,申请号,发明名称,申请人,申请日期,专利类型,法律状态,"
             << "发明人,分类号,代理人,代理机构,公开号,公开日期,授权日期,"
             << "专利等级,处理人,备注\n";
    }
    
    // 写入数据行
    for (size_t i = 0; i < patents.size(); i++) {
        const Patent& p = patents[i];
        
        file << "\"" << p.geke_code << "\","
             << "\"" << p.application_no << "\","
             << "\"" << p.title << "\","
             << "\"" << p.applicant << "\","
             << "\"" << p.application_date << "\","
             << "\"" << p.patent_type << "\","
             << "\"" << p.legal_status << "\","
             << "\"" << p.inventor << "\","
             << "\"" << p.classification << "\","
             << "\"" << p.agent << "\","
             << "\"" << p.agency << "\","
             << "\"" << p.publish_no << "\","
             << "\"" << p.publish_date << "\","
             << "\"" << p.grant_date << "\","
             << p.patent_level << ","
             << "\"" << p.handler << "\","
             << "\"" << p.notes << "\"\n";
        
        if (progress_callback && (i % 100 == 0)) {
            progress_callback(i, patents.size());
        }
    }
    
    return true;
}

std::string ExcelIO::GetLastError() const {
    return last_error_;
}

ExcelIO& GetExcelIO() {
    static ExcelIO instance;
    return instance;
}

} // namespace patx