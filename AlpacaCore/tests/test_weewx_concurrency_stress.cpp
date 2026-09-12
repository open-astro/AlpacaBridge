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

// Connect/disconnect/operate concurrency stress for the WeeWX
// ObservingConditions driver (issue #101). No fake seam exists for its curl
// HTTP fetch, so this points at http://127.0.0.1:1/ -- port 1 is privileged
// and never listening, so every connect fails fast at curl's connect() with
// no network round trip, and it can never reach a real WeeWX instance the
// way a plausible-looking host or port could. That still storms the
// AsyncConnectable machinery and the connect-failure unwind. It does NOT
// reach the poll thread: set_connected(true) throws out of fetch_snapshot()
// before start_polling() is called, so poll_running_ stays false for the
// whole run. The connected surface needs a fake HTTP seam this driver does
// not have.

#include <alpacacore/observingconditions_driver.h>
#include <alpacacore/vendor/weewx/weewx_observingconditions_driver.h>

#include <chrono>

#include "catch2_compat.h"
#include "concurrency_stress.h"

using alpacacore::AlpacaDriver;

namespace {

// Port 1 never listens; the short timeout keeps each failed connect cheap so
// the storm gets many full lifecycle cycles inside its window.
alpacacore::vendor::weewx::WeeWxHttpConfig unreachable_config() {
    alpacacore::vendor::weewx::WeeWxHttpConfig config;
    config.url = "http://127.0.0.1:1/";
    config.timeout = std::chrono::milliseconds(100);
    return config;
}

}  // namespace

TEST_CASE("WeeWX observing conditions - concurrent connect/disconnect/operate stress",
          "[weewx][observingconditions][stress]") {
    auto driver = alpacacore::vendor::weewx::create_weewx_observingconditions(0, unreachable_config());

    // open-astro#326: StressCallGuard replaces the local call() lambda. It
    // catches std::exception as well, like the lambda did, but records rather
    // than discards -- so anything escaping curl teardown now fails the case
    // instead of vanishing.
    // The set REPLACES the default {NotConnected}. NotImplemented is here
    // because a WeeWX station legitimately does not carry every sensor this
    // callback reads, so those getters throw "Sensor not implemented" on every
    // pass -- the ASCOM contract working, not a defect. The old call() lambda
    // discarded it; the guard counts it, so it has to be named. (The three
    // NotImplemented variants share one numeric code, so this covers all.)
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected,
                                            alpacacore::AlpacaError::NotImplemented};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& oc = static_cast<alpacacore::ObservingConditionsDriver&>(d);
        // The sensor getters throw NotConnected on this unreachable URL
        // (AveragePeriod answers without the device, DeviceState swallows
        // internally), and the harness only swallows the exception from the
        // callback as a whole -- so without per-call handling the first
        // throw would skip every call below it and they would never be
        // exercised at all. std::exception rather than AlpacaException:
        // anything else escaping curl teardown would unwind just the same.
        guard([&] { static_cast<void>(oc.get_temperature()); });
        guard([&] { static_cast<void>(oc.get_humidity()); });
        guard([&] { static_cast<void>(oc.get_dew_point()); });
        guard([&] { static_cast<void>(oc.get_pressure()); });
        guard([&] { static_cast<void>(oc.get_wind_speed()); });
        guard([&] { static_cast<void>(oc.get_sky_quality()); });
        guard([&] { static_cast<void>(oc.get_sky_temperature()); });
        guard([&] { static_cast<void>(oc.get_average_period()); });
        guard([&] { oc.set_average_period(0.0); });
        guard([&] { static_cast<void>(oc.get_time_since_last_update("temperature")); });
        guard([&] { static_cast<void>(oc.get_sensor_description("temperature")); });
        guard([&] { static_cast<void>(oc.get_device_state()); });
        guard([&] { oc.refresh(); });
    });

    // Connected can only be false here: the sentinel URL can never resolve to
    // a listening server, so no connect in the storm ever succeeds.
    CHECK(driver->get_connected() == false);
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("WeeWX observing conditions - destruction races an in-flight connect",
          "[weewx][observingconditions][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::weewx::create_weewx_observingconditions(0, unreachable_config()); });
}
