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
#include <alpacacore/util/power_port_switch_driver.h>
#include <alpacacore/vendor/touptek/touptek_powerbox_wrapper.h>
#include <alpacacore/vendor/touptek/touptek_switch_driver.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace alpacacore::vendor::touptek {

namespace {

constexpr const char* kLogCategory = "TOUPTEK_STELLAVITA";

// The port wrapper the shared driver drives: the libgpiod power box it owns, or
// a power box a test injects (which must outlive the driver).
class StellaVitaPowerbox {
public:
    StellaVitaPowerbox(std::string gpio_chip_path, std::vector<StellaVitaPortConfig> ports,
                       std::uint32_t pwm_frequency_hz)
        : owned_(
              std::make_unique<TouptekPowerboxWrapper>(std::move(gpio_chip_path), std::move(ports), pwm_frequency_hz)),
          box_(*owned_) {}

    explicit StellaVitaPowerbox(TouptekPowerboxSDK& injected) : box_(injected) {}

    StellaVitaPowerbox(const StellaVitaPowerbox&) = delete;
    StellaVitaPowerbox& operator=(const StellaVitaPowerbox&) = delete;
    StellaVitaPowerbox(StellaVitaPowerbox&&) = delete;
    StellaVitaPowerbox& operator=(StellaVitaPowerbox&&) = delete;
    ~StellaVitaPowerbox() = default;

    void open() { box_.open(); }
    void close() { box_.close(); }
    bool is_open() const { return box_.is_open(); }
    int get_value(std::size_t index) const { return box_.get_value(index); }
    void set_value(std::size_t index, int value) { box_.set_value(index, value); }

private:
    // Set only by the production constructor; declared before box_, which refers to it.
    std::unique_ptr<TouptekPowerboxWrapper> owned_;
    TouptekPowerboxSDK& box_;
};

using StellaVitaSwitchDriver = util::PowerPortSwitchDriver<StellaVitaPowerbox>;

// Takes the config by value: the injected factory hands it over, and the port names move out of it.
util::PowerPortProfile stellavita_profile(TouptekSwitchConfig config) {
    util::PowerPortProfile profile;
    profile.name = "ToupTek " + config.model_name;
    profile.description = "ToupTek " + config.model_name + " DC power switch (" + config.gpio_chip_path + ")";
    profile.driver_info = "AlpacaCore ToupTek " + config.model_name + " Switch";
    profile.unique_id_prefix = "ToupTek_StellaVita_";
    profile.log_category = kLogCategory;
    profile.not_connected_message = "StellaVita not connected";
    profile.destruction_warning = "Error during StellaVita switch destruction: ";
    profile.power_on_state = config.power_on_state;
    for (auto& p : config.ports) {
        util::PowerPortProfilePort port;
        port.name = std::move(p.name);
        port.description = "StellaVita DC power port on GPIO " + std::to_string(p.gpio_line) + " (" +
                           util::power_port_mode_text(p.pwm_enabled) + ")";
        port.range_error = "Switch value out of range [0," + std::to_string(p.pwm_enabled ? 100 : 1) + "]";
        port.writable = p.writable;
        port.pwm_enabled = p.pwm_enabled;
        profile.ports.push_back(std::move(port));
    }
    return profile;
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
    auto profile = stellavita_profile(config);
    return std::make_unique<StellaVitaSwitchDriver>(device_number, std::move(profile), config.gpio_chip_path,
                                                    std::move(config.ports), config.pwm_frequency_hz);
}

std::unique_ptr<SwitchDriver> create_touptek_switch(int device_number, TouptekSwitchConfig config,
                                                    TouptekPowerboxSDK& powerbox) {
    if (powerbox.port_count() != config.ports.size()) {
        throw AlpacaException("StellaVita power box has " + std::to_string(powerbox.port_count()) +
                                  " ports but the configuration lists " + std::to_string(config.ports.size()),
                              AlpacaError::InvalidValue);
    }
    return std::make_unique<StellaVitaSwitchDriver>(device_number, stellavita_profile(std::move(config)), powerbox);
}

}  // namespace alpacacore::vendor::touptek
