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

// Serial-port ownership for the three Gemini wrappers (issue #1202): each one
// opens its port through util::SerialLink, so a port another connected device
// holds is refused before it is opened or retuned, the refusal names the
// holder, the claim is released on disconnect, and the PDH's reader recovers
// from a port another program retuned (the field failure: a flat panel scan
// left the connected hub's port at 9600 baud and the hub never recovered).

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/vendor/gemini/gemini_flatpanel_protocol_wrapper.h>
#include <alpacacore/vendor/gemini/gemini_pdh_protocol_wrapper.h>
#include <alpacacore/vendor/gemini/gemini_protocol_wrapper.h>
#include <termios.h>

#include <chrono>
#include <future>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "fake_gemini_flatpanel.h"
#include "fake_gemini_focuser.h"
#include "fake_gemini_pdh.h"

namespace {

namespace gemini = alpacacore::vendor::gemini;
using alpacacore::util::is_serial_port_in_use;
using alpacacore::util::SerialClaimKind;
using alpacacore::util::SerialPortClaim;

speed_t line_baud(int fd) {
    struct termios tty {};
    REQUIRE(tcgetattr(fd, &tty) == 0);
    return cfgetospeed(&tty);
}

// Run @p connect against a port "Other device" holds and return the refusal
// text; fails the case if the connect succeeds or throws something else.
template <typename Connect>
std::string refusal_text(Connect&& connect) {
    try {
        connect();
    } catch (const alpacacore::AlpacaException& e) {
        return e.what();
    }
    FAIL("the connect was not refused");
    return {};
}

}  // namespace

TEST_CASE("Gemini focuser wrapper - refuses a port another connected device holds",
          "[gemini][focuser][serial][issue1202]") {
    alpacacore::test::FakeGeminiFocuser fake;
    auto other = SerialPortClaim::acquire(fake.slave_path(), SerialClaimKind::Link, "Other device");
    REQUIRE(other);
    const speed_t before = line_baud(fake.keepalive_fd());

    gemini::GeminiProtocolWrapper wrapper;
    gemini::ConnectionConfig config;
    config.serial_port = fake.slave_path();
    const std::string why = refusal_text([&] { wrapper.connect(config); });
    INFO(why);
    CHECK(why.find("Other device") != std::string::npos);
    CHECK_FALSE(wrapper.is_connected());
    CHECK(line_baud(fake.keepalive_fd()) == before);  // never retuned
    CHECK(fake.commands().empty());                   // never written to
}

TEST_CASE("Gemini focuser wrapper - holds its port while connected and releases it on disconnect",
          "[gemini][focuser][serial][issue1202]") {
    alpacacore::test::FakeGeminiFocuser fake;
    gemini::GeminiProtocolWrapper wrapper;
    gemini::ConnectionConfig config;
    config.serial_port = fake.slave_path();

    REQUIRE(wrapper.connect(config) > 0);
    CHECK(is_serial_port_in_use(fake.slave_path()));
    CHECK(line_baud(fake.keepalive_fd()) == B9600);
    wrapper.disconnect();
    CHECK_FALSE(is_serial_port_in_use(fake.slave_path()));

    // TIOCEXCL was released with the port, so the reconnect can open it.
    REQUIRE(wrapper.connect(config) > 0);
    CHECK(is_serial_port_in_use(fake.slave_path()));
    wrapper.disconnect();
    CHECK_FALSE(is_serial_port_in_use(fake.slave_path()));
}

TEST_CASE("Gemini flat panel wrapper - refuses a port another connected device holds",
          "[gemini][flatpanel][serial][issue1202]") {
    alpacacore::test::FakeGeminiFlatPanel fake;
    auto other = SerialPortClaim::acquire(fake.slave_path(), SerialClaimKind::Link, "Other device");
    REQUIRE(other);
    const speed_t before = line_baud(fake.keepalive_fd());

    gemini::GeminiFlatPanelProtocolWrapper wrapper;
    gemini::FlatPanelConnectionConfig config;
    config.serial_port = fake.slave_path();
    config.model = gemini::FlatPanelModel::Pro;
    const std::string why = refusal_text([&] { wrapper.connect(config); });
    INFO(why);
    CHECK(why.find("Other device") != std::string::npos);
    CHECK_FALSE(wrapper.is_connected());
    CHECK(line_baud(fake.keepalive_fd()) == before);
    CHECK(fake.commands().empty());
}

TEST_CASE("Gemini flat panel wrapper - holds its port while connected and releases it on disconnect",
          "[gemini][flatpanel][serial][issue1202]") {
    alpacacore::test::FakeGeminiFlatPanel fake;
    gemini::GeminiFlatPanelProtocolWrapper wrapper;
    gemini::FlatPanelConnectionConfig config;
    config.serial_port = fake.slave_path();
    config.model = gemini::FlatPanelModel::Pro;

    REQUIRE(wrapper.connect(config) == "107");
    CHECK(is_serial_port_in_use(fake.slave_path()));
    CHECK(line_baud(fake.keepalive_fd()) == B9600);
    wrapper.disconnect();
    CHECK_FALSE(is_serial_port_in_use(fake.slave_path()));

    REQUIRE(wrapper.connect(config) == "107");
    CHECK(is_serial_port_in_use(fake.slave_path()));
    wrapper.disconnect();
    CHECK_FALSE(is_serial_port_in_use(fake.slave_path()));
}

TEST_CASE("Gemini power hub wrapper - refuses a port another connected device holds",
          "[gemini][switch][serial][issue1202]") {
    alpacacore::test::FakeGeminiPdh fake;
    auto other = SerialPortClaim::acquire(fake.slave_path(), SerialClaimKind::Link, "Other device");
    REQUIRE(other);
    const speed_t before = line_baud(fake.keepalive_fd());

    gemini::GeminiPdhProtocolWrapper wrapper;
    gemini::PdhConnectionConfig config;
    config.serial_port = fake.slave_path();
    const std::string why = refusal_text([&] { wrapper.connect(config); });
    INFO(why);
    CHECK(why.find("Other device") != std::string::npos);
    CHECK_FALSE(wrapper.is_connected());
    CHECK(line_baud(fake.keepalive_fd()) == before);
    CHECK(fake.commands().empty());
}

TEST_CASE("Gemini power hub wrapper - holds its port while connected and releases it on disconnect",
          "[gemini][switch][serial][issue1202]") {
    alpacacore::test::FakeGeminiPdh fake;
    gemini::GeminiPdhProtocolWrapper wrapper;
    gemini::PdhConnectionConfig config;
    config.serial_port = fake.slave_path();

    REQUIRE(wrapper.connect(config) == 308);
    CHECK(is_serial_port_in_use(fake.slave_path()));
    CHECK(line_baud(fake.keepalive_fd()) == B19200);
    wrapper.disconnect();
    CHECK_FALSE(is_serial_port_in_use(fake.slave_path()));

    REQUIRE(wrapper.connect(config) == 308);
    CHECK(is_serial_port_in_use(fake.slave_path()));
    wrapper.disconnect();
    CHECK_FALSE(is_serial_port_in_use(fake.slave_path()));
}

TEST_CASE("Gemini power hub wrapper - disconnect returns promptly after the port is retuned to VMIN=1",
          "[gemini][switch][serial][issue1202]") {
    alpacacore::test::FakeGeminiPdh fake;
    gemini::GeminiPdhProtocolWrapper wrapper;
    gemini::PdhConnectionConfig config;
    config.serial_port = fake.slave_path();
    REQUIRE(wrapper.connect(config) == 308);

    // Another program retunes the live port to a blocking read, at the hub's
    // own speed (only the read mode changes), then the reader runs past one
    // read window under it.
    fake.retune(B19200);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    std::promise<void> done;
    auto done_future = done.get_future();
    std::thread disconnecter([&] {
        wrapper.disconnect();
        done.set_value();
    });
    const bool returned = done_future.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
    disconnecter.join();
    CHECK(returned);
    CHECK_FALSE(wrapper.is_connected());
    CHECK_FALSE(is_serial_port_in_use(fake.slave_path()));
}

TEST_CASE("Gemini power hub wrapper - a link retuned by another program recovers without a reconnect",
          "[gemini][switch][serial][issue1202]") {
    alpacacore::test::FakeGeminiPdh fake;
    gemini::GeminiPdhProtocolWrapper wrapper;
    gemini::PdhConnectionConfig config;
    config.serial_port = fake.slave_path();
    REQUIRE(wrapper.connect(config) == 308);
    REQUIRE(wrapper.get_state().valid);

    // The #1202 field failure: another scan leaves the connected hub's port at
    // 9600 baud. The fake then answers nothing and fills the line with
    // garbage, so the reader never has a quiet read window in which tty_read
    // would put the settings back by itself; only the link-fault path can.
    fake.set_garble_at_wrong_baud(true);
    fake.retune(B9600);
    REQUIRE(fake.line_baud() == B9600);

    // ~3 unanswered polls (6 s) latch the fault, and the latch restores 19200.
    bool latched = false;
    const auto latch_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (std::chrono::steady_clock::now() < latch_deadline) {
        if (wrapper.link_fault().has_value()) {
            latched = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(latched);

    // The next poll is answered at the right speed: the fault clears and the
    // status is served again, on the same connection.
    bool recovered = false;
    const auto recover_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < recover_deadline) {
        if (!wrapper.link_fault().has_value() && wrapper.get_state().valid) {
            recovered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(fake.line_baud() == B19200);
    CHECK(recovered);
    CHECK(wrapper.is_connected());
    wrapper.disconnect();
}
