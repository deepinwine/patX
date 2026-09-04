// USPTO Open Data Portal (Patent File Wrapper) API client.
//
// Endpoints (base URL configurable, default https://api.uspto.gov):
//   GET /api/v1/patent/applications/{applicationNumberText}
//   GET /api/v1/patent/applications/{applicationNumberText}/documents
//   GET /api/v1/patent/applications/{applicationNumberText}/continuity
//   GET /api/v1/patent/applications/search?query=...
//   GET /api/v1/download/applications/{app}/{docId}.pdf      (downloadUrl from API)
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

    // Pure JSON parsers, exposed for fixture-based unit tests (no network).
    static bool ParseApplicationBody(const std::string& json_body, UsptoCase& out_case,
                                     std::string& error);
    static bool ParseDocumentsBody(const std::string& json_body,
                                   std::vector<UsptoDocument>& out_documents,
                                   std::string& error);

private:
    ApiResult GetJson(const std::string& path_with_query, /*out*/ std::string& json_body);

    UsptoConfig config_;
    HttpClient http_;
};

} // namespace patx
