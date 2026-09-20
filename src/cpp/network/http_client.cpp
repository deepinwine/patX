#include "patx/http_client.hpp"
#include "patx/log.hpp"

#include <curl/curl.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace patx {

namespace {

class CurlGlobal {
public:
    CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobal() { curl_global_cleanup(); }
};

void EnsureGlobalInit() {
    static CurlGlobal global;
}

size_t WriteToString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t WriteToVector(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::vector<uint8_t>*>(userdata);
    out->insert(out->end(), ptr, ptr + size * nmemb);
    return size * nmemb;
}

size_t WriteToFile(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* file = static_cast<FILE*>(userdata);
    return fwrite(ptr, 1, size * nmemb, file);
}

std::string StatusLineFor(long code) {
    switch (code) {
        case 200: return "HTTP 200 OK";
        case 201: return "HTTP 201 Created";
        case 204: return "HTTP 204 No Content";
        case 400: return "HTTP 400 Bad Request";
        case 401: return "HTTP 401 Unauthorized";
        case 403: return "HTTP 403 Forbidden";
        case 404: return "HTTP 404 Not Found";
        case 408: return "HTTP 408 Request Timeout";
        case 429: return "HTTP 429 Too Many Requests";
        case 500: return "HTTP 500 Internal Server Error";
        case 502: return "HTTP 502 Bad Gateway";
        case 503: return "HTTP 503 Service Unavailable";
        case 504: return "HTTP 504 Gateway Timeout";
        default: return "HTTP " + std::to_string(code);
    }
}

// Statuses worth retrying: timeouts and server-side/transient errors.
bool RetryableStatus(long code) {
    switch (code) {
        case 408: case 429: case 500: case 502: case 503: case 504:
            return true;
        default:
            return false;
    }
}

} // namespace

HttpClient::HttpClient() {
    EnsureGlobalInit();
}

HttpClient::~HttpClient() = default;

void HttpClient::GlobalInit() { EnsureGlobalInit(); }
void HttpClient::GlobalCleanup() { /* handled by static destructor */ }

HttpResponse HttpClient::Get(const std::string& url, const HttpRequestOptions& options) {
    return ExecuteRequest(url, options, false, nullptr);
}

HttpResponse HttpClient::Post(const std::string& url, const std::string& body,
                              const std::string& content_type,
                              const HttpRequestOptions& options) {
    HttpRequestOptions opts = options;
    opts.post_body = body;
    opts.content_type = content_type;
    return ExecuteRequest(url, opts, false, nullptr);
}

HttpResponse HttpClient::DownloadToFile(const std::string& url, const std::string& dest_path,
                                        const HttpRequestOptions& options) {
    // Create parent directories before streaming to the file
    std::error_code ec;
    auto parent = std::filesystem::path(dest_path).parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            HttpResponse resp;
            resp.error = "cannot create directory: " + ec.message();
            return resp;
        }
    }
    return ExecuteRequest(url, options, false, &dest_path);
}

HttpResponse HttpClient::ExecuteRequest(const std::string& url, const HttpRequestOptions& options,
                                        bool /*want_binary*/, const std::string* file_path) {
    HttpResponse resp;

    const int attempts = options.max_retries > 0 ? options.max_retries : 1;
    for (int attempt = 1; attempt <= attempts; attempt++) {
        if (cancelled_.load()) {
            resp.error = "cancelled";
            return resp;
        }

        std::string body_str;
        std::vector<uint8_t> body_bin;
        FILE* file = nullptr;
        bool transport_ok = true;

        CURL* curl = curl_easy_init();
        if (!curl) {
            resp.error = "failed to initialize libcurl";
            return resp;
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, options.timeout_seconds);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");       // gzip etc.
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "patX/0.4 (+https://github.com/deepinwine/patX)");
        // NTLM/gssnego off: we only talk to public JSON APIs
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

        struct curl_slist* headers = nullptr;
        for (const auto& [name, value] : options.headers) {
            std::string line = name + ": " + value;
            headers = curl_slist_append(headers, line.c_str());
        }
        if (!options.post_body.empty()) {
            if (options.content_type.empty() || !headers) {
                headers = curl_slist_append(headers,
                    ("Content-Type: " + (options.content_type.empty() ? "application/json" : options.content_type)).c_str());
            }
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, options.post_body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)options.post_body.size());
        }
        if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        if (file_path) {
            file = fopen(file_path->c_str(), "wb");
            if (!file) {
                resp.error = "cannot open destination file: " + *file_path;
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                return resp;
            }
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToFile);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
        } else {
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body_str);
        }

        CURLcode rc = curl_easy_perform(curl);

        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        long retry_after = 0;
        curl_off_t retry_after_off = 0;
        if (status == 429) {
            // Honor Retry-After when present (seconds form)
            struct curl_header* h = nullptr;
            if (curl_easy_header(curl, "Retry-After", 0, CURLH_HEADER, -1, &h) == CURLHE_OK && h) {
                retry_after = atol(h->value);
            }
        }
        double size_down = 0;
        curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &size_down);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (file) fclose(file);

        if (cancelled_.load()) {
            resp.error = "cancelled";
            if (file_path) {
                std::error_code ec;
                std::filesystem::remove(*file_path, ec);
            }
            return resp;
        }

        if (rc != CURLE_OK) {
            transport_ok = false;
            resp.error = std::string("network error: ") + curl_easy_strerror(rc);
            resp.status_code = 0;
        } else {
            resp.status_code = status;
            resp.status_line = StatusLineFor(status);
        }

        if (transport_ok && status >= 200 && status < 300) {
            resp.ok = true;
            resp.body = std::move(body_str);
            resp.data = std::move(body_bin);
            resp.error.clear();
            return resp;
        }

        // Failure handling
        if (file_path) {
            std::error_code ec;
            std::filesystem::remove(*file_path, ec);
        }

        bool should_retry = (attempt < attempts) &&
                            (transport_ok ? RetryableStatus(status) : true);
        if (should_retry) {
            // Exponential backoff: 1s, 2s, 4s... capped, plus Retry-After on 429
            long wait_ms = 1000L << (attempt - 1);
            if (wait_ms > 8000) wait_ms = 8000;
            if (status == 429 && retry_after > 0) {
                wait_ms = std::max(wait_ms, retry_after * 1000L);
                if (wait_ms > 60000) wait_ms = 60000;
            }
            PATX_LOG_WARN("HTTP attempt " + std::to_string(attempt) + " failed (" +
                          (transport_ok ? resp.status_line : resp.error) +
                          "), retrying in " + std::to_string(wait_ms) + " ms");
            for (long slept = 0; slept < wait_ms; slept += 100) {
                if (cancelled_.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        if (!transport_ok && resp.error.empty()) resp.error = "network error";
        if (transport_ok) {
            resp.error = resp.status_line + (body_str.empty() ? "" : ": " + body_str.substr(0, 300));
        }
        return resp;
    }

    return resp;
}

std::string RedactUrlForLog(const std::string& url) {
    // Strip query string (may contain api key) - keep scheme+path only
    size_t q = url.find('?');
    return q == std::string::npos ? url : url.substr(0, q);
}

} // namespace patx
