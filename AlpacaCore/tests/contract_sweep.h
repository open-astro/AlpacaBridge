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

// Cross-driver contract sweep registry (issue #571).
//
// One entry per (vendor, device type) pair that Router::register_device_from_config()
// can construct. Each entry is a hardware-free factory (the same call the vendor's
// own "Defaults" case makes) plus the per-driver expectations and where each one
// comes from. test_contract_sweep.cpp expands CONTRACT_SWEEP_ENTRIES() into one
// TEST_CASE per (entry, case), so ctest lists every (driver, case) pair by name.
//
// Rules read by scripts/check_contract_sweep.py (do not restructure without
// updating it):
//   * every entry id is `<vendor>_<devicetype>` and appears as `X(<id>)` inside
//     exactly one CS_<GUARD>(X) list macro;
//   * each list macro sits under `#if` conditions naming its
//     ALPACACORE_ENABLE_<VENDOR> guard, with an empty `#else` twin, so a
//     vendors-off build sweeps what it has;
//   * every constructed router pair is here, or in the gate's ALLOWLIST with a
//     reason;
//   * AlpacaCore/tests/CMakeLists.txt defines the guards for alpacacore_tests
//     (they are NOT inherited from the vendor targets, so without that the
//     registry would compile to zero entries and the sweep would pass vacuously).
//
// The kFakeConnectableRoster at the bottom lists the drivers that can connect to
// an in-process fake; scripts/check_docs_drift.py pins it to the fake_*.h files on
// disk in both directions.

#pragma once

#include <alpacacore/alpaca_defs.h>
#include <alpacacore/alpacadriver.h>

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifdef ALPACACORE_ENABLE_ZWO
#include <alpacacore/vendor/zwo/zwo_asiair_plus_switch_driver.h>
#include <alpacacore/vendor/zwo/zwo_asiair_switch_driver.h>
#include <alpacacore/vendor/zwo/zwo_camera_driver.h>
#include <alpacacore/vendor/zwo/zwo_filterwheel_driver.h>
#include <alpacacore/vendor/zwo/zwo_focuser_driver.h>
#include <alpacacore/vendor/zwo/zwo_rotator_driver.h>
#include <alpacacore/vendor/zwo/zwo_switch_driver.h>
#include <alpacacore/vendor/zwo/zwo_telescope_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_QHY
#include <alpacacore/vendor/qhy/qhy_camera_driver.h>
#include <alpacacore/vendor/qhy/qhy_cfw3_filterwheel_driver.h>
#include <alpacacore/vendor/qhy/qhy_filterwheel_driver.h>
#include <alpacacore/vendor/qhy/qhy_focuser_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_IOPTRON
#include <alpacacore/vendor/ioptron/ioptron_filterwheel_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_ieaf_focuser_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_switch_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_SYNSCAN
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_SKYWATCHER
#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_ONSTEP
#include <alpacacore/vendor/onstep/onstep_telescope_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_WEEWX
#include <alpacacore/vendor/weewx/weewx_observingconditions_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_GEMINI
#include <alpacacore/vendor/gemini/gemini_flatpanel_driver.h>
#include <alpacacore/vendor/gemini/gemini_focuser_driver.h>
#include <alpacacore/vendor/gemini/gemini_pdh_switch_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_SVBONY
#include <alpacacore/vendor/svbony/svbony_camera_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_CELESTRON
#include <alpacacore/vendor/celestron/celestron_telescope_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_BISQUE
#include <alpacacore/vendor/bisque/bisque_telescope_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_TOUPTEK
#include <alpacacore/vendor/touptek/touptek_camera_driver.h>
#include <alpacacore/vendor/touptek/touptek_filterwheel_driver.h>
#include <alpacacore/vendor/touptek/touptek_focuser_driver.h>
#include <alpacacore/vendor/touptek/touptek_switch_driver.h>
#include <alpacacore/vendor/touptek/touptek_thermal_switch_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_PLAYERONE
#include <alpacacore/vendor/playerone/playerone_camera_driver.h>
#include <alpacacore/vendor/playerone/playerone_filterwheel_driver.h>
#include <alpacacore/vendor/playerone/playerone_switch_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_WANDERERASTRO
#include <alpacacore/vendor/wandererastro/wandererastro_box_switch_driver.h>
#include <alpacacore/vendor/wandererastro/wandererastro_covercalibrator_driver.h>
#include <alpacacore/vendor/wandererastro/wandererastro_filterwheel_driver.h>
#include <alpacacore/vendor/wandererastro/wandererastro_rotator_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_ASTROASIS
#include <alpacacore/vendor/astroasis/astroasis_focuser_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_GPHOTO
#include <alpacacore/vendor/gphoto/gphoto_camera_driver.h>
#endif

namespace alpacacore::test::contract {

using DriverFactory = std::function<std::unique_ptr<AlpacaDriver>(int device_number)>;

// Can* getters. The Can getters case reads every no-argument Can* getter on the device interface while
// disconnected (they must not throw): telescope (all but CanMoveAxis, which is checked per axis and against
// can_move_axis), camera, rotator. Excluded, with the reason:
//  - focuser, filter wheel, cover calibrator, observing conditions: the interface (AlpacaCore/include/
//    alpacacore/*_driver.h) declares no Can* getter. The focuser's Absolute is read as one.
//  - switch: CanWrite(id), CanAsync(id) and MaxSwitch while disconnected may answer from static configuration
//    or throw NotConnected; any other code is a defect (decided in #655). Each switch entry states which of the
//    two its driver does, with its source, and names a writable id that the set_switch probe uses. The
//    sweep pins exactly the stated mode, so a driver that changes flips the case and the entry is updated.
// How a per-id capability read behaves while disconnected: answered from static configuration, or NotConnected.
enum class DisconnectedRead { Static, NotConnected };

struct ContractEntry {
    const char* id;           // "<vendor>_<devicetype>", the ctest case name stem
    const char* vendor;       // router vendor string
    const char* device_type;  // router deviceType string
    DeviceType type;
    DriverFactory make;  // hardware-free construction, no connect
    // ASCOM Platform 7 InterfaceVersion for `type`.
    int interface_version;
    // Telescope only: expected get_can_move_axis(0), (1), (2) while disconnected.
    std::array<bool, 3> can_move_axis;
    // Custom actions the driver advertises through SupportedActions (empty for most drivers).
    std::vector<std::string> actions;
    // True when Command{Blind,Bool,String} forward to the device, so a disconnected call
    // throws NotConnected; false when they are unsupported and throw NotImplemented.
    bool command_passthrough;
    // Why there is no static out-of-range probe for this device type, or empty
    // when the type has one (telescope axis, switch id, filter position, ...).
    const char* no_invalid_probe_reason;
    // Where the expectations above come from: "protocol document", "hardware run"
    // or "assumption", with the reference. The sweep requires one of those words.
    const char* source;
    // Camera only. Non-null: this driver answers get_image_ready / get_ccd_temperature while disconnected
    // instead of throwing NotConnected (AGENTS.md: every operational property does). The string names the
    // issue and the source; the sweep pins today's behaviour until it is fixed.
    const char* image_ready_known_defect = nullptr;
    const char* ccd_temperature_known_defect = nullptr;
    // Switch only (with_switch_caps): what MaxSwitch and CanWrite/CanAsync do while disconnected, where that
    // is read from, and a writable id for the set_switch probe. A writable id is one whose writability the
    // driver checks BEFORE the connection (iOptron, ToupTek StellaVita: a read-only port would answer
    // NotImplemented) or that the connected device reports writable (the rest check the connection first).
    DisconnectedRead switch_max_disconnected = DisconnectedRead::Static;
    DisconnectedRead switch_caps_disconnected = DisconnectedRead::Static;
    int switch_writable_id = -1;
    const char* switch_caps_source = "";
};

// Platform 7 interface versions per device type.
inline int platform7_interface_version(DeviceType t) {
    switch (t) {
        case DeviceType::Telescope:
        case DeviceType::Camera:
        case DeviceType::Focuser:
        case DeviceType::Rotator:
            return 4;
        case DeviceType::FilterWheel:
        case DeviceType::Switch:
            return 3;
        case DeviceType::CoverCalibrator:
        case DeviceType::ObservingConditions:
            return 2;
        default:
            return -1;
    }
}

inline const char* invalid_probe_reason_for(DeviceType t) {
    switch (t) {
        case DeviceType::Camera:
        case DeviceType::Focuser:
            return "no argument with a static out-of-range value that is validated before the connection check "
                   "(assumption)";
        case DeviceType::ObservingConditions:
            return "weewx answers every sensor name it is not currently serving with PropertyNotImplemented, "
                   "including unknown ones (assumption, not checked against ConformU)";
        default:
            return "";
    }
}

// Camera getters that answer while disconnected, one issue for the set (open-astro#658). get_image_ready
// returns false on an early `!connected_` check in playerone (also behind the router's ioptron/camera arm),
// svbony, gphoto and touptek; qhy get_ccd_temperature returns 0.0 with no ensure_connected(). zwo checks both.
inline const char* image_ready_known_defect_for(const std::string& id) {
    if (id == "playerone_camera" || id == "ioptron_camera")
        return "known defect, open-astro#658: PlayerOneCameraDriver::get_image_ready returns false when "
               "!connected_";
    if (id == "svbony_camera")
        return "known defect, open-astro#658: SVBONYCameraDriver::get_image_ready returns false when "
               "!connected_";
    if (id == "gphoto_camera")
        return "known defect, open-astro#658: GPhotoCameraDriver::get_image_ready returns false when "
               "!connected_";
    if (id == "touptek_camera")
        return "known defect, open-astro#658: ToupTekCameraDriver::get_image_ready returns false when "
               "!connected_";
    return nullptr;
}

inline const char* ccd_temperature_known_defect_for(const std::string& id) {
    if (id == "qhy_camera")
        return "known defect, open-astro#658: QHYCameraDriver::get_ccd_temperature returns 0.0 with no "
               "ensure_connected()";
    return nullptr;
}

inline ContractEntry make_entry(const char* id, const char* vendor, const char* device_type, DeviceType type,
                                DriverFactory make, const char* source) {
    return ContractEntry{id,
                         vendor,
                         device_type,
                         type,
                         std::move(make),
                         platform7_interface_version(type),
                         {true, true, false},
                         {},
                         false,
                         invalid_probe_reason_for(type),
                         source,
                         image_ready_known_defect_for(id),
                         ccd_temperature_known_defect_for(id)};
}

inline ContractEntry with_command_passthrough(ContractEntry e) {
    // Command* forward to the mount protocol when connected (driver source; assumption for the
    // disconnected NotConnected code, checked by the sweep itself). Source string is kept.
    e.command_passthrough = true;
    return e;
}

inline ContractEntry with_switch_caps(ContractEntry e, DisconnectedRead max_switch, DisconnectedRead can_write_async,
                                      int writable_id, const char* source) {
    e.switch_max_disconnected = max_switch;
    e.switch_caps_disconnected = can_write_async;
    e.switch_writable_id = writable_id;
    e.switch_caps_source = source;
    return e;
}

// Source strings, kept short so every entry states one.
inline constexpr const char* kSrcAgents =
    "protocol document: AGENTS.md ASCOM contract precedence + ASCOM Platform 7 interface docs; "
    "assumption: the factory arguments are placeholders copied from the vendor Defaults case, not a hardware run";
inline constexpr const char* kSrcAgentsAxis =
    "protocol document: AGENTS.md ASCOM contract precedence + ASCOM ITelescopeV4 CanMoveAxis (axes 0/1 movable, 2 "
    "tertiary not); "
    "the per-vendor disconnected unit tests this sweep replaced (fake/disconnected only, not a hardware run); "
    "Command* passthrough flag and Bisque's absence of it are assumptions read from driver source";

#ifdef ALPACACORE_ENABLE_ZWO
inline ContractEntry contract_entry_zwo_camera() {
    return make_entry(
        "zwo_camera", "zwo", "camera", DeviceType::Camera,
        [](int n) -> std::unique_ptr<AlpacaDriver> { return vendor::zwo::create_zwo_camera_by_index(n, 0); },
        kSrcAgents);
}
inline ContractEntry contract_entry_zwo_telescope() {
    return with_command_passthrough(make_entry(
        "zwo_telescope", "zwo", "telescope", DeviceType::Telescope,
        [](int n) -> std::unique_ptr<AlpacaDriver> {
            vendor::zwo::ConnectionInfo conn;
            conn.type = vendor::zwo::ConnectionType::Serial;
            conn.port_path = "/dev/null";
            return vendor::zwo::create_zwo_telescope(n, conn);
        },
        kSrcAgentsAxis));
}
inline ContractEntry contract_entry_zwo_filterwheel() {
    return make_entry(
        "zwo_filterwheel", "zwo", "filterwheel", DeviceType::FilterWheel,
        [](int n) -> std::unique_ptr<AlpacaDriver> { return vendor::zwo::create_zwo_efw_filterwheel_by_index(n, 0); },
        kSrcAgents);
}
inline ContractEntry contract_entry_zwo_focuser() {
    return make_entry(
        "zwo_focuser", "zwo", "focuser", DeviceType::Focuser,
        [](int n) -> std::unique_ptr<AlpacaDriver> { return vendor::zwo::create_zwo_eaf_focuser_by_index(n, 0); },
        kSrcAgents);
}
inline ContractEntry contract_entry_zwo_rotator() {
    return make_entry(
        "zwo_rotator", "zwo", "rotator", DeviceType::Rotator,
        [](int n) -> std::unique_ptr<AlpacaDriver> { return vendor::zwo::create_zwo_caa_rotator_by_index(n, 0); },
        kSrcAgents);
}
inline ContractEntry contract_entry_zwo_switch() {
    return with_switch_caps(
        make_entry(
            "zwo_switch", "zwo", "switch", DeviceType::Switch,
            [](int n) -> std::unique_ptr<AlpacaDriver> {
                return vendor::zwo::create_zwo_dew_heater_switch_by_index(n, 0);
            },
            "protocol document: AGENTS.md + ASCOM ISwitchV3; assumption: the ASIAIR switch drivers behind the same "
            "router arm have their own entries below"),
        DisconnectedRead::Static, DisconnectedRead::NotConnected, 0,
        "ZWODewHeaterSwitchDriver: MaxSwitch returns 1; CanWrite and CanAsync validate the id then "
        "ensure_connected()");
}
// Second and third backends behind the same (zwo, switch) router pair: the on-board GPIO switch of the
// ASIAIR Pro and Plus (Pi CM4), and the ASIAIR Plus (RK3568) switch.
inline ContractEntry contract_entry_zwo_switch_asiair() {
    return with_switch_caps(make_entry(
                                "zwo_switch_asiair", "zwo", "switch", DeviceType::Switch,
                                [](int n) -> std::unique_ptr<AlpacaDriver> {
                                    return vendor::zwo::create_zwo_asiair_switch(
                                        n, vendor::zwo::default_asiair_pro_config());
                                },
                                kSrcAgents),
                            DisconnectedRead::Static, DisconnectedRead::Static, 0,
                            "ZWOAsiairSwitchDriver: MaxSwitch is the configured port count; CanWrite is "
                            "true and CanAsync false, both after validate_id only");
}
inline ContractEntry contract_entry_zwo_switch_asiair_plus() {
    return with_switch_caps(make_entry(
                                "zwo_switch_asiair_plus", "zwo", "switch", DeviceType::Switch,
                                [](int n) -> std::unique_ptr<AlpacaDriver> {
                                    return vendor::zwo::create_zwo_asiair_plus_switch(
                                        n, vendor::zwo::default_asiair_plus_rk3568_config());
                                },
                                kSrcAgents),
                            DisconnectedRead::Static, DisconnectedRead::Static, 0,
                            "ZWOAsiairPlusSwitchDriver: MaxSwitch is the configured port count; "
                            "CanWrite is true and CanAsync false, both after validate_id only");
}
#endif

#ifdef ALPACACORE_ENABLE_QHY
inline ContractEntry contract_entry_qhy_camera() {
    return make_entry(
        "qhy_camera", "qhy", "camera", DeviceType::Camera,
        [](int n) -> std::unique_ptr<AlpacaDriver> { return vendor::qhy::create_qhy_camera_by_index(n, 0); },
        kSrcAgents);
}
inline ContractEntry contract_entry_qhy_filterwheel() {
    return make_entry(
        "qhy_filterwheel", "qhy", "filterwheel", DeviceType::FilterWheel,
        [](int n) -> std::unique_ptr<AlpacaDriver> { return vendor::qhy::create_qhy_filterwheel_by_index(n, 0); },
        kSrcAgents);
}
// Second backend behind the same (qhy, filterwheel) router pair: the standalone CFW3 on a serial port.
inline ContractEntry contract_entry_qhy_filterwheel_cfw3() {
    return make_entry(
        "qhy_filterwheel_cfw3", "qhy", "filterwheel", DeviceType::FilterWheel,
        [](int n) -> std::unique_ptr<AlpacaDriver> {
            return vendor::qhy::create_qhy_cfw3_filterwheel(n, "/dev/qhy-cfw3-absent");
        },
        kSrcAgents);
}
inline ContractEntry contract_entry_qhy_focuser() {
    return make_entry(
        "qhy_focuser", "qhy", "focuser", DeviceType::Focuser,
        [](int n) -> std::unique_ptr<AlpacaDriver> {
            return vendor::qhy::create_qhy_focuser(n, "/dev/qhy-qfocuser-absent");
        },
        kSrcAgents);
}
#endif

#ifdef ALPACACORE_ENABLE_IOPTRON
inline ContractEntry contract_entry_ioptron_telescope() {
    return with_command_passthrough(make_entry(
        "ioptron_telescope", "ioptron", "telescope", DeviceType::Telescope,
        [](int n) -> std::unique_ptr<AlpacaDriver> {
            vendor::ioptron::ConnectionInfo conn;
            conn.type = vendor::ioptron::ConnectionType::Serial;
            conn.port_path = "/dev/null";
            return vendor::ioptron::create_ioptron_telescope(n, conn);
        },
        kSrcAgentsAxis));
}
inline ContractEntry contract_entry_ioptron_focuser() {
    return make_entry(
        "ioptron_focuser", "ioptron", "focuser", DeviceType::Focuser,
        [](int n) -> std::unique_ptr<AlpacaDriver> { return vendor::ioptron::create_ieaf_focuser(n, "/dev/ttyUSB0"); },
        kSrcAgents);
}
inline ContractEntry contract_entry_ioptron_filterwheel() {
    return make_entry(
        "ioptron_filterwheel", "ioptron", "filterwheel", DeviceType::FilterWheel,
        [](int n) -> std::unique_ptr<AlpacaDriver> { return vendor::ioptron::create_iefw_filterwheel_by_index(n, 0); },
        kSrcAgents);
}
#endif

#if defined(ALPACACORE_ENABLE_IOPTRON) && defined(ALPACACORE_IOPTRON_POWERBOX)
inline ContractEntry contract_entry_ioptron_switch() {
    return with_switch_caps(make_entry(
                                "ioptron_switch", "ioptron", "switch", DeviceType::Switch,
                                [](int n) -> std::unique_ptr<AlpacaDriver> {
                                    return vendor::ioptron::create_ioptron_switch(
                                        n, vendor::ioptron::default_imate_powerbox_config());
                                },
                                kSrcAgents),
                            DisconnectedRead::Static, DisconnectedRead::Static, 1,
                            "IoptronSwitchDriver and default_imate_powerbox_config(): static port table; id 0 "
                            "(DC3 always on) is read-only, id 1 (DC1) is writable");
}
#endif

#ifdef ALPACACORE_ENABLE_PLAYERONE
// The router builds ioptron/camera from the Player One camera driver (iCAM
// cameras are rebadged Player One), so it is guarded by the Player One flag.
inline ContractEntry with_playerone_actions(ContractEntry e) {
    // Action list read from playerone_camera_driver.cpp get_supported_actions() (assumption: not
    // checked against a hardware run). Source string is kept.
    e.actions = {"GetHeaterPower", "SetHeaterPower", "GetFanPower", "SetFanPower"};
    return e;
}
inline ContractEntry contract_entry_ioptron_camera() {
    return with_playerone_actions(make_entry(
        "ioptron_camera", "ioptron", "camera", DeviceType::Camera,
        [](int n) -> std::unique_ptr<AlpacaDriver> { return vendor::playerone::create_playerone_camera(n, 0); },
        "assumption: the router's ioptron/camera arm reuses the Player One camera driver (router.cpp); "
        "protocol document: AGENTS.md contract"));
}
inline ContractEntry contract_entry_playerone_camera() {
    return with_playerone_actions(make_entry(
        "playerone_camera", "playerone", "camera", DeviceType::Camera,
        [](int n) -> std::unique_ptr<AlpacaDriver> { return vendor::playerone::create_playerone_camera(n, 0); },
        kSrcAgents));
}
inline ContractEntry contract_entry_playerone_filterwheel() {
    return make_entry(
        "playerone_filterwheel", "playerone", "filterwheel", DeviceType::FilterWheel,
        [](int n) -> std::unique_ptr<AlpacaDriver> { return vendor::playerone::create_playerone_filterwheel(n, 0); },
        kSrcAgents);
}
inline ContractEntry contract_entry_playerone_switch() {
    return with_switch_caps(
        make_entry(
            "playerone_switch", "playerone", "switch", DeviceType::Switch,
            [](int n) -> std::unique_ptr<AlpacaDriver> { return vendor::playerone::create_playerone_switch(n, 0); },
            kSrcAgents),
        DisconnectedRead::Static, DisconnectedRead::NotConnected, 0,
        "PlayerOneSwitchDriver: MaxSwitch returns kMaxThermalElements while disconnected; CanWrite and "
        "CanAsync ensure_connected()");
}
#endif

#ifdef ALPACACORE_ENABLE_SYNSCAN
inline ContractEntry contract_entry_synscan_telescope() {
    return with_command_passthrough(make_entry(
        "synscan_telescope", "synscan", "telescope", DeviceType::Telescope,
        [](int n) -> std::unique_ptr<AlpacaDriver> {
            vendor::synscan::ConnectionInfo conn;
            conn.type = vendor::synscan::ConnectionType::Serial;
            conn.port_path = "/dev/null";
            return vendor::synscan::create_synscan_telescope(n, conn, vendor::synscan::SynScanVersion::Auto);
        },
        kSrcAgentsAxis));
}
#endif

#ifdef ALPACACORE_ENABLE_SKYWATCHER
inline ContractEntry contract_entry_skywatcher_telescope() {
    return with_command_passthrough(make_entry(
        "skywatcher_telescope", "skywatcher", "telescope", DeviceType::Telescope,
        [](int n) -> std::unique_ptr<AlpacaDriver> {
            vendor::skywatcher::ConnectionInfo conn;
            conn.type = vendor::skywatcher::ConnectionType::Serial;
            conn.port_path = "/dev/null";
            return vendor::skywatcher::create_skywatcher_telescope(n, conn);
        },
        kSrcAgentsAxis));
}
#endif

#ifdef ALPACACORE_ENABLE_ONSTEP
inline ContractEntry contract_entry_onstep_telescope() {
    return with_command_passthrough(make_entry(
        "onstep_telescope", "onstep", "telescope", DeviceType::Telescope,
        [](int n) -> std::unique_ptr<AlpacaDriver> {
            vendor::onstep::ConnectionInfo conn;
            conn.type = vendor::onstep::ConnectionType::Network;
            conn.host = "127.0.0.1";
            conn.tcp_port = 9;
            return vendor::onstep::create_onstep_telescope(n, conn);
        },
        kSrcAgentsAxis));
}
#endif

#ifdef ALPACACORE_ENABLE_CELESTRON
inline ContractEntry contract_entry_celestron_telescope() {
    return with_command_passthrough(make_entry(
        "celestron_telescope", "celestron", "telescope", DeviceType::Telescope,
        [](int n) -> std::unique_ptr<AlpacaDriver> {
            vendor::celestron::ConnectionInfo conn;
            conn.type = vendor::celestron::ConnectionType::Serial;
            conn.port_path = "/dev/null";
            return vendor::celestron::create_celestron_telescope(n, conn);
        },
        kSrcAgentsAxis));
}
#endif

#ifdef ALPACACORE_ENABLE_BISQUE
inline ContractEntry contract_entry_bisque_telescope() {
    return make_entry(
        "bisque_telescope", "bisque", "telescope", DeviceType::Telescope,
        [](int n) -> std::unique_ptr<AlpacaDriver> {
            vendor::bisque::ConnectionInfo conn;
            conn.host = "localhost";
            conn.tcp_port = 3040;
            return vendor::bisque::create_bisque_telescope(n, conn);
        },
        kSrcAgentsAxis);
}
#endif

#ifdef ALPACACORE_ENABLE_WEEWX
inline ContractEntry contract_entry_weewx_observingconditions() {
    return make_entry(
        "weewx_observingconditions", "weewx", "observingconditions", DeviceType::ObservingConditions,
        [](int n) -> std::unique_ptr<AlpacaDriver> {
            vendor::weewx::WeeWxHttpConfig config;
            config.url = "http://localhost:9999/dummy";
            return vendor::weewx::create_weewx_observingconditions(n, config);
        },
        kSrcAgents);
}
#endif

#ifdef ALPACACORE_ENABLE_GEMINI
inline ContractEntry contract_entry_gemini_covercalibrator() {
    return make_entry(
        "gemini_covercalibrator", "gemini", "covercalibrator", DeviceType::CoverCalibrator,
        [](int n) -> std::unique_ptr<AlpacaDriver> {
            return vendor::gemini::create_gemini_flatpanel(n, "/dev/ttyUSB0");
        },
        kSrcAgents);
}
// Second and third backends behind the same (gemini, covercalibrator) router pair: the Flat Panel v2 and
// the Motorized Flat Panel V3 (Pro). The entry above is the Cover Lite (create_gemini_flatpanel).
inline ContractEntry contract_entry_gemini_covercalibrator_v2() {
    return make_entry(
        "gemini_covercalibrator_v2", "gemini", "covercalibrator", DeviceType::CoverCalibrator,
        [](int n) -> std::unique_ptr<AlpacaDriver> {
            return vendor::gemini::create_gemini_flatpanel_v2(n, "/dev/ttyUSB0");
        },
        kSrcAgents);
}
inline ContractEntry contract_entry_gemini_covercalibrator_pro() {
    return make_entry(
        "gemini_covercalibrator_pro", "gemini", "covercalibrator", DeviceType::CoverCalibrator,
        [](int n) -> std::unique_ptr<AlpacaDriver> {
            return vendor::gemini::create_gemini_flatpanel_pro(n, "/dev/ttyUSB0");
        },
        kSrcAgents);
}
inline ContractEntry contract_entry_gemini_focuser() {
    return make_entry(
        "gemini_focuser", "gemini", "focuser", DeviceType::Focuser,
        [](int n) -> std::unique_ptr<AlpacaDriver> { return vendor::gemini::create_gemini_focuser(n, "/dev/ttyUSB0"); },
        kSrcAgents);
}
inline ContractEntry contract_entry_gemini_switch() {
    return with_switch_caps(make_entry(
                                "gemini_switch", "gemini", "switch", DeviceType::Switch,
                                [](int n) -> std::unique_ptr<AlpacaDriver> {
                                    return vendor::gemini::create_gemini_pdh_switch(n, "/dev/null");
                                },
                                kSrcAgents),
                            DisconnectedRead::Static, DisconnectedRead::NotConnected, 0,
                            "GeminiPdhSwitchDriver and kSwitches: MaxSwitch is kPdhSwitchCount; "
                            "CanWrite and CanAsync ensure_connected(); id 0 (USB A) is writable");
}
#endif

#ifdef ALPACACORE_ENABLE_SVBONY
inline ContractEntry contract_entry_svbony_camera() {
    return make_entry(
        "svbony_camera", "svbony", "camera", DeviceType::Camera,
        [](int n) -> std::unique_ptr<AlpacaDriver> { return vendor::svbony::create_svbony_camera(n, 0); }, kSrcAgents);
}
#endif

#ifdef ALPACACORE_ENABLE_GPHOTO
inline ContractEntry contract_entry_gphoto_camera() {
    return make_entry(
        "gphoto_camera", "gphoto", "camera", DeviceType::Camera,
        [](int n) -> std::unique_ptr<AlpacaDriver> { return vendor::gphoto::create_gphoto_camera(n, 0); }, kSrcAgents);
}
#endif

#ifdef ALPACACORE_ENABLE_TOUPTEK
inline ContractEntry contract_entry_touptek_camera() {
    return make_entry(
        "touptek_camera", "touptek", "camera", DeviceType::Camera,
        [](int n) -> std::unique_ptr<AlpacaDriver> { return vendor::touptek::create_touptek_camera(n, 0); },
        kSrcAgents);
}
inline ContractEntry contract_entry_touptek_filterwheel() {
    return make_entry(
        "touptek_filterwheel", "touptek", "filterwheel", DeviceType::FilterWheel,
        [](int n) -> std::unique_ptr<AlpacaDriver> {
            return vendor::touptek::create_touptek_filterwheel_by_index(n, 0);
        },
        kSrcAgents);
}
inline ContractEntry contract_entry_touptek_focuser() {
    return make_entry(
        "touptek_focuser", "touptek", "focuser", DeviceType::Focuser,
        [](int n) -> std::unique_ptr<AlpacaDriver> { return vendor::touptek::create_touptek_focuser_by_index(n, 0); },
        kSrcAgents);
}
#endif

#ifdef ALPACACORE_ENABLE_TOUPTEK
// Second backend behind the same (touptek, switch) router pair; needs no libgpiod.
inline ContractEntry contract_entry_touptek_switch_thermal() {
    return with_switch_caps(
        make_entry(
            "touptek_switch_thermal", "touptek", "switch", DeviceType::Switch,
            [](int n) -> std::unique_ptr<AlpacaDriver> { return vendor::touptek::create_touptek_thermal_switch(n, 0); },
            kSrcAgents),
        DisconnectedRead::Static, DisconnectedRead::NotConnected, 0,
        "ToupTekThermalSwitchDriver: MaxSwitch is the probed count or the kMaxThermalElements upper "
        "bound; CanWrite and CanAsync ensure_connected()");
}
#endif

#if defined(ALPACACORE_ENABLE_TOUPTEK) && defined(ALPACACORE_TOUPTEK_STELLAVITA)
inline ContractEntry contract_entry_touptek_switch() {
    return with_switch_caps(make_entry(
                                "touptek_switch", "touptek", "switch", DeviceType::Switch,
                                [](int n) -> std::unique_ptr<AlpacaDriver> {
                                    return vendor::touptek::create_touptek_switch(
                                        n, vendor::touptek::default_stellavita_config());
                                },
                                kSrcAgents),
                            DisconnectedRead::Static, DisconnectedRead::Static, 0,
                            "TouptekSwitchDriver and default_stellavita_config(): static port table, "
                            "all four ports writable");
}
#endif

#ifdef ALPACACORE_ENABLE_WANDERERASTRO
inline ContractEntry contract_entry_wandererastro_covercalibrator() {
    return make_entry(
        "wandererastro_covercalibrator", "wandererastro", "covercalibrator", DeviceType::CoverCalibrator,
        [](int n) -> std::unique_ptr<AlpacaDriver> {
            return vendor::wandererastro::create_wandererastro_covercalibrator(n, "/dev/ttyUSB0");
        },
        kSrcAgents);
}
inline ContractEntry contract_entry_wandererastro_filterwheel() {
    return make_entry(
        "wandererastro_filterwheel", "wandererastro", "filterwheel", DeviceType::FilterWheel,
        [](int n) -> std::unique_ptr<AlpacaDriver> {
            return vendor::wandererastro::create_wandererastro_filterwheel(n, "/dev/null");
        },
        kSrcAgents);
}
inline ContractEntry contract_entry_wandererastro_rotator() {
    return make_entry(
        "wandererastro_rotator", "wandererastro", "rotator", DeviceType::Rotator,
        [](int n) -> std::unique_ptr<AlpacaDriver> {
            return vendor::wandererastro::create_wandererastro_rotator(n, "/dev/null");
        },
        kSrcAgents);
}
inline ContractEntry contract_entry_wandererastro_switch() {
    return with_switch_caps(make_entry(
                                "wandererastro_switch", "wandererastro", "switch", DeviceType::Switch,
                                [](int n) -> std::unique_ptr<AlpacaDriver> {
                                    return vendor::wandererastro::create_wandererastro_box_switch(n, "/dev/null");
                                },
                                kSrcAgents),
                            DisconnectedRead::Static, DisconnectedRead::NotConnected, 2,
                            "WandererBoxSwitchDriver and kSwitches: MaxSwitch is kBoxSwitchCount; CanWrite and "
                            "CanAsync ensure_connected(); ids 0-1 are read-only, id 2 (DC3-4) is writable");
}
#endif

#ifdef ALPACACORE_ENABLE_ASTROASIS
inline ContractEntry contract_entry_astroasis_focuser() {
    return make_entry(
        "astroasis_focuser", "astroasis", "focuser", DeviceType::Focuser,
        [](int n) -> std::unique_ptr<AlpacaDriver> {
            return vendor::astroasis::create_astroasis_focuser(n, "/dev/hidraw0");
        },
        kSrcAgents);
}
#endif

}  // namespace alpacacore::test::contract

// ---------------------------------------------------------------------------
// The registry. Each CS_<GUARD>(X) list is one guard; the gate parses these.
// ---------------------------------------------------------------------------

#ifdef ALPACACORE_ENABLE_ZWO
// The formatter does not reach a fixed point on this continuation list.
// clang-format off
#define CS_ZWO(X) \
    X(zwo_camera) \
    X(zwo_telescope) \
    X(zwo_filterwheel) \
    X(zwo_focuser) \
    X(zwo_rotator) \
    X(zwo_switch) \
    X(zwo_switch_asiair) \
    X(zwo_switch_asiair_plus)
// clang-format on
#else
#define CS_ZWO(X)
#endif
#ifdef ALPACACORE_ENABLE_QHY
#define CS_QHY(X) X(qhy_camera) X(qhy_filterwheel) X(qhy_filterwheel_cfw3) X(qhy_focuser)
#else
#define CS_QHY(X)
#endif
#ifdef ALPACACORE_ENABLE_IOPTRON
#define CS_IOPTRON(X) X(ioptron_telescope) X(ioptron_focuser) X(ioptron_filterwheel)
#else
#define CS_IOPTRON(X)
#endif
#if defined(ALPACACORE_ENABLE_IOPTRON) && defined(ALPACACORE_IOPTRON_POWERBOX)
#define CS_IOPTRON_POWERBOX(X) X(ioptron_switch)
#else
#define CS_IOPTRON_POWERBOX(X)
#endif
#ifdef ALPACACORE_ENABLE_PLAYERONE
#define CS_PLAYERONE(X) X(playerone_camera) X(playerone_filterwheel) X(playerone_switch) X(ioptron_camera)
#else
#define CS_PLAYERONE(X)
#endif
#ifdef ALPACACORE_ENABLE_SYNSCAN
#define CS_SYNSCAN(X) X(synscan_telescope)
#else
#define CS_SYNSCAN(X)
#endif
#ifdef ALPACACORE_ENABLE_SKYWATCHER
#define CS_SKYWATCHER(X) X(skywatcher_telescope)
#else
#define CS_SKYWATCHER(X)
#endif
#ifdef ALPACACORE_ENABLE_ONSTEP
#define CS_ONSTEP(X) X(onstep_telescope)
#else
#define CS_ONSTEP(X)
#endif
#ifdef ALPACACORE_ENABLE_CELESTRON
#define CS_CELESTRON(X) X(celestron_telescope)
#else
#define CS_CELESTRON(X)
#endif
#ifdef ALPACACORE_ENABLE_BISQUE
#define CS_BISQUE(X) X(bisque_telescope)
#else
#define CS_BISQUE(X)
#endif
#ifdef ALPACACORE_ENABLE_WEEWX
#define CS_WEEWX(X) X(weewx_observingconditions)
#else
#define CS_WEEWX(X)
#endif
#ifdef ALPACACORE_ENABLE_GEMINI
// The formatter does not reach a fixed point on this continuation list.
// clang-format off
#define CS_GEMINI(X) \
    X(gemini_covercalibrator) \
    X(gemini_covercalibrator_v2) \
    X(gemini_covercalibrator_pro) \
    X(gemini_focuser) \
    X(gemini_switch)
// clang-format on
#else
#define CS_GEMINI(X)
#endif
#ifdef ALPACACORE_ENABLE_SVBONY
#define CS_SVBONY(X) X(svbony_camera)
#else
#define CS_SVBONY(X)
#endif
#ifdef ALPACACORE_ENABLE_GPHOTO
#define CS_GPHOTO(X) X(gphoto_camera)
#else
#define CS_GPHOTO(X)
#endif
#ifdef ALPACACORE_ENABLE_TOUPTEK
#define CS_TOUPTEK(X) X(touptek_camera) X(touptek_filterwheel) X(touptek_focuser) X(touptek_switch_thermal)
#else
#define CS_TOUPTEK(X)
#endif
#if defined(ALPACACORE_ENABLE_TOUPTEK) && defined(ALPACACORE_TOUPTEK_STELLAVITA)
#define CS_TOUPTEK_STELLAVITA(X) X(touptek_switch)
#else
#define CS_TOUPTEK_STELLAVITA(X)
#endif
#ifdef ALPACACORE_ENABLE_WANDERERASTRO
#define CS_WANDERERASTRO(X) \
    X(wandererastro_covercalibrator) X(wandererastro_filterwheel) X(wandererastro_rotator) X(wandererastro_switch)
#else
#define CS_WANDERERASTRO(X)
#endif
#ifdef ALPACACORE_ENABLE_ASTROASIS
#define CS_ASTROASIS(X) X(astroasis_focuser)
#else
#define CS_ASTROASIS(X)
#endif

// The formatter does not reach a fixed point on this continuation list.
// clang-format off
#define CONTRACT_SWEEP_ENTRIES(X) \
    CS_ZWO(X) \
    CS_QHY(X) \
    CS_IOPTRON(X) \
    CS_IOPTRON_POWERBOX(X) \
    CS_PLAYERONE(X) \
    CS_SYNSCAN(X) \
    CS_SKYWATCHER(X) \
    CS_ONSTEP(X) \
    CS_CELESTRON(X) \
    CS_BISQUE(X) \
    CS_WEEWX(X) \
    CS_GEMINI(X) \
    CS_SVBONY(X) \
    CS_GPHOTO(X) \
    CS_TOUPTEK(X) \
    CS_TOUPTEK_STELLAVITA(X) \
    CS_WANDERERASTRO(X) \
    CS_ASTROASIS(X)
// clang-format on

namespace alpacacore::test::contract {

inline std::vector<ContractEntry> contract_entries() {
    std::vector<ContractEntry> entries;
#define CS_PUSH(id) entries.push_back(contract_entry_##id());
    CONTRACT_SWEEP_ENTRIES(CS_PUSH)
#undef CS_PUSH
    return entries;
}

// ---------------------------------------------------------------------------
// Fake-connectable roster (tier 2): the drivers that can connect to an
// in-process fake today. Pinned to AlpacaCore/tests/fake_*.h by
// scripts/check_docs_drift.py (check 13): a fake on disk that is neither here
// nor a helper fake fails, and a row whose fake no longer exists fails. The
// tier-2 cases over this roster are in test_contract_sweep.cpp: the array stays a
// literal three-field list because check 13 parses it, so each row's connect recipe
// and the registry entry it hosts live in CONTRACT_SWEEP_TIER2_HOSTS there, and the
// case "Contract sweep tier 2 - hosts match kFakeConnectableRoster" pins the two
// together. Every row hosts every applicable case and no case is registered where it would assert
// nothing: target flags on telescopes only, InvalidValue only on types with a static out-of-range probe
// (invalid_probe_reason_for() states why the others have none). A row whose fake cannot hold a connect
// open states why and the source in its connecting_unobservable reason there instead of skipping the
// Connecting check silently. A row whose fake covers
// less than its registry entry says so inline.
// ---------------------------------------------------------------------------

struct FakeRosterRow {
    const char* vendor;
    const char* device_type;
    const char* fake_header;
};

inline constexpr FakeRosterRow kFakeConnectableRoster[] = {
    {"skywatcher", "telescope", "fake_skywatcher_mount.h"},
    {"skywatcher", "telescope", "fake_skywatcher_serial_board.h"},  // hosted as skywatcher_telescope_serial
    {"zwo", "telescope", "fake_mount_server.h"},
    {"celestron", "telescope", "fake_mount_server.h"},
    {"synscan", "telescope", "fake_mount_server.h"},
    {"onstep", "telescope", "fake_mount_server.h"},
    {"ioptron", "telescope", "fake_ioptron_mount.h"},
    {"ioptron", "focuser", "fake_ioptron_ieaf.h"},
    {"gemini", "focuser", "fake_gemini_focuser.h"},
    {"gemini", "covercalibrator", "fake_gemini_flatpanel.h"},  // Pro firmware only: gemini_covercalibrator_pro
    {"gemini", "switch", "fake_gemini_pdh.h"},
    {"qhy", "camera", "fake_qhy_sdk.h"},
    {"qhy", "filterwheel", "fake_qhy_cfw3.h"},  // CFW3 serial backend: qhy_filterwheel_cfw3
    {"qhy", "focuser", "fake_qhy_qfocuser.h"},
    {"touptek", "camera", "fake_touptek_sdk.h"},
    {"gphoto", "camera", "fake_gphoto_sdk.h"},
    {"wandererastro", "covercalibrator", "fake_serial_streamer.h"},
    {"wandererastro", "filterwheel", "fake_serial_streamer.h"},
    {"wandererastro", "switch", "fake_serial_streamer.h"},
};

}  // namespace alpacacore::test::contract
