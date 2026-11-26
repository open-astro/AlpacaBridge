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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/power_port_switch_driver.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "catch2_compat.h"

// The shared power-port Switch driver over a test-local port wrapper, so these cases run in
// every build, vendors on or off. The per-board behaviour is covered in each vendor's switch tests.

namespace {

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::AlpacaException;
using alpacacore::util::PowerPortProfile;
using alpacacore::util::PowerPortProfilePort;
using alpacacore::util::PowerPortSwitchDriver;

// An in-memory port wrapper with a stated port count; close() throws when asked to.
class ScriptedPorts {
public:
    explicit ScriptedPorts(std::size_t count, bool throw_on_close = false)
        : count_(count), throw_on_close_(throw_on_close), values_(count, 1) {}

    void open() { open_ = true; }
    void close() {
        open_ = false;
        if (throw_on_close_) throw AlpacaException("scripted close failure", AlpacaError::DriverException);
    }
    bool is_open() const { return open_; }
    std::size_t port_count() const { return count_; }
    int get_value(std::size_t index) const { return values_.at(index); }
    void set_value(std::size_t index, int value) { values_.at(index) = value; }

private:
    std::size_t count_;
    bool throw_on_close_;
    bool open_{false};
    std::vector<int> values_;
};

using ScriptedDriver = PowerPortSwitchDriver<ScriptedPorts>;

// A profile of n on/off ports.
PowerPortProfile make_profile(std::size_t n) {
    PowerPortProfile profile;
    profile.name = "Test Board";
    profile.description = "Test power-port board";
    profile.driver_info = "AlpacaCore test power-port board";
    profile.unique_id_prefix = "Test_PowerPort_";
    profile.log_category = "PP_TEST";
    profile.not_connected_message = "Test board not connected";
    profile.destruction_warning = "Error during test destruction: ";
    for (std::size_t i = 0; i < n; ++i) {
        PowerPortProfilePort port;
        port.name = "Port " + std::to_string(i + 1);
        port.description = "Test port";
        port.range_error = "Switch value out of range [0,1]";
        profile.ports.push_back(std::move(port));
    }
    return profile;
}

// Every WARN logged while it lives, as (component, message).
struct WarnCapture {
    std::vector<std::pair<std::string, std::string>> warns;
    alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
    WarnCapture() {
        alpacacore::logging::set_log_sink(
            [this](alpacacore::logging::LogLevel level, std::string_view component, std::string_view message) {
                if (level == alpacacore::logging::LogLevel::Warn) warns.emplace_back(component, message);
            });
    }
    ~WarnCapture() { alpacacore::logging::set_log_sink(previous); }
    WarnCapture(const WarnCapture&) = delete;
    WarnCapture& operator=(const WarnCapture&) = delete;
    WarnCapture(WarnCapture&&) = delete;
    WarnCapture& operator=(WarnCapture&&) = delete;
};

}  // namespace

// Falsified by: power_port_switch_driver.h constructor dropping the port_count() comparison (a mismatched wrapper
// is accepted and indexed past its table).
TEST_CASE("PowerPortSwitchDriver - a wrapper whose port count differs from the profile is refused",
          "[util][switch][power-port][unit]") {
    const auto profile = make_profile(4);
    bool thrown = false;
    std::string message;
    int code = 0;
    try {
        ScriptedDriver driver(0, profile, std::size_t{2});
    } catch (const AlpacaException& ex) {
        thrown = true;
        message = ex.what();
        code = ex.error_code();
    }
    REQUIRE(thrown);
    CHECK(code == AlpacaError::InvalidValue);
    CHECK(message == profile.name + ": the port wrapper has 2 ports but the profile lists 4");
}

// A pin against the comparison written with the wrong sense; it passes on the code before the check too.
// Falsified by: power_port_switch_driver.h constructor comparing with == instead of != (a matching wrapper is
// refused).
TEST_CASE("PowerPortSwitchDriver - a wrapper whose port count matches the profile is accepted",
          "[util][switch][power-port][unit]") {
    std::unique_ptr<ScriptedDriver> driver;
    REQUIRE_NOTHROW(driver = std::make_unique<ScriptedDriver>(0, make_profile(4), std::size_t{4}));
    CHECK(driver->get_max_switch() == 4);
}

// Falsified by: power_port_switch_driver.h ~PowerPortSwitchDriver logging a fixed text or a category other than
// profile_.log_category.
TEST_CASE("PowerPortSwitchDriver - a close that throws in the destructor logs the profile's warning",
          "[util][switch][power-port][unit]") {
    auto driver = std::make_unique<ScriptedDriver>(0, make_profile(2), std::size_t{2}, /*throw_on_close=*/true);
    WarnCapture capture;
    driver.reset();
    REQUIRE(capture.warns.size() == 1);
    CHECK(capture.warns[0].first == "PP_TEST");
    CHECK(capture.warns[0].second == "Error during test destruction: scripted close failure");
}
