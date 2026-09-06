// USPTO Open Data Portal (Patent File Wrapper) API client.
//
// Endpoints (base URL configurable, default https://api.uspto.gov):
//   GET  /api/v1/patent/applications/{applicationNumberText}
//   GET  /api/v1/patent/applications/{applicationNumberText}/documents
//   GET  /api/v1/patent/applications/{applicationNumberText}/continuity
//   POST /api/v1/patent/applications/search        (OpenSearch-style q, pagination)
//   GET  /api/v1/download/applications/{app}/{docId}.pdf   (downloadUrl from API)
//
// Auth: X-API-Key header (MyUSPTO account; the June 2026 ODP change requires
// a registered key). The key is resolved from settings or the environment -
// never compiled in, never committed, never logged in full.
#pragma once

#include "patx/http_client.hpp"
#include "patx/uspto_models.hpp"

#include <string>
#include <vector>

namespace patx {

struct ApiResult {
    bool ok = false;
    std::string error;
    std::string status_line;    // e.g. "HTTP 401 Unauthorized"
    long http_status = 0;
};

class UsptoClient {
public:
    explicit UsptoClient(const UsptoConfig& config);

    // Cheap connectivity + auth check against the status-codes endpoint.
    ApiResult TestConnection();

    // Fetches bibliographic data for one application. app_no is normalized
    // (US 17/248024 -> 17248024). case.app/practice fields are filled; the
    // application_number field keeps the normalized number.
    ApiResult FetchApplication(const std::string& application_number, UsptoCase& out_case);

    // Official search endpoint (POST, OpenSearch q syntax per ODP swagger).
    // Used to resolve a known publication/patent number to its application
    // number when the user does not know the latter.
    ApiResult SearchApplications(const std::string& q, std::vector<UsptoCase>& out_results,
                                 int offset = 0, int limit = 25);

    // Fetches the full document list (documentBag) for a case.
    ApiResult FetchDocuments(const std::string& application_number,
                             std::vector<UsptoDocument>& out_documents);

    // Fetches parent/child continuity JSON (stored raw on the case).
    ApiResult FetchContinuity(const std::string& application_number, std::string& out_json);

    // Downloads a document file (PDF/DOCX/XML archive). Returns the response;
    // dest_path is created. sha256 is filled when the download succeeded.
    ApiResult DownloadDocument(const std::string& download_url, const std::string& dest_path,
                               std::string& sha256, long long* size_bytes = nullptr);

    HttpClient& http() { return http_; }

    // Updates the key used for subsequent requests (HttpClient has an
    // internal cancel token and is not copyable, hence the setter).
    void SetApiKey(const std::string& api_key) { config_.api_key = api_key; }
    const UsptoConfig& config() const { return config_; }

    // "17248024", "17/248024", "US 17248024" -> "17248024"
    static std::string NormalizeApplicationNumber(const std::string& input);

    // "US 2021/0210819 A1" -> "US20210210819A1"; "11,646,472" -> "11646472".
    static std::string NormalizePublicationNumber(const std::string& input);
    static std::string NormalizePatentNumber(const std::string& input);

    // Builds the OpenSearch q strings to try for a user-entered identifier
    // that may be an application, publication or patent number, in priority
    // order. Pure function (unit tested offline).
    static std::vector<std::string> BuildSearchQueries(const std::string& input);

    // Picks the hit whose application/publication/patent number exactly
    // matches the input. Returns the index, -1 for no match, -2 when the
    // match is ambiguous (caller must not auto-pick).
    static int PickSearchHit(const std::string& input, const std::vector<UsptoCase>& results);

    // Pure JSON parsers, exposed for fixture-based unit tests (no network).
    static bool ParseApplicationBody(const std::string& json_body, UsptoCase& out_case,
                                     std::string& error);
    static bool ParseSearchBody(const std::string& json_body, std::vector<UsptoCase>& out_results,
                                std::string& error);
    static bool ParseDocumentsBody(const std::string& json_body,
                                   std::vector<UsptoDocument>& out_documents,
                                   std::string& error);

private:
    ApiResult RequestJson(const std::string& method, const std::string& path_with_query,
                          const std::string& post_body, std::string& json_body);
    ApiResult GetJson(const std::string& path_with_query, /*out*/ std::string& json_body);

    UsptoConfig config_;
    HttpClient http_;
};

} // namespace patx
