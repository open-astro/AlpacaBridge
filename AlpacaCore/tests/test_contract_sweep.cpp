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
// down, one host per kFakeConnectableRoster row. Tier 3 (issue #1861, failure after
// start) runs over the tier-2 hosts of the camera, telescope, focuser, rotator and
// filter wheel types, one recipe per host.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/camera_driver.h>
#include <alpacacore/catalog/builtin_catalog.h>
#include <alpacacore/covercalibrator_driver.h>
#include <alpacacore/filterwheel_driver.h>
#include <alpacacore/focuser_driver.h>
#include <alpacacore/observingconditions_driver.h>
#include <alpacacore/rotator_driver.h>
#include <alpacacore/switch_driver.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "camera_percent_cooler_checks.h"
#include "catch2_compat.h"
#include "contract_sweep.h"
#include "fake_task_clock.h"

// Tier 2 hosts: the roster fakes, each under its vendor guard. Fakes over a pty or a loopback socket
// are POSIX only.
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef ALPACACORE_ENABLE_ZWO
#include <alpacacore/vendor/zwo/zwo_telescope_driver.h>

#include "fake_zwo_caa_sdk.h"
#include "fake_zwo_eaf_sdk.h"
#include "fake_zwo_efw_sdk.h"
#include "fake_zwo_sdk.h"
#endif
#ifdef ALPACACORE_ENABLE_CELESTRON
#include <alpacacore/vendor/celestron/celestron_cgx_telescope_driver.h>
#include <alpacacore/vendor/celestron/celestron_focuser_driver.h>
#include <alpacacore/vendor/celestron/celestron_protocol_wrapper.h>
#include <alpacacore/vendor/celestron/celestron_telescope_driver.h>

#include "fake_celestron_cgx_mount.h"
#include "fake_celestron_focuser.h"
#endif
#ifdef ALPACACORE_ENABLE_SYNSCAN
#include <alpacacore/vendor/synscan/synscan_protocol_wrapper.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_ONSTEP
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>
#include <alpacacore/vendor/onstep/onstep_telescope_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_BISQUE
#include <alpacacore/vendor/bisque/bisque_telescope_driver.h>

#include "fake_bisque_mount.h"
#endif
#if defined(ALPACACORE_ENABLE_ZWO) || defined(ALPACACORE_ENABLE_CELESTRON) || defined(ALPACACORE_ENABLE_SYNSCAN) || \
    defined(ALPACACORE_ENABLE_ONSTEP) || defined(ALPACACORE_ENABLE_BISQUE)
#include "fake_mount_server.h"
#endif
#ifdef ALPACACORE_ENABLE_SKYWATCHER
#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>

#include "fake_skywatcher_mount.h"
#include "fake_skywatcher_serial_board.h"
#endif
#ifdef ALPACACORE_ENABLE_IOPTRON
#include <alpacacore/vendor/ioptron/ioptron_ieaf_focuser_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_iguider_camera_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>

#include "fake_iguider_v4l2.h"
#include "fake_ioptron_ieaf.h"
#include "fake_ioptron_mount.h"
#include "locked_iguider_v4l2.h"
#endif
#ifdef ALPACACORE_ENABLE_GEMINI
#include <alpacacore/vendor/gemini/gemini_flatpanel_driver.h>
#include <alpacacore/vendor/gemini/gemini_focuser_driver.h>
#include <alpacacore/vendor/gemini/gemini_pdh_switch_driver.h>

#include "fake_gemini_flatpanel.h"
#include "fake_gemini_focuser.h"
#include "fake_gemini_pdh.h"
#endif
#ifdef ALPACACORE_ENABLE_ASTROASIS
#include <alpacacore/vendor/astroasis/astroasis_focuser_driver.h>

#include "fake_astroasis_hid.h"
#include "locked_astroasis_hid_transport.h"
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
#ifdef ALPACACORE_ENABLE_ALTAIR
#include <alpacacore/vendor/altair/altair_camera_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_TOUPTEK
#include <alpacacore/vendor/touptek/touptek_camera_driver.h>

#include "fake_touptek_sdk.h"
#include "locked_touptek_sdk.h"
#endif
#if defined(ALPACACORE_ENABLE_IOPTRON) && defined(ALPACACORE_IOPTRON_POWERBOX)
#include "fake_ioptron_gpio.h"
#endif
#if defined(ALPACACORE_ENABLE_TOUPTEK) && defined(ALPACACORE_TOUPTEK_STELLAVITA)
#include <alpacacore/vendor/touptek/touptek_switch_driver.h>

#include "fake_touptek_powerbox_sdk.h"
#include "locked_touptek_powerbox_sdk.h"
#endif
#ifdef ALPACACORE_ENABLE_SVBONY
#include <alpacacore/vendor/svbony/svbony_camera_driver.h>

#include "fake_svbony_sdk.h"
#endif
#ifdef ALPACACORE_ENABLE_PLAYERONE
#include <alpacacore/vendor/playerone/playerone_camera_driver.h>
#include <alpacacore/vendor/playerone/playerone_filterwheel_driver.h>

#include "fake_playerone_pw_sdk.h"
#include "fake_playerone_sdk.h"
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
// connection check (driver-lifecycle.instructions.md ASCOM contract precedence).
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
        case DeviceType::ObservingConditions: {
            // A sensor name IObservingConditions does not define is InvalidValue, never NotImplemented.
            auto& o = dynamic_cast<alpacacore::ObservingConditionsDriver&>(d);
            p.push_back({"get_time_since_last_update(NoSuchSensor)",
                         [&] { (void)o.get_time_since_last_update("NoSuchSensor"); }});
            p.push_back(
                {"get_sensor_description(NoSuchSensor)", [&] { (void)o.get_sensor_description("NoSuchSensor"); }});
            p.push_back({"set_average_period(-1)", [&] { o.set_average_period(-1.0); }});
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

// The tier-1 probe list: the type's probes plus the focuser move(-1) of a driver that checks a static range.
std::vector<Probe> invalid_value_probes(AlpacaDriver& d, const ContractEntry& e) {
    auto p = invalid_value_probes(d, e.type);
    if (e.type == DeviceType::Focuser && e.focuser_static_range) {
        auto& f = dynamic_cast<alpacacore::FocuserDriver&>(d);
        p.push_back({"move(-1)", [&] { f.move(-1); }});
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
            p.push_back({"get_right_ascension_rate", [&] { (void)t.get_right_ascension_rate(); }});
            p.push_back({"get_declination_rate", [&] { (void)t.get_declination_rate(); }});
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
            // The id is named per entry: a writable one, so the probe reaches the connection check on every driver.
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
            p.push_back({"get_max_step", [&] { (void)f.get_max_step(); }});
            p.push_back({"get_max_increment", [&] { (void)f.get_max_increment(); }});
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
            p.push_back({"get_max_brightness", [&] { (void)c.get_max_brightness(); }});
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
            p.push_back({"get_last_exposure_duration", [&] { (void)c.get_last_exposure_duration(); }});
            p.push_back({"get_last_exposure_start_time", [&] { (void)c.get_last_exposure_start_time(); }});
            p.push_back({"get_camera_state", [&] { (void)c.get_camera_state(); }});
            p.push_back({"get_percent_completed", [&] { (void)c.get_percent_completed(); }});
            p.push_back({"get_ccd_temperature", [&] { (void)c.get_ccd_temperature(); }});
            p.push_back({"get_cooler_on", [&] { (void)c.get_cooler_on(); }});
            p.push_back({"get_cooler_power", [&] { (void)c.get_cooler_power(); }});
            p.push_back({"get_set_ccd_temperature", [&] { (void)c.get_set_ccd_temperature(); }});
            p.push_back({"set_set_ccd_temperature", [&] { c.set_set_ccd_temperature(0.0); }});
            p.push_back({"set_cooler_on_true", [&] { c.set_cooler_on(true); }});
            p.push_back({"set_cooler_on_false", [&] { c.set_cooler_on(false); }});
            p.push_back({"start_exposure", [&] { c.start_exposure(1.0, true); }});
            break;
        }
        default:
            break;
    }
    return p;
}

// Members a driver may not implement (capability off, vendor protocol without the command). While disconnected
// each must answer NotConnected, never NotImplemented: the connection check comes first, then the capability
// check (AGENTS.md "ASCOM contract precedence"). A probe that returns, or throws InvalidValue, is a static read
// or an argument check and passes. 0x402/0x40B reads while disconnected are a separate defect (not probed here).
std::vector<Probe> not_implemented_probes(AlpacaDriver& d, const ContractEntry& entry) {
    std::vector<Probe> p;
    switch (entry.type) {
        case DeviceType::Telescope: {
            auto& t = dynamic_cast<alpacacore::TelescopeDriver&>(d);
            p.push_back({"set_declination_rate", [&] { t.set_declination_rate(0.0); }});
            p.push_back({"set_right_ascension_rate", [&] { t.set_right_ascension_rate(0.0); }});
            p.push_back({"set_side_of_pier", [&] { t.set_side_of_pier(0); }});
            p.push_back({"get_does_refraction", [&] { (void)t.get_does_refraction(); }});
            p.push_back({"set_does_refraction", [&] { t.set_does_refraction(false); }});
            p.push_back({"get_guide_rate", [&] { (void)t.get_guide_rate(); }});
            p.push_back({"set_guide_rate", [&] { t.set_guide_rate(alpacacore::GuideRate{}); }});
            p.push_back({"set_tracking_rate", [&] { t.set_tracking_rate(0); }});
            p.push_back({"find_home", [&] { t.find_home(); }});
            p.push_back({"park", [&] { t.park(); }});
            p.push_back({"unpark", [&] { t.unpark(); }});
            p.push_back({"set_tracking", [&] { t.set_tracking(false); }});
            p.push_back({"sync_to_coordinates", [&] { t.sync_to_coordinates(1.0, 1.0); }});
            p.push_back({"sync_to_target", [&] { t.sync_to_target(); }});
            p.push_back({"slew_to_coordinates_async", [&] { t.slew_to_coordinates_async(1.0, 1.0); }});
            p.push_back({"slew_to_target_async", [&] { t.slew_to_target_async(); }});
            p.push_back({"pulse_guide", [&] { t.pulse_guide(0, 0); }});
            p.push_back({"set_park", [&] { t.set_park(); }});
            p.push_back({"slew_to_target", [&] { t.slew_to_target(); }});
            p.push_back({"slew_to_alt_az", [&] { t.slew_to_alt_az(1.0, 1.0); }});
            p.push_back({"slew_to_alt_az_async", [&] { t.slew_to_alt_az_async(1.0, 1.0); }});
            p.push_back({"sync_to_alt_az", [&] { t.sync_to_alt_az(1.0, 1.0); }});
            for (int axis = 0; axis <= 2; ++axis) {
                p.push_back({"move_axis(" + std::to_string(axis) + ")", [&t, axis] { t.move_axis(axis, 0.0); }});
            }
            break;
        }
        case DeviceType::Camera: {
            auto& c = dynamic_cast<alpacacore::CameraDriver&>(d);
            p.push_back({"get_bayer_offset_x", [&] { (void)c.get_bayer_offset_x(); }});
            p.push_back({"get_bayer_offset_y", [&] { (void)c.get_bayer_offset_y(); }});
            p.push_back({"get_exposure_max", [&] { (void)c.get_exposure_max(); }});
            p.push_back({"get_exposure_min", [&] { (void)c.get_exposure_min(); }});
            p.push_back({"get_gain_max", [&] { (void)c.get_gain_max(); }});
            p.push_back({"get_gain_min", [&] { (void)c.get_gain_min(); }});
            p.push_back({"get_offset_max", [&] { (void)c.get_offset_max(); }});
            p.push_back({"get_offset_min", [&] { (void)c.get_offset_min(); }});
            p.push_back({"get_fast_readout", [&] { (void)c.get_fast_readout(); }});
            p.push_back({"set_fast_readout", [&] { c.set_fast_readout(false); }});
            p.push_back({"get_gains", [&] { (void)c.get_gains(); }});
            p.push_back({"get_offsets", [&] { (void)c.get_offsets(); }});
            p.push_back({"get_heat_sink_temperature", [&] { (void)c.get_heat_sink_temperature(); }});
            p.push_back({"get_sub_exposure_duration", [&] { (void)c.get_sub_exposure_duration(); }});
            p.push_back({"set_sub_exposure_duration", [&] { c.set_sub_exposure_duration(0.0); }});
            p.push_back({"pulse_guide", [&] { c.pulse_guide(0, 0); }});
            p.push_back({"stop_exposure", [&] { c.stop_exposure(); }});
            p.push_back({"get_electrons_per_adu", [&] { (void)c.get_electrons_per_adu(); }});
            p.push_back({"get_full_well_capacity", [&] { (void)c.get_full_well_capacity(); }});
            p.push_back({"get_readout_mode", [&] { (void)c.get_readout_mode(); }});
            p.push_back({"set_readout_mode", [&] { c.set_readout_mode(0); }});
            p.push_back({"get_readout_modes", [&] { (void)c.get_readout_modes(); }});
            p.push_back({"get_offset", [&] { (void)c.get_offset(); }});
            p.push_back({"set_offset", [&] { c.set_offset(0); }});
            p.push_back({"set_gain", [&] { c.set_gain(0); }});
            break;
        }
        case DeviceType::Switch: {
            // Id 0 and the entry's writable id: a read-only port or a switch without async control must not
            // answer NotImplemented before the connection check (iOptron, ToupTek StellaVita).
            auto& s = dynamic_cast<alpacacore::SwitchDriver&>(d);
            for (const int id : {0, entry.switch_writable_id}) {
                const std::string at = "(" + std::to_string(id) + ")";
                p.push_back({"set_switch" + at, [&s, id] { s.set_switch(id, false); }});
                p.push_back({"set_async" + at, [&s, id] { s.set_async(id, false); }});
                p.push_back({"set_switch_value" + at, [&s, id] { s.set_switch_value(id, 0.0); }});
                p.push_back({"set_async_value" + at, [&s, id] { s.set_async_value(id, 0.0); }});
            }
            break;
        }
        case DeviceType::Focuser: {
            auto& f = dynamic_cast<alpacacore::FocuserDriver&>(d);
            p.push_back({"get_step_size", [&] { (void)f.get_step_size(); }});
            p.push_back({"get_temp_comp", [&] { (void)f.get_temp_comp(); }});
            p.push_back({"set_temp_comp", [&] { f.set_temp_comp(false); }});
            p.push_back({"get_temperature", [&] { (void)f.get_temperature(); }});
            break;
        }
        case DeviceType::FilterWheel: {
            auto& f = dynamic_cast<alpacacore::FilterWheelDriver&>(d);
            p.push_back({"get_focus_offsets", [&] { (void)f.get_focus_offsets(); }});
            p.push_back({"set_focus_offsets", [&] { f.set_focus_offsets({}); }});
            p.push_back({"get_names", [&] { (void)f.get_names(); }});
            p.push_back({"set_names", [&] { f.set_names({}); }});
            break;
        }
        case DeviceType::Rotator: {
            auto& r = dynamic_cast<alpacacore::RotatorDriver&>(d);
            p.push_back({"get_mechanical_position", [&] { (void)r.get_mechanical_position(); }});
            p.push_back({"get_target_position", [&] { (void)r.get_target_position(); }});
            p.push_back({"set_target_position", [&] { r.set_target_position(0.0); }});
            p.push_back({"get_reverse", [&] { (void)r.get_reverse(); }});
            p.push_back({"set_reverse", [&] { r.set_reverse(false); }});
            p.push_back({"move_absolute", [&] { r.move_absolute(0.0); }});
            p.push_back({"move_mechanical", [&] { r.move_mechanical(0.0); }});
            p.push_back({"sync", [&] { r.sync(0.0); }});
            break;
        }
        case DeviceType::CoverCalibrator: {
            auto& c = dynamic_cast<alpacacore::CoverCalibratorDriver&>(d);
            p.push_back({"open_cover", [&] { c.open_cover(); }});
            p.push_back({"close_cover", [&] { c.close_cover(); }});
            p.push_back({"halt_cover", [&] { c.halt_cover(); }});
            break;
        }
        case DeviceType::ObservingConditions: {
            auto& o = dynamic_cast<alpacacore::ObservingConditionsDriver&>(d);
            p.push_back({"get_cloud_cover", [&] { (void)o.get_cloud_cover(); }});
            p.push_back({"get_rain_rate", [&] { (void)o.get_rain_rate(); }});
            p.push_back({"get_sky_brightness", [&] { (void)o.get_sky_brightness(); }});
            p.push_back({"get_sky_quality", [&] { (void)o.get_sky_quality(); }});
            p.push_back({"get_sky_temperature", [&] { (void)o.get_sky_temperature(); }});
            p.push_back({"get_star_fwhm", [&] { (void)o.get_star_fwhm(); }});
            p.push_back({"get_wind_direction", [&] { (void)o.get_wind_direction(); }});
            p.push_back({"get_wind_gust", [&] { (void)o.get_wind_gust(); }});
            p.push_back({"get_wind_speed", [&] { (void)o.get_wind_speed(); }});
            p.push_back({"get_sensor_description(CloudCover)", [&] { (void)o.get_sensor_description("CloudCover"); }});
            break;
        }
        default:
            break;
    }
    return p;
}

// Entries whose driver files belong to other tickets: the NotImplemented-while-disconnected defect is known and
// fixed there. Each is skipped until upstream #1919 closes; remove the entry when its driver is fixed.
bool not_implemented_pending(const ContractEntry& e) {
    static const std::set<std::string> pending = {
        "zwo_camera",       "zwo_telescope",     "skywatcher_telescope",
        "bisque_telescope", "synscan_telescope", "gemini_covercalibrator"};  // open_cover, close_cover, halt_cover
    return pending.count(e.id) == 1;
}

// Static capability getters: readable while disconnected (they must not throw).
std::vector<Probe> can_getter_probes(AlpacaDriver& d, const ContractEntry& entry) {
    std::vector<Probe> p;
    switch (entry.type) {
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
        case DeviceType::Switch: {
            // Per-id reads: answered statically or NotConnected as the entry states (case_can_getters).
            auto& s = dynamic_cast<alpacacore::SwitchDriver&>(d);
            const int id = entry.switch_writable_id;
            p.push_back({"get_can_write(writable id)", [&s, id] { (void)s.get_can_write(id); }});
            p.push_back({"get_can_async(writable id)", [&s, id] { (void)s.get_can_async(id); }});
            break;
        }
        default:
            break;
    }
    return p;
}

// Which entries have a tier-1 subject. InvalidValue: the entry states no reason for lacking a probe. Can getters:
// the types whose Can* reads can_getter_probes lists (the interface of the others has none).
bool has_invalid_subject(const ContractEntry& e) { return std::string(e.no_invalid_probe_reason).empty(); }
bool has_can_subject(const ContractEntry& e) {
    switch (e.type) {
        case DeviceType::Telescope:
        case DeviceType::Camera:
        case DeviceType::Rotator:
        case DeviceType::Focuser:
        case DeviceType::Switch:
            return true;
        default:
            return false;
    }
}

// ---- case bodies ----------------------------------------------------------
// [[maybe_unused]]: a vendors-off build expands CONTRACT_SWEEP_ENTRIES to nothing, so no case
// references them and -Werror=unused-function would otherwise fail that build.

[[maybe_unused]] void case_invalid_before_not_connected(const ContractEntry& e) {
    auto d = e.make(0);
    REQUIRE(d != nullptr);
    REQUIRE_FALSE(d->get_connected());
    // Registered only for an entry with probes (has_invalid_subject); the guard case pins that.
    const auto probes = invalid_value_probes(*d, e);
    REQUIRE_FALSE(probes.empty());
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
    // Not-implemented members: NotConnected first, never NotImplemented (pending entries wait on upstream #1919).
    if (!not_implemented_pending(e)) {
        for (const auto& [name, fn] : not_implemented_probes(*d, e)) {
            INFO(e.id << " " << name << " must not answer NotImplemented while disconnected");
            CHECK(thrown_code(fn) != err::NotImplemented);
        }
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

// Filter wheels while disconnected (AGENTS.md "FilterWheel semantics"): every name is non-empty, and a names
// write and an offsets write are accepted and read back. One case over all filter wheel entries; it is
// registered once, not per entry, because the registry macro carries no device-type kind. Most drivers size
// their name and offset vectors only at connect, so a fresh disconnected read is empty: the non-empty-name loop
// then checks nothing, and the write legs use a fixed five-slot list so the read-back is not `{} == {}`.
[[maybe_unused]] void case_filterwheel_names_offsets_disconnected(const ContractEntry& e) {
    INFO(e.id);
    auto d = e.make(0);
    REQUIRE(d != nullptr);
    auto& f = dynamic_cast<alpacacore::FilterWheelDriver&>(*d);
    REQUIRE_FALSE(f.get_connected());
    std::vector<std::string> names;
    CHECK(thrown_code([&] { names = f.get_names(); }) == -1);
    for (const auto& n : names) {
        INFO(e.id << " name '" << n << "'");
        CHECK_FALSE(n.empty());
    }
    std::vector<std::string> written = names;
    written.resize(names.empty() ? 5 : names.size());
    for (std::size_t i = 0; i < written.size(); ++i) written[i] = "Sweep " + std::to_string(i + 1);
    CHECK(thrown_code([&] { f.set_names(written); }) == -1);
    CHECK(f.get_names() == written);
    std::vector<int> offsets;
    CHECK(thrown_code([&] { offsets = f.get_focus_offsets(); }) == -1);
    std::vector<int> written_offsets = offsets;
    written_offsets.resize(offsets.empty() ? 5 : offsets.size());
    for (std::size_t i = 0; i < written_offsets.size(); ++i) written_offsets[i] = static_cast<int>(10 * (i + 1));
    CHECK(thrown_code([&] { f.set_focus_offsets(written_offsets); }) == -1);
    CHECK(f.get_focus_offsets() == written_offsets);
}

[[maybe_unused]] void case_can_getters(const ContractEntry& e) {
    auto d = e.make(0);
    REQUIRE(d != nullptr);
    const auto probes = can_getter_probes(*d, e);
    REQUIRE_FALSE(probes.empty());
    // A switch states per entry whether CanWrite/CanAsync answer statically or throw NotConnected.
    const int expected = e.type == DeviceType::Switch && e.switch_caps_disconnected ==
                                                             alpacacore::test::contract::DisconnectedRead::NotConnected
                             ? err::NotConnected
                             : -1;
    for (const auto& [name, fn] : probes) {
        INFO(e.id << " " << name);
        CHECK(thrown_code(fn) == expected);
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
    int target_unset_error = err::ValueNotSet;
    // Telescope rows whose fake cannot reach a parked state (CanPark false, or a park the fake cannot answer)
    // say why and where here, and case_t2_async_initiators then skips the Park leg. Null on every row that parks.
    const char* park_unavailable = nullptr;
    // Likewise for MoveAxis(axis, 0) on a row whose fake does not answer the rate command. A row whose driver
    // reports CanMoveAxis false on every axis needs no reason: there is no call to time.
    const char* move_axis_unavailable = nullptr;
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
            // Connected is not part of this: a get_connected() that blocked behind the connect
            // (async_connectable.h; no driver does today) would only return once the connect has finished.
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
            // On a row whose get_connected() blocked behind the connect (async_connectable.h; none does today) this
            // check could not fail: that read would only return after the hold. It is load-bearing on every
            // row since the getters are lock-free (red with the holds set to 0).
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

// Case 2, telescope rows only. New implementations follow Master Interfaces 1.0.27
// (InvalidOperation); legacy drivers retain ValueNotSet until their separate migration.
[[maybe_unused]] void case_t2_target_flags(const Tier2Host& h) {
    // Registered over telescope hosts only (kind TEL below); the guard case pins that.
    REQUIRE(h.type == DeviceType::Telescope);
    Hosted hosted = connected_host(h);
    auto& t = dynamic_cast<alpacacore::TelescopeDriver&>(*hosted.driver);
    CHECK(thrown_code([&] { (void)t.get_target_right_ascension(); }) == h.target_unset_error);
    CHECK(thrown_code([&] { (void)t.get_target_declination(); }) == h.target_unset_error);
    t.set_target_right_ascension(5.5);
    CHECK(t.get_target_right_ascension() == 5.5);
    CHECK(thrown_code([&] { (void)t.get_target_declination(); }) == h.target_unset_error);
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

// Upper bound for the firmware / SDK-version hooks. They return a cached value without taking the driver
// mutex (alpacadriver.h; decision 0016), so they take microseconds. 100 ms is about three orders above that,
// which a sanitizer build or a loaded CI runner does not reach, and below the shortest per-row handshake
// hold (kSerialBoardHold, 150 ms), so a hook that waits for the handshake exceeds it on every row that holds.
constexpr std::chrono::milliseconds kHookBound{100};

// Firmware and SDK hooks. get_device_firmware() and get_device_sdk_version() are cheap: they return at once while a
// connect is in flight, and never throw while disconnected.
[[maybe_unused]] void case_t2_firmware_sdk_hooks(const Tier2Host& h) {
    INFO(h.id << " over " << h.fake_header);
    {
        Hosted hosted = h.connectable(false);
        REQUIRE(hosted.driver != nullptr);
        CHECK(thrown_code([&] { (void)hosted.driver->get_device_firmware(); }) == -1);
        CHECK(thrown_code([&] { (void)hosted.driver->get_device_sdk_version(); }) == -1);
    }
    Hosted hosted = h.connectable(h.can_hold_connect);
    REQUIRE(hosted.driver != nullptr);
    hosted.driver->connect();
    const auto t0 = Clock::now();
    int fw_code = 0;
    int sdk_code = 0;
    fw_code = thrown_code([&] { (void)hosted.driver->get_device_firmware(); });
    sdk_code = thrown_code([&] { (void)hosted.driver->get_device_sdk_version(); });
    const auto took = Clock::now() - t0;
    // A hook that waited for the handshake leaves Connecting false by the time it returns.
    const bool still_connecting = hosted.driver->get_connecting();
    INFO("both hooks took " << std::chrono::duration_cast<std::chrono::milliseconds>(took).count() << " ms");
    CHECK(fw_code == -1);
    CHECK(sdk_code == -1);
    CHECK(took < kHookBound);
    if (h.can_hold_connect) {
        CHECK(still_connecting);
    } else {
        INFO(h.id << " cannot show Connecting reading true: "
                  << (h.connecting_unobservable != nullptr ? h.connecting_unobservable : "(no reason stated)"));
        REQUIRE(h.connecting_unobservable != nullptr);
        REQUIRE(std::string(h.connecting_unobservable).size() > 0);
    }
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (hosted.driver->get_connecting() && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    REQUIRE_FALSE(hosted.driver->get_connecting());
}

// Asynchronous initiators, telescope rows only. MoveAxis(axis, 0) and Park are asynchronous initiators: each returns
// well under ConformU's 1 s STANDARD target (AGENTS.md "Telescope Park / MoveAxis(axis, 0)"). MoveAxis runs on one
// connected host and Park on another, so a driver that is parked does not turn the MoveAxis leg into a fast
// InvalidWhileParked throw; each call must also return without throwing.
constexpr std::chrono::milliseconds kInitiatorBound{1000};

[[maybe_unused]] void case_t2_async_initiators(const Tier2Host& h) {
    REQUIRE(h.type == DeviceType::Telescope);
    INFO(h.id << " over " << h.fake_header);
    {
        Hosted hosted = connected_host(h);
        auto& t = dynamic_cast<alpacacore::TelescopeDriver&>(*hosted.driver);
        if (h.move_axis_unavailable != nullptr) {
            INFO(h.id << " cannot answer MoveAxis(axis, 0): " << h.move_axis_unavailable);
            CHECK(std::string(h.move_axis_unavailable).size() > 0);
        } else {
            for (int axis = 0; axis < 3; ++axis) {
                if (!t.get_can_move_axis(axis)) continue;
                INFO("move_axis(" << axis << ", 0)");
                const auto t0 = Clock::now();
                const int code = thrown_code([&] { t.move_axis(axis, 0.0); });
                const auto took = Clock::now() - t0;
                CHECK(code == -1);
                CHECK(took < kInitiatorBound);
            }
        }
    }
    Hosted hosted = connected_host(h);
    auto& t = dynamic_cast<alpacacore::TelescopeDriver&>(*hosted.driver);
    if (h.park_unavailable != nullptr || !t.get_can_park()) {
        INFO(h.id << " cannot park: " << (h.park_unavailable != nullptr ? h.park_unavailable : "(no reason stated)"));
        REQUIRE(h.park_unavailable != nullptr);
        REQUIRE(std::string(h.park_unavailable).size() > 0);
        return;
    }
    const auto t0 = Clock::now();
    const int code = thrown_code([&] { t.park(); });
    const auto took = Clock::now() - t0;
    INFO("park() took " << std::chrono::duration_cast<std::chrono::milliseconds>(took).count() << " ms");
    CHECK(code == -1);
    CHECK(took < kInitiatorBound);
}

// The ASCOM type of each DeviceState member, per device type, from the Platform 7 interface docs: the
// DeviceState section of each interface page at https://ascom-standards.org/newdocs/ (ITelescopeV4,
// ICameraV4, IFocuserV4, IRotatorV4, IFilterWheelV3, ICoverCalibratorV2, ISwitchV3), whose member lists
// the rows below follow. DeviceStateValue holds
// the alternative that goes out as JSON, so the held alternative is what is compared. Only device types with
// a tier-2 host are listed; a new tier-2 device type needs a row here. Switch members carry a switch id
// suffix ("GetSwitch3"), stripped before the lookup.
enum class StateType { Bool, Int, Double, String };

const char* state_type_name(StateType t) {
    switch (t) {
        case StateType::Bool:
            return "bool";
        case StateType::Int:
            return "std::int32_t";
        case StateType::Double:
            return "double";
        case StateType::String:
            return "std::string";
    }
    return "?";
}

bool state_value_has_type(const alpacacore::DeviceStateValue& v, StateType t) {
    switch (t) {
        case StateType::Bool:
            return std::holds_alternative<bool>(v);
        case StateType::Int:
            return std::holds_alternative<std::int32_t>(v);
        case StateType::Double:
            return std::holds_alternative<double>(v);
        case StateType::String:
            return std::holds_alternative<std::string>(v);
    }
    return false;
}

const std::map<std::string, StateType>* ascom_device_state_types(DeviceType type) {
    using T = StateType;
    static const std::map<DeviceType, std::map<std::string, StateType>> table = {
        {DeviceType::Camera,
         {{"CameraState", T::Int},
          {"CCDTemperature", T::Double},
          {"CoolerPower", T::Double},
          {"HeatSinkTemperature", T::Double},
          {"ImageReady", T::Bool},
          {"IsPulseGuiding", T::Bool},
          {"PercentCompleted", T::Int},
          {"TimeStamp", T::String}}},
        {DeviceType::Telescope,
         {{"Altitude", T::Double},
          {"AtHome", T::Bool},
          {"AtPark", T::Bool},
          {"Azimuth", T::Double},
          {"Declination", T::Double},
          {"IsPulseGuiding", T::Bool},
          {"RightAscension", T::Double},
          {"SideOfPier", T::Int},
          {"SiderealTime", T::Double},
          {"Slewing", T::Bool},
          {"Tracking", T::Bool},
          {"UTCDate", T::String},
          {"TimeStamp", T::String}}},
        {DeviceType::Focuser,
         {{"IsMoving", T::Bool}, {"Position", T::Int}, {"Temperature", T::Double}, {"TimeStamp", T::String}}},
        {DeviceType::Rotator,
         {{"IsMoving", T::Bool}, {"MechanicalPosition", T::Double}, {"Position", T::Double}, {"TimeStamp", T::String}}},
        {DeviceType::FilterWheel, {{"Position", T::Int}, {"TimeStamp", T::String}}},
        {DeviceType::CoverCalibrator,
         {{"Brightness", T::Int},
          {"CalibratorState", T::Int},
          {"CalibratorChanging", T::Bool},
          {"CoverState", T::Int},
          {"CoverMoving", T::Bool},
          {"TimeStamp", T::String}}},
        {DeviceType::Switch,
         {{"GetSwitch", T::Bool},
          {"GetSwitchValue", T::Double},
          {"StateChangeComplete", T::Bool},
          {"TimeStamp", T::String}}},
    };
    const auto it = table.find(type);
    return it == table.end() ? nullptr : &it->second;
}

// Case 4b. Every connected DeviceState entry carries the ASCOM member's JSON type (an integer member emitted as
// a double reaches the client as "0.0", which ConformU cannot coerce), and a member the table does not know
// for the device type fails, so a new member needs a type entry above.
[[maybe_unused]] void case_t2_device_state_value_types(const Tier2Host& h) {
    Hosted hosted = connected_host(h);
    const auto* members = ascom_device_state_types(h.type);
    REQUIRE(members != nullptr);
    const auto state = hosted.driver->get_device_state();
    REQUIRE_FALSE(state.empty());
    for (const auto& entry : state) {
        INFO(h.id << " " << entry.name);
        std::string key = entry.name;
        if (h.type == DeviceType::Switch) {
            while (!key.empty() && std::isdigit(static_cast<unsigned char>(key.back()))) {
                key.pop_back();
            }
        }
        const auto it = members->find(key);
        REQUIRE(it != members->end());
        INFO("expected " << state_type_name(it->second));
        CHECK(state_value_has_type(entry.value, it->second));
    }
}

// Case 5, camera rows only. ImageArray before any exposure is the ICameraV4 InvalidOperation, flagged
// asked_too_early so the router logs it at DEBUG rather than ERROR (issue #1167).
[[maybe_unused]] void case_t2_image_array_asked_too_early(const Tier2Host& h) {
    // Registered over camera hosts only (kind CAM below); the guard case pins that.
    REQUIRE(h.type == DeviceType::Camera);
    Hosted hosted = connected_host(h);
    auto& camera = dynamic_cast<alpacacore::CameraDriver&>(*hosted.driver);
    bool thrown = false;
    try {
        (void)camera.get_image_array();
    } catch (const alpacacore::AlpacaException& e) {
        thrown = true;
        INFO(h.id << " ImageArray before any exposure: '" << e.what() << "'");
        CHECK(e.error_code() == err::InvalidOperation);
        CHECK(e.is_asked_too_early());
    }
    CHECK(thrown);
}

// Case 6, camera rows only. LastExposureDuration and LastExposureStartTime before any successful image are the
// ICameraV4 InvalidOperation, not ValueNotSet (issue #1477); while disconnected they raise NotConnected.
[[maybe_unused]] void case_t2_last_exposure_before_image(const Tier2Host& h) {
    REQUIRE(h.type == DeviceType::Camera);
    {
        // Disconnected: NotConnected wins over the no-image InvalidOperation.
        Hosted down = h.connectable(false);
        REQUIRE(down.driver != nullptr);
        auto& cam = dynamic_cast<alpacacore::CameraDriver&>(*down.driver);
        INFO(h.id << " LastExposure* while disconnected");
        alpacacore::test::require_error_code([&] { (void)cam.get_last_exposure_duration(); }, err::NotConnected);
        alpacacore::test::require_error_code([&] { (void)cam.get_last_exposure_start_time(); }, err::NotConnected);
    }
    Hosted hosted = connected_host(h);
    INFO(h.id << " LastExposure* before any exposure");
    alpacacore::test::require_last_exposure_unset(dynamic_cast<alpacacore::CameraDriver&>(*hosted.driver));
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
// The fake behind a FakeMountServer row's Hosted: while `silent` is set the responder answers nothing, as a mount
// that hangs with the socket open (tier 3).
struct MountServerHold {
    std::shared_ptr<std::atomic<bool>> silent = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<alpacacore::test::FakeMountServer> server;
};

// A FakeMountServer row's driver factory, `Over(port, hold, clock)`, on the default task clock (tier 2).
template <auto Over>
std::unique_ptr<AlpacaDriver> on_default_clock(int port, bool hold) {
    return Over(port, hold, alpacacore::util::default_task_clock());
}

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
        auto fake = std::make_shared<MountServerHold>();
        auto held = [responder, first, silent = fake->silent](const std::string& chunk) {
            if (silent->load()) return std::string();
            if (first->exchange(false)) std::this_thread::sleep_for(kHold);
            return responder(chunk);
        };
        fake->server = std::make_shared<alpacacore::test::FakeMountServer>(held);
        REQUIRE(fake->server->ok());
        Hosted hosted;
        hosted.driver = make(fake->server->port(), hold);
        hosted.fake = fake;
        return hosted;
    };
    h.failing = [make]() { return host_over_held_port(SOCK_STREAM, [make](int port) { return make(port, false); }); };
    return h;
}
#endif

#ifdef ALPACACORE_ENABLE_ZWO
std::unique_ptr<AlpacaDriver> zwo_telescope_over(int port, bool hold, alpacacore::util::TaskClock& clock) {
    namespace zwo = alpacacore::vendor::zwo;
    zwo::ConnectionInfo info;
    info.type = zwo::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = hold ? kHeldReplyTimeoutMs : 250;
    return zwo::create_zwo_telescope(0, info, clock);
}

// Recipe of test_zwo_concurrency_stress.cpp: LX200 replies, tracking reported on.
// Knob, no fake change: the responder also accepts :Sr/:Sd so the target setters succeed.
Tier2Host tier2_host_zwo_telescope() {
    return mount_server_host(
        "zwo_telescope", "zwo", "zwo_telescope",
        [](const std::string& chunk) -> std::string {
            if (chunk.find(":GVP#") != std::string::npos) return "AM5#";
            if (chunk.find(":GMA#") != std::string::npos) return "012345ABCDEF#";
            if (chunk.find(":GU#") != std::string::npos) return "NG#";
            if (chunk.find(":GR#") != std::string::npos) return "12:00:00#";
            if (chunk.find(":GD#") != std::string::npos) return "+45*00:00#";
            // Tracking on, and the target setters (:Sr, :Sd) accepted; "0#" would make the driver report
            // "ZWO mount rejected target RA".
            return (chunk.find(":GAT") != std::string::npos || chunk.find(":Sr") != std::string::npos ||
                    chunk.find(":Sd") != std::string::npos)
                       ? "1#"
                       : "0#";
        },
        on_default_clock<zwo_telescope_over>);
}

Tier2Host tier2_host_zwo_camera() {
    Tier2Host h{"zwo_camera", "zwo", "camera", "fake_zwo_sdk.h", DeviceType::Camera, "zwo_camera", {}, true, {}, ""};
    h.connectable = [](bool hold) {
        auto sdk = std::make_shared<alpacacore::test::FakeZWOSDK>();
        if (hold) {
            sdk->before_call = [](const std::string& name) {
                if (name == "open_camera") std::this_thread::sleep_for(kHold);
            };
        }
        return host_over(sdk, [](alpacacore::test::FakeZWOSDK& fake) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::zwo::create_zwo_camera(0, fake.camera.camera_id, fake);
        });
    };
    h.failing = []() {
        auto sdk = std::make_shared<alpacacore::test::FakeZWOSDK>();
        sdk->fail_open = true;
        return host_over(sdk, [](alpacacore::test::FakeZWOSDK& fake) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::zwo::create_zwo_camera(0, fake.camera.camera_id, fake);
        });
    };
    return h;
}

// The EFW, EAF and CAA fakes share one recipe: open_delay holds the connect open, fail_open fails it.
Tier2Host tier2_host_zwo_filterwheel() {
    Tier2Host h{"zwo_filterwheel",
                "zwo",
                "filterwheel",
                "fake_zwo_efw_sdk.h",
                DeviceType::FilterWheel,
                "zwo_filterwheel",
                {},
                true,
                {},
                ""};
    h.connectable = [](bool hold) {
        auto sdk = std::make_shared<alpacacore::test::FakeZWOEFWSDK>();
        if (hold) sdk->open_delay = kHold;
        return host_over(sdk, [](alpacacore::test::FakeZWOEFWSDK& fake) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::zwo::create_zwo_efw_filterwheel(0, fake.id, fake);
        });
    };
    h.failing = []() {
        auto sdk = std::make_shared<alpacacore::test::FakeZWOEFWSDK>();
        sdk->fail_open = true;
        return host_over(sdk, [](alpacacore::test::FakeZWOEFWSDK& fake) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::zwo::create_zwo_efw_filterwheel(0, fake.id, fake);
        });
    };
    return h;
}

Tier2Host tier2_host_zwo_focuser() {
    Tier2Host h{"zwo_focuser", "zwo", "focuser", "fake_zwo_eaf_sdk.h", DeviceType::Focuser, "zwo_focuser", {},
                true,          {},    ""};
    h.connectable = [](bool hold) {
        auto sdk = std::make_shared<alpacacore::test::FakeZWOEAFSDK>();
        if (hold) sdk->open_delay = kHold;
        return host_over(sdk, [](alpacacore::test::FakeZWOEAFSDK& fake) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, fake.id, fake);
        });
    };
    h.failing = []() {
        auto sdk = std::make_shared<alpacacore::test::FakeZWOEAFSDK>();
        sdk->fail_open = true;
        return host_over(sdk, [](alpacacore::test::FakeZWOEAFSDK& fake) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::zwo::create_zwo_eaf_focuser(0, fake.id, fake);
        });
    };
    return h;
}

Tier2Host tier2_host_zwo_rotator() {
    Tier2Host h{"zwo_rotator", "zwo", "rotator", "fake_zwo_caa_sdk.h", DeviceType::Rotator, "zwo_rotator", {},
                true,          {},    ""};
    h.connectable = [](bool hold) {
        auto sdk = std::make_shared<alpacacore::test::FakeZWOCAASDK>();
        if (hold) sdk->open_delay = kHold;
        return host_over(sdk, [](alpacacore::test::FakeZWOCAASDK& fake) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::zwo::create_zwo_caa_rotator(0, fake.id, fake);
        });
    };
    h.failing = []() {
        auto sdk = std::make_shared<alpacacore::test::FakeZWOCAASDK>();
        sdk->fail_open = true;
        return host_over(sdk, [](alpacacore::test::FakeZWOCAASDK& fake) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::zwo::create_zwo_caa_rotator(0, fake.id, fake);
        });
    };
    return h;
}
#endif

#ifdef ALPACACORE_ENABLE_CELESTRON
std::unique_ptr<AlpacaDriver> celestron_telescope_over(int port, bool hold, alpacacore::util::TaskClock& clock) {
    namespace cel = alpacacore::vendor::celestron;
    cel::ConnectionInfo info;
    info.type = cel::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = hold ? kHeldReplyTimeoutMs : 50;
    return cel::create_celestron_telescope_with_site(0, info, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                                                     cel::CelestronAlignmentSetting::Auto, clock);
}

// Recipe of test_celestron_concurrency_stress.cpp: a position-pair sized reply ends every read at once.
Tier2Host tier2_host_celestron_telescope() {
    Tier2Host h = mount_server_host(
        "celestron_telescope", "celestron", "celestron_telescope",
        [](const std::string&) { return std::string("00000000,00000000#"); },
        on_default_clock<celestron_telescope_over>);
    h.move_axis_unavailable =
        "the fixed-reply responder does not answer the variable-rate axis frame, so move_axis() times out and "
        "throws (fake_mount_server.h, celestron_protocol_wrapper move_axis_variable_rate)";
    h.park_unavailable =
        "the fixed-reply responder does not answer the park command, so park() times out after the 50 ms reply timeout "
        "and throws (fake_mount_server.h)";
    return h;
}

Tier2Host tier2_host_celestron_cgx_telescope() {
    namespace cel = alpacacore::vendor::celestron;
    auto fake = std::make_shared<alpacacore::test::FakeCelestronCgxMount>();
    const auto make = [fake](int device_number) -> std::unique_ptr<AlpacaDriver> {
        cel::CelestronCgxConnectionInfo info;
        info.port_path = fake->slave_path();
        info.site_latitude_degrees = 45.0;
        info.site_longitude_degrees = -75.0;
        return cel::create_celestron_cgx_telescope(device_number, info);
    };
    Tier2Host host{
        "celestron_telescope_cgx",
        "celestron",
        "telescope",
        "fake_celestron_cgx_mount.h",
        DeviceType::Telescope,
        "celestron_telescope_cgx",
        [fake, make](bool) -> Hosted { return {fake, make(0)}; },
        false,
        [fake, make]() -> Hosted {
            fake->corrupt_next_reply_checksum();
            return {fake, make(0)};
        },
        "",
        "Firmware replies are immediate; Connecting may complete before its first poll (fake protocol timing)",
        err::InvalidOperation};
    return host;
}

// FakeCelestronFocuser over a pty (recipe of test_celestron_focuser.cpp, issue #896).
Tier2Host tier2_host_celestron_focuser() {
    Tier2Host h{"celestron_focuser",
                "celestron",
                "focuser",
                "fake_celestron_focuser.h",
                DeviceType::Focuser,
                "celestron_focuser",
                {},
                false,
                {},
                "",
                "The fake answers every AUX frame at once and has no hold knob; Connecting may complete before "
                "its first poll (fake_celestron_focuser.h)"};
    h.connectable = [](bool) {
        auto fake = std::make_shared<alpacacore::test::FakeCelestronFocuser>();
        return host_over(fake, [](const alpacacore::test::FakeCelestronFocuser& f) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::celestron::create_celestron_focuser(0, f.slave_path());
        });
    };
    h.failing = []() {
        Hosted hosted;
        hosted.driver = alpacacore::vendor::celestron::create_celestron_focuser(0, kAbsentPort);
        return hosted;
    };
    return h;
}
#endif

#ifdef ALPACACORE_ENABLE_SYNSCAN
std::unique_ptr<AlpacaDriver> synscan_telescope_over(int port, bool hold, alpacacore::util::TaskClock& clock) {
    namespace syn = alpacacore::vendor::synscan;
    syn::ConnectionInfo info;
    info.type = syn::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = hold ? kHeldReplyTimeoutMs : 50;
    return syn::create_synscan_telescope_with_site(0, info, syn::SynScanVersion::V4, std::nullopt, std::nullopt,
                                                   std::nullopt, std::nullopt, syn::SynScanAlignmentSetting::Auto,
                                                   clock);
}

// Recipe of test_synscan_concurrency_stress.cpp: V4 and the default responder's 'K' echo, which the connect gates on.
Tier2Host tier2_host_synscan_telescope() {
    Tier2Host h = mount_server_host("synscan_telescope", "synscan", "synscan_telescope",
                                    alpacacore::test::FakeMountServer::default_responder(),
                                    on_default_clock<synscan_telescope_over>);
    h.park_unavailable =
        "FakeMountServer::default_responder() does not answer the SynScan position read (the driver logs 'Invalid "
        "SynScan RA/Dec response'), so park() throws DriverException before it can start the slew "
        "(fake_mount_server.h, synscan_telescope_driver.cpp park())";
    return h;
}
#endif

#ifdef ALPACACORE_ENABLE_ONSTEP
std::unique_ptr<AlpacaDriver> onstep_telescope_over(int port, bool hold, alpacacore::util::TaskClock& clock) {
    namespace ons = alpacacore::vendor::onstep;
    ons::ConnectionInfo info;
    info.type = ons::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = hold ? kHeldReplyTimeoutMs : 50;
    return ons::create_onstep_telescope(0, info, clock);
}

// Recipe of test_onstep_concurrency_stress.cpp, with an idle OnStepX for
// :GU# and :hP#. The default "0#" has no 'N', so :GU# would report a GOTO that
// never ends, and Park stops running motion and waits for it first (#1430).
Tier2Host tier2_host_onstep_telescope() {
    auto idle_onstep = [fallback = alpacacore::test::FakeMountServer::default_responder()](const std::string& chunk) {
        if (chunk.rfind(":GU", 0) == 0) {
            return std::string("nNp#");  // not tracking, no GOTO, not parked
        }
        if (chunk.rfind(":hP", 0) == 0) {
            return std::string("1");  // park accepted (bare digit)
        }
        return fallback(chunk);
    };
    return mount_server_host("onstep_telescope", "onstep", "onstep_telescope", idle_onstep,
                             on_default_clock<onstep_telescope_over>);
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
// FakeIGuiderV4L2 behind LockedIGuiderV4L2 (recipe of test_ioptron_iguider_camera.cpp), both in one holder: the
// driver keeps a reference to the seam.
struct IGuiderV4L2Hold {
    alpacacore::test::FakeIGuiderV4L2 fake;
    alpacacore::test::LockedIGuiderV4L2 v4l2{fake};
};

Tier2Host tier2_host_iguider_camera() {
    Tier2Host h{"iguider_camera",
                "iguider",
                "camera",
                "fake_iguider_v4l2.h",
                DeviceType::Camera,
                "iguider_camera",
                {},
                true,
                {},
                ""};
    const auto make = [](IGuiderV4L2Hold& s) -> std::unique_ptr<AlpacaDriver> {
        return alpacacore::vendor::ioptron::create_iguider_camera(0, "", s.v4l2);
    };
    h.connectable = [make](bool hold) {
        auto s = std::make_shared<IGuiderV4L2Hold>();
        if (hold) s->fake.open_delay = kHold;  // fake_iguider_v4l2.h: the explicit open() hook
        return host_over(s, make);
    };
    h.failing = [make]() {
        auto s = std::make_shared<IGuiderV4L2Hold>();
        s->fake.devices.clear();  // no iGuider attached: the connect fails at the scan
        return host_over(s, make);
    };
    return h;
}
#endif

#ifdef ALPACACORE_ENABLE_ASTROASIS
// FakeHidTransport behind the Locked decorator, over the by-path factory's driver. The fake answers every
// exchange at once, so the handshake cannot be held open.
Tier2Host tier2_host_astroasis_focuser() {
    Tier2Host h{"astroasis_focuser",
                "astroasis",
                "focuser",
                "fake_astroasis_hid.h",
                DeviceType::Focuser,
                "astroasis_focuser",
                {},
                false,
                {},
                ""};
    h.connecting_unobservable =
        "FakeHidTransport answers the handshake in-process with no delay knob (fake_astroasis_hid.h write(), "
        "astroasis_protocol_wrapper.cpp connect())";
    auto make = [](bool dead) {
        auto state = std::make_shared<alpacacore::test::FakeHidState>();
        state->dead = dead;
        Hosted hosted;
        hosted.driver = alpacacore::vendor::astroasis::create_astroasis_focuser(
            0, "/dev/hidraw-fake",
            std::make_unique<alpacacore::test::LockedAstroasisHidTransport>(
                std::make_unique<alpacacore::test::FakeHidTransport>(state)));
        hosted.fake = state;
        return hosted;
    };
    h.connectable = [make](bool) { return make(false); };
    // Reads time out, as a pulled cable does: the handshake gets no reply and connect fails.
    h.failing = [make]() { return make(true); };
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

#ifdef ALPACACORE_ENABLE_ALTAIR
// Recipe of altair_concurrency_stress.cpp: the touptek_camera host through the
// Altair factory, over the same fake.
Tier2Host tier2_host_altair_camera() {
    Tier2Host h{"altair_camera",
                "altair",
                "camera",
                "fake_touptek_sdk.h",
                DeviceType::Camera,
                "altair_camera",
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
            return alpacacore::vendor::altair::create_altair_camera(0, 0, s.sdk);
        });
    };
    h.failing = []() {
        auto hold = std::make_shared<ToupTekSdkHold>();
        hold->fake.throw_from.insert("open_camera_by_id");  // fake_touptek_sdk.h fault injection
        return host_over(hold, [](ToupTekSdkHold& s) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::altair::create_altair_camera(0, 0, s.sdk);
        });
    };
    return h;
}
#endif

#if defined(ALPACACORE_ENABLE_IOPTRON) && defined(ALPACACORE_IOPTRON_POWERBOX)
// The iMate PowerBox GPIO side: request_delay holds the connect open, fail_request fails it.
Tier2Host tier2_host_ioptron_switch() {
    Tier2Host h{"ioptron_switch",
                "ioptron",
                "switch",
                "fake_ioptron_gpio.h",
                DeviceType::Switch,
                "ioptron_switch",
                {},
                true,
                {},
                ""};
    const auto make = [](alpacacore::test::FakeIoptronGpio& gpio) -> std::unique_ptr<AlpacaDriver> {
        return alpacacore::vendor::ioptron::create_ioptron_switch(
            0, alpacacore::vendor::ioptron::default_imate_powerbox_config(), gpio);
    };
    h.connectable = [make](bool hold_open) {
        auto gpio = std::make_shared<alpacacore::test::FakeIoptronGpio>();
        if (hold_open) gpio->request_delay = kHold;
        return host_over(gpio, make);
    };
    h.failing = [make]() {
        auto gpio = std::make_shared<alpacacore::test::FakeIoptronGpio>();
        gpio->fail_request = true;
        return host_over(gpio, make);
    };
    return h;
}
#endif

#if defined(ALPACACORE_ENABLE_TOUPTEK) && defined(ALPACACORE_TOUPTEK_STELLAVITA)
// The StellaVita power box: open_delay holds the connect open, fail_open fails it. The fake and its
// Locked decorator live in one holder, because the driver keeps a reference to the box.
struct StellaVitaHold {
    alpacacore::test::FakeTouptekPowerboxSDK fake;
    alpacacore::test::LockedTouptekPowerboxSDK box{fake};
};

Tier2Host tier2_host_touptek_switch() {
    Tier2Host h{"touptek_switch",
                "touptek",
                "switch",
                "fake_touptek_powerbox_sdk.h",
                DeviceType::Switch,
                "touptek_switch",
                {},
                true,
                {},
                ""};
    h.connectable = [](bool hold_open) {
        auto hold = std::make_shared<StellaVitaHold>();
        if (hold_open) hold->fake.open_delay = kHold;
        return host_over(hold, [](StellaVitaHold& s) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::touptek::create_touptek_switch(
                0, alpacacore::vendor::touptek::default_stellavita_config(), s.box);
        });
    };
    h.failing = []() {
        auto hold = std::make_shared<StellaVitaHold>();
        hold->fake.fail_open = true;
        return host_over(hold, [](StellaVitaHold& s) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::touptek::create_touptek_switch(
                0, alpacacore::vendor::touptek::default_stellavita_config(), s.box);
        });
    };
    return h;
}
#endif

#ifdef ALPACACORE_ENABLE_PLAYERONE
Tier2Host tier2_host_playerone_camera() {
    Tier2Host h{"playerone_camera",
                "playerone",
                "camera",
                "fake_playerone_sdk.h",
                DeviceType::Camera,
                "playerone_camera",
                {},
                true,
                {},
                ""};
    h.connectable = [](bool hold) {
        auto sdk = std::make_shared<alpacacore::test::FakePlayerOneSDK>();
        if (hold) sdk->set_open_delay(kHold);
        return host_over(sdk, [](alpacacore::test::FakePlayerOneSDK& fake) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::playerone::create_playerone_camera(0, 0, fake);
        });
    };
    h.failing = []() {
        auto sdk = std::make_shared<alpacacore::test::FakePlayerOneSDK>();
        sdk->set_no_cameras(true);
        return host_over(sdk, [](alpacacore::test::FakePlayerOneSDK& fake) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::playerone::create_playerone_camera(0, 0, fake);
        });
    };
    return h;
}

Tier2Host tier2_host_playerone_filterwheel() {
    Tier2Host h{"playerone_filterwheel",
                "playerone",
                "filterwheel",
                "fake_playerone_pw_sdk.h",
                DeviceType::FilterWheel,
                "playerone_filterwheel",
                {},
                true,
                {},
                ""};
    h.connectable = [](bool hold) {
        auto sdk = std::make_shared<alpacacore::test::FakePlayerOnePWSDK>();
        if (hold) sdk->set_open_delay(kHold);
        return host_over(sdk, [](alpacacore::test::FakePlayerOnePWSDK& fake) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::playerone::create_playerone_filterwheel(0, 0, fake);
        });
    };
    h.failing = []() {
        auto sdk = std::make_shared<alpacacore::test::FakePlayerOnePWSDK>();
        sdk->set_no_wheels(true);
        return host_over(sdk, [](alpacacore::test::FakePlayerOnePWSDK& fake) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::playerone::create_playerone_filterwheel(0, 0, fake);
        });
    };
    return h;
}
#endif

#ifdef ALPACACORE_ENABLE_SVBONY
Tier2Host tier2_host_svbony_camera() {
    Tier2Host h{"svbony_camera",
                "svbony",
                "camera",
                "fake_svbony_sdk.h",
                DeviceType::Camera,
                "svbony_camera",
                {},
                true,
                {},
                ""};
    h.connectable = [](bool hold) {
        auto sdk = std::make_shared<alpacacore::test::FakeSVBSDK>();
        if (hold) sdk->set_open_delay(kHold);
        return host_over(sdk, [](alpacacore::test::FakeSVBSDK& fake) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::svbony::create_svbony_camera(0, 0, fake);
        });
    };
    h.failing = []() {
        auto sdk = std::make_shared<alpacacore::test::FakeSVBSDK>();
        sdk->set_camera_present(false);
        return host_over(sdk, [](alpacacore::test::FakeSVBSDK& fake) -> std::unique_ptr<AlpacaDriver> {
            return alpacacore::vendor::svbony::create_svbony_camera(0, 0, fake);
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
// with no such probe (invalid_probe_reason_for() in contract_sweep.h states why; the guard case pins it),
// CAM = a camera, which has no such probe either and also gets the ImageArray case.
// clang-format off
#ifdef ALPACACORE_ENABLE_ZWO
#define CS2_ZWO(X) X(zwo_telescope, TEL) X(zwo_camera, CAM) X(zwo_filterwheel, PRB) X(zwo_focuser, NPR) X(zwo_rotator, PRB)
#else
#define CS2_ZWO(X)
#endif
#ifdef ALPACACORE_ENABLE_CELESTRON
#define CS2_CELESTRON(X) X(celestron_telescope, TEL) X(celestron_cgx_telescope, TEL) X(celestron_focuser, NPR)
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
#ifdef ALPACACORE_IOPTRON_POWERBOX
#define CS2_IOPTRON_POWERBOX(X) X(ioptron_switch, PRB)
#else
#define CS2_IOPTRON_POWERBOX(X)
#endif
#ifdef ALPACACORE_ENABLE_IOPTRON
#define CS2_IOPTRON(X) \
    X(ioptron_telescope, TEL) X(ioptron_focuser, NPR) X(iguider_camera, CAM) CS2_IOPTRON_POWERBOX(X)
#else
#define CS2_IOPTRON(X)
#endif
#ifdef ALPACACORE_ENABLE_GEMINI
#define CS2_GEMINI(X) X(gemini_focuser, NPR) X(gemini_covercalibrator, PRB) X(gemini_switch, PRB)
#else
#define CS2_GEMINI(X)
#endif
#ifdef ALPACACORE_ENABLE_ASTROASIS
#define CS2_ASTROASIS(X) X(astroasis_focuser, NPR)
#else
#define CS2_ASTROASIS(X)
#endif
#ifdef ALPACACORE_ENABLE_QHY
#define CS2_QHY(X) X(qhy_camera, CAM) X(qhy_filterwheel, PRB) X(qhy_focuser, NPR)
#else
#define CS2_QHY(X)
#endif
#ifdef ALPACACORE_TOUPTEK_STELLAVITA
#define CS2_TOUPTEK_STELLAVITA(X) X(touptek_switch, PRB)
#else
#define CS2_TOUPTEK_STELLAVITA(X)
#endif
#ifdef ALPACACORE_ENABLE_TOUPTEK
#define CS2_TOUPTEK(X) X(touptek_camera, CAM) CS2_TOUPTEK_STELLAVITA(X)
#else
#define CS2_TOUPTEK(X)
#endif
#ifdef ALPACACORE_ENABLE_ALTAIR
#define CS2_ALTAIR(X) X(altair_camera, CAM)
#else
#define CS2_ALTAIR(X)
#endif
#ifdef ALPACACORE_ENABLE_SVBONY
#define CS2_SVBONY(X) X(svbony_camera, CAM)
#else
#define CS2_SVBONY(X)
#endif
#ifdef ALPACACORE_ENABLE_PLAYERONE
#define CS2_PLAYERONE(X) X(playerone_camera, CAM) X(playerone_filterwheel, PRB)
#else
#define CS2_PLAYERONE(X)
#endif
#ifdef ALPACACORE_ENABLE_GPHOTO
#define CS2_GPHOTO(X) X(gphoto_camera, CAM)
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
    CS2_ASTROASIS(X) \
    CS2_GEMINI(X) \
    CS2_QHY(X) \
    CS2_TOUPTEK(X) \
    CS2_ALTAIR(X) \
    CS2_SVBONY(X) \
    CS2_PLAYERONE(X) \
    CS2_GPHOTO(X) \
    CS2_WANDERERASTRO(X)
// clang-format on

// ---------------------------------------------------------------------------
// Tier 3 (issue #1861): failure after start. On a connected host the type's asynchronous operation starts (an
// exposure, a slew, a move), the fake then fails while it runs, and the completion property (ImageReady, Slewing,
// IsMoving, FilterWheel Position) must raise the failure: never a clean completion, and never "still running"
// past the budget (the ASCOM async FAQ, https://ascom-standards.org/AlpacaDeveloper/Async.htm; the shared rule
// "A failed async operation body keeps its error", ascom-contract.instructions.md). Before the fault the healthy
// fake keeps the operation running; after it the recipe undoes the fault and the property must still never report
// done. One recipe per tier-2 host of those five types, listed in CONTRACT_SWEEP_TIER3_HOSTS; the guard case pins
// the list to the tier-2 hosts.
// ---------------------------------------------------------------------------

struct Tier3Recipe {
    // The type's asynchronous initiator, on a healthy fake. It must return normally.
    std::function<void(Hosted&)> start;
    // Makes the fake fail while the operation runs.
    std::function<void(Hosted&)> fail;
    // Undoes `fail`: the fake answers again and still models the operation running (it never completes it).
    std::function<void(Hosted&)> recover;
    // What `fail` does to the fake, quoted in the failure message.
    const char* fault;
    // The driver over a fake that can run the operation, when the tier-2 host's fake cannot (its responder does not
    // model it); empty to use the tier-2 host's connectable. Same contract as Tier2Host::connectable.
    std::function<Hosted(bool hold)> connectable = {};
};

bool is_tier3_type(DeviceType t) {
    return t == DeviceType::Camera || t == DeviceType::Telescope || t == DeviceType::Focuser ||
           t == DeviceType::Rotator || t == DeviceType::FilterWheel;
}

// One read of the completion property: `done` when it reports the operation finished; `code` the AlpacaException it
// raised, -1 when none. Any other exception passes through.
struct T3Read {
    bool done = false;
    int code = -1;
    std::string message;
};

T3Read t3_read(AlpacaDriver& d, DeviceType type) {
    T3Read r;
    try {
        switch (type) {
            case DeviceType::Camera:
                r.done = dynamic_cast<alpacacore::CameraDriver&>(d).get_image_ready();
                break;
            case DeviceType::Telescope:
                r.done = !dynamic_cast<alpacacore::TelescopeDriver&>(d).get_slewing();
                break;
            case DeviceType::Focuser:
                r.done = !dynamic_cast<alpacacore::FocuserDriver&>(d).get_is_moving();
                break;
            case DeviceType::Rotator:
                r.done = !dynamic_cast<alpacacore::RotatorDriver&>(d).get_is_moving();
                break;
            case DeviceType::FilterWheel:
                // -1 is the moving sentinel (IFilterWheelV3 Position); any slot is a finished move.
                r.done = dynamic_cast<alpacacore::FilterWheelDriver&>(d).get_position() != -1;
                break;
            default:
                FAIL("no tier-3 completion property for this device type");
        }
    } catch (const AlpacaException& e) {
        r.code = e.error_code();
        r.message = e.what();
    }
    return r;
}

const char* t3_outcome(const T3Read& r) {
    return r.code != -1 ? "raised" : (r.done ? "reported done" : "still running");
}

// The longest real wait after the fault is the QHY CFW3 move wait (12 s from the start, 11 s after the fault): a
// protocol-wrapper wire deadline on steady_clock (decision 0005 rule 6), in a driver not on the task clock. A recipe
// whose raise waits on a driver timer on the task clock moves its fake clock past that timer instead (0005 rule 2).
// The rest is margin so a slow runner does not turn a late raise into "still running".
constexpr std::chrono::seconds kTier3Budget{20};
// How long the healthy fake must keep the operation running before the fault.
constexpr std::chrono::seconds kTier3Control{1};
// How long the case watches the property after recover. A recipe on the task clock has already stepped its polls
// in `recover` (t3_on_clock); a driver not on the clock gets three seconds of its real-time status poll to see the
// answering fake.
constexpr std::chrono::seconds kTier3RecoverWindow{3};

[[maybe_unused]] void case_t3_failure_after_start(const Tier2Host& h, const Tier3Recipe& r) {
    REQUIRE(is_tier3_type(h.type));
    INFO(h.id << " over " << h.fake_header << "; fault: " << r.fault);
    Hosted hosted = r.connectable ? r.connectable(false) : h.connectable(false);
    REQUIRE(hosted.driver != nullptr);
    REQUIRE(connect_and_observe(*hosted.driver).settled);
    REQUIRE(hosted.driver->get_connected());
    {
        int start_code = -1;
        std::string start_message;
        try {
            r.start(hosted);
        } catch (const AlpacaException& e) {
            start_code = e.error_code();
            start_message = e.what();
        }
        INFO("the initiator threw " << start_code << ": '" << start_message << "'");
        REQUIRE(start_code == -1);
    }
    // Control: on the healthy fake the operation keeps running, neither raising nor finishing, so the raise below is
    // the fault's. A fake that finished (or failed) the operation by itself would let a driver that ignores every
    // failure pass.
    for (const auto until = Clock::now() + kTier3Control; Clock::now() < until;) {
        const T3Read rd = t3_read(*hosted.driver, h.type);
        INFO("completion property before the fault: " << t3_outcome(rd) << " (code " << rd.code << ", '" << rd.message
                                                      << "')");
        REQUIRE(rd.code == -1);
        REQUIRE_FALSE(rd.done);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    r.fail(hosted);
    T3Read after;
    for (const auto deadline = Clock::now() + kTier3Budget; Clock::now() < deadline;) {
        after = t3_read(*hosted.driver, h.type);
        if (after.done || after.code != -1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    INFO("completion property after the fault: " << t3_outcome(after) << " (code " << after.code << ", '"
                                                 << after.message << "')");
    CHECK_FALSE(after.done);
    REQUIRE(after.code != -1);
    // Every fault keeps the transport open, and the case ends well inside util::kLinkStalenessBound (30 s), so the
    // link is faulted, not lost: the read raises the operation's failure, not NotConnected (CONTEXT.md "Connected
    // vs link health", decision 0009).
    CHECK(after.code != err::NotConnected);
    // While the fault lasts the link stays faulted, so the next read raises too (decision 0009 rule 1).
    CHECK(t3_read(*hosted.driver, h.type).code != -1);
    // The fake answers again: the next good reply may clear the link fault (0009 rule 1), but the operation it
    // interrupted never finished, so the property raises a failure the body kept or reports the operation running;
    // done is a body that swallowed the failure behind the live link fault ("A failed async operation body keeps its
    // error", ascom-contract.instructions.md).
    r.recover(hosted);
    T3Read recovered;
    for (const auto until = Clock::now() + kTier3RecoverWindow; Clock::now() < until;) {
        recovered = t3_read(*hosted.driver, h.type);
        if (recovered.done) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    INFO("completion property after recover: " << t3_outcome(recovered) << " (code " << recovered.code << ", '"
                                               << recovered.message << "')");
    CHECK_FALSE(recovered.done);
}

// Initiators shared by the recipes; [[maybe_unused]] because a build with none of their vendors calls none.
[[maybe_unused]] void t3_start_slew(Hosted& hosted, double ra_offset_hours, double dec) {
    auto& t = dynamic_cast<alpacacore::TelescopeDriver&>(*hosted.driver);
    if (t.get_can_set_tracking() && !t.get_tracking()) t.set_tracking(true);
    t.slew_to_coordinates_async(std::fmod(t.get_right_ascension() + ra_offset_hours + 24.0, 24.0), dec);
}

// A slew to `ha_hours` west of the meridian, which no mount here takes as a meridian flip.
[[maybe_unused]] void t3_start_slew_at_hour_angle(Hosted& hosted, double ha_hours, double dec) {
    auto& t = dynamic_cast<alpacacore::TelescopeDriver&>(*hosted.driver);
    if (t.get_can_set_tracking() && !t.get_tracking()) t.set_tracking(true);
    t.slew_to_coordinates_async(std::fmod(t.get_sidereal_time() - ha_hours + 24.0, 24.0), dec);
}

[[maybe_unused]] void t3_start_focuser_move(Hosted& hosted, int steps_away) {
    auto& f = dynamic_cast<alpacacore::FocuserDriver&>(*hosted.driver);
    f.move(f.get_absolute() ? f.get_position() + steps_away : steps_away);
}

[[maybe_unused]] void t3_start_exposure(Hosted& hosted, double seconds) {
    dynamic_cast<alpacacore::CameraDriver&>(*hosted.driver).start_exposure(seconds, true);
}

template <class Fake>
Fake& t3_fake(Hosted& hosted) {
    return *static_cast<Fake*>(hosted.fake.get());
}

// A recipe whose raise or recover waits on a driver timer on the task clock (decision 0005 rule 2) builds the driver
// on one of these and steps it in its own legs, so its case's real-time loops wait on no driver timer, only on wire
// reads (0005 rule 6). The recipe's lambdas hold it, so it outlives the Hosted driver.
using T3Clock = std::shared_ptr<alpacacore::test::FakeTaskClock>;

// One poll tick of every driver stepped here (100 ms to 1 s periods); each step waits for the woken threads to
// settle, as advance_until() in test_celestron_async_slew_failure.cpp does.
constexpr std::chrono::milliseconds kT3Step{250};

// Steps `clock` until `done` holds, at most `virtual_limit` of virtual time; returns whether `done` held.
[[maybe_unused]] bool t3_step(alpacacore::test::FakeTaskClock& clock, std::chrono::milliseconds virtual_limit,
                              const std::function<bool()>& done = {}) {
    for (std::chrono::milliseconds stepped{0}; stepped < virtual_limit; stepped += kT3Step) {
        if (done && done()) return true;
        clock.wait_for_waiters(1, std::chrono::milliseconds(100));
        clock.advance(kT3Step);
        clock.wait_for_woken_settled(std::chrono::seconds(2));
    }
    return done && done();
}

// `r` on `clock`: `start` steps 3 s so the control leg reads state the healthy polls refreshed; `fail` steps until
// the property raises or reports done, at most 15 s, so a 3-failure latch on a 1 s poll trips and the link stays
// inside util::kLinkStalenessBound (30 s); `recover` steps 3 s, three periods of the slowest poll (1 s:
// kStatusPollPeriod in the Celestron, SynScan and OnStep telescope drivers), so the driver sees the answering fake.
[[maybe_unused]] Tier3Recipe t3_on_clock(Tier3Recipe r, DeviceType type, T3Clock clock) {
    r.start = [start = std::move(r.start), clock](Hosted& hosted) {
        start(hosted);
        t3_step(*clock, std::chrono::seconds(3));
    };
    r.fail = [fail = std::move(r.fail), type, clock](Hosted& hosted) {
        fail(hosted);
        t3_step(*clock, std::chrono::seconds(15), [&] {
            const T3Read rd = t3_read(*hosted.driver, type);
            return rd.done || rd.code != -1;
        });
    };
    r.recover = [recover = std::move(r.recover), clock](Hosted& hosted) {
        recover(hosted);
        t3_step(*clock, std::chrono::seconds(3));
    };
    return r;
}

#if defined(ALPACACORE_ENABLE_ZWO) || defined(ALPACACORE_ENABLE_CELESTRON) || defined(ALPACACORE_ENABLE_SYNSCAN) || \
    defined(ALPACACORE_ENABLE_ONSTEP)
// FakeMountServer rows: the tier-2 responders answer a fixed reply and model no goto, so each row's recipe brings a
// responder whose goto, once started, never ends (the slew status keeps reporting it running). The goto starts, then
// the mount stops answering with the socket open.
// `started` is set by the responder when the GOTO reaches it, so the fault lands on a goto the mount is running, not on
// its dispatch. All four drivers are on the task clock: `make(port, hold, clock)` builds one on the recipe's clock.
template <class MakeDriver>
Tier3Recipe tier3_mount_server_recipe(const char* id, const char* vendor, std::shared_ptr<std::atomic<bool>> started,
                                      alpacacore::test::FakeMountServer::Responder r, MakeDriver make) {
    auto clock = std::make_shared<alpacacore::test::FakeTaskClock>();
    auto on_clock = [make, clock](int port, bool hold) { return make(port, hold, *clock); };
    return t3_on_clock({[](Hosted& hosted) { t3_start_slew_at_hour_angle(hosted, 1.0, 30.0); },
                        [started](Hosted& hosted) {
                            const auto deadline = Clock::now() + std::chrono::seconds(5);
                            while (!started->load() && Clock::now() < deadline)
                                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                            REQUIRE(started->load());
                            t3_fake<MountServerHold>(hosted).silent->store(true);
                        },
                        [](Hosted& hosted) { t3_fake<MountServerHold>(hosted).silent->store(false); },
                        "FakeMountServer responder silent mid-goto",
                        mount_server_host(id, vendor, id, std::move(r), on_clock).connectable},
                       DeviceType::Telescope, clock);
}
#endif

#ifdef ALPACACORE_ENABLE_CELESTRON
// A NexStar hand controller on a CGX-L (model 24, firmware 5.35), aligned, tracking EQ north; the replies are those
// of test_celestron_async_slew_failure.cpp. Falsified by: get_slewing() answering from the cached slew flag when the
// "L" status poll fails.
Tier3Recipe tier3_recipe_celestron_telescope() {
    auto started = std::make_shared<std::atomic<bool>>(false);
    auto responder = [started](const std::string& chunk) -> std::string {
        if (chunk.empty()) return "0#";
        switch (chunk[0]) {
            case 'm':
                return std::string(1, static_cast<char>(24)) + "#";
            case 'V':
                return std::string("\x05\x23#");
            case 'w':
                return std::string(8, '\0') + "#";
            case 'e':
            case 'E':
            case 'z':
            case 'Z':
                return "12AB0500,20000500#";
            case 'r':
            case 'R':
            case 'b':
            case 'B':
                started->store(true);
                return "#";
            case 'L':
                return started->load() ? "1#" : "0#";
            case 'J':
                return "1#";
            case 'p':
                return "W#";
            case 't':
                return std::string("\x02#");
            case 'T':
            case 'W':
            case 'M':
                return "#";
            case 'P': {  // AUX passthrough: P len dev op ...
                const unsigned char op = chunk.size() > 3 ? static_cast<unsigned char>(chunk[3]) : 0;
                if (op == 0xFE) return std::string("\x00\x00#", 3);  // bus device firmware probe
                if (op == 0x02 || op == 0x17) {                      // MC_GOTO_FAST / MC_GOTO_SLOW
                    started->store(true);
                    return "#";
                }
                if (op == 0x13)
                    return started->load() ? std::string("\x00#", 2) : std::string("\xFF#");  // MC_SLEW_DONE
                return std::string("\xFF#");
            }
            default:
                return "0#";
        }
    };
    return tier3_mount_server_recipe("celestron_telescope", "celestron", started, responder, celestron_telescope_over);
}

// CGX: homed and tracking (the driver refuses tracking before FindHome), a goto then stays running because the fake
// stops completing motions, and the mount then stops answering. Falsified by: the slew body swallowing the failed
// motor-controller read and clearing slewing.
Tier3Recipe tier3_recipe_celestron_cgx_telescope() {
    return {[](Hosted& hosted) {
                auto& fake = t3_fake<alpacacore::test::FakeCelestronCgxMount>(hosted);
                auto& t = dynamic_cast<alpacacore::TelescopeDriver&>(*hosted.driver);
                fake.set_auto_complete_motions(true);
                t.find_home();
                const auto deadline = Clock::now() + std::chrono::seconds(10);
                while (t.get_slewing() && Clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                t.set_tracking(true);
                fake.set_auto_complete_motions(false);
                t.slew_to_coordinates_async(std::fmod(t.get_sidereal_time() + 23.0, 24.0), 85.0);
            },
            [](Hosted& hosted) { t3_fake<alpacacore::test::FakeCelestronCgxMount>(hosted).set_muted(true); },
            [](Hosted& hosted) { t3_fake<alpacacore::test::FakeCelestronCgxMount>(hosted).set_muted(false); },
            "FakeCelestronCgxMount::set_muted(true) mid-goto"};
}

// Focus motor: a GOTO stays running until complete_move(); the motor then stops answering. Falsified by: the
// move-poll treating an unanswered position read as arrival.
Tier3Recipe tier3_recipe_celestron_focuser() {
    auto clock = std::make_shared<alpacacore::test::FakeTaskClock>();
    return t3_on_clock(
        {[](Hosted& hosted) { t3_start_focuser_move(hosted, 500); },
         [](Hosted& hosted) { t3_fake<alpacacore::test::FakeCelestronFocuser>(hosted).set_muted(true); },
         [](Hosted& hosted) { t3_fake<alpacacore::test::FakeCelestronFocuser>(hosted).set_muted(false); },
         "FakeCelestronFocuser::set_muted(true) mid-move",
         [clock](bool) {
             return host_over(std::make_shared<alpacacore::test::FakeCelestronFocuser>(),
                              [&](const alpacacore::test::FakeCelestronFocuser& f) -> std::unique_ptr<AlpacaDriver> {
                                  return alpacacore::vendor::celestron::create_celestron_focuser(0, f.slave_path(),
                                                                                                 *clock);
                              });
         }},
        DeviceType::Focuser, clock);
}
#endif

#ifdef ALPACACORE_ENABLE_SKYWATCHER
// UDP motor controller: a two-hour goto ramps on the board's axis model, so it is still running when the board stops
// answering. Falsified by: the slew task ending on a timed-out status read as if the axes had stopped.
Tier3Recipe tier3_recipe_skywatcher_telescope() {
    auto clock = std::make_shared<alpacacore::test::FakeTaskClock>();
    return t3_on_clock(
        {[](Hosted& hosted) { t3_start_slew(hosted, 2.0, 30.0); },
         [](Hosted& hosted) { t3_fake<alpacacore::test::FakeSkyWatcherMount>(hosted).set_silent(true); },
         [](Hosted& hosted) { t3_fake<alpacacore::test::FakeSkyWatcherMount>(hosted).set_silent(false); },
         "FakeSkyWatcherMount::set_silent(true) mid-goto",
         [clock](bool) {
             return host_over(std::make_shared<alpacacore::test::FakeSkyWatcherMount>(),
                              [&](const alpacacore::test::FakeSkyWatcherMount& m) -> std::unique_ptr<AlpacaDriver> {
                                  sw::ConnectionInfo info;
                                  info.type = sw::ConnectionType::Network;
                                  info.host = "127.0.0.1";
                                  info.udp_port = m.port();
                                  info.response_timeout_ms = 250;
                                  return sw::create_skywatcher_telescope(0, info, 51.477833, -0.0015, 46.0, {}, {},
                                                                         *clock);
                              });
         }},
        DeviceType::Telescope, clock);
}

// Serial motor board (a southern site): same recipe, the board stops answering with the port open.
Tier3Recipe tier3_recipe_skywatcher_telescope_serial() {
    auto clock = std::make_shared<alpacacore::test::FakeTaskClock>();
    return t3_on_clock(
        {[](Hosted& hosted) { t3_start_slew(hosted, 2.0, -30.0); },
         [](Hosted& hosted) { t3_fake<alpacacore::test::FakeSkyWatcherSerialBoard>(hosted).set_muted(true); },
         [](Hosted& hosted) { t3_fake<alpacacore::test::FakeSkyWatcherSerialBoard>(hosted).set_muted(false); },
         "FakeSkyWatcherSerialBoard::set_muted(true) mid-goto",
         [clock](bool) {
             return host_over(
                 std::make_shared<alpacacore::test::FakeSkyWatcherSerialBoard>(),
                 [&](const alpacacore::test::FakeSkyWatcherSerialBoard& b) -> std::unique_ptr<AlpacaDriver> {
                     sw::ConnectionInfo info;
                     info.type = sw::ConnectionType::Serial;
                     info.port_path = b.slave_path();
                     info.baud_rate = 9600;
                     info.response_timeout_ms = 300;
                     return sw::create_skywatcher_telescope(0, info, -33.856784, 151.215297, 42.0, {}, {}, *clock);
                 });
         }},
        DeviceType::Telescope, clock);
}
#endif

#ifdef ALPACACORE_ENABLE_IOPTRON
// The firmware reports "slewing" (system status 2) so the goto stays running, then every position and status read
// fails. Falsified by: the settle loop treating failed status reads as a landed goto.
Tier3Recipe tier3_recipe_ioptron_telescope() {
    auto clock = std::make_shared<alpacacore::test::FakeTaskClock>();
    return t3_on_clock(
        {[](Hosted& hosted) {
             t3_fake<alpacacore::test::FakeIoptronMount>(hosted).set_system_status(2);
             t3_start_slew(hosted, 2.0, 30.0);
         },
         [](Hosted& hosted) { t3_fake<alpacacore::test::FakeIoptronMount>(hosted).set_fail_reads(true); },
         [](Hosted& hosted) { t3_fake<alpacacore::test::FakeIoptronMount>(hosted).set_fail_reads(false); },
         "FakeIoptronMount::set_fail_reads(true) mid-goto",
         [clock](bool) {
             auto mount = std::make_shared<alpacacore::test::FakeIoptronMount>("0012", 12.0);
             REQUIRE(mount->ok());
             return host_over(mount, [&](const alpacacore::test::FakeIoptronMount& m) -> std::unique_ptr<AlpacaDriver> {
                 alpacacore::vendor::ioptron::ConnectionInfo info;
                 info.type = alpacacore::vendor::ioptron::ConnectionType::Network;
                 info.host = "127.0.0.1";
                 info.tcp_port = m.port();
                 return alpacacore::vendor::ioptron::create_ioptron_telescope(0, info, *clock);
             });
         }},
        DeviceType::Telescope, clock);
}

// iEAF: the motor keeps reporting it is running, then the firmware stops answering. Falsified by: the move poll
// reporting IsMoving false on a failed :FI# read.
Tier3Recipe tier3_recipe_ioptron_focuser() {
    auto clock = std::make_shared<alpacacore::test::FakeTaskClock>();
    return t3_on_clock({[](Hosted& hosted) {
                            t3_fake<alpacacore::test::FakeIoptronIeaf>(hosted).set_focuser_moving(true);
                            t3_start_focuser_move(hosted, 500);
                        },
                        [](Hosted& hosted) { t3_fake<alpacacore::test::FakeIoptronIeaf>(hosted).set_silent(true); },
                        [](Hosted& hosted) { t3_fake<alpacacore::test::FakeIoptronIeaf>(hosted).set_silent(false); },
                        "FakeIoptronIeaf::set_silent(true) mid-move",
                        [clock](bool) {
                            return host_over(
                                std::make_shared<alpacacore::test::FakeIoptronIeaf>(),
                                [&](const alpacacore::test::FakeIoptronIeaf& f) -> std::unique_ptr<AlpacaDriver> {
                                    return alpacacore::vendor::ioptron::create_ieaf_focuser(0, f.slave_path(), "ieaf",
                                                                                            *clock);
                                });
                        }},
                       DeviceType::Focuser, clock);
}

// The iGuider fake for tier 3. From `start` on, before_frame_return holds each finished frame until `fail` claims it
// (then until `recover`) or, with no fault, for kHealthyDelay. `fail` sets `stalled` before it moves kHolding to
// kFaulted, and the hook moves kHolding to kReleased, so exactly one of them wins: a frame already past read_frame()'s
// stalled check never reaches the driver after the fault (the old 300 ms sleep let one through under load).
// The driver runs on `clock` (decision 0005 rule 2): `fail` moves it past the exposure deadline.
struct IGuiderT3Hold : IGuiderV4L2Hold {
    enum Phase : int { kIdle, kHolding, kReleased, kFaulted };
    alpacacore::test::FakeTaskClock clock;
    // Only a run without the fault (the no-op mutant) gets here: inside the 10.7 s deadline, so the exposure ends.
    static constexpr std::chrono::seconds kHealthyDelay{5};
    // Bounds every hold, so a case that fails before `recover` still disconnects.
    static constexpr std::chrono::seconds kHoldBound{30};
    std::atomic<int> phase{kIdle};

    void hold_frame() {
        for (const auto entered = Clock::now(); Clock::now() - entered < kHoldBound;) {
            int p = phase.load();
            if (p == kIdle || p == kReleased || (p == kFaulted && !fake.stalled.load())) return;
            if (p == kHolding && Clock::now() - entered >= kHealthyDelay && phase.compare_exchange_strong(p, kReleased))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};

// iGuider: the stream stops during the exposure (frames held as above), and the task clock moves 11 s, past the
// exposure deadline (10.7 s for 0.02 s) and kStallTimeout (10 s), under util::kLinkStalenessBound. The deadline ends
// the exposure as failed, not only the link. Falsified by: the exposure worker waiting on read_frame() with no
// deadline, so ImageReady stays false and never raises.
Tier3Recipe tier3_recipe_iguider_camera() {
    const auto make = [](IGuiderT3Hold& s) -> std::unique_ptr<AlpacaDriver> {
        return alpacacore::vendor::ioptron::create_iguider_camera(0, "", s.v4l2, s.clock);
    };
    return {[](Hosted& hosted) {
                t3_fake<IGuiderT3Hold>(hosted).phase.store(IGuiderT3Hold::kHolding);
                t3_start_exposure(hosted, 0.02);
            },
            [](Hosted& hosted) {
                auto& s = t3_fake<IGuiderT3Hold>(hosted);
                s.fake.stalled.store(true);
                int holding = IGuiderT3Hold::kHolding;
                s.phase.compare_exchange_strong(holding, IGuiderT3Hold::kFaulted);
                s.clock.advance(std::chrono::seconds(11));
            },
            [](Hosted& hosted) { t3_fake<IGuiderT3Hold>(hosted).fake.stalled.store(false); },
            "FakeIGuiderV4L2::stalled = true mid-exposure",
            [make](bool) {
                auto s = std::make_shared<IGuiderT3Hold>();
                // Set before the stream worker starts: the hook is not locked.
                s->fake.before_frame_return = [hold = s.get()](std::int32_t) { hold->hold_frame(); };
                return host_over(s, make);
            }};
}
#endif

#ifdef ALPACACORE_ENABLE_SYNSCAN
// A SynScan V4 handset tracking, the replies those of test_synscan_async_slew_failure.cpp. Falsified by: the slew
// task clearing slewing when the "L" poll times out.
Tier3Recipe tier3_recipe_synscan_telescope() {
    auto started = std::make_shared<std::atomic<bool>>(false);
    auto responder = [started](const std::string& chunk) -> std::string {
        if (chunk.empty()) return "0#";
        switch (chunk[0]) {
            case 'K':
                return std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
            case 'e':
            case 'E':
            case 'z':
            case 'Z':
                return "12AB0500,20000500#";
            case 'r':
            case 'R':
            case 'b':
            case 'B':
                started->store(true);
                return "#";
            case 'L':
                return started->load() ? "1#" : "0#";
            case 'm':
                return std::string(1, static_cast<char>(50)) + "#";
            case 't':
                return std::string("\x01#");
            case 'w':
                return std::string(8, '\0') + "#";
            case 'T':
            case 'W':
            case 'M':
            case 'P':
                return "#";
            default:
                return "0#";
        }
    };
    return tier3_mount_server_recipe("synscan_telescope", "synscan", started, responder, synscan_telescope_over);
}
#endif

#ifdef ALPACACORE_ENABLE_ONSTEP
// OnStep tracking and idle, then a goto running once :MS# is accepted; the replies are those of
// test_onstep_link_loss.cpp. Falsified by: get_slewing() reporting the cached :GU# state when the status read times
// out.
Tier3Recipe tier3_recipe_onstep_telescope() {
    auto started = std::make_shared<std::atomic<bool>>(false);
    auto responder = [started](const std::string& cmd) -> std::string {
        if (cmd.rfind(":GVP", 0) == 0) return "On-Step#";
        if (cmd.rfind(":GVN", 0) == 0) return "10.23a#";
        if (cmd.rfind(":GU", 0) == 0) return started->load() ? "pEo190#" : "NpEo190#";
        if (cmd.rfind(":GR", 0) == 0) return "01:00:00.0000#";
        if (cmd.rfind(":GD", 0) == 0) return "+10*00:00.000#";
        if (cmd.rfind(":GA", 0) == 0) return "+40*00:00#";
        if (cmd.rfind(":GZ", 0) == 0) return "000*00:00#";
        if (cmd.rfind(":MS", 0) == 0) {
            started->store(true);
            return "0";
        }
        if (cmd.rfind(":Q", 0) == 0) return "";
        if (cmd.rfind(":S", 0) == 0 || cmd.rfind(":GXEE", 0) == 0) return "1";
        return "0#";
    };
    return tier3_mount_server_recipe("onstep_telescope", "onstep", started, responder, onstep_telescope_over);
}
#endif

#ifdef ALPACACORE_ENABLE_ASTROASIS
// Oasis: the device keeps reporting "moving", then the state poll goes unanswered. Falsified by: the status poll
// clearing IsMoving when the 0x32 read times out.
Tier3Recipe tier3_recipe_astroasis_focuser() {
    auto clock = std::make_shared<alpacacore::test::FakeTaskClock>();
    return t3_on_clock({[](Hosted& hosted) { t3_start_focuser_move(hosted, 500); },
                        [](Hosted& hosted) {
                            auto& st = t3_fake<alpacacore::test::FakeHidState>(hosted);
                            std::lock_guard<std::mutex> lock(st.mutex);
                            st.dead = true;
                        },
                        [](Hosted& hosted) {
                            auto& st = t3_fake<alpacacore::test::FakeHidState>(hosted);
                            std::lock_guard<std::mutex> lock(st.mutex);
                            st.dead = false;
                        },
                        "FakeHidState::dead = true mid-move",
                        [clock](bool) {
                            auto state = std::make_shared<alpacacore::test::FakeHidState>();
                            Hosted hosted;
                            hosted.driver = alpacacore::vendor::astroasis::create_astroasis_focuser(
                                0, "/dev/hidraw-fake",
                                std::make_unique<alpacacore::test::LockedAstroasisHidTransport>(
                                    std::make_unique<alpacacore::test::FakeHidTransport>(state)),
                                *clock);
                            hosted.fake = state;
                            return hosted;
                        }},
                       DeviceType::Focuser, clock);
}
#endif

#ifdef ALPACACORE_ENABLE_GEMINI
// MyFocuserPro2: the firmware reports moving, then answers the state queries with frames the driver cannot parse,
// as a wedged firmware does. Falsified by: the IsMoving read answering false when the ":01#" reply is invalid.
Tier3Recipe tier3_recipe_gemini_focuser() {
    auto clock = std::make_shared<alpacacore::test::FakeTaskClock>();
    return t3_on_clock(
        {[](Hosted& hosted) {
             t3_fake<alpacacore::test::FakeGeminiFocuser>(hosted).set_moving(true);
             t3_start_focuser_move(hosted, 500);
         },
         [](Hosted& hosted) { t3_fake<alpacacore::test::FakeGeminiFocuser>(hosted).set_garbage_state_replies(true); },
         [](Hosted& hosted) { t3_fake<alpacacore::test::FakeGeminiFocuser>(hosted).set_garbage_state_replies(false); },
         "FakeGeminiFocuser::set_garbage_state_replies(true) mid-move",
         [clock](bool) {
             return host_over(std::make_shared<alpacacore::test::FakeGeminiFocuser>(),
                              [&](const alpacacore::test::FakeGeminiFocuser& f) -> std::unique_ptr<AlpacaDriver> {
                                  return alpacacore::vendor::gemini::create_gemini_focuser(0, f.slave_path(), 9600,
                                                                                           *clock);
                              });
         }},
        DeviceType::Focuser, clock);
}
#endif

#ifdef ALPACACORE_ENABLE_WANDERERASTRO
// The tier-2 streamer with the task clock the wrapper's silence check reads (decision 0005 rule 2). Members are
// destroyed after the driver, the clock last.
struct WandererT3Hold {
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeSerialStreamer streamer{kWandererSfwFrame, std::chrono::milliseconds(50)};
};

// SFW: the streamed frame keeps reporting the old slot, so the move cannot land; the stream then stops with the port
// open (a hung MCU), and the task clock moves past kFilterWheelLinkSilenceMs (10 s). Falsified by: Position reporting
// the requested slot when the stream dies.
Tier3Recipe tier3_recipe_wandererastro_filterwheel() {
    return {[](Hosted& hosted) {
                auto& f = dynamic_cast<alpacacore::FilterWheelDriver&>(*hosted.driver);
                f.set_position(f.get_position() == 1 ? 2 : 1);
            },
            [](Hosted& hosted) {
                auto& s = t3_fake<WandererT3Hold>(hosted);
                s.streamer.set_muted(true);
                // Twice, each step waiting for the reader's next clock read: a frame already in the pty when the mute
                // lands restarts the silence window once. 20 s stays under util::kLinkStalenessBound.
                for (int i = 0; i < 2; ++i) {
                    s.clock.advance(std::chrono::milliseconds(10001));
                    (void)s.clock.wait_for_now_calls(s.clock.now_calls() + 1, std::chrono::milliseconds(2000));
                }
            },
            [](Hosted& hosted) { t3_fake<WandererT3Hold>(hosted).streamer.set_muted(false); },
            "FakeSerialStreamer::set_muted(true) mid-move, task clock +20 s",
            [](bool) {
                return host_over(std::make_shared<WandererT3Hold>(),
                                 [](WandererT3Hold& s) -> std::unique_ptr<AlpacaDriver> {
                                     return alpacacore::vendor::wandererastro::create_wandererastro_filterwheel(
                                         0, s.streamer.slave_path(), 19200, s.clock);
                                 });
            }};
}
#endif

[[maybe_unused]] void t3_start_filterwheel_move(Hosted& hosted) {
    auto& f = dynamic_cast<alpacacore::FilterWheelDriver&>(*hosted.driver);
    f.set_position(f.get_position() == 1 ? 2 : 1);
}

// The SDK cameras expose for a minute, long enough that no driver deadline ends the exposure inside the budget.
constexpr double kTier3SdkExposureSeconds = 60.0;
// The Player One fake caps an exposure at 10 s. Its driver is on the task clock: the watchdog (9 s + 15 s grace)
// lands after the 18 s t3_on_clock steps at most before recover.
constexpr double kTier3CappedExposureSeconds = 9.0;

#ifdef ALPACACORE_ENABLE_ZWO
// An AM mount: ":MS#" is accepted and the mount then reports itself slewing (no 'N' in ":GU#") for good; the setters
// the goto sends first (site, time, target) are accepted. Falsified by: the slew status clearing Slewing when the
// ":GU#" poll goes unanswered.
Tier3Recipe tier3_recipe_zwo_telescope() {
    auto started = std::make_shared<std::atomic<bool>>(false);
    auto responder = [started](const std::string& chunk) -> std::string {
        const auto has = [&chunk](const char* command) { return chunk.find(command) != std::string::npos; };
        if (has(":GVP#")) return "AM5#";
        if (has(":GMA#")) return "012345ABCDEF#";
        if (has(":GU#")) return started->load() ? "G#" : "NG#";
        if (has(":GR#")) return "12:00:00#";
        if (has(":GD#")) return "+45*00:00#";
        if (has(":GMGE#")) return "-33*51:24&-151*12:55#";
        if (has(":GMTI#")) return "01/02/2026&03:04:05&+00:00#";
        if (has(":MS#")) {
            started->store(true);
            return "0#";
        }
        if (has(":Q#")) return "";
        return (has(":GAT") || has(":Sr") || has(":Sd") || has(":SMGE") || has(":SMTI")) ? "1#" : "0#";
    };
    return tier3_mount_server_recipe("zwo_telescope", "zwo", started, responder, zwo_telescope_over);
}

// The exposure never completes (auto_complete_exposure off) until the status reads Failed. The driver retries a failed
// exposure twice, 250 ms apart on the task clock, before it latches the failure. Falsified by: the exposure poll
// treating a Failed status as done, so ImageReady goes true on a failed exposure.
Tier3Recipe tier3_recipe_zwo_camera() {
    using Fake = alpacacore::test::FakeZWOSDK;
    auto clock = std::make_shared<alpacacore::test::FakeTaskClock>();
    return t3_on_clock(
        {[](Hosted& hosted) {
             t3_fake<Fake>(hosted).auto_complete_exposure = false;
             t3_start_exposure(hosted, kTier3SdkExposureSeconds);
         },
         [](Hosted& hosted) {
             auto& fake = t3_fake<Fake>(hosted);
             fake.fail_every_exposure = true;
             fake.set_exposure_status(Fake::ExposureStatus::Failed);
         },
         [](Hosted& hosted) {
             auto& fake = t3_fake<Fake>(hosted);
             fake.fail_every_exposure = false;
             fake.set_exposure_status(Fake::ExposureStatus::Working);
         },
         "FakeZWOSDK::fail_every_exposure = true mid-exposure",
         [clock](bool) {
             return host_over(std::make_shared<Fake>(), [&](Fake& fake) -> std::unique_ptr<AlpacaDriver> {
                 return alpacacore::vendor::zwo::create_zwo_camera(0, fake.camera.camera_id, fake, *clock);
             });
         }},
        DeviceType::Camera, clock);
}

// The wheel reports -1 (moving) for good; then every Position read throws. Falsified by: Position answering the
// requested slot when the read fails.
Tier3Recipe tier3_recipe_zwo_filterwheel() {
    using Fake = alpacacore::test::FakeZWOEFWSDK;
    return {[](Hosted& hosted) {
                t3_fake<Fake>(hosted).moving_reads = 1 << 30;
                t3_start_filterwheel_move(hosted);
            },
            [](Hosted& hosted) { t3_fake<Fake>(hosted).fail_reads = true; },
            [](Hosted& hosted) { t3_fake<Fake>(hosted).fail_reads = false; },
            "FakeZWOEFWSDK::fail_reads = true mid-move"};
}

// The unit keeps reporting "moving"; then every status read throws. Falsified by: IsMoving answering false when the
// read fails.
Tier3Recipe tier3_recipe_zwo_focuser() {
    using Fake = alpacacore::test::FakeZWOEAFSDK;
    return {[](Hosted& hosted) {
                t3_fake<Fake>(hosted).moving = true;
                t3_start_focuser_move(hosted, 500);
            },
            [](Hosted& hosted) { t3_fake<Fake>(hosted).fail_reads = true; },
            [](Hosted& hosted) { t3_fake<Fake>(hosted).fail_reads = false; },
            "FakeZWOEAFSDK::fail_reads = true mid-move"};
}

// The rotator keeps reporting "moving"; then every status read throws. Falsified by: IsMoving answering false when
// the read fails.
Tier3Recipe tier3_recipe_zwo_rotator() {
    using Fake = alpacacore::test::FakeZWOCAASDK;
    return {[](Hosted& hosted) {
                t3_fake<Fake>(hosted).moving = true;
                dynamic_cast<alpacacore::RotatorDriver&>(*hosted.driver).move_absolute(90.0);
            },
            [](Hosted& hosted) { t3_fake<Fake>(hosted).fail_reads = true; },
            [](Hosted& hosted) { t3_fake<Fake>(hosted).fail_reads = false; },
            "FakeZWOCAASDK::fail_reads = true mid-move"};
}
#endif

#ifdef ALPACACORE_ENABLE_QHY
// The frame call is held (before_call on get_mem_length, the worker's first call after the trigger) until `fail`, which
// makes that call throw: the worker reports no frame buffer and the exposure fails. The hold is bounded, so a case that
// never reaches `fail` still lets the worker end. The fake answers read_directly, so the worker skips its 1 s settle
// sleep on the task clock and the raise waits on no driver timer. Falsified by: the exposure worker treating the failed
// buffer query as a finished frame.
Tier3Recipe tier3_recipe_qhy_camera() {
    auto gate = std::make_shared<std::atomic<bool>>(false);
    return {[](Hosted& hosted) { t3_start_exposure(hosted, kTier3SdkExposureSeconds); },
            [gate](Hosted& hosted) {
                t3_fake<QhySdkHold>(hosted).fake.fail_from("get_mem_length");
                gate->store(true);
            },
            [](Hosted& hosted) { t3_fake<QhySdkHold>(hosted).fake.clear_fail_from(); },
            "FakeQHYSDK::fail_from(get_mem_length) mid-exposure",
            [gate](bool) {
                auto sdk_hold = std::make_shared<QhySdkHold>();
                sdk_hold->fake.read_directly = true;
                sdk_hold->fake.before_call = [gate](const std::string& name) {
                    if (name != "get_mem_length") return;
                    const auto bound = Clock::now() + std::chrono::seconds(30);
                    while (!gate->load() && Clock::now() < bound)
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                };
                return host_over(sdk_hold, [](QhySdkHold& s) -> std::unique_ptr<AlpacaDriver> {
                    return alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", s.sdk);
                });
            }};
}

// The CFW3 takes 8 s for the move (its reply would land then) and the driver waits 12 s for it; `fail` mutes the wheel
// at 1 s, so the reply never comes. Unmuted the move completes at 8 s, so a fault that does nothing ends the case with
// the move done. Falsified by: the wheel's move wait reporting the requested slot when the reply never arrives.
Tier3Recipe tier3_recipe_qhy_filterwheel() {
    using Fake = alpacacore::test::FakeQhyCfw3;
    return {[](Hosted& hosted) { t3_start_filterwheel_move(hosted); },
            [](Hosted& hosted) { t3_fake<Fake>(hosted).set_muted(true); },
            [](Hosted& hosted) { t3_fake<Fake>(hosted).set_muted(false); }, "FakeQhyCfw3::set_muted(true) mid-move",
            [](bool) {
                auto wheel = std::make_shared<Fake>();
                wheel->set_travel_ms(8000);
                return host_over(wheel, [](const Fake& f) -> std::unique_ptr<AlpacaDriver> {
                    alpacacore::vendor::qhy::Cfw3Settings settings;
                    settings.boot_timeout_ms = 150;
                    settings.reply_timeout_ms = 300;
                    settings.move_timeout_ms = 12000;
                    return alpacacore::vendor::qhy::create_qhy_cfw3_filterwheel(0, f.slave_path(), settings);
                });
            }};
}

// The Q-Focuser advances one step per position poll, so a 500-step move never ends; then the firmware goes silent.
// Falsified by: IsMoving answering false when the position poll times out.
Tier3Recipe tier3_recipe_qhy_focuser() {
    using Fake = alpacacore::test::FakeQhyQFocuser;
    auto clock = std::make_shared<alpacacore::test::FakeTaskClock>();
    return t3_on_clock(
        {[](Hosted& hosted) {
             t3_fake<Fake>(hosted).set_steps_per_poll(1);
             t3_start_focuser_move(hosted, 500);
         },
         [](Hosted& hosted) { t3_fake<Fake>(hosted).set_muted(true); },
         [](Hosted& hosted) { t3_fake<Fake>(hosted).set_muted(false); }, "FakeQhyQFocuser::set_muted(true) mid-move",
         [clock](bool) {
             return host_over(std::make_shared<Fake>(), [&](const Fake& f) -> std::unique_ptr<AlpacaDriver> {
                 return alpacacore::vendor::qhy::create_qhy_focuser(0, f.slave_path(), {}, *clock);
             });
         }},
        DeviceType::Focuser, clock);
}
#endif

#if defined(ALPACACORE_ENABLE_TOUPTEK) || defined(ALPACACORE_ENABLE_ALTAIR)
// The frame wait is armed to block (hold_next_wait_image, which the stream restart inside StartExposure does not
// spend); `fail` releases it with no frame delivered, so the camera reports no frame. The failed exposure keeps its
// error, so there is nothing to undo. Falsified by: the exposure thread treating a wait that returns no frame as a
// finished exposure.
Tier3Recipe tier3_recipe_toup_family_camera() {
    return {[](Hosted& hosted) {
                t3_fake<ToupTekSdkHold>(hosted).fake.hold_next_wait_image();
                t3_start_exposure(hosted, kTier3SdkExposureSeconds);
            },
            [](Hosted& hosted) { t3_fake<ToupTekSdkHold>(hosted).fake.release_wait_image(); }, [](Hosted&) {},
            "FakeToupTekSDK::release_wait_image() with no frame mid-exposure"};
}
#endif

#ifdef ALPACACORE_ENABLE_TOUPTEK
Tier3Recipe tier3_recipe_touptek_camera() { return tier3_recipe_toup_family_camera(); }
#endif

#ifdef ALPACACORE_ENABLE_ALTAIR
Tier3Recipe tier3_recipe_altair_camera() { return tier3_recipe_toup_family_camera(); }
#endif

#ifdef ALPACACORE_ENABLE_SVBONY
// Every video-data read is held for up to 20 s (the exposure is running); `fail` makes the held read throw. The
// failed exposure keeps its error, so there is nothing to undo. The fake allows a 60 s exposure, so with no fault
// the driver gives up at its own deadline (exposure + 10 s), well after the recover leg ends. Falsified by: the
// exposure worker swallowing a failed video-data read and ending the exposure as a frame.
Tier3Recipe tier3_recipe_svbony_camera() {
    using Fake = alpacacore::test::FakeSVBSDK;
    return {[](Hosted& hosted) {
                auto& fake = t3_fake<Fake>(hosted);
                fake.set_max_read_wait(std::chrono::seconds(20));
                fake.block_video_data();
                t3_start_exposure(hosted, kTier3SdkExposureSeconds);
            },
            [](Hosted& hosted) {
                auto& fake = t3_fake<Fake>(hosted);
                fake.fail_next_held_read();
                fake.release_video_data();
            },
            [](Hosted&) {}, "FakeSVBSDK::fail_next_held_read() mid-exposure",
            [](bool) {
                auto sdk = std::make_shared<Fake>();
                sdk->set_exposure_max(std::chrono::seconds(static_cast<int>(kTier3SdkExposureSeconds)));
                return host_over(sdk, [](Fake& fake) -> std::unique_ptr<AlpacaDriver> {
                    return alpacacore::vendor::svbony::create_svbony_camera(0, 0, fake);
                });
            }};
}
#endif

#ifdef ALPACACORE_ENABLE_PLAYERONE
// The readiness poll reports not ready (set_image_ready(false)); then it throws. The poll waits 50 ms on the task
// clock between checks. Falsified by: the exposure worker ending the exposure when the readiness check throws.
Tier3Recipe tier3_recipe_playerone_camera() {
    using Fake = alpacacore::test::FakePlayerOneSDK;
    auto clock = std::make_shared<alpacacore::test::FakeTaskClock>();
    return t3_on_clock({[](Hosted& hosted) {
                            t3_fake<Fake>(hosted).set_image_ready(false);
                            t3_start_exposure(hosted, kTier3CappedExposureSeconds);
                        },
                        [](Hosted& hosted) { t3_fake<Fake>(hosted).set_readiness_error(true); },
                        [](Hosted& hosted) { t3_fake<Fake>(hosted).set_readiness_error(false); },
                        "FakePlayerOneSDK::set_readiness_error(true) mid-exposure",
                        [clock](bool) {
                            return host_over(std::make_shared<Fake>(),
                                             [&](Fake& fake) -> std::unique_ptr<AlpacaDriver> {
                                                 return alpacacore::vendor::playerone::create_playerone_camera(
                                                     0, 0, fake, std::chrono::seconds(15), *clock);
                                             });
                        }},
                       DeviceType::Camera, clock);
}

// The wheel reports -1 (moving) for good; then every Position read throws. Falsified by: Position answering the
// requested slot when the read fails.
Tier3Recipe tier3_recipe_playerone_filterwheel() {
    using Fake = alpacacore::test::FakePlayerOnePWSDK;
    return {[](Hosted& hosted) {
                t3_fake<Fake>(hosted).set_moving_reads(1 << 30);
                t3_start_filterwheel_move(hosted);
            },
            [](Hosted& hosted) { t3_fake<Fake>(hosted).set_fail_reads(true); },
            [](Hosted& hosted) { t3_fake<Fake>(hosted).set_fail_reads(false); },
            "FakePlayerOnePWSDK::set_fail_reads(true) mid-move"};
}
#endif

#ifdef ALPACACORE_ENABLE_GPHOTO
// A bulb exposure (60 s, past the longest native speed) pumps events in 100 ms slices; `fail` makes the event pump
// throw, which closes the shutter and fails the exposure. Falsified by: the exposure worker publishing a frame after
// the hold throws.
Tier3Recipe tier3_recipe_gphoto_camera() {
    return {[](Hosted& hosted) { t3_start_exposure(hosted, kTier3SdkExposureSeconds); },
            [](Hosted& hosted) { t3_fake<GPhotoSdkHold>(hosted).fake.fail_from("drain_events"); },
            [](Hosted& hosted) { t3_fake<GPhotoSdkHold>(hosted).fake.clear_fail_from(); },
            "FakeGPhotoSDK::fail_from(drain_events) mid-exposure"};
}
#endif

// One X(id, status) per tier-3 recipe, under the vendor's guard. status OK: the case must pass. status DEFECT: the
// driver violates the contract today; the case carries [!shouldfail] and its Fix removes the tag.
// clang-format off
#ifdef ALPACACORE_ENABLE_CELESTRON
#define CT3_CELESTRON(X) X(celestron_telescope, OK) X(celestron_cgx_telescope, OK) X(celestron_focuser, OK)
#else
#define CT3_CELESTRON(X)
#endif
#ifdef ALPACACORE_ENABLE_SKYWATCHER
#define CT3_SKYWATCHER(X) X(skywatcher_telescope, OK) X(skywatcher_telescope_serial, OK)
#else
#define CT3_SKYWATCHER(X)
#endif
#ifdef ALPACACORE_ENABLE_IOPTRON
#define CT3_IOPTRON(X) X(ioptron_telescope, OK) X(ioptron_focuser, OK) X(iguider_camera, OK)
#else
#define CT3_IOPTRON(X)
#endif
#ifdef ALPACACORE_ENABLE_SYNSCAN
#define CT3_SYNSCAN(X) X(synscan_telescope, OK)
#else
#define CT3_SYNSCAN(X)
#endif
#ifdef ALPACACORE_ENABLE_ONSTEP
#define CT3_ONSTEP(X) X(onstep_telescope, OK)
#else
#define CT3_ONSTEP(X)
#endif
#ifdef ALPACACORE_ENABLE_ASTROASIS
#define CT3_ASTROASIS(X) X(astroasis_focuser, OK)
#else
#define CT3_ASTROASIS(X)
#endif
#ifdef ALPACACORE_ENABLE_GEMINI
#define CT3_GEMINI(X) X(gemini_focuser, OK)
#else
#define CT3_GEMINI(X)
#endif
#ifdef ALPACACORE_ENABLE_WANDERERASTRO
#define CT3_WANDERERASTRO(X) X(wandererastro_filterwheel, OK)
#else
#define CT3_WANDERERASTRO(X)
#endif
#ifdef ALPACACORE_ENABLE_ZWO
#define CT3_ZWO(X) X(zwo_telescope, OK) X(zwo_camera, OK) X(zwo_filterwheel, OK) X(zwo_focuser, OK) X(zwo_rotator, OK)
#else
#define CT3_ZWO(X)
#endif
#ifdef ALPACACORE_ENABLE_QHY
#define CT3_QHY(X) X(qhy_camera, OK) X(qhy_filterwheel, OK) X(qhy_focuser, OK)
#else
#define CT3_QHY(X)
#endif
#ifdef ALPACACORE_ENABLE_TOUPTEK
#define CT3_TOUPTEK(X) X(touptek_camera, OK)
#else
#define CT3_TOUPTEK(X)
#endif
#ifdef ALPACACORE_ENABLE_ALTAIR
#define CT3_ALTAIR(X) X(altair_camera, OK)
#else
#define CT3_ALTAIR(X)
#endif
#ifdef ALPACACORE_ENABLE_SVBONY
#define CT3_SVBONY(X) X(svbony_camera, OK)
#else
#define CT3_SVBONY(X)
#endif
#ifdef ALPACACORE_ENABLE_PLAYERONE
#define CT3_PLAYERONE(X) X(playerone_camera, OK) X(playerone_filterwheel, OK)
#else
#define CT3_PLAYERONE(X)
#endif
#ifdef ALPACACORE_ENABLE_GPHOTO
#define CT3_GPHOTO(X) X(gphoto_camera, OK)
#else
#define CT3_GPHOTO(X)
#endif
#define CONTRACT_SWEEP_TIER3_HOSTS(X) \
    CT3_CELESTRON(X) \
    CT3_SYNSCAN(X) \
    CT3_ONSTEP(X) \
    CT3_SKYWATCHER(X) \
    CT3_IOPTRON(X) \
    CT3_ASTROASIS(X) \
    CT3_GEMINI(X) \
    CT3_WANDERERASTRO(X) \
    CT3_ZWO(X) \
    CT3_QHY(X) \
    CT3_TOUPTEK(X) \
    CT3_ALTAIR(X) \
    CT3_SVBONY(X) \
    CT3_PLAYERONE(X) \
    CT3_GPHOTO(X)
// clang-format on

#endif  // !_WIN32

}  // namespace

// One TEST_CASE per (entry, case). The name carries the registry id, so a
// failure and `ctest -R` both name the driver.
#define CS_CASE(id, casename, body)                                                   \
    TEST_CASE("Contract sweep - " #id " - " casename, "[contract][tier1][" #id "]") { \
        body(alpacacore::test::contract::contract_entry_##id());                      \
    }

#define CS_T1_NOTCONN(id) CS_CASE(id, "operations throw NotConnected", case_operations_throw_not_connected)
#define CS_T1_STATE(id) CS_CASE(id, "disconnected DeviceState is empty", case_device_state_empty)
#define CS_T1_IFACE(id) CS_CASE(id, "InterfaceVersion is Platform 7", case_interface_version)
#define CS_T1_DRVVER(id) CS_CASE(id, "DriverVersion is kVersion", case_driver_version)
#define CS_T1_UID(id) CS_CASE(id, "UniqueID distinct per device number", case_unique_id)
#define CS_T1_ACTION(id) CS_CASE(id, "unsupported action contract", case_unsupported_action)
#define CS_T1_ERRVOCAB(id) CS_CASE(id, "error code vocabulary", case_error_vocabulary)
// A case with a subject test is registered only for an entry that passes it, so no case runs with nothing to
// assert; the guard case below pins each subject test to the probe lists.
#define CS_CASE_IF(id, casename, body, subject)                                                              \
    void cs_t1_##body##_##id() { body(alpacacore::test::contract::contract_entry_##id()); }                  \
    const auto cs_t1_reg_##body##_##id =                                                                     \
        subject(alpacacore::test::contract::contract_entry_##id())                                           \
            ? std::make_unique<Catch::AutoReg>(                                                              \
                  Catch::makeTestInvoker(&cs_t1_##body##_##id), CATCH_INTERNAL_LINEINFO, Catch::StringRef(), \
                  Catch::NameAndTags{"Contract sweep - " #id " - " casename, "[contract][tier1][" #id "]"})  \
            : nullptr;
#define CS_T1_INVALID(id) \
    CS_CASE_IF(id, "InvalidValue precedes NotConnected", case_invalid_before_not_connected, has_invalid_subject)
#define CS_T1_CAN(id) CS_CASE_IF(id, "Can getters", case_can_getters, has_can_subject)

CONTRACT_SWEEP_ENTRIES(CS_T1_INVALID)
CONTRACT_SWEEP_ENTRIES(CS_T1_NOTCONN)
CONTRACT_SWEEP_ENTRIES(CS_T1_STATE)
CONTRACT_SWEEP_ENTRIES(CS_T1_IFACE)
CONTRACT_SWEEP_ENTRIES(CS_T1_DRVVER)
CONTRACT_SWEEP_ENTRIES(CS_T1_UID)
CONTRACT_SWEEP_ENTRIES(CS_T1_ACTION)
CONTRACT_SWEEP_ENTRIES(CS_T1_ERRVOCAB)
CONTRACT_SWEEP_ENTRIES(CS_T1_CAN)

// Non-vacuity: a tier-1 case is registered exactly for the entries that pass its subject test, and each such
// entry has a non-empty probe list (an unregistered entry states why in no_invalid_probe_reason).
void check_tier1_cases_have_a_subject() {
    std::set<std::string> registered;
    for (const auto& t : Catch::getRegistryHub().getTestCaseRegistry().getAllTests()) {
        registered.insert(t.getTestCaseInfo().name);
    }
    // Case names use the macro id, which differs from ContractEntry::id for some entries (celestron_cgx_telescope).
    [[maybe_unused]] const auto check = [&](const char* macro_id, const ContractEntry& e) {
        auto d = e.make(0);
        REQUIRE(d != nullptr);
        const std::string stem = std::string("Contract sweep - ") + macro_id + " - ";
        INFO(macro_id);
        CHECK(!invalid_value_probes(*d, e).empty() == has_invalid_subject(e));
        CHECK((registered.count(stem + "InvalidValue precedes NotConnected") == 1) == has_invalid_subject(e));
        CHECK(!can_getter_probes(*d, e).empty() == has_can_subject(e));
        CHECK((registered.count(stem + "Can getters") == 1) == has_can_subject(e));
    };
#define CS_T1_GUARD(id) check(#id, alpacacore::test::contract::contract_entry_##id());
    CONTRACT_SWEEP_ENTRIES(CS_T1_GUARD)
#undef CS_T1_GUARD
}

TEST_CASE("Contract sweep - filter wheels - names and offsets while disconnected", "[contract][tier1][filterwheel]") {
    int seen = 0;
    for (const auto& e : alpacacore::test::contract::contract_entries()) {
        if (e.type != DeviceType::FilterWheel) continue;
        ++seen;
        case_filterwheel_names_offsets_disconnected(e);
    }
    INFO("filter wheel entries in this build: " << seen);
}

#ifndef _WIN32
#define CS2_CASE(id, casename, body) \
    TEST_CASE("Contract sweep tier 2 - " #id " - " casename, "[contract][tier2][" #id "]") { body(tier2_host_##id()); }
#define CS2_CONNECTING(id, kind) CS2_CASE(id, "Connecting semantics", case_t2_connecting)
#define CS2_HOOKS(id, kind) CS2_CASE(id, "firmware and SDK hooks do not block", case_t2_firmware_sdk_hooks)
#define CS2_INITIATORS(id, kind) CS2_INITIATORS_##kind(id)
#define CS2_INITIATORS_TEL(id) CS2_CASE(id, "Park and MoveAxis return at once", case_t2_async_initiators)
#define CS2_INITIATORS_PRB(id)
#define CS2_INITIATORS_NPR(id)
#define CS2_INITIATORS_CAM(id)
#define CS2_STATE(id, kind) CS2_CASE(id, "connected DeviceState has TimeStamp", case_t2_device_state_connected)
#define CS2_STATE_TYPES(id, kind) \
    CS2_CASE(id, "DeviceState value types match the ASCOM members", case_t2_device_state_value_types)
// Target flags: telescopes only. InvalidValue: every kind except NPR and CAM.
#define CS2_TARGETS(id, kind) CS2_TARGETS_##kind(id)
#define CS2_TARGETS_TEL(id) CS2_CASE(id, "target flags until set", case_t2_target_flags)
#define CS2_TARGETS_PRB(id)
#define CS2_TARGETS_NPR(id)
#define CS2_TARGETS_CAM(id)
#define CS2_INVALID(id, kind) CS2_INVALID_##kind(id)
#define CS2_INVALID_TEL(id) CS2_CASE(id, "InvalidValue wins while connected", case_t2_invalid_value_connected)
#define CS2_INVALID_PRB(id) CS2_INVALID_TEL(id)
#define CS2_INVALID_NPR(id)
#define CS2_INVALID_CAM(id)
// ImageArray before an exposure: cameras only.
#define CS2_IMAGE(id, kind) CS2_IMAGE_##kind(id)
#define CS2_IMAGE_CAM(id) \
    CS2_CASE(id, "ImageArray before any exposure is asked too early", case_t2_image_array_asked_too_early)
#define CS2_IMAGE_TEL(id)
#define CS2_IMAGE_PRB(id)
#define CS2_IMAGE_NPR(id)
// LastExposure* before any image: cameras only. Every camera id is named here, so a new camera host fails to
// compile until it says whether it takes the case. The #1397 hold still applies to ToupTek/Altair cooler work.
#define CS2_LASTEXP(id, kind) CS2_LASTEXP_##kind(id)
#define CS2_LASTEXP_TEL(id)
#define CS2_LASTEXP_PRB(id)
#define CS2_LASTEXP_NPR(id)
#define CS2_LASTEXP_CAM(id) CS2_LASTEXP_##id(id)
#define CS2_LASTEXP_RUN(id) \
    CS2_CASE(id, "LastExposure before any image is InvalidOperation", case_t2_last_exposure_before_image)
#define CS2_LASTEXP_zwo_camera(id) CS2_LASTEXP_RUN(id)
#define CS2_LASTEXP_qhy_camera(id) CS2_LASTEXP_RUN(id)
#define CS2_LASTEXP_iguider_camera(id) CS2_LASTEXP_RUN(id)
#define CS2_LASTEXP_svbony_camera(id) CS2_LASTEXP_RUN(id)
#define CS2_LASTEXP_playerone_camera(id) CS2_LASTEXP_RUN(id)
#define CS2_LASTEXP_gphoto_camera(id) CS2_LASTEXP_RUN(id)
#define CS2_LASTEXP_touptek_camera(id) CS2_LASTEXP_RUN(id)
#define CS2_LASTEXP_altair_camera(id) CS2_LASTEXP_RUN(id)

CONTRACT_SWEEP_TIER2_HOSTS(CS2_CONNECTING)
CONTRACT_SWEEP_TIER2_HOSTS(CS2_TARGETS)
CONTRACT_SWEEP_TIER2_HOSTS(CS2_INVALID)
CONTRACT_SWEEP_TIER2_HOSTS(CS2_STATE)
CONTRACT_SWEEP_TIER2_HOSTS(CS2_STATE_TYPES)
CONTRACT_SWEEP_TIER2_HOSTS(CS2_IMAGE)
CONTRACT_SWEEP_TIER2_HOSTS(CS2_LASTEXP)
CONTRACT_SWEEP_TIER2_HOSTS(CS2_HOOKS)
CONTRACT_SWEEP_TIER2_HOSTS(CS2_INITIATORS)
#endif

#ifndef _WIN32
#define CT3_CASE_TAGS_OK "[contract][tier3]"
#define CT3_CASE_TAGS_DEFECT "[contract][tier3][!shouldfail]"
// Generated from CONTRACT_SWEEP_TIER3_HOSTS rows, like the tier-1 and tier-2 cases; each recipe names its mutation.
#define CT3_NAME(id) "Contract sweep tier 3 - " #id " - failure after start is never a clean completion"
#define CT3_CASE(id, status)                                                 \
    TEST_CASE(CT3_NAME(id), CT3_CASE_TAGS_##status "[" #id "]") {            \
        case_t3_failure_after_start(tier2_host_##id(), tier3_recipe_##id()); \
    }
CONTRACT_SWEEP_TIER3_HOSTS(CT3_CASE)

// Non-vacuity guard for tier 3: every tier-2 host of a type with an asynchronous completion property (camera,
// telescope, focuser, rotator, filter wheel) has a tier-3 recipe with a start, a fault, its undo and a description, and
// no other host has one. A host of those types with no recipe is a fake no case ever fails mid-operation. Run from the
// tier-2 roster guard below, as check_tier1_cases_have_a_subject() is from the tier-1 one.
void check_tier3_hosts_have_a_recipe() {
    std::map<std::string, Tier3Recipe> recipes;
#define CT3_PUSH(id, status) CHECK(recipes.emplace(#id, tier3_recipe_##id()).second);
    CONTRACT_SWEEP_TIER3_HOSTS(CT3_PUSH)
#undef CT3_PUSH
    std::set<std::string> hosted_ids;
#define CT3_HOST(id, kind)                                         \
    {                                                              \
        const Tier2Host h = tier2_host_##id();                     \
        hosted_ids.insert(#id);                                    \
        INFO("tier-2 host " #id);                                  \
        CHECK((recipes.count(#id) == 1) == is_tier3_type(h.type)); \
    }
    CONTRACT_SWEEP_TIER2_HOSTS(CT3_HOST)
#undef CT3_HOST
    for (const auto& [id, r] : recipes) {
        INFO("tier-3 recipe " << id);
        CHECK(hosted_ids.count(id) == 1);
        CHECK(static_cast<bool>(r.start));
        CHECK(static_cast<bool>(r.fail));
        CHECK(static_cast<bool>(r.recover));
        CHECK((r.fault != nullptr && std::string(r.fault).size() > 0));
    }
}
#endif

#ifndef _WIN32
// Non-vacuity guard for tier 2. The roster stays a literal three-field array (docs-drift check 13 parses
// it), so the connect recipes live in CONTRACT_SWEEP_TIER2_HOSTS; this pins the two to each other in both
// directions. A roster row without a host would be a fake no case ever connects to, and a host without a
// row would be a fake that check 13 never sees.
TEST_CASE("Contract sweep tier 2 - hosts match kFakeConnectableRoster", "[contract][tier2][contract-sweep-guard]") {
    check_tier3_hosts_have_a_recipe();
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
    CS2_ENABLED("iguider")  // the iGuider camera's own catalog vendor, built by the iOptron option
#endif
#ifdef ALPACACORE_ENABLE_ASTROASIS
    CS2_ENABLED("astroasis")
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
#ifdef ALPACACORE_ENABLE_ALTAIR
    CS2_ENABLED("altair")
#endif
#ifdef ALPACACORE_ENABLE_SVBONY
    CS2_ENABLED("svbony")
#endif
#ifdef ALPACACORE_ENABLE_PLAYERONE
    CS2_ENABLED("playerone")
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
#ifndef ALPACACORE_TOUPTEK_STELLAVITA
        // The StellaVita switch is compiled only with libgpiod, so a ToupTek build without it has no such host.
        if (std::string(row.fake_header) == "fake_touptek_powerbox_sdk.h") continue;
#endif
#ifndef ALPACACORE_IOPTRON_POWERBOX
        // Same for the iMate PowerBox.
        if (std::string(row.fake_header) == "fake_ioptron_gpio.h") continue;
#endif
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
        CHECK((kind == "CAM") == (h.type == DeviceType::Camera));
        const bool no_probe_kind = kind == "NPR" || kind == "CAM";
        const bool states_no_probe =
            std::string(alpacacore::test::contract::invalid_probe_reason_for(h.type)).size() > 0;
        CHECK(states_no_probe == no_probe_kind);
        if (h.connectable) {
            Hosted probe_host = h.connectable(false);
            REQUIRE(probe_host.driver != nullptr);
            CHECK(invalid_value_probes(*probe_host.driver, h.type).empty() == no_probe_kind);
        }
        CHECK((h.can_hold_connect ||
               (h.connecting_unobservable != nullptr && std::string(h.connecting_unobservable).size() > 0)));
        CHECK(
            ((h.park_unavailable == nullptr && h.move_axis_unavailable == nullptr) || h.type == DeviceType::Telescope));
        CHECK((h.move_axis_unavailable == nullptr || std::string(h.move_axis_unavailable).size() > 0));
        CHECK((h.park_unavailable == nullptr || std::string(h.park_unavailable).size() > 0));
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

// Catalog pairs (fork #71): the built-in catalog itself says which (vendor, device type) pairs this
// build constructs, so scripts/check_contract_sweep.py no longer parses *_catalog.cpp for them. A
// descriptor is `available` when its vendor's factory is built in, which is also when that vendor's
// registry entries compile.
namespace {

using CatalogPair = std::pair<std::string, std::string>;  // (vendor, router deviceType string)

std::vector<alpacacore::catalog::DescriptorView> builtin_catalog_views(alpacacore::catalog::DeviceCatalog& catalog) {
    alpacacore::catalog::register_builtin_schemas(catalog);
    alpacacore::catalog::register_builtin_factories(catalog);
    return catalog.describe();
}

CatalogPair catalog_pair(const alpacacore::catalog::DescriptorView& view) {
    std::string type = alpacacore::device_type_to_string(view.key.type);
    std::transform(type.begin(), type.end(), type.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return {view.key.vendor, type};
}

}  // namespace

TEST_CASE("Contract sweep - every available catalog descriptor has a registry entry",
          "[contract][contract-sweep-guard]") {
    alpacacore::catalog::DeviceCatalog catalog;
    const auto views = builtin_catalog_views(catalog);
    std::set<CatalogPair> registered;
    for (const auto& e : alpacacore::test::contract::contract_entries()) registered.insert({e.vendor, e.device_type});
    std::set<std::string> described_vendors;
    for (const auto& v : views) described_vendors.insert(v.key.vendor);

    std::size_t compared = 0;
    for (const auto& v : views) {
        if (!v.available) continue;
        ++compared;
        const CatalogPair pair = catalog_pair(v);
        INFO("the catalog constructs " << pair.first << "/" << pair.second << " but contract_sweep.h has no X("
                                       << pair.first << "_" << pair.second << ") entry");
        CHECK(registered.count(pair) == 1);
    }
    // Not vacuous: a build whose registry holds a catalog vendor's entries has that vendor's factory, so
    // at least one view was compared (zero means register_builtin_factories() compiled empty).
    const bool catalog_vendor_built = std::any_of(registered.begin(), registered.end(), [&](const CatalogPair& p) {
        return described_vendors.count(p.first) == 1;
    });
    if (catalog_vendor_built) CHECK(compared > 0);
}

// The reverse direction, which the gate's ORPHAN ENTRY rule no longer covers for catalog vendors: a
// registry entry whose vendor has descriptors or factories names one of them. A vendor counts by its
// factories too, so dropping its whole schema registration leaves its entries checked, not skipped.
// ZWO's camera was the last pair of a catalog vendor still built by a router arm (open-astro#999), so
// every entry of a catalog vendor is expected to have a descriptor.
TEST_CASE("Contract sweep - every registry entry of a catalog vendor has a descriptor",
          "[contract][contract-sweep-guard]") {
    alpacacore::catalog::DeviceCatalog catalog;
    const auto views = builtin_catalog_views(catalog);
    std::set<CatalogPair> described;
    std::set<std::string> catalog_vendors;
    for (const auto& v : views) {
        described.insert(catalog_pair(v));
        catalog_vendors.insert(v.key.vendor);
    }
    for (const auto& k : catalog.factory_keys()) catalog_vendors.insert(k.vendor);
    for (const auto& e : alpacacore::test::contract::contract_entries()) {
        if (catalog_vendors.count(e.vendor) == 0) continue;
        INFO(e.id << " is in the registry but the catalog has no " << e.vendor << "/" << e.device_type
                  << " descriptor");
        CHECK(described.count({e.vendor, e.device_type}) == 1);
    }
}

// Non-vacuity guard. The vendor ALPACACORE_ENABLE_<V> macros are not inherited
// from the vendor targets: tests/CMakeLists.txt must define them for
// alpacacore_tests, or the registry compiles to nothing and every case above
// silently disappears. Each macro that IS defined must contribute an entry.
TEST_CASE("Contract sweep - registry is not vacuous", "[contract][contract-sweep-guard]") {
    check_tier1_cases_have_a_subject();
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

// Snapshot adopters (decision 0010 rule 6, issue #1443): "stale alone never refuses". For every driver whose
// Adoption row is `adopted`, the cases below connect over the driver's fake on a FakeTaskClock, let the
// connect's frame publish, hold the device so no newer frame comes, and move the task clock.
//  - Past the snapshot max_age (largest is 3 s, Celestron telescope) and under util::kLinkStalenessBound (30 s)
//    with no fault latched: Connected stays true, no link fault is reported, every snapshot-served getter
//    still returns the value it returned before (within kSnapshotValueMargin), and DeviceState still lists
//    the frame.
//  - Past the bound (decision 0009): every one of the same getters throws NotConnected, Connected reads false
//    and DeviceState is empty.
// How the frame is held differs by backend and each rig says where it comes from: a telescope with a poll
// pause seam pauses its poll (no failed poll, so no latch); a focuser fake is muted or made to answer garbage
// so the one poll the clock step wakes fails, which stays under every latch threshold.
// The loss case fails kSnapshotLatchPolls polls (a latch needs three where an adopter has one), then passes the
// bound: SynScan, OnStep, Celestron (telescope) and Bisque lose the link from the latched fault's age; Sky-Watcher, QHY
// and Astroasis also from the age of the frame.
// The roster is kSnapshotAdopterRoster (contract_sweep.h); rule 6 of scripts/check_contract_sweep.py holds it,
// and the hooks below, to the ADR table.
#ifndef _WIN32
namespace {

using alpacacore::test::FakeTaskClock;
namespace snap_chrono = std::chrono;

constexpr snap_chrono::seconds kSnapshotStaleStep{10};  // above every max_age (<= 3 s), far under the 30 s bound
// Loss (decision 0009) is a latched fault that has stood for util::kLinkStalenessBound, or (Sky-Watcher, QHY,
// Astroasis) a frame that old: the poll fails on each of kSnapshotLatchPolls one-second steps (the adopters with
// a latch threshold use three), then the clock passes the bound.
constexpr int kSnapshotLatchPolls = 4;
constexpr snap_chrono::seconds kSnapshotLostStep{31};
static_assert(kSnapshotStaleStep < alpacacore::util::kLinkStalenessBound);
static_assert(kSnapshotLostStep > alpacacore::util::kLinkStalenessBound);
constexpr snap_chrono::milliseconds kSnapshotBound{10000};

// One connected adopter with its fake and clock. Members are destroyed in reverse order: the driver first.
struct SnapshotRig {
    std::shared_ptr<void> fake;
    std::shared_ptr<FakeTaskClock> clock = std::make_shared<FakeTaskClock>();
    std::unique_ptr<AlpacaDriver> driver;
    // Keeps newer frames from arriving without a failed poll (pauses the poll) or by muting the fake; runs after
    // the baseline reads.
    std::function<void()> hold_frame;
    // The loss case: the device stops answering and the poll runs on (a paused poll never notices a loss).
    // Empty means hold_frame already does that.
    std::function<void()> lose_link;
    // Optional: one direct poll tick after the clock passed the bound, for backends whose loss is noticed there.
    std::function<void()> tick;
    // Getter names left out; the rig that fills it says why next to the assignment.
    std::vector<std::string> left_out;

    SnapshotRig() = default;
    SnapshotRig(const SnapshotRig&) = delete;
    SnapshotRig& operator=(const SnapshotRig&) = delete;
    ~SnapshotRig() {
        if (!driver) return;
        try {
            driver->set_connected(false);
        } catch (...) {
        }
    }

    // Moves the task clock and lets the woken poll body finish its tick.
    void pass(snap_chrono::seconds d) {
        clock->advance(d);
        (void)clock->wait_for_woken_settled(kSnapshotBound);
        (void)clock->wait_for_waiters(1, snap_chrono::milliseconds(300));  // parked again, unless the poll body ended
    }
    // The connect's poll body parks on the clock after Connect returns.
    void await_poll() { (void)clock->wait_for_waiters(1, snap_chrono::milliseconds(2000)); }
};

struct SnapshotGetter {
    std::string name;
    std::function<double(AlpacaDriver&)> read;
};

// Every served value as a number: a position, a flag, a pier side.
template <class T>
double snap_text(const T& v) {
    return static_cast<double>(v);
}

// A dead-reckoning mount (Sky-Watcher, decision 0010 Sky-Watcher adoption point 2) advances its position with the
// clock between two reads, so a value is compared within this margin; a refused getter throws, and a dropped or
// zeroed frame is far outside it.
constexpr double kSnapshotValueMargin = 0.01;

[[maybe_unused]] std::vector<SnapshotGetter> snapshot_telescope_getters(bool with_slewing) {
    using alpacacore::TelescopeDriver;
    std::vector<SnapshotGetter> g;
#define SNAP_TEL(label, call) \
    g.push_back({label, [](AlpacaDriver& d) { return snap_text(static_cast<TelescopeDriver&>(d).call); }});
    SNAP_TEL("RightAscension", get_right_ascension())
    SNAP_TEL("Declination", get_declination())
    SNAP_TEL("Altitude", get_altitude())
    SNAP_TEL("Azimuth", get_azimuth())
    SNAP_TEL("SideOfPier", get_side_of_pier())
    SNAP_TEL("Tracking", get_tracking())
    if (with_slewing) SNAP_TEL("Slewing", get_slewing())
#undef SNAP_TEL
    return g;
}

// The Celestron focus motor has no temperature sensor (its Temperature getter throws NotImplemented, not served
// from the frame), so its list leaves Temperature out.
[[maybe_unused]] std::vector<SnapshotGetter> snapshot_focuser_getters(bool with_temperature = true) {
    using alpacacore::FocuserDriver;
    std::vector<SnapshotGetter> g;
#define SNAP_FOC(label, call) \
    g.push_back({label, [](AlpacaDriver& d) { return snap_text(static_cast<FocuserDriver&>(d).call); }});
    SNAP_FOC("Position", get_position())
    SNAP_FOC("IsMoving", get_is_moving())
    if (with_temperature) SNAP_FOC("Temperature", get_temperature())
#undef SNAP_FOC
    return g;
}

bool snapshot_has_time_stamp(const AlpacaDriver& driver) {
    const auto state = driver.get_device_state();
    return std::any_of(state.begin(), state.end(), [](const auto& e) { return e.name == "TimeStamp"; });
}

#ifdef ALPACACORE_ENABLE_SKYWATCHER
// Poll pause seam of the Sky-Watcher driver; the connect recipe of test_skywatcher_snapshot.cpp.
std::unique_ptr<SnapshotRig> snapshot_rig_skywatcher_telescope() {
    namespace sw = alpacacore::vendor::skywatcher;
    auto rig_owner = std::make_unique<SnapshotRig>();
    SnapshotRig& rig = *rig_owner;
    auto mount = std::make_shared<alpacacore::test::FakeSkyWatcherMount>();
    sw::ConnectionInfo info;
    info.type = sw::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.udp_port = mount->port();
    info.response_timeout_ms = 250;
    rig.fake = mount;
    rig.driver = sw::create_skywatcher_telescope(0, info, -33.856784, 151.215297, 42.0, {}, {}, *rig.clock);
    rig.driver->set_connected(true);
    rig.await_poll();
    auto* driver = rig.driver.get();
    rig.hold_frame = [driver] {
        sw::set_status_poll_paused_for_testing(static_cast<alpacacore::TelescopeDriver&>(*driver), true);
    };
    rig.lose_link = [driver, mount] {
        mount->set_silent(true);
        sw::set_status_poll_paused_for_testing(static_cast<alpacacore::TelescopeDriver&>(*driver), false);
    };
    return rig_owner;
}
#endif

#ifdef ALPACACORE_ENABLE_BISQUE
// Recipe of test_bisque_telescope_snapshot.cpp (Rig): paused poll, one direct tick for the loss.
// Slewing is control-path state there (decision 0010, Bisque adoption), so it is not swept.
std::unique_ptr<SnapshotRig> snapshot_rig_bisque_telescope() {
    namespace bq = alpacacore::vendor::bisque;
    auto rig_owner = std::make_unique<SnapshotRig>();
    SnapshotRig& rig = *rig_owner;
    auto mount = std::make_shared<alpacacore::test::FakeBisqueMount>();
    auto server = std::make_shared<alpacacore::test::FakeMountServer>(mount->responder());
    REQUIRE(server->ok());
    bq::ConnectionInfo info;
    info.host = "127.0.0.1";
    info.tcp_port = server->port();
    info.response_timeout_ms = 300;
    rig.fake = std::make_shared<std::pair<decltype(mount), decltype(server)>>(mount, server);
    rig.driver = bq::create_bisque_telescope(0, info, *rig.clock);
    rig.driver->set_connected(true);
    rig.await_poll();
    auto* driver = rig.driver.get();
    rig.hold_frame = [driver] {
        bq::set_status_poll_paused_for_testing(static_cast<alpacacore::TelescopeDriver&>(*driver), true);
    };
    rig.lose_link = [driver, mount] {
        mount->mute = true;
        bq::set_status_poll_paused_for_testing(static_cast<alpacacore::TelescopeDriver&>(*driver), false);
    };
    rig.tick = [driver] { bq::poll_status_now_for_testing(static_cast<alpacacore::TelescopeDriver&>(*driver)); };
    rig.left_out = {"Slewing"};
    return rig_owner;
}
#endif

#ifdef ALPACACORE_ENABLE_SYNSCAN
struct SnapshotHandset {
    std::atomic<bool> mute{false};
};

// The handset answers of test_synscan_telescope_snapshot.cpp (reply()), an EQ mount idle on a fixed position.
std::string snapshot_synscan_reply(SnapshotHandset& st, const std::string& chunk) {
    if (chunk.empty()) return "0#";
    if (st.mute.load() && chunk[0] != 'K' && chunk[0] != 'V') return "";
    switch (chunk[0]) {
        case 'K':
            return std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
        case 'V':
            return "042A00#";
        case 'e':
            return "5CECE200,D001B900#";
        case 'z':
            return "8A4FDA00,0D527400#";
        case 'L':
            return "0#";
        case 'm':
            return std::string(1, '\x32') + "#";
        case 'p':
            return "E#";
        case 't':
            return std::string(1, '\x01') + "#";
        default:
            return "#";
    }
}

std::unique_ptr<SnapshotRig> snapshot_rig_synscan_telescope() {
    namespace sc = alpacacore::vendor::synscan;
    auto rig_owner = std::make_unique<SnapshotRig>();
    SnapshotRig& rig = *rig_owner;
    auto st = std::make_shared<SnapshotHandset>();
    auto server = std::make_shared<alpacacore::test::FakeMountServer>(
        [st](const std::string& c) { return snapshot_synscan_reply(*st, c); });
    REQUIRE(server->ok());
    sc::ConnectionInfo info;
    info.type = sc::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server->port();
    info.response_timeout_ms = 300;
    rig.fake = std::make_shared<std::pair<decltype(st), decltype(server)>>(st, server);
    rig.driver = sc::create_synscan_telescope_with_site(0, info, sc::SynScanVersion::V4, -33.856784, 151.215297, 42.0,
                                                        false, sc::SynScanAlignmentSetting::Auto, *rig.clock);
    rig.driver->set_connected(true);
    rig.await_poll();
    auto* driver = rig.driver.get();
    rig.hold_frame = [driver] {
        sc::set_status_poll_paused_for_testing(static_cast<alpacacore::TelescopeDriver&>(*driver), true);
    };
    rig.lose_link = [driver, st] {
        st->mute = true;
        sc::set_status_poll_paused_for_testing(static_cast<alpacacore::TelescopeDriver&>(*driver), false);
    };
    rig.tick = [driver] { sc::poll_status_now_for_testing(static_cast<alpacacore::TelescopeDriver&>(*driver)); };
    return rig_owner;
}
#endif

#ifdef ALPACACORE_ENABLE_ONSTEP
// The idle OnStepX of test_onstep_telescope_snapshot.cpp (SnapshotMount): tracking, no GOTO, not parked.
std::string snapshot_onstep_reply(const std::atomic<bool>& mute, const std::string& chunk) {
    if (mute.load()) return "";
    if (chunk.rfind(":GVP", 0) == 0) return "OnStep#";
    if (chunk.rfind(":GVN", 0) == 0) return "10.23a#";
    if (chunk.rfind(":GXEE", 0) == 0) return "1";
    if (chunk.rfind(":GU", 0) == 0) return "NpEW180#";
    if (chunk.rfind(":GRa", 0) == 0) return "12:00:00.0000#";
    if (chunk.rfind(":GDe", 0) == 0) return "+45*00:00.000#";
    if (chunk.rfind(":GA", 0) == 0) return "+45*00:00#";
    if (chunk.rfind(":GZ", 0) == 0) return "180*00:00#";
    if (chunk.rfind(":GL", 0) == 0) return "10:20:30#";
    if (chunk.rfind(":GC", 0) == 0) return "10/28/26#";
    if (chunk.rfind(":GG", 0) == 0) return "+00:00#";
    if (chunk.rfind(":R", 0) == 0) return "";
    if (chunk.rfind(":S", 0) == 0) return "1";
    return "0#";
}

// Recipe of test_onstep_telescope_snapshot.cpp (Rig): paused poll, one direct tick for the loss.
std::unique_ptr<SnapshotRig> snapshot_rig_onstep_telescope() {
    namespace ons = alpacacore::vendor::onstep;
    auto rig_owner = std::make_unique<SnapshotRig>();
    SnapshotRig& rig = *rig_owner;
    auto mute = std::make_shared<std::atomic<bool>>(false);
    auto server = std::make_shared<alpacacore::test::FakeMountServer>(
        [mute](const std::string& c) { return snapshot_onstep_reply(*mute, c); });
    REQUIRE(server->ok());
    ons::ConnectionInfo info;
    info.type = ons::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server->port();
    info.response_timeout_ms = 300;
    rig.fake = std::make_shared<std::pair<decltype(mute), decltype(server)>>(mute, server);
    rig.driver = ons::create_onstep_telescope_with_site(0, info, -33.856784, 151.215297, 42.0, false, *rig.clock);
    rig.driver->set_connected(true);
    rig.await_poll();
    auto* driver = rig.driver.get();
    rig.hold_frame = [driver] {
        ons::set_status_poll_paused_for_testing(static_cast<alpacacore::TelescopeDriver&>(*driver), true);
    };
    rig.lose_link = [driver, mute] {
        mute->store(true);
        ons::set_status_poll_paused_for_testing(static_cast<alpacacore::TelescopeDriver&>(*driver), false);
    };
    rig.tick = [driver] { ons::poll_status_now_for_testing(static_cast<alpacacore::TelescopeDriver&>(*driver)); };
    return rig_owner;
}
#endif

#ifdef ALPACACORE_ENABLE_CELESTRON
struct SnapshotNexStar {
    std::atomic<bool> mute{false};
};

// The mount answers of test_celestron_telescope.cpp (snapshot_reply()): a German equatorial idle, tracking on.
std::string snapshot_celestron_reply(SnapshotNexStar& st, const std::string& chunk) {
    if (chunk.empty()) return "0#";
    if (st.mute.load()) return "";
    switch (chunk[0]) {
        case 'm':
            return std::string(1, static_cast<char>(24)) + "#";
        case 'V':
            return std::string("\x05\x23#", 3);
        case 'w':
            return std::string(8, '\0') + "#";
        case 'e':
        case 'E':
            return "12AB0500,20000500#";
        case 'z':
        case 'Z':
            return "20AB0500,10000500#";
        case 'L':
            return "0#";
        case 'J':
            return "1#";
        case 'p':
            return "W#";
        case 't':
            return std::string("\x02#");
        case 'P': {
            const unsigned char op = chunk.size() > 3 ? static_cast<unsigned char>(chunk[3]) : 0;
            const unsigned char device = chunk.size() > 2 ? static_cast<unsigned char>(chunk[2]) : 0;
            if (op == 0xFE && device == 0x32) return std::string("\x01\x00#", 3);
            if (op == 0xFE) return std::string("\x00\x00#", 3);
            if (op == 0x26) return "#";
            return std::string("\xFF#");
        }
        default:
            return "#";
    }
}

std::unique_ptr<SnapshotRig> snapshot_rig_celestron_telescope() {
    namespace cel = alpacacore::vendor::celestron;
    auto rig_owner = std::make_unique<SnapshotRig>();
    SnapshotRig& rig = *rig_owner;
    auto st = std::make_shared<SnapshotNexStar>();
    auto server = std::make_shared<alpacacore::test::FakeMountServer>(
        [st](const std::string& c) { return snapshot_celestron_reply(*st, c); });
    REQUIRE(server->ok());
    cel::ConnectionInfo info;
    info.type = cel::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server->port();
    info.response_timeout_ms = 300;
    rig.fake = std::make_shared<std::pair<decltype(st), decltype(server)>>(st, server);
    rig.driver = cel::create_celestron_telescope_with_site(0, info, -33.856784, 151.215297, 42.0, false,
                                                           cel::CelestronAlignmentSetting::Auto, *rig.clock);
    rig.driver->set_connected(true);
    rig.await_poll();
    auto* driver = rig.driver.get();
    rig.hold_frame = [driver] {
        cel::set_status_poll_paused_for_testing(static_cast<alpacacore::TelescopeDriver&>(*driver), true);
    };
    rig.lose_link = [driver, st] {
        st->mute = true;
        cel::set_status_poll_paused_for_testing(static_cast<alpacacore::TelescopeDriver&>(*driver), false);
    };
    rig.tick = [driver] { cel::poll_status_now_for_testing(static_cast<alpacacore::TelescopeDriver&>(*driver)); };
    return rig_owner;
}

// Recipe of test_celestron_focuser.cpp: the AUX fake over a pty, muted to hold the frame.
std::unique_ptr<SnapshotRig> snapshot_rig_celestron_focuser() {
    auto rig_owner = std::make_unique<SnapshotRig>();
    SnapshotRig& rig = *rig_owner;
    auto fake = std::make_shared<alpacacore::test::FakeCelestronFocuser>();
    rig.fake = fake;
    rig.driver = alpacacore::vendor::celestron::create_celestron_focuser(0, fake->slave_path(), *rig.clock);
    rig.driver->set_connected(true);
    rig.await_poll();
    rig.hold_frame = [fake] { fake->set_muted(true); };
    return rig_owner;
}
#endif

#ifdef ALPACACORE_ENABLE_GEMINI
// Recipe of test_gemini_focuser_snapshot.cpp. The fake has no mute: its garbage state replies fail a poll at
// once, without the serial timeout.
std::unique_ptr<SnapshotRig> snapshot_rig_gemini_focuser() {
    auto rig_owner = std::make_unique<SnapshotRig>();
    SnapshotRig& rig = *rig_owner;
    auto fake = std::make_shared<alpacacore::test::FakeGeminiFocuser>();
    rig.fake = fake;
    rig.driver = alpacacore::vendor::gemini::create_gemini_focuser(0, fake->slave_path(), 9600, *rig.clock);
    rig.driver->set_connected(true);
    rig.await_poll();
    rig.hold_frame = [fake] { fake->set_garbage_state_replies(true); };
    return rig_owner;
}
#endif

#ifdef ALPACACORE_ENABLE_QHY
// Recipe of test_qhy_focuser_snapshot.cpp.
std::unique_ptr<SnapshotRig> snapshot_rig_qhy_focuser() {
    auto rig_owner = std::make_unique<SnapshotRig>();
    SnapshotRig& rig = *rig_owner;
    auto fake = std::make_shared<alpacacore::test::FakeQhyQFocuser>();
    rig.fake = fake;
    rig.driver = alpacacore::vendor::qhy::create_qhy_focuser(0, fake->slave_path(), {}, *rig.clock);
    rig.driver->set_connected(true);
    rig.await_poll();
    rig.hold_frame = [fake] { fake->set_muted(true); };
    return rig_owner;
}
#endif

#ifdef ALPACACORE_ENABLE_ASTROASIS
// Recipe of test_astroasis_focuser.cpp: the HID fake with its reads timing out ("dead") holds the frame.
std::unique_ptr<SnapshotRig> snapshot_rig_astroasis_focuser() {
    auto rig_owner = std::make_unique<SnapshotRig>();
    SnapshotRig& rig = *rig_owner;
    auto state = std::make_shared<alpacacore::test::FakeHidState>();
    rig.fake = state;
    rig.driver = alpacacore::vendor::astroasis::create_astroasis_focuser(
        0, "/dev/hidraw-fake",
        std::make_unique<alpacacore::test::LockedAstroasisHidTransport>(
            std::make_unique<alpacacore::test::FakeHidTransport>(state)),
        *rig.clock);
    rig.driver->set_connected(true);
    rig.await_poll();
    rig.hold_frame = [state] {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->dead = true;
    };
    return rig_owner;
}
#endif

#ifdef ALPACACORE_ENABLE_ZWO
std::unique_ptr<SnapshotRig> snapshot_rig_zwo_telescope() {
    namespace zwo = alpacacore::vendor::zwo;
    auto rig = std::make_unique<SnapshotRig>();
    auto muted = std::make_shared<std::atomic<bool>>(false);
    auto mount =
        std::make_shared<alpacacore::test::FakeMountServer>([muted](const std::string& command) -> std::string {
            if (command.find(":GVP#") != std::string::npos) return "AM5#";
            if (command.find(":GMA#") != std::string::npos) return "012345ABCDEF#";
            if (command.find(":GU#") != std::string::npos) return muted->load() ? "e1#" : "NG#";
            if (command.find(":GR#") != std::string::npos) return "12:00:00#";
            if (command.find(":GD#") != std::string::npos) return "+45*00:00#";
            if (command.find(":GA#") != std::string::npos) return "+45*00:00#";
            if (command.find(":GZ#") != std::string::npos) return "180*00:00#";
            if (command.find(":Ggr#") != std::string::npos) return "0.5#";
            if (command.starts_with(":S")) return "1#";
            return "0#";
        });
    REQUIRE(mount->ok());
    zwo::ConnectionInfo info;
    info.type = zwo::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = mount->port();
    info.response_timeout_ms = 500;
    rig->fake = mount;
    rig->driver = zwo::create_zwo_telescope_with_site(0, info, 30.0, -100.0, {}, false, *rig->clock);
    rig->driver->set_connected(true);
    rig->await_poll();
    rig->hold_frame = [muted] { muted->store(true); };
    return rig;
}
#endif

// One X(id, getters) per adopter this build compiles in. `id` is the roster id in contract_sweep.h.
// clang-format off
#ifdef ALPACACORE_ENABLE_SKYWATCHER
#define CS3_SKYWATCHER(X) X(skywatcher_telescope, snapshot_telescope_getters(true))
#else
#define CS3_SKYWATCHER(X)
#endif
#ifdef ALPACACORE_ENABLE_ZWO
#define CS3_ZWO(X) X(zwo_telescope, snapshot_telescope_getters(true))
#else
#define CS3_ZWO(X)
#endif
#ifdef ALPACACORE_ENABLE_CELESTRON
#define CS3_CELESTRON(X) X(celestron_telescope, snapshot_telescope_getters(true)) X(celestron_focuser, snapshot_focuser_getters(false))
#else
#define CS3_CELESTRON(X)
#endif
#ifdef ALPACACORE_ENABLE_BISQUE
#define CS3_BISQUE(X) X(bisque_telescope, snapshot_telescope_getters(false))
#else
#define CS3_BISQUE(X)
#endif
#ifdef ALPACACORE_ENABLE_SYNSCAN
#define CS3_SYNSCAN(X) X(synscan_telescope, snapshot_telescope_getters(true))
#else
#define CS3_SYNSCAN(X)
#endif
#ifdef ALPACACORE_ENABLE_ONSTEP
#define CS3_ONSTEP(X) X(onstep_telescope, snapshot_telescope_getters(true))
#else
#define CS3_ONSTEP(X)
#endif
#ifdef ALPACACORE_ENABLE_GEMINI
#define CS3_GEMINI(X) X(gemini_focuser, snapshot_focuser_getters())
#else
#define CS3_GEMINI(X)
#endif
#ifdef ALPACACORE_ENABLE_QHY
#define CS3_QHY(X) X(qhy_focuser, snapshot_focuser_getters())
#else
#define CS3_QHY(X)
#endif
#ifdef ALPACACORE_ENABLE_ASTROASIS
#define CS3_ASTROASIS(X) X(astroasis_focuser, snapshot_focuser_getters())
#else
#define CS3_ASTROASIS(X)
#endif
#define CONTRACT_SWEEP_SNAPSHOT_ADOPTERS(X) \
    CS3_ZWO(X) CS3_SKYWATCHER(X) CS3_CELESTRON(X) CS3_BISQUE(X) CS3_SYNSCAN(X) CS3_ONSTEP(X) CS3_GEMINI(X) CS3_QHY(X) \
        CS3_ASTROASIS(X)
// clang-format on

// Every getter must answer on the live link first: a getter that throws here is a setup defect of the hook,
// never dropped from the list (the Bisque Slewing exclusion is the ADR's, stated in the rig).
std::vector<double> snapshot_baseline(SnapshotRig& rig, const std::vector<SnapshotGetter>& getters) {
    std::vector<double> values;
    for (const auto& g : getters) {
        if (std::find(rig.left_out.begin(), rig.left_out.end(), g.name) != rig.left_out.end()) {
            values.push_back(0.0);
            continue;
        }
        INFO("baseline getter " << g.name);
        double value = 0.0;
        REQUIRE_NOTHROW(value = g.read(*rig.driver));
        values.push_back(value);
    }
    REQUIRE(rig.driver->get_connected());
    REQUIRE(snapshot_has_time_stamp(*rig.driver));
    return values;
}

[[maybe_unused]] void snapshot_stale_is_served(SnapshotRig& rig, const std::vector<SnapshotGetter>& getters) {
    REQUIRE(rig.driver != nullptr);
    const auto baseline = snapshot_baseline(rig, getters);
    rig.hold_frame();
    rig.pass(kSnapshotStaleStep);
    CHECK(rig.driver->get_connected());
    CHECK(rig.driver->get_link_fault().empty());
    for (std::size_t i = 0; i < getters.size(); ++i) {
        if (std::find(rig.left_out.begin(), rig.left_out.end(), getters[i].name) != rig.left_out.end()) continue;
        INFO(getters[i].name << " past max_age and under the bound must still be served");
        double value = 0.0;
        CHECK_NOTHROW(value = getters[i].read(*rig.driver));
        CHECK(value == Catch::Approx(baseline[i]).margin(kSnapshotValueMargin));
    }
    CHECK(snapshot_has_time_stamp(*rig.driver));
}

[[maybe_unused]] void snapshot_past_bound_stops_serving(SnapshotRig& rig, const std::vector<SnapshotGetter>& getters) {
    REQUIRE(rig.driver != nullptr);
    (void)snapshot_baseline(rig, getters);
    if (rig.lose_link) {
        rig.lose_link();
    } else {
        rig.hold_frame();
    }
    for (int i = 0; i < kSnapshotLatchPolls; ++i) rig.pass(snap_chrono::seconds(1));
    rig.pass(kSnapshotLostStep);
    if (rig.tick) rig.tick();
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.driver->get_device_state().empty());
    for (const auto& g : getters) {
        if (std::find(rig.left_out.begin(), rig.left_out.end(), g.name) != rig.left_out.end()) continue;
        INFO(g.name << " past the link-loss bound must throw NotConnected");
        try {
            (void)g.read(*rig.driver);
            FAIL_CHECK("no throw");
        } catch (const AlpacaException& e) {
            CHECK(e.error_code() == alpacacore::AlpacaError::NotConnected);
        }
    }
}

}  // namespace

#define CS3_CASE(id, getters)                                                                                    \
    TEST_CASE("Contract sweep snapshot - " #id " - a stale frame is served, never refused",                      \
              "[contract][snapshot][" #id "]") {                                                                 \
        auto rig_owner = snapshot_rig_##id();                                                                    \
        snapshot_stale_is_served(*rig_owner, getters);                                                           \
    }                                                                                                            \
    TEST_CASE("Contract sweep snapshot - " #id " - past the link-loss bound the getters stop serving the frame", \
              "[contract][snapshot][" #id "]") {                                                                 \
        auto rig_owner = snapshot_rig_##id();                                                                    \
        snapshot_past_bound_stops_serving(*rig_owner, getters);                                                  \
    }
CONTRACT_SWEEP_SNAPSHOT_ADOPTERS(CS3_CASE)
#undef CS3_CASE
#endif  // !_WIN32
