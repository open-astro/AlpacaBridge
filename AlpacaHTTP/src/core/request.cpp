// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
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
#include <cstdlib>

namespace alpacahttp {

namespace {

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool is_client_transaction_id_key(const std::string& key) {
    return to_lower_copy(key) == "clienttransactionid";
}

bool is_client_id_key(const std::string& key) {
    return to_lower_copy(key) == "clientid";
}

bool is_case_insensitive_key(const std::string& key) {
    return is_client_transaction_id_key(key) || is_client_id_key(key);
}

} // namespace

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
            if (key == "accept") {
                auto existing = headers_.find(key);
                if (existing != headers_.end() && !existing->second.empty()) {
                    existing->second.append(", ");
                    existing->second.append(value);
                } else {
                    headers_[key] = value;
                }
            } else {
                headers_[key] = value;
            }
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
    } else if (method_str == "DELETE") {
        return HttpMethod::DELETE_;
    }
    return HttpMethod::UNKNOWN;
}

void Request::parse_query_string() {
    if (query_string_.empty()) {
        return;
    }
    query_params_.clear();
    query_params_lower_.clear();

    auto url_decode = [](const std::string& value) -> std::string {
        std::string result;
        result.reserve(value.size());
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '%' && i + 2 < value.size()) {
                std::string hex = value.substr(i + 1, 2);
                char decoded = static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
                result.push_back(decoded);
                i += 2;
            } else if (value[i] == '+') {
                result.push_back(' ');
            } else {
                result.push_back(value[i]);
            }
        }
        return result;
    };

    std::istringstream iss(query_string_);
    std::string pair;

    while (std::getline(iss, pair, '&')) {
        auto eq_pos = pair.find('=');
        if (eq_pos != std::string::npos) {
            std::string key = url_decode(pair.substr(0, eq_pos));
            std::string value = url_decode(pair.substr(eq_pos + 1));
            query_params_[key] = value;
            query_params_lower_[to_lower_copy(key)] = value;
        } else {
            std::string key = url_decode(pair);
            query_params_[key] = "";
            query_params_lower_[to_lower_copy(key)] = "";
        }
    }
}

std::string Request::get_query_param(const std::string& key) const {
    const bool strict = strict_casing_enabled();
    if (!strict) {
        auto it = query_params_lower_.find(to_lower_copy(key));
        if (it != query_params_lower_.end()) {
            return it->second;
        }
        return "";
    }
    if (auto it = query_params_.find(key); it != query_params_.end()) {
        return it->second;
    }
    if (is_case_insensitive_key(key)) {
        auto it = query_params_lower_.find(to_lower_copy(key));
        if (it != query_params_lower_.end()) {
            return it->second;
        }
    }
    return "";
}

bool Request::has_query_param(const std::string& key) const {
    const bool strict = strict_casing_enabled();
    if (!strict) {
        return query_params_lower_.find(to_lower_copy(key)) != query_params_lower_.end();
    }
    if (query_params_.find(key) != query_params_.end()) {
        return true;
    }
    if (is_case_insensitive_key(key)) {
        return query_params_lower_.find(to_lower_copy(key)) != query_params_lower_.end();
    }
    return false;
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

bool Request::strict_casing_enabled() const {
    std::string user_agent = get_header("user-agent");
    if (user_agent.empty()) {
        return false;
    }

    std::string lower_agent = to_lower_copy(user_agent);
    return lower_agent.find("conformuniversal") != std::string::npos ||
        lower_agent.find("conformu") != std::string::npos;
}

} // namespace alpacahttp
