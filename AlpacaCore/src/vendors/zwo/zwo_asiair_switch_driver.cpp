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
#include <alpacacore/vendor/zwo/zwo_asiair_switch_driver.h>

#include <string>
#include <utility>

namespace alpacacore::vendor::zwo {

namespace {

constexpr const char* kLogCategory = "ZWO_ASIAIR";

util::PowerPortProfile asiair_profile(const AsiairSwitchConfig& config) {
    const std::string board = config.board.empty() ? std::string() : " (" + config.board + ")";
    util::PowerPortProfile profile;
    profile.name = "ZWO " + config.model_name + " Switch";
    profile.description = "ZWO " + config.model_name + board + " 12V power switch (" + config.gpio_chip_path + ")";
    profile.driver_info = "AlpacaCore ZWO " + config.model_name + board + " Switch";
    profile.unique_id_prefix = "ZWO_ASIAIR_";
    profile.log_category = kLogCategory;
    profile.not_connected_message = "ASIAIR switch not connected";
    // Logged only when close() throws. This wrapper's close() logs a failed settle instead of throwing, so only an
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

std::unique_ptr<SwitchDriver> create_zwo_asiair_switch(int device_number, AsiairSwitchConfig config) {
    auto profile = asiair_profile(config);
    return std::make_unique<util::PowerPortSwitchDriver<AsiairProtocolWrapper>>(
        device_number, std::move(profile), config.gpio_chip_path, std::move(config.ports), config.pwm_frequency_hz);
}

} // namespace alpacacore::vendor::zwo
