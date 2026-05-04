/**
 * patX PDF 解析模块接口
 * 
 * 使用 poppler-cpp 解析 OA 通知书
 */

#pragma once

#include <string>
#include <vector>

namespace patx {

// OA 通知书类型
enum class OaType {
    OFFICE_ACTION,      // 审查意见通知书
    RETIFICATION,       // 补正通知书  
    REJECTION,          // 驳回通知书
    GRANT,              // 授权通知书
    APPLICATION_ACCEPT, // 受理通知书
    ANNOUNCEMENT,       // 公告
    UNKNOWN
};

// 解析后的 OA 信息
struct ParsedOaInfo {
    std::string application_no;      // 申请号
    std::string issue_date;          // 发文日期
    std::string official_deadline;   // 绝限日期
    OaType oa_type;                  // 通知书类型
    int oa_number;                   // 第几次OA (1, 2, 3...)
    std::string geke_code;           // 格科编码
    std::string patent_title;        // 专利名称
    std::string applicant;           // 申请人
    std::string full_text;           // 全文内容
    bool valid;                      // 是否有效解析
    std::string error_message;       // 错误信息
};

// PDF 解析器类
class PdfParser {
public:
    PdfParser();
    ~PdfParser();
    
    // 解析 PDF 文件
    ParsedOaInfo Parse(const std::string& pdf_path);
    
    // 批量解析
    std::vector<ParsedOaInfo> ParseBatch(const std::vector<std::string>& pdf_paths);
    
    // 设置 DPI
    void SetDpi(int dpi);
    
private:
    int dpi_;
};

// 全局实例
PdfParser& GetPdfParser();

} // namespace patx
