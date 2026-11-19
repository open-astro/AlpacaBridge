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

#include <mutex>
#include <string>
#include <utility>

// The libcurl implementation of util::HttpClient (decision 0023, issue #1753). The bounds came over
// unchanged from the WeeWX driver's own fetch (#1718): the body cap through CURLOPT_MAXFILESIZE_LARGE and
// the write callback, the http,https allow-list on the request and on every redirect, and the per-call
// timeout. Only GET became a choice of GET or a PUT with a form body.

namespace alpacacore::util {
namespace {

std::size_t write_capped(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    if (!userdata) {
        return 0;
    }
    auto* buffer = static_cast<std::string*>(userdata);
    if (size * nmemb > kHttpMaxResponseBytes - buffer->size()) {
        return 0;  // short count: curl aborts with CURLE_WRITE_ERROR
    }
    buffer->append(ptr, size * nmemb);
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
        return perform(url, nullptr, timeout);
    }

    HttpResponse put_form(const std::string& url, const std::string& form_body,
                          std::chrono::milliseconds timeout) override {
        return perform(url, &form_body, timeout);
    }

private:
    // One request on its own handle. `form_body` null is a GET, else a PUT with that form body.
    HttpResponse perform(const std::string& url, const std::string* form_body, std::chrono::milliseconds timeout) {
        ensure_curl_global_init();
        CURL* curl = curl_easy_init();
        if (!curl) {
            throw AlpacaException("Unable to initialize HTTP client", AlpacaError::DriverException);
        }

        HttpResponse response;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        // A GET follows redirects. A PUT does not: it is a command, and a redirect would send it again to
        // another URL; its 3xx comes back as the status instead.
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, form_body == nullptr ? 1L : 0L);
        // The URL may be user-set: only http and https, also after a redirect, never file://, ldap:// etc.
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout.count()));
        // Refuses a declared Content-Length over the cap before the body arrives.
        curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(kHttpMaxResponseBytes));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_capped);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
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
            curl_easy_cleanup(curl);
            throw AlpacaException(message, AlpacaError::DriverException);
        }
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
        curl_easy_cleanup(curl);
        return response;
    }

    std::string user_agent_;
};

}  // namespace

std::shared_ptr<HttpClient> make_curl_http_client(std::string user_agent) {
    return std::make_shared<CurlHttpClient>(std::move(user_agent));
}

}  // namespace alpacacore::util
