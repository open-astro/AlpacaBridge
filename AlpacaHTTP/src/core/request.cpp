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
        unsigned long content_length = 0;
        try {
            std::size_t consumed = 0;
            content_length = std::stoul(content_length_str, &consumed);
            if (consumed != content_length_str.size()) {
                return false;  // Trailing garbage in Content-Length
            }
        } catch (...) {
            return false;  // Invalid or out-of-range Content-Length
        }
        // Never trust the header for the allocation size: cap at the maximum
        // body size (see kMaxBodyBytes) so a hostile Content-Length cannot
        // force a multi-gigabyte resize.
        if (content_length > kMaxBodyBytes) {
            return false;
        }
        body_.resize(content_length);
        iss.read(&body_[0], static_cast<std::streamsize>(content_length));
        body_.resize(static_cast<std::size_t>(std::max<std::streamsize>(iss.gcount(), 0)));
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
    // Alpaca spec: "Parameter names are not case sensitive, so clients and
    // drivers should be prepared for parameter names to be supplied ... with
    // any casing." Always match query parameter names case-insensitively.
    auto it = query_params_lower_.find(to_lower_copy(key));
    if (it != query_params_lower_.end()) {
        return it->second;
    }
    return "";
}

bool Request::has_query_param(const std::string& key) const {
    return query_params_lower_.find(to_lower_copy(key)) != query_params_lower_.end();
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
