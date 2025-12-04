// AlpacaHTTP
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the Server Side Public License, version 1 (SSPL v1).
// https://github.com/open-astro/AlpacaHTTP/blob/main/LICENSE
//
// If you use this library to provide a network-accessible service, you must comply
// with the SSPL v1 requirements.

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <cstdint>

namespace alpacahttp {

enum class HttpMethod {
    GET,
    PUT,
    UNKNOWN
};

class Request {
public:
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
    std::unordered_map<std::string, std::string> headers_;
    std::string body_;
    std::string_view http_version_;

    void parse_query_string();
    HttpMethod parse_method(std::string_view method_str);
};

} // namespace alpacahttp

