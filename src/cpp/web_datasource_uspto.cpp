// USPTO ODP 原生客户端实现。见 web_datasource_uspto.hpp。
#include "web_datasource_uspto.hpp"

#include "web_datasource.hpp"
#include "web_dossier.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <set>
#include <tuple>

namespace webdossier {
namespace uspto_odp {

namespace {

std::string ToUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

bool Contains(const std::string& hay, const char* needle) {
    return ToUpper(hay).find(needle) != std::string::npos;
}

// 与 web_datasource.cpp 一致的 event_key 输入规范化（去空白）
std::string NormalizeTitleForEventKey(const std::string& title) {
    std::string out;
    for (char c : title) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        out += c;
    }
    return out;
}

} // namespace

UsEventClass ClassifyUsEvent(const std::string& event_code,
                             const std::string& description) {
    UsEventClass result;
    std::string code = event_code;
    // 事件码有 "CTNF." / "_ACL." 之类变体尾巴
    code.erase(std::remove_if(code.begin(), code.end(),
                               [](unsigned char c) {
                                   return c == '.' || c == '_' || c == '/' ||
                                          c == '=' || c == ' ';
                               }),
               code.end());
    const std::string bare = ToUpper(code);
    bool mail = bare.rfind("M", 0) == 0 && bare.size() > 1;

    auto official = [&](const char* type, const char* title) {
        result.is_official = true;
        result.is_remindable = true;
        result.document_type = type;
        result.title = title;
        result.is_mail_variant = mail;
        return result;
    };
    std::string up = ToUpper(description);
    bool mentions_response = up.rfind("RESPONSE", 0) == 0 ||
                             up.find("FILED") != std::string::npos;

    // 第一层：事件码精确匹配（官方文书码，申请人事件不会携带）
    if (bare == "CTNF" || bare == "MCTNF")
        return official("OFFICE_ACTION_NTH", "Non-Final Office Action");
    if (bare == "CTFR" || bare == "MCTFR")
        return official("OFFICE_ACTION_NTH", "Final Office Action");
    if (bare == "CTRS" || bare == "MCTRS" || bare == "CTC")
        return official("OFFICE_ACTION_NTH", "Restriction Requirement");
    if (bare == "CTAV" || bare == "MCTAV" || bare == "ADVA")
        return official("OFFICE_ACTION_NTH", "Advisory Action");
    if (bare == "NOAL" || bare == "NOAM" || bare == "MN" || bare == "MCN")
        return official("GRANT_NOTICE", "Notice of Allowance");
    if (bare == "NOAB")
        return official("OTHER_OFFICIAL", "Notice of Abandonment");
    // RCEX/RCE 本身是申请人请求（ Bare "RCE" 不能子串匹配——
    // "Disposal for a RCE" 是官方流程事件）
    if (bare == "RCEX" || bare == "RCE")
        return result;   // is_official=false → 申请人方向

    // 第二层：申请人事件（描述匹配；Mail* 事件绝不属于申请人）
    static const char* applicant_marks[] = {
        "RESPONSE AFTER", "RESPONSE TO", "AMENDMENT", "REQUEST FOR CONTINUED",
        "INFORMATION DISCLOSURE", "PETITION ENTERED", "APPEAL", "BRIEF",
        "PREVIOUSLY FILED"};
    if (!mail) {
        for (const char* m : applicant_marks) {
            if (up.find(m) != std::string::npos) {
                result.is_official = false;   // 申请人方向
                return result;
            }
        }
    }

    // 第三层：描述匹配（须排除“答复/提交了某文书”的申请人描述，
    // 如 "Response to Election / Restriction Filed"）
    if (!mentions_response) {
        if (up.find("NON-FINAL REJECTION") != std::string::npos)
            return official("OFFICE_ACTION_NTH", "Non-Final Office Action");
        if (up.find("FINAL REJECTION") != std::string::npos)
            return official("OFFICE_ACTION_NTH", "Final Office Action");
        if (up.find("RESTRICTION") != std::string::npos ||
            up.find("ELECTION REQUIREMENT") != std::string::npos)
            return official("OFFICE_ACTION_NTH", "Restriction Requirement");
        if (up.find("ADVISORY ACTION") != std::string::npos)
            return official("OFFICE_ACTION_NTH", "Advisory Action");
        if (up.find("NOTICE OF ALLOWANCE") != std::string::npos &&
            up.find("VERIFICATION") == std::string::npos)
            return official("GRANT_NOTICE", "Notice of Allowance");
        if (up.find("NOTICE OF ABANDONMENT") != std::string::npos)
            return official("OTHER_OFFICIAL", "Notice of Abandonment");
    }

    // 其余（含 Mail* 流程事件）为官方中性：不建提醒、也不算申请人活动
    result.is_official = true;
    result.is_remindable = false;
    return result;
}

std::string NormalizeUsAppNumber(const std::string& raw) {
    std::string digits;
    for (char c : raw) {
        if (c >= '0' && c <= '9') digits += c;
    }
    return digits.size() == 8 ? digits : "";
}

ParseResult ParseApplicationJson(const std::string& body,
                                 const std::string& application_number) {
    ParseResult result;
    if (body.find("Missing Authentication Token") != std::string::npos ||
        body.find("\"message\":\"API key") != std::string::npos) {
        result.code = "AUTH_ERROR";
        result.message = "USPTO ODP 密钥无效或缺失";
        return result;
    }
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(body);
    } catch (const std::exception&) {
        result.code = "PAGE_STRUCTURE_CHANGED";
        result.message = "USPTO ODP 响应不是 JSON";
        return result;
    }
    if (root.value("count", 0) == 0 || !root.contains("patentFileWrapperDataBag") ||
        root["patentFileWrapperDataBag"].empty()) {
        result.code = "CASE_NOT_FOUND";
        result.message = "USPTO ODP 未收录该申请号";
        return result;
    }
    const auto& bag = root["patentFileWrapperDataBag"][0];
    if (bag.contains("applicationMetaData")) {
        result.status_description =
            bag["applicationMetaData"].value("applicationStatusDescriptionText", "");
    }

    // Mail* 与非 Mail* 成对出现：同一事件保留 Mail 变体日期（期限起算）
    struct Candidate {
        std::string type;
        std::string title;
        std::string date;
        std::string code;
        bool mail;
    };
    std::vector<Candidate> officials;
    if (bag.contains("eventDataBag") && bag["eventDataBag"].is_array()) {
        for (const auto& e : bag["eventDataBag"]) {
            std::string code = e.value("eventCode", "");
            std::string desc = e.value("eventDescriptionText", "");
            std::string date = e.value("eventDate", "");
            if (date.size() >= 10) date = date.substr(0, 10);
            if (date.empty()) continue;
            UsEventClass cls = ClassifyUsEvent(code, desc);
            if (cls.is_official && cls.is_remindable) {
                officials.push_back({cls.document_type, cls.title, date,
                                     ToUpper(code), cls.is_mail_variant});
            } else if (!cls.is_official) {
                if (result.latest_applicant_activity.empty() ||
                    date > result.latest_applicant_activity) {
                    result.latest_applicant_activity = date;
                }
            }
        }
    }
    // 同类型同日附近去重：同类型里 Mail 变体优先，日期不同则各留一条
    std::vector<Candidate> kept;
    for (const auto& c : officials) {
        bool dup = false;
        for (const auto& k : kept) {
            if (k.type == c.type && k.title == c.title && k.date == c.date) {
                dup = true;
                break;
            }
            // 同一文书的 Mail 与 record 变体通常差 0-2 天：视为同一事件
            if (k.type == c.type && k.title == c.title && c.mail != k.mail) {
                auto dayno = [](const std::string& d) {
                    // 真实日序数（含闰年累计），跨月也准确
                    int y = std::stoi(d.substr(0, 4));
                    int m = std::stoi(d.substr(5, 2));
                    int dd = std::stoi(d.substr(8, 2));
                    static const int cum[] = {0, 31, 59, 90, 120, 151, 181,
                                              212, 243, 273, 304, 334};
                    long long days = y * 365LL + y / 4 - y / 100 + y / 400 +
                                     cum[m - 1] + dd;
                    if (m > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
                        days += 1;
                    return days;
                };
                    long long gap = dayno(c.date) - dayno(k.date);
                    if (gap >= -7 && gap <= 7) {   // record→mail 滞后数日
                        dup = true;
                        break;
                    }
            }
        }
        if (!dup) kept.push_back(c);
    }

    std::sort(kept.begin(), kept.end(), [](const Candidate& a, const Candidate& b) {
        return std::tie(a.date, a.title) < std::tie(b.date, b.title);
    });
    for (const auto& c : kept) {
        RemoteDocument rd;
        rd.document_type = c.type;
        rd.document_title = c.title;
        rd.raw_title = c.title;
        rd.official_date = c.date;
        rd.direction = "official";
        rd.confidence = "HIGH";
        rd.oa_ordinal = 0;
        rd.remote_document_id = "US-EVT-" + c.code + "-" + c.date;
        rd.document_code = c.code;
        rd.document_version = "ORIGINAL";
        rd.source = "uspto_odp";
        rd.source_trace = "uspto_odp";
        rd.event_key = webdossier::uspto::ExportSha256Hex(
            "US|" + application_number + "|" + c.type + "|0|" + c.date + "|" +
            NormalizeTitleForEventKey(c.title));
        rd.fingerprint = webdossier::uspto::ExportSha256Hex(
            "US|" + application_number + "|" + NormalizeTitleForEventKey(c.title) +
            "|" + c.date + "|" + rd.remote_document_id);
        result.documents.push_back(std::move(rd));
    }
    result.code = "OK";
    return result;
}

// ---- libcurl 抓取 ----
namespace {
int CurlFetchUspto(const std::string& url, const std::string& api_key,
                   std::string& body);
}

NativeUsptoOdpClient::NativeUsptoOdpClient(std::string api_key, FetchJson fetch)
    : api_key_(std::move(api_key)) {
    if (fetch) fetch_ = std::move(fetch);
    else fetch_ = [](const std::string& url, const std::string& key,
                     std::string& out) { return CurlFetchUspto(url, key, out); };
}

ParseResult NativeUsptoOdpClient::FetchCase(const std::string& application_number) {
    std::string app8 = NormalizeUsAppNumber(application_number);
    if (app8.empty()) {
        ParseResult r;
        r.code = "RESOLVE_FAILED";
        r.message = "US 申请号需为 8 位数字（如 17/469,033）";
        return r;
    }
    const std::string url =
        "https://api.uspto.gov/api/v1/patent/applications/" + app8;
    std::string body;
    int http = fetch_(url, api_key_, body);
    if (http == 0) {
        ParseResult r;
        r.code = "NETWORK_ERROR";
        r.message = "USPTO ODP 连接失败";
        return r;
    }
    if (http == 401 || http == 403) {
        ParseResult r;
        r.code = "AUTH_ERROR";
        r.message = "USPTO ODP 密钥被拒绝（HTTP " + std::to_string(http) + "）";
        return r;
    }
    if (http == 404) {
        ParseResult r;
        r.code = "CASE_NOT_FOUND";
        r.message = "USPTO ODP 未收录该申请号";
        return r;
    }
    return ParseApplicationJson(body, app8);
}

} // namespace uspto_odp
} // namespace webdossier

#ifdef PATX_HAS_LIBCURL
#include <curl/curl.h>
namespace webdossier {
namespace uspto_odp {
namespace {
int CurlFetchUspto(const std::string& url, const std::string& api_key,
                   std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) return 0;
    body.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    // 认证头只在此处拼接；任何日志都不打印 key
    headers = curl_slist_append(headers, ("X-API-Key: " + api_key).c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](char* ptr, size_t size, size_t nmemb, void* ud) -> size_t {
            static_cast<std::string*>(ud)->append(ptr, size * nmemb);
            return size * nmemb;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK ? static_cast<int>(http_code) : 0;
}
} // namespace
} // namespace uspto_odp
} // namespace webdossier
#else
namespace webdossier {
namespace uspto_odp {
namespace {
int CurlFetchUspto(const std::string&, const std::string&, std::string&) {
    return 0;   // 未编译 libcurl：网络层不可用（测试用注入 fetcher）
}
} // namespace
} // namespace uspto_odp
} // namespace webdossier
#endif
