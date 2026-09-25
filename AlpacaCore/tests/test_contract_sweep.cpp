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

// Cross-driver contract sweep, tier 1 (issue #571): every driver the router can
// construct, disconnected, no fake. The registry is contract_sweep.h; each
// (driver, case) pair below is its own TEST_CASE so `ctest -N` lists it by name
// and a failure names the driver. Tier 2 (connected over a fake) follows in a
// second PR.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/camera_driver.h>
#include <alpacacore/covercalibrator_driver.h>
#include <alpacacore/filterwheel_driver.h>
#include <alpacacore/focuser_driver.h>
#include <alpacacore/observingconditions_driver.h>
#include <alpacacore/rotator_driver.h>
#include <alpacacore/switch_driver.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "contract_sweep.h"

// Tier 2 hosts: the roster fakes, each under its vendor guard. Fakes over a pty or a loopback socket
// are POSIX only.
#ifndef _WIN32
#ifdef ALPACACORE_ENABLE_CELESTRON
#include <alpacacore/vendor/celestron/celestron_protocol_wrapper.h>
#include <alpacacore/vendor/celestron/celestron_telescope_driver.h>

#include "fake_mount_server.h"
#endif
#ifdef ALPACACORE_ENABLE_GEMINI
#include <alpacacore/vendor/gemini/gemini_focuser_driver.h>

#include "fake_gemini_focuser.h"
#endif
#endif

namespace {

using alpacacore::AlpacaDriver;
using alpacacore::AlpacaException;
using alpacacore::DeviceType;
using alpacacore::test::contract::ContractEntry;
namespace err = alpacacore::AlpacaError;

// The Alpaca error code `fn` throws, or -1 if it returns normally. Anything that
// is not an AlpacaException fails the calling case.
int thrown_code(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const AlpacaException& ex) {
        return ex.error_code();
    } catch (const std::exception& ex) {
        FAIL("non-Alpaca exception escaped: " << ex.what());
    }
    return -1;
}

using Probe = std::pair<std::string, std::function<void()>>;

// Out-of-range arguments that must be rejected with InvalidValue before the
// connection check (AGENTS.md ASCOM contract precedence).
std::vector<Probe> invalid_value_probes(AlpacaDriver& d, DeviceType type) {
    std::vector<Probe> p;
    switch (type) {
        case DeviceType::Telescope: {
            auto& t = dynamic_cast<alpacacore::TelescopeDriver&>(d);
            p.push_back({"get_can_move_axis(-1)", [&] { (void)t.get_can_move_axis(-1); }});
            p.push_back({"get_can_move_axis(3)", [&] { (void)t.get_can_move_axis(3); }});
            break;
        }
        case DeviceType::Switch: {
            auto& s = dynamic_cast<alpacacore::SwitchDriver&>(d);
            p.push_back({"get_switch_name(-1)", [&] { (void)s.get_switch_name(-1); }});
            break;
        }
        case DeviceType::FilterWheel: {
            auto& f = dynamic_cast<alpacacore::FilterWheelDriver&>(d);
            p.push_back({"set_position(-1)", [&] { f.set_position(-1); }});
            break;
        }
        case DeviceType::CoverCalibrator: {
            auto& c = dynamic_cast<alpacacore::CoverCalibratorDriver&>(d);
            p.push_back({"calibrator_on(-1)", [&] { c.calibrator_on(-1); }});
            break;
        }
        case DeviceType::Rotator: {
            // NaN is a static invalid argument: InvalidValue must win over NotConnected.
            auto& r = dynamic_cast<alpacacore::RotatorDriver&>(d);
            // NaN is built inside each lambda: the probes run after this block's scope has ended.
            p.push_back(
                {"set_target_position(NaN)", [&] { r.set_target_position(std::numeric_limits<double>::quiet_NaN()); }});
            p.push_back({"move(NaN)", [&] { r.move(std::numeric_limits<double>::quiet_NaN()); }});
            p.push_back({"sync(NaN)", [&] { r.sync(std::numeric_limits<double>::quiet_NaN()); }});
            p.push_back({"move_absolute(NaN)", [&] { r.move_absolute(std::numeric_limits<double>::quiet_NaN()); }});
            p.push_back({"move_mechanical(NaN)", [&] { r.move_mechanical(std::numeric_limits<double>::quiet_NaN()); }});
            break;
        }
        default:
            break;
    }
    return p;
}

// Operational properties and methods that must throw NotConnected while
// disconnected, with no early return that skips the check.
std::vector<Probe> not_connected_probes(AlpacaDriver& d, DeviceType type) {
    std::vector<Probe> p;
    switch (type) {
        case DeviceType::Telescope: {
            auto& t = dynamic_cast<alpacacore::TelescopeDriver&>(d);
            p.push_back({"get_right_ascension", [&] { (void)t.get_right_ascension(); }});
            p.push_back({"get_declination", [&] { (void)t.get_declination(); }});
            p.push_back({"get_altitude", [&] { (void)t.get_altitude(); }});
            p.push_back({"get_azimuth", [&] { (void)t.get_azimuth(); }});
            p.push_back({"get_tracking", [&] { (void)t.get_tracking(); }});
            p.push_back({"get_slewing", [&] { (void)t.get_slewing(); }});
            p.push_back({"get_at_park", [&] { (void)t.get_at_park(); }});
            p.push_back({"slew_to_coordinates", [&] { t.slew_to_coordinates(1.0, 1.0); }});
            p.push_back({"abort_slew", [&] { t.abort_slew(); }});
            break;
        }
        case DeviceType::Switch: {
            auto& s = dynamic_cast<alpacacore::SwitchDriver&>(d);
            p.push_back({"get_switch(0)", [&] { (void)s.get_switch(0); }});
            p.push_back({"get_switch_value(0)", [&] { (void)s.get_switch_value(0); }});
            // Write probe on the first writable id: iOptron's iMate switch 0 is a read-only
            // pass-through that throws NotImplemented before the connection check (AGENTS.md).
            int writable = -1;
            try {
                for (int id = 0; id < s.get_max_switch() && writable < 0; ++id) {
                    if (s.get_can_write(id)) writable = id;
                }
            } catch (const AlpacaException&) {
                // max_switch not answerable while disconnected: no write probe for this driver.
            }
            if (writable >= 0) {
                p.push_back({"set_switch(writable)", [&s, writable] { s.set_switch(writable, false); }});
            }
            break;
        }
        case DeviceType::FilterWheel: {
            auto& f = dynamic_cast<alpacacore::FilterWheelDriver&>(d);
            p.push_back({"get_position", [&] { (void)f.get_position(); }});
            p.push_back({"set_position(0)", [&] { f.set_position(0); }});
            break;
        }
        case DeviceType::Focuser: {
            auto& f = dynamic_cast<alpacacore::FocuserDriver&>(d);
            p.push_back({"get_position", [&] { (void)f.get_position(); }});
            p.push_back({"get_is_moving", [&] { (void)f.get_is_moving(); }});
            p.push_back({"move(0)", [&] { f.move(0); }});
            p.push_back({"halt", [&] { f.halt(); }});
            break;
        }
        case DeviceType::Rotator: {
            auto& r = dynamic_cast<alpacacore::RotatorDriver&>(d);
            p.push_back({"get_position", [&] { (void)r.get_position(); }});
            p.push_back({"get_is_moving", [&] { (void)r.get_is_moving(); }});
            p.push_back({"move(0)", [&] { r.move(0.0); }});
            p.push_back({"halt", [&] { r.halt(); }});
            break;
        }
        case DeviceType::CoverCalibrator: {
            auto& c = dynamic_cast<alpacacore::CoverCalibratorDriver&>(d);
            p.push_back({"get_cover_state", [&] { (void)c.get_cover_state(); }});
            p.push_back({"get_calibrator_state", [&] { (void)c.get_calibrator_state(); }});
            p.push_back({"get_brightness", [&] { (void)c.get_brightness(); }});
            // No open_cover probe: the Gemini Cover Lite has no cover and answers NotImplemented.
            break;
        }
        case DeviceType::ObservingConditions: {
            auto& o = dynamic_cast<alpacacore::ObservingConditionsDriver&>(d);
            p.push_back({"get_temperature", [&] { (void)o.get_temperature(); }});
            p.push_back({"get_humidity", [&] { (void)o.get_humidity(); }});
            p.push_back({"get_pressure", [&] { (void)o.get_pressure(); }});
            break;
        }
        case DeviceType::Camera: {
            auto& c = dynamic_cast<alpacacore::CameraDriver&>(d);
            // get_ccd_temperature and get_image_ready are not probed: qhy answers the first and
            // playerone, svbony, gphoto and touptek answer the second while disconnected.
            p.push_back({"get_gain", [&] { (void)c.get_gain(); }});
            p.push_back({"start_exposure", [&] { c.start_exposure(1.0, true); }});
            break;
        }
        default:
            break;
    }
    return p;
}

// Static capability getters: readable while disconnected (they must not throw).
std::vector<Probe> can_getter_probes(AlpacaDriver& d, DeviceType type) {
    std::vector<Probe> p;
    switch (type) {
        case DeviceType::Telescope: {
            auto& t = dynamic_cast<alpacacore::TelescopeDriver&>(d);
            p.push_back({"get_can_slew", [&] { (void)t.get_can_slew(); }});
            p.push_back({"get_can_slew_async", [&] { (void)t.get_can_slew_async(); }});
            p.push_back({"get_can_slew_alt_az", [&] { (void)t.get_can_slew_alt_az(); }});
            p.push_back({"get_can_sync", [&] { (void)t.get_can_sync(); }});
            p.push_back({"get_can_park", [&] { (void)t.get_can_park(); }});
            p.push_back({"get_can_unpark", [&] { (void)t.get_can_unpark(); }});
            p.push_back({"get_can_find_home", [&] { (void)t.get_can_find_home(); }});
            p.push_back({"get_can_pulse_guide", [&] { (void)t.get_can_pulse_guide(); }});
            p.push_back({"get_can_set_tracking", [&] { (void)t.get_can_set_tracking(); }});
            p.push_back({"get_can_set_declination_rate", [&] { (void)t.get_can_set_declination_rate(); }});
            p.push_back({"get_can_set_guide_rates", [&] { (void)t.get_can_set_guide_rates(); }});
            p.push_back({"get_can_set_park", [&] { (void)t.get_can_set_park(); }});
            p.push_back({"get_can_set_pier_side", [&] { (void)t.get_can_set_pier_side(); }});
            p.push_back({"get_can_set_right_ascension_rate", [&] { (void)t.get_can_set_right_ascension_rate(); }});
            p.push_back({"get_can_slew_alt_az_async", [&] { (void)t.get_can_slew_alt_az_async(); }});
            p.push_back({"get_can_sync_alt_az", [&] { (void)t.get_can_sync_alt_az(); }});
            break;
        }
        case DeviceType::Camera: {
            auto& c = dynamic_cast<alpacacore::CameraDriver&>(d);
            p.push_back({"get_can_abort_exposure", [&] { (void)c.get_can_abort_exposure(); }});
            p.push_back({"get_can_stop_exposure", [&] { (void)c.get_can_stop_exposure(); }});
            p.push_back({"get_can_pulse_guide", [&] { (void)c.get_can_pulse_guide(); }});
            p.push_back({"get_can_asymmetric_bin", [&] { (void)c.get_can_asymmetric_bin(); }});
            p.push_back({"get_can_fast_readout", [&] { (void)c.get_can_fast_readout(); }});
            p.push_back({"get_can_get_cooler_power", [&] { (void)c.get_can_get_cooler_power(); }});
            p.push_back({"get_can_set_ccd_temperature", [&] { (void)c.get_can_set_ccd_temperature(); }});
            break;
        }
        case DeviceType::Rotator: {
            auto& r = dynamic_cast<alpacacore::RotatorDriver&>(d);
            p.push_back({"get_can_reverse", [&] { (void)r.get_can_reverse(); }});
            break;
        }
        case DeviceType::Focuser: {
            auto& f = dynamic_cast<alpacacore::FocuserDriver&>(d);
            p.push_back({"get_absolute", [&] { (void)f.get_absolute(); }});
            break;
        }
        default:
            break;
    }
    return p;
}

// ---- case bodies ----------------------------------------------------------
// [[maybe_unused]]: a vendors-off build expands CONTRACT_SWEEP_ENTRIES to nothing, so no case
// references them and -Werror=unused-function would otherwise fail that build.

[[maybe_unused]] void case_invalid_before_not_connected(const ContractEntry& e) {
    auto d = e.make(0);
    REQUIRE(d != nullptr);
    REQUIRE_FALSE(d->get_connected());
    const auto probes = invalid_value_probes(*d, e.type);
    if (probes.empty()) {
        INFO("no static out-of-range probe: " << e.no_invalid_probe_reason);
        REQUIRE(std::string(e.no_invalid_probe_reason).size() > 0);
        return;
    }
    for (const auto& [name, fn] : probes) {
        INFO(e.id << " " << name);
        CHECK(thrown_code(fn) == err::InvalidValue);
    }
}

[[maybe_unused]] void case_operations_throw_not_connected(const ContractEntry& e) {
    auto d = e.make(0);
    REQUIRE(d != nullptr);
    const auto probes = not_connected_probes(*d, e.type);
    REQUIRE_FALSE(probes.empty());
    for (const auto& [name, fn] : probes) {
        INFO(e.id << " " << name);
        CHECK(thrown_code(fn) == err::NotConnected);
    }
}

[[maybe_unused]] void case_device_state_empty(const ContractEntry& e) {
    auto d = e.make(0);
    REQUIRE(d != nullptr);
    const auto state = d->get_device_state();
    CHECK(state.empty());
    for (const auto& s : state) {
        INFO("unexpected DeviceState entry " << s.name);
        CHECK(s.name != "TimeStamp");
    }
}

[[maybe_unused]] void case_interface_version(const ContractEntry& e) {
    auto d = e.make(0);
    REQUIRE(d != nullptr);
    CHECK(d->get_device_type() == e.type);
    CHECK(d->get_interface_version() == e.interface_version);
}

[[maybe_unused]] void case_driver_version(const ContractEntry& e) {
    auto d = e.make(0);
    REQUIRE(d != nullptr);
    CHECK(d->get_driver_version() == alpacacore::kVersion);
}

[[maybe_unused]] void case_unique_id(const ContractEntry& e) {
    auto a = e.make(0);
    auto b = e.make(3);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK_FALSE(a->get_unique_id().empty());
    CHECK_FALSE(b->get_unique_id().empty());
    CHECK(a->get_unique_id() != b->get_unique_id());
}

[[maybe_unused]] void case_unsupported_action(const ContractEntry& e) {
    auto d = e.make(0);
    REQUIRE(d != nullptr);
    auto advertised = d->get_supported_actions();
    auto expected = e.actions;
    std::sort(advertised.begin(), advertised.end());
    std::sort(expected.begin(), expected.end());
    CHECK(advertised == expected);
    for (const auto& name : e.actions) {
        INFO("advertised action " << name);
        CHECK(d->can_action(name));
    }
    CHECK_FALSE(d->can_action("no-such-action"));
    const int a = thrown_code([&] { (void)d->action("no-such-action", ""); });
    CHECK(a == err::ActionNotImplemented);
    // Command*: forwarded to the device (NotConnected while disconnected) or unsupported.
    const int want = e.command_passthrough ? err::NotConnected : err::NotImplemented;
    CHECK(thrown_code([&] { d->command_blind("x"); }) == want);
    CHECK(thrown_code([&] { (void)d->command_bool("x"); }) == want);
    CHECK(thrown_code([&] { (void)d->command_string("x"); }) == want);
}

// Every error a disconnected driver raises is in the ASCOM-reserved range, and
// "not supported" is never the generic DriverException.
[[maybe_unused]] void case_error_vocabulary(const ContractEntry& e) {
    auto d = e.make(0);
    REQUIRE(d != nullptr);
    std::vector<Probe> all;
    for (auto&& p : invalid_value_probes(*d, e.type)) all.push_back(std::move(p));
    for (auto&& p : not_connected_probes(*d, e.type)) all.push_back(std::move(p));
    all.push_back({"action", [&] { (void)d->action("no-such-action", ""); }});
    all.push_back({"command_blind", [&] { d->command_blind("x"); }});
    all.push_back({"command_bool", [&] { (void)d->command_bool("x"); }});
    all.push_back({"command_string", [&] { (void)d->command_string("x"); }});
    for (const auto& [name, fn] : all) {
        INFO(e.id << " " << name);
        const int code = thrown_code(fn);
        if (code == -1) continue;  // returned normally: the NotConnected case owns that
        CHECK(code != err::DriverException);
        CHECK(code != err::UnspecifiedError);
        CHECK(code >= 0x400);
        CHECK(code < 0x500);
    }
}

[[maybe_unused]] void case_can_getters(const ContractEntry& e) {
    auto d = e.make(0);
    REQUIRE(d != nullptr);
    for (const auto& [name, fn] : can_getter_probes(*d, e.type)) {
        INFO(e.id << " " << name);
        CHECK(thrown_code(fn) == -1);
    }
    if (e.type == DeviceType::Telescope) {
        auto& t = dynamic_cast<alpacacore::TelescopeDriver&>(*d);
        CHECK(t.get_can_move_axis(0) == e.can_move_axis[0]);
        CHECK(t.get_can_move_axis(1) == e.can_move_axis[1]);
        CHECK(t.get_can_move_axis(2) == e.can_move_axis[2]);
    }
}

// ---------------------------------------------------------------------------
// Tier 2 (issue #655): the connected half of the sweep, one host per
// kFakeConnectableRoster row. contract_sweep.h keeps the roster a literal
// three-field array because docs-drift check 13 parses it; the connect recipe and
// the per-case expectations live here in CS2_HOSTS, and a non-vacuity case pins the
// two lists to each other in both directions.
// ---------------------------------------------------------------------------
#ifndef _WIN32

// A driver together with the fake it talks to. The fake is declared first so it is
// destroyed after the driver.
struct Hosted {
    std::shared_ptr<void> fake;
    std::unique_ptr<AlpacaDriver> driver;
};

struct Tier2Host {
    const char* id;           // ctest stem: <vendor>_<devicetype>, plus _serial for the second Sky-Watcher fake
    const char* vendor;       // roster row
    const char* device_type;  // roster row
    const char* fake_header;  // roster row
    DeviceType type;
    const char* hosts_registry_id;  // the tier-1 registry entry whose backend the fake exercises
    // A driver over the fake that reaches Connected. `hold` asks the fake to keep the handshake
    // open long enough that Connecting is observably true; a host that cannot says so through
    // can_hold_connect.
    std::function<Hosted(bool hold)> connectable;
    bool can_hold_connect;
    // A driver whose connect must fail, or empty with failing_unavailable naming why and the source.
    std::function<Hosted()> failing;
    const char* failing_unavailable;
};

using Clock = std::chrono::steady_clock;

// Handshake hold for fakes that have the knob, and the part of it Connected is checked inside.
constexpr std::chrono::milliseconds kHoldDelay{300};
constexpr std::chrono::milliseconds kHoldWindow{100};

struct ConnectObservation {
    bool saw_connecting = false;
    bool connected_early = false;  // Connected read true inside the hold window, before the handshake ended
    bool settled = false;
};

// Connect() the way the router does (async), then poll until the task finishes. set_connected() is
// deliberately not used: it is the synchronous PUT path and never exercises Connecting.
// Connected and Connecting may both read true for an instant at the tail of a connect (the task calls
// set_connected(true) and only then publishes Idle, async_connectable.h run_connection_task), so overlap
// is not asserted. `hold_window` is the stretch in which a fake that holds its handshake guarantees the
// connect is still open: Connected must read false throughout it.
ConnectObservation connect_and_observe(AlpacaDriver& d, std::chrono::milliseconds hold_window = {},
                                       std::chrono::milliseconds budget = std::chrono::seconds(30)) {
    ConnectObservation o;
    d.connect();
    const auto t0 = Clock::now();
    const auto deadline = t0 + budget;
    while (Clock::now() < deadline) {
        const bool connecting = d.get_connecting();
        const bool connected = d.get_connected();
        if (connecting) o.saw_connecting = true;
        if (connected && Clock::now() - t0 < hold_window) o.connected_early = true;
        if (!connecting) {
            o.settled = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return o;
}

Hosted connected_host(const Tier2Host& h) {
    Hosted hosted = h.connectable(false);
    REQUIRE(hosted.driver != nullptr);
    const auto o = connect_and_observe(*hosted.driver);
    REQUIRE(o.settled);
    REQUIRE(hosted.driver->get_connected());
    return hosted;
}

// Case 1. Connect returns at once; Connecting reads true, then false; Connected reads true only
// after. A connect that fails leaves Connected false and keeps the driver's reason.
[[maybe_unused]] void case_t2_connecting(const Tier2Host& h) {
    INFO(h.id << " over " << h.fake_header);
    {
        Hosted hosted = h.connectable(h.can_hold_connect);
        REQUIRE(hosted.driver != nullptr);
        CHECK_FALSE(hosted.driver->get_connected());
        const auto t0 = Clock::now();
        const auto o = connect_and_observe(*hosted.driver, h.can_hold_connect ? kHoldWindow : std::chrono::milliseconds{});
        CHECK(o.settled);
        CHECK(hosted.driver->get_connected());
        CHECK_FALSE(hosted.driver->get_connecting());
        if (h.can_hold_connect) {
            INFO("the fake holds the handshake for " << kHoldDelay.count() << " ms: Connecting reads true and Connected false");
            CHECK(o.saw_connecting);
            CHECK_FALSE(o.connected_early);
        }
        CHECK(Clock::now() - t0 < std::chrono::seconds(30));
    }
    if (!h.failing) {
        INFO(h.id << " cannot host the failed-connect leg: " << h.failing_unavailable);
        REQUIRE(std::string(h.failing_unavailable).size() > 0);
        return;
    }
    Hosted hosted = h.failing();
    REQUIRE(hosted.driver != nullptr);
    const auto o = connect_and_observe(*hosted.driver);
    CHECK(o.settled);
    CHECK_FALSE(hosted.driver->get_connected());
    CHECK_FALSE(hosted.driver->get_connecting());
    const std::string reason = hosted.driver->get_last_connect_error();
    INFO("get_last_connect_error() after a failed connect: '" << reason << "'");
    CHECK_FALSE(reason.empty());
    // The reason stays until the next attempt clears it.
    CHECK(hosted.driver->get_last_connect_error() == reason);
}

// Case 2, telescope rows only. AGENTS.md (TargetRightAscension and TargetDeclination are independent):
// each getter throws ValueNotSet until that property itself is written. Issue #655 says
// InvalidOperation; every driver and every per-vendor test uses ValueNotSet (0x402), so that is the rule.
[[maybe_unused]] void case_t2_target_flags(const Tier2Host& h) {
    if (h.type != DeviceType::Telescope) {
        SUCCEED(std::string(h.id) + ": target flags apply to telescopes only");
        return;
    }
    Hosted hosted = connected_host(h);
    auto& t = dynamic_cast<alpacacore::TelescopeDriver&>(*hosted.driver);
    CHECK(thrown_code([&] { (void)t.get_target_right_ascension(); }) == err::ValueNotSet);
    CHECK(thrown_code([&] { (void)t.get_target_declination(); }) == err::ValueNotSet);
    t.set_target_right_ascension(5.5);
    CHECK(t.get_target_right_ascension() == 5.5);
    CHECK(thrown_code([&] { (void)t.get_target_declination(); }) == err::ValueNotSet);
    t.set_target_declination(20.25);
    CHECK(t.get_target_right_ascension() == 5.5);
    CHECK(t.get_target_declination() == 20.25);
}

// Case 3. The out-of-range inputs tier 1 probes while disconnected are still InvalidValue when connected.
[[maybe_unused]] void case_t2_invalid_value_connected(const Tier2Host& h) {
    Hosted hosted = connected_host(h);
    const auto probes = invalid_value_probes(*hosted.driver, h.type);
    if (probes.empty()) {
        const std::string reason = alpacacore::test::contract::invalid_probe_reason_for(h.type);
        INFO(h.id << " has no static out-of-range probe: " << reason);
        CHECK_FALSE(reason.empty());
        return;
    }
    for (const auto& [name, fn] : probes) {
        INFO(h.id << " " << name);
        CHECK(thrown_code(fn) == err::InvalidValue);
    }
}

// Case 4. Connected DeviceState is non-empty and carries TimeStamp (base classes, PR #625).
[[maybe_unused]] void case_t2_device_state_connected(const Tier2Host& h) {
    Hosted hosted = connected_host(h);
    const auto state = hosted.driver->get_device_state();
    CHECK_FALSE(state.empty());
    const bool has_timestamp = std::any_of(state.begin(), state.end(), [](const alpacacore::DeviceState& s) {
        return s.name == "TimeStamp";
    });
    CHECK(has_timestamp);
}

#ifdef ALPACACORE_ENABLE_CELESTRON
// FakeMountServer answering every command with a position-pair sized reply; the recipe of
// test_celestron_concurrency_stress.cpp.
Tier2Host tier2_host_celestron_telescope() {
    namespace cel = alpacacore::vendor::celestron;
    auto endpoint = [](int port) {
        cel::ConnectionInfo info;
        info.type = cel::ConnectionType::Network;
        info.host = "127.0.0.1";
        info.tcp_port = port;
        info.response_timeout_ms = 50;
        return info;
    };
    Tier2Host h{"celestron_telescope", "celestron", "telescope", "fake_mount_server.h", DeviceType::Telescope,
                "celestron_telescope", {}, false, {}, ""};
    h.connectable = [endpoint](bool) {
        auto server = std::make_shared<alpacacore::test::FakeMountServer>(
            [](const std::string&) { return std::string("00000000,00000000#"); });
        REQUIRE(server->ok());
        Hosted hosted;
        hosted.driver = cel::create_celestron_telescope(0, endpoint(server->port()));
        hosted.fake = server;
        return hosted;
    };
    h.failing = [endpoint]() {
        int port = 0;
        {
            alpacacore::test::FakeMountServer probe;  // a port nothing listens on once it is gone
            port = probe.port();
        }
        Hosted hosted;
        hosted.driver = cel::create_celestron_telescope(0, endpoint(port));
        return hosted;
    };
    return h;
}
#endif

#ifdef ALPACACORE_ENABLE_GEMINI
// FakeGeminiFocuser over a pty; its handshake delay is the hold knob that makes Connecting
// deterministic (recipe of test_gemini_focuser.cpp).
Tier2Host tier2_host_gemini_focuser() {
    Tier2Host h{"gemini_focuser", "gemini", "focuser", "fake_gemini_focuser.h", DeviceType::Focuser,
                "gemini_focuser", {}, true, {}, ""};
    h.connectable = [](bool hold) {
        auto fake = std::make_shared<alpacacore::test::FakeGeminiFocuser>();
        if (hold) fake->set_handshake_delay(kHoldDelay);
        Hosted hosted;
        hosted.driver = alpacacore::vendor::gemini::create_gemini_focuser(0, fake->slave_path());
        hosted.fake = fake;
        return hosted;
    };
    h.failing = []() {
        Hosted hosted;
        hosted.driver = alpacacore::vendor::gemini::create_gemini_focuser(0, "/dev/alpacacore-no-such-port");
        return hosted;
    };
    return h;
}
#endif

// One X(id) per hosted roster row, under the vendor's guard.
// clang-format off
#ifdef ALPACACORE_ENABLE_CELESTRON
#define CS2_CELESTRON(X) X(celestron_telescope)
#else
#define CS2_CELESTRON(X)
#endif
#ifdef ALPACACORE_ENABLE_GEMINI
#define CS2_GEMINI(X) X(gemini_focuser)
#else
#define CS2_GEMINI(X)
#endif
#define CONTRACT_SWEEP_TIER2_HOSTS(X) \
    CS2_CELESTRON(X) \
    CS2_GEMINI(X)
// clang-format on

#endif  // !_WIN32

}  // namespace

// One TEST_CASE per (entry, case). The name carries the registry id, so a
// failure and `ctest -R` both name the driver.
#define CS_CASE(id, casename, body)                                                   \
    TEST_CASE("Contract sweep - " #id " - " casename, "[contract][tier1][" #id "]") { \
        body(alpacacore::test::contract::contract_entry_##id());                      \
    }

#define CS_T1_INVALID(id) CS_CASE(id, "InvalidValue precedes NotConnected", case_invalid_before_not_connected)
#define CS_T1_NOTCONN(id) CS_CASE(id, "operations throw NotConnected", case_operations_throw_not_connected)
#define CS_T1_STATE(id) CS_CASE(id, "disconnected DeviceState is empty", case_device_state_empty)
#define CS_T1_IFACE(id) CS_CASE(id, "InterfaceVersion is Platform 7", case_interface_version)
#define CS_T1_DRVVER(id) CS_CASE(id, "DriverVersion is kVersion", case_driver_version)
#define CS_T1_UID(id) CS_CASE(id, "UniqueID distinct per device number", case_unique_id)
#define CS_T1_ACTION(id) CS_CASE(id, "unsupported action contract", case_unsupported_action)
#define CS_T1_ERRVOCAB(id) CS_CASE(id, "error code vocabulary", case_error_vocabulary)
#define CS_T1_CAN(id) CS_CASE(id, "Can getters", case_can_getters)

CONTRACT_SWEEP_ENTRIES(CS_T1_INVALID)
CONTRACT_SWEEP_ENTRIES(CS_T1_NOTCONN)
CONTRACT_SWEEP_ENTRIES(CS_T1_STATE)
CONTRACT_SWEEP_ENTRIES(CS_T1_IFACE)
CONTRACT_SWEEP_ENTRIES(CS_T1_DRVVER)
CONTRACT_SWEEP_ENTRIES(CS_T1_UID)
CONTRACT_SWEEP_ENTRIES(CS_T1_ACTION)
CONTRACT_SWEEP_ENTRIES(CS_T1_ERRVOCAB)
CONTRACT_SWEEP_ENTRIES(CS_T1_CAN)

#ifndef _WIN32
#define CS2_CASE(id, casename, body)                                                          \
    TEST_CASE("Contract sweep tier 2 - " #id " - " casename, "[contract][tier2][" #id "]") { \
        body(tier2_host_##id());                                                              \
    }
#define CS2_CONNECTING(id) CS2_CASE(id, "Connecting semantics", case_t2_connecting)
#define CS2_TARGETS(id) CS2_CASE(id, "target flags until set", case_t2_target_flags)
#define CS2_INVALID(id) CS2_CASE(id, "InvalidValue wins while connected", case_t2_invalid_value_connected)
#define CS2_STATE(id) CS2_CASE(id, "connected DeviceState has TimeStamp", case_t2_device_state_connected)

CONTRACT_SWEEP_TIER2_HOSTS(CS2_CONNECTING)
CONTRACT_SWEEP_TIER2_HOSTS(CS2_TARGETS)
CONTRACT_SWEEP_TIER2_HOSTS(CS2_INVALID)
CONTRACT_SWEEP_TIER2_HOSTS(CS2_STATE)
#endif

// Non-vacuity guard. The vendor ALPACACORE_ENABLE_<V> macros are not inherited
// from the vendor targets: tests/CMakeLists.txt must define them for
// alpacacore_tests, or the registry compiles to nothing and every case above
// silently disappears. Each macro that IS defined must contribute an entry.
TEST_CASE("Contract sweep - registry is not vacuous", "[contract][contract-sweep-guard]") {
    const auto entries = alpacacore::test::contract::contract_entries();
    std::set<std::string> vendors;
    std::set<std::string> ids;
    for (const auto& e : entries) {
        vendors.insert(e.vendor);
        CHECK(ids.insert(e.id).second);
        // A second backend behind one router pair carries a suffix: "<vendor>_<devicetype>_<backend>".
        CHECK(std::string(e.id).rfind(std::string(e.vendor) + "_" + e.device_type, 0) == 0);
        {
            const std::string src = e.source;
            INFO(e.id << " source must name its basis: protocol document, hardware run or assumption");
            CHECK((src.find("protocol document") != std::string::npos ||
                   src.find("hardware run") != std::string::npos || src.find("assumption") != std::string::npos));
        }
    }
#define CS_EXPECT_VENDOR(macro, name)                                                       \
    do {                                                                                    \
        INFO("ALPACACORE_ENABLE_" macro " is defined but no " name " entry is registered"); \
        CHECK(vendors.count(name) == 1);                                                    \
    } while (0)
#ifdef ALPACACORE_ENABLE_ZWO
    CS_EXPECT_VENDOR("ZWO", "zwo");
#endif
#ifdef ALPACACORE_ENABLE_QHY
    CS_EXPECT_VENDOR("QHY", "qhy");
#endif
#ifdef ALPACACORE_ENABLE_IOPTRON
    CS_EXPECT_VENDOR("IOPTRON", "ioptron");
#endif
#ifdef ALPACACORE_ENABLE_SYNSCAN
    CS_EXPECT_VENDOR("SYNSCAN", "synscan");
#endif
#ifdef ALPACACORE_ENABLE_SKYWATCHER
    CS_EXPECT_VENDOR("SKYWATCHER", "skywatcher");
#endif
#ifdef ALPACACORE_ENABLE_ONSTEP
    CS_EXPECT_VENDOR("ONSTEP", "onstep");
#endif
#ifdef ALPACACORE_ENABLE_WEEWX
    CS_EXPECT_VENDOR("WEEWX", "weewx");
#endif
#ifdef ALPACACORE_ENABLE_GEMINI
    CS_EXPECT_VENDOR("GEMINI", "gemini");
#endif
#ifdef ALPACACORE_ENABLE_SVBONY
    CS_EXPECT_VENDOR("SVBONY", "svbony");
#endif
#ifdef ALPACACORE_ENABLE_CELESTRON
    CS_EXPECT_VENDOR("CELESTRON", "celestron");
#endif
#ifdef ALPACACORE_ENABLE_BISQUE
    CS_EXPECT_VENDOR("BISQUE", "bisque");
#endif
#ifdef ALPACACORE_ENABLE_TOUPTEK
    CS_EXPECT_VENDOR("TOUPTEK", "touptek");
#endif
#ifdef ALPACACORE_ENABLE_PLAYERONE
    CS_EXPECT_VENDOR("PLAYERONE", "playerone");
#endif
#ifdef ALPACACORE_ENABLE_WANDERERASTRO
    CS_EXPECT_VENDOR("WANDERERASTRO", "wandererastro");
#endif
#ifdef ALPACACORE_ENABLE_ASTROASIS
    CS_EXPECT_VENDOR("ASTROASIS", "astroasis");
#endif
#ifdef ALPACACORE_ENABLE_GPHOTO
    CS_EXPECT_VENDOR("GPHOTO", "gphoto");
#endif
#undef CS_EXPECT_VENDOR
    // Entries under a second macro: the vendor check above passes without them, so pin the entry itself.
#if defined(ALPACACORE_ENABLE_IOPTRON) && defined(ALPACACORE_IOPTRON_POWERBOX)
    CHECK(ids.count("ioptron_switch") == 1);
#endif
#if defined(ALPACACORE_ENABLE_TOUPTEK) && defined(ALPACACORE_TOUPTEK_STELLAVITA)
    CHECK(ids.count("touptek_switch") == 1);
#endif
}
