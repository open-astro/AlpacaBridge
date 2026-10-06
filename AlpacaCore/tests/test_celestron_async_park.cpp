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
// the ConformU 4.5 STANDARD 1 s target while the park slew runs in the
// background, Slewing stays true until the mount arrives, and AtPark flips
// true in the same step Slewing drops. A FakeMountServer plays a Celestron
// NexStar handset whose GOTO takes ~1.5 s, so the whole lifecycle runs hardware-free.
#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/celestron/celestron_protocol_wrapper.h>
#include <alpacacore/vendor/celestron/celestron_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_mount_server.h"

namespace {

using Clock = std::chrono::steady_clock;

struct FakeCelestronState {
    std::atomic<bool> goto_seen{false};
    std::atomic<int> goto_count{0};
    std::atomic<int> level_start_count{0};
    std::atomic<Clock::rep> goto_started{0};
    // #742: `silent` makes the handset answer nothing (every command times
    // out, the stops included); `silent_from_goto` flips it on at the next
    // GOTO, which then goes unanswered too, so a park dispatch fails.
    std::atomic<bool> silent{false};
    std::atomic<bool> silent_from_goto{false};
    // #832: `tracking_on` makes the 't' query answer EQ-North so the completion
    // tail has a tracking restore to perform; `t_count` counts every 'T' write.
    std::atomic<bool> tracking_on{false};
    std::atomic<int> t_count{0};
    static constexpr auto kGotoDuration = std::chrono::milliseconds(1500);
    bool goto_in_progress() const {
        if (!goto_seen.load()) return false;
        const auto started = Clock::time_point(Clock::duration(goto_started.load()));
        return Clock::now() - started < kGotoDuration;
    }
};

alpacacore::test::FakeMountServer::Responder celestron_responder(std::shared_ptr<FakeCelestronState> st) {
    return [st](const std::string& chunk) -> std::string {
        if (st->silent.load()) return "";
        if (chunk.empty()) return "0#";
        switch (chunk[0]) {
            case 'e':
            case 'E':
            case 'z':
            case 'Z':
                return "12AB0500,20000500#";  // parseable 16/24-bit position pair
            case 'r':
            case 'R':
            case 'b':
            case 'B':
                if (st->silent_from_goto.load()) {
                    st->silent.store(true);
                    return "";
                }
                st->goto_started.store(Clock::now().time_since_epoch().count());
                st->goto_seen.store(true);
                st->goto_count.fetch_add(1);
                return "#";
            case 'L':
                return st->goto_in_progress() ? "1#" : "0#";
            case 'M':  // cancel goto
                st->goto_seen.store(false);
                return "#";
            case 'J':  // alignment complete (the slew-safety gate requires it)
                return "1#";
            case 'T':  // tracking mode write
                st->t_count.fetch_add(1);
                return "#";
            case 't':  // tracking mode query
                return st->tracking_on.load() ? std::string("\x02#") : std::string("\x00#", 2);
            case 'P': {  // AUX passthrough: P len dev op ...
                const unsigned char op = chunk.size() > 3 ? static_cast<unsigned char>(chunk[3]) : 0;
                if (op == 0x0B) {
                    st->level_start_count.fetch_add(1);
                    return "#";
                }
                if (op == 0x02 || op == 0x17) {  // MC_GOTO_FAST / MC_GOTO_SLOW
                    if (st->silent_from_goto.load()) {
                        st->silent.store(true);
                        return "";
                    }
                    st->goto_started.store(Clock::now().time_since_epoch().count());
                    st->goto_seen.store(true);
                    st->goto_count.fetch_add(1);
                    return "#";
                }
                if (op == 0x13) {  // MC_SLEW_DONE: 0x00 = still slewing, 0xFF = done
                    return st->goto_in_progress() ? std::string("\x00#", 2) : std::string("\xFF#");
                }
                return std::string("\xFF#");
            }
            default:
                return "0#";
        }
    };
}

alpacacore::vendor::celestron::ConnectionInfo endpoint(int port) {
    alpacacore::vendor::celestron::ConnectionInfo info;
    info.type = alpacacore::vendor::celestron::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = 200;
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

TEST_CASE("Celestron async - Park returns immediately, AtPark flips when the slew ends",
          "[celestron][telescope][async]") {
    auto st = std::make_shared<FakeCelestronState>();
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE_FALSE(driver->get_at_park());

    const auto t0 = Clock::now();
    driver->park();
    const auto park_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    CHECK(park_ms < 1000);           // ConformU 4.5 STANDARD target for an async initiator
    REQUIRE(driver->get_slewing());  // parking reports Slewing until AtPark
    REQUIRE_FALSE(driver->get_at_park());
    REQUIRE(wait_until([&] { return st->goto_seen.load(); }, 5000));  // the GOTO was dispatched
    // Park while a park is in flight is a no-op: the running slew is neither
    // cancelled nor restarted (still exactly one GOTO on the wire).
    driver->park();
    REQUIRE(driver->get_slewing());
    REQUIRE_FALSE(driver->get_at_park());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(st->goto_count.load() == 1);

    REQUIRE(wait_until([&] { return driver->get_at_park(); }, 20000));
    REQUIRE_FALSE(driver->get_slewing());
    REQUIRE_FALSE(driver->get_tracking());  // park stops tracking

    driver->park();  // second Park on a parked mount is harmless
    REQUIRE(driver->get_at_park());
    REQUIRE_FALSE(driver->get_slewing());

    driver->unpark();
    REQUIRE_FALSE(driver->get_at_park());
    driver->set_connected(false);
}

TEST_CASE("Celestron async - Unpark during a park cancels it", "[celestron][telescope][async]") {
    auto st = std::make_shared<FakeCelestronState>();
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->park();
    REQUIRE(driver->get_slewing());
    driver->unpark();
    REQUIRE_FALSE(driver->get_at_park());
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 5000));
    // The cancelled park task must never flip AtPark afterwards.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    REQUIRE_FALSE(driver->get_at_park());

    // A park in flight gates motion members like a completed park does, so a
    // slew or axis jog cannot silently clobber it (ParkedException, 0x408).
    driver->park();
    REQUIRE(driver->get_slewing());
    CHECK_THROWS_AS(driver->move_axis(0, 0.5), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->slew_to_coordinates_async(5.0, 20.0), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->sync_to_coordinates(5.0, 20.0), alpacacore::AlpacaException);
    try {
        driver->move_axis(0, 0.5);
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidWhileParked);
    }
    REQUIRE(driver->get_slewing());  // the park is still in flight
    driver->abort_slew();            // ...but AbortSlew may cancel it
    REQUIRE_FALSE(driver->get_at_park());
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 5000));

    // Disconnect with a park in flight joins the task cleanly.
    driver->park();
    driver->set_connected(false);
    REQUIRE_FALSE(driver->get_connected());
}

TEST_CASE("Celestron FindHome - refuses during Park without replacing the park task",
          "[celestron][telescope][async][home]") {
    auto st = std::make_shared<FakeCelestronState>();
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->park();
    REQUIRE(wait_until([&] { return st->goto_seen.load(); }, 5000));
    try {
        driver->find_home();
        FAIL("FindHome was accepted while Park was in progress");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidWhileParked);
    }
    CHECK(st->level_start_count.load() == 0);
    CHECK(driver->get_slewing());
    CHECK(wait_until([&] { return driver->get_at_park(); }, 20000));
    CHECK(st->goto_count.load() == 1);
    driver->unpark();
    driver->set_connected(false);
}

// #742: the three stops Unpark sends to cancel a park in flight (cancel GOTO,
// then both axes to rate 0) sat in one empty catch, and Unpark then set
// Slewing false and returned success. With a handset that answers nothing,
// Unpark must throw once the park task is joined, and Slewing must not read
// false: the park slew may still be running.
TEST_CASE("Celestron async - Unpark reports stops a silent handset never answered", "[celestron][telescope][async]") {
    auto st = std::make_shared<FakeCelestronState>();
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->park();
    REQUIRE(driver->get_slewing());
    REQUIRE(wait_until([&] { return st->goto_seen.load(); }, 5000));  // the park slew is on the wire
    st->silent.store(true);

    try {
        driver->unpark();
        FAIL("Unpark returned success although no stop was answered");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).rfind("Unpark could not stop the park slew: ", 0) == 0);
    }
    CHECK_FALSE(driver->get_at_park());
    CHECK(driver->get_slewing());  // the hardware poll fails too, so the cached state stands

    driver->set_connected(false);
}

// #742: when the park task fails (here the GOTO itself goes unanswered) it
// stops the mount; those stops were swallowed and Slewing set false. A stop
// that fails must be logged at ERROR, and Slewing must not read false while
// the handset answers nothing.
TEST_CASE("Celestron async - a failed park logs the stops a silent handset never answered",
          "[celestron][telescope][async]") {
    std::atomic<int> stop_errors{0};
    struct SinkGuard {
        alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
        ~SinkGuard() { alpacacore::logging::set_log_sink(previous); }
    } sink_guard;
    alpacacore::logging::set_log_sink(
        [&](alpacacore::logging::LogLevel level, std::string_view component, std::string_view message) {
            if (level == alpacacore::logging::LogLevel::Error && component == "Celestron" &&
                message.find("stop after park failure failed: ") != std::string_view::npos &&
                message.find("the mount may still be moving") != std::string_view::npos) {
                ++stop_errors;
            }
        });

    auto st = std::make_shared<FakeCelestronState>();
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->silent_from_goto.store(true);
    driver->park();
    REQUIRE(driver->get_slewing());
    REQUIRE(wait_until([&] { return stop_errors.load() > 0; }, 10000));
    CHECK(stop_errors.load() == 1);
    CHECK(st->silent.load());  // the handset is still silent
    CHECK_FALSE(driver->get_at_park());
    CHECK(driver->get_slewing());

    driver->set_connected(false);
}

// #832: a synchronous SlewToCoordinates must notice another client taking the
// mount (AbortSlew, Park, MoveAxis) and throw InvalidOperation instead of
// returning success, and must not run its completion tail (the T2 tracking
// restore) over the superseding motion.
TEST_CASE("Celestron sync slew - superseded by AbortSlew, Park or MoveAxis throws InvalidOperation",
          "[celestron][telescope][async]") {
    using alpacacore::AlpacaException;
    struct Case {
        const char* name;
        std::function<void(alpacacore::TelescopeDriver&)> supersede;
    };
    const Case cases[] = {
        {"AbortSlew", [](alpacacore::TelescopeDriver& d) { d.abort_slew(); }},
        {"Park", [](alpacacore::TelescopeDriver& d) { d.park(); }},
        {"MoveAxis", [](alpacacore::TelescopeDriver& d) { d.move_axis(0, 0.5); }},
    };
    for (const auto& c : cases) {
        INFO(c.name);
        auto st = std::make_shared<FakeCelestronState>();
        st->tracking_on.store(true);
        alpacacore::test::FakeMountServer server(celestron_responder(st));
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

        std::atomic<bool> done{false};
        std::atomic<int> code{-1};
        std::atomic<int> t_at_exit{-1};
        std::thread slewer([&] {
            try {
                driver->slew_to_coordinates(5.0, 20.0);
                code.store(0);
            } catch (const AlpacaException& e) {
                code.store(static_cast<int>(e.error_code()));
            }
            t_at_exit.store(st->t_count.load());
            done.store(true);
        });
        REQUIRE(wait_until([&] { return st->goto_seen.load(); }, 5000));  // the GOTO is on the wire
        c.supersede(*driver);
        const int t_after_supersede = st->t_count.load();
        slewer.join();
        CHECK(code.load() == static_cast<int>(alpacacore::AlpacaError::InvalidOperation));
        CHECK(t_at_exit.load() == t_after_supersede);  // no tracking restore after the supersede
        driver->set_connected(false);
    }
}

TEST_CASE("Celestron sync slew - uncontended slew returns and runs the tracking restore",
          "[celestron][telescope][async]") {
    auto st = std::make_shared<FakeCelestronState>();
    st->tracking_on.store(true);
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    const int t_before = st->t_count.load();
    REQUIRE_NOTHROW(driver->slew_to_coordinates(5.0, 20.0));
    CHECK(st->goto_count.load() == 1);
    CHECK(st->t_count.load() > t_before);  // the completion tail restored tracking
    driver->set_connected(false);
}

#endif  // !_WIN32
