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

ApiResult UsptoClient::GetJson(const std::string& path_with_query, std::string& json_body) {
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
    HttpResponse resp = http_.Get(url, options);
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

bool UsptoClient::ParseApplicationBody(const std::string& json_body, UsptoCase& out_case,
                                      std::string& error) {
    try {
        auto parsed = json::parse(json_body);

        auto it = parsed.find("patentFileWrapperDataBag");
        if (it == parsed.end() || !it->is_array() || it->empty()) {
            error = "USPTO response has no patentFileWrapperDataBag";
            return false;
        }
        const json& wrapper = (*it)[0];
        const json* meta = nullptr;
        auto meta_it = wrapper.find("applicationMetaData");
        if (meta_it != wrapper.end()) meta = &(*meta_it);

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
    } catch (const json::exception& e) {
        error = std::string("failed to parse USPTO application JSON: ") + e.what();
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
