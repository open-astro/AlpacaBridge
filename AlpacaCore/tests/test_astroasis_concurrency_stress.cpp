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

// Connect/disconnect/operate concurrency stress for the Astroasis Oasis
// Focuser (issue #101). The lifecycle and destruction cases run the real driver and
// protocol wrapper over FakeHidTransport behind LockedAstroasisHidTransport, so
// connects succeed and the storm reaches the handshake, the state poll and the
// operate calls, not just the connect-failure path (issue #606, #1380). The
// transport is injected, so no case here opens a hidraw node.
//
// The enumeration-vs-connect case at the bottom stays on the real hidapi-backed
// wrapper: it connects to the deliberately-nonexistent
// "/dev/hidraw-alpacabridge-absent" (not the unit tests' "/dev/hidraw0", which
// can exist on a dev box and would risk hid_open_path matching an unrelated HID
// device -- hid_open_path does not check VID:PID), and it calls
// enumerate_astroasis_focusers() for real, which is a read-only udev/libusb scan
// for VID:PID 338F:A0F0. It opens nothing and commands nothing, on any device.

#include <alpacacore/focuser_driver.h>
#include <alpacacore/vendor/astroasis/astroasis_focuser_driver.h>
#include <alpacacore/vendor/astroasis/astroasis_protocol_wrapper.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_astroasis_hid.h"
#include "locked_astroasis_hid_transport.h"

using alpacacore::AlpacaDriver;

namespace {

// The driver over the shared fake, serialised the way hidapi serialises its calls.
std::unique_ptr<alpacacore::FocuserDriver> make_locked_fake_focuser(
    const std::shared_ptr<alpacacore::test::FakeHidState>& state) {
    return alpacacore::vendor::astroasis::create_astroasis_focuser(
        0, "/dev/hidraw-fake",
        std::make_unique<alpacacore::test::LockedAstroasisHidTransport>(
            std::make_unique<alpacacore::test::FakeHidTransport>(state)));
}

}  // namespace

TEST_CASE("Astroasis focuser - concurrent connect/disconnect/operate stress", "[astroasis][focuser][stress]") {
    auto state = std::make_shared<alpacacore::test::FakeHidState>();
    auto driver = make_locked_fake_focuser(state);

    // open-astro#326: StressCallGuard replaces the local call() lambda. Same
    // per-call isolation, but it now COUNTS what it swallows, so an unexpected
    // throw fails the case instead of being discarded silently.
    // NotConnected while a lifecycle thread has the driver down; step size and temperature
    // compensation answer NotImplemented (PropertyNotImplemented == 0x400), the Oasis having neither.
    alpacacore::test::StressCallGuard guard(
        {alpacacore::AlpacaError::NotConnected, alpacacore::AlpacaError::NotImplemented});
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& focuser = static_cast<alpacacore::FocuserDriver&>(d);
        // Any of these throws NotConnected while a lifecycle thread has the
        // driver disconnected -- the harness only swallows the exception from
        // the WHOLE callback per call, so without individual catches here the
        // first throw (get_is_moving) would short-circuit the rest and every
        // other locked getter/setter below would go unexercised.
        guard([&] { static_cast<void>(focuser.get_is_moving()); });
        guard([&] { static_cast<void>(focuser.get_position()); });
        guard([&] { static_cast<void>(focuser.get_max_step()); });
        guard([&] { static_cast<void>(focuser.get_max_increment()); });
        guard([&] { static_cast<void>(focuser.get_temperature()); });
        guard([&] { static_cast<void>(focuser.get_step_size()); });
        guard([&] { focuser.set_temp_comp(true); });
        guard([&] { focuser.move(2000); });
        guard([&] { focuser.halt(); });
    });

    // open-astro#326: settle_connected() rather than a bare set_connected():
    // right after a storm the last async task may still be in flight, so a
    // single sync disconnect can legitimately no-op against the pending-
    // disconnect machinery and a bare CHECK would fail on a correct driver.
    CHECK(alpacacore::test::settle_connected(*driver, false));

    // The storm got past the connect-failure path: the fake opened, and the
    // handshake and state poll reached the wire. Read under the fake's lock.
    int opens = 0;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        opens = state->opens;
    }
    CHECK(opens > 0);
    CHECK(state->transactions() > 0);

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Astroasis focuser - destruction races an in-flight connect", "[astroasis][focuser][stress]") {
    // One fresh fake per iteration; the driver's transport owns a reference to it, so it dies with the driver.
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return make_locked_fake_focuser(std::make_shared<alpacacore::test::FakeHidState>()); });
}

// enumerate_astroasis_focusers() (the by-index factory, on an HTTP request
// thread) racing Impl::connect() on separate wrapper instances. Both reach
// hidapi's process-global state -- hid_init's ref-counted context, the backend
// bus scan behind hid_enumerate/hid_open_path -- which the per-instance
// Impl::mutex_ cannot order; hid_global_mutex() in the wrapper does.
//
// Be clear about what this does NOT do: it will not fail on the unlocked
// version. libhidapi is uninstrumented, so TSan sees nothing inside it, and an
// hid_init/hid_enumerate race does not reliably fault. The serialization is
// correctness-by-construction, held by review against the wrapper's comment.
//
// Its coverage is also half a connect: with no device attached, hid_open_path
// fails on the sentinel and connect() throws before the handshake, so the
// hid_write/hid_read_timeout and hid_close halves are never reached (verified
// by mutation -- holding the global lock across all of connect(), which would
// self-deadlock in close_device_locked(), still passes here). What it does hold
// is the pairing the finding named -- enumerate on one thread against
// hid_init/hid_open_path on another, twice over, on distinct instances --
// running clean and terminating: a smoke and liveness check over the paths that
// are reachable without hardware.
TEST_CASE("Astroasis focuser - enumeration races connect across instances", "[astroasis][focuser][stress]") {
    using namespace alpacacore::vendor::astroasis;

    constexpr int kIterations = 200;
    // Enumeration is the expensive half: each pass is a real udev/libusb bus
    // scan, and they are now all serialized on hid_global_mutex(), so the case
    // costs (enumerator passes x scan time). That is near-zero in a CI
    // container with no hidraw nodes, but tens of milliseconds per scan on a
    // dev box or SBC with a populated USB tree -- where this also runs under
    // RUN_TSAN=1 ./scripts/ci_preflight.sh. Fewer passes cost nothing here:
    // what is being checked is that the two paths interleave without hanging,
    // not throughput.
    constexpr int kEnumerationIterations = 50;
    std::atomic<bool> start{false};
    std::atomic<int> enumerations{0};
    std::atomic<int> connect_attempts{0};

    // Two wrappers, each connecting to the absent sentinel path (see the file
    // header: never a real HID node, so this cannot touch attached hardware).
    AstroasisProtocolWrapper first;
    AstroasisProtocolWrapper second;

    auto connector = [&](AstroasisProtocolWrapper& wrapper) {
        while (!start.load()) {
            std::this_thread::yield();
        }
        for (int i = 0; i < kIterations; ++i) {
            try {
                wrapper.connect("/dev/hidraw-alpacabridge-absent");
            } catch (const alpacacore::AlpacaException&) {
                // Expected every time: hid_open_path fails on the sentinel.
            }
            wrapper.disconnect();
            connect_attempts.fetch_add(1);
        }
    };

    auto enumerator = [&]() {
        while (!start.load()) {
            std::this_thread::yield();
        }
        for (int i = 0; i < kEnumerationIterations; ++i) {
            // Read-only VID:PID bus scan; the result depends on what is
            // plugged in, so only the fact that it returns is asserted.
            static_cast<void>(enumerate_astroasis_focusers());
            enumerations.fetch_add(1);
        }
    };

    std::vector<std::thread> threads;
    threads.emplace_back([&] { connector(first); });
    threads.emplace_back([&] { connector(second); });
    threads.emplace_back(enumerator);
    threads.emplace_back(enumerator);
    start.store(true);
    for (auto& t : threads) {
        t.join();
    }

    CHECK(connect_attempts.load() == 2 * kIterations);
    CHECK(enumerations.load() == 2 * kEnumerationIterations);
    CHECK(first.is_connected() == false);
    CHECK(second.is_connected() == false);
}
