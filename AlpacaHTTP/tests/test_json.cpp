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

#include <alpacahttp/json_utils.h>
#include <alpacahttp/response.h>
#include "test_assert.h"
#include <iostream>

using namespace alpacahttp;

int main() {
    std::cout << "Testing JSON utilities...\n";

    // Test creating error response
    auto error_response = make_error_response(123, 456, 1024, "Test error");
    EXPECT(error_response.client_transaction_id == 123);
    EXPECT(error_response.server_transaction_id == 456);
    EXPECT(error_response.error_number == 1024);
    EXPECT(error_response.error_message == "Test error");

    // Test JSON serialization
    auto json = to_json(error_response);
    EXPECT(json["ClientTransactionID"] == 123);
    EXPECT(json["ServerTransactionID"] == 456);
    EXPECT(json["ErrorNumber"] == 1024);
    EXPECT(json["ErrorMessage"] == "Test error");

    // Test success response with value
    auto success_response = make_success_response(789, 101112, true);
    EXPECT(success_response.client_transaction_id == 789);
    EXPECT(success_response.server_transaction_id == 101112);
    EXPECT(success_response.error_number == 0);
    EXPECT(success_response.value.has_value());
    EXPECT(std::get<bool>(success_response.value.value()) == true);

    // Test JSON parsing
    auto parsed = from_json(json);
    EXPECT(parsed.client_transaction_id == 123);
    EXPECT(parsed.server_transaction_id == 456);
    EXPECT(parsed.error_number == 1024);

    std::cout << "All JSON tests passed!\n";
    return 0;
}

