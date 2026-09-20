// Pure, GUI-free helpers for the web dossier sync: Chinese OA type
// normalization and result-code mapping. Kept in their own translation unit
// so the offline unit tests link without wxWidgets.
#include "web_dossier.hpp"

namespace webdossier {

const char* ToString(ResultCode c) {
    switch (c) {
        case ResultCode::Ok: return "OK";
        case ResultCode::NoChange: return "NO_CHANGE";
        case ResultCode::NewOfficeAction: return "NEW_OFFICE_ACTION";
        case ResultCode::AuthRequired: return "AUTH_REQUIRED";
        case ResultCode::SessionExpired: return "SESSION_EXPIRED";
        case ResultCode::CaseNotFound: return "CASE_NOT_FOUND";
        case ResultCode::PublicationNotAvailable: return "PUBLICATION_NOT_AVAILABLE";
        case ResultCode::AccessDenied: return "ACCESS_DENIED";
        case ResultCode::ResolveFailed: return "RESOLVE_FAILED";
        case ResultCode::PageStructureChanged: return "PAGE_STRUCTURE_CHANGED";
        case ResultCode::RateLimited: return "RATE_LIMITED";
        case ResultCode::TemporaryError: return "TEMPORARY_ERROR";
        case ResultCode::NetworkError: return "NETWORK_ERROR";
        case ResultCode::DateParseFailed: return "DATE_PARSE_FAILED";
        case ResultCode::DateConflict: return "DATE_CONFLICT";
        case ResultCode::UnsupportedJurisdiction: return "UNSUPPORTED_JURISDICTION";
        case ResultCode::ManualReviewRequired: return "MANUAL_REVIEW_REQUIRED";
        case ResultCode::SidecarError: return "SIDECAR_ERROR";
        case ResultCode::Cancelled: return "CANCELLED";
    }
    return "UNKNOWN";
}

ResultCode ResultCodeFromString(const std::string& name) {
    static const std::pair<const char*, ResultCode> map[] = {
        {"OK", ResultCode::Ok},
        {"NO_CHANGE", ResultCode::NoChange},
        {"NEW_OFFICE_ACTION", ResultCode::NewOfficeAction},
        {"AUTH_REQUIRED", ResultCode::AuthRequired},
        {"SESSION_EXPIRED", ResultCode::SessionExpired},
        {"CASE_NOT_FOUND", ResultCode::CaseNotFound},
        {"PUBLICATION_NOT_AVAILABLE", ResultCode::PublicationNotAvailable},
        {"ACCESS_DENIED", ResultCode::AccessDenied},
        {"RESOLVE_FAILED", ResultCode::ResolveFailed},
        {"PAGE_STRUCTURE_CHANGED", ResultCode::PageStructureChanged},
        {"RATE_LIMITED", ResultCode::RateLimited},
        {"TEMPORARY_ERROR", ResultCode::TemporaryError},
        {"NETWORK_ERROR", ResultCode::NetworkError},
        {"DATE_PARSE_FAILED", ResultCode::DateParseFailed},
        {"DATE_CONFLICT", ResultCode::DateConflict},
        {"UNSUPPORTED_JURISDICTION", ResultCode::UnsupportedJurisdiction},
        {"MANUAL_REVIEW_REQUIRED", ResultCode::ManualReviewRequired},
    };
    for (const auto& e : map) {
        if (name == e.first) return e.second;
    }
    return ResultCode::SidecarError;
}

namespace {

// 全角 digit -> ASCII
std::string NormalizeFullwidth(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0xEF && i + 2 < s.size()) {
            // U+FF10..FF19 fullwidth digits encode as EF BC 90..EF BC 99
            unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
            if (c1 == 0xBC && c2 >= 0x90 && c2 <= 0x99) {
                out += static_cast<char>('0' + (c2 - 0x90));
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

int ChineseNumberToOrdinal(const std::string& zh) {
    if (zh == "一" || zh == "1") return 1;
    if (zh == "二" || zh == "两" || zh == "2") return 2;
    if (zh == "三" || zh == "3") return 3;
    if (zh == "四" || zh == "4") return 4;
    if (zh == "五" || zh == "5") return 5;
    if (zh == "六" || zh == "6") return 6;
    if (zh == "七" || zh == "7") return 7;
    if (zh == "八" || zh == "8") return 8;
    if (zh == "九" || zh == "9") return 9;
    if (zh == "十") return 10;
    int v = atoi(zh.c_str());
    return v > 0 && v < 100 ? v : 0;
}

std::string OrdinalToChinese(int n) {
    static const char* zh[] = {"", "一", "二", "三", "四", "五", "六", "七", "八", "九", "十"};
    if (n >= 1 && n <= 10) return zh[n];
    return std::to_string(n);
}

} // namespace

bool IsOfficeActionTypeCn(const std::string& normalized) {
    return normalized.find("审查意见通知书") != std::string::npos;
}

int OaTypeOrdinalCn(const std::string& raw) {
    std::string s = NormalizeFullwidth(raw);
    size_t pos = s.find("第");
    while (pos != std::string::npos) {
        size_t after = pos + 3;   // "第" is 3 bytes in UTF-8
        if (after < s.size()) {
            if (static_cast<unsigned char>(s[after]) >= '0' &&
                static_cast<unsigned char>(s[after]) <= '9') {
                size_t end = after;
                while (end < s.size() && s[end] >= '0' && s[end] <= '9') end++;
                int v = atoi(s.substr(after, end - after).c_str());
                if (v > 0) return v;
            } else {
                std::string zh = s.substr(after, 3);
                int v = ChineseNumberToOrdinal(zh);
                if (v > 0) return v;
            }
        }
        pos = s.find("第", pos + 3);
    }
    if (s.find("一通") != std::string::npos) return 1;
    if (s.find("二通") != std::string::npos) return 2;
    if (s.find("三通") != std::string::npos) return 3;
    if (s.find("四通") != std::string::npos) return 4;
    return 0;
}

std::string NormalizeOaTypeCn(const std::string& raw) {
    std::string s = NormalizeFullwidth(raw);
    // collapse inner spaces so "第二次 审查意见通知书" still matches
    std::string compact;
    for (char c : s) {
        if (c != ' ' && c != '\t') compact += c;
    }
    if (compact.find("审查意见通知书") == std::string::npos) {
        // internal abbreviations: 一通/二通/三通/四通
        int abbr = 0;
        if (compact.find("一通") != std::string::npos) abbr = 1;
        else if (compact.find("二通") != std::string::npos) abbr = 2;
        else if (compact.find("三通") != std::string::npos) abbr = 3;
        else if (compact.find("四通") != std::string::npos) abbr = 4;
        if (abbr == 0) return raw;   // not OA family
        return "第" + OrdinalToChinese(abbr) + "次审查意见通知书";
    }

    int ordinal = OaTypeOrdinalCn(compact);
    if (ordinal >= 1) return "第" + OrdinalToChinese(ordinal) + "次审查意见通知书";
    return "审查意见通知书";
}

} // namespace webdossier
