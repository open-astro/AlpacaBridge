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

#include <alpacahttp/response.h>
#include <alpacahttp/json_utils.h>
#include <sstream>
#include <unordered_map>

namespace alpacahttp {

void Response::set_status(std::uint16_t status_code, const std::string& reason_phrase) {
    status_code_ = status_code;
    if (reason_phrase.empty()) {
        reason_phrase_ = status_to_reason_phrase(status_code);
    } else {
        reason_phrase_ = reason_phrase;
    }
}

void Response::set_header(const std::string& key, const std::string& value) {
    headers_[key] = value;
}

void Response::set_content_type(const std::string& type) {
    set_header("Content-Type", type);
}

void Response::set_content_length(std::size_t length) {
    set_header("Content-Length", std::to_string(length));
}

void Response::set_body(const std::string& body) {
    body_ = body;
    set_content_length(body_.size());
}

void Response::set_body(const AlpacaResponse& alpaca_response) {
    auto json = to_json(alpaca_response);
    body_ = json.dump();
    set_content_type("application/json");
    set_content_length(body_.size());
}

std::string Response::to_string() const {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status_code_ << " " << reason_phrase_ << "\r\n";

    for (const auto& [key, value] : headers_) {
        oss << key << ": " << value << "\r\n";
    }

    oss << "\r\n";
    oss << body_;

    return oss.str();
}

const std::string& Response::get_header(const std::string& key) const {
    auto it = headers_.find(key);
    if (it != headers_.end()) {
        return it->second;
    }
    static const std::string empty_string;
    return empty_string;
}

std::string Response::status_to_reason_phrase(std::uint16_t code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        default: return "Unknown";
    }
}

} // namespace alpacahttp

