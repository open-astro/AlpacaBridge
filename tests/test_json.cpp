// AlpacaHTTP
// Copyright (c) 2025 Joey Troy
//
// This file is part of AlpacaHTTP.
//
// Licensed under the Server Side Public License, v1.
// https://www.mongodb.com/licensing/server-side-public-license

#include <alpacahttp/json_utils.h>
#include <alpacahttp/response.h>
#include <cassert>
#include <iostream>

int main() {
    std::cout << "Testing JSON utilities...\n";

    // Test creating error response
    auto error_response = make_error_response(123, 456, 1024, "Test error");
    assert(error_response.client_transaction_id == 123);
    assert(error_response.server_transaction_id == 456);
    assert(error_response.error_number == 1024);
    assert(error_response.error_message == "Test error");

    // Test JSON serialization
    auto json = to_json(error_response);
    assert(json["ClientTransactionID"] == 123);
    assert(json["ServerTransactionID"] == 456);
    assert(json["ErrorNumber"] == 1024);
    assert(json["ErrorMessage"] == "Test error");

    // Test success response with value
    auto success_response = make_success_response(789, 101112, true);
    assert(success_response.client_transaction_id == 789);
    assert(success_response.server_transaction_id == 101112);
    assert(success_response.error_number == 0);
    assert(success_response.value.has_value());
    assert(std::get<bool>(success_response.value.value()) == true);

    // Test JSON parsing
    auto parsed = from_json(json);
    assert(parsed.client_transaction_id == 123);
    assert(parsed.server_transaction_id == 456);
    assert(parsed.error_number == 1024);

    std::cout << "All JSON tests passed!\n";
    return 0;
}

