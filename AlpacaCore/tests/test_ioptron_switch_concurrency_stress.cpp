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

// Connect/disconnect/operate concurrency stress for the iOptron iMate
// PowerBox Switch (#271, #1068). The driver drives a libgpiod chip, so a
// connect can only succeed on a host with the iMate's GPIO bank. These cases
// hand the driver a FakeGpioLineBackend through the create_ioptron_switch(...,
// util::GpioLineBackend&, TaskClock&) test seam instead: the connect really succeeds, the
// storm reaches the post-connect paths (boolean writes, the racing disconnect)
// and nothing needs hardware. The iMate has no serial side (the switch driver
// opens no serial port), so there is no pty here.
//
// The cases count the backend's opens and closes: a claim the storm made and did not release fails them.
//
// This is a SEPARATE file from test_ioptron_concurrency_stress.cpp because
// the PowerBox Switch driver, unlike the mount/filter-wheel/focuser above,
// is only compiled into alpacacore_ioptron when libgpiod >= 2.0 was found
// (ALPACACORE_IOPTRON_POWERBOX, see AlpacaCore/src/vendors/ioptron/CMakeLists.txt)
// -- putting this in the always-built file would fail to link whenever that
// feature is off. CMakeLists.txt only adds this file to TEST_SOURCES under
// the same guard as test_ioptron_switch.cpp.

#include <alpacacore/switch_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_switch_driver.h>

#include <memory>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_gpio_line_backend.h"
#include "fake_ioptron_gpio.h"

using alpacacore::AlpacaDriver;

namespace {

using alpacacore::test::FakeGpioLineBackend;
using Kind = FakeGpioLineBackend::Event::Kind;

// The clock and the backend outlive every driver built over them.
using Rig = alpacacore::test::FakeIoptronGpio;

std::unique_ptr<alpacacore::SwitchDriver> make_switch(Rig& rig) {
    return rig.make(alpacacore::vendor::ioptron::default_imate_powerbox_config());
}

int count_events(const FakeGpioLineBackend& gpio, Kind kind) {
    int n = 0;
    for (const auto& e : gpio.events()) {
        n += e.kind == kind ? 1 : 0;
    }
    return n;
}

}  // namespace

TEST_CASE("iMate PowerBox Switch - concurrent connect/disconnect/operate stress", "[ioptron][switch][stress]") {
    Rig rig;
    auto driver = make_switch(rig);

    // The connect really succeeds before the storm (#1068).
    REQUIRE(alpacacore::test::settle_connected(*driver, true));
    REQUIRE(count_events(rig.gpio, Kind::Open) >= 1);

    // open-astro#326: one guard per call -- before this the callback stopped
    // at the first throw, so only get_max_switch() was ever storm-tested.
    //
    // Only the default {NotConnected} is expected: against the fake backend a storm saw
    // no other code (guard.report() empty).
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& sw = static_cast<alpacacore::SwitchDriver&>(d);
        guard([&] { static_cast<void>(sw.get_max_switch()); });
        guard([&] { static_cast<void>(sw.get_switch(1)); });
        // switch 1 ("DC1"); switch 0 is read-only pass-through
        guard([&] { sw.set_switch(1, true); });
        guard([&] { static_cast<void>(sw.get_switch_value(1)); });
        guard([&] { sw.set_switch_value(1, 1.0); });
    });

    // open-astro#326: settle_connected() rather than a bare set_connected():
    // right after a storm the last async task may still be in flight, so a
    // single sync disconnect can legitimately no-op against the pending-
    // disconnect machinery and the CHECK below would fail on a correct driver.
    CHECK(alpacacore::test::settle_connected(*driver, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
    // Every claim the storm made was released: nothing is left holding the lines.
    CHECK_FALSE(rig.gpio.is_open());
    CHECK(count_events(rig.gpio, Kind::Close) == count_events(rig.gpio, Kind::Open));
}

TEST_CASE("iMate PowerBox Switch - a racing disconnect is never dropped", "[ioptron][switch][stress]") {
    Rig rig;
    auto driver = make_switch(rig);

    CHECK_FALSE(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, false));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, true));
    CHECK_FALSE(rig.gpio.is_open());
}

TEST_CASE("iMate PowerBox Switch - destruction races an in-flight connect", "[ioptron][switch][stress]") {
    Rig rig;
    alpacacore::test::run_destruction_during_connect_stress([&rig]() { return make_switch(rig); });
    // Whatever the destroyed drivers claimed was released with them.
    CHECK_FALSE(rig.gpio.is_open());
}
