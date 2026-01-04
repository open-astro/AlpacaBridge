# ConformU Test Results

This folder contains ConformU test results for AlpacaHTTP protocol verification.

## Overview

ConformU (ASCOM Universal Device Conformance Checker) is used to verify that AlpacaHTTP correctly implements the ASCOM Alpaca HTTP/REST protocol specification. These tests verify that the HTTP endpoints, request/response formats, and protocol behavior comply with the ASCOM Alpaca API specification.

## Structure

Test results are organized by device type and test run:
- Each test file contains ConformU protocol verification output for a specific device type
- Test files typically include:
  - `ascom.conformu.*.txt` - Detailed test execution log showing protocol compliance
  - Protocol verification results for HTTP endpoints, request/response formats, and error handling

## Current Test Results

- **Telescope** - Protocol verification for Telescope device endpoints (iOptron driver)

## Notes

- All HTTP endpoints must pass ConformU protocol verification before being considered compliant
- Protocol verification tests the HTTP/REST layer, not the underlying driver implementation
- Test results are generated using ConformU version 4.1.0 or later
- Tests verify:
  - HTTP endpoint URLs and methods (GET, PUT, POST)
  - Request/response JSON format compliance
  - Error response format and error codes
  - Parameter ordering and handling
  - Protocol version compatibility
- For more information about ConformU, see the [ASCOM ConformU documentation](https://ascom-standards.org/)

