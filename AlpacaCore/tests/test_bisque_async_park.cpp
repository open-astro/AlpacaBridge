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
//
// Park is an asynchronous initiator (issue #208): it must return well inside
// the ConformU 4.5 STANDARD 1 s target while TheSkyX parks in the background,
// Slewing stays true until IsParked reports true, and AtPark flips true in the
// same step Slewing drops. A FakeMountServer plays TheSkyX's TCP JavaScript
// endpoint whose park completes ~1.5 s after ParkAndDoNotDisconnect.
#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/bisque/bisque_protocol_wrapper.h>
#include <alpacacore/vendor/bisque/bisque_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_mount_server.h"

namespace {

using Clock = std::chrono::steady_clock;
constexpr const char* kOk = "|No error. Error = 0.";

struct FakeTheSkyXState {
    std::atomic<bool> park_requested{false};
    std::atomic<Clock::rep> park_started{0};
    static constexpr auto kParkDuration = std::chrono::milliseconds(1500);
    bool is_parked() const {
        if (!park_requested.load()) return false;
        const auto started = Clock::time_point(Clock::duration(park_started.load()));
        return Clock::now() - started >= kParkDuration;
    }
};

alpacacore::test::FakeMountServer::Responder theskyx_responder(std::shared_ptr<FakeTheSkyXState> st) {
    return [st](const std::string& chunk) -> std::string {
        auto has = [&](const char* s) { return chunk.find(s) != std::string::npos; };
        if (has("IsConnected")) return "1#";  // handshake has no error prefix
        if (has("ParkAndDoNotDisconnect")) {
            st->park_started.store(Clock::now().time_since_epoch().count());
            st->park_requested.store(true);
            return std::string(kOk) + "OK#";
        }
        if (has("Unpark()") || has("Abort()")) {
            st->park_requested.store(false);
            return std::string(kOk) + "OK#";
        }
        if (has("IsParked()")) return std::string(kOk) + (st->is_parked() ? "true#" : "false#");
        if (has("IsSlewComplete")) return std::string(kOk) + "1#";
        if (has("IsTracking")) return std::string(kOk) + "0#";
        if (has("GetRaDec")) return std::string(kOk) + "5.5,20.25#";
        if (has("GetAzAlt")) return std::string(kOk) + "120.5,45.5#";
        return std::string(kOk) + "OK#";
    };
}

alpacacore::vendor::bisque::ConnectionInfo endpoint(int port) {
    alpacacore::vendor::bisque::ConnectionInfo info;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = 500;
    return info;
}

bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return pred();
}

}  // namespace

TEST_CASE("Bisque async - Park returns immediately, AtPark flips when TheSkyX reports parked",
          "[bisque][telescope][async]") {
    auto st = std::make_shared<FakeTheSkyXState>();
    alpacacore::test::FakeMountServer server(theskyx_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE_FALSE(driver->get_at_park());

    const auto t0 = Clock::now();
    driver->park();
    const auto park_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    CHECK(park_ms < 1000);  // ConformU 4.5 STANDARD target for an async initiator
    REQUIRE(st->park_requested.load());
    REQUIRE(driver->get_slewing());  // parking reports Slewing until AtPark
    REQUIRE_FALSE(driver->get_at_park());

    REQUIRE(wait_until([&] { return driver->get_at_park(); }, 10000));
    REQUIRE_FALSE(driver->get_slewing());

    driver->park();  // second Park on a parked mount is harmless
    REQUIRE(driver->get_at_park());

    driver->unpark();
    REQUIRE_FALSE(driver->get_at_park());
    driver->set_connected(false);
}

TEST_CASE("Bisque async - Unpark during a park cancels it", "[bisque][telescope][async]") {
    auto st = std::make_shared<FakeTheSkyXState>();
    alpacacore::test::FakeMountServer server(theskyx_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::bisque::create_bisque_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->park();
    REQUIRE(driver->get_slewing());
    driver->unpark();
    REQUIRE_FALSE(driver->get_at_park());
    REQUIRE_FALSE(driver->get_slewing());
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    REQUIRE_FALSE(driver->get_at_park());  // the cancelled park task never flips AtPark

    // Disconnect with a park in flight joins the task cleanly.
    driver->park();
    driver->set_connected(false);
    REQUIRE_FALSE(driver->get_connected());
}

#endif  // !_WIN32
