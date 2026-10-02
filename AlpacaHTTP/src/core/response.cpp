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

#include <alpacahttp/json_utils.h>
#include <alpacahttp/response.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>

namespace alpacahttp {

namespace {

// Header field names are case-insensitive (RFC 7230 §3.2). Request already
// normalizes its keys to lowercase on parse; Response keeps the caller's
// spelling for the wire, so it must compare names case-insensitively instead
// -- otherwise a handler's "connection" and the server's "Connection" would
// coexist and both be emitted.
bool header_name_equals(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}

}  // namespace

void Response::set_status(std::uint16_t status_code, const std::string& reason_phrase) {
    status_code_ = status_code;
    if (reason_phrase.empty()) {
        reason_phrase_ = status_to_reason_phrase(status_code);
    } else {
        reason_phrase_ = reason_phrase;
    }
}

void Response::set_header(const std::string& key, const std::string& value) {
    // Replace any other spelling of this field first, so exactly one
    // instance of it is ever emitted, under the caller's casing.
    for (auto it = headers_.begin(); it != headers_.end();) {
        if (it->first != key && header_name_equals(it->first, key)) {
            it = headers_.erase(it);
        } else {
            ++it;
        }
    }
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
    // Issue #764: any string value (e.g. Wi-Fi SSIDs) can be arbitrary octets. The strict
    // default throws type_error.316 and fails the whole response; replace
    // invalid bytes with U+FFFD instead.
    body_ = json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    set_content_type("application/json");
    set_content_length(body_.size());
}

std::string Response::to_string() const {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status_code_ << " " << reason_phrase_ << "\r\n";

    bool has_connection = false;
    bool has_content_length = false;
    for (const auto& [key, value] : headers_) {
        if (header_name_equals(key, "Connection")) {
            has_connection = true;
        }
        if (header_name_equals(key, "Content-Length")) {
            has_content_length = true;
        }
        oss << key << ": " << value << "\r\n";
    }
    if (!has_connection) {
        oss << "Connection: close\r\n";
    }
    // A response with no Content-Length is framed by connection close, which
    // cannot work on a persistent connection: the client would keep reading,
    // waiting for a body that never ends, or take the next response's status
    // line for this one's body. set_body() sets the header for every response
    // the router builds today, so this only catches a handler that sets a
    // status and no body (204, or an early return) -- but that response would
    // be unframeable, and defaulting it here makes the invariant hold by
    // construction rather than by the caller remembering.
    if (!has_content_length) {
        oss << "Content-Length: " << body_.size() << "\r\n";
    }

    oss << "\r\n";
    oss << body_;

    return oss.str();
}

const std::string& Response::get_header(const std::string& key) const {
    for (const auto& [name, value] : headers_) {
        if (header_name_equals(name, key)) {
            return value;
        }
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
