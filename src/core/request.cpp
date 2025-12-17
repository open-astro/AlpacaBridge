// AlpacaHTTP
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

#include <alpacahttp/request.h>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace alpacahttp {

bool Request::parse(std::string_view raw_request) {
    if (raw_request.empty()) {
        return false;
    }

    std::istringstream iss{std::string(raw_request)};
    std::string line;

    // Parse request line
    if (!std::getline(iss, line)) {
        return false;
    }

    // Remove trailing \r if present
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    std::istringstream request_line(line);
    std::string method_str, path_and_query, http_version_str;

    if (!(request_line >> method_str >> path_and_query)) {
        return false;
    }

    method_ = parse_method(method_str);

    // Extract HTTP version if present
    if (request_line >> http_version_str) {
        http_version_ = http_version_str;
    }

    // Split path and query string
    auto query_pos = path_and_query.find('?');
    if (query_pos != std::string::npos) {
        path_ = path_and_query.substr(0, query_pos);
        query_string_ = path_and_query.substr(query_pos + 1);
        parse_query_string();
    } else {
        path_ = path_and_query;
    }

    // Parse headers
    while (std::getline(iss, line) && !line.empty() && line != "\r") {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        auto colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 1);

            // Trim whitespace
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            // Convert key to lowercase for case-insensitive lookup
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            headers_[key] = value;
        }
    }

    // Parse body (if present)
    if (has_header("content-length")) {
        auto content_length_str = get_header("content-length");
        try {
            auto content_length = std::stoul(content_length_str);
            body_.resize(content_length);
            iss.read(&body_[0], static_cast<std::streamsize>(content_length));
        } catch (...) {
            // Invalid content-length
        }
    }

    return true;
}

HttpMethod Request::parse_method(std::string_view method_str) {
    if (method_str == "GET") {
        return HttpMethod::GET;
    } else if (method_str == "POST") {
        return HttpMethod::POST;
    } else if (method_str == "PUT") {
        return HttpMethod::PUT;
    }
    return HttpMethod::UNKNOWN;
}

void Request::parse_query_string() {
    if (query_string_.empty()) {
        return;
    }

    std::istringstream iss(query_string_);
    std::string pair;

    while (std::getline(iss, pair, '&')) {
        auto eq_pos = pair.find('=');
        if (eq_pos != std::string::npos) {
            std::string key = pair.substr(0, eq_pos);
            std::string value = pair.substr(eq_pos + 1);

            // URL decode (basic implementation)
            // TODO: Implement proper URL decoding
            query_params_[key] = value;
        } else {
            query_params_[pair] = "";
        }
    }
}

std::string Request::get_query_param(const std::string& key) const {
    auto it = query_params_.find(key);
    if (it != query_params_.end()) {
        return it->second;
    }
    return "";
}

bool Request::has_query_param(const std::string& key) const {
    return query_params_.find(key) != query_params_.end();
}

std::string Request::get_header(const std::string& key) const {
    std::string lower_key = key;
    std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
    auto it = headers_.find(lower_key);
    if (it != headers_.end()) {
        return it->second;
    }
    return "";
}

bool Request::has_header(const std::string& key) const {
    std::string lower_key = key;
    std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
    return headers_.find(lower_key) != headers_.end();
}

} // namespace alpacahttp

