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

// Decision 0009 (link-loss and relink policy), Sky-Watcher as the reference
// driver, on both transports. A latched fault that stands past
// util::kLinkStalenessBound turns into a LOST link: Connected false,
// operations NotConnected, DeviceState empty, link_lost_at stamped on the task
// clock, and no reconnect until the client asks. Every wait is virtual time on
// a FakeTaskClock; the only real time is the fake transport's reply timeout.

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/serial_io.h>
#include <alpacacore/vendor/skywatcher/skywatcher_protocol_wrapper.h>
#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "catch2_compat.h"
#include "fake_skywatcher_mount.h"
#include "fake_skywatcher_serial_board.h"
#include "fake_task_clock.h"

namespace sw = alpacacore::vendor::skywatcher;
using alpacacore::test::FakeSkyWatcherMount;
using alpacacore::test::FakeSkyWatcherSerialBoard;
using alpacacore::test::FakeTaskClock;

namespace {

struct Transport {
    sw::ConnectionInfo info;
    std::function<void(bool)> silent;  // true: the board stops answering
    std::function<int()> traffic;      // frames the board has seen
};

void require_not_connected(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::NotConnected);
        return;
    }
    FAIL("expected AlpacaException(NotConnected)");
}

void run_staleness_scenario(const Transport& t, FakeTaskClock& clock) {
    auto owned = std::make_unique<sw::SkyWatcherProtocolWrapper>();
    auto* protocol = owned.get();
    auto driver = sw::create_skywatcher_telescope(0, t.info, 39.7, -104.9, 1609.0, std::move(owned), {}, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // Stage one: faulted. Connected stays true, the fault text is published.
    t.silent(true);
    for (int i = 0; i < 3; ++i) CHECK_THROWS(protocol->inquire_position(sw::kAxisRa));
    REQUIRE(protocol->link_faulted());
    const std::string fault = driver->get_link_fault();
    CHECK(fault.find("consecutive failures") != std::string::npos);
    clock.advance(alpacacore::util::kLinkStalenessBound - std::chrono::seconds(1));
    CHECK(driver->get_connected());

    // Stage two: past the bound the link is lost.
    clock.advance(std::chrono::seconds(1));
    CHECK_FALSE(driver->get_connected());
    require_not_connected([&] { driver->get_right_ascension(); });
    require_not_connected([&] { driver->set_tracking(true); });
    CHECK(driver->get_device_state().empty());
    // link_lost_at is stamped once, on the task clock, for this transport.
    const auto lost_at = protocol->consume_link_lost_at();
    REQUIRE(lost_at.has_value());
    CHECK(*lost_at == clock.now());
    // The last fault text stays, so the listing shows why.
    CHECK(driver->get_link_fault().find("consecutive failures") != std::string::npos);

    // No autonomous reconnect: the board comes back, nothing reopens the link.
    t.silent(false);
    const int traffic = t.traffic();
    clock.advance(std::chrono::seconds(120));
    CHECK_FALSE(driver->get_connected());
    require_not_connected([&] { driver->get_declination(); });
    CHECK(t.traffic() == traffic);

    // Only the client's Connect relinks, and it re-runs the connect probe.
    driver->set_connected(true);
    CHECK(driver->get_connected());
    CHECK(t.traffic() > traffic);
    CHECK(driver->get_link_fault().empty());
    driver->set_connected(false);
}

}  // namespace

TEST_CASE("SkyWatcher link loss - UDP: a fault past the staleness bound loses the link",
          "[skywatcher][linkloss][udp]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    Transport t;
    t.info.type = sw::ConnectionType::Network;
    t.info.host = "127.0.0.1";
    t.info.udp_port = mount.port();
    t.info.response_timeout_ms = 100;
    t.silent = [&](bool on) { mount.set_silent(on); };
    t.traffic = [&] { return mount.transactions_served(); };
    run_staleness_scenario(t, clock);
}

TEST_CASE("SkyWatcher link loss - serial: a fault past the staleness bound loses the link",
          "[skywatcher][linkloss][serial]") {
    FakeTaskClock clock;
    FakeSkyWatcherSerialBoard board;
    Transport t;
    t.info.type = sw::ConnectionType::Serial;
    t.info.port_path = board.slave_path();
    t.info.baud_rate = 9600;
    t.info.response_timeout_ms = 100;
    t.silent = [&](bool on) { board.set_muted(on); };
    t.traffic = [&] { return board.count_frames('j') + board.count_frames('e'); };
    run_staleness_scenario(t, clock);
}

TEST_CASE("SkyWatcher link loss - a fault that clears inside the bound is not a loss",
          "[skywatcher][linkloss][udp]") {
    FakeTaskClock clock;
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    sw::ConnectionInfo info;
    info.type = sw::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.udp_port = mount.port();
    info.response_timeout_ms = 100;
    auto owned = std::make_unique<sw::SkyWatcherProtocolWrapper>();
    auto* protocol = owned.get();
    auto driver = sw::create_skywatcher_telescope(0, info, 39.7, -104.9, 1609.0, std::move(owned), {}, clock);
    driver->set_connected(true);
    mount.set_silent(true);
    for (int i = 0; i < 3; ++i) CHECK_THROWS(protocol->inquire_position(sw::kAxisRa));
    REQUIRE(protocol->link_faulted());
    clock.advance(std::chrono::seconds(29));
    mount.set_silent(false);
    CHECK(protocol->inquire_position(sw::kAxisRa) != 0);
    CHECK_FALSE(protocol->link_faulted());
    clock.advance(std::chrono::seconds(60));
    CHECK(driver->get_connected());
    CHECK_FALSE(protocol->consume_link_lost_at().has_value());
    driver->set_connected(false);
}
