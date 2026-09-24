// USPTO Global Dossier 原生 C++ 客户端实现。见 web_datasource.hpp。
#include "web_datasource.hpp"

#include "web_dossier.hpp"

#include <nlohmann/json.hpp>

#ifdef PATX_HAS_LIBCURL
#include <curl/curl.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <mutex>
#include <regex>
#include <thread>

namespace webdossier {
namespace uspto {

namespace {

// ---- SHA-256（公有域写法，避免引入 OpenSSL 依赖）----
struct Sha256Ctx {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t data[64];
    size_t datalen;
};

constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t Rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

void Sha256Transform(Sha256Ctx& ctx, const uint8_t data[]) {
    uint32_t m[64], a, b, c, d, e, f, g, h, t1, t2;
    for (unsigned i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = (data[j] << 24) | (data[j + 1] << 16) | (data[j + 2] << 8) | data[j + 3];
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t s0 = Rotr(m[i - 15], 7) ^ Rotr(m[i - 15], 18) ^ (m[i - 15] >> 3);
        uint32_t s1 = Rotr(m[i - 2], 17) ^ Rotr(m[i - 2], 19) ^ (m[i - 2] >> 10);
        m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }
    a = ctx.state[0]; b = ctx.state[1]; c = ctx.state[2]; d = ctx.state[3];
    e = ctx.state[4]; f = ctx.state[5]; g = ctx.state[6]; h = ctx.state[7];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        t1 = h + s1 + ch + K[i] + m[i];
        uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx.state[0] += a; ctx.state[1] += b; ctx.state[2] += c; ctx.state[3] += d;
    ctx.state[4] += e; ctx.state[5] += f; ctx.state[6] += g; ctx.state[7] += h;
}

std::string Sha256Hex(const std::string& input) {
    Sha256Ctx ctx;
    ctx.bitlen = 0;
    ctx.datalen = 0;
    ctx.state[0] = 0x6a09e667; ctx.state[1] = 0xbb67ae85;
    ctx.state[2] = 0x3c6ef372; ctx.state[3] = 0xa54ff53a;
    ctx.state[4] = 0x510e527f; ctx.state[5] = 0x9b05688c;
    ctx.state[6] = 0x1f83d9ab; ctx.state[7] = 0x5be0cd19;

    const uint8_t* data = reinterpret_cast<const uint8_t*>(input.data());
    size_t len = input.size();
    for (size_t i = 0; i < len; ++i) {
        ctx.data[ctx.datalen++] = data[i];
        if (ctx.datalen == 64) {
            Sha256Transform(ctx, ctx.data);
            ctx.bitlen += 512;
            ctx.datalen = 0;
        }
    }
    size_t i = ctx.datalen;
    if (ctx.datalen < 56) {
        ctx.data[i++] = 0x80;
        while (i < 56) ctx.data[i++] = 0x00;
    } else {
        ctx.data[i++] = 0x80;
        while (i < 64) ctx.data[i++] = 0x00;
        Sha256Transform(ctx, ctx.data);
        memset(ctx.data, 0, 56);
    }
    ctx.bitlen += ctx.datalen * 8;
    ctx.data[63] = ctx.bitlen;
    ctx.data[62] = ctx.bitlen >> 8;
    ctx.data[61] = ctx.bitlen >> 16;
    ctx.data[60] = ctx.bitlen >> 24;
    ctx.data[59] = ctx.bitlen >> 32;
    ctx.data[58] = ctx.bitlen >> 40;
    ctx.data[57] = ctx.bitlen >> 48;
    ctx.data[56] = ctx.bitlen >> 56;
    Sha256Transform(ctx, ctx.data);

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int w = 0; w < 8; ++w)
        for (int b = 3; b >= 0; --b) {
            uint8_t byte = (ctx.state[w] >> (b * 8)) & 0xff;
            out += hex[byte >> 4];
            out += hex[byte & 0xf];
        }
    return out;
}

// 与 Python normalize_title 对齐：去所有空白（含全角空格），全角数字转半角
std::string NormalizeTitleForEventKey(const std::string& title) {
    std::string out;
    out.reserve(title.size());
    for (size_t i = 0; i < title.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(title[i]);
        if (c == 0xEF && i + 2 < title.size()) {
            // U+3000 全角空格 EF BC 80；U+FF10-19 全角数字 EF BC 90-99
            unsigned char c1 = static_cast<unsigned char>(title[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(title[i + 2]);
            if (c1 == 0xBC && c2 == 0x80) { i += 2; continue; }
            if (c1 == 0xBC && c2 >= 0x90 && c2 <= 0x99) {
                out += static_cast<char>('0' + (c2 - 0x90));
                i += 2;
                continue;
            }
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        out += title[i];
    }
    return out;
}

// MM/DD/YYYY -> YYYY-MM-DD
std::string NormalizeUsDate(const std::string& text) {
    static const std::regex re(R"(^(\d{1,2})/(\d{1,2})/(\d{4})$)");
    std::smatch m;
    if (std::regex_match(text, m, re)) {
        int mo = std::stoi(m[1]), d = std::stoi(m[2]), y = std::stoi(m[3]);
        char buf[16];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, mo, d);
        return buf;
    }
    return "";
}

std::string StripVersionSuffix(const std::string& title, bool& is_translated) {
    static const std::regex re(R"(\((ORIGINAL|TRANSLATED)\)\s*$)",
                               std::regex::icase);
    std::smatch m;
    is_translated = false;
    if (std::regex_search(title, m, re)) {
        std::string v = m[1].str();
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char ch) { return std::tolower(ch); });
        is_translated = (v == "translated");
        return title.substr(0, m.position());
    }
    return title;
}

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

} // namespace

EnClassification ClassifyOfficialDocumentEn(const std::string& raw_title,
                                             const std::string& document_code) {
    static const std::regex applicant_re(
        R"(response to|remarks|observations|amendment|amended claims|)"
        R"(substitution of|power of attorney|assignment|withdrawal|request for|)"
        R"(instructions|right-claiming|\bclaims\b|\bdescription\b|\bdrawings\b|\babstract\b)");
    static const std::regex final_re(R"(final rejection|decision to reject)");
    static const std::regex search_re(R"(\bsearch\b|search report)");
    static const std::regex grant_re(
        R"(grant patent right|notification to grant|registration formalities)");
    static const std::regex correction_re(R"(rectification|correction notice)");
    static const std::string oa_pattern = "(examination opinions|office action)";
    static const std::regex ws(R"(\s+)");
    static const char* ordinal_words[] = {"first", "second", "third", "fourth",
                                          "fifth", "sixth", "seventh", "eighth",
                                          "ninth", "tenth"};

    std::string compact;
    {
        std::string squeezed = std::regex_replace(raw_title, ws, " ");
        // 去首尾空白
        size_t b = squeezed.find_first_not_of(' ');
        size_t e = squeezed.find_last_not_of(' ');
        compact = (b == std::string::npos) ? "" : squeezed.substr(b, e - b + 1);
    }
    std::string low = ToLower(compact);

    auto make = [](const char* type, int ordinal, const char* conf,
                   bool official, bool remindable) {
        return EnClassification{type, ordinal, conf, official, remindable};
    };

    if (!low.empty() && std::regex_search(low, applicant_re))
        return make("RESPONSE_TO_OFFICE_ACTION", 0, "HIGH", false, false);
    if (std::regex_search(low, final_re))
        return make("REJECTION_DECISION", 0, "HIGH", true, true);
    for (int i = 0; i < 10; ++i) {
        std::regex pat("\\b" + std::string(ordinal_words[i]) + "\\b.*" + oa_pattern);
        if (std::regex_search(low, pat)) {
            if (i == 0) return make("OFFICE_ACTION_FIRST", 1, "HIGH", true, true);
            if (i == 1) return make("OFFICE_ACTION_SECOND", 2, "HIGH", true, true);
            return make("OFFICE_ACTION_NTH", i + 1, "HIGH", true, true);
        }
    }
    if (std::regex_search(low, search_re))
        return make("SEARCH_REPORT", 0, "HIGH", true, false);
    if (std::regex_search(low, grant_re))
        return make("GRANT_NOTICE", 0, "HIGH", true, true);
    if (std::regex_search(low, correction_re))
        return make("CORRECTION_NOTICE", 0, "HIGH", true, true);
    return make("UNKNOWN", 0, "LOW", true, false);
}

std::string ComputeEventKey(const std::string& jurisdiction,
                            const std::string& application_number,
                            const std::string& document_type,
                            int oa_ordinal,
                            const std::string& official_date,
                            const std::string& document_title) {
    std::string basis = jurisdiction + "|" + application_number + "|" +
                        document_type + "|" + std::to_string(oa_ordinal) + "|" +
                        official_date + "|" +
                        NormalizeTitleForEventKey(document_title);
    return Sha256Hex(basis);
}

ParseResult ParseFamilyJson(const std::string& body,
                            const std::string& application_number,
                            const std::string& publication_number) {
    ParseResult result;
    if (body.empty() || body.find_first_not_of(" \t\r\n") == std::string::npos) {
        result.code = "TEMPORARY_ERROR";
        result.message = "USPTO Global Dossier 返回空白响应";
        return result;
    }
    if (body.find("429") != std::string::npos &&
        body.find("RATE LIMITED") != std::string::npos) {
        result.code = "RATE_LIMITED";
        result.message = "USPTO Global Dossier 限流（429），稍后重试";
        return result;
    }
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(body);
    } catch (const std::exception&) {
        result.code = "PAGE_STRUCTURE_CHANGED";
        result.message = "USPTO Global Dossier 响应不是 JSON";
        return result;
    }
    if (!root.contains("list") || !root["list"].is_array()) {
        result.code = "PAGE_STRUCTURE_CHANGED";
        result.message = "USPTO family 响应缺少 list 数组";
        return result;
    }
    const nlohmann::json* cn_docs = nullptr;
    for (const auto& member : root["list"]) {
        if (member.value("countryCode", "") == "CN" && member.contains("docList")) {
            const auto& docs = member["docList"];
            if (docs.contains("docs") && docs["docs"].is_array())
                cn_docs = &docs["docs"];
            break;
        }
    }
    if (cn_docs == nullptr) {
        result.code = "CASE_NOT_FOUND";
        result.message = "USPTO Global Dossier 未收录该 CN 案件";
        return result;
    }

    for (const auto& doc : *cn_docs) {
        std::string desc = doc.value("docDesc", "");
        bool translated = false;
        std::string raw_title = StripVersionSuffix(desc, translated);
        if (raw_title.empty()) continue;
        if (translated) continue;                       // 机翻副本丢弃
        std::string code = doc.value("docCode", "");
        EnClassification cls = ClassifyOfficialDocumentEn(raw_title, code);
        if (!cls.is_official || !cls.is_remindable) continue;

        RemoteDocument rd;
        rd.document_type = cls.document_type;
        rd.oa_ordinal = cls.oa_ordinal;
        rd.raw_title = raw_title;
        rd.official_date = NormalizeUsDate(doc.value("legalDateStr", ""));
        rd.remote_document_id = doc.value("docId", "");
        rd.document_code = code;
        rd.document_version = "ORIGINAL";
        rd.confidence = cls.confidence;
        rd.direction = "official";
        // 规范中文标题：OA 家族按次数直接生成（与 Python event_title_cn 一致），
        // 其他类型走既有映射
        if (cls.document_type.rfind("OFFICE_ACTION_", 0) == 0) {
            if (rd.oa_ordinal >= 1) {
                static const char* zh[] = {"", "一", "二", "三", "四", "五",
                                           "六", "七", "八", "九", "十"};
                std::string num = rd.oa_ordinal <= 10
                    ? zh[rd.oa_ordinal] : std::to_string(rd.oa_ordinal);
                rd.document_title = "第" + num + "次审查意见通知书";
            } else {
                rd.document_title = "审查意见通知书";
            }
        } else {
            rd.document_title = OfficialEventTitleCn(rd);
        }
        rd.event_key = ComputeEventKey("CN", application_number,
                                       rd.document_type, rd.oa_ordinal,
                                       rd.official_date, rd.document_title);
        rd.source = "uspto_global_dossier";
        rd.source_trace = "uspto_global_dossier";
        result.documents.push_back(std::move(rd));
    }
    result.code = "OK";
    return result;
}

// ---- libcurl 默认抓取（浏览器 UA + 节流）----
namespace {

std::mutex g_pace_mutex;
std::chrono::steady_clock::time_point g_last_request{};

bool CurlFetch(const std::string& url, std::string& body) {
#ifdef PATX_HAS_LIBCURL
    // 节流：与 Python 版一致的最小 2 秒间隔
    {
        std::lock_guard<std::mutex> lock(g_pace_mutex);
        auto now = std::chrono::steady_clock::now();
        auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - g_last_request).count();
        if (since > 0 && since < 2000) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(2000 - since));
        }
        g_last_request = std::chrono::steady_clock::now();
    }
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    body.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers,
        "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
        "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0 Safari/537.36");
    headers = curl_slist_append(headers, "Accept: application/json, text/plain, */*");
    headers = curl_slist_append(headers, "Origin: https://globaldossier.uspto.gov");
    headers = curl_slist_append(headers, "Referer: https://globaldossier.uspto.gov/");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
            return size * nmemb;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) return false;
    if (http_code != 200) {
        body.clear();
        if (http_code == 429) body = "{\"ERROR\":\"429 - CLOUDFRONT RATE LIMITED\"}";
        return false;
    }
    return true;
#else
    (void)url;
    (void)body;
    return false;
#endif
}

} // namespace

NativeUsptoClient::NativeUsptoClient(FetchJson fetch) {
    if (fetch) fetch_ = std::move(fetch);
    else fetch_ = [](const std::string& url, std::string& body) {
        return CurlFetch(url, body);
    };
}

ParseResult NativeUsptoClient::FetchDocuments(
    const std::string& application_number_12_digits,
    const std::string& application_number,
    const std::string& publication_number) {
    const std::string url =
        "https://d1kazzu6rbodne.cloudfront.net/patent-family/svc/family/"
        "application/CN/" + application_number_12_digits;
    std::string body;
    if (!fetch_(url, body)) {
        ParseResult r;
        r.code = "NETWORK_ERROR";
        r.message = body.find("429") != std::string::npos
                        ? "USPTO Global Dossier 限流（429）"
                        : "USPTO Global Dossier 连接失败";
        return r;
    }
    return ParseFamilyJson(body, application_number, publication_number);
}

} // namespace uspto
} // namespace webdossier
