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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/gemini/gemini_pdh_protocol_wrapper.h>
#include <alpacacore/vendor/gemini/gemini_pdh_switch_driver.h>
#include <alpacacore/version.h>

#include <array>
#include <functional>

#include "catch2_compat.h"

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

// Synthetic >G# frame in the vendor driver's positional layout (tag letters
// are only split points, so any letter from the D/U/A/T/M/B/C/S/H/V/P set
// works between fields):
//   DC2-5 "1010", USB A-F "111000", AHT20 1, DS18B20 1,
//   DEW6 enabled 1 / mode 0 (Auto), DEW7 enabled 0 / mode 2 (Switch),
//   DEW6 manual 25 %, DEW7 manual 60 %,
//   lens 12.5, ambient 21.3, humidity 45.2, dew point 8.9,
//   13.1 V, 0.85 A, 11.2 W
const char* const kSampleFrame = "*G1010U111000A1T1D1M0D0M2B25C60S12.5H21.3V45.2P8.9D13.1C0.85B11.2#";

}  // namespace

TEST_CASE("Gemini PDH Switch Driver - Defaults", "[gemini][switch][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_pdh_switch(0, "/dev/null");

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Switch);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE_FALSE(driver->get_connected());
    CHECK(driver->get_name() == "Gemini Power & Data Hubs Advanced 3");
    CHECK(driver->get_max_switch() == 24);
    CHECK_FALSE(driver->get_connecting());
}

TEST_CASE("Gemini PDH Switch Driver - Device metadata", "[gemini][switch][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_pdh_switch(3, "/dev/null");

    REQUIRE(driver != nullptr);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "Gemini Power & Data Hubs Advanced 3 Power Box Switch Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore Gemini Power & Data Hub Switch Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "GEMINI_PDH_ADV3_3");
    // Firmware surfaces via the web UI only, and only once connected.
    CHECK_FALSE(driver->get_device_firmware().has_value());
}

TEST_CASE("Gemini PDH Switch Driver - Not connected throws", "[gemini][switch][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_pdh_switch(0, "/dev/null");

    REQUIRE_FALSE(driver->get_connected());
    require_alpaca_error([&]() { driver->get_switch(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch_value(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch(7, false); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch_value(11, 50.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_async(7, false); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_async_value(11, 0.0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_state_change_complete(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_can_write(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_can_async(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch_name(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch_name(0, "x"); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch_description(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_min_switch_value(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_switch_value(0); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch_step(0); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("Gemini PDH Switch Driver - Unsupported actions", "[gemini][switch][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_pdh_switch(0, "/dev/null");

    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("anything"));

    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

TEST_CASE("Gemini PDH Switch Driver - Value range validation", "[gemini][switch][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_pdh_switch(0, "/dev/null");

    // ID validation must run before the connection check: an out-of-range ID
    // throws InvalidValue even while disconnected (ASCOM spec), not NotConnected.
    REQUIRE_FALSE(driver->get_connected());
    require_alpaca_error([&]() { driver->get_switch(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch(24); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_value(24); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_switch_value(24, 0.5); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->set_async_value(-1, 0.5); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_state_change_complete(24); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_name(24); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_min_switch_value(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_max_switch_value(24); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_step(24); }, alpacacore::AlpacaError::InvalidValue);
    // Value validation itself runs after the connection check, so a bad value
    // on a disconnected driver reports NotConnected (same ordering as the
    // WandererBox switch driver).
    require_alpaca_error([&]() { driver->set_switch_value(11, 500.0); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("Gemini PDH Switch Driver - Disconnected DeviceState", "[gemini][switch][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_pdh_switch(0, "/dev/null");

    // DeviceState is the empty list while disconnected: the SwitchDriver base
    // builds it from the public getters, which throw NotConnected and are
    // omitted, and TimeStamp itself is withheld too (ASCOM read-all FAQ).
    const auto state = driver->get_device_state();
    REQUIRE(state.empty());
}

TEST_CASE("Gemini PDH Switch Driver - Connect failure on invalid port", "[gemini][switch][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_pdh_switch(0, "/dev/nonexistent-gemini-hub-port");

    // Synchronous connect against a missing port must fail with NotConnected
    // and leave the driver disconnected.
    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(driver->get_connected());
    // set_connected(false) while already disconnected is an idempotent no-op.
    CHECK_NOTHROW(driver->set_connected(false));
}

TEST_CASE("Gemini PDH Switch Driver - Fixed baud rate", "[gemini][switch][unit]") {
    // The protocol is fixed at 19200: a different rate is rejected at connect
    // (InvalidValue) before any port is opened, not silently ignored.
    auto driver = alpacacore::vendor::gemini::create_gemini_pdh_switch(0, "/dev/null", 9600);
    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::InvalidValue);
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("Gemini PDH Protocol Wrapper - Defaults and disconnected state", "[gemini][switch][unit]") {
    using namespace alpacacore::vendor::gemini;

    static_assert(kPdhBaudRate == 19200);
    static_assert(kPdhMinFirmware == 308);
    static_assert(kPdhDewPwmMax == 100);
    CHECK(std::string(kPdhHandshakeReply) == "*HGeminiPowerBoxPlusAdv3");

    GeminiPdhProtocolWrapper wrapper;
    CHECK_FALSE(wrapper.is_connected());
    CHECK_FALSE(wrapper.get_firmware().has_value());

    const auto state = wrapper.get_state();
    CHECK_FALSE(state.valid);
    CHECK(state.input_voltage == 0.0);
    CHECK_FALSE(state.usb[0]);
    CHECK(state.dew6.mode == PdhDewMode::Manual);

    // Operations on a disconnected wrapper report NotConnected; out-of-range
    // arguments report InvalidValue regardless of connection.
    require_alpaca_error([&]() { wrapper.set_output(2, true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { wrapper.set_dew_manual_pwm(6, 50); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { wrapper.set_dew_enabled(7, true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { wrapper.set_dew_mode(6, PdhDewMode::Auto); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { wrapper.request_status(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { wrapper.set_output(0, true); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { wrapper.set_output(12, true); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { wrapper.set_dew_manual_pwm(5, 50); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { wrapper.set_dew_manual_pwm(6, 101); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { wrapper.set_dew_manual_pwm(7, -1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { wrapper.set_dew_enabled(8, true); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { wrapper.set_dew_mode(5, PdhDewMode::Auto); }, alpacacore::AlpacaError::InvalidValue);

    PdhConnectionConfig bad_baud;
    bad_baud.serial_port = "/dev/null";
    bad_baud.baud_rate = 9600;
    require_alpaca_error([&]() { wrapper.connect(bad_baud); }, alpacacore::AlpacaError::InvalidValue);

    // Disconnect on a never-connected wrapper is a safe no-op.
    CHECK_NOTHROW(wrapper.disconnect());
}

TEST_CASE("Gemini PDH Protocol Wrapper - Handshake discrimination", "[gemini][switch][unit]") {
    using namespace alpacacore::vendor::gemini;

    CHECK(is_pdh_handshake_reply("*HGeminiPowerBoxPlusAdv3"));
    CHECK(is_pdh_handshake_reply("*HGeminiPowerBoxPlusAdv3#"));
    CHECK(is_pdh_handshake_reply("*HGeminiPowerBoxPlusAdv3#\r\n"));
    // Sibling Gemini devices on the same CH340 adapter family must be rejected.
    CHECK_FALSE(is_pdh_handshake_reply("*HGeminiFlatPanelLite#"));
    CHECK_FALSE(is_pdh_handshake_reply("*HGeminiFlatPanelPro#"));
    // The previous-generation PowerBox Plus V3 has a different frame layout.
    CHECK_FALSE(is_pdh_handshake_reply("*HGeminiPowerBoxPlusV3#"));
    CHECK_FALSE(is_pdh_handshake_reply(""));
    CHECK_FALSE(is_pdh_handshake_reply("#"));

    CHECK(format_pdh_firmware(308) == "3.0.8");
    CHECK(format_pdh_firmware(310) == "3.1.0");
    CHECK(format_pdh_firmware(1001) == "1001");
}

TEST_CASE("Gemini PDH Protocol Wrapper - Status frame parsing", "[gemini][switch][unit]") {
    using namespace alpacacore::vendor::gemini;

    const auto parsed = parse_pdh_status_frame(kSampleFrame);
    REQUIRE(parsed.has_value());
    const PdhState& s = *parsed;
    CHECK(s.valid);

    // DC2..DC5 = "1010"
    CHECK(s.dc[0]);
    CHECK_FALSE(s.dc[1]);
    CHECK(s.dc[2]);
    CHECK_FALSE(s.dc[3]);
    // USB A..F = "111000"
    CHECK(s.usb[0]);
    CHECK(s.usb[1]);
    CHECK(s.usb[2]);
    CHECK_FALSE(s.usb[3]);
    CHECK_FALSE(s.usb[4]);
    CHECK_FALSE(s.usb[5]);

    CHECK(s.aht20_attached);
    CHECK(s.ds18b20_attached);

    CHECK(s.dew6.enabled);
    CHECK(s.dew6.mode == PdhDewMode::Auto);
    CHECK(s.dew6.manual_pwm == 25);
    CHECK_FALSE(s.dew7.enabled);
    CHECK(s.dew7.mode == PdhDewMode::Switch);
    CHECK(s.dew7.manual_pwm == 60);

    CHECK(s.lens_temp == 12.5);
    CHECK(s.ambient_temp == 21.3);
    CHECK(s.humidity == 45.2);
    CHECK(s.dew_point == 8.9);
    CHECK(s.input_voltage == 13.1);
    CHECK(s.output_current == 0.85);
    CHECK(s.output_power == 11.2);

    // Absent sensors: the temperature fields take the -127 degC sentinel and
    // humidity 0, whatever the firmware put in those fields (vendor shows NaN).
    const auto no_sensors = parse_pdh_status_frame("*G0000U000000A0T0D0M1D0M1B0C0S99.9H99.9V99.9P99.9D12.0C0.1B1.2#");
    REQUIRE(no_sensors.has_value());
    CHECK_FALSE(no_sensors->aht20_attached);
    CHECK_FALSE(no_sensors->ds18b20_attached);
    CHECK(no_sensors->lens_temp == -127.0);
    CHECK(no_sensors->ambient_temp == -127.0);
    CHECK(no_sensors->humidity == 0.0);
    CHECK(no_sensors->dew_point == -127.0);
    CHECK(no_sensors->dew6.mode == PdhDewMode::Manual);
    CHECK(no_sensors->input_voltage == 12.0);

    // Frame captured from real hardware (firmware 3.0.9, 2026-09-08): the tags
    // are SUFFIXES and the numbers are fixed-width with leading spaces, which
    // the split-and-strtod parser must tolerate.
    const auto real =
        parse_pdh_status_frame("*G1111D111111U1A1T1A1M1B1M100C100C 24.06S 23.39T 42.04H  9.76D12.6V 0.11C  1.38P#");
    REQUIRE(real.has_value());
    CHECK(real->dc == std::array<bool, 4>{{true, true, true, true}});
    CHECK(real->usb == std::array<bool, 6>{{true, true, true, true, true, true}});
    CHECK(real->aht20_attached);
    CHECK(real->ds18b20_attached);
    CHECK(real->dew6.enabled);
    CHECK(real->dew6.mode == PdhDewMode::Manual);
    CHECK(real->dew6.manual_pwm == 100);
    CHECK(real->dew7.enabled);
    CHECK(real->dew7.mode == PdhDewMode::Manual);
    CHECK(real->dew7.manual_pwm == 100);
    CHECK(real->lens_temp == 24.06);
    CHECK(real->ambient_temp == 23.39);
    CHECK(real->humidity == 42.04);
    CHECK(real->dew_point == 9.76);
    CHECK(real->input_voltage == 12.6);
    CHECK(real->output_current == 0.11);
    CHECK(real->output_power == 1.38);

    // Non-status frames and malformed digit blocks are rejected, not misparsed.
    CHECK_FALSE(parse_pdh_status_frame("*V308#").has_value());
    CHECK_FALSE(parse_pdh_status_frame("*HGeminiPowerBoxPlusAdv3#").has_value());
    CHECK_FALSE(parse_pdh_status_frame("*G101U111000A1T1D1M0D0M2B25C60S1H2V3P4D5C6B7#").has_value());  // 3 DC digits
    CHECK_FALSE(parse_pdh_status_frame("*G1010U11100A1T1D1M0D0M2B25C60S1H2V3P4D5C6B7#").has_value());  // 5 USB digits
    CHECK_FALSE(parse_pdh_status_frame("*G1010U111000A1T1#").has_value());                             // truncated
    CHECK_FALSE(parse_pdh_status_frame("").has_value());
}

// ---------------------------------------------------------------------------
// Wire-level behaviour over a pty-backed fake hub (see fake_gemini_pdh.h):
// the connect handshake/firmware gate/first frame, the reader thread routing
// streamed frames vs. pending requests, and the commanded-value write paths.
// ---------------------------------------------------------------------------

#include <time.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "fake_gemini_pdh.h"

namespace {

using alpacacore::test::FakeGeminiPdh;

std::unique_ptr<alpacacore::SwitchDriver> connect_fake_hub(const FakeGeminiPdh& hub) {
    auto driver = alpacacore::vendor::gemini::create_gemini_pdh_switch(0, hub.slave_path(), 19200);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    return driver;
}

template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

}  // namespace

TEST_CASE("Gemini PDH Switch Driver - Connects over a fake hub and serves the first frame",
          "[gemini][switch][unit][fake]") {
    FakeGeminiPdh hub;
    const auto t0 = std::chrono::steady_clock::now();
    auto driver = connect_fake_hub(hub);
    const auto connect_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    // First handshake attempt goes out after 100 ms; a healthy hub connects well
    // inside the ASCOM client's 10 s budget.
    CHECK(connect_ms < 3000);

    CHECK(hub.received(">H#"));
    CHECK(hub.received(">V#"));
    CHECK(hub.received(">G#"));
    REQUIRE(driver->get_device_firmware().has_value());
    CHECK(*driver->get_device_firmware() == "3.0.8");

    // Fake defaults: every output on, both dew channels Manual at 0 %, sensors attached.
    for (int id = 0; id <= 5; ++id) CHECK(driver->get_switch(id));
    CHECK(driver->get_switch(6));  // DC1 always-on
    for (int id = 7; id <= 10; ++id) CHECK(driver->get_switch(id));
    CHECK(driver->get_switch_value(11) == 0.0);
    CHECK(driver->get_switch_value(13) == 1.0);  // Manual
    CHECK(driver->get_max_switch_value(11) == 100.0);
    CHECK(driver->get_max_switch_value(13) == 2.0);
    CHECK(driver->get_switch_value(15) == 13.1);
    CHECK(driver->get_switch_value(16) == 0.85);
    CHECK(driver->get_switch_value(17) == 11.2);
    CHECK(driver->get_switch_value(18) == 21.3);
    CHECK(driver->get_switch_value(19) == 45.2);
    CHECK(driver->get_switch_value(20) == 8.9);
    CHECK(driver->get_switch_value(21) == 12.5);
    CHECK(driver->get_switch(22));
    CHECK(driver->get_switch(23));
    CHECK_FALSE(driver->get_can_write(6));
    CHECK(driver->get_can_write(11));
    CHECK(driver->get_switch_name(0) == "USB A");
    CHECK(driver->get_switch_name(21) == "Lens Temperature");

    // Every id contributes GetSwitchN/GetSwitchValueN/StateChangeCompleteN, plus TimeStamp.
    const auto state = driver->get_device_state();
    CHECK(state.size() == 24 * 3 + 1);

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK_FALSE(driver->get_device_firmware().has_value());
    // A reconnect on the same port works (HUPCL/registry bookkeeping released).
    driver->set_connected(true);
    CHECK(driver->get_connected());
}

TEST_CASE("Gemini PDH Switch Driver - Disconnect is never observed beside a stale firmware string",
          "[gemini][switch][unit][fake]") {
    // Issue #387 ordering: connected_ is stored false before the wrapper's
    // close, and that close clears the firmware cache only after joining a
    // reader that can sit in a 500 ms read. The getter relies on the cache
    // being empty while disconnected (AGENTS.md), so the driver must clear it
    // before the store. A sampler reads (connected, firmware) pairs for the
    // whole disconnect; one pair of (false, present) is the defect. Red
    // without the clear_firmware() call (hundreds of stale samples), green
    // with it.
    FakeGeminiPdh hub;
    auto driver = connect_fake_hub(hub);
    REQUIRE(driver->get_device_firmware().has_value());

    std::atomic<bool> done{false};
    std::atomic<int> stale{0};
    std::thread sampler([&] {
        while (!done.load()) {
            const bool connected = driver->get_connected();
            const auto firmware = driver->get_device_firmware();
            if (!connected && firmware.has_value()) {
                stale.fetch_add(1);
            }
        }
    });
    driver->set_connected(false);
    done.store(true);
    sampler.join();

    CHECK(stale.load() == 0);
    CHECK_FALSE(driver->get_connected());
    CHECK_FALSE(driver->get_device_firmware().has_value());
}

TEST_CASE("Gemini PDH Switch Driver - Writes go on the wire and read back as commanded",
          "[gemini][switch][unit][fake]") {
    FakeGeminiPdh hub;
    auto driver = connect_fake_hub(hub);

    // USB A -> wire channel 6; DC4 -> wire channel 4.
    driver->set_switch(0, false);
    CHECK(wait_until([&] { return hub.received(">C6#"); }, std::chrono::milliseconds(500)));
    CHECK_FALSE(driver->get_switch(0));
    CHECK(wait_until([&] { return !hub.output(6); }, std::chrono::milliseconds(500)));
    driver->set_switch_value(9, 0.0);
    CHECK(wait_until([&] { return hub.received(">C4#"); }, std::chrono::milliseconds(500)));
    driver->set_switch(9, true);
    CHECK(wait_until([&] { return hub.received(">O4#"); }, std::chrono::milliseconds(500)));
    CHECK(driver->get_switch(9));

    // Manual mode: DEW6 value is a PWM percent via >X.
    driver->set_switch_value(11, 40.0);
    CHECK(wait_until([&] { return hub.received(">X40#"); }, std::chrono::milliseconds(500)));
    CHECK(driver->get_switch_value(11) == 40.0);
    CHECK(wait_until([&] { return hub.dew_manual_pwm(6) == 40; }, std::chrono::milliseconds(500)));
    // Quantised to the 1 % step before it goes on the wire.
    driver->set_switch_value(12, 33.4);
    CHECK(wait_until([&] { return hub.received(">Y33#"); }, std::chrono::milliseconds(500)));
    CHECK(driver->get_switch_value(12) == 33.0);

    // Switching DEW6 to Auto (>M10#) turns its output into an on/off enable (>Z1x#).
    driver->set_switch_value(13, 0.0);
    CHECK(wait_until([&] { return hub.received(">M10#"); }, std::chrono::milliseconds(500)));
    CHECK(driver->get_switch_value(13) == 0.0);
    CHECK(driver->get_max_switch_value(11) == 1.0);
    driver->set_switch(11, true);
    CHECK(wait_until([&] { return hub.received(">Z11#"); }, std::chrono::milliseconds(500)));
    CHECK(driver->get_switch(11));
    driver->set_switch(11, false);
    CHECK(wait_until([&] { return hub.received(">Z10#"); }, std::chrono::milliseconds(500)));
    // A PWM percent is now out of range for the Auto-mode on/off switch.
    require_alpaca_error([&]() { driver->set_switch_value(11, 40.0); }, alpacacore::AlpacaError::InvalidValue);

    // DEW7 to Switch mode (>M22#), back to Manual (>M21#).
    driver->set_switch_value(14, 2.0);
    CHECK(wait_until([&] { return hub.received(">M22#"); }, std::chrono::milliseconds(500)));
    CHECK(driver->get_max_switch_value(12) == 1.0);
    driver->set_switch_value(14, 1.0);
    CHECK(wait_until([&] { return hub.received(">M21#"); }, std::chrono::milliseconds(500)));
    CHECK(driver->get_max_switch_value(12) == 100.0);

    // Read-only ids reject writes with NotImplemented; the always-on rail never
    // produces a wire command.
    require_alpaca_error([&]() { driver->set_switch_value(6, 0.0); }, alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->set_switch(15, false); }, alpacacore::AlpacaError::NotImplemented);
    CHECK_FALSE(hub.received(">C1#"));
    CHECK_FALSE(hub.received(">O1#"));
}

TEST_CASE("Gemini PDH Switch Driver - Streamed status frames are routed to the cache, not the handshake",
          "[gemini][switch][unit][fake]") {
    FakeGeminiPdh hub;
    // Stream a frame every 30 ms from before connect: the handshake must still
    // find its *H reply among the interleaved *G frames.
    hub.set_stream_interval(std::chrono::milliseconds(30));
    auto driver = connect_fake_hub(hub);
    CHECK(driver->get_switch_value(15) == 13.1);

    // A change on the fake reaches the driver without any further >G# poll:
    // the count of >G# requests stays at what connect (and no writes) issued.
    const int polls_before = hub.count(">G#");
    hub.set_input_voltage(12.4);
    CHECK(wait_until([&] { return driver->get_switch_value(15) == 12.4; }, std::chrono::milliseconds(1000)));
    CHECK(hub.count(">G#") == polls_before);
}

TEST_CASE("Gemini PDH Switch Driver - Stale cache is re-polled when the hub does not stream",
          "[gemini][switch][unit][fake]") {
    FakeGeminiPdh hub;
    auto driver = connect_fake_hub(hub);
    const int polls_before = hub.count(">G#");
    // With a reply-only hub the reader re-polls >G# once the cache is older
    // than 2 s, so a change shows up within a few seconds and never blocks a read.
    hub.set_input_voltage(11.9);
    CHECK(wait_until([&] { return driver->get_switch_value(15) == 11.9; }, std::chrono::milliseconds(6000)));
    CHECK(hub.count(">G#") > polls_before);
}

// Issue #237: a hub that stops answering must not be served from the cache
// forever. Three silent polls (~6 s) latch a link fault: value reads and
// writes throw DriverException while Connected stays true, and the first
// frame to arrive again clears it without a reconnect.
TEST_CASE("Gemini PDH Switch Driver - Silent hub faults the link, and frames restore it",
          "[gemini][switch][unit][fake]") {
    FakeGeminiPdh hub;
    auto driver = connect_fake_hub(hub);
    CHECK(driver->get_switch_value(15) == 13.1);

    hub.set_muted(true);
    // Reads keep answering (from cache) until the fault latches, then throw.
    auto value_read_throws = [&] {
        try {
            (void)driver->get_switch_value(19);
            return false;
        } catch (const alpacacore::AlpacaException&) {
            return true;
        }
    };
    CHECK(wait_until(value_read_throws, std::chrono::milliseconds(12000)));
    CHECK(driver->get_connected());  // the client decides whether to reconnect
    require_alpaca_error([&]() { (void)driver->get_switch_value(19); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&]() { (void)driver->get_switch(0); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&]() { driver->set_switch_value(0, 0.0); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&]() { (void)driver->get_max_switch_value(11); }, alpacacore::AlpacaError::DriverException);
    // Static metadata does not depend on the hub and keeps answering.
    CHECK(driver->get_switch_name(0) == "USB A");
    CHECK(driver->get_can_write(0));
    CHECK(driver->get_max_switch_value(15) == 30.0);
    // DeviceState omits the unavailable members rather than failing outright.
    const auto state = driver->get_device_state();
    REQUIRE(state.size() == 1);
    CHECK(state.front().name == "TimeStamp");

    // The reader keeps polling; the next answered poll restores the link.
    hub.set_input_voltage(12.4);
    hub.set_muted(false);
    CHECK(wait_until([&] { return !value_read_throws(); }, std::chrono::milliseconds(6000)));
    CHECK(driver->get_switch_value(15) == 12.4);
    CHECK(driver->get_connected());
}

TEST_CASE("Gemini PDH Switch Driver - Dead serial link (EIO) faults the link instead of serving stale status",
          "[gemini][switch][unit][fake]") {
    FakeGeminiPdh hub;
    auto driver = connect_fake_hub(hub);
    driver->set_switch_value(0, 0.0);  // a commanded value that must NOT survive the fault
    CHECK(driver->get_switch_value(0) == 0.0);

    hub.sever_link();  // master side closed: writes on the slave are EIO, reads return 0
    auto value_read_throws = [&] {
        try {
            (void)driver->get_switch_value(0);
            return false;
        } catch (const alpacacore::AlpacaException&) {
            return true;
        }
    };
    CHECK(wait_until(value_read_throws, std::chrono::milliseconds(12000)));
    CHECK(driver->get_connected());
    // Commanded values are gated the same way as live telemetry.
    require_alpaca_error([&]() { (void)driver->get_switch_value(0); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&]() { (void)driver->get_switch_value(18); }, alpacacore::AlpacaError::DriverException);
    require_alpaca_error([&]() { driver->set_switch(1, true); }, alpacacore::AlpacaError::DriverException);
    // Disconnect is still clean with a dead fd.
    CHECK_NOTHROW(driver->set_connected(false));
    CHECK_FALSE(driver->get_connected());
}

// Issue #772 (CC-18): once the master side is gone the slave is hung up, and
// a read on it returns 0 at once instead of waiting out VTIME. The reader
// thread must back off on that early 0 rather than spin a full core.
TEST_CASE("Gemini PDH Switch Driver - Reader does not spin on a hung-up serial link", "[gemini][switch][unit][fake]") {
    FakeGeminiPdh hub;
    auto driver = connect_fake_hub(hub);
    auto process_cpu_s = [] {
        timespec ts{};
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
        return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
    };

    hub.sever_link();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const double start = process_cpu_s();
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const double used = process_cpu_s() - start;
    INFO("process CPU over 2 s after the cut: " << used << " s");
    CHECK(used < 0.5);
    CHECK_NOTHROW(driver->set_connected(false));
}

TEST_CASE("Gemini PDH Switch Driver - Old firmware is refused at connect", "[gemini][switch][unit][fake]") {
    FakeGeminiPdh hub;
    hub.set_firmware(305);
    auto driver = alpacacore::vendor::gemini::create_gemini_pdh_switch(0, hub.slave_path(), 19200);
    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(driver->get_connected());
    CHECK(hub.received(">V#"));
    CHECK_FALSE(hub.received(">G#"));
}

TEST_CASE("Gemini PDH Switch Driver - Concurrent DEW value and mode writes never disagree",
          "[gemini][switch][unit][fake]") {
    FakeGeminiPdh hub;
    auto driver = connect_fake_hub(hub);

    // One writer flips DEW6's mode between Manual and Switch while another
    // writes the DEW6 output at whatever max the driver currently reports.
    // Writes are serialized under the driver's write mutex, so the recorded
    // value must always sit inside the range of the mode it was validated
    // against -- an out-of-range InvalidValue here would mean a value was
    // checked against a mode that changed underneath it (PR #236 review).
    std::atomic<int> invalid_value_errors{0};
    std::atomic<int> other_errors{0};
    auto value_writer = [&] {
        for (int i = 0; i < 150; ++i) {
            try {
                driver->set_switch(11, (i % 2) == 0);
            } catch (const alpacacore::AlpacaException& e) {
                if (e.error_code() == alpacacore::AlpacaError::InvalidValue) {
                    invalid_value_errors.fetch_add(1);
                } else {
                    other_errors.fetch_add(1);
                }
            }
        }
    };
    auto mode_writer = [&] {
        for (int i = 0; i < 150; ++i) {
            try {
                driver->set_switch_value(13, (i % 2) == 0 ? 2.0 : 1.0);
            } catch (const alpacacore::AlpacaException&) {
                other_errors.fetch_add(1);
            }
        }
    };
    std::thread t1(value_writer);
    std::thread t2(mode_writer);
    t1.join();
    t2.join();
    CHECK(invalid_value_errors.load() == 0);
    CHECK(other_errors.load() == 0);
    // Final state is self-consistent: value within the max of the final mode.
    CHECK(driver->get_switch_value(11) <= driver->get_max_switch_value(11));
    CHECK(driver->get_switch_value(11) >= 0.0);
}
