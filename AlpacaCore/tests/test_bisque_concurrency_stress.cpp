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
// (issue #101). This driver is deliberately in the pool for its SHAPE, not
// its vendor: get_connected() takes the driver state mutex_ (unlike the
// atomic-connected_ drivers the rest of the suite covers), so it exercises
// the AsyncConnectable base against mutex-guarded state reads — the exact
// combination behind the PR #115 round-2 ABBA finding (get_connected()
// formerly called under pending_mutex_ vs the sync setter's
// mutex_ -> pending_mutex_ order). TSan's lock-order analysis sees the
// inversion on this driver even when no run actually deadlocks.
//
// Hardware-free: connects fail fast (TCP connection refused on the loopback
// port below), which still storms the base machinery, the failure-path
// cleanup, and the mutex-guarded getters racing the lifecycle. SynScan /
// Celestron / iOptron telescope share the same shape via the same base.

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/bisque/bisque_protocol_wrapper.h>
#include <alpacacore/vendor/bisque/bisque_telescope_driver.h>

#include "catch2_compat.h"
#include "concurrency_stress.h"

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

    alpacacore::test::run_lifecycle_stress(*driver, [](AlpacaDriver& d) {
        auto& scope = static_cast<alpacacore::TelescopeDriver&>(d);
        static_cast<void>(scope.get_tracking());
        static_cast<void>(scope.get_right_ascension());
        static_cast<void>(scope.get_declination());
        static_cast<void>(scope.get_at_park());
    });

    static_cast<void>(driver->get_connected());
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
}

TEST_CASE("Bisque telescope - destruction races an in-flight connect", "[bisque][telescope][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::bisque::create_bisque_telescope(0, refused_endpoint()); });
}
