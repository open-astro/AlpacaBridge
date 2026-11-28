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
#include <alpacacore/vendor/ioptron/ioptron_switch_driver.h>

#include <optional>
#include <string>
#include <utility>

namespace alpacacore::vendor::ioptron {

namespace {

constexpr const char* kLogCategory = "IOPTRON_POWERBOX";
constexpr const char* kGpioConsumer = "alpacabridge-imate-powerbox";

util::PowerPortProfile imate_profile(const IoptronSwitchConfig& config) {
    util::PowerPortProfile profile;
    profile.name = "iOptron " + config.model_name;
    profile.description = "iOptron " + config.model_name + " DC power switch (" + config.gpio_chip_path + ")";
    profile.driver_info = "AlpacaCore iOptron " + config.model_name + " Switch";
    profile.unique_id_prefix = "iOptron_iMate_PowerBox_";
    profile.log_category = kLogCategory;
    profile.not_connected_message = "iMate PowerBox not connected";
    // Logged only when close() throws. The engine's close() logs a failed settle instead of throwing, so only an
    // unexpected exception (a failed thread join or allocation) reaches it.
    profile.destruction_warning = "Error during iMate PowerBox switch destruction: ";
    profile.power_on_state = config.power_on_state;
    for (const auto& p : config.ports) {
        util::PowerPortProfilePort port;
        port.name = p.name;
        port.description = p.has_line ? "iMate DC power port on GPIO " + std::to_string(p.gpio_line) + " (" +
                                            util::power_port_mode_text(p.pwm_enabled) + ")"
                                      : "iMate always-on DC pass-through (read-only)";
        port.range_error = "Switch value out of range [0," + std::to_string(p.pwm_enabled ? 100 : 1) + "]";
        port.writable = p.writable;
        port.has_line = p.has_line;
        port.pwm_enabled = p.pwm_enabled;
        profile.ports.push_back(std::move(port));
    }
    return profile;
}

// The GPIO chip is a libgpiod character-device node; it must be an absolute /dev/ path. Reject anything else up
// front (empty string, relative path, a bare "gpiochip0") with a clear InvalidValue rather than letting it reach
// gpiod_chip_open() as an opaque failure.
util::PortsConfig imate_ports(const IoptronSwitchConfig& config) {
    if (config.gpio_chip_path.rfind("/dev/", 0) != 0) {
        throw AlpacaException(
            "iMate PowerBox GPIO chip path must be an absolute /dev/ device node (got '" + config.gpio_chip_path + "')",
            AlpacaError::InvalidValue);
    }
    util::PortsConfig ports;
    ports.device_path = config.gpio_chip_path;
    ports.pwm_frequency_hz = config.pwm_frequency_hz;
    ports.model_name = "iMate PowerBox";
    ports.log_category = kLogCategory;
    for (const auto& p : config.ports) {
        std::optional<unsigned int> line;
        if (p.has_line) {
            line = p.gpio_line;
        }
        ports.ports.push_back({p.name, line, p.writable, p.pwm_enabled});
    }
    return ports;
}

}  // namespace

IoptronSwitchConfig default_imate_powerbox_config() {
    IoptronSwitchConfig cfg;
    // OpenAstro runs the iMate on Armbian's mainline kernel, where the H6 main
    // GPIO bank is /dev/gpiochip1 (the dead stock BSP exposed it as gpiochip0).
    cfg.gpio_chip_path = "/dev/gpiochip1";
    cfg.ports = {
        // name,                has_line, gpio_line, writable
        {"DC3 (always on)", false, 0u, false},
        {"DC1", true, 118u, true},
        {"DC2", true, 114u, true},
    };
    return cfg;
}

std::unique_ptr<SwitchDriver> create_ioptron_switch(int device_number, const IoptronSwitchConfig& config) {
    auto ports = imate_ports(config);
    return std::make_unique<util::PowerPortSwitchDriver<util::OwnedSoftPwmPorts<util::GpiodLineBackend>>>(
        device_number, imate_profile(config), std::move(ports), kGpioConsumer);
}

std::unique_ptr<SwitchDriver> create_ioptron_switch(int device_number, const IoptronSwitchConfig& config,
                                                    util::GpioLineBackend& backend, util::TaskClock& clock) {
    auto ports = imate_ports(config);
    return std::make_unique<util::PowerPortSwitchDriver<util::SoftPwmPorts>>(device_number, imate_profile(config),
                                                                             std::move(ports), backend, clock);
}

}  // namespace alpacacore::vendor::ioptron
