// Thin libcurl wrapper used by the USPTO client. No hand-rolled sockets.
//
// Features: GET/POST (string and binary), custom headers, API key header,
// timeout, bounded retries with exponential backoff, Retry-After handling on
// 429, cooperative cancellation.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace patx {

struct HttpResponse {
    bool ok = false;             // transport succeeded AND 2xx status
    long status_code = 0;        // HTTP status (0 = transport failure)
    std::string body;            // text body (also used for binary via data())
    std::vector<uint8_t> data;   // binary body when requested via GetBinary
    std::string error;           // human readable error message
    std::string status_line;     // e.g. "HTTP 429 Too Many Requests"

    bool IsTransportError() const { return status_code == 0; }
};

struct HttpRequestOptions {
    std::vector<std::pair<std::string, std::string>> headers;
    long timeout_seconds = 30;
    int max_retries = 3;         // total attempts = max_retries (bounded!)
    std::string post_body;       // empty -> GET
    std::string content_type;
};

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    // Global libcurl init/cleanup (call once per process; cheap if repeated)
    static void GlobalInit();
    static void GlobalCleanup();

    // Cancel token shared between caller and in-flight request
    void Cancel() { cancelled_ = true; }
    void ResetCancel() { cancelled_ = false; }
    bool IsCancelled() const { return cancelled_.load(); }

    HttpResponse Get(const std::string& url, const HttpRequestOptions& options = {});
    HttpResponse Post(const std::string& url, const std::string& body,
                      const std::string& content_type,
                      const HttpRequestOptions& options = {});
    // Downloads to dest_path; returns response with ok=true and empty body.
    // Creates parent directories as needed.
    HttpResponse DownloadToFile(const std::string& url, const std::string& dest_path,
                                const HttpRequestOptions& options = {});

private:
    HttpResponse ExecuteRequest(const std::string& url, const HttpRequestOptions& options,
                                bool want_binary, const std::string* file_path);

    std::atomic<bool> cancelled_{false};
};

// Returns e.g. "http://example.com/a/../b" -> "http://example.com/b" without
// resolving DNS - only path normalization for display/logging.
std::string RedactUrlForLog(const std::string& url);

} // namespace patx
