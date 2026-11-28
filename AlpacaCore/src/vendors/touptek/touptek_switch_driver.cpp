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
#include <alpacacore/util/gpiod_line_backend.h>
#include <alpacacore/util/power_port_switch_driver.h>
#include <alpacacore/util/soft_pwm_ports.h>
#include <alpacacore/vendor/touptek/touptek_switch_driver.h>

#include <memory>
#include <string>
#include <utility>

namespace alpacacore::vendor::touptek {

namespace {

constexpr const char* kLogCategory = "TOUPTEK_STELLAVITA";
constexpr const char* kGpioConsumer = "alpacabridge-stellavita-powerbox";

util::PowerPortProfile stellavita_profile(const TouptekSwitchConfig& config) {
    util::PowerPortProfile profile;
    profile.name = "ToupTek " + config.model_name;
    profile.description = "ToupTek " + config.model_name + " DC power switch (" + config.gpio_chip_path + ")";
    profile.driver_info = "AlpacaCore ToupTek " + config.model_name + " Switch";
    profile.unique_id_prefix = "ToupTek_StellaVita_";
    profile.log_category = kLogCategory;
    profile.not_connected_message = "StellaVita not connected";
    profile.destruction_warning = "Error during StellaVita switch destruction: ";
    profile.power_on_state = config.power_on_state;
    for (const auto& p : config.ports) {
        util::PowerPortProfilePort port;
        port.name = p.name;
        port.description = "StellaVita DC power port on GPIO " + std::to_string(p.gpio_line) + " (" +
                           util::power_port_mode_text(p.pwm_enabled) + ")";
        port.range_error = "Switch value out of range [0," + std::to_string(p.pwm_enabled ? 100 : 1) + "]";
        port.writable = p.writable;
        port.pwm_enabled = p.pwm_enabled;
        profile.ports.push_back(std::move(port));
    }
    return profile;
}

// The GPIO chip is a libgpiod character-device node; it must be an absolute /dev/ path. Reject anything else up
// front (empty string, relative path, a bare "gpiochip0") with a clear InvalidValue rather than letting it reach
// gpiod_chip_open() as an opaque failure.
util::PortsConfig stellavita_ports(const TouptekSwitchConfig& config) {
    if (config.gpio_chip_path.rfind("/dev/", 0) != 0) {
        throw AlpacaException(
            "StellaVita GPIO chip path must be an absolute /dev/ device node (got '" + config.gpio_chip_path + "')",
            AlpacaError::InvalidValue);
    }
    util::PortsConfig ports;
    ports.device_path = config.gpio_chip_path;
    ports.pwm_frequency_hz = config.pwm_frequency_hz;
    ports.model_name = "StellaVita";
    ports.log_category = kLogCategory;
    for (const auto& p : config.ports) {
        ports.ports.push_back({p.name, p.gpio_line, p.writable, p.pwm_enabled});
    }
    return ports;
}

}  // namespace

TouptekSwitchConfig default_stellavita_config() {
    TouptekSwitchConfig cfg;
    // StellaVita is a Raspberry Pi CM4 (BCM2711). The 40-pin header bank is
    // /dev/gpiochip0 (pinctrl-bcm2711); the libgpiod line offset equals the BCM
    // GPIO number. Mapping verified on hardware against config.txt
    // (gpio=18,10,17,4,9,11=op,dh,pu) — 9/11 power the USB hub and are excluded.
    cfg.gpio_chip_path = "/dev/gpiochip0";
    cfg.ports = {
        // name,      gpio_line, writable
        {"Port 1", 18u, true},
        {"Port 2", 10u, true},
        {"Port 3", 17u, true},
        {"Port 4", 4u, true},
    };
    return cfg;
}

std::unique_ptr<SwitchDriver> create_touptek_switch(int device_number, TouptekSwitchConfig config) {
    auto ports = stellavita_ports(config);
    return std::make_unique<util::PowerPortSwitchDriver<util::OwnedSoftPwmPorts<util::GpiodLineBackend>>>(
        device_number, stellavita_profile(config), std::move(ports), kGpioConsumer);
}

std::unique_ptr<SwitchDriver> create_touptek_switch(int device_number, const TouptekSwitchConfig& config,
                                                    util::GpioLineBackend& backend, util::TaskClock& clock) {
    return std::make_unique<util::PowerPortSwitchDriver<util::SoftPwmPorts>>(device_number, stellavita_profile(config),
                                                                             stellavita_ports(config), backend, clock);
}

}  // namespace alpacacore::vendor::touptek
