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

// Connect/disconnect/operate concurrency stress for the Bisque telescope
// (issue #101). This driver is in the pool for the lifecycle, not for a
// locking getter: its get_connected() reads an atomic published at the end of
// a connect (issue #910), so the base's get_connected() call is lock-free. The
// case still storms set_connected() (mutex_ -> pending_mutex_ order, the PR
// #115 round-2 ABBA finding) against the base machinery, where TSan's
// lock-order analysis would see the inversion if a getter took mutex_ again.
//
// Hardware-free: connects fail fast (TCP connection refused on the loopback
// port below), which still storms the base machinery, the failure-path
// cleanup, and the getters racing the lifecycle. Whether any driver is a
// blocking-getter shape is decided in async_connectable.h; do not restate it
// here.

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/bisque/bisque_protocol_wrapper.h>
#include <alpacacore/vendor/bisque/bisque_telescope_driver.h>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_bisque_mount.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"

using alpacacore::AlpacaDriver;

namespace {

// Loopback + a port from the dynamic range nothing listens on: instant
// ECONNREFUSED instead of a multi-second timeout per connect attempt.
alpacacore::vendor::bisque::ConnectionInfo refused_endpoint() {
    alpacacore::vendor::bisque::ConnectionInfo info;
    info.host = "127.0.0.1";
    info.tcp_port = 59999;
    info.response_timeout_ms = 250;
    return info;
}

}  // namespace

TEST_CASE("Bisque telescope - concurrent connect/disconnect/operate stress (mutex-guarded getters)",
          "[bisque][telescope][stress]") {
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, refused_endpoint());

    // open-astro#326: one guard per call. run_lifecycle_stress wraps the WHOLE
    // callback in a single try/catch, so before this a storm racing a
    // disconnect exercised get_tracking() and skipped the other three.
    alpacacore::test::StressCallGuard guard;
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& scope = static_cast<alpacacore::TelescopeDriver&>(d);
        guard([&] { scope.set_target_right_ascension(5.0); });
        guard([&] { scope.set_target_declination(20.0); });
        guard([&] { static_cast<void>(scope.get_target_right_ascension()); });
        guard([&] { static_cast<void>(scope.get_target_declination()); });
        guard([&] { static_cast<void>(scope.get_guide_rate()); });
        guard([&] { scope.set_guide_rate({0.004, 0.004}); });
        guard([&] { scope.slew_to_target_async(); });
        guard([&] { scope.sync_to_target(); });
        guard([&] { static_cast<void>(scope.get_tracking()); });
        guard([&] { static_cast<void>(scope.get_right_ascension()); });
        guard([&] { static_cast<void>(scope.get_declination()); });
        guard([&] { static_cast<void>(scope.get_at_park()); });
    });

    // open-astro#326: settle_connected() rather than a bare set_connected():
    // right after a storm the last async task may still be in flight, so a
    // single sync disconnect can legitimately no-op against the pending-
    // disconnect machinery and the CHECK below would fail on a correct driver.
    CHECK(alpacacore::test::settle_connected(*driver, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Bisque telescope - destruction races an in-flight connect", "[bisque][telescope][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::bisque::create_bisque_telescope(0, refused_endpoint()); });
}

TEST_CASE("Bisque UTCDate - connected fake lifecycle stress", "[bisque][telescope][utcdate][stress]") {
    alpacacore::test::FakeBisqueMount mount;
    alpacacore::test::FakeMountServer server(mount.responder());
    alpacacore::test::FakeTaskClock clock;
    REQUIRE(server.ok());
    auto endpoint = refused_endpoint();
    endpoint.tcp_port = server.port();
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, endpoint, clock);
    driver->set_connected(true);
    REQUIRE(driver->get_utc_date() ==
            std::chrono::system_clock::time_point(std::chrono::milliseconds(mount.utc_millis)));
    alpacacore::test::StressCallGuard guard;
    alpacacore::test::StressCallGuard writes(
        {alpacacore::AlpacaError::NotConnected, alpacacore::AlpacaError::PropertyNotImplemented});
    alpacacore::test::run_lifecycle_stress(*driver, [&guard, &writes](AlpacaDriver& d) {
        auto& scope = static_cast<alpacacore::TelescopeDriver&>(d);
        guard([&] { (void)scope.get_utc_date(); });
        guard([&] { (void)scope.get_device_state(); });
        writes([&] { scope.set_utc_date({}); });
    });
    CHECK(alpacacore::test::settle_connected(*driver, false));
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
    INFO(writes.report());
    CHECK(writes.unexpected_count() == 0);
    CHECK(writes.total_calls() > 0);
}
