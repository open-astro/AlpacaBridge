// AlpacaHTTP
// Copyright (c) 2025 Joey Troy
//
// This file is part of AlpacaHTTP.
//
// Licensed under the Server Side Public License, v1.
// https://www.mongodb.com/licensing/server-side-public-license

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

