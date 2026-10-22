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

// open-astro#575, ZWO: the GOTO setup thread sends ":MS" after the initiator
// returned; a mount that rejects it ("Mount rejected GOTO request") was logged
// as "GOTO failed" and Slewing simply dropped to false once slew_force_until_
// expired. The next Slewing read must return an Alpaca error instead
// (contract reference in test_skywatcher_async_slew_failure.cpp).

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/zwo/zwo_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"  // settle_connected
#include "fake_mount_server.h"
#include "fake_task_clock.h"

namespace {

using Clock = std::chrono::steady_clock;

struct FakeZwoState {
    std::atomic<bool> reject_goto{false};
    std::atomic<int> goto_count{0};
    // Delay the ":Sr" ack (still inside the 250 ms reply timeout) so a test
    // can land AbortSlew while the GOTO setup thread is mid-setup.
    std::atomic<int> sr_delay_ms{0};
    // Answer ":Sr" with "0" (rejected) instead of the "1" ack.
    std::atomic<bool> reject_sr{false};
    std::atomic<int> sr_count{0};  // ":Sr" received (counted before the delay)
    // Delay the ":SMTI" ack the same way, to land a cancel inside the
    // site/time sync that precedes the target write.
    std::atomic<int> smti_delay_ms{0};
    std::atomic<int> smti_count{0};  // ":SMTI" received (counted before the delay)
    std::atomic<int> sd_count{0};    // ":Sd" received
    // Every command only the GOTO setup thread sends (site read, site/time
    // write, target write), counted on receipt: open-astro#720 requires that
    // none arrives after the call that fences the GOTO has returned.
    std::atomic<int> setup_count{0};
    // open-astro#720: what the initiators that must fence a pending GOTO put
    // on the wire. The mount's own traffic (poll thread) sends none of these.
    std::atomic<int> park_count{0};  // ":hP#"
    std::atomic<int> home_count{0};  // ":hC#"
    std::atomic<int> stop_count{0};  // ":Q#" (not the per-direction ":Qe#" etc.)
    std::atomic<int> jog_count{0};   // ":Me#" / ":Mw#" / ":Mn#" / ":Ms#"
    // Answer ":GU" as a stopped mount and ":GR"/":GD" with a fixed position
    // (12h, +45 deg), so Slewing and RA/Dec reads reach the mount's answer.
    std::atomic<bool> serve_position{true};
    // What ":GAT" answers: tracking on (the default) or off.
    std::atomic<bool> tracking_on{true};
    // A GOTO in flight that reports no tracking: ":GU" answers "n#" (not
    // tracking, no 'N': slewing) and ":GAT" "0#".
    std::atomic<bool> goto_not_tracking{false};
};

// A chunk can carry more than one command, so count occurrences.
int count_command(const std::string& chunk, const std::string& command) {
    int count = 0;
    for (auto pos = chunk.find(command); pos != std::string::npos; pos = chunk.find(command, pos + command.size())) {
        ++count;
    }
    return count;
}

// LX200-style AM5 dialect: ":Sr"/":Sd"/":SMTI" are acked with a bare "1",
// ":MS" answers "0" (accepted) or a non-zero code (rejected), ":GAT" reports
// tracking, and "0#" is a validly terminated reply for everything else.
alpacacore::test::FakeMountServer::Responder zwo_responder(const std::shared_ptr<FakeZwoState>& st) {
    return [st](const std::string& chunk) -> std::string {
        st->park_count.fetch_add(count_command(chunk, ":hP#"));
        st->home_count.fetch_add(count_command(chunk, ":hC#"));
        st->stop_count.fetch_add(count_command(chunk, ":Q#"));
        for (const char* jog : {":Me#", ":Mw#", ":Mn#", ":Ms#"}) {
            st->jog_count.fetch_add(count_command(chunk, jog));
        }
        if (chunk.find(":MS") != std::string::npos) {
            st->goto_count.fetch_add(1);
            return st->reject_goto.load() ? "2" : "0";
        }
        for (const char* setup : {":GMGE", ":SMGE", ":SMTI", ":Sr", ":Sd"}) {
            st->setup_count.fetch_add(count_command(chunk, setup));
        }
        st->sr_count.fetch_add(count_command(chunk, ":Sr"));
        st->sd_count.fetch_add(count_command(chunk, ":Sd"));
        st->smti_count.fetch_add(count_command(chunk, ":SMTI"));
        if (chunk.find(":SMTI") != std::string::npos && st->smti_delay_ms.load() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(st->smti_delay_ms.load()));
        }
        if (chunk.find(":Sr") != std::string::npos && st->sr_delay_ms.load() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(st->sr_delay_ms.load()));
        }
        if (chunk.find(":Sr") != std::string::npos && st->reject_sr.load()) {
            return "0";
        }
        if (chunk.find(":Sr") != std::string::npos || chunk.find(":Sd") != std::string::npos ||
            chunk.find(":SMTI") != std::string::npos) {
            return "1";
        }
        if (chunk.find(":GAT") != std::string::npos) {
            return st->tracking_on.load() && !st->goto_not_tracking.load() ? "1#" : "0#";
        }
        if (st->goto_not_tracking.load() && chunk.find(":GU") != std::string::npos) {
            return "n#";
        }
        // ":GU" carries the tracking flag too: 'n' is "not tracking", 'N' "not slewing". The status poll
        // refreshes the cached Tracking state from it, so the fake must agree with ":GAT".
        if (!st->tracking_on.load() && chunk.find(":GU") != std::string::npos) {
            return "nN#";
        }
        if (st->serve_position.load()) {
            if (chunk.find(":GU") != std::string::npos) return "N#";
            if (chunk.find(":GR") != std::string::npos) return "12:00:00#";
            if (chunk.find(":GD") != std::string::npos) return "+45*00:00#";
        }
        return "0#";
    };
}

alpacacore::vendor::zwo::ConnectionInfo endpoint(int port) {
    alpacacore::vendor::zwo::ConnectionInfo conn;
    conn.type = alpacacore::vendor::zwo::ConnectionType::Network;
    conn.host = "127.0.0.1";
    conn.tcp_port = port;
    conn.response_timeout_ms = 250;
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

// ConformU's STANDARD response target for an initiator.
constexpr auto kInitiatorBudget = std::chrono::milliseconds(1000);

std::chrono::milliseconds timed(const std::function<void()>& call) {
    const auto start = Clock::now();
    call();
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
}

// Longer than the whole GOTO setup (site sync + delayed ":Sr" + ":Sd" +
// ":MS"), so a ":MS" that was going to follow the initiator has gone out.
bool goto_sent_within_setup_window(const FakeZwoState& st) {
    return wait_until([&] { return st.goto_count.load() > 0; }, 2000);
}

// open-astro#720: the setup commands received once the fencing call returned.
// Waits out the whole setup window, so a setup thread the call did not join
// has had time to send whatever it was going to send.
int setup_sent_after_return(const FakeZwoState& st, int at_return) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    return st.setup_count.load() - at_return;
}

}  // namespace

TEST_CASE("ZWO telescope - all four equatorial slews are refused while Tracking is false",
          "[zwo][telescope][async][tracking-guard]") {
    auto st = std::make_shared<FakeZwoState>();
    st->tracking_on.store(false);
    alpacacore::test::FakeMountServer server(zwo_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE_FALSE(driver->get_tracking());

    const auto require_code = [](const std::function<void()>& call, int code) {
        try {
            call();
            FAIL("Expected an AlpacaException");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == code);
        }
    };
    // Argument validation comes first: a bad Dec is InvalidValue even with Tracking off.
    require_code([&] { driver->slew_to_coordinates(5.5, 95.0); }, alpacacore::AlpacaError::InvalidValue);
    require_code([&] { driver->slew_to_coordinates_async(5.5, 95.0); }, alpacacore::AlpacaError::InvalidValue);

    driver->set_target_right_ascension(5.5);  // the target forms need one; the setters write it to the mount
    driver->set_target_declination(20.0);
    const int targets_before = st->sr_count.load() + st->sd_count.load();
    require_code([&] { driver->slew_to_coordinates(5.5, 20.0); }, alpacacore::AlpacaError::InvalidOperation);
    require_code([&] { driver->slew_to_coordinates_async(5.5, 20.0); }, alpacacore::AlpacaError::InvalidOperation);
    require_code([&] { driver->slew_to_target(); }, alpacacore::AlpacaError::InvalidOperation);
    require_code([&] { driver->slew_to_target_async(); }, alpacacore::AlpacaError::InvalidOperation);

    // Neither a target write nor a GOTO reached the mount, and the driver is not slewing.
    CHECK_FALSE(wait_until(
        [&] { return st->goto_count.load() != 0 || st->sr_count.load() + st->sd_count.load() != targets_before; },
        500));
    CHECK(read_slewing(*driver) == SlewingRead::False);

    // With the mount tracking the same call goes through.
    st->tracking_on.store(true);
    REQUIRE(wait_until([&] { return driver->get_tracking(); }, 5000));  // the cached read ages out after 2 s
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    CHECK(wait_until([&] { return st->goto_count.load() >= 1; }, 5000));
    driver->set_connected(false);
}

// ":GU" may drop the tracking flag while a GOTO runs: a retarget of the driver's own GOTO is not refused for
// it, past the 5 s force window too. A commanded Tracking=false is still off.
TEST_CASE("ZWO telescope - a retarget of its own GOTO in flight is not refused for Tracking",
          "[zwo][telescope][async][tracking-guard]") {
    auto st = std::make_shared<FakeZwoState>();
    alpacacore::test::FakeMountServer server(zwo_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_tracking());

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return st->goto_count.load() >= 1; }, 5000));
    st->goto_not_tracking.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(5500));  // past the 5 s Slewing force window
    REQUIRE(wait_until([&] { return !driver->get_tracking(); }, 3000));
    CHECK(read_slewing(*driver) == SlewingRead::True);

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(6.0, 25.0));
    CHECK(wait_until([&] { return st->goto_count.load() >= 2; }, 5000));

    // Tracking=false during the GOTO is a commanded off: the next slew is refused, nothing more sent.
    driver->set_tracking(false);
    try {
        driver->slew_to_coordinates_async(6.5, 25.0);
        FAIL("Expected an AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
    }
    CHECK_FALSE(wait_until([&] { return st->goto_count.load() > 2; }, 500));
    driver->set_connected(false);
}

TEST_CASE("ZWO slew completion waits for the final coordinate pair", "[zwo][telescope][async][arrival]") {
    const bool reject_final = GENERATE(false, true);
    CAPTURE(reject_final);
    for (const bool delay_ra : {true, false}) {
        CAPTURE(delay_ra);
        struct State {
            std::atomic<bool> arrived{false};
            std::atomic<bool> stopped_status_sent{false};
            std::atomic<bool> coordinate_read_started{false};
            std::atomic<bool> reject_coordinate{false};
            std::atomic<int> stopped_status_count{0};
        };
        auto state = std::make_shared<State>();
        const auto fallback = zwo_responder(std::make_shared<FakeZwoState>());
        alpacacore::test::FakeMountServer server([state, delay_ra, fallback](const std::string& command) {
            if (command.find(":GU") != std::string::npos) {
                const bool arrived = state->arrived.load();
                if (arrived) {
                    state->stopped_status_sent.store(true);
                    state->stopped_status_count.fetch_add(1);
                }
                return std::string(arrived ? "NG#" : "G#");
            }
            const bool ra = command.find(":GR") != std::string::npos;
            const bool dec = command.find(":GD") != std::string::npos;
            if (ra || dec) {
                const bool arrived = state->arrived.load();
                if (arrived && state->stopped_status_sent.load() && ra == delay_ra) {
                    state->coordinate_read_started.store(true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    if (state->reject_coordinate.load()) return std::string("bad#");
                }
                return std::string(ra ? (arrived ? "20:47:56#" : "20:47:00#")
                                      : (arrived ? "+65*00:00#" : "+64*59:00#"));
            }
            return fallback(command);
        });
        REQUIRE(server.ok());
        auto conn = endpoint(server.port());
        conn.response_timeout_ms = 2000;
        auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, conn);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
        REQUIRE(driver->get_slewing());
        state->reject_coordinate.store(reject_final);
        state->arrived.store(true);
        REQUIRE(wait_until([&] { return state->coordinate_read_started.load(); }, 5000));
        // GU says stopped, but GR/GD are still in flight: completion must stay hidden.
        CHECK(driver->get_slewing());
        if (reject_final) {
            // The next GU proves the previous coordinate read failed and the poll retried.
            REQUIRE(wait_until([&] { return state->stopped_status_count.load() >= 2; }, 5000));
            CHECK(driver->get_slewing());
            state->reject_coordinate.store(false);
        }
        REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 5000));
        CHECK(std::abs(driver->get_right_ascension() - (20.0 + 47.0 / 60.0 + 56.0 / 3600.0)) < 1e-9);
        CHECK(std::abs(driver->get_declination() - 65.0) < 1e-9);
        CHECK(alpacacore::test::settle_connected(*driver, false));
    }
}

TEST_CASE("ZWO own GOTO remains retargetable until arrival is published unless Tracking is commanded off",
          "[zwo][telescope][async][arrival][goto-ownership]") {
    const bool reject_ra = GENERATE(false, true);
    const bool command_tracking_off = GENERATE(false, true);
    alpacacore::test::FakeTaskClock clock;
    CAPTURE(reject_ra, command_tracking_off);
    struct State {
        std::atomic<bool> moving{false};
        std::atomic<bool> stopped{false};
        std::atomic<bool> reject_coordinate{true};
        std::atomic<int> moving_pairs{0};
        std::atomic<int> stopped_polls{0};
    };
    auto state = std::make_shared<State>();
    auto counts = std::make_shared<FakeZwoState>();
    const auto fallback = zwo_responder(counts);
    alpacacore::test::FakeMountServer server([state, reject_ra, fallback,
                                              pending = std::string{}](const std::string& chunk) mutable {
        pending += chunk;
        std::string response;
        for (auto end = pending.find('#'); end != std::string::npos; end = pending.find('#')) {
            const auto command = pending.substr(0, end + 1);
            pending.erase(0, end + 1);
            const bool stopped = state->stopped.load();
            if (command == ":GU#") {
                if (stopped) state->stopped_polls.fetch_add(1);
                response += stopped ? "nN#" : (state->moving.load() ? "n#" : "N#");
            } else if (command == ":GAT#") {
                response += state->moving.load() ? "0#" : "1#";
            } else if (command == ":GR#" || command == ":GD#") {
                const bool ra = command == ":GR#";
                if (stopped && state->reject_coordinate.load() && ra == reject_ra) {
                    response += "bad#";
                } else {
                    if (!ra && state->moving.load() && !stopped) state->moving_pairs.fetch_add(1);
                    response += ra ? (stopped ? "13:00:00#" : "12:00:00#") : (stopped ? "+46*00:00#" : "+45*00:00#");
                }
            } else {
                response += fallback(command);
            }
        }
        return response;
    });
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, endpoint(server.port()), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    REQUIRE(driver->get_tracking());
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return counts->goto_count.load() == 1; }, 5000));
    // Drain the setup exchange before advancing the poll clock; MS receipt precedes its reply completion.
    REQUIRE_NOTHROW(driver->command_string(":GVP", false));
    state->moving.store(true);
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    clock.advance(std::chrono::milliseconds(250));
    REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(5)));
    REQUIRE(wait_until([&] { return state->moving_pairs.load() > 0; }, 5000));
    // The force window alone would permit a retarget even if GU cleared ownership too early.
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
    clock.advance(std::chrono::milliseconds(5500));
    REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(5)));
    state->stopped.store(true);
    clock.advance(std::chrono::milliseconds(250));
    REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(5)));
    REQUIRE(state->stopped_polls.load() > 0);
    REQUIRE_FALSE(driver->get_tracking());
    REQUIRE(driver->get_slewing());

    const auto require_refused_without_writes = [&] {
        const int gotos = counts->goto_count.load();
        const int targets = counts->sr_count.load() + counts->sd_count.load();
        try {
            driver->slew_to_coordinates_async(6.5, 25.0);
            FAIL("Expected Tracking-off retarget to be refused");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
        }
        CHECK_FALSE(wait_until(
            [&] {
                return counts->goto_count.load() != gotos ||
                       counts->sr_count.load() + counts->sd_count.load() != targets;
            },
            500));
    };

    if (command_tracking_off) {
        // Explicit Tracking=false revokes the exception even when the mount already reports it off.
        REQUIRE_NOTHROW(driver->set_tracking(false));
        require_refused_without_writes();
    } else {
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(6.0, 25.0));
        REQUIRE(wait_until([&] { return counts->goto_count.load() == 2; }, 5000));
        REQUIRE_NOTHROW(driver->command_string(":GVP", false));
        CHECK(driver->get_slewing());
        state->reject_coordinate.store(false);
        REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));
        clock.advance(std::chrono::milliseconds(5500));
        REQUIRE(clock.wait_for_woken_settled(std::chrono::seconds(5)));
        REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 8000));
        CHECK(std::abs(driver->get_right_ascension() - 13.0) < 1e-9);
        CHECK(std::abs(driver->get_declination() - 46.0) < 1e-9);
        REQUIRE_FALSE(driver->get_tracking());
        require_refused_without_writes();
    }
    CHECK(alpacacore::test::settle_connected(*driver, false));
}

TEST_CASE("ZWO moving status remains visible when coordinate reads fail", "[zwo][telescope][async][arrival]") {
    auto moving = std::make_shared<std::atomic<bool>>(false);
    auto state = std::make_shared<FakeZwoState>();
    state->serve_position.store(true);
    const auto fallback = zwo_responder(state);
    alpacacore::test::FakeMountServer server([moving, fallback](const std::string& command) {
        if (moving->load()) {
            if (command.find(":GU") != std::string::npos) return std::string("G#");
            if (command.find(":GR") != std::string::npos) return std::string("bad#");
        }
        return fallback(command);
    });
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    REQUIRE_FALSE(driver->get_slewing());
    moving->store(true);
    CHECK(wait_until([&] { return driver->get_slewing(); }, 2000));
    CHECK(alpacacore::test::settle_connected(*driver, false));
}

TEST_CASE("ZWO horizon rejection never retries with mirrored longitude", "[zwo][telescope][async][longitude]") {
    const bool west = GENERATE(false, true);
    CAPTURE(west);
    const double longitude = west ? -100.5 : 100.5;
    const std::string wire_site = west ? "+30*00:00&+100*30:00" : "+30*00:00&-100*30:00";
    struct Counts {
        std::atomic<int> attempts{0};
        std::atomic<int> site_writes{0};
        std::atomic<int> wrong_site_writes{0};
    };
    auto counts = std::make_shared<Counts>();
    auto state = std::make_shared<FakeZwoState>();
    state->serve_position.store(true);
    const auto fallback = zwo_responder(state);
    alpacacore::test::FakeMountServer server(
        [counts, wire_site, fallback, pending = std::string{}](const std::string& chunk) mutable {
            pending += chunk;
            std::string response;
            for (auto end = pending.find('#'); end != std::string::npos; end = pending.find('#')) {
                const auto command = pending.substr(0, end + 1);
                pending.erase(0, end + 1);
                if (command == ":MS#") {
                    counts->attempts.fetch_add(1);
                    response += "e5#";
                } else if (command == ":GMGE#") {
                    response += wire_site + "#";
                } else if (command.starts_with(":SMGE")) {
                    counts->site_writes.fetch_add(1);
                    if (command != ":SMGE" + wire_site + "#") counts->wrong_site_writes.fetch_add(1);
                    response += "1#";
                } else if (command.starts_with(":SMTI")) {
                    response += "1#";
                } else {
                    response += fallback(command);
                }
            }
            return response;
        });
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    const int writes_before = counts->site_writes.load();
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    std::string error;
    REQUIRE(wait_until([&] { return read_slewing(*driver, &error) == SlewingRead::Threw; }, 10000));
    CHECK(error.find("e5") != std::string::npos);
    CHECK(counts->attempts.load() == 2);
    CHECK(counts->site_writes.load() >= writes_before + 2);
    CHECK(counts->wrong_site_writes.load() == 0);
    CHECK(std::abs(driver->get_site_longitude() - longitude) < 1e-9);
    CHECK(alpacacore::test::settle_connected(*driver, false));
}

TEST_CASE("ZWO async - a GOTO the mount rejects surfaces through Slewing (#575)",
          "[zwo][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeZwoState>();
    alpacacore::test::FakeMountServer server(zwo_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->reject_goto.store(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return st->goto_count.load() >= 1; }, 5000));  // ":MS" went out on the GOTO thread

    // Today: "GOTO failed: Mount rejected GOTO request" in the log, then
    // Slewing=false after the 5 s force window. Must read as an error.
    std::string message;
    REQUIRE(wait_until([&] { return read_slewing(*driver, &message) == SlewingRead::Threw; }, 10000));
    CHECK_FALSE(message.empty());
    CHECK(message.find("Slew") != std::string::npos);
    CHECK(read_slewing(*driver) == SlewingRead::Threw);  // preserved, not one-shot

    // Only Slewing reports the failure: a rejected GOTO must not turn a
    // position read into that error.
    // (This fake serves no :GR/:GD, so the read may still fail to parse.)
    const auto position_read_error = [&](const std::function<void()>& read) {
        try {
            read();
        } catch (const std::exception& ex) {
            return std::string(ex.what());
        }
        return std::string();
    };
    CHECK(position_read_error([&] { (void)driver->get_right_ascension(); }).find("SlewToTargetAsync") ==
          std::string::npos);
    CHECK(position_read_error([&] { (void)driver->get_declination(); }).find("SlewToTargetAsync") == std::string::npos);
    CHECK(read_slewing(*driver) == SlewingRead::Threw);

    // AbortSlew is a valid later command: it clears the stored failure.
    REQUIRE_NOTHROW(driver->abort_slew());
    const SlewingRead after_abort = read_slewing(*driver);
    CHECK((after_abort == SlewingRead::True || after_abort == SlewingRead::False));
    driver->set_connected(false);
}

TEST_CASE("ZWO async - AbortSlew during GOTO setup is not reported as a slew failure (#575)",
          "[zwo][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeZwoState>();
    alpacacore::test::FakeMountServer server(zwo_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    // The mount would reject the GOTO, but the client aborts while the setup
    // thread is still writing the target: the abort cancels the setup, so the
    // rejection that would have followed must never be recorded.
    st->reject_goto.store(true);
    st->sr_delay_ms.store(150);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    // open-astro#720: abort while the ":Sr" is in flight. The fence lets that
    // one round trip finish and nothing after it, and nothing at all once
    // AbortSlew has returned.
    REQUIRE(wait_until([&] { return st->sr_count.load() > 0; }, 1000));
    REQUIRE_NOTHROW(driver->abort_slew());
    const int setup_at_return = st->setup_count.load();
    // Outlast the setup (site sync + delayed ":Sr" + ":Sd" + ":MS").
    CHECK_FALSE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 2000));
    CHECK(st->goto_count.load() == 0);  // the aborted GOTO was never sent
    CHECK(setup_sent_after_return(*st, setup_at_return) == 0);
    CHECK(st->sd_count.load() == 0);
    driver->set_connected(false);
}

TEST_CASE("ZWO async - a MoveAxis during GOTO setup owns the error slot (#575)",
          "[zwo][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeZwoState>();
    alpacacore::test::FakeMountServer server(zwo_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    // A newer initiator (here MoveAxis) lands while the GOTO setup thread is
    // still writing the target; the superseded GOTO's later rejection is not
    // the jog's failure and must not make Slewing throw during the jog.
    st->reject_goto.store(true);
    st->sr_delay_ms.store(150);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    std::chrono::milliseconds elapsed{0};
    REQUIRE_NOTHROW(elapsed = timed([&] { driver->move_axis(0, 1.0); }));
    CHECK(elapsed < kInitiatorBudget);
    CHECK_FALSE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 2000));
    // open-astro#720: the jog fences the pending GOTO, so its ":MS" never
    // lands on top of the jog.
    CHECK(st->goto_count.load() == 0);
    CHECK(st->jog_count.load() == 1);
    REQUIRE_NOTHROW(driver->move_axis(0, 0.0));
    driver->set_connected(false);
}

// open-astro#720: every initiator that commands the mount fences a GOTO setup
// thread that is still writing the target (cancel AND join) before it sends
// its own command. The GOTO here would be accepted, so a ":MS" on the wire is
// the stale GOTO overriding the newer command.
TEST_CASE("ZWO async - Park during GOTO setup cancels the pending GOTO (#720)",
          "[zwo][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeZwoState>();
    alpacacore::test::FakeMountServer server(zwo_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->sr_delay_ms.store(150);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return st->sr_count.load() > 0; }, 1000));
    std::chrono::milliseconds elapsed{0};
    REQUIRE_NOTHROW(elapsed = timed([&] { driver->park(); }));
    const int setup_at_return = st->setup_count.load();
    CHECK(elapsed < kInitiatorBudget);
    CHECK_FALSE(goto_sent_within_setup_window(*st));
    CHECK(st->goto_count.load() == 0);
    CHECK(st->park_count.load() == 1);
    CHECK(setup_sent_after_return(*st, setup_at_return) == 0);
    CHECK(st->sd_count.load() == 0);
    driver->set_connected(false);
}

TEST_CASE("ZWO async - FindHome during GOTO setup cancels the pending GOTO (#720)",
          "[zwo][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeZwoState>();
    alpacacore::test::FakeMountServer server(zwo_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->sr_delay_ms.store(150);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    std::chrono::milliseconds elapsed{0};
    REQUIRE_NOTHROW(elapsed = timed([&] { driver->find_home(); }));
    CHECK(elapsed < kInitiatorBudget);
    CHECK_FALSE(goto_sent_within_setup_window(*st));
    CHECK(st->goto_count.load() == 0);
    CHECK(st->home_count.load() == 1);
    driver->set_connected(false);
}

TEST_CASE("ZWO async - MoveAxis stop during GOTO setup cancels the pending GOTO (#720)",
          "[zwo][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeZwoState>();
    alpacacore::test::FakeMountServer server(zwo_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->sr_delay_ms.store(150);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    std::chrono::milliseconds elapsed{0};
    REQUIRE_NOTHROW(elapsed = timed([&] { driver->move_axis(0, 0.0); }));
    CHECK(elapsed < kInitiatorBudget);
    CHECK_FALSE(goto_sent_within_setup_window(*st));
    CHECK(st->goto_count.load() == 0);
    CHECK(st->stop_count.load() == 1);
    driver->set_connected(false);
}

// open-astro#720: the cancelled GOTO never slewed, so it must not leave its
// 5 s Slewing force window behind or report the abandoned target as position.
TEST_CASE("ZWO async - a GOTO cancelled during setup leaves no slew bookkeeping (#720)",
          "[zwo][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeZwoState>();
    st->serve_position.store(true);
    alpacacore::test::FakeMountServer server(zwo_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->sr_delay_ms.store(150);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE_NOTHROW(driver->move_axis(0, 0.0));
    CHECK_FALSE(goto_sent_within_setup_window(*st));
    // Still inside the GOTO's force window, but nothing is moving.
    CHECK(read_slewing(*driver) == SlewingRead::False);
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::False; }, 6000));
    CHECK(std::fabs(driver->get_right_ascension() - 12.0) < 1e-6);
    CHECK(std::fabs(driver->get_declination() - 45.0) < 1e-6);
    driver->set_connected(false);
}

// open-astro#720: the ":Sr" the canceller waits for comes back rejected, so the
// cancelled GOTO leaves through its outer catch; that exit abandons it too.
TEST_CASE("ZWO async - a cancelled GOTO whose setup then fails leaves no slew bookkeeping (#720)",
          "[zwo][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeZwoState>();
    st->serve_position.store(true);
    alpacacore::test::FakeMountServer server(zwo_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->sr_delay_ms.store(150);
    st->reject_sr.store(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    // Cancel while the ":Sr" is in flight, not at an earlier checkpoint.
    REQUIRE(wait_until([&] { return st->sr_count.load() > 0; }, 1000));
    REQUIRE_NOTHROW(driver->move_axis(0, 0.0));
    CHECK_FALSE(goto_sent_within_setup_window(*st));
    CHECK(read_slewing(*driver) == SlewingRead::False);
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::False; }, 6000));
    CHECK(std::fabs(driver->get_right_ascension() - 12.0) < 1e-6);
    CHECK(std::fabs(driver->get_declination() - 45.0) < 1e-6);
    driver->set_connected(false);
}

TEST_CASE("ZWO async - Disconnect during GOTO setup cancels the pending GOTO (#720)",
          "[zwo][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeZwoState>();
    alpacacore::test::FakeMountServer server(zwo_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->sr_delay_ms.store(150);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return st->sr_count.load() > 0; }, 1000));
    std::chrono::milliseconds elapsed{0};
    REQUIRE_NOTHROW(elapsed = timed([&] { driver->set_connected(false); }));
    const int setup_at_return = st->setup_count.load();
    CHECK(elapsed < kInitiatorBudget);
    CHECK_FALSE(goto_sent_within_setup_window(*st));
    CHECK(st->goto_count.load() == 0);
    CHECK(setup_sent_after_return(*st, setup_at_return) == 0);
    CHECK(st->sd_count.load() == 0);
}

// open-astro#720: the cancel lands while the site/time sync's ":SMTI" is in
// flight, so the GOTO must stop there: no target write follows the sync,
// before or after Connected=false returns.
TEST_CASE("ZWO async - Disconnect during the GOTO's site/time sync sends no target (#720)",
          "[zwo][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeZwoState>();
    alpacacore::test::FakeMountServer server(zwo_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->smti_delay_ms.store(150);
    const int smti_before = st->smti_count.load();
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return st->smti_count.load() > smti_before; }, 1000));
    std::chrono::milliseconds elapsed{0};
    REQUIRE_NOTHROW(elapsed = timed([&] { driver->set_connected(false); }));
    const int setup_at_return = st->setup_count.load();
    CHECK(elapsed < kInitiatorBudget);
    CHECK(setup_sent_after_return(*st, setup_at_return) == 0);
    CHECK(st->sr_count.load() == 0);
    CHECK(st->sd_count.load() == 0);
    CHECK(st->goto_count.load() == 0);
}

TEST_CASE("ZWO async - position reports mount feedback after GOTO", "[zwo][telescope][async][position]") {
    auto st = std::make_shared<FakeZwoState>();
    st->serve_position.store(true);
    alpacacore::test::FakeMountServer server(zwo_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    const double mount_ra = driver->get_right_ascension();
    const double mount_dec = driver->get_declination();
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(6.0, 22.0));
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 8000));

    CHECK(std::abs(driver->get_right_ascension() - mount_ra) < 1e-6);
    CHECK(std::abs(driver->get_declination() - mount_dec) < 1e-6);
    driver->set_connected(false);
}

TEST_CASE("ZWO blocking slew - position reports mount feedback after GOTO", "[zwo][telescope][position]") {
    auto st = std::make_shared<FakeZwoState>();
    st->serve_position.store(true);
    alpacacore::test::FakeMountServer server(zwo_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    const double mount_ra = driver->get_right_ascension();
    const double mount_dec = driver->get_declination();
    REQUIRE_NOTHROW(driver->slew_to_coordinates(4.0, -12.0));

    CHECK(std::abs(driver->get_right_ascension() - mount_ra) < 1e-6);
    CHECK(std::abs(driver->get_declination() - mount_dec) < 1e-6);
    driver->set_connected(false);
}

TEST_CASE("ZWO async - redundant Connected=true preserves a stored slew failure",
          "[zwo][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeZwoState>();
    alpacacore::test::FakeMountServer server(zwo_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::zwo::create_zwo_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->reject_sr.store(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 5000));
    CHECK(read_slewing(*driver) == SlewingRead::Threw);

    // Platform 7 may resend Connected=true while already connected. That is
    // only a refresh, not a successful motion recovery.
    REQUIRE_NOTHROW(driver->set_connected(true));
    CHECK(read_slewing(*driver) == SlewingRead::Threw);

    // A full successful reconnect is a valid recovery and clears the old error.
    st->reject_sr.store(false);
    driver->set_connected(false);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    const SlewingRead after_reconnect = read_slewing(*driver);
    CHECK((after_reconnect == SlewingRead::True || after_reconnect == SlewingRead::False));
    driver->set_connected(false);
}

#endif  // _WIN32
