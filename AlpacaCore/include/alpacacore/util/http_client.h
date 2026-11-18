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

#pragma once

// The one outbound HTTP seam an AlpacaCore vendor uses (decision 0023, issue #1753). A vendor reaches
// HTTP only through this interface; no vendor file names libcurl. The production implementation is
// make_curl_http_client(); tests inject the scripted FakeHttpClient (AlpacaCore/tests/fake_http_client.h)
// and the [stress] runs wrap it in LockedHttpClient (AlpacaCore/tests/locked_http_client.h), per
// decision 0015.
//
// Every call is bounded the way decision 0007 asks of an outbound client: the response body is capped
// at kHttpMaxResponseBytes, only http and https are allowed on the request and on every redirect, and
// each call has its own timeout.

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

namespace alpacacore::util {

// The response body cap. A larger declared Content-Length is refused before the body arrives, and a body
// that grows past it without one is cut off and refused.
inline constexpr std::size_t kHttpMaxResponseBytes = std::size_t{1024} * 1024;

struct HttpResponse {
    long status = 0;   // the final HTTP status, after any redirect
    std::string body;  // at most kHttpMaxResponseBytes
};

class HttpClient {
public:
    virtual ~HttpClient() = default;

    // GET `url`. Throws AlpacaException(DriverException) when no response arrives: a transport error, the
    // timeout, a body over the cap, or a scheme other than http and https on the request or a redirect.
    // An HTTP error status is not thrown; the caller reads HttpResponse::status.
    virtual HttpResponse get(const std::string& url, std::chrono::milliseconds timeout) = 0;

    // PUT `form_body` (application/x-www-form-urlencoded, already encoded) to `url`. Same bounds and
    // errors as get(), but a redirect is not followed: a PUT is a command, so the 3xx comes back as the
    // status rather than the command being sent again to another URL.
    virtual HttpResponse put_form(const std::string& url, const std::string& form_body,
                                  std::chrono::milliseconds timeout) = 0;

protected:
    HttpClient() = default;
    HttpClient(const HttpClient&) = default;
    HttpClient& operator=(const HttpClient&) = default;
};

// The libcurl client. Each call uses its own curl handle, so one client is safe to share between threads.
// `user_agent` is sent on every request.
std::shared_ptr<HttpClient> make_curl_http_client(std::string user_agent);

}  // namespace alpacacore::util
