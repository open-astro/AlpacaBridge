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
#include <alpacacore/vendor/remotealpaca/remote_alpaca_client.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace alpacacore::vendor::remotealpaca {

namespace {

// The wrapper's own JSON reader (decision 0013 section 4: no JSON type leaves the wrapper). It reads one
// document of at most util::kHttpMaxResponseBytes, nested at most kMaxDepth deep, and keeps a number's text so
// an integer is read without a round trip through double.
struct Json {
    enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    std::string text;  // a String's value, or a Number's literal
    std::vector<Json> items;
    // An Object's members as two parallel lists: std::vector allows an incomplete element type, but a
    // std::pair<std::string, Json> inside Json does not (clang rejects it).
    std::vector<std::string> member_names;
    std::vector<Json> member_values;

    const Json* find(std::string_view key) const {
        for (std::size_t i = 0; i < member_names.size(); ++i) {
            if (member_names[i] == key) return &member_values[i];
        }
        return nullptr;
    }
};

const char* kind_name(Json::Kind kind) {
    switch (kind) {
        case Json::Kind::Null:
            return "null";
        case Json::Kind::Bool:
            return "boolean";
        case Json::Kind::Number:
            return "number";
        case Json::Kind::String:
            return "string";
        case Json::Kind::Array:
            return "array";
        case Json::Kind::Object:
            return "object";
    }
    return "unknown";
}

class JsonReader {
public:
    explicit JsonReader(std::string_view text) : text_(text) {}

    // The document, or nullopt when it is not one JSON value with nothing after it.
    std::optional<Json> read() {
        Json value;
        if (!value_into(value, 0)) return std::nullopt;
        skip_space();
        if (pos_ != text_.size()) return std::nullopt;
        return value;
    }

private:
    static constexpr int kMaxDepth = 32;

    void skip_space() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    bool value_into(Json& out, int depth) {
        if (depth > kMaxDepth) return false;
        skip_space();
        if (pos_ >= text_.size()) return false;
        const char c = text_[pos_];
        if (c == '{') return object_into(out, depth);
        if (c == '[') return array_into(out, depth);
        if (c == '"') {
            out.kind = Json::Kind::String;
            return string_into(out.text);
        }
        if (literal("true")) {
            out.kind = Json::Kind::Bool;
            out.boolean = true;
            return true;
        }
        if (literal("false")) {
            out.kind = Json::Kind::Bool;
            return true;
        }
        if (literal("null")) {
            out.kind = Json::Kind::Null;
            return true;
        }
        return number_into(out);
    }

    bool object_into(Json& out, int depth) {
        out.kind = Json::Kind::Object;
        ++pos_;  // '{'
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            skip_space();
            std::string key;
            if (pos_ >= text_.size() || text_[pos_] != '"' || !string_into(key)) return false;
            skip_space();
            if (pos_ >= text_.size() || text_[pos_] != ':') return false;
            ++pos_;
            Json value;
            if (!value_into(value, depth + 1)) return false;
            out.member_names.push_back(std::move(key));
            out.member_values.push_back(std::move(value));
            skip_space();
            if (pos_ >= text_.size()) return false;
            if (text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            if (text_[pos_] != ',') return false;
            ++pos_;
        }
    }

    bool array_into(Json& out, int depth) {
        out.kind = Json::Kind::Array;
        ++pos_;  // '['
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            Json value;
            if (!value_into(value, depth + 1)) return false;
            out.items.push_back(std::move(value));
            skip_space();
            if (pos_ >= text_.size()) return false;
            if (text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            if (text_[pos_] != ',') return false;
            ++pos_;
        }
    }

    static void append_utf8(std::string& out, std::uint32_t code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    bool hex4(std::uint32_t& code) {
        if (pos_ + 4 > text_.size()) return false;
        code = 0;
        for (int k = 0; k < 4; ++k) {
            const char h = text_[pos_++];
            code <<= 4;
            if (h >= '0' && h <= '9') {
                code |= static_cast<std::uint32_t>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
                code |= static_cast<std::uint32_t>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
                code |= static_cast<std::uint32_t>(h - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    bool string_into(std::string& out) {
        ++pos_;  // '"'
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) return false;
            const char e = text_[pos_++];
            switch (e) {
                case '"':
                case '\\':
                case '/':
                    out += e;
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    std::uint32_t code = 0;
                    if (!hex4(code)) return false;
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        std::uint32_t low = 0;
                        if (!literal("\\u") || !hex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    } else if (code >= 0xDC00 && code <= 0xDFFF) {
                        return false;
                    }
                    append_utf8(out, code);
                    break;
                }
                default:
                    return false;
            }
        }
        return false;
    }

    // RFC 8259 number grammar: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
    bool number_into(Json& out) {
        const std::size_t start = pos_;
        auto digits = [this] {
            const std::size_t from = pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) ++pos_;
            return pos_ - from;
        };
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
        if (pos_ < text_.size() && text_[pos_] == '0') {
            ++pos_;
        } else if (digits() == 0) {
            return false;
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (digits() == 0) return false;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            if (digits() == 0) return false;
        }
        out.kind = Json::Kind::Number;
        out.text = std::string(text_.substr(start, pos_ - start));
        return true;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

// A Number literal as an int, or nullopt when it has a fraction or exponent or falls outside int.
std::optional<int> whole_int(const Json& value) {
    if (value.kind != Json::Kind::Number || value.text.find_first_of(".eE") != std::string::npos) return std::nullopt;
    long long parsed = 0;
    const auto* end = value.text.data() + value.text.size();
    const auto result = std::from_chars(value.text.data(), end, parsed);
    if (result.ec != std::errc() || result.ptr != end) return std::nullopt;
    if (parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max()) return std::nullopt;
    return static_cast<int>(parsed);
}

// application/x-www-form-urlencoded and query encoding: RFC 3986 unreserved characters as they are, every
// other byte as %XX.
std::string url_encode(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) != 0 || c == '-' || c == '.' || c == '_' || c == '~') {
            out += ch;
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
    }
    return out;
}

bool lower_letters(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 'a' && c <= 'z'; });
}

// Two colons at least: "10.0.0.5:11111" is a host with a port typed into it, not an IPv6 address.
bool is_ipv6_literal(std::string_view host) { return std::count(host.begin(), host.end(), ':') >= 2; }

// A host name (letters, digits, '-', '.'), an IPv4 address, or an IPv6 address (hex digits, ':', '.').
bool host_valid(std::string_view host) {
    if (host.empty() || host.size() > 253) return false;
    if (is_ipv6_literal(host)) {
        return std::all_of(host.begin(), host.end(),
                           [](unsigned char c) { return std::isxdigit(c) != 0 || c == ':' || c == '.'; });
    }
    return std::all_of(host.begin(), host.end(),
                       [](unsigned char c) { return std::isalnum(c) != 0 || c == '-' || c == '.'; });
}

std::string authority(const UpstreamDevice& upstream) {
    const std::string host = is_ipv6_literal(upstream.host) ? "[" + upstream.host + "]" : upstream.host;
    return host + ":" + std::to_string(upstream.port);
}

}  // namespace

struct RemoteAlpacaClient::Reply {
    Json root;
    std::string member;

    const Json& value(const std::string& prefix) const {
        const Json* found = root.find("Value");
        if (found == nullptr) throw RemoteTransportError(prefix + "the reply to " + member + " has no Value");
        return *found;
    }
};

RemoteAlpacaClient::RemoteAlpacaClient(std::shared_ptr<util::HttpClient> http, UpstreamDevice upstream,
                                       std::uint32_t client_id, std::chrono::milliseconds timeout)
    : http_(std::move(http)), upstream_(std::move(upstream)), client_id_(client_id), timeout_(timeout) {
    if (!http_) {
        throw AlpacaException("Remote Alpaca client needs an HTTP client", AlpacaError::InvalidValue);
    }
    if (!host_valid(upstream_.host)) {
        throw AlpacaException("Upstream host must be a host name or an IP address", AlpacaError::InvalidValue);
    }
    if (upstream_.port < 1 || upstream_.port > 65535) {
        throw AlpacaException("Upstream port must be from 1 to 65535", AlpacaError::InvalidValue);
    }
    if (!lower_letters(upstream_.device_type)) {
        throw AlpacaException("Upstream device type must be the lower-case Alpaca device type",
                              AlpacaError::InvalidValue);
    }
    if (upstream_.device_number < 0) {
        throw AlpacaException("Upstream device number must be 0 or more", AlpacaError::InvalidValue);
    }
    prefix_ = authority(upstream_) + ": ";
}

std::string RemoteAlpacaClient::member_url(std::string_view member) const {
    return "http://" + authority(upstream_) + "/api/v1/" + upstream_.device_type + "/" +
           std::to_string(upstream_.device_number) + "/" + std::string(member);
}

RemoteAlpacaClient::Reply RemoteAlpacaClient::call(Verb verb, std::string_view member, const RemoteParams& params) {
    if (!lower_letters(member)) {
        throw AlpacaException("Alpaca member name must be lower-case letters", AlpacaError::InvalidValue);
    }
    const std::string name(member);

    util::HttpResponse response;
    {
        std::lock_guard<std::mutex> lock(call_mutex_);
        const std::uint32_t transaction_id = next_transaction_id_;
        // 0 means "no id" to a server, so the count skips it when it wraps.
        next_transaction_id_ =
            next_transaction_id_ == std::numeric_limits<std::uint32_t>::max() ? 1 : next_transaction_id_ + 1;
        std::string encoded;
        for (const auto& [key, value] : params) {
            encoded += url_encode(key) + "=" + url_encode(value) + "&";
        }
        encoded += "ClientID=" + std::to_string(client_id_) + "&ClientTransactionID=" + std::to_string(transaction_id);
        try {
            response = verb == Verb::Get ? http_->get(member_url(member) + "?" + encoded, timeout_)
                                         : http_->put_form(member_url(member), encoded, timeout_);
        } catch (const util::HttpConnectRefused& e) {
            throw RemoteConnectRefused(prefix_ + "no reply to " + name + ": " + e.what());
        } catch (const std::exception& e) {
            throw RemoteTransportError(prefix_ + "no reply to " + name + ": " + e.what());
        }
    }

    if (response.status != 200) {
        throw RemoteTransportError(prefix_ + "HTTP status " + std::to_string(response.status) + " for " + name);
    }
    auto root = JsonReader(response.body).read();
    if (!root || root->kind != Json::Kind::Object) {
        throw RemoteTransportError(prefix_ + "the reply to " + name + " is not an Alpaca JSON object");
    }
    const Json* number = root->find("ErrorNumber");
    const auto error_number = number != nullptr ? whole_int(*number) : std::nullopt;
    if (!error_number) {
        throw RemoteTransportError(prefix_ + "the reply to " + name + " has no whole ErrorNumber");
    }
    if (*error_number != 0) {
        const Json* message = root->find("ErrorMessage");
        const std::string text =
            message != nullptr && message->kind == Json::Kind::String ? message->text : std::string();
        throw AlpacaException(prefix_ + text, *error_number);
    }
    return Reply{std::move(*root), name};
}

bool RemoteAlpacaClient::get_bool(std::string_view member, const RemoteParams& params) {
    const Reply reply = call(Verb::Get, member, params);
    const Json& value = reply.value(prefix_);
    if (value.kind != Json::Kind::Bool) {
        throw RemoteTransportError(prefix_ + "the reply to " + reply.member + " has a " + kind_name(value.kind) +
                                   " Value, not a boolean");
    }
    return value.boolean;
}

int RemoteAlpacaClient::get_int(std::string_view member, const RemoteParams& params) {
    const Reply reply = call(Verb::Get, member, params);
    const Json& value = reply.value(prefix_);
    const auto parsed = whole_int(value);
    if (!parsed) {
        throw RemoteTransportError(prefix_ + "the reply to " + reply.member + " has a " + kind_name(value.kind) +
                                   " Value that is not a whole number inside int");
    }
    return *parsed;
}

double RemoteAlpacaClient::get_double(std::string_view member, const RemoteParams& params) {
    const Reply reply = call(Verb::Get, member, params);
    const Json& value = reply.value(prefix_);
    double parsed = 0.0;
    if (value.kind == Json::Kind::Number) {
        const auto* end = value.text.data() + value.text.size();
        const auto result = std::from_chars(value.text.data(), end, parsed);
        if (result.ec == std::errc() && result.ptr == end && std::isfinite(parsed)) return parsed;
    }
    throw RemoteTransportError(prefix_ + "the reply to " + reply.member + " has a " + kind_name(value.kind) +
                               " Value, not a finite number");
}

std::string RemoteAlpacaClient::get_string(std::string_view member, const RemoteParams& params) {
    const Reply reply = call(Verb::Get, member, params);
    const Json& value = reply.value(prefix_);
    if (value.kind != Json::Kind::String) {
        throw RemoteTransportError(prefix_ + "the reply to " + reply.member + " has a " + kind_name(value.kind) +
                                   " Value, not a string");
    }
    return value.text;
}

std::vector<std::string> RemoteAlpacaClient::get_string_list(std::string_view member, const RemoteParams& params) {
    const Reply reply = call(Verb::Get, member, params);
    const Json& value = reply.value(prefix_);
    std::vector<std::string> list;
    if (value.kind == Json::Kind::Array) {
        for (const Json& item : value.items) {
            if (item.kind != Json::Kind::String) break;
            list.push_back(item.text);
        }
        if (list.size() == value.items.size()) return list;
    }
    throw RemoteTransportError(prefix_ + "the reply to " + reply.member + " has a " + kind_name(value.kind) +
                               " Value, not an array of strings");
}

std::vector<int> RemoteAlpacaClient::get_int_list(std::string_view member, const RemoteParams& params) {
    const Reply reply = call(Verb::Get, member, params);
    const Json& value = reply.value(prefix_);
    std::vector<int> list;
    if (value.kind == Json::Kind::Array) {
        for (const Json& item : value.items) {
            const auto parsed = whole_int(item);
            if (!parsed) break;
            list.push_back(*parsed);
        }
        if (list.size() == value.items.size()) return list;
    }
    throw RemoteTransportError(prefix_ + "the reply to " + reply.member + " has a " + kind_name(value.kind) +
                               " Value, not an array of whole numbers inside int");
}

void RemoteAlpacaClient::put(std::string_view member, const RemoteParams& params) { call(Verb::Put, member, params); }

}  // namespace alpacacore::vendor::remotealpaca
