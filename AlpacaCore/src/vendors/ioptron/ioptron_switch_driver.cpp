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

#include <alpacacore/util/power_port_switch_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_switch_driver.h>

#include <string>
#include <utility>

namespace alpacacore::vendor::ioptron {

namespace {

constexpr const char* kLogCategory = "IOPTRON_POWERBOX";

using PowerboxSwitchDriver = util::PowerPortSwitchDriver<IoptronPowerboxWrapper>;

util::PowerPortProfile imate_profile(const IoptronSwitchConfig& config) {
    util::PowerPortProfile profile;
    profile.name = "iOptron " + config.model_name;
    profile.description = "iOptron " + config.model_name + " DC power switch (" + config.gpio_chip_path + ")";
    profile.driver_info = "AlpacaCore iOptron " + config.model_name + " Switch";
    profile.unique_id_prefix = "iOptron_iMate_PowerBox_";
    profile.log_category = kLogCategory;
    profile.not_connected_message = "iMate PowerBox not connected";
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

std::unique_ptr<SwitchDriver> create_ioptron_switch(int device_number, IoptronSwitchConfig config) {
    auto profile = imate_profile(config);
    return std::make_unique<PowerboxSwitchDriver>(device_number, std::move(profile), config.gpio_chip_path,
                                                  std::move(config.ports), config.pwm_frequency_hz);
}

std::unique_ptr<SwitchDriver> create_ioptron_switch(int device_number, IoptronSwitchConfig config,
                                                    IoptronGpioBackend& backend) {
    auto profile = imate_profile(config);
    return std::make_unique<PowerboxSwitchDriver>(device_number, std::move(profile), config.gpio_chip_path,
                                                  std::move(config.ports), config.pwm_frequency_hz, &backend);
}

}  // namespace alpacacore::vendor::ioptron
