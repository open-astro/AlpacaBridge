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

// open-astro#575, Celestron: the async slew task dispatches the GOTO (HC "r"
// or the AUX MC_GOTO_FAST passthrough) on its own thread; a hand controller
// that never acknowledges it made the task log "Async slew dispatch failed"
// and drop Slewing to false. The next Slewing read must return an Alpaca
// error instead (contract reference in test_skywatcher_async_slew_failure.cpp).

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/celestron/celestron_telescope_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"  // settle_connected
#include "fake_mount_server.h"

namespace {

using Clock = std::chrono::steady_clock;

struct FakeCelestronState {
    std::atomic<bool> reject_goto{false};  // true: swallow the GOTO -> the wrapper times out and throws
    std::atomic<bool> shifted_position{false};
    std::atomic<int> goto_count{0};
    std::atomic<Clock::rep> goto_started{0};
    std::mutex command_mutex;
    std::vector<std::string> commands;
    void record(const std::string& command) {
        std::lock_guard<std::mutex> lock(command_mutex);
        commands.push_back(command);
    }
    int command_count(char prefix) {
        std::lock_guard<std::mutex> lock(command_mutex);
        return static_cast<int>(std::count_if(commands.begin(), commands.end(), [prefix](const std::string& command) {
            return !command.empty() && command[0] == prefix;
        }));
    }
    int guide_command_count(int axis) {
        const auto device = static_cast<unsigned char>(axis == 0 ? 0x10 : 0x11);
        std::lock_guard<std::mutex> lock(command_mutex);
        return static_cast<int>(std::count_if(commands.begin(), commands.end(), [device](const std::string& command) {
            return command.size() >= 8 && command[0] == 'P' && static_cast<unsigned char>(command[2]) == device &&
                   static_cast<unsigned char>(command[3]) == 0x26;
        }));
    }
    static constexpr auto kGotoDuration = std::chrono::milliseconds(1500);
    bool goto_in_progress() const {
        if (goto_count.load() == 0) return false;
        const auto started = Clock::time_point(Clock::duration(goto_started.load()));
        return Clock::now() - started < kGotoDuration;
    }
    std::string goto_reply() {
        if (reject_goto.load()) {
            return "";  // no reply at all: the response timeout fires inside the slew task
        }
        goto_started.store(Clock::now().time_since_epoch().count());
        goto_count.fetch_add(1);
        return "#";
    }
};

alpacacore::test::FakeMountServer::Responder celestron_responder(const std::shared_ptr<FakeCelestronState>& st) {
    return [st](const std::string& chunk) -> std::string {
        if (chunk.empty()) return "0#";
        if (chunk[0] == 'T' || chunk[0] == 'P') {
            st->record(chunk);
        }
        switch (chunk[0]) {
            case 'e':
            case 'E':
            case 'z':
            case 'Z':
                return st->shifted_position.load() ? "13AB0500,21000500#" : "12AB0500,20000500#";
            case 'r':
            case 'R':
            case 'b':
            case 'B':
                return st->goto_reply();
            case 'L':
                return st->goto_in_progress() ? "1#" : "0#";
            case 'M':  // cancel goto
                return "#";
            case 'J':  // alignment complete (the slew-safety gate requires it)
                return "1#";
            case 'p':  // pier side used by the DEC guide command
                return "W#";
            case 'T':
                return "#";
            case 'P': {  // AUX passthrough: P len dev op ...
                const unsigned char op = chunk.size() > 3 ? static_cast<unsigned char>(chunk[3]) : 0;
                const unsigned char device = chunk.size() > 2 ? static_cast<unsigned char>(chunk[2]) : 0;
                if (op == 0xFE && device == 0x32) {  // DEC autoguider port firmware probe
                    return std::string("\x01\x00#", 3);
                }
                if (op == 0xFE) {  // complete the binary firmware reply for absent bus devices
                    return std::string("\x00\x00#", 3);
                }
                if (op == 0x26) {  // MC_AUX_GUIDE
                    return "#";
                }
                if (op == 0x02 || op == 0x17) {  // MC_GOTO_FAST / MC_GOTO_SLOW
                    return st->goto_reply();
                }
                if (op == 0x13) {  // MC_SLEW_DONE
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
    info.response_timeout_ms = 1000;
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

}  // namespace

TEST_CASE("Celestron async - a GOTO the hand controller never acknowledges surfaces through Slewing (#575)",
          "[celestron][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeCelestronState>();
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->reject_goto.store(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    // True: published before the task ran. Threw: the task won mutex_ first
    // and its dispatch already failed -- never a silent False.
    const SlewingRead first = read_slewing(*driver);
    REQUIRE((first == SlewingRead::True || first == SlewingRead::Threw));

    std::string message;
    REQUIRE(wait_until([&] { return read_slewing(*driver, &message) == SlewingRead::Threw; }, 10000));
    CHECK_FALSE(message.empty());
    CHECK(message.find("Slew") != std::string::npos);
    CHECK(read_slewing(*driver) == SlewingRead::Threw);  // preserved, not one-shot

    // A retried initiator against a responsive controller clears it.
    st->reject_goto.store(false);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    CHECK(read_slewing(*driver) == SlewingRead::True);
    REQUIRE(wait_until([&] { return st->goto_count.load() == 1; }, 5000));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::False; }, 15000));
    driver->set_connected(false);
}

TEST_CASE("Celestron async - AbortSlew clears a stored slew failure (#575)",
          "[celestron][telescope][async][slewfailure]") {
    auto st = std::make_shared<FakeCelestronState>();
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->reject_goto.store(true);
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    REQUIRE(wait_until([&] { return read_slewing(*driver) == SlewingRead::Threw; }, 10000));

    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(read_slewing(*driver) == SlewingRead::False);
    driver->set_connected(false);
}

TEST_CASE("Celestron async - AbortSlew fences a pending GOTO", "[celestron][telescope][async][abort]") {
    auto st = std::make_shared<FakeCelestronState>();
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    for (int i = 0; i < 50; ++i) {
        const int before = st->goto_count.load();
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
        REQUIRE_NOTHROW(driver->abort_slew());
        const int at_abort_return = st->goto_count.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        INFO("iteration " << i << ", GOTO count before=" << before << " at abort return=" << at_abort_return
                          << " after=" << st->goto_count.load());
        CHECK(st->goto_count.load() == at_abort_return);
    }
    driver->set_connected(false);
}

TEST_CASE("Celestron AbortSlew - a stop failure still reaps the cancelled slew task",
          "[celestron][telescope][async][abort]") {
    auto st = std::make_shared<FakeCelestronState>();
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(server.drop_connections());
    CHECK_THROWS_AS(driver->abort_slew(), alpacacore::AlpacaException);

    REQUIRE_NOTHROW(driver->set_connected(false));
    REQUIRE_NOTHROW(driver->set_connected(true));
    const int before = st->goto_count.load();
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(5.5, 20.0));
    CHECK(wait_until([&] { return st->goto_count.load() > before; }, 5000));
    driver->set_connected(false);
}

TEST_CASE("Celestron async - MoveAxis owns motion after superseding an async slew",
          "[celestron][telescope][async][ownership]") {
    auto st = std::make_shared<FakeCelestronState>();
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_tracking(true);

    const int before = st->goto_count.load();
    driver->slew_to_coordinates_async(5.5, 20.0);
    REQUIRE(wait_until([&] { return st->goto_count.load() > before; }, 3000));
    driver->move_axis(0, 0.5);
    const int tracking_writes_after_move = st->command_count('T');
    std::this_thread::sleep_for(std::chrono::milliseconds(1700));

    CHECK(st->command_count('T') == tracking_writes_after_move);
    driver->set_connected(false);
}

TEST_CASE("Celestron sync - a blocking slew reaps a prior async slew task",
          "[celestron][telescope][async][ownership]") {
    auto st = std::make_shared<FakeCelestronState>();
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_tracking(true);
    const int tracking_writes_before = st->command_count('T');

    const int before = st->goto_count.load();
    driver->slew_to_coordinates_async(5.5, 20.0);
    REQUIRE(wait_until([&] { return st->goto_count.load() > before; }, 3000));
    driver->slew_to_coordinates(6.0, 22.0);
    const int tracking_writes_after_sync = st->command_count('T');
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    CHECK(tracking_writes_after_sync == tracking_writes_before + 1);
    CHECK(st->command_count('T') == tracking_writes_after_sync);
    driver->set_connected(false);
}

TEST_CASE("Celestron PulseGuide - overlapping axes keep independent pulse chains",
          "[celestron][telescope][pulseguiding][ownership]") {
    auto st = std::make_shared<FakeCelestronState>();
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_can_pulse_guide());

    const int ra_before = st->guide_command_count(0);
    const int dec_before = st->guide_command_count(1);
    const double ra_start = driver->get_right_ascension();
    const double ra_delta = driver->get_guide_rate().ra * 8.0 / 15.0;
    driver->pulse_guide(2, 8000);  // East: RA chain continues beyond the 2.55 s hardware limit.
    driver->pulse_guide(0, 3500);  // North: must not cancel RA's remaining chunks.

    REQUIRE(wait_until([&] { return st->guide_command_count(0) >= ra_before + 2; }, 4500));
    REQUIRE(st->guide_command_count(1) >= dec_before + 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    CHECK(driver->get_is_pulse_guiding());  // DEC expired; the longer RA pulse is still active.
    CHECK(std::abs(driver->get_right_ascension() - (ra_start + ra_delta)) < 1e-5);
    driver->set_connected(false);
}

TEST_CASE("Celestron MoveAxis - a jog on one axis preserves the other axis pulse chain",
          "[celestron][telescope][pulseguiding][ownership]") {
    auto st = std::make_shared<FakeCelestronState>();
    alpacacore::test::FakeMountServer server(celestron_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_can_pulse_guide());

    const int ra_before = st->guide_command_count(0);
    driver->pulse_guide(2, 6000);  // RA pulse is still chaining after its first hardware chunk.
    driver->move_axis(1, 0.0);     // A Dec-axis stop must not cancel the RA pulse.

    CHECK(wait_until([&] { return st->guide_command_count(0) >= ra_before + 2; }, 4500));
    driver->set_connected(false);
}

TEST_CASE("Celestron AbortSlew - stops each active pulse chain before returning",
          "[celestron][telescope][pulseguiding][abort]") {
    for (const int direction : {0, 2}) {
        auto st = std::make_shared<FakeCelestronState>();
        alpacacore::test::FakeMountServer server(celestron_responder(st));
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        REQUIRE(driver->get_can_pulse_guide());

        const int axis = direction == 0 ? 1 : 0;
        const int before = st->guide_command_count(axis);
        driver->pulse_guide(direction, 6000);
        REQUIRE(st->guide_command_count(axis) > before);
        REQUIRE(driver->get_is_pulse_guiding());
        REQUIRE_NOTHROW(driver->abort_slew());
        const int stopped_count = st->guide_command_count(axis);
        CHECK_FALSE(driver->get_is_pulse_guiding());
        std::this_thread::sleep_for(std::chrono::milliseconds(2700));
        CHECK(st->guide_command_count(axis) == stopped_count);
        driver->set_connected(false);
    }
}

TEST_CASE("Celestron PulseGuide - an expired unpolled opposite-axis pulse does not disable the hold",
          "[celestron][telescope][pulseguiding][ownership]") {
    for (const auto& [first_direction, second_direction, is_ra] : {std::tuple{2, 0, true}, std::tuple{0, 2, false}}) {
        auto st = std::make_shared<FakeCelestronState>();
        alpacacore::test::FakeMountServer server(celestron_responder(st));
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::celestron::create_celestron_telescope(0, endpoint(server.port()));
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        REQUIRE(driver->get_can_pulse_guide());

        driver->pulse_guide(first_direction, 500);
        // Past the 500 ms pulse plus the driver's 1 s completion delay, so the
        // first pulse has expired. Do not poll IsPulseGuiding.
        std::this_thread::sleep_for(std::chrono::milliseconds(1700));
        // The first read after a pulse returns its one-shot readback correction;
        // take it now so `held` below compares hold against hold.
        (void)(is_ra ? driver->get_right_ascension() : driver->get_declination());
        driver->pulse_guide(second_direction, 1500);
        const double held = is_ra ? driver->get_right_ascension() : driver->get_declination();
        st->shifted_position.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));  // Expire the 2 s position cache.

        const double during = is_ra ? driver->get_right_ascension() : driver->get_declination();
        CHECK(std::abs(during - held) < 1e-5);
        driver->set_connected(false);
    }
}

#endif  // _WIN32
