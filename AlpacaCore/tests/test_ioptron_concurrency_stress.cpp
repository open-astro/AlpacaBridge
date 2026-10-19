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

// Connect/disconnect/operate concurrency stress for the iOptron
// telescope (audit follow-up, 3.0.1), plus the iEFW filter wheel and iEAF
// focuser cases further down (#271, documented in their own block comment).
// The audited detached-thread fixes on the telescope driver (async slew +
// pulse-guide threads racing set_connected(false) and destruction) only run
// on a CONNECTED driver, so the telescope tests connect through a
// FakeMountServer: the protocol wrapper treats a successful TCP
// connect as mount-connected and every post-connect query tolerates failure,
// so a canned "0#" responder is enough to spawn the driver's worker threads
// on a hardware-free host. Operations racing a disconnect are EXPECTED to
// throw; what must never happen is a crash, hang, or TSan report.

#ifndef _WIN32

#include <alpacacore/filterwheel_driver.h>
#include <alpacacore/focuser_driver.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_filterwheel_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_ieaf_focuser_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_protocol_wrapper.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_ioptron_ieaf.h"
#include "fake_mount_server.h"
#include "fake_pty_write.h"

using alpacacore::AlpacaDriver;

namespace {

alpacacore::vendor::ioptron::ConnectionInfo ioptron_endpoint(int port) {
    alpacacore::vendor::ioptron::ConnectionInfo info;
    info.type = alpacacore::vendor::ioptron::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = 50;  // dumb canned replies leave some reads to time out — keep that cheap
    return info;
}

void telescope_operate(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& scope = static_cast<alpacacore::TelescopeDriver&>(d);
    guard([&] { scope.set_target_right_ascension(5.0); });
    guard([&] { scope.set_target_declination(20.0); });
    guard([&] { static_cast<void>(scope.get_target_right_ascension()); });
    guard([&] { static_cast<void>(scope.get_target_declination()); });
    guard([&] { static_cast<void>(scope.get_tracking()); });
    guard([&] { static_cast<void>(scope.get_right_ascension()); });
    guard([&] { static_cast<void>(scope.get_declination()); });
    guard([&] { static_cast<void>(scope.get_slewing()); });
    // The newly-fixed thread paths: async slew and pulse guiding issued
    // while the lifecycle threads disconnect underneath them.
    guard([&] { scope.slew_to_coordinates_async(5.0, 20.0); });
    guard([&] { scope.pulse_guide(0, 50); });
    guard([&] { scope.abort_slew(); });
    guard([&] { scope.slew_to_target(); });
}

}  // namespace

TEST_CASE("iOptron telescope - concurrent connect/disconnect/slew/pulse stress", "[ioptron][telescope][stress]") {
    alpacacore::test::FakeMountServer server;
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, ioptron_endpoint(server.port()));

    // open-astro#326: one guard per call, replacing no isolation at all --
    // run_lifecycle_stress wraps the WHOLE callback in one try/catch, so a
    // storm racing a disconnect exercised get_tracking() and skipped the six
    // calls below it, including the async slew and pulse-guide paths this
    // registration exists to storm.
    //
    // The set REPLACES the default {NotConnected}. This registration runs
    // CONNECTED over FakeMountServer, so a live driver's operate callback
    // legitimately throws more than that: InvalidValue and InvalidOperation
    // from a slew or pulse racing a park or another motion, and the shared
    // NotImplemented code where the canned fake cannot answer.
    //
    // DriverException is here for the FAKE, not the driver: FakeMountServer
    // answers with deliberately dumb canned replies (this file's own
    // destruction case notes the connect sequence "rides several read
    // timeouts" because of them), so the driver correctly reports "Invalid
    // status response from mount" and the like. Those were invisible while the
    // callback aborted at the first throw; the guard counts them, so they have
    // to be named. A real driver defect would show up as a code outside this
    // set, and guard.report() names every distinct one it saw.
    alpacacore::test::StressCallGuard guard{
        alpacacore::AlpacaError::NotConnected,     alpacacore::AlpacaError::InvalidValue,
        alpacacore::AlpacaError::InvalidOperation, alpacacore::AlpacaError::ValueNotSet,
        alpacacore::AlpacaError::NotImplemented,   alpacacore::AlpacaError::DriverException};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { telescope_operate(guard, d); });

    // open-astro#326: settle_connected() rather than a bare set_connected():
    // right after a storm the last async task may still be in flight, so a
    // single sync disconnect can legitimately no-op against the pending-
    // disconnect machinery and a bare CHECK would fail on a correct driver.
    CHECK(alpacacore::test::settle_connected(*driver, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("iOptron telescope - destruction races an in-flight connect", "[ioptron][telescope][stress]") {
    alpacacore::test::FakeMountServer server;
    REQUIRE(server.ok());
    const int port = server.port();
    alpacacore::test::run_destruction_during_connect_stress(
        [port]() { return alpacacore::vendor::ioptron::create_ioptron_telescope(0, ioptron_endpoint(port)); },
        // 25 iterations, not the default 100: with the dumb canned replies a
        // fake-connected mount's connect sequence rides several read
        // timeouts, and each destruction joins the in-flight connect.
        25);
}

TEST_CASE("iOptron telescope - destruction mid-operation (slew/pulse threads live)", "[ioptron][telescope][stress]") {
    alpacacore::test::FakeMountServer server;
    REQUIRE(server.ok());

    for (int i = 0; i < 10; ++i) {
        auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, ioptron_endpoint(server.port()));
        static_cast<void>(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
        try {
            driver->slew_to_coordinates_async(5.0, 20.0);
        } catch (const std::exception&) {
        }
        try {
            driver->pulse_guide(0, 300);
        } catch (const std::exception&) {
        }
        if ((i % 2) != 0) {
            // Half the time a disconnect is also in flight at destruction —
            // the destructor must join every worker thread, every time.
            driver->disconnect();
        }
        driver.reset();
    }
}

// iEFW filter wheel and iEAF focuser stress (#271, #1068): these are separate
// serial devices from the mount above (each on its own Prolific USB-serial
// bridge). Both run CONNECTED over a pty fake: the driver opens the pty slave
// like the device's port, so the connect handshake succeeds and the storm
// reaches the post-connect paths, not only the failed-connect cleanup. The
// iEAF uses FakeIoptronIeaf (fake_ioptron_ieaf.h). FakeIefw below answers the
// iEFW command set; it lives in this file because a new fake_*.h needs a
// kFakeConnectableRoster row (check_docs_drift.py), which is a tier-2 sweep
// change of its own.
//
// Both use the explicit-port factory: the by-index factories scan
// enumerate_*_ports() at connect, which on a dev box with a Prolific adapter
// attached opens (and DTR-toggles) real hardware, and never finds a pty.
// The by-index laziness is pinned by test_ioptron_ieaf_focuser_deferred_connect.cpp.
namespace {

using alpacacore::test::pty_write_bounded;
using alpacacore::test::PtyPair;

class FakeIefw {
public:
    FakeIefw() : pty_("FakeIefw") {
        reader_ = std::thread([this] { run(); });
    }

    ~FakeIefw() {
        stop_.store(true);
        if (reader_.joinable()) reader_.join();
    }

    FakeIefw(const FakeIefw&) = delete;
    FakeIefw& operator=(const FakeIefw&) = delete;

    std::string slave_path() const { return pty_.slave_path(); }

    /// Every command received so far, in wire order.
    std::vector<std::string> commands() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return commands_;
    }

    int count(const std::string& needle) const {
        std::lock_guard<std::mutex> lock(mutex_);
        int n = 0;
        for (const auto& c : commands_) {
            if (c.find(needle) != std::string::npos) ++n;
        }
        return n;
    }

    /// Handshakes that reached the wire: one per connect.
    int connects() const { return count(":DeviceInfo#"); }

    int slot() const { return slot_.load(); }

private:
    void run() {
        std::string pending;
        char buf[64];
        while (!stop_.load()) {
            struct pollfd pfd {};
            pfd.fd = pty_.master_fd();
            pfd.events = POLLIN;
            if (poll(&pfd, 1, 20) <= 0) continue;
            const ssize_t n = read(pty_.master_fd(), buf, sizeof(buf));
            if (n <= 0) continue;
            for (ssize_t i = 0; i < n; ++i) {
                if (pending.empty() && buf[i] != ':') continue;  // skip stray bytes
                pending += buf[i];
                if (buf[i] == '#') {
                    handle(pending);
                    pending.clear();
                }
            }
        }
    }

    void handle(const std::string& cmd) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            commands_.push_back(cmd);
        }
        char reply[40] = {};
        if (cmd == ":DeviceInfo#") {
            std::snprintf(reply, sizeof(reply), "%06d%02d%04d#", slot_.load(), 99, 100);
        } else if (cmd == ":FW1#") {
            std::snprintf(reply, sizeof(reply), "IEFW-FAKE-01#");
        } else if (cmd == ":WP#") {
            std::snprintf(reply, sizeof(reply), "%02d#", slot_.load());
        } else if (cmd.rfind(":WM", 0) == 0) {
            slot_.store(std::atoi(cmd.c_str() + 3));
            std::snprintf(reply, sizeof(reply), "1");
        } else if (cmd.rfind(":WF", 0) == 0) {
            std::snprintf(reply, sizeof(reply), "+00000#");
        } else if (cmd.rfind(":WO", 0) == 0) {
            std::snprintf(reply, sizeof(reply), "1");
        } else {
            return;  // unknown command: the firmware stays silent
        }
        pty_write_bounded(pty_.master_fd(), std::string(reply), stop_);
    }

    PtyPair pty_;
    std::thread reader_;
    std::atomic<bool> stop_{false};

    mutable std::mutex mutex_;
    std::vector<std::string> commands_;

    std::atomic<int> slot_{0};
};

}  // namespace

TEST_CASE("iEFW filter wheel - concurrent connect/disconnect/operate stress", "[ioptron][filterwheel][stress]") {
    FakeIefw fake;
    auto driver = alpacacore::vendor::ioptron::create_iefw_filterwheel(0, fake.slave_path());

    // The connect really succeeds before the storm (#1068): without this a fake
    // that cannot connect would leave the storm on the failed-connect path again.
    REQUIRE(alpacacore::test::settle_connected(*driver, true));
    REQUIRE(fake.connects() >= 1);

    // open-astro#326: one guard per call. Only the default {NotConnected} is
    // expected: against the fake a storm saw no other code (guard.report() empty).
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& wheel = static_cast<alpacacore::FilterWheelDriver&>(d);
        guard([&] { static_cast<void>(wheel.get_position()); });
        guard([&] { wheel.set_position(1); });
        guard([&] { static_cast<void>(wheel.get_names()); });
        guard([&] { wheel.set_names({"L", "R", "G", "B", "Ha"}); });
        guard([&] { static_cast<void>(wheel.get_focus_offsets()); });
        guard([&] { wheel.set_focus_offsets({0, 0, 0, 0, 0}); });
    });

    // open-astro#326: settle_connected() rather than a bare set_connected().
    CHECK(alpacacore::test::settle_connected(*driver, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("iEFW filter wheel - a racing disconnect is never dropped", "[ioptron][filterwheel][stress]") {
    FakeIefw fake;
    auto driver = alpacacore::vendor::ioptron::create_iefw_filterwheel(0, fake.slave_path());

    CHECK_FALSE(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, false));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, true));
}

TEST_CASE("iEFW filter wheel - destruction races an in-flight connect", "[ioptron][filterwheel][stress]") {
    // One fresh fake per iteration: a shared pty keeps the replies written to a
    // driver destroyed mid-connect queued for the next one.
    alpacacore::test::run_destruction_during_connect_stress_fresh_fake(
        []() { return std::make_unique<FakeIefw>(); },
        [](FakeIefw& fake) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::ioptron::create_iefw_filterwheel(0, fake.slave_path());
        },
        25);
}

TEST_CASE("iEAF focuser - concurrent connect/disconnect/operate stress", "[ioptron][focuser][stress]") {
    alpacacore::test::FakeIoptronIeaf fake;
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, fake.slave_path());

    // The connect really succeeds before the storm (#1068).
    REQUIRE(alpacacore::test::settle_connected(*driver, true));
    REQUIRE(fake.connects() >= 1);

    // open-astro#326: one guard per call. Only the default {NotConnected} is
    // expected: against the fake a storm saw no other code (guard.report() empty).
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& focuser = static_cast<alpacacore::FocuserDriver&>(d);
        guard([&] { static_cast<void>(focuser.get_position()); });
        guard([&] { static_cast<void>(focuser.get_is_moving()); });
        guard([&] { static_cast<void>(focuser.get_temperature()); });
        guard([&] { focuser.move(100); });
        guard([&] { focuser.halt(); });
    });

    // open-astro#326: settle_connected() rather than a bare set_connected().
    CHECK(alpacacore::test::settle_connected(*driver, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("iEAF focuser - a racing disconnect is never dropped", "[ioptron][focuser][stress]") {
    alpacacore::test::FakeIoptronIeaf fake;
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, fake.slave_path());

    CHECK_FALSE(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, false));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, true));
}

TEST_CASE("iEAF focuser - destruction races an in-flight connect", "[ioptron][focuser][stress]") {
    alpacacore::test::run_destruction_during_connect_stress_fresh_fake(
        []() { return std::make_unique<alpacacore::test::FakeIoptronIeaf>(); },
        [](alpacacore::test::FakeIoptronIeaf& fake) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::ioptron::create_ieaf_focuser(0, fake.slave_path());
        },
        25);
}

#endif  // !_WIN32
