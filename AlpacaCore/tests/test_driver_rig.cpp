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

#include <alpacacore/alpaca_defs.h>
#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::AlpacaException;
using namespace alpacacore::test;

// Falsified by: driver_rig.h:40 `==` changed to `!=` in the code comparison (the expected code is then rejected).
TEST_CASE("driver_rig - require_alpaca_error accepts the expected code", "[driver_rig]") {
    require_alpaca_error([] { throw AlpacaException("x", AlpacaError::NotConnected); }, AlpacaError::NotConnected);
}

// A failed helper aborts its test case, so each failure mode is its own [!shouldfail] case: the case passes
// only while the helper fails.
// Falsified by: require_alpaca_error() dropping the FAIL() after fn() (a call that does not throw passes).
TEST_CASE("driver_rig - require_alpaca_error fails when nothing is thrown", "[driver_rig][!shouldfail]") {
    require_alpaca_error([] {}, AlpacaError::NotConnected);
}

// Falsified by: require_alpaca_error() not comparing error_code() with the expected code.
TEST_CASE("driver_rig - require_alpaca_error fails on the wrong code", "[driver_rig][!shouldfail]") {
    require_alpaca_error([] { throw AlpacaException("x", AlpacaError::InvalidValue); }, AlpacaError::NotConnected);
}

// Falsified by: require_alpaca_error() catching std::exception and treating it as a pass.
TEST_CASE("driver_rig - require_alpaca_error does not swallow a non-Alpaca exception", "[driver_rig][!shouldfail]") {
    require_alpaca_error([] { throw std::runtime_error("boom"); }, AlpacaError::NotConnected);
}

// Falsified by: timed() returning zero or the start time instead of the elapsed time.
TEST_CASE("driver_rig - timed returns the elapsed duration of the call", "[driver_rig]") {
    const auto spent = timed([] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); });
    CHECK(spent >= std::chrono::milliseconds(20));
    CHECK(spent < std::chrono::seconds(5));
}

// Falsified by: parse_timestamp() dropping the milliseconds or reading the month as zero-based.
TEST_CASE("driver_rig - parse_timestamp reads the DeviceState TimeStamp format", "[driver_rig]") {
    const auto stamp = parse_timestamp("2026-10-10T21:46:30.626Z");
    const auto base = parse_timestamp("2026-10-10T21:46:30.000Z");
    CHECK(stamp - base == std::chrono::milliseconds(626));
    CHECK(parse_timestamp("2026-10-10T21:46:31.000Z") - base == std::chrono::seconds(1));
    CHECK(parse_timestamp("2026-11-10T21:46:30.000Z") - base == std::chrono::hours(24 * 31));
}

// Falsified by: parse_timestamp() accepting text that is not the TimeStamp format.
TEST_CASE("driver_rig - parse_timestamp rejects a malformed stamp", "[driver_rig][!shouldfail]") {
    (void)parse_timestamp("not a timestamp");
}

// Falsified by: time_stamp_of() returning another entry's value or a non-empty string when TimeStamp is absent.
TEST_CASE("driver_rig - time_stamp_of picks the TimeStamp entry", "[driver_rig]") {
    std::vector<alpacacore::DeviceState> state;
    CHECK(time_stamp_of(state).empty());
    state.push_back({"Other", std::string("no")});
    state.push_back({"TimeStamp", std::string("2026-10-10T21:46:30.626Z")});
    CHECK(time_stamp_of(state) == "2026-10-10T21:46:30.626Z");
}
