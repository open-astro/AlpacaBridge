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

// The ZWO factories, doing what the router arms they replace did. Compiled only
// under ALPACACORE_ENABLE_ZWO (unlike zwo_schema.cpp), so the vendor headers are
// fine here. Every create_* call is hardware-free (the SDK is reached at
// connect), so nothing is probed at registration. The one exception is the
// camera's bind hook, which enumerates the SDK at registration as the router arm
// it replaces did, to learn the serial and model name it binds by (#914); the
// camera therefore has no create_* at all.

#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/zwo/zwo_asiair_plus_switch_driver.h>
#include <alpacacore/vendor/zwo/zwo_asiair_switch_driver.h>
#include <alpacacore/vendor/zwo/zwo_camera_driver.h>
#include <alpacacore/vendor/zwo/zwo_filterwheel_driver.h>
#include <alpacacore/vendor/zwo/zwo_focuser_driver.h>
#include <alpacacore/vendor/zwo/zwo_rotator_driver.h>
#include <alpacacore/vendor/zwo/zwo_sdk_wrapper.h>
#include <alpacacore/vendor/zwo/zwo_switch_driver.h>
#include <alpacacore/vendor/zwo/zwo_telescope_driver.h>

#include <optional>
#include <string>

#include "../../catalog/builtin_descriptors.h"
#include "zwo_fields.h"

namespace alpacacore::catalog {

namespace {

void refuse_if(const std::optional<std::string>& refusal) {
    if (refusal) throw AlpacaException(*refusal, AlpacaError::InvalidValue);
}

std::unique_ptr<AlpacaDriver> create_telescope(const DeviceConfig& config, int device_number) {
    vendor::zwo::ConnectionInfo info;
    // normalize has left "auto", "serial" or "network"; an absent value (no
    // normalize ran) is "auto", and anything else is read as serial, never auto (#380).
    const std::string type = config.get(kZwoConnectionType);
    if (type == "auto") {
        info.type = vendor::zwo::ConnectionType::Auto;
    } else if (type == "network") {
        info.type = vendor::zwo::ConnectionType::Network;
        info.host = config.get(kZwoHost);
        info.tcp_port = static_cast<int>(config.get(kZwoTcpPort));
    } else {
        info.type = vendor::zwo::ConnectionType::Serial;
        info.port_path = config.get(kZwoPortPath);
        info.baud_rate = static_cast<int>(config.get(kZwoBaudRate));
    }
    if (const auto timeout = config.find(kZwoResponseTimeoutMs)) {
        info.response_timeout_ms = static_cast<int>(*timeout);
    }

    // find(), not get(): the 0.0 defaults are not a site.
    const std::optional<double> latitude = config.find(kZwoSiteLatitude);
    const std::optional<double> longitude = config.find(kZwoSiteLongitude);
    const std::optional<double> elevation = config.find(kZwoSiteElevation);
    const std::optional<bool> sync_time = config.find(kZwoSyncTimeOnConnect);
    auto telescope =
        vendor::zwo::create_zwo_telescope_with_site(device_number, info, latitude, longitude, elevation, sync_time);

    if (const double aperture = config.get(kZwoApertureDiameter); aperture > 0.0) {
        telescope->set_aperture_diameter(aperture);
    }
    if (const double focal = config.get(kZwoFocalLength); focal > 0.0) {
        telescope->set_focal_length(focal);
    }
    if (elevation) {
        telescope->set_site_elevation(*elevation);
    }
    return telescope;
}

std::unique_ptr<AlpacaDriver> create_switch(const DeviceConfig& config, int device_number) {
    refuse_if(zwo_switch_refusal(config));
    const std::string type = zwo_switch_type(config);

    if (type == "asiair-plus-rk3568") {
        auto plus_config = vendor::zwo::default_asiair_plus_rk3568_config();
        if (const auto hz = config.find(kZwoPwmFrequencyHz)) {
            plus_config.pwm_frequency_hz = static_cast<decltype(plus_config.pwm_frequency_hz)>(*hz);
        }
        if (const auto records = config.find(kZwoPorts); records && !records->empty()) {
            std::vector<vendor::zwo::AsiairPlusPortConfig> ports;
            ports.reserve(records->size());
            for (const DeviceConfig& record : *records) {
                vendor::zwo::AsiairPlusPortConfig port;
                port.name = record.find(kZwoPortName).value_or("Port " + std::to_string(ports.size() + 1));
                port.pwm_enabled = record.get(kZwoPortPwm);
                ports.push_back(std::move(port));
            }
            plus_config.ports = std::move(ports);
        }
        return vendor::zwo::create_zwo_asiair_plus_switch(device_number, plus_config);
    }

    // ASIAIR Pro (Pi 4) and ASIAIR Plus (Pi CM4) share the same on-board libgpiod
    // wiring, verified against live CM4 hardware: one driver and default config;
    // only the model label differs.
    if (type == "asiair" || type == "asiair-plus-picm4") {
        auto asiair_config = vendor::zwo::default_asiair_pro_config();
        if (type == "asiair-plus-picm4") {
            asiair_config.model_name = "ASIAIR Plus (Pi CM4)";
        }
        asiair_config.gpio_chip_path = config.get(kZwoGpioChip);
        if (const auto hz = config.find(kZwoPwmFrequencyHz)) {
            asiair_config.pwm_frequency_hz = static_cast<decltype(asiair_config.pwm_frequency_hz)>(*hz);
        }
        if (const auto records = config.find(kZwoPorts); records && !records->empty()) {
            std::vector<vendor::zwo::AsiairPortConfig> ports;
            ports.reserve(records->size());
            for (const DeviceConfig& record : *records) {
                vendor::zwo::AsiairPortConfig port;
                port.name = record.find(kZwoPortName).value_or("Port " + std::to_string(ports.size() + 1));
                port.gpio_line = static_cast<std::uint32_t>(record.get(kZwoPortGpio));
                port.pwm_enabled = record.get(kZwoPortPwm);
                ports.push_back(std::move(port));
            }
            asiair_config.ports = std::move(ports);
        }
        return vendor::zwo::create_zwo_asiair_switch(device_number, std::move(asiair_config));
    }

    const int camera_id = static_cast<int>(config.get(kZwoCameraId));
    if (camera_id >= 0) return vendor::zwo::create_zwo_dew_heater_switch(device_number, camera_id);
    return vendor::zwo::create_zwo_dew_heater_switch_by_index(device_number,
                                                              static_cast<int>(config.get(kZwoCameraIndex)));
}

// The camera entry binds by the physical body (#914): the serial, not the
// enumeration order, says which camera it is. `context.peers` are the stored
// camera entries, this one's own included; the serials the others hold are
// claimed (a serial-less entry skips them), and a serial or UniqueID another
// entry holds is refused from the API and cleared, to be relearned, in a stored
// entry. What registration learns (serial, model name, UniqueID) is returned for
// the entry to store, so the next start binds by it.
BindResult bind_camera(vendor::zwo::ZWOSDK& sdk, const DeviceConfig& config, int device_number,
                       const BindContext& context) {
    BindResult result;
    const auto selector = [&](const Field<std::int64_t>& field) { return static_cast<int>(config.get(field)); };
    const int camera_id = selector(kZwoCamId);
    const int camera_index = selector(kZwoCamIndex);
    std::string configured_serial = config.get(kZwoCamSerial);
    const std::string configured_name = config.get(kZwoCamName);
    std::string unique_id = config.get(kZwoCamUniqueId);
    const std::string supplied_unique_id = unique_id;
    const bool unique_id_supplied = !unique_id.empty();

    vendor::zwo::ZwoCameraBinding binding;
    if (camera_id >= 0) binding.identity.camera_id = camera_id;
    if (camera_index >= 0) binding.identity.camera_index = camera_index;
    binding.identity.serial = configured_serial;
    binding.identity.camera_name = configured_name;

    bool duplicate_serial = false;
    bool duplicate_unique_id = false;
    bool cleared_duplicate_serial = false;
    bool cleared_duplicate_unique_id = false;
    for (const PeerEntry& peer : context.peers) {
        const std::optional<std::string> peer_serial = peer.config.find(kZwoCamSerial);
        const std::optional<std::string> peer_unique_id = peer.config.find(kZwoCamUniqueId);
        if (peer.device_number == device_number) {
            // A re-configure that does not resend uniqueId keeps the one stored for this number.
            if (unique_id.empty() && peer_unique_id && zwo_identity_text_valid(*peer_unique_id)) {
                unique_id = *peer_unique_id;
            }
            continue;
        }
        if (peer_serial && !peer_serial->empty()) {
            binding.claimed_serials.insert(*peer_serial);
            duplicate_serial = duplicate_serial || *peer_serial == configured_serial;
        }
        if (unique_id_supplied && peer_unique_id && *peer_unique_id == unique_id) duplicate_unique_id = true;
    }

    // One serial or UniqueID names one camera: another entry holding it would bind
    // the same body twice (or report one UniqueID twice).
    if (configured_serial.empty()) duplicate_serial = false;
    if (duplicate_serial || duplicate_unique_id) {
        const char* field = duplicate_serial ? kZwoCamSerial.key : kZwoCamUniqueId.key;
        if (context.source == Source::Api) {
            result.rejection = std::string("Invalid value for ") + field + ": another ZWO camera already uses it";
            return result;
        }
        ALPACA_LOG_WARN("ZWO", std::string("Ignoring duplicate ZWO camera ") + field + " in the stored entry");
        // Each duplicated field is cleared and relearned on its own.
        if (duplicate_serial) {
            configured_serial.clear();
            binding.identity.serial.clear();
            cleared_duplicate_serial = true;
        }
        if (duplicate_unique_id) {
            unique_id.clear();
            cleared_duplicate_unique_id = true;
        }
    }

    // A camera that is not plugged in yet still registers with the identity it was
    // configured with; connect reports why it is absent. Known limit: an entry with
    // only an index/id hint learns whatever serial sits at that hint. With two saved
    // cameras and only one plugged in, the first entry can learn the other body's serial.
    std::string learned_serial = configured_serial;
    std::string learned_name = configured_name;
    try {
        const auto resolved = vendor::zwo::resolve_zwo_camera(
            binding.identity, sdk.enumerate_identified_cameras(vendor::zwo::trim_zwo_name(configured_name)),
            binding.claimed_serials);
        if (resolved.camera.has_value()) {
            const auto& found = resolved.camera.value();
            binding.identity.camera_id = found.camera_id;
            binding.identity.camera_index = found.index;
            if (learned_serial.empty()) learned_serial = found.serial;
            if (learned_name.empty()) learned_name = vendor::zwo::trim_zwo_name(found.name);
        }
    } catch (const std::exception& e) {
        ALPACA_LOG_WARN("ZWO", std::string("ZWO camera enumeration failed: ") + e.what());
    }
    binding.identity.serial = learned_serial;
    binding.identity.camera_name = learned_name;
    if (learned_serial.empty() && unique_id.empty()) unique_id = vendor::zwo::generate_zwo_unique_id();
    binding.unique_id = unique_id;

    if (configured_serial.empty() && !learned_serial.empty()) {
        result.learned.set(kZwoCamSerial.key, learned_serial);
    } else if (cleared_duplicate_serial) {
        // Persist the clear for this entry, so the entry processed after it no longer
        // sees a duplicate and keeps the serial.
        result.learned.set(kZwoCamSerial.key, std::string());
    }
    if (configured_name.empty() && !learned_name.empty()) result.learned.set(kZwoCamName.key, learned_name);
    if (!unique_id.empty() && unique_id != supplied_unique_id) {
        result.learned.set(kZwoCamUniqueId.key, unique_id);
    } else if (cleared_duplicate_unique_id) {
        // Persist the clear, as for a serial: otherwise the duplicate stays in the file and
        // both entries clear it again on every start.
        result.learned.set(kZwoCamUniqueId.key, std::string());
    }

    result.driver = vendor::zwo::create_zwo_camera_bound(device_number, binding, sdk);
    return result;
}

}  // namespace

void register_zwo_camera_factory(DeviceCatalog& catalog, vendor::zwo::ZWOSDK& sdk) {
    Factory camera;
    camera.key = DeviceKey{"zwo", DeviceType::Camera};
    camera.bind = [&sdk](const DeviceConfig& config, int device_number, const BindContext& context) {
        return bind_camera(sdk, config, device_number, context);
    };
    catalog.add(std::move(camera));
}

void register_zwo_factory(DeviceCatalog& catalog) {
    register_zwo_camera_factory(catalog, vendor::zwo::ZWOSDKWrapper::instance());

    Factory telescope;
    telescope.key = DeviceKey{"zwo", DeviceType::Telescope};
    telescope.create = create_telescope;
    catalog.add(std::move(telescope));

    Factory wheel;
    wheel.key = DeviceKey{"zwo", DeviceType::FilterWheel};
    wheel.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        const int id = static_cast<int>(config.get(kZwoFilterwheelId));
        std::unique_ptr<FilterWheelDriver> driver =
            id >= 0 ? vendor::zwo::create_zwo_efw_filterwheel(device_number, id)
                    : vendor::zwo::create_zwo_efw_filterwheel_by_index(
                          device_number, static_cast<int>(config.get(kZwoFilterwheelIndex)));
        if (auto names = config.find(kZwoFilterNames)) driver->set_names(*names);
        return driver;
    };
    catalog.add(std::move(wheel));

    Factory focuser;
    focuser.key = DeviceKey{"zwo", DeviceType::Focuser};
    focuser.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        const int id = static_cast<int>(config.get(kZwoFocuserId));
        if (id >= 0) return vendor::zwo::create_zwo_eaf_focuser(device_number, id);
        return vendor::zwo::create_zwo_eaf_focuser_by_index(device_number,
                                                            static_cast<int>(config.get(kZwoFocuserIndex)));
    };
    catalog.add(std::move(focuser));

    Factory rotator;
    rotator.key = DeviceKey{"zwo", DeviceType::Rotator};
    rotator.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        const int id = static_cast<int>(config.get(kZwoRotatorId));
        if (id >= 0) return vendor::zwo::create_zwo_caa_rotator(device_number, id);
        return vendor::zwo::create_zwo_caa_rotator_by_index(device_number,
                                                            static_cast<int>(config.get(kZwoRotatorIndex)));
    };
    catalog.add(std::move(rotator));

    Factory sw;
    sw.key = DeviceKey{"zwo", DeviceType::Switch};
    sw.create = create_switch;
    catalog.add(std::move(sw));
}

}  // namespace alpacacore::catalog
