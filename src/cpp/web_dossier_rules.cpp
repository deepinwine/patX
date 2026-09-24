// Pure, GUI-free helpers for the web dossier sync: Chinese OA type
// normalization and result-code mapping. Kept in their own translation unit
// so the offline unit tests link without wxWidgets.
#include "web_dossier.hpp"

namespace webdossier {

const char* ToString(ResultCode c) {
    switch (c) {
        case ResultCode::Ok: return "OK";
        case ResultCode::NoChange: return "NO_CHANGE";
        case ResultCode::NewOfficialEvent: return "NEW_OFFICIAL_EVENT";
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
        // NEW_OFFICE_ACTION is the legacy sidecar spelling of the same event
        {"NEW_OFFICIAL_EVENT", ResultCode::NewOfficialEvent},
        {"NEW_OFFICE_ACTION", ResultCode::NewOfficialEvent},
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

std::string OfficialEventTitleCn(const RemoteDocument& document) {
    if (document.document_type.rfind("OFFICE_ACTION_", 0) == 0) {
        return NormalizeOaTypeCn(document.document_title);
    }
    if (document.document_type == "REJECTION_DECISION") return "驳回决定";
    if (document.document_type == "GRANT_NOTICE") return "授权通知";
    if (document.document_type == "CORRECTION_NOTICE") return "补正通知";
    if (document.document_type == "OTHER_OFFICIAL") {
        return document.document_title.empty() ? "其他官方通知" : document.document_title;
    }
    return document.document_title.empty() ? document.raw_title : document.document_title;
}

EventMergeResult MergeOfficialEvent(Database& db, const Patent& patent,
                                    const RemoteDocument& document) {
    EventMergeResult result;
    result.canonical_title = OfficialEventTitleCn(document);

    if (document.confidence != "HIGH") {
        result.code = ResultCode::ManualReviewRequired;
        result.message = "置信度 " + document.confidence + "，待人工确认: " +
                         result.canonical_title + " @ " + document.official_date;
        return result;
    }
    if (document.official_date.empty()) {
        result.code = ResultCode::DateParseFailed;
        result.message = "官方发文缺少可解析的官文日: " + result.canonical_title;
        return result;
    }

    const bool is_oa_family = document.document_type.rfind("OFFICE_ACTION_", 0) == 0;
    // Suggested response deadline from the rule engine - OA family only,
    // same convention as the PDF import (marked calculated, never manual).
    // 发明分档：一通4个月，后续OA 2个月；实用新型2个月。
    auto suggest_deadline = [&]() -> std::string {
        if (!is_oa_family) return "";
        if (patent.patent_type == "utility") {
            return db.CalculateDeadline("CN", "oa_response_utility",
                                        document.official_date);
        }
        bool first = document.document_type == "OFFICE_ACTION_FIRST" ||
                     document.oa_ordinal == 1;
        return db.CalculateDeadline(
            "CN", first ? "oa_response_invention" : "oa_response_invention_further",
            document.official_date);
    };
    auto existing = db.GetOAsForPatentId(patent.id);
    const OARecord* same_type = nullptr;
    const OARecord* same_date = nullptr;
    for (const auto& e : existing) {
        std::string e_canonical = NormalizeOaTypeCn(e.oa_type);
        bool type_match = is_oa_family
            ? (e_canonical == result.canonical_title && IsOfficeActionTypeCn(e_canonical))
            : (e_canonical == result.canonical_title && !e_canonical.empty());
        if (type_match) same_type = &e;
        bool date_match = !e.issue_date.empty() && e.issue_date == document.official_date;
        if (is_oa_family) {
            // date takes precedence across the whole OA family
            if (date_match && (IsOfficeActionTypeCn(e_canonical) || e.oa_type.empty()))
                same_date = &e;
        } else if (type_match && date_match) {
            same_date = &e;
        }
    }

    if (same_date) {
        if (same_type && same_type->issue_date.empty()) {
            db.UpdateOASyncFields(same_type->id, document.official_date, "auto_filled_date");
            std::string deadline = suggest_deadline();
            if (db.FillOADeadlineIfEmpty(same_type->id, deadline)) {
                result.message = "已补入官文日 " + document.official_date +
                                 "，建议答复期限 " + deadline;
            } else {
                result.message = "已补入官文日 " + document.official_date;
            }
        } else {
            result.code = ResultCode::NoChange;
        }
        return result;
    }
    if (same_type) {
        if (same_type->issue_date.empty()) {
            db.UpdateOASyncFields(same_type->id, document.official_date, "auto_filled_date");
            std::string deadline = suggest_deadline();
            if (db.FillOADeadlineIfEmpty(same_type->id, deadline)) {
                result.message = "已补入官文日 " + document.official_date +
                                 "，建议答复期限 " + deadline;
            } else {
                result.message = "已补入官文日 " + document.official_date;
            }
            result.code = ResultCode::NoChange;
        } else {
            // Same event, different local date: never overwrite.
            db.UpdateOASyncFields(same_type->id, "", "date_conflict");
            result.code = ResultCode::DateConflict;
            result.date_conflict = true;
            result.message = "本地 " + result.canonical_title + " 官文日 " +
                             same_type->issue_date + "，官网 " + document.official_date +
                             "，待人工确认";
        }
        return result;
    }

    OARecord oa;
    oa.patent_id = patent.id;
    oa.geke_code = patent.geke_code;
    oa.patent_title = patent.title;
    oa.oa_type = result.canonical_title;
    oa.issue_date = document.official_date;
    oa.source = document.source.empty() ? "cnipa" : document.source;
    oa.remote_document_id = document.remote_document_id;
    oa.sync_flag = "web_new";
    std::string deadline = suggest_deadline();
    if (!deadline.empty()) {
        oa.official_deadline = deadline;
        oa.deadline_source = "calculated";
    }
    int id = db.InsertOA(oa, /*log_undo=*/true);
    if (id > 0) {
        result.code = ResultCode::NewOfficialEvent;
        result.oa_created_id = id;
        result.message = "发现新的官方发文: " + result.canonical_title + " @ " +
                         document.official_date;
        if (!deadline.empty()) {
            result.message += "，建议答复期限 " + deadline;
        }
    } else {
        result.code = ResultCode::TemporaryError;
        result.message = "官方发文写入失败: " + result.canonical_title;
    }
    return result;
}

} // namespace webdossier
