/**
 * patX PDF 解析模块
 * 
 * 使用 poppler-cpp 解析 OA 通知书
 */

#include <poppler/cpp/poppler-document.h>
#include <poppler/cpp/poppler-page.h>
#include <poppler/cpp/poppler-page-renderer.h>
#include <poppler/cpp/poppler-embedded-file.h>
#include <string>
#include <vector>
#include <regex>
#include <chrono>
#include <cstring>
#include <algorithm>

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
    std::string geke_code;           // 我司编码
    std::string patent_title;        // 专利名称
    std::string applicant;           // 申请人
    std::string full_text;           // 全文内容
    bool valid;                      // 是否有效解析
    std::string error_message;       // 错误信息
};

// 中文日期解析
class DateParser {
public:
    // 解析中文日期格式: "YYYY年MM月DD日" 
    static std::string ParseChineseDate(const std::string& text) {
        std::regex pattern("(\d{4})\s*年\s*(\d{1,2})\s*月\s*(\d{1,2})\s*日");
        std::smatch match;
        if (std::regex_search(text, match, pattern)) {
            int year = std::stoi(match[1].str());
            int month = std::stoi(match[2].str());
            int day = std::stoi(match[3].str());
            char buf[16];
            snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
            return std::string(buf);
        }
        return "";
    }
    
    // 解析标准日期格式: "YYYY-MM-DD" 或 "YYYY.MM.DD"
    static std::string ParseStandardDate(const std::string& text) {
        std::regex pattern("(\d{4})[-./](\d{1,2})[-./](\d{1,2})");
        std::smatch match;
        if (std::regex_search(text, match, pattern)) {
            int year = std::stoi(match[1].str());
            int month = std::stoi(match[2].str());
            int day = std::stoi(match[3].str());
            char buf[16];
            snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
            return std::string(buf);
        }
        return "";
    }
    
    // 计算绝限日期 (发文日期 + 指定月数)
    static std::string CalculateDeadline(const std::string& issue_date, int months) {
        if (issue_date.empty()) return "";
        
        int year, month, day;
        sscanf(issue_date.c_str(), "%d-%d-%d", &year, &month, &day);
        
        month += months;
        while (month > 12) {
            month -= 12;
            year++;
        }
        
        // 确保日期有效
        int max_day = GetMaxDaysInMonth(year, month);
        if (day > max_day) day = max_day;
        
        char buf[16];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
        return std::string(buf);
    }
    
private:
    static int GetMaxDaysInMonth(int year, int month) {
        static const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
            return 29;
        }
        return days[month];
    }
};

// PDF 解析器类
class PdfParser {
public:
    PdfParser() : dpi_(150) {}
    ~PdfParser() = default;
    
    // 解析 PDF 文件
    ParsedOaInfo Parse(const std::string& pdf_path) {
        ParsedOaInfo result;
        result.valid = false;
        result.oa_type = OaType::UNKNOWN;
        
        // 加载 PDF 文档
        std::unique_ptr<poppler::document> doc(poppler::document::load_from_file(pdf_path));
        if (!doc) {
            result.error_message = "无法加载 PDF 文件: " + pdf_path;
            return result;
        }
        
        if (doc->pages() == 0) {
            result.error_message = "PDF 文件无页面";
            return result;
        }
        
        // 提取所有页面文本
        std::string full_text;
        for (int i = 0; i < doc->pages(); ++i) {
            std::unique_ptr<poppler::page> page(doc->create_page(i));
            if (page) {
                // Use to_utf8() for proper Chinese character handling
                auto ustr = page->text();
                for (size_t j = 0; j < ustr.length(); ++j) {
                    auto ch = ustr[j];
                    if (ch < 128) {
                        full_text += static_cast<char>(ch);
                    } else {
                        // UTF-8 encoding for Unicode characters
                        if (ch < 0x800) {
                            full_text += static_cast<char>(0xC0 | (ch >> 6));
                            full_text += static_cast<char>(0x80 | (ch & 0x3F));
                        } else {
                            full_text += static_cast<char>(0xE0 | (ch >> 12));
                            full_text += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                            full_text += static_cast<char>(0x80 | (ch & 0x3F));
                        }
                    }
                }
                full_text += "\n";
            }
        }
        result.full_text = full_text;
        
        // 解析文本信息
        if (!ParseOaContent(full_text, result)) {
            return result;
        }
        
        result.valid = true;
        return result;
    }
    
    // 批量解析
    std::vector<ParsedOaInfo> ParseBatch(const std::vector<std::string>& pdf_paths) {
        std::vector<ParsedOaInfo> results;
        results.reserve(pdf_paths.size());
        
        for (const auto& path : pdf_paths) {
            results.push_back(Parse(path));
        }
        
        return results;
    }
    
    // 设置 DPI
    void SetDpi(int dpi) { dpi_ = dpi; }
    
private:
    int dpi_;
    
    // 解析 OA 内容
    bool ParseOaContent(const std::string& text, ParsedOaInfo& info) {
        // 判断通知书类型
        info.oa_type = DetectOaType(text);
        info.oa_number = 0;

        // 提取OA次数 (第几次审查意见)
        if (info.oa_type == OaType::OFFICE_ACTION) {
            info.oa_number = ExtractOaNumber(text);
        }

        // 提取申请号
        info.application_no = ExtractApplicationNo(text);
        if (info.application_no.empty()) {
            info.error_message = "未找到申请号";
            return false;
        }

        // 提取发文日期
        info.issue_date = ExtractIssueDate(text);
        if (info.issue_date.empty()) {
            info.error_message = "未找到发文日期";
            return false;
        }

        // 计算绝限日期
        info.official_deadline = CalculateDeadline(text, info.issue_date, info.oa_type);

        // 提取其他信息
        info.patent_title = ExtractTitle(text);
        info.applicant = ExtractApplicant(text);

        return true;
    }

    // 中文数字映射
    int ChineseNumToInt(const std::string& chinese) {
        static const std::map<std::string, int> num_map = {
            {"一", 1}, {"二", 2}, {"三", 3}, {"四", 4}, {"五", 5},
            {"六", 6}, {"七", 7}, {"八", 8}, {"九", 9}, {"十", 10}
        };
        auto it = num_map.find(chinese);
        return it != num_map.end() ? it->second : 0;
    }

    // 提取 OA 次数 (第几次审查意见)
    int ExtractOaNumber(const std::string& text) {
        // Pattern: 第X次审查意见
        std::regex chinese_pattern("第\s*(一|二|三|四|五|六|七|八|九|十)\s*次\s*审\s*查\s*意\s*见");
        std::smatch match;
        if (std::regex_search(text, match, chinese_pattern)) {
            return ChineseNumToInt(match[1].str());
        }

        // Pattern: 第N次审查意见 (数字)
        std::regex digit_pattern("第\s*(\d+)\s*次\s*审\s*查\s*意\s*见");
        if (std::regex_search(text, match, digit_pattern)) {
            return std::stoi(match[1].str());
        }

        // 默认第一次
        return 1;
    }
    
    // 检测 OA 类型
    OaType DetectOaType(const std::string& text) {
        if (text.find("审查意见通知书") != std::string::npos) {
            return OaType::OFFICE_ACTION;
        }
        if (text.find("补正通知书") != std::string::npos) {
            return OaType::RETIFICATION;
        }
        if (text.find("驳回通知书") != std::string::npos) {
            return OaType::REJECTION;
        }
        if (text.find("授权通知书") != std::string::npos || 
            text.find("授予专利权通知书") != std::string::npos) {
            return OaType::GRANT;
        }
        if (text.find("专利申请受理通知书") != std::string::npos) {
            return OaType::APPLICATION_ACCEPT;
        }
        return OaType::UNKNOWN;
    }
    
    // 提取申请号
    std::string ExtractApplicationNo(const std::string& text) {
        // 申请号格式: CNXXXXXXXX.X 或 XXXXXXXXXXXX
        // 中文申请号: 申请号: XXXXXXXXXXXX
        std::vector<std::regex> patterns = {
            std::regex("申请号[：:]\s*(\d{12,13}[A-Z]?)"),
            std::regex("申请号[：:]\s*CN(\d{12,13}[A-Z]?)\.?"),
            std::regex("(CN\d{12,13}[A-Z]?)\.?"),
            std::regex("(\d{13}[A-Z]?)"),
        };
        
        for (const auto& pattern : patterns) {
            std::smatch match;
            if (std::regex_search(text, match, pattern)) {
                std::string app_no = match[1].str();
                // 清理格式
                app_no.erase(std::remove_if(app_no.begin(), app_no.end(), 
                    [](char c) { return c == ' ' || c == ':'; }), app_no.end());
                return app_no;
            }
        }
        
        return "";
    }
    
    // 提取发文日期
    std::string ExtractIssueDate(const std::string& text) {
        // 发文日期格式: 发文日 YYYY年MM月DD日
        std::vector<std::regex> patterns = {
            std::regex("发文日[期]?[：:]\s*(\d{4}\s*年\s*\d{1,2}\s*月\s*\d{1,2}\s*日)"),
            std::regex("发文日期[：:]\s*(\d{4}[-./]\d{1,2}[-./]\d{1,2})"),
            std::regex("(\d{4}\s*年\s*\d{1,2}\s*月\s*\d{1,2}\s*日)"),
        };
        
        for (const auto& pattern : patterns) {
            std::smatch match;
            if (std::regex_search(text, match, pattern)) {
                std::string date_str = match[1].str();
                // 先尝试中文格式
                std::string result = DateParser::ParseChineseDate(date_str);
                if (!result.empty()) return result;
                // 尝试标准格式
                result = DateParser::ParseStandardDate(date_str);
                if (!result.empty()) return result;
            }
        }
        
        return "";
    }
    
    // 计算绝限日期
    std::string CalculateDeadline(const std::string& text, 
                                   const std::string& issue_date, 
                                   OaType oa_type) {
        int months = 0;
        
        // 根据通知书类型确定回复期限
        switch (oa_type) {
            case OaType::OFFICE_ACTION:
                months = 4;  // 审查意见通知书一般4个月
                // 检查是否有一通、二通等，期限可能不同
                if (text.find("第一次") != std::string::npos) {
                    months = 4;
                } else if (text.find("第二次") != std::string::npos) {
                    months = 2;
                }
                break;
            case OaType::RETIFICATION:
                months = 2;  // 补正通知书一般2个月
                break;
            case OaType::REJECTION:
                months = 3;  // 驳回通知书一般3个月
                break;
            case OaType::GRANT:
                months = 2;  // 授权通知书一般2个月缴纳费用
                break;
            default:
                months = 2;
        }
        
        // 尝试从文本中提取明确的绝限日期
        std::regex deadline_pattern("答复期限[：:]\s*(\d{4}\s*年\s*\d{1,2}\s*月\s*\d{1,2}\s*日)");
        std::smatch match;
        if (std::regex_search(text, match, deadline_pattern)) {
            std::string deadline = DateParser::ParseChineseDate(match[1].str());
            if (!deadline.empty()) return deadline;
        }
        
        return DateParser::CalculateDeadline(issue_date, months);
    }
    
    // 提取专利名称
    std::string ExtractTitle(const std::string& text) {
        std::regex pattern("发明名称[：:]\s*(.+?)(?:\n|申请人|$)");
        std::smatch match;
        if (std::regex_search(text, match, pattern)) {
            std::string title = match[1].str();
            // 清理空白字符
            title.erase(0, title.find_first_not_of(" \t\n\r"));
            title.erase(title.find_last_not_of(" \t\n\r") + 1);
            return title;
        }
        return "";
    }
    
    // 提取申请人
    std::string ExtractApplicant(const std::string& text) {
        std::regex pattern("申请人[：:]\s*(.+?)(?:\n|发明人|$)");
        std::smatch match;
        if (std::regex_search(text, match, pattern)) {
            std::string applicant = match[1].str();
            applicant.erase(0, applicant.find_first_not_of(" \t\n\r"));
            applicant.erase(applicant.find_last_not_of(" \t\n\r") + 1);
            return applicant;
        }
        return "";
    }
};

// 全局实例
PdfParser& GetPdfParser() {
    static PdfParser instance;
    return instance;
}

} // namespace patx
