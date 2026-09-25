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
#include <functional>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "contract_sweep.h"

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
            // get_at_park is deliberately not probed: bisque, celestron, synscan and skywatcher
            // answer it from driver-side parked state while disconnected, and no source in the
            // repo says it must throw (recorded as an open question in the #571 PR body).
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
            break;
        }
        case DeviceType::Camera: {
            auto& c = dynamic_cast<alpacacore::CameraDriver&>(d);
            p.push_back({"get_can_abort_exposure", [&] { (void)c.get_can_abort_exposure(); }});
            p.push_back({"get_can_stop_exposure", [&] { (void)c.get_can_stop_exposure(); }});
            p.push_back({"get_can_pulse_guide", [&] { (void)c.get_can_pulse_guide(); }});
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
    CHECK((a == err::ActionNotImplemented || a == err::NotImplemented));
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
        CHECK_FALSE(std::string(e.source).empty());
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
}
