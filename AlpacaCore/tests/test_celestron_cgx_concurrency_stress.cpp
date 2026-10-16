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

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/celestron/celestron_cgx_telescope_driver.h>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_celestron_cgx_mount.h"

namespace {

std::unique_ptr<alpacacore::TelescopeDriver> make_driver(
    const std::shared_ptr<alpacacore::test::FakeCelestronCgxMount>& mount) {
    mount->set_motor_identity(0x10, {7, 17, 0, 25}, {0x17, 0x88});
    mount->set_motor_identity(0x11, {7, 17, 0, 25}, {0x17, 0x88});
    mount->set_cgx_switches_present(false);
    alpacacore::vendor::celestron::CelestronCgxConnectionInfo info;
    info.port_path = mount->slave_path();
    info.site_latitude_degrees = 45.0;
    info.site_longitude_degrees = -75.0;
    return alpacacore::vendor::celestron::create_celestron_cgx_telescope(0, info);
}

void operate(alpacacore::test::StressCallGuard& guard, alpacacore::AlpacaDriver& base) {
    auto& scope = static_cast<alpacacore::TelescopeDriver&>(base);
    guard([&] { scope.set_target_right_ascension(6.0); });
    guard([&] { scope.set_target_declination(30.0); });
    guard([&] { static_cast<void>(scope.get_target_right_ascension()); });
    guard([&] { static_cast<void>(scope.get_target_declination()); });
    guard([&] { static_cast<void>(scope.get_tracking()); });
    guard([&] { static_cast<void>(scope.get_tracking_rate()); });
    guard([&] { scope.set_tracking_rate(1); });
    guard([&] { static_cast<void>(scope.get_at_home()); });
    guard([&] { static_cast<void>(scope.get_at_park()); });
    guard([&] { static_cast<void>(scope.get_slewing()); });
    guard([&] { scope.find_home(); });
    guard([&] { scope.slew_to_coordinates_async(6.0, 30.0); });
    guard([&] { scope.pulse_guide(0, 100); });
    guard([&] { static_cast<void>(scope.get_is_pulse_guiding()); });
    guard([&] { scope.abort_slew(); });
    guard([&] { scope.park(); });
    guard([&] { scope.unpark(); });
    guard([&] { scope.sync_to_coordinates(6.0, 30.0); });
    guard([&] { scope.set_tracking(true); });
    guard([&] { scope.set_tracking(false); });
}

}  // namespace

TEST_CASE("Celestron CGX USB telescope - connection and motion lifecycle stress", "[celestron][telescope][stress]") {
    auto mount = std::make_shared<alpacacore::test::FakeCelestronCgxMount>();
    REQUIRE(mount->ok());
    mount->set_auto_complete_motions(true);

    alpacacore::test::StressCallGuard guard{
        alpacacore::AlpacaError::NotConnected, alpacacore::AlpacaError::InvalidValue,
        alpacacore::AlpacaError::InvalidOperation, alpacacore::AlpacaError::InvalidWhileParked,
        alpacacore::AlpacaError::NotImplemented};
    auto driver = make_driver(mount);
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](alpacacore::AlpacaDriver& d) { operate(guard, d); });
    CHECK(alpacacore::test::settle_connected(*driver, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Celestron CGX USB telescope - destruction races an in-flight connect", "[celestron][telescope][stress]") {
    auto mount = std::make_shared<alpacacore::test::FakeCelestronCgxMount>();
    REQUIRE(mount->ok());
    mount->set_auto_complete_motions(true);
    alpacacore::test::run_destruction_during_connect_stress([mount] { return make_driver(mount); }, 25);
}

#endif  // !_WIN32
