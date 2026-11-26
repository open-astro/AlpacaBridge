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
#include <alpacacore/vendor/zwo/zwo_asiair_switch_driver.h>

#include <string>
#include <utility>

namespace alpacacore::vendor::zwo {

namespace {

constexpr const char* kLogCategory = "ZWO_ASIAIR";
constexpr const char* kGpioConsumer = "alpacabridge-asiair";

util::PowerPortProfile asiair_profile(const AsiairSwitchConfig& config) {
    const std::string board = config.board.empty() ? std::string() : " (" + config.board + ")";
    util::PowerPortProfile profile;
    profile.name = "ZWO " + config.model_name + " Switch";
    profile.description = "ZWO " + config.model_name + board + " 12V power switch (" + config.gpio_chip_path + ")";
    profile.driver_info = "AlpacaCore ZWO " + config.model_name + board + " Switch";
    profile.unique_id_prefix = "ZWO_ASIAIR_";
    profile.log_category = kLogCategory;
    profile.not_connected_message = "ASIAIR switch not connected";
    // Logged only when close() throws. The engine's close() logs a failed settle instead of throwing, so only an
    // unexpected exception (a failed thread join or allocation) reaches it.
    profile.destruction_warning = "Error during ASIAIR switch destruction: ";
    profile.power_on_state = config.power_on_state;
    for (const auto& p : config.ports) {
        util::PowerPortProfilePort port;
        port.name = p.name;
        port.description = "ASIAIR power port on GPIO " + std::to_string(p.gpio_line) + " (" +
                           util::power_port_mode_text(p.pwm_enabled) + ")";
        port.range_error = "Switch value out of range";
        port.pwm_enabled = p.pwm_enabled;
        profile.ports.push_back(std::move(port));
    }
    return profile;
}

} // namespace

AsiairSwitchConfig default_asiair_pro_config() {
    AsiairSwitchConfig cfg;
    cfg.gpio_chip_path = "/dev/gpiochip0";
    cfg.pwm_frequency_hz = 1000;
    cfg.ports = {
        {"Port 1", 12u, false},
        {"Port 2", 13u, false},
        {"Port 3", 26u, false},
        {"Port 4", 18u, false},
    };
    return cfg;
}

std::unique_ptr<SwitchDriver> create_zwo_asiair_switch(int device_number, const AsiairSwitchConfig& config) {
    // The GPIO chip is a libgpiod character-device node; it must be an absolute /dev/ path. Reject anything
    // else up front (empty string, relative path, a bare "gpiochip0") with a clear InvalidValue rather than
    // letting it reach gpiod_chip_open() as an opaque failure.
    if (config.gpio_chip_path.rfind("/dev/", 0) != 0) {
        throw AlpacaException(
            "ASIAIR GPIO chip path must be an absolute /dev/ device node (got '" + config.gpio_chip_path + "')",
            AlpacaError::InvalidValue);
    }
    auto profile = asiair_profile(config);
    util::PortsConfig ports;
    ports.device_path = config.gpio_chip_path;
    ports.pwm_frequency_hz = config.pwm_frequency_hz;
    ports.model_name = "ASIAIR";
    ports.log_category = kLogCategory;
    for (const auto& p : config.ports) {
        ports.ports.push_back({p.name, p.gpio_line, true, p.pwm_enabled});
    }
    return std::make_unique<util::PowerPortSwitchDriver<util::OwnedSoftPwmPorts<util::GpiodLineBackend>>>(
        device_number, std::move(profile), std::move(ports), kGpioConsumer);
}

} // namespace alpacacore::vendor::zwo
