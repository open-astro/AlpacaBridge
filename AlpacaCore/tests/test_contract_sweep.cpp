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
// and a failure names the driver. Tier 2 (connected over a roster fake) is further
// down, one host per kFakeConnectableRoster row.

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
#include <tuple>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "contract_sweep.h"

// Tier 2 hosts: the roster fakes, each under its vendor guard. Fakes over a pty or a loopback socket
// are POSIX only.
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef ALPACACORE_ENABLE_ZWO
#include <alpacacore/vendor/zwo/zwo_telescope_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_CELESTRON
#include <alpacacore/vendor/celestron/celestron_protocol_wrapper.h>
#include <alpacacore/vendor/celestron/celestron_telescope_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_SYNSCAN
#include <alpacacore/vendor/synscan/synscan_protocol_wrapper.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_ONSTEP
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>
#include <alpacacore/vendor/onstep/onstep_telescope_driver.h>
#endif
#if defined(ALPACACORE_ENABLE_ZWO) || defined(ALPACACORE_ENABLE_CELESTRON) || defined(ALPACACORE_ENABLE_SYNSCAN) || \
    defined(ALPACACORE_ENABLE_ONSTEP)
#include "fake_mount_server.h"
#endif
#ifdef ALPACACORE_ENABLE_SKYWATCHER
#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>

#include "fake_skywatcher_mount.h"
#include "fake_skywatcher_serial_board.h"
#endif
#ifdef ALPACACORE_ENABLE_IOPTRON
#include <alpacacore/vendor/ioptron/ioptron_ieaf_focuser_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>

#include "fake_ioptron_ieaf.h"
#include "fake_ioptron_mount.h"
#endif
#ifdef ALPACACORE_ENABLE_GEMINI
#include <alpacacore/vendor/gemini/gemini_flatpanel_driver.h>
#include <alpacacore/vendor/gemini/gemini_focuser_driver.h>
#include <alpacacore/vendor/gemini/gemini_pdh_switch_driver.h>

#include "fake_gemini_flatpanel.h"
#include "fake_gemini_focuser.h"
#include "fake_gemini_pdh.h"
#endif
#ifdef ALPACACORE_ENABLE_QHY
#include <alpacacore/vendor/qhy/qhy_camera_driver.h>
#include <alpacacore/vendor/qhy/qhy_cfw3_filterwheel_driver.h>
#include <alpacacore/vendor/qhy/qhy_focuser_driver.h>

#include "fake_qhy_cfw3.h"
#include "fake_qhy_qfocuser.h"
#include "fake_qhy_sdk.h"
#include "locked_qhy_sdk.h"
#endif
#ifdef ALPACACORE_ENABLE_TOUPTEK
#include <alpacacore/vendor/touptek/touptek_camera_driver.h>

#include "fake_touptek_sdk.h"
#include "locked_touptek_sdk.h"
#endif
#ifdef ALPACACORE_ENABLE_GPHOTO
#include <alpacacore/vendor/gphoto/gphoto_camera_driver.h>

#include "fake_gphoto_sdk.h"
#include "fake_raw_decoder.h"
#include "locked_gphoto_sdk.h"
#endif
#ifdef ALPACACORE_ENABLE_WANDERERASTRO
#include <alpacacore/vendor/wandererastro/wandererastro_box_switch_driver.h>
#include <alpacacore/vendor/wandererastro/wandererastro_covercalibrator_driver.h>
#include <alpacacore/vendor/wandererastro/wandererastro_filterwheel_driver.h>

#include "fake_serial_streamer.h"
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
std::vector<Probe> not_connected_probes(AlpacaDriver& d, DeviceType type, const ContractEntry& entry) {
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
            p.push_back({"get_at_home", [&] { (void)t.get_at_home(); }});
            p.push_back({"slew_to_coordinates", [&] { t.slew_to_coordinates(1.0, 1.0); }});
            p.push_back({"abort_slew", [&] { t.abort_slew(); }});
            break;
        }
        case DeviceType::Switch: {
            auto& s = dynamic_cast<alpacacore::SwitchDriver&>(d);
            p.push_back({"get_switch(0)", [&] { (void)s.get_switch(0); }});
            p.push_back({"get_switch_value(0)", [&] { (void)s.get_switch_value(0); }});
            // Write probe on the id the registry entry names (never scanned from CanWrite, which throws
            // NotConnected on most switches while disconnected and used to drop the probe silently).
            // iOptron's iMate switch 0 is a read-only pass-through that throws NotImplemented before the
            // connection check (AGENTS.md), which is why the id is named per entry.
            const int writable = entry.switch_writable_id;
            p.push_back({"set_switch(writable id)", [&s, writable] { s.set_switch(writable, false); }});
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
            p.push_back({"get_gain", [&] { (void)c.get_gain(); }});
            p.push_back({"get_image_ready", [&] { (void)c.get_image_ready(); }});
            p.push_back({"get_ccd_temperature", [&] { (void)c.get_ccd_temperature(); }});
            p.push_back({"get_cooler_on", [&] { (void)c.get_cooler_on(); }});
            p.push_back({"get_set_ccd_temperature", [&] { (void)c.get_set_ccd_temperature(); }});
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
    const auto probes = not_connected_probes(*d, e.type, e);
    REQUIRE_FALSE(probes.empty());
    for (const auto& [name, fn] : probes) {
        INFO(e.id << " " << name);
        CHECK(thrown_code(fn) == err::NotConnected);
    }
    if (e.type == DeviceType::Switch) {
        auto& s = dynamic_cast<alpacacore::SwitchDriver&>(*d);
        const int id = e.switch_writable_id;
        INFO(e.id << " switch_caps_source: " << e.switch_caps_source);
        REQUIRE(id >= 0);
        REQUIRE(std::string(e.switch_caps_source).size() > 0);
        using alpacacore::test::contract::DisconnectedRead;
        // Static: the read returns. NotConnected: it throws that code. Any other code is a defect.
        const auto expect = [&](const char* name, DisconnectedRead mode, const std::function<void()>& fn) {
            INFO(e.id << " " << name);
            CHECK(thrown_code(fn) == (mode == DisconnectedRead::Static ? -1 : err::NotConnected));
        };
        expect("get_max_switch", e.switch_max_disconnected, [&] { (void)s.get_max_switch(); });
        expect("get_can_write(id)", e.switch_caps_disconnected, [&] { (void)s.get_can_write(id); });
        expect("get_can_async(id)", e.switch_caps_disconnected, [&] { (void)s.get_can_async(id); });
        if (e.switch_max_disconnected == DisconnectedRead::Static) {
            INFO(e.id << " writable id " << id << " must be inside the static MaxSwitch");
            CHECK(s.get_max_switch() > id);
        }
    }
}

[[maybe_unused]] void case_device_state_empty(const ContractEntry& e) {
    auto d = e.make(0);
    REQUIRE(d != nullptr);
    const auto state = d->get_device_state();
    CHECK(state.empty());
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
    for (auto&& p : not_connected_probes(*d, e.type, e)) all.push_back(std::move(p));
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
// the per-case expectations live here in CONTRACT_SWEEP_TIER2_HOSTS, and a non-vacuity case pins the
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
    // A row with can_hold_connect == false cannot show Connecting reading true (its fake completes the
    // handshake at once), so it states why and the source here; the case then asserts only that the first
    // sample after connect() is Connecting or Connected. Never left empty for such a row.
    const char* connecting_unobservable = nullptr;
};

using Clock = std::chrono::steady_clock;

// Handshake hold for fakes that have the knob, and the part of it Connected is checked inside.
constexpr std::chrono::milliseconds kHoldDelay{300};
constexpr std::chrono::milliseconds kHoldWindow{100};
// Per-row holds, each longer than kHoldWindow and shorter than that row's own reply timeout.
constexpr std::chrono::milliseconds kSerialBoardHold{150};  // Sky-Watcher serial: response_timeout_ms is 300
constexpr std::chrono::milliseconds kFlatPanelHold{300};    // Gemini flat panel: handshake read timeout is 2 s
constexpr std::chrono::milliseconds kQhyOpenHold{300};      // QHY: the SDK open_camera call, no reply timeout
constexpr std::chrono::milliseconds kHold{300};  // generic hold for rows whose reply timeout is >= 1.5 s or has none
constexpr std::chrono::milliseconds kUdpMountHold{150};  // Sky-Watcher UDP: response_timeout_ms is 250
constexpr std::chrono::milliseconds kCfw3Hold{200};      // QHY CFW3: reply_timeout_ms is 300 (VRS reply held)
// Upper bound on how long the connect() call itself may take (it must hand back without waiting for the
// handshake). A constant of its own, never a hold length: it is not derived from, and must not equal, any hold.
constexpr std::chrono::milliseconds kConnectCallBound{1000};
constexpr int kHeldReplyTimeoutMs = 1000;  // mount-server rows raise their reply timeout to this in the hold case

struct ConnectObservation {
    bool saw_connecting = false;
    bool first_sample_in_flight = false;   // right after connect() returned: Connecting or already Connected
    bool first_sample_connecting = false;  // Connecting read true on the very first sample after connect() returned
    std::chrono::milliseconds connect_call{0};  // how long the connect() call itself took
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
    const auto call_start = Clock::now();
    d.connect();
    const auto t0 = Clock::now();
    o.connect_call = std::chrono::duration_cast<std::chrono::milliseconds>(t0 - call_start);
    const auto deadline = t0 + budget;
    bool first = true;
    while (Clock::now() < deadline) {
        const bool connecting = d.get_connecting();
        const bool connected = d.get_connected();
        if (first) {
            o.first_sample_in_flight = connecting || connected;
            // Connected is not part of this: on the drivers whose get_connected() blocks behind the connect
            // (async_connectable.h), that read only returns once the connect has finished.
            o.first_sample_connecting = connecting;
        }
        first = false;
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
        const auto o =
            connect_and_observe(*hosted.driver, h.can_hold_connect ? kHoldWindow : std::chrono::milliseconds{});
        CHECK(o.settled);
        CHECK(o.first_sample_in_flight);
        CHECK(hosted.driver->get_connected());
        CHECK_FALSE(hosted.driver->get_connecting());
        if (!h.can_hold_connect) {
            INFO(h.id << " cannot show Connecting reading true: "
                      << (h.connecting_unobservable != nullptr ? h.connecting_unobservable : "(no reason stated)"));
            REQUIRE(h.connecting_unobservable != nullptr);
            REQUIRE(std::string(h.connecting_unobservable).size() > 0);
        } else {
            INFO("the fake holds the handshake open past the "
                 << kHoldWindow.count() << " ms window: Connecting reads true and Connected false");
            // A synchronous connect cannot pass these: connect() must return before the shortest hold ends, the
            // very first sample must read Connecting true, and Connected stays false for the window.
            CHECK(o.connect_call < kConnectCallBound);
            CHECK(o.first_sample_connecting);
            CHECK(o.saw_connecting);
            // On rows whose get_connected() blocks behind the connect (async_connectable.h) this check cannot fail:
            // that read only returns after the hold. It is load-bearing on 9 of the 19 rows (red with the holds
            // set to 0); the PR body lists them.
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
    // A second read returns the same reason (reading does not consume it). Whether a later attempt
    // clears it is not asserted here.
    CHECK(hosted.driver->get_last_connect_error() == reason);
}

// Case 2, telescope rows only. AGENTS.md (TargetRightAscension and TargetDeclination are independent):
// each getter throws ValueNotSet until that property itself is written. Issue #655 says
// InvalidOperation; every driver and every per-vendor test uses ValueNotSet (0x402), so that is the rule.
[[maybe_unused]] void case_t2_target_flags(const Tier2Host& h) {
    // Registered over telescope hosts only (kind TEL below); the guard case pins that.
    REQUIRE(h.type == DeviceType::Telescope);
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
    // Registered only over hosts whose type has probes (kinds TEL and PRB below); a type without a static
    // out-of-range input states why through invalid_probe_reason_for() in contract_sweep.h, checked by the guard.
    const auto probes = invalid_value_probes(*hosted.driver, h.type);
    REQUIRE_FALSE(probes.empty());
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
    const bool has_timestamp =
        std::any_of(state.begin(), state.end(), [](const alpacacore::DeviceState& s) { return s.name == "TimeStamp"; });
    CHECK(has_timestamp);
}

template <class Fake, class Make>
Hosted host_over(std::shared_ptr<Fake> fake, Make make) {
    Hosted hosted;
    hosted.driver = make(*fake);
    hosted.fake = std::move(fake);
    return hosted;
}

// A loopback endpoint nothing else can take for the length of a failed-connect leg. The old recipe
// destroyed a fake server and connected to its port, which another process (ctest -j) could bind in
// between. This socket stays bound for the lifetime of the Hosted value: a TCP one never listens, so a
// connect is refused; a UDP one never replies, so the exchange times out.
class HeldPort {
public:
    explicit HeldPort(int sock_type) {
        fd_ = ::socket(AF_INET, sock_type, 0);
        REQUIRE(fd_ >= 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        REQUIRE(::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        socklen_t len = sizeof(addr);
        REQUIRE(::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        port_ = ntohs(addr.sin_port);
    }
    ~HeldPort() {
        if (fd_ >= 0) ::close(fd_);
    }
    HeldPort(const HeldPort&) = delete;
    HeldPort& operator=(const HeldPort&) = delete;
    int port() const { return port_; }

private:
    int fd_ = -1;
    int port_ = 0;
};

template <class Make>
Hosted host_over_held_port(int sock_type, Make make) {
    auto held = std::make_shared<HeldPort>(sock_type);
    Hosted hosted;
    hosted.driver = make(held->port());
    hosted.fake = held;
    return hosted;
}

// A serial device path that does not exist: the connect fails at open().
constexpr const char* kAbsentPort = "/dev/alpacacore-contract-sweep-absent";

#if defined(ALPACACORE_ENABLE_ZWO) || defined(ALPACACORE_ENABLE_CELESTRON) || defined(ALPACACORE_ENABLE_SYNSCAN) || \
    defined(ALPACACORE_ENABLE_ONSTEP)
// FakeMountServer rows share one shape: a loopback endpoint on the fake for the connect, and a held,
// never-listening loopback port for the failed connect.
template <class MakeDriver>
Tier2Host mount_server_host(const char* id, const char* vendor, const char* registry_id,
                            alpacacore::test::FakeMountServer::Responder responder, MakeDriver make) {
    Tier2Host h{id, vendor, "telescope", "fake_mount_server.h", DeviceType::Telescope, registry_id, {}, false, {}, ""};
    h.can_hold_connect = true;
    // FakeMountServer::Responder (fake_mount_server.h) runs before every reply, so a responder that sleeps on
    // the first chunk holds the connect with no fake change. The driver's reply timeout is raised for the
    // hold case (make(port, hold)) so the held reply is late, not lost.
    h.connectable = [responder, make](bool hold) {
        auto first = std::make_shared<std::atomic<bool>>(hold);
        auto held = [responder, first](const std::string& chunk) {
            if (first->exchange(false)) std::this_thread::sleep_for(kHold);
            return responder(chunk);
        };
        auto server = std::make_shared<alpacacore::test::FakeMountServer>(held);
        REQUIRE(server->ok());
        Hosted hosted;
        hosted.driver = make(server->port(), hold);
        hosted.fake = server;
        return hosted;
    };
    h.failing = [make]() { return host_over_held_port(SOCK_STREAM, [make](int port) { return make(port, false); }); };
    return h;
}
#endif

#ifdef ALPACACORE_ENABLE_ZWO
// Recipe of test_zwo_concurrency_stress.cpp: LX200 replies, tracking reported on.
// Knob, no fake change: the responder also accepts :Sr/:Sd so the target setters succeed.
Tier2Host tier2_host_zwo_telescope() {
    namespace zwo = alpacacore::vendor::zwo;
    return mount_server_host(
        "zwo_telescope", "zwo", "zwo_telescope",
        [](const std::string& chunk) -> std::string {
            // Tracking on, and the target setters (:Sr, :Sd) accepted; "0#" would make the driver report
            // "ZWO mount rejected target RA".
            return (chunk.find(":GAT") != std::string::npos || chunk.find(":Sr") != std::string::npos ||
                    chunk.find(":Sd") != std::string::npos)
                       ? "1#"
                       : "0#";
        },
        [](int port, bool hold) -> std::unique_ptr<AlpacaDriver> {
            zwo::ConnectionInfo info;
            info.type = zwo::ConnectionType::Network;
            info.host = "127.0.0.1";
            info.tcp_port = port;
            info.response_timeout_ms = hold ? kHeldReplyTimeoutMs : 250;
            return zwo::create_zwo_telescope(0, info);
        });
}
#endif

#ifdef ALPACACORE_ENABLE_CELESTRON
// Recipe of test_celestron_concurrency_stress.cpp: a position-pair sized reply ends every read at once.
Tier2Host tier2_host_celestron_telescope() {
    namespace cel = alpacacore::vendor::celestron;
    return mount_server_host(
        "celestron_telescope", "celestron", "celestron_telescope",
        [](const std::string&) { return std::string("00000000,00000000#"); },
        [](int port, bool hold) -> std::unique_ptr<AlpacaDriver> {
            cel::ConnectionInfo info;
            info.type = cel::ConnectionType::Network;
            info.host = "127.0.0.1";
            info.tcp_port = port;
            info.response_timeout_ms = hold ? kHeldReplyTimeoutMs : 50;
            return cel::create_celestron_telescope(0, info);
        });
}
#endif

#ifdef ALPACACORE_ENABLE_SYNSCAN
// Recipe of test_synscan_concurrency_stress.cpp: V4 and the default responder's 'K' echo, which the connect gates on.
Tier2Host tier2_host_synscan_telescope() {
    namespace syn = alpacacore::vendor::synscan;
    return mount_server_host("synscan_telescope", "synscan", "synscan_telescope",
                             alpacacore::test::FakeMountServer::default_responder(),
                             [](int port, bool hold) -> std::unique_ptr<AlpacaDriver> {
                                 syn::ConnectionInfo info;
                                 info.type = syn::ConnectionType::Network;
                                 info.host = "127.0.0.1";
                                 info.tcp_port = port;
                                 info.response_timeout_ms = hold ? kHeldReplyTimeoutMs : 50;
                                 return syn::create_synscan_telescope(0, info, syn::SynScanVersion::V4);
                             });
}
#endif

#ifdef ALPACACORE_ENABLE_ONSTEP
// Recipe of test_onstep_concurrency_stress.cpp.
Tier2Host tier2_host_onstep_telescope() {
    namespace ons = alpacacore::vendor::onstep;
    return mount_server_host("onstep_telescope", "onstep", "onstep_telescope",
                             alpacacore::test::FakeMountServer::default_responder(),
                             [](int port, bool hold) -> std::unique_ptr<AlpacaDriver> {
                                 ons::ConnectionInfo info;
                                 info.type = ons::ConnectionType::Network;
                                 info.host = "127.0.0.1";
                                 info.tcp_port = port;
                                 info.response_timeout_ms = hold ? kHeldReplyTimeoutMs : 50;
                                 return ons::create_onstep_telescope(0, info);
                             });
}
#endif

#ifdef ALPACACORE_ENABLE_SKYWATCHER
namespace sw = alpacacore::vendor::skywatcher;

// Recipe of test_skywatcher_concurrency_stress.cpp: the UDP mount fake.
Tier2Host tier2_host_skywatcher_telescope() {
    auto make = [](int port) -> std::unique_ptr<AlpacaDriver> {
        sw::ConnectionInfo info;
        info.type = sw::ConnectionType::Network;
        info.host = "127.0.0.1";
        info.udp_port = port;
        info.response_timeout_ms = 250;
        return sw::create_skywatcher_telescope(0, info, 39.7392, -104.9903, 1609.0);
    };
    Tier2Host h{"skywatcher_telescope",
                "skywatcher",
                "telescope",
                "fake_skywatcher_mount.h",
                DeviceType::Telescope,
                "skywatcher_telescope",
                {},
                true,
                {},
                ""};
    h.connectable = [make](bool hold) {
        auto mount = std::make_shared<alpacacore::test::FakeSkyWatcherMount>();
        if (hold) mount->hold_next_reply(kUdpMountHold);
        return host_over(mount, [&](const alpacacore::test::FakeSkyWatcherMount& m) { return make(m.port()); });
    };
    h.failing = [make]() { return host_over_held_port(SOCK_DGRAM, make); };
    return h;
}

// Recipe of test_skywatcher_serial.cpp: the serial motor-board fake.
Tier2Host tier2_host_skywatcher_telescope_serial() {
    auto make = [](const std::string& path) -> std::unique_ptr<AlpacaDriver> {
        sw::ConnectionInfo info;
        info.type = sw::ConnectionType::Serial;
        info.port_path = path;
        info.baud_rate = 9600;
        info.response_timeout_ms = 300;
        return sw::create_skywatcher_telescope(0, info, -37.0, 175.0, 50.0);
    };
    Tier2Host h{"skywatcher_telescope_serial",
                "skywatcher",
                "telescope",
                "fake_skywatcher_serial_board.h",
                DeviceType::Telescope,
                "skywatcher_telescope",
                {},
                true,
                {},
                ""};
    // delay_next_reply(ms) with no command holds the reply to the next frame of any kind. protocol.connect()
    // only opens the port (skywatcher_protocol_wrapper.cpp connect_serial), so the next frame is the first
    // one the driver's set_connected sends, inside the connect task.
    h.connectable = [make](bool hold) {
        auto board = std::make_shared<alpacacore::test::FakeSkyWatcherSerialBoard>();
        if (hold) board->delay_next_reply(static_cast<int>(kSerialBoardHold.count()));
        return host_over(board,
                         [&](const alpacacore::test::FakeSkyWatcherSerialBoard& b) { return make(b.slave_path()); });
    };
    h.failing = [make]() {
        Hosted hosted;
        hosted.driver = make(kAbsentPort);
        return hosted;
    };
    return h;
}
#endif

#ifdef ALPACACORE_ENABLE_IOPTRON
// Recipe of test_ioptron_telescope.cpp (HAE16 EQ, 0012) and test_ioptron_ieaf_focuser.cpp.
Tier2Host tier2_host_ioptron_telescope() {
    namespace iop = alpacacore::vendor::ioptron;
    auto make = [](int port) -> std::unique_ptr<AlpacaDriver> {
        iop::ConnectionInfo info;
        info.type = iop::ConnectionType::Network;
        info.host = "127.0.0.1";
        info.tcp_port = port;
        return iop::create_ioptron_telescope(0, info);
    };
    Tier2Host h{"ioptron_telescope",
                "ioptron",
                "telescope",
                "fake_ioptron_mount.h",
                DeviceType::Telescope,
                "ioptron_telescope",
                {},
                true,
                {},
                ""};
    h.connectable = [make](bool hold) {
        auto mount = std::make_shared<alpacacore::test::FakeIoptronMount>("0012", 12.0);
        REQUIRE(mount->ok());
        if (hold) mount->hold_next_reply(kHold);
        return host_over(mount, [&](const alpacacore::test::FakeIoptronMount& m) { return make(m.port()); });
    };
    h.failing = [make]() { return host_over_held_port(SOCK_STREAM, make); };
    return h;
}

Tier2Host tier2_host_ioptron_focuser() {
    Tier2Host h{"ioptron_focuser",
                "ioptron",
                "focuser",
                "fake_ioptron_ieaf.h",
                DeviceType::Focuser,
                "ioptron_focuser",
                {},
                true,
                {},
                ""};
    h.connectable = [](bool hold) {
        auto ieaf = std::make_shared<alpacacore::test::FakeIoptronIeaf>();
        if (hold) ieaf->hold_next_reply(kHold);
        return host_over(ieaf, [](const alpacacore::test::FakeIoptronIeaf& f) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::ioptron::create_ieaf_focuser(0, f.slave_path());
        });
    };
    h.failing = []() {
        Hosted hosted;
        hosted.driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, kAbsentPort);
        return hosted;
    };
    return h;
}
#endif

#ifdef ALPACACORE_ENABLE_GEMINI
// FakeGeminiFocuser over a pty; its handshake delay is the hold knob that makes Connecting
// deterministic (recipe of test_gemini_focuser.cpp).
Tier2Host tier2_host_gemini_focuser() {
    Tier2Host h{"gemini_focuser",
                "gemini",
                "focuser",
                "fake_gemini_focuser.h",
                DeviceType::Focuser,
                "gemini_focuser",
                {},
                true,
                {},
                ""};
    h.connectable = [](bool hold) {
        auto fake = std::make_shared<alpacacore::test::FakeGeminiFocuser>();
        if (hold) fake->set_handshake_delay(kHoldDelay);
        return host_over(fake, [](const alpacacore::test::FakeGeminiFocuser& f) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::gemini::create_gemini_focuser(0, f.slave_path());
        });
    };
    h.failing = []() {
        Hosted hosted;
        hosted.driver = alpacacore::vendor::gemini::create_gemini_focuser(0, kAbsentPort);
        return hosted;
    };
    return h;
}

// fake_gemini_flatpanel.h models the Pro firmware only, so this row hosts the gemini_covercalibrator_pro
// registry entry; Cover Lite and v2 have no connected coverage (source: the fake header).
Tier2Host tier2_host_gemini_covercalibrator() {
    Tier2Host h{"gemini_covercalibrator",
                "gemini",
                "covercalibrator",
                "fake_gemini_flatpanel.h",
                DeviceType::CoverCalibrator,
                "gemini_covercalibrator_pro",
                {},
                true,
                {},
                ""};
    // The Pro connect sleeps 100 ms, sends ">H#" and reads the identity reply with a 2 s timeout
    // (gemini_flatpanel_protocol_wrapper.cpp Impl::connect), so holding that reply holds the connect.
    h.connectable = [](bool hold) {
        auto panel = std::make_shared<alpacacore::test::FakeGeminiFlatPanel>();
        if (hold) panel->set_reply_delay(">H#", kFlatPanelHold);
        return host_over(panel, [](const alpacacore::test::FakeGeminiFlatPanel& f) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::gemini::create_gemini_flatpanel_pro(0, f.slave_path(), 9600);
        });
    };
    h.failing = []() {
        Hosted hosted;
        hosted.driver = alpacacore::vendor::gemini::create_gemini_flatpanel_pro(0, kAbsentPort, 9600);
        return hosted;
    };
    return h;
}

Tier2Host tier2_host_gemini_switch() {
    Tier2Host h{"gemini_switch",
                "gemini",
                "switch",
                "fake_gemini_pdh.h",
                DeviceType::Switch,
                "gemini_switch",
                {},
                true,
                {},
                ""};
    h.connectable = [](bool hold) {
        auto pdh = std::make_shared<alpacacore::test::FakeGeminiPdh>();
        if (hold) pdh->hold_next_reply(kHold);
        return host_over(pdh, [](const alpacacore::test::FakeGeminiPdh& f) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::gemini::create_gemini_pdh_switch(0, f.slave_path(), 19200);
        });
    };
    h.failing = []() {
        Hosted hosted;
        hosted.driver = alpacacore::vendor::gemini::create_gemini_pdh_switch(0, kAbsentPort, 19200);
        return hosted;
    };
    return h;
}
#endif

#ifdef ALPACACORE_ENABLE_QHY
// FakeQHYSDK behind LockedQHYSDK (recipe of test_qhy_camera.cpp). Both live in one holder: the driver keeps a
// reference to the SDK.
struct QhySdkHold {
    alpacacore::test::FakeQHYSDK fake = alpacacore::test::FakeQHYSDK::with_one_camera("fake-qhy-0");
    alpacacore::test::LockedQHYSDK sdk{fake};
};

Tier2Host tier2_host_qhy_camera() {
    Tier2Host h{"qhy_camera", "qhy", "camera", "fake_qhy_sdk.h", DeviceType::Camera, "qhy_camera", {}, true, {}, ""};
    // The driver's connect calls sdk_.open_camera(id) (qhy_camera_driver.cpp), which the fake reports through
    // hit("open_camera"): the sanctioned before_call hook blocks that one named call. Set before the driver
    // exists and matched on the name only, so no other call is ever held.
    h.connectable = [](bool hold) {
        auto sdk_hold = std::make_shared<QhySdkHold>();
        if (hold) {
            sdk_hold->fake.before_call = [](const std::string& name) {
                if (name == "open_camera") std::this_thread::sleep_for(kQhyOpenHold);
            };
        }
        return host_over(sdk_hold, [](QhySdkHold& s) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", s.sdk);
        });
    };
    h.failing = []() {
        auto hold = std::make_shared<QhySdkHold>();
        hold->fake.sdk_resource_available = false;  // fake_qhy_sdk.h: the resource load fails
        return host_over(hold, [](QhySdkHold& s) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", s.sdk);
        });
    };
    return h;
}

// The registry entry qhy_filterwheel_cfw3 is the backend this fake models.
Tier2Host tier2_host_qhy_filterwheel() {
    auto settings = []() {
        alpacacore::vendor::qhy::Cfw3Settings s;  // a pty never resets the fake: keep the boot wait short
        s.boot_timeout_ms = 150;
        s.reply_timeout_ms = 300;
        s.move_timeout_ms = 3000;
        return s;
    };
    Tier2Host h{"qhy_filterwheel",
                "qhy",
                "filterwheel",
                "fake_qhy_cfw3.h",
                DeviceType::FilterWheel,
                "qhy_filterwheel_cfw3",
                {},
                true,
                {},
                ""};
    h.connectable = [settings](bool hold) {
        auto wheel = std::make_shared<alpacacore::test::FakeQhyCfw3>();
        if (hold) wheel->hold_next_reply(kCfw3Hold);
        return host_over(wheel, [&](const alpacacore::test::FakeQhyCfw3& f) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::qhy::create_qhy_cfw3_filterwheel(0, f.slave_path(), settings());
        });
    };
    h.failing = [settings]() {
        Hosted hosted;
        hosted.driver = alpacacore::vendor::qhy::create_qhy_cfw3_filterwheel(0, kAbsentPort, settings());
        return hosted;
    };
    return h;
}

Tier2Host tier2_host_qhy_focuser() {
    Tier2Host h{"qhy_focuser", "qhy", "focuser", "fake_qhy_qfocuser.h", DeviceType::Focuser, "qhy_focuser", {},
                true,          {},    ""};
    h.connectable = [](bool hold) {
        auto focuser = std::make_shared<alpacacore::test::FakeQhyQFocuser>();
        if (hold) focuser->hold_next_reply(kHold);
        return host_over(focuser, [](const alpacacore::test::FakeQhyQFocuser& f) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::qhy::create_qhy_focuser(0, f.slave_path());
        });
    };
    h.failing = []() {
        Hosted hosted;
        hosted.driver = alpacacore::vendor::qhy::create_qhy_focuser(0, kAbsentPort);
        return hosted;
    };
    return h;
}
#endif

#ifdef ALPACACORE_ENABLE_TOUPTEK
struct ToupTekSdkHold {
    alpacacore::test::FakeToupTekSDK fake;
    alpacacore::test::LockedToupTekSDK sdk{fake};
    ToupTekSdkHold() {
        fake.cameras.push_back(alpacacore::test::FakeToupTekSDK::default_camera("fake-cam-0", "FakeCam One"));
    }
};

// Recipe of test_touptek_concurrency_stress.cpp.
Tier2Host tier2_host_touptek_camera() {
    Tier2Host h{"touptek_camera",
                "touptek",
                "camera",
                "fake_touptek_sdk.h",
                DeviceType::Camera,
                "touptek_camera",
                {},
                true,
                {},
                ""};
    h.connectable = [](bool hold) {
        auto sdk_hold = std::make_shared<ToupTekSdkHold>();
        if (hold) {
            sdk_hold->fake.before_call = [](const std::string& name) {
                if (name == "open_camera_by_id") std::this_thread::sleep_for(kHold);
            };
        }
        return host_over(sdk_hold, [](ToupTekSdkHold& s) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::touptek::create_touptek_camera(0, 0, s.sdk);
        });
    };
    h.failing = []() {
        auto hold = std::make_shared<ToupTekSdkHold>();
        hold->fake.throw_from.insert("open_camera_by_id");  // fake_touptek_sdk.h fault injection
        return host_over(hold, [](ToupTekSdkHold& s) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::touptek::create_touptek_camera(0, 0, s.sdk);
        });
    };
    return h;
}
#endif

#ifdef ALPACACORE_ENABLE_GPHOTO
struct GPhotoSdkHold {
    alpacacore::test::FakeGPhotoSDK fake;
    alpacacore::test::LockedGPhotoSDK sdk{fake};
    alpacacore::test::FakeRawDecoder decoder;
    explicit GPhotoSdkHold(bool with_camera = true) {
        alpacacore::test::reset_gphoto_sensor_cache();
        if (!with_camera) return;
        alpacacore::test::FakeGPhotoSDK::FakeCamera cam;
        cam.model = alpacacore::test::unique_test_model("Nikon DSC D5300 (contract sweep)");
        cam.port = "usb:001,099";
        cam.choices["iso"] = {"100", "200", "400"};
        cam.choice_value["iso"] = "200";
        cam.choices["shutterspeed2"] = {"1/200", "1", "bulb"};
        cam.choice_value["shutterspeed2"] = "1/200";
        cam.toggle_value["bulb"] = false;
        fake.cameras.push_back(cam);
    }
};

// Recipe of test_gphoto_concurrency_stress.cpp.
Tier2Host tier2_host_gphoto_camera() {
    Tier2Host h{"gphoto_camera",
                "gphoto",
                "camera",
                "fake_gphoto_sdk.h",
                DeviceType::Camera,
                "gphoto_camera",
                {},
                true,
                {},
                ""};
    h.connectable = [](bool hold) {
        auto sdk_hold = std::make_shared<GPhotoSdkHold>();
        if (hold) {
            sdk_hold->fake.before_call = [](const std::string& name) {
                if (name == "open_camera") std::this_thread::sleep_for(kHold);
            };
        }
        return host_over(sdk_hold, [](GPhotoSdkHold& s) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, s.sdk, s.decoder);
        });
    };
    h.failing = []() {
        // No camera at index 0, the case test_gphoto_fake_sdk.cpp pins ("connect with no camera at the index").
        return host_over(std::make_shared<GPhotoSdkHold>(false), [](GPhotoSdkHold& s) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, s.sdk, s.decoder);
        });
    };
    return h;
}
#endif

#ifdef ALPACACORE_ENABLE_WANDERERASTRO
// Frames copied from test_wandererastro_concurrency_stress.cpp (they live in that file's anonymous namespace).
constexpr const char* kWandererCoverFrame = "WandererCoverV4ProA20240301A10.0A270.0A10.0A12.5A0A0A0\n";
constexpr const char* kWandererSfwFrame = "WSFW368A20260124A3ABCDEFGHIA0A0A0A0A0A0A0A0A1A\n";
constexpr const char* kWandererBoxFrame =
    "ZXWBProV3A20250410A-127.00A-127.00A-127.00A45.20A21.30A1.50A0.20A0.30A13.10A1A1A1A1A1A1A0A0A0A1A1A120A\n";

Tier2Host tier2_host_wandererastro_covercalibrator() {
    Tier2Host h{"wandererastro_covercalibrator",
                "wandererastro",
                "covercalibrator",
                "fake_serial_streamer.h",
                DeviceType::CoverCalibrator,
                "wandererastro_covercalibrator",
                {},
                true,
                {},
                ""};
    h.connectable = [](bool hold) {
        auto streamer = std::make_shared<alpacacore::test::FakeSerialStreamer>(
            kWandererCoverFrame, std::chrono::milliseconds(50), hold ? kHold : std::chrono::milliseconds(0));
        return host_over(streamer, [](const alpacacore::test::FakeSerialStreamer& f) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::wandererastro::create_wandererastro_covercalibrator(0, f.slave_path());
        });
    };
    h.failing = []() {
        Hosted hosted;
        hosted.driver = alpacacore::vendor::wandererastro::create_wandererastro_covercalibrator(0, kAbsentPort);
        return hosted;
    };
    return h;
}

Tier2Host tier2_host_wandererastro_filterwheel() {
    Tier2Host h{"wandererastro_filterwheel",
                "wandererastro",
                "filterwheel",
                "fake_serial_streamer.h",
                DeviceType::FilterWheel,
                "wandererastro_filterwheel",
                {},
                true,
                {},
                ""};
    h.connectable = [](bool hold) {
        auto streamer = std::make_shared<alpacacore::test::FakeSerialStreamer>(
            kWandererSfwFrame, std::chrono::milliseconds(50), hold ? kHold : std::chrono::milliseconds(0));
        return host_over(streamer, [](const alpacacore::test::FakeSerialStreamer& f) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::wandererastro::create_wandererastro_filterwheel(0, f.slave_path());
        });
    };
    h.failing = []() {
        Hosted hosted;
        hosted.driver = alpacacore::vendor::wandererastro::create_wandererastro_filterwheel(0, kAbsentPort);
        return hosted;
    };
    return h;
}

Tier2Host tier2_host_wandererastro_switch() {
    Tier2Host h{"wandererastro_switch",
                "wandererastro",
                "switch",
                "fake_serial_streamer.h",
                DeviceType::Switch,
                "wandererastro_switch",
                {},
                true,
                {},
                ""};
    h.connectable = [](bool hold) {
        auto streamer = std::make_shared<alpacacore::test::FakeSerialStreamer>(
            kWandererBoxFrame, std::chrono::milliseconds(50), hold ? kHold : std::chrono::milliseconds(0));
        return host_over(streamer, [](const alpacacore::test::FakeSerialStreamer& f) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::wandererastro::create_wandererastro_box_switch(0, f.slave_path());
        });
    };
    h.failing = []() {
        Hosted hosted;
        hosted.driver = alpacacore::vendor::wandererastro::create_wandererastro_box_switch(0, kAbsentPort);
        return hosted;
    };
    return h;
}
#endif

// One X(id, kind) per hosted roster row, under the vendor's guard. kind decides which cases the row is
// registered for, so no case is registered where it would assert nothing: TEL = telescope (target flags and
// InvalidValue apply), PRB = other type with static out-of-range probes (InvalidValue applies), NPR = a type
// with no such probe (invalid_probe_reason_for() in contract_sweep.h states why; the guard case pins it).
// clang-format off
#ifdef ALPACACORE_ENABLE_ZWO
#define CS2_ZWO(X) X(zwo_telescope, TEL)
#else
#define CS2_ZWO(X)
#endif
#ifdef ALPACACORE_ENABLE_CELESTRON
#define CS2_CELESTRON(X) X(celestron_telescope, TEL)
#else
#define CS2_CELESTRON(X)
#endif
#ifdef ALPACACORE_ENABLE_SYNSCAN
#define CS2_SYNSCAN(X) X(synscan_telescope, TEL)
#else
#define CS2_SYNSCAN(X)
#endif
#ifdef ALPACACORE_ENABLE_ONSTEP
#define CS2_ONSTEP(X) X(onstep_telescope, TEL)
#else
#define CS2_ONSTEP(X)
#endif
#ifdef ALPACACORE_ENABLE_SKYWATCHER
#define CS2_SKYWATCHER(X) X(skywatcher_telescope, TEL) X(skywatcher_telescope_serial, TEL)
#else
#define CS2_SKYWATCHER(X)
#endif
#ifdef ALPACACORE_ENABLE_IOPTRON
#define CS2_IOPTRON(X) X(ioptron_telescope, TEL) X(ioptron_focuser, NPR)
#else
#define CS2_IOPTRON(X)
#endif
#ifdef ALPACACORE_ENABLE_GEMINI
#define CS2_GEMINI(X) X(gemini_focuser, NPR) X(gemini_covercalibrator, PRB) X(gemini_switch, PRB)
#else
#define CS2_GEMINI(X)
#endif
#ifdef ALPACACORE_ENABLE_QHY
#define CS2_QHY(X) X(qhy_camera, NPR) X(qhy_filterwheel, PRB) X(qhy_focuser, NPR)
#else
#define CS2_QHY(X)
#endif
#ifdef ALPACACORE_ENABLE_TOUPTEK
#define CS2_TOUPTEK(X) X(touptek_camera, NPR)
#else
#define CS2_TOUPTEK(X)
#endif
#ifdef ALPACACORE_ENABLE_GPHOTO
#define CS2_GPHOTO(X) X(gphoto_camera, NPR)
#else
#define CS2_GPHOTO(X)
#endif
#ifdef ALPACACORE_ENABLE_WANDERERASTRO
#define CS2_WANDERERASTRO(X) X(wandererastro_covercalibrator, PRB) X(wandererastro_filterwheel, PRB) X(wandererastro_switch, PRB)
#else
#define CS2_WANDERERASTRO(X)
#endif
#define CONTRACT_SWEEP_TIER2_HOSTS(X) \
    CS2_ZWO(X) \
    CS2_CELESTRON(X) \
    CS2_SYNSCAN(X) \
    CS2_ONSTEP(X) \
    CS2_SKYWATCHER(X) \
    CS2_IOPTRON(X) \
    CS2_GEMINI(X) \
    CS2_QHY(X) \
    CS2_TOUPTEK(X) \
    CS2_GPHOTO(X) \
    CS2_WANDERERASTRO(X)
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
#define CS2_CASE(id, casename, body) \
    TEST_CASE("Contract sweep tier 2 - " #id " - " casename, "[contract][tier2][" #id "]") { body(tier2_host_##id()); }
#define CS2_CONNECTING(id, kind) CS2_CASE(id, "Connecting semantics", case_t2_connecting)
#define CS2_STATE(id, kind) CS2_CASE(id, "connected DeviceState has TimeStamp", case_t2_device_state_connected)
// Target flags: telescopes only. InvalidValue: every kind except NPR.
#define CS2_TARGETS(id, kind) CS2_TARGETS_##kind(id)
#define CS2_TARGETS_TEL(id) CS2_CASE(id, "target flags until set", case_t2_target_flags)
#define CS2_TARGETS_PRB(id)
#define CS2_TARGETS_NPR(id)
#define CS2_INVALID(id, kind) CS2_INVALID_##kind(id)
#define CS2_INVALID_TEL(id) CS2_CASE(id, "InvalidValue wins while connected", case_t2_invalid_value_connected)
#define CS2_INVALID_PRB(id) CS2_INVALID_TEL(id)
#define CS2_INVALID_NPR(id)

CONTRACT_SWEEP_TIER2_HOSTS(CS2_CONNECTING)
CONTRACT_SWEEP_TIER2_HOSTS(CS2_TARGETS)
CONTRACT_SWEEP_TIER2_HOSTS(CS2_INVALID)
CONTRACT_SWEEP_TIER2_HOSTS(CS2_STATE)
#endif

#ifndef _WIN32
// Non-vacuity guard for tier 2. The roster stays a literal three-field array (docs-drift check 13 parses
// it), so the connect recipes live in CONTRACT_SWEEP_TIER2_HOSTS; this pins the two to each other in both
// directions. A roster row without a host would be a fake no case ever connects to, and a host without a
// row would be a fake that check 13 never sees.
TEST_CASE("Contract sweep tier 2 - hosts match kFakeConnectableRoster", "[contract][tier2][contract-sweep-guard]") {
    using alpacacore::test::contract::kFakeConnectableRoster;
    std::vector<Tier2Host> hosts;
    std::vector<std::string> kinds;
#define CS2_PUSH(id, kind)              \
    hosts.push_back(tier2_host_##id()); \
    kinds.push_back(#kind);
    CONTRACT_SWEEP_TIER2_HOSTS(CS2_PUSH)
#undef CS2_PUSH

    // Vendors this build compiles in that have a roster row.
    std::set<std::string> enabled;
#define CS2_ENABLED(name) enabled.insert(name);
#ifdef ALPACACORE_ENABLE_ZWO
    CS2_ENABLED("zwo")
#endif
#ifdef ALPACACORE_ENABLE_CELESTRON
    CS2_ENABLED("celestron")
#endif
#ifdef ALPACACORE_ENABLE_SYNSCAN
    CS2_ENABLED("synscan")
#endif
#ifdef ALPACACORE_ENABLE_ONSTEP
    CS2_ENABLED("onstep")
#endif
#ifdef ALPACACORE_ENABLE_SKYWATCHER
    CS2_ENABLED("skywatcher")
#endif
#ifdef ALPACACORE_ENABLE_IOPTRON
    CS2_ENABLED("ioptron")
#endif
#ifdef ALPACACORE_ENABLE_GEMINI
    CS2_ENABLED("gemini")
#endif
#ifdef ALPACACORE_ENABLE_QHY
    CS2_ENABLED("qhy")
#endif
#ifdef ALPACACORE_ENABLE_TOUPTEK
    CS2_ENABLED("touptek")
#endif
#ifdef ALPACACORE_ENABLE_GPHOTO
    CS2_ENABLED("gphoto")
#endif
#ifdef ALPACACORE_ENABLE_WANDERERASTRO
    CS2_ENABLED("wandererastro")
#endif
#undef CS2_ENABLED
    // A vendors-off build compiles no fake and so has no host; a build with any of them must have hosts.
    if (!enabled.empty()) REQUIRE_FALSE(hosts.empty());

    using Row = std::tuple<std::string, std::string, std::string>;
    std::multiset<Row> from_roster;
    for (const auto& row : kFakeConnectableRoster) {
        if (enabled.count(row.vendor) != 0) from_roster.emplace(row.vendor, row.device_type, row.fake_header);
    }
    std::multiset<Row> from_hosts;
    std::set<std::string> registry_ids;
    for (const auto& e : alpacacore::test::contract::contract_entries()) registry_ids.insert(e.id);
    std::set<std::string> host_ids;
    for (std::size_t i = 0; i < hosts.size(); ++i) {
        const auto& h = hosts[i];
        from_hosts.emplace(h.vendor, h.device_type, h.fake_header);
        INFO("host " << h.id);
        CHECK(host_ids.insert(h.id).second);
        CHECK(std::string(h.id).rfind(std::string(h.vendor) + "_" + h.device_type, 0) == 0);
        CHECK(registry_ids.count(h.hosts_registry_id) == 1);
        CHECK(static_cast<bool>(h.connectable));
        // The registration kind must match the host, so no case is registered where it asserts nothing.
        const std::string& kind = kinds[i];
        CHECK((kind == "TEL") == (h.type == DeviceType::Telescope));
        const bool states_no_probe =
            std::string(alpacacore::test::contract::invalid_probe_reason_for(h.type)).size() > 0;
        CHECK(states_no_probe == (kind == "NPR"));
        if (h.connectable) {
            Hosted probe_host = h.connectable(false);
            REQUIRE(probe_host.driver != nullptr);
            CHECK(invalid_value_probes(*probe_host.driver, h.type).empty() == (kind == "NPR"));
        }
        CHECK((h.can_hold_connect ||
               (h.connecting_unobservable != nullptr && std::string(h.connecting_unobservable).size() > 0)));
        CHECK((static_cast<bool>(h.failing) || std::string(h.failing_unavailable).size() > 0));
    }
    for (const auto& r : from_roster) {
        INFO("roster row {" << std::get<0>(r) << ", " << std::get<1>(r) << ", " << std::get<2>(r)
                            << "} has no tier-2 host in CONTRACT_SWEEP_TIER2_HOSTS");
        CHECK(from_hosts.count(r) == from_roster.count(r));
    }
    for (const auto& r : from_hosts) {
        INFO("tier-2 host {" << std::get<0>(r) << ", " << std::get<1>(r) << ", " << std::get<2>(r)
                             << "} has no kFakeConnectableRoster row");
        CHECK(from_roster.count(r) == from_hosts.count(r));
    }
}
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
