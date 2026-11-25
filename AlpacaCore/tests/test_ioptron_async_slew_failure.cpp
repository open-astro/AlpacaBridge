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
// (a non-null async_ctx), so a mount that answers ":MS1" with "0" (target
// rejected, e.g. below the altitude limit) is logged as "treating as no-op"
// and Slewing drops to false -- the client is told nothing. That soft-fail is
// exactly the "work fails after initiation" case of the completion-property
// contract: the next Slewing read must return an Alpaca error instead
// (contract reference in test_skywatcher_async_slew_failure.cpp).

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"  // settle_connected
#include "driver_rig.h"
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

using alpacacore::test::advance_polling;
using alpacacore::test::age_past_connect_grace;
using alpacacore::test::require_alpaca_error;

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
        advance_polling(clock, std::chrono::milliseconds(100));
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
    mount.set_system_status(0);               // an Alt/Az slew needs Tracking off (#1377)
    age_past_connect_grace(clock);            // past the connect fast-cache grace and the status TTL
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

// Issue #2522 (the #2263 policy): an async GOTO claims both axes, so a MoveAxis on either axis, a jog or a stop, ends
// a dispatch body that has not sent its :MS1# yet. The case waits for the body to return on its own before it stops
// the jog or disconnects, so neither of those is what ends it.
// Falsified by: ioptron_telescope_driver.cpp move_axis_under_lock() claiming nothing for a non-zero rate (the jog
// legs fail), or cancelling nothing for a zero rate (the stop legs fail).
TEST_CASE("iOptron motion owner - a MoveAxis on either axis supersedes an async GOTO not yet dispatched (#2522)",
          "[ioptron][telescope][async][motion-owner]") {
    for (const int axis : {0, 1}) {
        for (const bool jog : {false, true}) {
            CAPTURE(axis, jog);
            alpacacore::test::FakeIoptronMount mount("0012", 0.0);
            REQUIRE(mount.ok());
            WorkerGate gate;
            gate.arm();
            std::atomic<bool> body_returned{false};  // before the driver, whose slot joins the thread that sets it
            alpacacore::test::FakeTaskClock clock;
            auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope_with_slew_worker_hook(
                0, endpoint(mount.port()), [&] { gate.before_worker_lock(mount); }, std::nullopt, std::nullopt, {}, {},
                clock);
            alpacacore::vendor::ioptron::set_slew_spawn_for_testing(*driver, [&](std::function<void()> run) {
                return std::thread([run = std::move(run), &body_returned] {
                    run();
                    body_returned.store(true);
                });
            });
            REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

            REQUIRE_NOTHROW(driver->slew_to_coordinates_async(0.001, 0.0));
            const bool worker_paused = gate.wait_until_entered();
            if (!worker_paused) gate.release();
            REQUIRE(worker_paused);

            REQUIRE_NOTHROW(driver->move_axis(axis, jog ? driver->get_axis_rate_range(axis).first : 0.0));
            gate.release();
            REQUIRE(wait_until([&] { return body_returned.load(); }, 5000));
            CHECK(mount.count(":MS1#") == 0);
            CHECK(mount.count(":Z") == 0);  // no ghost GOTO, so no HAE16 RA trim either
            if (jog) driver->move_axis(axis, 0.0);
            driver->set_connected(false);
        }
    }
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

    mount.set_reject_normal_goto(true);  // :MS2# would be accepted: a re-aim shows (#2395)
    require_alpaca_error([&] { driver->slew_to_coordinates(5.5, 20.0); }, alpacacore::AlpacaError::DriverException);
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
    CHECK(mount.count(":MS2#") == 0);
}

namespace {

// Runs `call` on another thread and steps the fake clock until it returns, so a synchronous slew (which waits on the
// task clock) ends instead of hanging. Each step lets the poll finish its tick, and `on_step` (if any) sees the virtual
// time stepped so far. Returns what it threw, or null.
std::exception_ptr run_stepping_clock(alpacacore::test::FakeTaskClock& clock, const std::function<void()>& call,
                                      const std::function<void(std::chrono::nanoseconds)>& on_step = {}) {
    const auto began = clock.now();
    auto done = std::async(std::launch::async, [&]() -> std::exception_ptr {
        try {
            call();
        } catch (...) {
            return std::current_exception();
        }
        return nullptr;
    });
    for (int i = 0; i < 2000 && done.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready; ++i) {
        advance_polling(clock, std::chrono::milliseconds(500));
        if (on_step) on_step(clock.now() - began);
    }
    return done.get();
}

std::string message_of(const std::exception_ptr& error, int* code = nullptr) {
    try {
        if (error) std::rethrow_exception(error);
    } catch (const alpacacore::AlpacaException& ex) {
        if (code) *code = ex.error_code();
        return ex.what();
    } catch (const std::exception& ex) {
        return ex.what();
    }
    return "";
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// Captures the iOptron log lines with their level, for decision 0007 rule 9 checks.
class IoptronLogCapture {
public:
    IoptronLogCapture() {
        alpacacore::logging::set_log_level(alpacacore::logging::LogLevel::Info);
        alpacacore::logging::set_log_sink(
            [this](alpacacore::logging::LogLevel level, std::string_view component, std::string_view message) {
                if (component != "iOptron") return;
                std::lock_guard<std::mutex> lock(mutex_);
                lines_.push_back({level, std::string(message)});
            });
    }
    ~IoptronLogCapture() {
        alpacacore::logging::set_log_sink(previous_sink_);
        alpacacore::logging::set_log_level(previous_level_);
    }
    IoptronLogCapture(const IoptronLogCapture&) = delete;
    IoptronLogCapture& operator=(const IoptronLogCapture&) = delete;
    int count(alpacacore::logging::LogLevel level, const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(std::count_if(lines_.begin(), lines_.end(), [&](const auto& line) {
            return line.first == level && line.second.find(text) != std::string::npos;
        }));
    }

private:
    alpacacore::logging::LogSink previous_sink_ = alpacacore::logging::get_log_sink();
    alpacacore::logging::LogLevel previous_level_ = alpacacore::logging::get_log_level();
    std::mutex mutex_;
    std::vector<std::pair<alpacacore::logging::LogLevel, std::string>> lines_;
};

// A synthetic test site (the equator at Greenwich), for the Alt/Az conversions only.
std::unique_ptr<alpacacore::TelescopeDriver> safety_driver(alpacacore::test::FakeIoptronMount& mount,
                                                           alpacacore::test::FakeTaskClock& clock) {
    return alpacacore::vendor::ioptron::create_ioptron_telescope_with_site(0, endpoint(mount.port()), 0.0, 0.0,
                                                                           std::nullopt, false, clock);
}

}  // namespace

// Issue #2395 (AB-1): a GOTO the firmware refuses on :MS1# is an error from every initiator, and the driver never
// re-sends it as the counterweight-up :MS2#. In this fake mode :MS2# would be accepted and move the mount, so a
// regression shows as motion as well as in the command log.
TEST_CASE("iOptron goto-safety - a GOTO refused on :MS1# is never re-aimed counterweight-up (:MS2#)",
          "[ioptron][telescope][slewfailure][goto-safety]") {
    struct Initiator {
        const char* name;
        bool alt_az;
        bool async;
        std::function<void(alpacacore::TelescopeDriver&)> call;
    };
    const auto set_target = [](alpacacore::TelescopeDriver& d) {
        d.set_target_right_ascension(5.5);
        d.set_target_declination(20.0);
    };
    const std::vector<Initiator> initiators = {
        {"SlewToCoordinates", false, false, [](auto& d) { d.slew_to_coordinates(5.5, 20.0); }},
        {"SlewToCoordinatesAsync", false, true, [](auto& d) { d.slew_to_coordinates_async(5.5, 20.0); }},
        {"SlewToTarget", false, false,
         [&](auto& d) {
             set_target(d);
             d.slew_to_target();
         }},
        {"SlewToTargetAsync", false, true,
         [&](auto& d) {
             set_target(d);
             d.slew_to_target_async();
         }},
        {"SlewToAltAz", true, false, [](auto& d) { d.slew_to_alt_az(30.0, 90.0); }},
        {"SlewToAltAzAsync", true, true, [](auto& d) { d.slew_to_alt_az_async(30.0, 90.0); }},
    };
    for (const auto& initiator : initiators) {
        CAPTURE(initiator.name);
        alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
        REQUIRE(mount.ok());
        mount.set_position(1.0, 10.0);
        mount.set_reject_normal_goto(true);
        alpacacore::test::FakeTaskClock clock;
        auto driver = safety_driver(mount, clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        if (initiator.alt_az) {
            mount.set_system_status(0);               // an Alt/Az slew needs Tracking off (#1377)
            age_past_connect_grace(clock);            // past the connect fast-cache grace and the status TTL
        }

        IoptronLogCapture logs;
        const std::exception_ptr error = run_stepping_clock(clock, [&] { initiator.call(*driver); });
        if (initiator.async) {
            CHECK_FALSE(error);
            std::string message;
            REQUIRE(wait_until([&] { return read_slewing(*driver, &message) == SlewingRead::Threw; }, 10000));
            // SlewToTargetAsync reports under the SlewToCoordinatesAsync label it forwards to.
            CHECK(contains(message, " failed: mount rejected the target"));
            // Decision 0007 rule 9: the client chose the target, so the refusal is INFO, never WARN.
            CHECK(logs.count(alpacacore::logging::LogLevel::Info, " failed: mount rejected the target") == 1);
            CHECK(logs.count(alpacacore::logging::LogLevel::Warn, "mount rejected the target") == 0);
        } else {
            int code = 0;
            const std::string message = message_of(error, &code);
            CHECK(code == alpacacore::AlpacaError::DriverException);
            CHECK(contains(message, "Slew rejected by mount"));
        }
        CHECK(mount.count(":MS1#") == 1);
        CHECK(mount.count(":MS2#") == 0);
        CHECK(mount.ra_hours() == Catch::Approx(1.0));  // nothing moved
        CHECK(mount.dec_degrees() == Catch::Approx(10.0));
        driver->set_connected(false);
    }
}

// Issue #2396 (AB-2): no reply to :MS1#, or a reply that is neither "1" nor "0", is not acceptance. When the status
// read after it shows no slew the GOTO fails, synchronous and async alike, and the mount is sent :Q#, so a GOTO
// that did arrive is stopped.
TEST_CASE("iOptron goto-safety - a GOTO with no reply that the mount did not start fails and stops the mount",
          "[ioptron][telescope][slewfailure][goto-safety]") {
    for (const std::string reply : {"", "x"}) {
        for (const bool async : {false, true}) {
            CAPTURE(reply, async);
            alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
            REQUIRE(mount.ok());
            mount.set_position(1.0, 10.0);
            alpacacore::test::FakeTaskClock clock;
            auto driver = safety_driver(mount, clock);
            REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
            mount.set_goto_reply(reply, /*moves=*/false);
            const int stops_before = mount.count(":Q#");

            const std::exception_ptr error = run_stepping_clock(clock, [&] {
                if (async) {
                    driver->slew_to_coordinates_async(5.5, 20.0);
                } else {
                    driver->slew_to_coordinates(5.5, 20.0);
                }
            });
            std::string message;
            if (async) {
                CHECK_FALSE(error);
                REQUIRE(wait_until([&] { return read_slewing(*driver, &message) == SlewingRead::Threw; }, 10000));
            } else {
                int code = 0;
                message = message_of(error, &code);
                CHECK(code == alpacacore::AlpacaError::DriverException);
            }
            CHECK(contains(message, reply.empty() ? "gave no reply to the GOTO" : "gave an unknown reply to the GOTO"));
            CHECK(contains(message, "the mount was sent a stop"));
            CHECK(mount.count(":MS1#") == 1);
            CHECK(mount.count(":Q#") == stops_before + 1);
            CHECK(mount.ra_hours() == Catch::Approx(1.0));
            driver->set_connected(false);
        }
    }
}

// Issue #2396 (AB-2): a GOTO the mount runs although its :MS1# reply was lost is confirmed by the status read
// after it (2, slewing): no stop, and Slewing reads true.
TEST_CASE("iOptron goto-safety - a GOTO with no reply that the mount runs is confirmed by its status",
          "[ioptron][telescope][slewfailure][goto-safety]") {
    alpacacore::test::FakeIoptronMount mount("0036", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    mount.set_goto_tracks_off(true);  // an accepted GOTO reads status 2 until it is stopped
    alpacacore::test::FakeTaskClock clock;
    auto driver = safety_driver(mount, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    mount.set_goto_reply("", /*moves=*/true);
    const int stops_before = mount.count(":Q#");
    const int status_reads = mount.count(":GLS#");

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    // One :GLS# before the :MS1# (was the mount already moving?) and one after it (is it moving now?).
    REQUIRE(wait_until([&] { return mount.count(":MS1#") >= 1 && mount.count(":GLS#") > status_reads + 1; }, 10000));
    advance_polling(clock, std::chrono::seconds(6));  // past the dispatch window: the mount's status decides
    CHECK(read_slewing(*driver) == SlewingRead::True);
    CHECK(mount.count(":Q#") == stops_before);
    driver->abort_slew();
    driver->set_connected(false);
}

// Issue #2396 (AB-2): a status of 2 after an unanswered :MS1# confirms nothing when the mount was already moving
// before it (an earlier GOTO being retargeted, a Park, a #843 pulse that reads 2): the 2 may be that older motion. The
// GOTO fails and the mount is sent :Q#, not left to finish the old motion and read as landed at the new target.
TEST_CASE("iOptron goto-safety - an unanswered GOTO sent while the mount was already moving is not confirmed",
          "[ioptron][telescope][slewfailure][goto-safety]") {
    for (const bool async : {false, true}) {
        CAPTURE(async);
        alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
        REQUIRE(mount.ok());
        mount.set_position(1.0, 10.0);
        alpacacore::test::FakeTaskClock clock;
        auto driver = safety_driver(mount, clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        mount.set_system_status(2);  // earlier motion still running
        mount.set_goto_reply("", /*moves=*/false);
        const int stops_before = mount.count(":Q#");

        const std::exception_ptr error = run_stepping_clock(clock, [&] {
            if (async) {
                driver->slew_to_coordinates_async(5.5, 20.0);
            } else {
                driver->slew_to_coordinates(5.5, 20.0);
            }
        });
        std::string message;
        if (async) {
            CHECK_FALSE(error);
            REQUIRE(wait_until([&] { return mount.count(":Q#") > stops_before; }, 10000));
            mount.set_system_status(1);  // the old motion ends
            REQUIRE(wait_until(
                [&] {
                    advance_polling(clock, std::chrono::milliseconds(100));
                    return read_slewing(*driver, &message) == SlewingRead::Threw;
                },
                10000));
        } else {
            int code = 0;
            message = message_of(error, &code);
            CHECK(code == alpacacore::AlpacaError::DriverException);
        }
        CHECK(contains(message, "gave no reply to the GOTO"));
        CHECK(contains(message, "the mount was sent a stop"));
        CHECK(mount.count(":Q#") == stops_before + 1);
        CHECK(mount.ra_hours() == Catch::Approx(1.0));
        mount.set_system_status(1);
        driver->set_connected(false);
    }
}

// Issue #2396 (AB-7 landing gate): a synchronous GOTO whose position settles far from the target (over 600 arcsec)
// is an error, not "position stabilized but target not reached"; a settle inside that is still a landing.
TEST_CASE("iOptron goto-safety - a synchronous GOTO that settles far from the target fails",
          "[ioptron][telescope][slewfailure][goto-safety]") {
    for (const double landing_error : {3600.0, 300.0}) {
        CAPTURE(landing_error);
        // HEM27: no final-approach trim, so the landing error stands.
        alpacacore::test::FakeIoptronMount mount("0025", landing_error);
        REQUIRE(mount.ok());
        alpacacore::test::FakeTaskClock clock;
        auto driver = safety_driver(mount, clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

        const int stops_before = mount.count(":Q#");
        const std::exception_ptr error = run_stepping_clock(clock, [&] { driver->slew_to_coordinates(5.5, 20.0); });
        if (landing_error > 600.0) {
            int code = 0;
            const std::string message = message_of(error, &code);
            CHECK(code == alpacacore::AlpacaError::DriverException);
            CHECK(contains(message, "SlewToCoordinates failed: the mount stopped"));
            CHECK(contains(message, "the mount was sent a stop"));
            CHECK(mount.count(":Q#") == stops_before + 1);
        } else {
            CHECK_FALSE(error);
        }
        CHECK(read_slewing(*driver) == SlewingRead::False);
        driver->set_connected(false);
    }
}

// Issue #2396: a synchronous GOTO the mount never finishes (status 2 past the 300 s bound) fails, and the mount is
// sent :Q# so it does not keep moving after the throw. The slew state, hold included, ends with it.
TEST_CASE("iOptron goto-safety - a synchronous GOTO that times out stops the mount",
          "[ioptron][telescope][slewfailure][goto-safety]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    mount.set_goto_tracks_off(true);  // the accepted GOTO reads status 2 (slewing) until it is stopped
    alpacacore::test::FakeTaskClock clock;
    auto driver = safety_driver(mount, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    const int stops_before = mount.count(":Q#");

    const std::exception_ptr error = run_stepping_clock(clock, [&] { driver->slew_to_coordinates(5.5, 20.0); });
    int code = 0;
    const std::string message = message_of(error, &code);
    CHECK(code == alpacacore::AlpacaError::DriverException);
    CHECK(contains(message, "SlewToCoordinates timed out"));
    CHECK(contains(message, "the mount was sent a stop"));
    CHECK(mount.count(":Q#") == stops_before + 1);
    CHECK(read_slewing(*driver) == SlewingRead::False);
    driver->set_connected(false);
}

// Issue #2396: a synchronous wait that another client's GOTO superseded does not own the mount. When it times out,
// it sends no :Q# and leaves the newer GOTO's state alone: the newer GOTO keeps slewing.
TEST_CASE("iOptron goto-safety - a superseded synchronous GOTO that times out does not stop the newer GOTO",
          "[ioptron][telescope][slewfailure][goto-safety]") {
    alpacacore::test::FakeIoptronMount mount("0012", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    mount.set_goto_tracks_off(true);  // an accepted GOTO reads status 2 (slewing) until it is stopped
    alpacacore::test::FakeTaskClock clock;
    auto driver = safety_driver(mount, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    const int stops_before = mount.count(":Q#");
    // The newer async GOTO starts with the synchronous one, so its own 300 s bound (#2396 AB-8, the motion bound
    // watchdog) would stop it at the same moment. Refusing that slot leaves this case about the waiter alone.
    alpacacore::vendor::ioptron::set_motion_bound_watchdog_spawn_for_testing(
        *driver, [](std::function<void()>) -> std::thread {
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        });

    const std::exception_ptr error = run_stepping_clock(clock, [&] {
        auto newer = std::async(std::launch::async, [&] {
            // No REQUIRE off the test thread; the count check below catches a GOTO that never went out.
            if (wait_until([&] { return mount.count(":MS1#") == 1; }, 10000)) {
                driver->slew_to_coordinates_async(6.0, 25.0);  // another client
            }
        });
        driver->slew_to_coordinates(5.5, 20.0);
    });
    int code = 0;
    const std::string message = message_of(error, &code);
    CHECK(code == alpacacore::AlpacaError::DriverException);
    CHECK(contains(message, "SlewToCoordinates timed out"));
    CHECK(contains(message, "a newer motion owns the mount, so no stop was sent"));
    CHECK(mount.count(":MS1#") == 2);
    CHECK(mount.count(":Q#") == stops_before);
    CHECK(read_slewing(*driver) == SlewingRead::True);
    driver->abort_slew();
    driver->set_connected(false);
}

// Issue #2396: a failed position read is no landing sample, and three in a row fail a synchronous slew. The mount is
// sent :Q# once, so it does not keep moving after the throw.
TEST_CASE("iOptron goto-safety - a synchronous GOTO whose landing position cannot be read fails and stops the mount",
          "[ioptron][telescope][slewfailure][goto-safety]") {
    // HEM27: no final-approach trim, so the wait goes straight to its position reads.
    alpacacore::test::FakeIoptronMount mount("0025", /*landing_ra_error_arcsec=*/0.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = safety_driver(mount, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    const int stops_before = mount.count(":Q#");

    // The poll reads the position too, and three failed polls would latch a fault that Slewing reports before the wait
    // reaches its landing reads. So the poll is paused and ticked by hand: once the 5 s hold is over, one tick samples
    // the stopped mount (Slewing ends), and only then do the position reads start to fail, for the wait's own three
    // direct reads alone.
    alpacacore::vendor::ioptron::set_state_poll_paused_for_testing(*driver, true);
    bool armed = false;
    const std::exception_ptr error = run_stepping_clock(
        clock, [&] { driver->slew_to_coordinates(5.5, 20.0); },
        [&](std::chrono::nanoseconds stepped) {
            if (!armed && stepped >= std::chrono::seconds(6)) {
                armed = true;
                alpacacore::vendor::ioptron::poll_state_now_for_testing(*driver);
                mount.fail_next_position_reads(3);
            }
        });
    int code = 0;
    const std::string message = message_of(error, &code);
    CHECK(code == alpacacore::AlpacaError::DriverException);
    CHECK(contains(message, "the mount position could not be read to confirm the landing"));
    CHECK(contains(message, "the mount was sent a stop"));
    CHECK(mount.count(":Q#") == stops_before + 1);
    driver->set_connected(false);
}

// Issue #2396: Slewing goes false, but the position keeps moving (over 30 arcsec a poll, so never stable) and never
// comes within 60 arcsec of the target. The 300 s bound of the stability wait, on the task clock, fails the slew and
// sends :Q#.
TEST_CASE("iOptron goto-safety - a synchronous GOTO whose position never settles times out and stops the mount",
          "[ioptron][telescope][slewfailure][goto-safety]") {
    // HEM27: no final-approach trim; the mount lands 3600 arcsec off and drifts further away on every read.
    alpacacore::test::FakeIoptronMount mount("0025", /*landing_ra_error_arcsec=*/3600.0);
    REQUIRE(mount.ok());
    alpacacore::test::FakeTaskClock clock;
    auto driver = safety_driver(mount, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    mount.set_position_drift_per_read(100.0);
    const int stops_before = mount.count(":Q#");

    const std::exception_ptr error = run_stepping_clock(clock, [&] { driver->slew_to_coordinates(5.5, 20.0); });
    int code = 0;
    const std::string message = message_of(error, &code);
    CHECK(code == alpacacore::AlpacaError::DriverException);
    CHECK(contains(message, "SlewToCoordinates timed out waiting for the mount to land on the target"));
    CHECK(contains(message, "the mount was sent a stop"));
    CHECK(mount.count(":Q#") == stops_before + 1);
    mount.set_position_drift_per_read(0.0);
    driver->set_connected(false);
}

#endif  // _WIN32
