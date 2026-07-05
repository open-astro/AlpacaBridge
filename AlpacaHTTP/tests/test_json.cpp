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

#include <iostream>

#include "test_assert.h"

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
    EXPECT(success_response.value->is_boolean());
    EXPECT(success_response.value->get<bool>() == true);

    // Test JSON parsing
    auto parsed = from_json(json);
    EXPECT(parsed.client_transaction_id == 123);
    EXPECT(parsed.server_transaction_id == 456);
    EXPECT(parsed.error_number == 1024);

    // A string Value that happens to look like JSON must stay a string — the
    // old to_json re-parsed strings, which corrupted e.g. numeric-string
    // properties into numbers (wrong ASCOM type).
    auto numeric_string = make_success_response(1, 2, std::string("12345"));
    auto numeric_string_json = to_json(numeric_string);
    EXPECT(numeric_string_json["Value"].is_string());
    EXPECT(numeric_string_json["Value"] == "12345");

    auto bool_string = make_success_response(1, 2, std::string("true"));
    EXPECT(to_json(bool_string)["Value"].is_string());

    // A structured Value is emitted as real JSON, not an escaped string.
    nlohmann::json arr = nlohmann::json::array({"DeviceType", "Vendor"});
    auto array_response = make_success_response(1, 2, arr);
    auto array_json = to_json(array_response);
    EXPECT(array_json["Value"].is_array());
    EXPECT(array_json["Value"].size() == 2);

    std::cout << "All JSON tests passed!\n";
    return 0;
}

