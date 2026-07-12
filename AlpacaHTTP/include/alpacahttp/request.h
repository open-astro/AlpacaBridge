// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace alpacahttp {

enum class HttpMethod {
    GET,
    POST,
    PUT,
    DELETE_,
    UNKNOWN
};

class Request {
public:
    // Maximum accepted request body size (bounds Content-Length so a hostile
    // header can neither over-allocate nor exhaust the read loop). Shared by
    // Request::parse and the server's recv path.
    static constexpr std::size_t kMaxBodyBytes = std::size_t{10} * 1024 * 1024;

    Request() = default;
    ~Request() = default;

    // Parse HTTP request from raw data
    bool parse(std::string_view raw_request);

    // Getters
    HttpMethod method() const { return method_; }
    const std::string& path() const { return path_; }
    const std::string& query_string() const { return query_string_; }
    const std::unordered_map<std::string, std::string>& query_params() const { return query_params_; }
    const std::unordered_map<std::string, std::string>& headers() const { return headers_; }
    const std::string& body() const { return body_; }
    std::string_view http_version() const { return http_version_; }

    // Query parameter helpers
    std::string get_query_param(const std::string& key) const;
    bool has_query_param(const std::string& key) const;

    // Header helpers
    std::string get_header(const std::string& key) const;
    bool has_header(const std::string& key) const;

private:
    HttpMethod method_ = HttpMethod::UNKNOWN;
    std::string path_;
    std::string query_string_;
    std::unordered_map<std::string, std::string> query_params_;
    std::unordered_map<std::string, std::string> query_params_lower_;
    std::unordered_map<std::string, std::string> headers_;
    std::string body_;
    std::string_view http_version_;

    void parse_query_string();
    HttpMethod parse_method(std::string_view method_str);
};

} // namespace alpacahttp
