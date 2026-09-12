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
// Focuser (issue #101). No fake seam exists for the hidapi-backed protocol
// wrapper, so this connects against the deliberately-nonexistent
// "/dev/hidraw-alpacabridge-absent" (not the unit tests' "/dev/hidraw0",
// which can exist on a dev box and would risk hid_open_path matching an
// unrelated HID device -- hid_open_path does not check VID:PID). Every
// connect fails fast at that open, which still storms the AsyncConnectable
// machinery and the failure-path cleanup; this test never exercises the
// real connect path, regardless of what hardware is attached.
//
// The file does touch the USB bus, though: the enumeration-vs-connect case at
// the bottom calls enumerate_astroasis_focusers() for real, which is a
// read-only udev/libusb scan for VID:PID 338F:A0F0. It opens nothing and
// commands nothing, on any device.

#include <alpacacore/focuser_driver.h>
#include <alpacacore/vendor/astroasis/astroasis_focuser_driver.h>
#include <alpacacore/vendor/astroasis/astroasis_protocol_wrapper.h>

#include <atomic>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"

using alpacacore::AlpacaDriver;

TEST_CASE("Astroasis focuser - concurrent connect/disconnect/operate stress", "[astroasis][focuser][stress]") {
    auto driver = alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw-alpacabridge-absent");

    // open-astro#326: StressCallGuard replaces the local call() lambda. Same
    // per-call isolation, but it now COUNTS what it swallows, so an unexpected
    // throw fails the case instead of being discarded silently.
    alpacacore::test::StressCallGuard guard;
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& focuser = static_cast<alpacacore::FocuserDriver&>(d);
        // Every one of these throws NotConnected on this sentinel path,
        // every time (the storm never actually connects) -- the harness
        // only swallows the exception from the WHOLE callback per call, so
        // without individual catches here the first throw (get_is_moving)
        // would short-circuit the rest and every other locked getter/setter
        // below would go completely unexercised, not just unconnected.
        guard([&] { static_cast<void>(focuser.get_is_moving()); });
        guard([&] { static_cast<void>(focuser.get_position()); });
        guard([&] { static_cast<void>(focuser.get_max_step()); });
        guard([&] { static_cast<void>(focuser.get_max_increment()); });
        guard([&] { static_cast<void>(focuser.get_temperature()); });
        guard([&] { static_cast<void>(focuser.get_step_size()); });
        guard([&] { focuser.set_temp_comp(true); });
        guard([&] { focuser.move(1234); });
        guard([&] { focuser.halt(); });
    });

    // Still alive and coherent after the storm. Unlike the SVBONY/ZWO
    // fail-fast cases, Connected can only be false here: the sentinel path
    // never resolves to a real HID node, so no connect in the storm ever
    // succeeds.
    CHECK(driver->get_connected() == false);
    // open-astro#326: settle_connected() rather than a bare set_connected():
    // right after a storm the last async task may still be in flight, so a
    // single sync disconnect can legitimately no-op against the pending-
    // disconnect machinery and a bare CHECK would fail on a correct driver.
    CHECK(alpacacore::test::settle_connected(*driver, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Astroasis focuser - destruction races an in-flight connect", "[astroasis][focuser][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw-alpacabridge-absent"); });
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
