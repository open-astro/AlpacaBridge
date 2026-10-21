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

// open-astro#575, iOptron: the async slew dispatch runs on the slew slot body
// (a non-null async_ctx), so a mount that answers ":MS1"/":MS2" with "0" (target
// rejected, e.g. below the altitude limit) is logged as "treating as no-op"
// and Slewing drops to false -- the client is told nothing. That soft-fail is
// exactly the "work fails after initiation" case of the completion-property
// contract: the next Slewing read must return an Alpaca error instead
// (contract reference in test_skywatcher_async_slew_failure.cpp).

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"  // settle_connected
#include "fake_ioptron_mount.h"
#include "fake_task_clock.h"

namespace {

using Clock = std::chrono::steady_clock;

alpacacore::vendor::ioptron::ConnectionInfo endpoint(int port) {
    alpacacore::vendor::ioptron::ConnectionInfo conn;
    conn.type = alpacacore::vendor::ioptron::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = port;
    // NOT 200 like the other four vendors' #575 tests: iOptron's
    // read_response() sleeps 10ms after every character it reads, so the
    // fake's fixed-width :GEP/:GLS replies (21-24 bytes) alone cost
    // ~200-240ms even with an instant round trip on the wire (confirmed via
    // strace: the fake server's recv() of the request completes and its
    // reply is sent well under 1ms; the 200ms is spent entirely in this
    // driver's own per-character poll loop). 200ms made ensure_not_parked's
    // synchronous status refresh -- called before the #575 code path is even
    // reached -- time out on every run. test_ioptron_telescope.cpp's
    // loopback_endpoint() sees the same cost and avoids it by leaving this
    // at the 5000ms default; matching that here instead of the 200ms used by
    // the other vendors' fakes (whose replies are shorter / read
    // differently).
    conn.response_timeout_ms = 2000;
    return conn;
}

bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return pred();
}

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

class WorkerGate {
public:
    void arm(bool reject_after_release = false, bool accept_second = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        pause_next_ = true;
        entered_ = false;
        released_ = false;
        reject_after_release_ = reject_after_release;
        accept_second_ = accept_second;
    }

    void before_worker_lock(alpacacore::test::FakeIoptronMount& mount) {
        std::unique_lock<std::mutex> lock(mutex_);
        ++calls_;
        const bool pause = pause_next_;
        pause_next_ = false;
        if (pause) {
            entered_ = true;
            cv_.notify_all();
            cv_.wait(lock, [&] { return released_; });
        }
        const bool reject = pause && reject_after_release_;
        const bool accept = calls_ == 2 && accept_second_;
        lock.unlock();
        if (reject) mount.set_reject_goto(true);
        if (accept) mount.set_reject_goto(false);
    }

    bool wait_until_entered() {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::seconds(5), [&] { return entered_; });
    }

    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int calls_ = 0;
    bool pause_next_ = false;
    bool reject_after_release_ = false;
    bool accept_second_ = false;
    bool entered_ = false;
    bool released_ = false;
};

class SlewHandoffGate {
public:
    void before_lock() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++attempts_;
        cv_.notify_all();
    }

    void before_reap() {
        std::unique_lock<std::mutex> lock(mutex_);
        ++reaps_;
        cv_.notify_all();
        if (reaps_ == 1) {
            first_reap_entered_ = true;
            cv_.notify_all();
            cv_.wait(lock, [&] { return release_first_; });
        }
    }

    bool wait_for_attempts(int count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return attempts_ >= count; });
    }

    bool wait_for_first_reap(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return first_reap_entered_; });
    }

    bool wait_for_reaps(int count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return reaps_ >= count; });
    }

    void release_first() {
        std::lock_guard<std::mutex> lock(mutex_);
        release_first_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int attempts_ = 0;
    int reaps_ = 0;
    bool first_reap_entered_ = false;
    bool release_first_ = false;
};

// Threw only for the driver-specific DriverException (0x500) the #575 contract
// requires; any other code is ThrewOther, so a changed code fails the tests.
enum class SlewingRead : std::uint8_t { True, False, Threw, ThrewOther };

SlewingRead read_slewing(alpacacore::TelescopeDriver& driver, std::string* message = nullptr) {
    try {
        return driver.get_slewing() ? SlewingRead::True : SlewingRead::False;
    } catch (const alpacacore::AlpacaException& ex) {
        if (message) *message = ex.what();
        return ex.error_code() == alpacacore::AlpacaError::DriverException ? SlewingRead::Threw
                                                                           : SlewingRead::ThrewOther;
    }
}

// Steps the driver's clock until Slewing reads false: the 5 s post-dispatch override and the status-cache
// age both run on it, so the virtual 20 s the old real-time wait allowed costs no real time.
bool advance_until_not_slewing(alpacacore::TelescopeDriver& driver, alpacacore::test::FakeTaskClock& clock) {
    const auto deadline = clock.now() + std::chrono::seconds(20);
    while (read_slewing(driver) != SlewingRead::False) {
        if (clock.now() >= deadline) return false;
        clock.advance(std::chrono::milliseconds(100));
    }
    return true;
}

}  // namespace

TEST_CASE("iOptron async - a GOTO the mount rejects surfaces through Slewing instead of a silent no-op (#575)",
          "[ioptron][telescope][async][slewfailure]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, endpoint(mount.port()), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    mount.set_reject_goto(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return mount.count(":MS1#") >= 1; }, 5000));  // the dispatch ran on the task

    // Today: "Slew rejected by mount - treating as no-op for async slew" in
    // the log, Slewing=false to the client. Must read as an error.
    std::string message;
    REQUIRE(wait_until([&] { return read_slewing(*driver, &message) == SlewingRead::Threw; }, 10000));
    CHECK_FALSE(message.empty());
    CHECK(message.find("Slew") != std::string::npos);
    CHECK(read_slewing(*driver) == SlewingRead::Threw);  // preserved, not one-shot

    // A retried initiator the mount accepts clears it.
    mount.set_reject_goto(false);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return mount.count(":MS1#") >= 2; }, 5000));
    const SlewingRead after_retry = read_slewing(*driver);
    CHECK((after_retry == SlewingRead::True || after_retry == SlewingRead::False));
    REQUIRE(advance_until_not_slewing(*driver, clock));
    driver->set_connected(false);
}

TEST_CASE("iOptron async - AbortSlew clears a stored slew failure (#575)", "[ioptron][telescope][async][slewfailure]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, endpoint(mount.port()), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    mount.set_reject_goto(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 10000));

    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(read_slewing(*driver) == SlewingRead::False);
    driver->set_connected(false);
}

TEST_CASE("iOptron async AltAz retry clears its prior error before worker preparation",
          "[ioptron][telescope][async][slewfailure]") {
    alpacacore::test::FakeIoptronMount mount("0012", 0.0);
    REQUIRE(mount.ok());
    WorkerGate gate;
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope_with_slew_worker_hook(
        0, endpoint(mount.port()), [&] { gate.before_worker_lock(mount); }, 45.0, -122.0, {}, {}, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    mount.set_reject_goto(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 10000));

    require_alpaca_error([&] { driver->slew_to_alt_az_async(91.0, 90.0); }, alpacacore::AlpacaError::InvalidValue);
    CHECK(read_slewing(*driver) == SlewingRead::Threw);

    mount.set_reject_goto(false);
    gate.arm();
    REQUIRE_NOTHROW(driver->slew_to_alt_az_async(30.0, 90.0));
    const bool worker_paused = gate.wait_until_entered();
    if (!worker_paused) gate.release();
    REQUIRE(worker_paused);
    CHECK(read_slewing(*driver) == SlewingRead::True);
    gate.release();
    REQUIRE(wait_until([&] { return mount.count(":MS1#") >= 2; }, 5000));
    REQUIRE(advance_until_not_slewing(*driver, clock));
    driver->set_connected(false);
}

TEST_CASE("iOptron async - a stale worker cannot publish over a newer accepted slew",
          "[ioptron][telescope][async][slewfailure]") {
    alpacacore::test::FakeIoptronMount mount("0012", 0.0);
    REQUIRE(mount.ok());
    WorkerGate gate;
    gate.arm(/*reject_after_release=*/true, /*accept_second=*/true);
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope_with_slew_worker_hook(
        0, endpoint(mount.port()), [&] { gate.before_worker_lock(mount); }, std::nullopt, std::nullopt, {}, {}, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    const bool worker_paused = gate.wait_until_entered();
    if (!worker_paused) gate.release();
    REQUIRE(worker_paused);
    std::exception_ptr retry_error;
    std::thread retry([&] {
        try {
            driver->slew_to_coordinates_async(6.5, 25.0);
        } catch (...) {
            retry_error = std::current_exception();
        }
    });
    const bool newer_target_published = wait_until([&] { return driver->get_target_right_ascension() == 6.5; }, 5000);
    gate.release();
    retry.join();
    REQUIRE(newer_target_published);
    if (retry_error) std::rethrow_exception(retry_error);

    REQUIRE(wait_until([&] { return mount.count(":MS1#") >= 1; }, 5000));
    CHECK(mount.count(":MS1#") == 1);
    REQUIRE(advance_until_not_slewing(*driver, clock));
    driver->set_connected(false);
}

TEST_CASE("iOptron - rejected synchronous GOTO clears the prior async failure",
          "[ioptron][telescope][async][slewfailure]") {
    alpacacore::test::FakeIoptronMount mount("0012", 0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, endpoint(mount.port()), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    mount.set_reject_goto(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 10000));

    try {
        driver->slew_to_coordinates(6.5, 25.0);
        FAIL("Synchronous SlewToCoordinates returned after the mount refused the target");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("Slew rejected by mount") != std::string::npos);
    }
    mount.set_reject_goto(false);
    CHECK(read_slewing(*driver) == SlewingRead::False);
    driver->set_connected(false);
}

TEST_CASE("iOptron - MoveAxis clears a superseded dispatch before its stale worker returns",
          "[ioptron][telescope][async][slewfailure]") {
    alpacacore::test::FakeIoptronMount mount("0012", 0.0);
    REQUIRE(mount.ok());
    WorkerGate gate;
    gate.arm();
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope_with_slew_worker_hook(
        0, endpoint(mount.port()), [&] { gate.before_worker_lock(mount); }, std::nullopt, std::nullopt, {}, {}, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(0.001, 0.0));
    const bool worker_paused = gate.wait_until_entered();
    if (!worker_paused) gate.release();
    REQUIRE(worker_paused);

    REQUIRE_NOTHROW(driver->move_axis(0, 0.0));
    gate.release();
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::False; }, 2000));
    driver->set_connected(false);  // joins the stale dispatch before checking the wire
    CHECK(mount.count(":MS1#") == 0);
    CHECK(mount.count(":Z") == 0);  // a ghost GOTO must not trigger the HAE16 RA trim
}

TEST_CASE("iOptron async - a newer initiator cannot be reaped by an older handoff",
          "[ioptron][telescope][async][slewfailure]") {
    alpacacore::test::FakeIoptronMount mount("0012", 0.0);
    REQUIRE(mount.ok());
    WorkerGate worker_gate;
    worker_gate.arm();
    SlewHandoffGate handoff;
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope_with_slew_worker_hook(
        0, endpoint(mount.port()), [&] { worker_gate.before_worker_lock(mount); }, 45.0, -122.0,
        [&] { handoff.before_reap(); }, [&] { handoff.before_lock(); }, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    std::exception_ptr older_error;
    std::exception_ptr newer_error;
    std::jthread older;
    std::jthread newer;
    struct ReleaseGate {
        SlewHandoffGate& handoff;
        WorkerGate& worker;
        ~ReleaseGate() {
            handoff.release_first();
            worker.release();
        }
    } release_gate{handoff, worker_gate};

    older = std::jthread([&] {
        try {
            driver->slew_to_coordinates_async(5.5, 20.0);
        } catch (...) {
            older_error = std::current_exception();
        }
    });
    REQUIRE(handoff.wait_for_attempts(1, std::chrono::seconds(5)));
    REQUIRE(handoff.wait_for_first_reap(std::chrono::seconds(5)));

    std::atomic<bool> newer_call_started{false};
    newer = std::jthread([&] {
        newer_call_started.store(true, std::memory_order_release);
        try {
            driver->slew_to_coordinates_async(6.5, 25.0);
        } catch (...) {
            newer_error = std::current_exception();
        }
    });
    REQUIRE(wait_until([&] { return newer_call_started.load(std::memory_order_acquire); }, 5000));
    REQUIRE(handoff.wait_for_attempts(2, std::chrono::seconds(5)));
    CHECK_FALSE(handoff.wait_for_reaps(2, std::chrono::milliseconds(100)));
    CHECK(driver->get_target_right_ascension() == 5.5);

    handoff.release_first();
    REQUIRE(handoff.wait_for_reaps(2, std::chrono::seconds(5)));
    REQUIRE(worker_gate.wait_until_entered());
    worker_gate.release();
    older.join();
    newer.join();
    if (older_error) std::rethrow_exception(older_error);
    if (newer_error) std::rethrow_exception(newer_error);

    CHECK(driver->get_target_right_ascension() == 6.5);
    REQUIRE(wait_until([&] { return mount.count(":MS1#") >= 1; }, 5000));
    REQUIRE(advance_until_not_slewing(*driver, clock));
    driver->set_connected(false);
}

// open-astro#763: the driver never rewrites the mount's own limits. A GOTO the
// firmware refuses (user altitude limit +00 here) is reported and not retried
// after lowering :SAL / :SMT.
TEST_CASE("iOptron - a refused GOTO never writes the altitude limit or meridian treatment (#763)",
          "[ioptron][telescope][slewfailure]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, endpoint(mount.port()), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    mount.set_reject_goto(true);
    CHECK_THROWS_AS(driver->slew_to_coordinates(5.5, 20.0), alpacacore::AlpacaException);
    const int goto_attempts = mount.count(":MS1#");
    CHECK(goto_attempts == 1);  // no retry after a limit rewrite

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return mount.count(":MS1#") >= goto_attempts + 1; }, 5000));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 10000));
    CHECK(mount.count(":MS1#") == goto_attempts + 1);

    REQUIRE_NOTHROW(driver->abort_slew());
    driver->set_connected(false);

    CHECK(mount.count(":SAL") == 0);
    CHECK(mount.count(":SMT") == 0);
}

#endif  // _WIN32
