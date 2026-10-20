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

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/celestron/celestron_cgx_protocol.h>
#include <alpacacore/vendor/celestron/celestron_cgx_protocol_wrapper.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "fake_celestron_cgx_mount.h"

namespace {

using alpacacore::vendor::celestron::AuxFrame;
using alpacacore::vendor::celestron::decode_aux_frame;
using alpacacore::vendor::celestron::encode_aux_frame;

}  // namespace

TEST_CASE("Celestron CGX AUX codec - encodes a motor position query", "[celestron][cgx][protocol][unit]") {
    const AuxFrame request{.source = 0x00, .destination = 0x10, .command = 0x01, .payload = {}};
    const std::array<std::uint8_t, 6> expected{0x3b, 0x03, 0x00, 0x10, 0x01, 0xec};

    CHECK(encode_aux_frame(request) == std::vector<std::uint8_t>(expected.begin(), expected.end()));
}

TEST_CASE("Celestron CGX AUX codec - decodes and verifies a position reply", "[celestron][cgx][protocol][unit]") {
    const std::array<std::uint8_t, 9> bytes{0x3b, 0x06, 0x10, 0x20, 0x01, 0x12, 0x34, 0x56, 0x2d};

    const AuxFrame reply = decode_aux_frame(bytes);

    CHECK(reply.source == 0x10);
    CHECK(reply.destination == 0x20);
    CHECK(reply.command == 0x01);
    CHECK(reply.payload == std::vector<std::uint8_t>{0x12, 0x34, 0x56});
}

TEST_CASE("Celestron CGX AUX codec - rejects malformed frames", "[celestron][cgx][protocol][unit]") {
    const std::array<std::uint8_t, 6> bad_checksum{0x3b, 0x03, 0x00, 0x10, 0x01, 0xed};
    const std::array<std::uint8_t, 6> bad_start{0x3a, 0x03, 0x00, 0x10, 0x01, 0xed};
    const std::array<std::uint8_t, 5> truncated{0x3b, 0x03, 0x00, 0x10, 0x01};

    CHECK_THROWS_AS(decode_aux_frame(bad_checksum), std::invalid_argument);
    CHECK_THROWS_AS(decode_aux_frame(bad_start), std::invalid_argument);
    CHECK_THROWS_AS(decode_aux_frame(truncated), std::invalid_argument);
}

TEST_CASE("Celestron CGX AUX link - validates transaction timeout bounds", "[celestron][cgx][protocol][unit]") {
    using alpacacore::vendor::celestron::CelestronCgxProtocol;

    CelestronCgxProtocol protocol;
    const AuxFrame request{.source = 0x00, .destination = 0x10, .command = 0xfe, .payload = {}};
    for (const auto timeout : {std::chrono::milliseconds::zero(), std::chrono::milliseconds(5001)}) {
        try {
            static_cast<void>(protocol.exchange(request, timeout));
            FAIL("Expected invalid timeout to be rejected");
        } catch (const alpacacore::AlpacaException& error) {
            CHECK(error.error_code() == alpacacore::AlpacaError::InvalidValue);
        }
    }
}

TEST_CASE("Celestron CGX AUX link - resync clears a timeout but never reopens a closed port",
          "[celestron][cgx][protocol][unit]") {
    // Issue #896: the focus motor's Halt resynchronizes before its stop. A
    // resync that reopened the port could run after a racing disconnect and
    // leave the port held, exclusive, by a driver reporting Disconnected.
    alpacacore::test::FakeCelestronCgxMount fake;
    REQUIRE(fake.ok());
    alpacacore::vendor::celestron::CelestronCgxProtocol link;
    link.connect_serial(fake.slave_path());

    fake.set_muted(true);
    CHECK_THROWS_AS(
        link.get_axis_position(alpacacore::vendor::celestron::CgxAxis::RightAscension, std::chrono::milliseconds(50)),
        alpacacore::AlpacaException);
    REQUIRE_FALSE(link.is_healthy());
    fake.set_muted(false);
    link.resync();
    CHECK(link.is_healthy());
    CHECK_NOTHROW(link.get_axis_position(alpacacore::vendor::celestron::CgxAxis::RightAscension));

    link.disconnect();
    try {
        link.resync();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::NotConnected);
    }
    CHECK_FALSE(link.is_connected());
    // The port is free: another link can claim it.
    alpacacore::vendor::celestron::CelestronCgxProtocol other;
    CHECK_NOTHROW(other.connect_serial(fake.slave_path()));
    CHECK(other.is_connected());
}

TEST_CASE("Celestron CGX AUX link - claims kernel exclusive ownership and releases it on reconnect",
          "[celestron][cgx][protocol][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    // Hold an observer open so last-close cannot hide a missing TIOCNXCL.
    const auto close_file = [](FILE* file) { std::fclose(file); };
    const int observer_fd = ::open(mount.slave_path().c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    REQUIRE(observer_fd >= 0);
    std::unique_ptr<FILE, decltype(close_file)> observer(::fdopen(observer_fd, "r+"), close_file);
    if (!observer) ::close(observer_fd);
    REQUIRE(observer);
    alpacacore::vendor::celestron::CelestronCgxProtocol protocol;
    protocol.connect_serial(mount.slave_path());
    int exclusive = 0;
    REQUIRE(::ioctl(::fileno(observer.get()), TIOCGEXCL, &exclusive) == 0);
    CHECK(exclusive == 1);
    if (::geteuid() != 0) {
        const int second = ::open(mount.slave_path().c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        const int error = errno;
        if (second >= 0) ::close(second);
        CHECK(second == -1);
        CHECK(error == EBUSY);
    }
    protocol.connect_serial(mount.slave_path());
    CHECK(protocol.is_connected());
    protocol.disconnect();
    REQUIRE(::ioctl(::fileno(observer.get()), TIOCGEXCL, &exclusive) == 0);
    CHECK(exclusive == 0);
    const int reopened = ::open(mount.slave_path().c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (reopened >= 0) ::close(reopened);
    CHECK(reopened >= 0);
}

#ifndef _WIN32
TEST_CASE("Celestron CGX AUX link - healthy same-port reconnect keeps the session generation and fd",
          "[celestron][cgx][protocol][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    const auto close_file = [](FILE* file) { std::fclose(file); };
    const int observer_fd = ::open(mount.slave_path().c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    REQUIRE(observer_fd >= 0);
    std::unique_ptr<FILE, decltype(close_file)> observer(::fdopen(observer_fd, "r+"), close_file);
    if (!observer) ::close(observer_fd);
    REQUIRE(observer);
    alpacacore::vendor::celestron::CelestronCgxProtocol protocol;
    const auto before_open = protocol.session_generation();
    protocol.connect_serial(mount.slave_path());
    const auto opened = protocol.session_generation();
    CHECK(opened > before_open);

    // PTYs ignore baud in their data path. A marker speed detects a needless
    // reopen/reconfigure without accessing the wrapper's private descriptor.
    struct termios settings {};
    REQUIRE(::tcgetattr(::fileno(observer.get()), &settings) == 0);
    REQUIRE(::cfsetispeed(&settings, B57600) == 0);
    REQUIRE(::cfsetospeed(&settings, B57600) == 0);
    REQUIRE(::tcsetattr(::fileno(observer.get()), TCSANOW, &settings) == 0);
    for (int reconnect = 0; reconnect < 2; ++reconnect) {
        protocol.connect_serial(mount.slave_path());
        CHECK(protocol.session_generation() == opened);
        REQUIRE(::tcgetattr(::fileno(observer.get()), &settings) == 0);
        CHECK(::cfgetospeed(&settings) == B57600);
        int exclusive = 0;
        REQUIRE(::ioctl(::fileno(observer.get()), TIOCGEXCL, &exclusive) == 0);
        CHECK(exclusive == 1);
        CHECK(protocol.get_axis_position(alpacacore::vendor::celestron::CgxAxis::RightAscension) == 0U);
    }
    protocol.disconnect();
    const auto closed = protocol.session_generation();
    CHECK(closed > opened);
    protocol.disconnect();
    CHECK(protocol.session_generation() == closed);
    protocol.connect_serial(mount.slave_path());
    CHECK(protocol.session_generation() > closed);
    REQUIRE(::tcgetattr(::fileno(observer.get()), &settings) == 0);
    CHECK(::cfgetospeed(&settings) == B115200);
}

TEST_CASE("Celestron CGX AUX link - desync advances generation and recovers on the exclusively owned fd",
          "[celestron][cgx][protocol][integration]") {
    for (const bool checksum_failure : {false, true}) {
        alpacacore::test::FakeCelestronCgxMount mount;
        REQUIRE(mount.ok());
        const auto close_file = [](FILE* file) { std::fclose(file); };
        const int observer_fd = ::open(mount.slave_path().c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        REQUIRE(observer_fd >= 0);
        std::unique_ptr<FILE, decltype(close_file)> observer(::fdopen(observer_fd, "r+"), close_file);
        if (!observer) ::close(observer_fd);
        REQUIRE(observer);
        alpacacore::vendor::celestron::CelestronCgxProtocol protocol;
        protocol.connect_serial(mount.slave_path());
        const auto healthy = protocol.session_generation();
        struct termios settings {};
        REQUIRE(::tcgetattr(::fileno(observer.get()), &settings) == 0);
        REQUIRE(::cfsetispeed(&settings, B57600) == 0);
        REQUIRE(::cfsetospeed(&settings, B57600) == 0);
        REQUIRE(::tcsetattr(::fileno(observer.get()), TCSANOW, &settings) == 0);
        if (checksum_failure)
            mount.corrupt_next_reply_checksum();
        else
            mount.fragment_next_reply();
        const AuxFrame request{.source = 0x00, .destination = 0x10, .command = 0xfe, .payload = {}};
        try {
            static_cast<void>(protocol.exchange(request, std::chrono::milliseconds(50)));
            FAIL("Expected a desynchronizing transaction failure");
        } catch (const alpacacore::AlpacaException& error) {
            CHECK(error.error_code() == alpacacore::AlpacaError::DriverException);
        }
        const auto desynchronized = protocol.session_generation();
        CHECK(desynchronized > healthy);
        if (!checksum_failure) {
            // Queue the old reply's tail only after its transaction expired.
            // Recovery must clear both this input and its buffered first half.
            const bool held = mount.wait_for_reply_fragment(std::chrono::milliseconds(100));
            mount.release_reply_tail();
            REQUIRE(held);
            int queued = 0;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            do {
                REQUIRE(::ioctl(::fileno(observer.get()), FIONREAD, &queued) == 0);
                if (queued > 0) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (std::chrono::steady_clock::now() < deadline);
            REQUIRE(queued > 0);
        }
        mount.set_motor_identity(0x10, {7, 19}, {0x17, 0x88});
        const int sent = mount.count_command(0x10, 0xfe);
        CHECK_THROWS_AS(protocol.exchange(request, std::chrono::milliseconds(100)), alpacacore::AlpacaException);
        CHECK(protocol.session_generation() == desynchronized);
        CHECK(mount.count_command(0x10, 0xfe) == sent);
        int exclusive = 0;
        REQUIRE(::ioctl(::fileno(observer.get()), TIOCGEXCL, &exclusive) == 0);
        CHECK(exclusive == 1);

        protocol.connect_serial(mount.slave_path());
        CHECK(protocol.session_generation() == desynchronized);
        REQUIRE(::tcgetattr(::fileno(observer.get()), &settings) == 0);
        CHECK(::cfgetospeed(&settings) == B57600);  // Recovery flushed the held fd, not a new open.
        REQUIRE(::ioctl(::fileno(observer.get()), TIOCGEXCL, &exclusive) == 0);
        CHECK(exclusive == 1);
        CHECK(protocol.exchange(request, std::chrono::milliseconds(100)).payload == std::vector<std::uint8_t>{7, 19});
        CHECK(protocol.session_generation() == desynchronized);
        CHECK(mount.count_command(0x10, 0xfe) == sent + 1);
        protocol.disconnect();
        CHECK(protocol.session_generation() > desynchronized);
        REQUIRE(::ioctl(::fileno(observer.get()), TIOCGEXCL, &exclusive) == 0);
        CHECK(exclusive == 0);
    }
}

TEST_CASE("Celestron CGX AUX link - does not claim a port held by another instance",
          "[celestron][cgx][protocol][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    alpacacore::vendor::celestron::CelestronCgxProtocol first;
    alpacacore::vendor::celestron::CelestronCgxProtocol second;
    first.connect_serial(mount.slave_path());

    try {
        second.connect_serial(mount.slave_path());
        FAIL("Expected an already claimed serial port to be refused");
    } catch (const alpacacore::AlpacaException& error) {
        CHECK(error.error_code() == alpacacore::AlpacaError::InvalidOperation);
    }

    first.disconnect();
    CHECK_NOTHROW(second.connect_serial(mount.slave_path()));
}

TEST_CASE("Celestron CGX AUX wrapper - exposes typed position, home and slew operations",
          "[celestron][cgx][protocol][integration]") {
    using alpacacore::vendor::celestron::CelestronCgxProtocol;
    using alpacacore::vendor::celestron::CgxAxis;

    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_encoder_position(0x10, 0x123456);
    mount.set_encoder_position(0x11, 0x654321);
    CelestronCgxProtocol protocol;
    protocol.connect_serial(mount.slave_path());

    CHECK(protocol.get_axis_position(CgxAxis::RightAscension) == 0x123456U);
    CHECK(protocol.get_axis_position(CgxAxis::Declination) == 0x654321U);
    protocol.goto_axis_fast(CgxAxis::RightAscension, 0x010203U);
    protocol.goto_axis_fast(CgxAxis::Declination, 0x040506U);
    CHECK(protocol.is_axis_slew_done(CgxAxis::RightAscension) == false);
    CHECK(protocol.is_axis_slew_done(CgxAxis::Declination) == false);
    mount.complete_axis_slew(0x10);
    CHECK(protocol.is_axis_slew_done(CgxAxis::RightAscension));
    CHECK_FALSE(protocol.is_axis_slew_done(CgxAxis::Declination));
    mount.complete_axis_slew(0x11);
    CHECK(protocol.is_axis_slew_done(CgxAxis::Declination));

    for (const auto& [axis, address, target] :
         {std::tuple{CgxAxis::RightAscension, 0x10, 0xfedcbaU}, std::tuple{CgxAxis::Declination, 0x11, 0xabcdefU}}) {
        const auto before = protocol.get_axis_position(axis);
        protocol.goto_axis_slow(axis, target);
        CHECK_FALSE(protocol.is_axis_slew_done(axis));
        CHECK(protocol.get_axis_position(axis) == before);
        const auto frames = mount.requests();
        const auto slow = std::find_if(frames.rbegin(), frames.rend(), [address](const auto& frame) {
            return frame.destination == address && frame.command == 0x17;
        });
        REQUIRE(slow != frames.rend());
        CHECK(slow->source == 0x00);
        CHECK(slow->payload == std::vector<std::uint8_t>{static_cast<std::uint8_t>(target >> 16),
                                                         static_cast<std::uint8_t>(target >> 8),
                                                         static_cast<std::uint8_t>(target)});
        mount.complete_axis_slew(address);
        CHECK(protocol.is_axis_slew_done(axis));
        CHECK(protocol.get_axis_position(axis) == target);
    }

    protocol.start_axis_home(CgxAxis::RightAscension);
    protocol.start_axis_home(CgxAxis::Declination);
    CHECK_FALSE(protocol.is_axis_home_done(CgxAxis::RightAscension));
    CHECK_FALSE(protocol.is_axis_home_done(CgxAxis::Declination));
    mount.complete_axis_home(0x10);
    CHECK(protocol.is_axis_home_done(CgxAxis::RightAscension));
    CHECK_FALSE(protocol.is_axis_home_done(CgxAxis::Declination));
    mount.complete_axis_home(0x11);
    CHECK(protocol.is_axis_home_done(CgxAxis::Declination));
}

TEST_CASE("Celestron CGX AUX wrapper - slow GOTO preserves validation and ambiguous timeout",
          "[celestron][cgx][protocol][integration]") {
    using namespace std::chrono_literals;
    using alpacacore::vendor::celestron::CelestronCgxProtocol;
    using alpacacore::vendor::celestron::CgxAxis;
    const auto require_error = [](auto&& call, int expected) {
        try {
            call();
            FAIL("Expected AlpacaException");
        } catch (const alpacacore::AlpacaException& error) {
            REQUIRE(error.error_code() == expected);
        }
    };
    CelestronCgxProtocol disconnected;
    require_error([&] { disconnected.goto_axis_slow(CgxAxis::RightAscension, 0x1000000); },
                  alpacacore::AlpacaError::InvalidValue);
    require_error([&] { disconnected.goto_axis_slow(static_cast<CgxAxis>(0x12), 0); },
                  alpacacore::AlpacaError::InvalidValue);
    require_error([&] { disconnected.goto_axis_slow(CgxAxis::RightAscension, 0, 0ms); },
                  alpacacore::AlpacaError::InvalidValue);
    require_error([&] { disconnected.goto_axis_slow(CgxAxis::RightAscension, 0); },
                  alpacacore::AlpacaError::NotConnected);
    for (const bool applied : {false, true}) {
        alpacacore::test::FakeCelestronCgxMount mount;
        REQUIRE(mount.ok());
        CelestronCgxProtocol protocol;
        protocol.connect_serial(mount.slave_path());
        const auto generation = protocol.session_generation();
        // A silent/unsupported command and a lost ACK after application both
        // fail without replaying the destination or reporting completion.
        mount.drop_next_reply(0x10, 0x17, applied);
        const auto started = std::chrono::steady_clock::now();
        require_error([&] { protocol.goto_axis_slow(CgxAxis::RightAscension, 0x123456, 50ms); },
                      alpacacore::AlpacaError::DriverException);
        CHECK(std::chrono::steady_clock::now() - started < 500ms);
        CHECK(mount.count_command(0x10, 0x17) == 1);
        CHECK(mount.count_command(0x10, 0x02) == 0);
        CHECK_FALSE(protocol.is_healthy());
        CHECK(protocol.session_generation() != generation);
        require_error([&] { protocol.goto_axis_slow(CgxAxis::RightAscension, 0x123456); },
                      alpacacore::AlpacaError::DriverException);
        CHECK(mount.count_command(0x10, 0x17) == 1);
    }
}

TEST_CASE("Celestron CGX AUX wrapper - starts and polls native pulse guiding",
          "[celestron][cgx][protocol][integration]") {
    using alpacacore::vendor::celestron::CelestronCgxProtocol;
    using alpacacore::vendor::celestron::CgxAxis;

    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    CelestronCgxProtocol protocol;
    protocol.connect_serial(mount.slave_path());

    protocol.pulse_guide_axis(CgxAxis::RightAscension, -50, 100);
    CHECK(protocol.is_aux_guide_active(CgxAxis::RightAscension));
    const auto requests = mount.requests();
    const auto pulse =
        std::find_if(requests.rbegin(), requests.rend(), [](const auto& frame) { return frame.command == 0x26; });
    REQUIRE(pulse != requests.rend());
    CHECK(pulse->payload == std::vector<std::uint8_t>{0xce, 100});
    mount.complete_axis_guide(0x10);
    CHECK_FALSE(protocol.is_aux_guide_active(CgxAxis::RightAscension));
    CHECK_FALSE(protocol.is_aux_guide_active(CgxAxis::Declination));
}

TEST_CASE("Celestron CGX AUX link - exchanges a framed command over serial",
          "[celestron][cgx][protocol][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());

    alpacacore::vendor::celestron::CelestronCgxProtocol protocol;
    protocol.connect_serial(mount.slave_path());

    const AuxFrame response = protocol.exchange(
        AuxFrame{.source = 0x00, .destination = 0x10, .command = 0xfe, .payload = {}}, std::chrono::milliseconds(500));

    CHECK(response.source == 0x10);
    CHECK(response.destination == 0x00);
    CHECK(response.command == 0xfe);
    CHECK(response.payload == std::vector<std::uint8_t>{7, 18});
}

TEST_CASE("Celestron CGX AUX link - ignores request echoes and assembles fragmented replies",
          "[celestron][cgx][protocol][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_echo_requests(true);
    mount.fragment_next_reply();

    alpacacore::vendor::celestron::CelestronCgxProtocol protocol;
    protocol.connect_serial(mount.slave_path());
    auto exchange = std::async(std::launch::async, [&] {
        return protocol.exchange(AuxFrame{.source = 0x00, .destination = 0x10, .command = 0xfe, .payload = {}},
                                 std::chrono::milliseconds(1000));
    });
    const bool held = mount.wait_for_reply_fragment(std::chrono::milliseconds(500));
    const auto before_tail = exchange.wait_for(std::chrono::milliseconds::zero());
    mount.release_reply_tail();
    REQUIRE(held);
    CHECK(before_tail == std::future_status::timeout);
    const auto response = exchange.get();

    CHECK(response.source == 0x10);
    CHECK(response.destination == 0x00);
    CHECK(response.payload == std::vector<std::uint8_t>{7, 18});
}

TEST_CASE("Fake Celestron CGX mount - sever cancels a held reply", "[celestron][cgx][protocol][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    mount.fragment_next_reply();
    alpacacore::vendor::celestron::CelestronCgxProtocol protocol;
    protocol.connect_serial(mount.slave_path());
    auto exchange = std::async(std::launch::async, [&] {
        return protocol.exchange(AuxFrame{.source = 0x00, .destination = 0x10, .command = 0xfe, .payload = {}},
                                 std::chrono::milliseconds(1000));
    });
    const bool held = mount.wait_for_reply_fragment(std::chrono::milliseconds(500));
    const auto started = std::chrono::steady_clock::now();
    mount.sever_link();
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500));
    REQUIRE(held);
    CHECK_THROWS_AS(exchange.get(), alpacacore::AlpacaException);
    CHECK_FALSE(protocol.is_connected());
}

TEST_CASE("Celestron CGX AUX link - reports a bad response checksum as a driver error",
          "[celestron][cgx][protocol][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.corrupt_next_reply_checksum();

    alpacacore::vendor::celestron::CelestronCgxProtocol protocol;
    protocol.connect_serial(mount.slave_path());
    try {
        static_cast<void>(
            protocol.exchange(AuxFrame{.source = 0x00, .destination = 0x10, .command = 0xfe, .payload = {}},
                              std::chrono::milliseconds(500)));
        FAIL("Expected a checksum failure");
    } catch (const alpacacore::AlpacaException& error) {
        CHECK(error.error_code() == alpacacore::AlpacaError::DriverException);
    }

    const int commands_before_rejection = mount.count_command(0x10, 0xfe);
    try {
        static_cast<void>(
            protocol.exchange(AuxFrame{.source = 0x00, .destination = 0x10, .command = 0xfe, .payload = {}},
                              std::chrono::milliseconds(500)));
        FAIL("A malformed frame leaves the response stream desynchronized");
    } catch (const alpacacore::AlpacaException& error) {
        CHECK(error.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(error.what()).find("reconnect before issuing another command") != std::string::npos);
    }
    CHECK(mount.count_command(0x10, 0xfe) == commands_before_rejection);

    protocol.connect_serial(mount.slave_path());
    const auto recovered = protocol.exchange(
        AuxFrame{.source = 0x00, .destination = 0x10, .command = 0xfe, .payload = {}}, std::chrono::milliseconds(500));
    CHECK(recovered.payload == std::vector<std::uint8_t>{7, 18});
}

TEST_CASE("Fake Celestron CGX mount - tracks independent motor positions and slews",
          "[celestron][cgx][protocol][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_encoder_position(0x10, 0x123456);
    mount.set_encoder_position(0x11, 0x654321);

    alpacacore::vendor::celestron::CelestronCgxProtocol protocol;
    protocol.connect_serial(mount.slave_path());
    const auto transact = [&protocol](std::uint8_t axis, std::uint8_t command, std::vector<std::uint8_t> payload = {}) {
        return protocol.exchange(
            AuxFrame{.source = 0x00, .destination = axis, .command = command, .payload = std::move(payload)},
            std::chrono::milliseconds(500));
    };

    CHECK(transact(0x10, 0x01).payload == std::vector<std::uint8_t>{0x12, 0x34, 0x56});
    CHECK(transact(0x11, 0x01).payload == std::vector<std::uint8_t>{0x65, 0x43, 0x21});
    CHECK(transact(0x10, 0x02, {0x01, 0x02, 0x03}).payload.empty());
    CHECK(transact(0x11, 0x02, {0x04, 0x05, 0x06}).payload.empty());
    CHECK(transact(0x10, 0x13).payload == std::vector<std::uint8_t>{0x00});
    CHECK(transact(0x11, 0x13).payload == std::vector<std::uint8_t>{0x00});

    mount.complete_axis_slew(0x10);
    CHECK(transact(0x10, 0x13).payload == std::vector<std::uint8_t>{0xff});
    CHECK(transact(0x11, 0x13).payload == std::vector<std::uint8_t>{0x00});
    mount.complete_axis_slew(0x11);
    CHECK(transact(0x11, 0x13).payload == std::vector<std::uint8_t>{0xff});
    CHECK(transact(0x10, 0x01).payload == std::vector<std::uint8_t>{0x01, 0x02, 0x03});
    CHECK(transact(0x11, 0x01).payload == std::vector<std::uint8_t>{0x04, 0x05, 0x06});
}

TEST_CASE("Fake Celestron CGX mount - reports home completion independently per axis",
          "[celestron][cgx][protocol][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());

    alpacacore::vendor::celestron::CelestronCgxProtocol protocol;
    protocol.connect_serial(mount.slave_path());
    const auto transact = [&protocol](std::uint8_t axis, std::uint8_t command) {
        return protocol.exchange(AuxFrame{.source = 0x00, .destination = axis, .command = command, .payload = {}},
                                 std::chrono::milliseconds(500));
    };

    CHECK(transact(0x10, 0x0b).payload.empty());
    CHECK(transact(0x11, 0x0b).payload.empty());
    CHECK(transact(0x10, 0x12).payload == std::vector<std::uint8_t>{0x00});
    CHECK(transact(0x11, 0x12).payload == std::vector<std::uint8_t>{0x00});

    mount.complete_axis_home(0x10);
    CHECK(transact(0x10, 0x12).payload == std::vector<std::uint8_t>{0xff});
    CHECK(transact(0x11, 0x12).payload == std::vector<std::uint8_t>{0x00});
    mount.complete_axis_home(0x11);
    CHECK(transact(0x11, 0x12).payload == std::vector<std::uint8_t>{0xff});
}

TEST_CASE("Fake Celestron CGX mount - advances GOTO and tracking from elapsed simulated time",
          "[celestron][cgx][protocol][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_axis_velocity_counts_per_second(0x10, 1000);
    mount.set_encoder_position(0x10, 0x100000);

    alpacacore::vendor::celestron::CelestronCgxProtocol protocol;
    protocol.connect_serial(mount.slave_path());
    protocol.goto_axis_fast(alpacacore::vendor::celestron::CgxAxis::RightAscension, 0x100fa0);

    mount.advance_motion(std::chrono::seconds(1));
    CHECK(protocol.get_axis_position(alpacacore::vendor::celestron::CgxAxis::RightAscension) == 0x1003e8U);
    CHECK_FALSE(protocol.is_axis_slew_done(alpacacore::vendor::celestron::CgxAxis::RightAscension));
    mount.advance_motion(std::chrono::seconds(3));
    CHECK(protocol.get_axis_position(alpacacore::vendor::celestron::CgxAxis::RightAscension) == 0x100fa0U);
    CHECK(protocol.is_axis_slew_done(alpacacore::vendor::celestron::CgxAxis::RightAscension));

    const AuxFrame tracking{.source = 0x00, .destination = 0x10, .command = 0x06, .payload = {0, 0, 1}};
    static_cast<void>(protocol.exchange(tracking, std::chrono::milliseconds(500)));
    mount.advance_motion(std::chrono::seconds(1));
    CHECK(protocol.get_axis_position(alpacacore::vendor::celestron::CgxAxis::RightAscension) == 0x101388U);

    static_cast<void>(protocol.exchange(AuxFrame{.source = 0x00, .destination = 0x10, .command = 0x24, .payload = {1}},
                                        std::chrono::milliseconds(500)));
    mount.advance_motion(std::chrono::seconds(1));
    CHECK(protocol.get_axis_position(alpacacore::vendor::celestron::CgxAxis::RightAscension) == 0x101770U);
    static_cast<void>(protocol.exchange(AuxFrame{.source = 0x00, .destination = 0x10, .command = 0x24, .payload = {0}},
                                        std::chrono::milliseconds(500)));
    mount.advance_motion(std::chrono::seconds(1));
    CHECK(mount.axis_position(0x10) == 0x101770U);
    static_cast<void>(protocol.exchange(AuxFrame{.source = 0x00, .destination = 0x10, .command = 0x25, .payload = {1}},
                                        std::chrono::milliseconds(500)));
    mount.advance_motion(std::chrono::seconds(1));
    CHECK(mount.axis_position(0x10) == 0x101388U);
}

TEST_CASE("Fake Celestron CGX mount - encoder relabel does not move physical position",
          "[celestron][cgx][protocol][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    mount.set_axis_velocity_counts_per_second(0x10, 1000);
    mount.set_encoder_position(0x10, 0x100000);

    alpacacore::vendor::celestron::CelestronCgxProtocol protocol;
    protocol.connect_serial(mount.slave_path());
    const auto exchange = [&](std::uint8_t command, std::vector<std::uint8_t> payload = {}) {
        return protocol.exchange(
            AuxFrame{.source = 0x00, .destination = 0x10, .command = command, .payload = std::move(payload)},
            std::chrono::milliseconds(500));
    };
    CHECK(protocol.get_axis_position(alpacacore::vendor::celestron::CgxAxis::RightAscension) == 0x100000U);

    static_cast<void>(exchange(0x04, {0x20, 0x00, 0x00}));
    CHECK(mount.axis_physical_position(0x10) == 0U);
    CHECK(protocol.get_axis_position(alpacacore::vendor::celestron::CgxAxis::RightAscension) == 0x200000U);
    const AuxFrame tracking{.source = 0x00, .destination = 0x10, .command = 0x06, .payload = {0, 0, 1}};
    static_cast<void>(protocol.exchange(tracking, std::chrono::milliseconds(500)));
    mount.advance_motion(std::chrono::seconds(1));
    CHECK(protocol.get_axis_position(alpacacore::vendor::celestron::CgxAxis::RightAscension) == 0x2003e8U);
    CHECK(mount.axis_physical_position(0x10) == 1000U);
    static_cast<void>(exchange(0x04, {0x30, 0x00, 0x00}));
    CHECK(mount.axis_physical_position(0x10) == 1000U);
    protocol.start_axis_home(alpacacore::vendor::celestron::CgxAxis::RightAscension);
    mount.advance_motion(std::chrono::seconds(1));
    CHECK(mount.axis_homing(0x10));
    CHECK(mount.axis_physical_position(0x10) == 1000U);
    mount.complete_axis_home(0x10);
    CHECK(mount.axis_physical_position(0x10) == 0U);
    CHECK(mount.axis_position(0x10) == 0x2ffc18U);
}

TEST_CASE("Fake Celestron CGX mount - fractional motion wraps and rejects overflowing time",
          "[celestron][cgx][protocol][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    mount.set_axis_velocity_counts_per_second(0x10, 1);
    mount.set_encoder_position(0x10, 0xffffff);
    alpacacore::vendor::celestron::CelestronCgxProtocol protocol;
    protocol.connect_serial(mount.slave_path());
    protocol.goto_axis_fast(alpacacore::vendor::celestron::CgxAxis::RightAscension, 1);
    for (int i = 0; i < 4; ++i) mount.advance_motion(std::chrono::milliseconds(250));
    CHECK(mount.axis_position(0x10) == 0U);
    CHECK_FALSE(protocol.is_axis_slew_done(alpacacore::vendor::celestron::CgxAxis::RightAscension));
    mount.advance_motion(std::chrono::seconds(1));
    CHECK(mount.axis_position(0x10) == 1U);
    CHECK(protocol.is_axis_slew_done(alpacacore::vendor::celestron::CgxAxis::RightAscension));

    static_cast<void>(
        protocol.exchange(AuxFrame{.source = 0x00, .destination = 0x10, .command = 0x07, .payload = {0, 0, 1}},
                          std::chrono::milliseconds(500)));
    for (int i = 0; i < 4; ++i) mount.advance_motion(std::chrono::milliseconds(250));
    CHECK(mount.axis_position(0x10) == 0U);
    mount.advance_motion(std::chrono::seconds(1));
    CHECK(mount.axis_position(0x10) == 0xffffffU);
    CHECK_THROWS_AS(mount.advance_motion(std::chrono::milliseconds(-1)), std::invalid_argument);
    CHECK_THROWS_AS(mount.advance_motion(std::chrono::milliseconds::max()), std::invalid_argument);
    CHECK(mount.axis_position(0x10) == 0xffffffU);
    CHECK(mount.axis_position(0x11) == 0U);
}

TEST_CASE("Celestron CGX AUX link - reports a removed USB device as a driver error",
          "[celestron][cgx][protocol][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    alpacacore::vendor::celestron::CelestronCgxProtocol protocol;
    protocol.connect_serial(mount.slave_path());
    mount.sever_link();

    try {
        static_cast<void>(
            protocol.exchange(AuxFrame{.source = 0x00, .destination = 0x10, .command = 0xfe, .payload = {}},
                              std::chrono::milliseconds(500)));
        FAIL("Expected the removed serial device to fail the transaction");
    } catch (const alpacacore::AlpacaException& error) {
        CHECK(error.error_code() == alpacacore::AlpacaError::DriverException);
    }
}

TEST_CASE("Celestron CGX AUX link - requires reconnect after a timed-out reply",
          "[celestron][cgx][protocol][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    alpacacore::vendor::celestron::CelestronCgxProtocol protocol;
    protocol.connect_serial(mount.slave_path());
    mount.set_muted(true);

    const AuxFrame request{.source = 0x00, .destination = 0x10, .command = 0xfe, .payload = {}};
    try {
        static_cast<void>(protocol.exchange(request, std::chrono::milliseconds(20)));
        FAIL("Expected a muted controller to time out");
    } catch (const alpacacore::AlpacaException& error) {
        CHECK(error.error_code() == alpacacore::AlpacaError::DriverException);
    }

    mount.set_muted(false);
    CHECK_THROWS_AS(protocol.exchange(request, std::chrono::milliseconds(100)), alpacacore::AlpacaException);
    protocol.disconnect();
    protocol.connect_serial(mount.slave_path());
    CHECK(protocol.exchange(request, std::chrono::milliseconds(100)).payload == std::vector<std::uint8_t>{7, 18});
}

TEST_CASE("Celestron CGX AUX link - logs the original transaction failure once without changing exceptions",
          "[celestron][cgx][protocol][integration]") {
    bool checksum_failure = false;
    SECTION("timeout") {}
    SECTION("invalid response checksum") { checksum_failure = true; }
    for (const bool throwing_sink : {false, true}) {
        CAPTURE(checksum_failure, throwing_sink);
        alpacacore::test::FakeCelestronCgxMount mount;
        REQUIRE(mount.ok());
        alpacacore::vendor::celestron::CelestronCgxProtocol protocol;
        protocol.connect_serial(mount.slave_path());
        std::vector<std::string> messages;
        struct SinkGuard {
            alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
            alpacacore::logging::LogLevel level = alpacacore::logging::get_log_level();
            ~SinkGuard() {
                alpacacore::logging::set_log_sink(previous);
                alpacacore::logging::set_log_level(level);
            }
        } sink_guard;
        alpacacore::logging::set_log_level(alpacacore::logging::LogLevel::Error);
        alpacacore::logging::set_log_sink(
            [&](alpacacore::logging::LogLevel level, std::string_view component, std::string_view message) {
                if (level == alpacacore::logging::LogLevel::Error && component == "Celestron CGX") {
                    messages.emplace_back(message);
                    if (throwing_sink) throw std::runtime_error("log sink failure");
                }
            });
        if (checksum_failure)
            mount.corrupt_next_reply_checksum();
        else
            mount.set_muted(true);
        const AuxFrame request{
            .source = 0x20, .destination = 0x11, .command = 0xfe, .payload = {'p', 'r', 'i', 'v', 'a', 't', 'e'}};
        const std::string cause = checksum_failure ? "Celestron AUX frame checksum is invalid"
                                                   : "Celestron CGX USB: AUX transaction timed out";
        try {
            static_cast<void>(protocol.exchange(request, std::chrono::milliseconds(50)));
            FAIL("Expected the original transaction failure");
        } catch (const alpacacore::AlpacaException& error) {
            CHECK((error.error_code() == alpacacore::AlpacaError::DriverException));
            CHECK((std::string(error.what()) ==
                   (checksum_failure ? "Celestron CGX USB: invalid AUX response: " + cause : cause)));
        }
        REQUIRE((messages.size() == 1));
        CHECK((messages[0].find("source=32 destination=17 command=254 ") != std::string::npos));
        CHECK((messages[0].find(" timeout_ms=50 error=" + cause) != std::string::npos));
        CHECK((messages[0].find("private") == std::string::npos));
        CHECK((messages[0].find(mount.slave_path()) == std::string::npos));
        const auto elapsed_at = messages[0].find(" elapsed_ms=");
        REQUIRE((elapsed_at != std::string::npos));
        const double elapsed_ms = std::stod(messages[0].substr(elapsed_at + 12));
        CHECK((elapsed_ms >= (checksum_failure ? 0.0 : 50.0)));
        CHECK_FALSE(protocol.is_healthy());
        CHECK_THROWS_AS(protocol.exchange(request, std::chrono::milliseconds(50)), alpacacore::AlpacaException);
        CHECK((messages.size() == 1));
        protocol.disconnect();
        CHECK_THROWS_AS(protocol.exchange(request, std::chrono::milliseconds(50)), alpacacore::AlpacaException);
        CHECK((messages.size() == 1));
    }
}

TEST_CASE("Celestron CGX AUX link - enforces its deadline while unrelated frames keep arriving",
          "[celestron][cgx][protocol][integration]") {
    alpacacore::test::FakeCelestronCgxMount mount;
    REQUIRE(mount.ok());
    alpacacore::vendor::celestron::CelestronCgxProtocol protocol;
    protocol.connect_serial(mount.slave_path());
    mount.set_muted(true);
    mount.set_unrelated_noise_duration(std::chrono::milliseconds(200));
    const AuxFrame request{.source = 0x00, .destination = 0x10, .command = 0xfe, .payload = {}};

    const auto started = std::chrono::steady_clock::now();
    try {
        static_cast<void>(protocol.exchange(request, std::chrono::milliseconds(20)));
        FAIL("Expected unrelated AUX frames not to extend the exchange deadline");
    } catch (const alpacacore::AlpacaException& error) {
        CHECK(error.error_code() == alpacacore::AlpacaError::DriverException);
    }
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(100));
    protocol.disconnect();
    mount.sever_link();
}
#endif
