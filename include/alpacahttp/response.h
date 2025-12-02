// AlpacaHTTP
// Copyright (c) 2025 Joey Troy
//
// This file is part of AlpacaHTTP.
//
// Licensed under the Server Side Public License, v1.
// https://www.mongodb.com/licensing/server-side-public-license

#pragma once

#include <string>
#include <cstdint>
#include <variant>
#include <optional>
#include <unordered_map>

namespace alpacahttp {

// Alpaca response structure
struct AlpacaResponse {
    std::uint32_t client_transaction_id = 0;
    std::uint32_t server_transaction_id = 0;
    std::int32_t error_number = 0;
    std::string error_message;
    std::optional<std::variant<bool, std::int32_t, double, std::string>> value;

    AlpacaResponse() = default;
    AlpacaResponse(std::uint32_t client_tx_id, std::uint32_t server_tx_id)
        : client_transaction_id(client_tx_id)
        , server_transaction_id(server_tx_id)
    {}
};

// HTTP response wrapper
class Response {
public:
    Response() = default;
    ~Response() = default;

    // Set HTTP status
    void set_status(std::uint16_t status_code, const std::string& reason_phrase = "");
    std::uint16_t status_code() const { return status_code_; }
    const std::string& reason_phrase() const { return reason_phrase_; }

    // Set headers
    void set_header(const std::string& key, const std::string& value);
    void set_content_type(const std::string& type);
    void set_content_length(std::size_t length);

    // Set body
    void set_body(const std::string& body);
    void set_body(const AlpacaResponse& alpaca_response);

    // Get formatted HTTP response
    std::string to_string() const;

    // Getters
    const std::string& body() const { return body_; }
    const std::string& get_header(const std::string& key) const;

private:
    std::uint16_t status_code_ = 200;
    std::string reason_phrase_ = "OK";
    std::unordered_map<std::string, std::string> headers_;
    std::string body_;

    static std::string status_to_reason_phrase(std::uint16_t code);
};

} // namespace alpacahttp

