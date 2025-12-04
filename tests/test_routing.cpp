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

#include <alpacahttp/router.h>
#include <alpacahttp/request.h>
#include <cassert>
#include <iostream>

int main() {
    std::cout << "Testing routing...\n";

    alpacahttp::Router router;
    alpacahttp::Request request;

    // Test management endpoint parsing
    std::string test_request = "GET /management/v1/description HTTP/1.1\r\n\r\n";
    assert(request.parse(test_request));
    assert(request.path() == "/management/v1/description");

    // Test device endpoint parsing
    test_request = "GET /api/v1/camera/0/canconnect HTTP/1.1\r\n\r\n";
    assert(request.parse(test_request));
    assert(request.path() == "/api/v1/camera/0/canconnect");

    // Test query parameters
    test_request = "GET /api/v1/mount/0/slewto?RightAscension=1.5&Declination=-20.3 HTTP/1.1\r\n\r\n";
    assert(request.parse(test_request));
    assert(request.path() == "/api/v1/mount/0/slewto");
    assert(request.has_query_param("RightAscension"));
    assert(request.has_query_param("Declination"));

    std::cout << "All routing tests passed!\n";
    return 0;
}

