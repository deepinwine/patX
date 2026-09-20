#include "patx/uspto_client.hpp"
#include "patx/log.hpp"
#include "patx/document_classifier.hpp"

#include <nlohmann/json.hpp>
#include <cctype>
#include <cstdio>
#include <fstream>

// SHA-256 over the downloaded file content (tiny self-contained
// implementation to avoid an OpenSSL dependency just for hashing).
#include "patx/sha256.hpp"

namespace patx {

using nlohmann::json;

std::string UsptoConfig::ResolveApiKey(const std::string& configured) {
    if (!configured.empty()) return configured;
    const char* env = std::getenv("USPTO_API_KEY");
    if (env && *env) return env;
    return "";
}

UsptoClient::UsptoClient(const UsptoConfig& config) : config_(config) {}

std::string UsptoClient::NormalizeApplicationNumber(const std::string& input) {
    std::string digits;
    for (char c : input) {
        if (c >= '0' && c <= '9') digits += c;
        // series/serial separator ("17/248024" -> "17248024")
    }
    return digits;
}

std::string UsptoClient::NormalizePublicationNumber(const std::string& input) {
    // "US 2021/0210819 A1" -> "US20210210819A1" (matches ODP storage form)
    std::string out;
    for (char c : input) {
        if (c == ' ' || c == '/' || c == ',' || c == '-' || c == '\\') continue;
        out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string UsptoClient::NormalizePatentNumber(const std::string& input) {
    // "11,646,472" -> "11646472" (ODP stores digits without separators)
    std::string out;
    for (char c : input) {
        if (c >= '0' && c <= '9') out += c;
    }
    return out;
}

std::vector<std::string> UsptoClient::BuildSearchQueries(const std::string& input) {
    std::vector<std::string> queries;
    std::string pub = NormalizePublicationNumber(input);
    std::string patent = NormalizePatentNumber(input);
    std::string app = NormalizeApplicationNumber(input);

    // Publication numbers carry a country code/kind code (letters); a bare
    // digit string is ambiguous between application and patent numbers.
    bool has_letters = pub.find_first_not_of("0123456789") != std::string::npos;

    if (has_letters) {
        queries.push_back("applicationMetaData.earliestPublicationNumber:\"" + pub + "\"");
    } else {
        if (!patent.empty() && patent.size() >= 7) {
            queries.push_back("applicationMetaData.patentNumber:\"" + patent + "\"");
        }
        if (!app.empty() && app.size() >= 7) {
            queries.push_back("applicationNumberText:\"" + app + "\"");
        }
    }
    return queries;
}

int UsptoClient::PickSearchHit(const std::string& input, const std::vector<UsptoCase>& results) {
    std::string pub = NormalizePublicationNumber(input);
    std::string patent = NormalizePatentNumber(input);
    std::string app = NormalizeApplicationNumber(input);

    int first_hit = -1;
    int hits = 0;
    for (size_t i = 0; i < results.size(); i++) {
        const UsptoCase& c = results[i];
        bool match =
            (!pub.empty() && NormalizePublicationNumber(c.publication_number) == pub) ||
            (!patent.empty() && NormalizePatentNumber(c.patent_number) == patent) ||
            (!app.empty() && NormalizeApplicationNumber(c.application_number) == app);
        if (match) {
            hits++;
            if (first_hit < 0) first_hit = static_cast<int>(i);
        }
    }
    if (hits == 0) return -1;
    if (hits > 1) return -2;   // ambiguous: caller must ask the user
    return first_hit;
}

ApiResult UsptoClient::SearchApplications(const std::string& q, std::vector<UsptoCase>& out_results,
                                          int offset, int limit) {
    out_results.clear();
    if (q.empty()) {
        ApiResult r;
        r.error = "empty search query";
        return r;
    }

    // POST body per ODP swagger PatentSearchRequest: q + pagination
    json body = {
        {"q", q},
        {"pagination", {{"offset", offset}, {"limit", limit}}},
    };

    std::string response;
    ApiResult result = RequestJson("POST", "/api/v1/patent/applications/search",
                                   body.dump(), response);
    if (!result.ok) return result;

    // 404-style "no records" comes back as 200 with an empty bag
    std::string parse_error;
    if (!ParseSearchBody(response, out_results, parse_error)) {
        result.error = parse_error;
        return result;
    }
    result.ok = true;
    return result;
}

ApiResult UsptoClient::GetJson(const std::string& path_with_query, std::string& json_body) {
    return RequestJson("GET", path_with_query, "", json_body);
}

ApiResult UsptoClient::RequestJson(const std::string& method, const std::string& path_with_query,
                                   const std::string& post_body, std::string& json_body,
                                   const std::string& content_type) {
    ApiResult result;

    HttpRequestOptions options;
    options.timeout_seconds = config_.timeout_seconds;
    options.max_retries = config_.max_retries;
    if (config_.HasApiKey()) {
        options.headers.emplace_back("X-API-Key", config_.api_key);
        options.headers.emplace_back("Accept", "application/json");
    } else {
        result.error = "USPTO API key is not configured (set it in USPTO Settings or the "
                       "USPTO_API_KEY environment variable)";
        PATX_LOG_WARN("USPTO request skipped: no API key configured");
        return result;
    }

    std::string url = config_.api_base_url + path_with_query;
    HttpResponse resp = method == "POST"
        ? http_.Post(url, post_body, content_type, options)
        : http_.Get(url, options);
    result.http_status = resp.status_code;
    result.status_line = resp.status_line;

    if (!resp.ok) {
        // Map the common statuses to actionable user guidance
        switch (resp.status_code) {
            case 401:
                result.error = "USPTO rejected the API key (HTTP 401 Unauthorized). "
                               "Check the key in USPTO Settings.";
                break;
            case 403:
                result.error = "Access denied by USPTO (HTTP 403 Forbidden). The key may lack "
                               "permission for this API or the account needs verification.";
                break;
            case 404:
                result.error = "Application not found on the USPTO Open Data Portal "
                               "(HTTP 404). Check the application number.";
                break;
            case 429:
                result.error = "USPTO rate limit hit (HTTP 429 Too Many Requests). "
                               "Retried with backoff; try again later.";
                break;
            default:
                result.error = resp.error.empty() ? resp.status_line : resp.error;
        }
        PATX_LOG_WARN("USPTO request failed " + resp.status_line + " url=" +
                      RedactUrlForLog(url));
        return result;
    }

    json_body = std::move(resp.body);
    result.ok = true;
    return result;
}

ApiResult UsptoClient::TestConnection() {
    std::string body;
    ApiResult result = GetJson("/api/v1/patent/status-codes?limit=1", body);
    if (result.ok) {
        // Validate it really is JSON (auth errors sometimes return HTML)
        try {
            auto parsed = json::parse(body, nullptr, false);
            if (parsed.is_discarded()) {
                result.ok = false;
                result.error = "USPTO returned a non-JSON response; check the API base URL";
            }
        } catch (...) {
            result.ok = false;
            result.error = "USPTO returned an unparseable response";
        }
    }
    return result;
}

namespace {

// ODP timestamps arrive as "2023-10-03T09:18:25.000-0400" or plain dates.
std::string ToDateString(const std::string& value) {
    if (value.empty()) return "";
    // Keep only YYYY-MM-DD
    if (value.size() >= 10 && value[4] == '-') return value.substr(0, 10);
    return value;
}

} // namespace

namespace {

// Fills one UsptoCase from a single patentFileWrapperDataBag entry. Shared by
// the direct-application endpoint and the search endpoint (same shape).
void ParseWrapper(const json& wrapper, UsptoCase& out_case) {
    const json* meta = nullptr;
    auto meta_it = wrapper.find("applicationMetaData");
    if (meta_it != wrapper.end()) meta = &(*meta_it);

    auto wrapper_str = [&wrapper](const char* key) -> std::string {
        auto f = wrapper.find(key);
        if (f != wrapper.end() && f->is_string()) return f->get<std::string>();
        return "";
    };
    out_case.application_number = wrapper_str("applicationNumberText");

    if (meta) {
        auto get_str = [&meta](const char* key) -> std::string {
            auto f = meta->find(key);
            if (f != meta->end() && f->is_string()) return f->get<std::string>();
            return "";
        };
        out_case.application_status = get_str("applicationStatusDescriptionText");
        out_case.status_date = get_str("applicationStatusDate");
        out_case.application_type = get_str("applicationTypeCode");
        out_case.examiner_name = get_str("examinerNameText");
        out_case.filing_date = ToDateString(get_str("filingDate"));
        out_case.first_named_inventor = get_str("firstInventorName");
        out_case.applicant_name = get_str("firstApplicantName");
        out_case.grant_date = ToDateString(get_str("grantDate"));
        out_case.art_unit = get_str("groupArtUnitNumber");
        out_case.attorney_docket_number = get_str("docketNumber");
        out_case.title = get_str("inventionTitle");
        out_case.publication_number = get_str("earliestPublicationNumber");
        out_case.publication_date = ToDateString(get_str("earliestPublicationDate"));
        out_case.patent_number = get_str("patentNumber");

        // Technology center: first two digits of the art unit
        if (!out_case.art_unit.empty() && out_case.art_unit.size() >= 2) {
            out_case.technology_center = out_case.art_unit.substr(0, 2);
        }

        auto entity = meta->find("entityStatusData");
        if (entity != meta->end() && entity->is_object()) {
            auto cat = entity->find("businessEntityStatusCategory");
            if (cat != entity->end() && cat->is_string()) {
                out_case.entity_status = cat->get<std::string>();
            }
        }
        // Effective filing date doubles as the priority date
        out_case.priority_date = ToDateString(get_str("effectiveFilingDate"));
    }

    auto last_ing = wrapper.find("lastIngestionDateTime");
    if (last_ing != wrapper.end() && last_ing->is_string()) {
        out_case.last_uspto_modified_at = last_ing->get<std::string>();
    }
    auto assign_bag = wrapper.find("assignmentBag");
    if (assign_bag != wrapper.end() && assign_bag->is_array() && !assign_bag->empty()) {
        const json& a = assign_bag->back();
        auto name = a.find("assigneeName");
        if (name == a.end()) name = a.find("assignorOrAssigneeName");
        if (name != a.end() && name->is_string()) {
            out_case.assignee_name = name->get<std::string>();
        }
    }
}

} // namespace

bool UsptoClient::ParseApplicationBody(const std::string& json_body, UsptoCase& out_case,
                                      std::string& error) {
    try {
        auto parsed = json::parse(json_body);
        auto it = parsed.find("patentFileWrapperDataBag");
        if (it == parsed.end() || !it->is_array() || it->empty()) {
            error = "USPTO response has no patentFileWrapperDataBag";
            return false;
        }
        ParseWrapper((*it)[0], out_case);
    } catch (const json::exception& e) {
        error = std::string("failed to parse USPTO application JSON: ") + e.what();
        PATX_LOG_ERROR(error);
        return false;
    }
    return true;
}

bool UsptoClient::ParseSearchBody(const std::string& json_body,
                                  std::vector<UsptoCase>& out_results, std::string& error) {
    out_results.clear();
    try {
        auto parsed = json::parse(json_body);
        auto it = parsed.find("patentFileWrapperDataBag");
        if (it == parsed.end() || !it->is_array()) {
            error = "USPTO search response has no patentFileWrapperDataBag";
            return false;
        }
        for (const auto& wrapper : *it) {
            UsptoCase c;
            ParseWrapper(wrapper, c);
            if (!c.application_number.empty()) {
                out_results.push_back(std::move(c));
            }
        }
    } catch (const json::exception& e) {
        error = std::string("failed to parse USPTO search JSON: ") + e.what();
        PATX_LOG_ERROR(error);
        return false;
    }
    return true;
}

ApiResult UsptoClient::FetchApplication(const std::string& application_number,
                                        UsptoCase& out_case) {
    std::string app_no = NormalizeApplicationNumber(application_number);
    if (app_no.empty()) {
        ApiResult r;
        r.error = "empty application number";
        return r;
    }

    std::string body;
    ApiResult result = GetJson("/api/v1/patent/applications/" + app_no, body);
    if (!result.ok) return result;

    std::string parse_error;
    if (!ParseApplicationBody(body, out_case, parse_error)) {
        result.error = parse_error;
        result.http_status = 404;
        result.status_line = "HTTP 404 Not Found";
        return result;
    }
    out_case.application_number = app_no;
    result.ok = true;
    return result;
}

bool UsptoClient::ParseDocumentsBody(const std::string& json_body,
                                     std::vector<UsptoDocument>& out_documents,
                                     std::string& error) {
    out_documents.clear();
    try {
        auto parsed = json::parse(json_body);
        auto bag_it = parsed.find("documentBag");
        if (bag_it == parsed.end() || !bag_it->is_array()) {
            error = "USPTO response has no documentBag";
            return false;
        }

        for (const auto& doc : *bag_it) {
            UsptoDocument d;
            auto get_str = [&doc](const char* key) -> std::string {
                auto f = doc.find(key);
                if (f != doc.end() && f->is_string()) return f->get<std::string>();
                return "";
            };
            d.document_identifier = get_str("documentIdentifier");
            d.document_code = get_str("documentCode");
            d.document_description = get_str("documentCodeDescriptionText");
            d.filing_date = ToDateString(get_str("officialDate"));
            d.mail_date = d.filing_date;

            auto options_bag = doc.find("downloadOptionBag");
            if (options_bag != doc.end() && options_bag->is_array()) {
                for (const auto& opt : *options_bag) {
                    auto url_f = opt.find("downloadUrl");
                    auto mime_f = opt.find("mimeTypeIdentifier");
                    auto pages_f = opt.find("pageTotalQuantity");
                    if (url_f == opt.end() || !url_f->is_string()) continue;
                    std::string url = url_f->get<std::string>();
                    std::string mime = mime_f != opt.end() && mime_f->is_string()
                                           ? mime_f->get<std::string>() : "";
                    if (mime == "PDF" || url.substr(url.size() - 4) == ".pdf") {
                        d.file_download_uri = url;
                        d.mime_type = "PDF";
                    } else if (mime == "XML" || url.find("xmlarchive") != std::string::npos) {
                        d.xml_download_uri = url;
                    } else if (mime == "MS_WORD" || mime == "DOCX") {
                        if (d.file_download_uri.empty()) {
                            d.file_download_uri = url;
                            d.mime_type = "DOCX";
                        }
                    }
                    if (pages_f != opt.end() && pages_f->is_number()) {
                        d.page_count = std::max(d.page_count, pages_f->get<int>());
                    }
                }
            }

            // Classify once with the official code + description
            auto cls = DocumentClassifier::Classify(d.document_code, d.document_description);
            d.category = cls.category;
            d.party = cls.party;
            d.document_category = ToString(cls.category);
            d.parse_confidence = cls.confidence;
            d.raw_document_code = d.document_code;
            d.raw_description = d.document_description;

            if (d.document_identifier.empty()) continue;  // no stable key, skip
            out_documents.push_back(std::move(d));
        }
    } catch (const json::exception& e) {
        error = std::string("failed to parse USPTO documents JSON: ") + e.what();
        PATX_LOG_ERROR(error);
        return false;
    }
    return true;
}

ApiResult UsptoClient::FetchDocuments(const std::string& application_number,
                                      std::vector<UsptoDocument>& out_documents) {
    out_documents.clear();
    std::string app_no = NormalizeApplicationNumber(application_number);
    std::string body;
    ApiResult result = GetJson("/api/v1/patent/applications/" + app_no + "/documents", body);
    if (!result.ok) return result;

    std::string parse_error;
    if (!ParseDocumentsBody(body, out_documents, parse_error)) {
        result.error = parse_error;
        return result;
    }
    result.ok = true;
    return result;
}

ApiResult UsptoClient::FetchContinuity(const std::string& application_number,
                                       std::string& out_json) {
    std::string app_no = NormalizeApplicationNumber(application_number);
    std::string body;
    ApiResult result = GetJson("/api/v1/patent/applications/" + app_no + "/continuity", body);
    if (!result.ok) return result;
    out_json = body;   // stored raw; parsed on demand by the UI
    result.ok = true;
    return result;
}

namespace {

// Percent-encodes a value for an application/x-www-form-urlencoded body.
std::string FormEncode(const std::string& value) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

// DSAPI flag fields arrive as 0/1 numbers or true/false booleans.
int ReadFlag(const json& doc, const char* key) {
    auto f = doc.find(key);
    if (f == doc.end()) return -1;
    if (f->is_number()) return f->get<int>() != 0 ? 1 : 0;
    if (f->is_boolean()) return f->get<bool>() ? 1 : 0;
    return -1;
}

} // namespace

bool UsptoClient::ParseDsapiBody(const std::string& json_body,
                                 std::vector<OaOfficialRecord>& out_records,
                                 long* out_num_found, std::string& error) {
    out_records.clear();
    if (out_num_found) *out_num_found = 0;
    try {
        auto parsed = json::parse(json_body);
        auto resp_it = parsed.find("response");
        if (resp_it == parsed.end() || !resp_it->is_object()) {
            error = "DSAPI response has no response object";
            return false;
        }
        auto count = resp_it->find("numFound");
        if (count != resp_it->end() && count->is_number() && out_num_found) {
            *out_num_found = count->get<long>();
        }
        auto docs = resp_it->find("docs");
        if (docs == resp_it->end() || !docs->is_array()) {
            error = "DSAPI response has no docs array";
            return false;
        }

        for (const auto& doc : *docs) {
            if (!doc.is_object()) continue;
            OaOfficialRecord r;
            auto get_str = [&doc](const char* key) -> std::string {
                auto f = doc.find(key);
                if (f != doc.end() && f->is_string()) return f->get<std::string>();
                return "";
            };
            r.application_number = get_str("patentApplicationNumber");
            r.action_type = get_str("actionType");
            r.mailed_date = get_str("mailedDate");
            r.record_id = get_str("id");
            r.legal_section_code = get_str("legalSectionCode");
            r.group_art_unit = get_str("groupArtUnitNumber");
            r.has_rej_101 = ReadFlag(doc, "hasRej101");
            r.has_rej_102 = ReadFlag(doc, "hasRej102");
            r.has_rej_103 = ReadFlag(doc, "hasRej103");
            r.has_rej_112 = ReadFlag(doc, "hasRej112");
            r.has_rej_dp = ReadFlag(doc, "hasRejDP");
            auto alice = doc.find("aliceIndicator");
            if (alice != doc.end() && alice->is_boolean()) r.alice_indicator = alice->get<bool>();
            auto bilski = doc.find("bilskiIndicator");
            if (bilski != doc.end() && bilski->is_boolean()) r.bilski_indicator = bilski->get<bool>();
            out_records.push_back(std::move(r));
        }
    } catch (const json::exception& e) {
        error = std::string("failed to parse DSAPI JSON: ") + e.what();
        PATX_LOG_ERROR(error);
        return false;
    }
    return true;
}

ApiResult UsptoClient::SearchOaActions(const std::string& application_number,
                                       std::vector<OaOfficialRecord>& out_records) {
    std::string app_no = NormalizeApplicationNumber(application_number);
    std::string form = "criteria=" + FormEncode("patentApplicationNumber:" + app_no) +
                       "&start=0&rows=100";
    std::string body;
    ApiResult result = RequestJson("POST", "/api/v1/patent/oa/oa_actions/v1/records",
                                   form, body, "application/x-www-form-urlencoded");
    if (!result.ok) return result;

    std::string parse_error;
    if (!ParseDsapiBody(body, out_records, nullptr, parse_error)) {
        result.error = parse_error;
        return result;
    }
    result.ok = true;
    return result;
}

ApiResult UsptoClient::SearchOaRejections(const std::string& application_number,
                                          std::vector<OaOfficialRecord>& out_records) {
    std::string app_no = NormalizeApplicationNumber(application_number);
    std::string form = "criteria=" + FormEncode("patentApplicationNumber:" + app_no) +
                       "&start=0&rows=200";
    std::string body;
    ApiResult result = RequestJson("POST", "/api/v1/patent/oa/oa_rejections/v2/records",
                                   form, body, "application/x-www-form-urlencoded");
    if (!result.ok) return result;

    std::string parse_error;
    if (!ParseDsapiBody(body, out_records, nullptr, parse_error)) {
        result.error = parse_error;
        return result;
    }
    result.ok = true;
    return result;
}

ApiResult UsptoClient::DownloadDocument(const std::string& download_url,
                                        const std::string& dest_path,
                                        std::string& sha256, long long* size_bytes) {
    ApiResult result;
    if (!config_.HasApiKey()) {
        result.error = "USPTO API key is not configured";
        return result;
    }

    HttpRequestOptions options;
    options.timeout_seconds = std::max(config_.timeout_seconds, 120L);  // PDFs are big
    options.max_retries = config_.max_retries;
    options.headers.emplace_back("X-API-Key", config_.api_key);

    HttpResponse resp = http_.DownloadToFile(download_url, dest_path, options);
    result.http_status = resp.status_code;
    result.status_line = resp.status_line;
    if (!resp.ok) {
        result.error = resp.error.empty() ? resp.status_line : resp.error;
        PATX_LOG_WARN("USPTO download failed " + resp.status_line + " url=" +
                      RedactUrlForLog(download_url));
        return result;
    }

    // Hash the file we actually received
    std::ifstream f(dest_path, std::ios::binary);
    if (!f.is_open()) {
        result.error = "downloaded file is not readable: " + dest_path;
        return result;
    }
    SHA256 hash;
    std::string buf(65536, '\0');
    long long total = 0;
    while (f.good()) {
        f.read(buf.data(), buf.size());
        std::streamsize got = f.gcount();
        if (got > 0) {
            hash.update(reinterpret_cast<const uint8_t*>(buf.data()), static_cast<size_t>(got));
            total += got;
        }
    }
    sha256 = hash.final_hex();
    if (size_bytes) *size_bytes = total;
    result.ok = true;
    return result;
}

} // namespace patx
