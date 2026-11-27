// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/http_client.h>
#include <curl/curl.h>

#include <cerrno>
#include <exception>
#include <mutex>
#include <string>
#include <utility>

// The libcurl implementation of util::HttpClient (decision 0023, issue #1753). The bounds came over
// unchanged from the WeeWX driver's own fetch (#1718): the body cap through CURLOPT_MAXFILESIZE_LARGE and
// the write callback, the http,https allow-list on the request and on every redirect, and the per-call
// timeout. Only GET became a choice of GET or a PUT with a form body, and get_with() a GET with its own Accept header
// and body cap (the remotealpaca camera's image, issue #2703).

namespace alpacacore::util {
namespace {

// The body of one call, its cap and its handle (for the declared length).
struct CappedBody {
    std::string* body;
    std::size_t cap;
    CURL* curl;
};

std::size_t write_capped(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    if (!userdata) {
        return 0;
    }
    auto* sink = static_cast<CappedBody*>(userdata);
    if (size * nmemb > sink->cap - sink->body->size()) {
        return 0;  // short count: curl aborts with CURLE_WRITE_ERROR
    }
    // No exception may leave this C callback: a full-frame body can outgrow the free memory (bad_alloc).
    try {
        if (sink->body->empty()) {
            // One allocation for a declared length (at most the cap, which MAXFILESIZE already held it to), so a
            // full-frame image body does not double its buffer as it grows.
            curl_off_t declared = -1;
            if (curl_easy_getinfo(sink->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &declared) == CURLE_OK &&
                declared > 0 && static_cast<std::size_t>(declared) <= sink->cap) {
                sink->body->reserve(static_cast<std::size_t>(declared));
            }
        }
        sink->body->append(ptr, size * nmemb);
    } catch (const std::exception&) {
        return 0;  // short count: curl aborts with CURLE_WRITE_ERROR
    }
    return size * nmemb;
}

void ensure_curl_global_init() {
    static std::once_flag init_flag;
    std::call_once(init_flag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

class CurlHttpClient final : public HttpClient {
public:
    explicit CurlHttpClient(std::string user_agent) : user_agent_(std::move(user_agent)) {}

    HttpResponse get(const std::string& url, std::chrono::milliseconds timeout) override {
        return perform(url, nullptr, timeout, HttpGetOptions{});
    }

    HttpResponse get_with(const std::string& url, std::chrono::milliseconds timeout,
                          const HttpGetOptions& options) override {
        return perform(url, nullptr, timeout, options);
    }

    HttpResponse put_form(const std::string& url, const std::string& form_body,
                          std::chrono::milliseconds timeout) override {
        return perform(url, &form_body, timeout, HttpGetOptions{});
    }

private:
    // One request on its own handle. `form_body` null is a GET, else a PUT with that form body.
    HttpResponse perform(const std::string& url, const std::string* form_body, std::chrono::milliseconds timeout,
                         const HttpGetOptions& options) {
        ensure_curl_global_init();
        CURL* curl = curl_easy_init();
        if (!curl) {
            throw AlpacaException("Unable to initialize HTTP client", AlpacaError::DriverException);
        }

        HttpResponse response;
        CappedBody sink{&response.body, options.max_body_bytes, curl};
        curl_slist* headers = nullptr;
        if (!options.accept.empty()) {
            const std::string accept = "Accept: " + options.accept;
            headers = curl_slist_append(nullptr, accept.c_str());
            if (!headers) {
                curl_easy_cleanup(curl);
                throw AlpacaException("Unable to build the HTTP request headers", AlpacaError::DriverException);
            }
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        }
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        // A GET follows redirects. A PUT does not: it is a command, and a redirect would send it again to
        // another URL; its 3xx comes back as the status instead.
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, form_body == nullptr ? 1L : 0L);
        // The URL may be user-set: only http and https, also after a redirect, never file://, ldap:// etc.
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout.count()));
        // Refuses a declared Content-Length over the cap before the body arrives.
        curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(options.max_body_bytes));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_capped);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent_.c_str());
        if (form_body != nullptr) {
            // POSTFIELDS sends the body with Content-Type application/x-www-form-urlencoded; the custom
            // request turns the POST into a PUT.
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(form_body->size()));
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, form_body->c_str());
        }

        const CURLcode result = curl_easy_perform(curl);
        if (result != CURLE_OK) {
            std::string message = "HTTP request failed: ";
            message += curl_easy_strerror(result);
            // CURLE_COULDNT_CONNECT also covers an unreachable host or network, which a Wi-Fi drop restores; only
            // the connect's own ECONNREFUSED says nothing listens there (decision 0009 point 2(a)).
            long os_errno = 0;
            curl_easy_getinfo(curl, CURLINFO_OS_ERRNO, &os_errno);
            curl_easy_cleanup(curl);
            curl_slist_free_all(headers);
            if (result == CURLE_COULDNT_CONNECT && os_errno == ECONNREFUSED) {
                throw HttpConnectRefused(message + " (connection refused)");
            }
            throw AlpacaException(message, AlpacaError::DriverException);
        }
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
        const char* content_type = nullptr;
        if (curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type) == CURLE_OK && content_type != nullptr) {
            response.content_type = content_type;
        }
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        return response;
    }

    std::string user_agent_;
};

}  // namespace

std::shared_ptr<HttpClient> make_curl_http_client(std::string user_agent) {
    return std::make_shared<CurlHttpClient>(std::move(user_agent));
}

}  // namespace alpacacore::util
