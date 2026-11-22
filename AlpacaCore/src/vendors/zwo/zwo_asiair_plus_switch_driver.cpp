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
#include <alpacacore/util/soft_pwm_ports.h>
#include <alpacacore/vendor/zwo/zwo_asiair_plus_switch_driver.h>

#include <cstddef>
#include <string>
#include <utility>

#include "pwm_gpio_misc_backend.h"

namespace alpacacore::vendor::zwo {

namespace {

constexpr const char* kLogCategory = "ZWO_ASIAIR_PLUS";

util::PowerPortProfile asiair_plus_profile(const AsiairPlusSwitchConfig& config) {
    util::PowerPortProfile profile;
    profile.name = "ZWO ASIAIR Plus Switch";
    profile.description = "ZWO ASIAIR Plus (RK3568) 12V power switch (" + config.device_path + ")";
    profile.driver_info = "AlpacaCore ZWO ASIAIR Plus Switch";
    profile.unique_id_prefix = "ZWO_ASIAIR_PLUS_RK3568_";
    profile.log_category = kLogCategory;
    profile.not_connected_message = "ASIAIR Plus switch not connected";
    // Logged only when close() throws. The engine's close() logs a failed settle instead of throwing, so only an
    // unexpected exception (a failed thread join or allocation) reaches it.
    profile.destruction_warning = "Error during ASIAIR Plus switch destruction: ";
    profile.power_on_state = config.power_on_state;
    for (std::size_t i = 0; i < config.ports.size(); ++i) {
        const auto& p = config.ports[i];
        util::PowerPortProfilePort port;
        port.name = p.name;
        port.description =
            "ASIAIR Plus DC port " + std::to_string(i + 1) + " (" + util::power_port_mode_text(p.pwm_enabled) + ")";
        port.range_error = "Switch value out of range";
        port.pwm_enabled = p.pwm_enabled;
        profile.ports.push_back(std::move(port));
    }
    return profile;
}

} // namespace

AsiairPlusSwitchConfig default_asiair_plus_rk3568_config() {
    AsiairPlusSwitchConfig cfg;
    cfg.device_path = "/dev/pwm-gpio-misc";
    // Userspace soft-PWM frequency. Set to 50 Hz to match what ZWO's
    // stock `zwoair_imager` daemon actually drives the kernel module
    // with — confirmed by booting back to stock firmware and reading
    // the per-port config via `PWM_GPIO_GET_CONFIG`:
    //
    //   Port 1 (camera, boolean): no PWM config
    //   Port 2 (dew heater 37%):  period_ns = 20,000,000 → 50 Hz, duty 37.0%
    //   Port 3 (flat panel 100%): period_ns = 20,000,000 → 50 Hz, duty 100.0%
    //   Port 4 (flat panel 43%):  period_ns = 20,000,000 → 50 Hz, duty 43.0%
    //
    // Earlier defaults in this driver (1 kHz, then 200 Hz) were chosen
    // from indoor bench tests against an HSK A4 LED tracing pad whose
    // own internal dimming controller created cascaded-PWM artifacts
    // that don't show up against real astrophotography loads. The
    // resistive dew heaters and the flat-field panels with their own
    // DC-DC regulators that ZWO designed around are happy at 50 Hz —
    // dew heaters are thermal loads (slow time constant, no observable
    // flicker), and panel drivers smooth the input. 50 Hz also matches
    // mains frequency in China (where ZWO is based), likely chosen to
    // avoid beating against AC ripple in the 12 V input.
    //
    // We still do PWM in userspace rather than via the kernel module's
    // own PWM mode. The .ko's hrtimer-based PWM does work on stock ZWO
    // (`/proc/timer_list` shows `pwm_gpio_timer_func` scheduled at the
    // expected intervals when the daemon is running), but the exact
    // ioctl sequence to arm it from a fresh process is opaque to us —
    // every documented ordering we tried left the hrtimer unscheduled.
    // So we replicate ZWO's *waveform* (50 Hz at the user's duty) via
    // our pwm_loop worker thread without depending on their specific
    // kernel-module activation sequence.
    //
    // Hardware constraint still applies: the RK3568 has 16 hardware
    // PWM peripherals but NONE of them mux to the airplus-gpios pins
    // on GPIO bank 4, so a true-hardware-PWM path isn't an option
    // either way.
    cfg.pwm_frequency_hz = 50;
    cfg.ports = {
        {"Port 1", false},
        {"Port 2", false},
        {"Port 3", false},
        {"Port 4", false},
    };
    return cfg;
}

std::unique_ptr<SwitchDriver> create_zwo_asiair_plus_switch(int device_number, const AsiairPlusSwitchConfig& config) {
    if (config.ports.size() > 4) {
        throw AlpacaException("ASIAIR Plus exposes 4 DC ports; configuration lists more", AlpacaError::InvalidValue);
    }
    auto profile = asiair_plus_profile(config);
    util::PortsConfig ports;
    ports.device_path = config.device_path;
    ports.pwm_frequency_hz = config.pwm_frequency_hz;
    ports.model_name = "ASIAIR Plus";
    ports.log_category = kLogCategory;
    // Ports 1..4 are lines 0..3: the backend maps them onto the kernel indices 4..7.
    for (std::size_t i = 0; i < config.ports.size(); ++i) {
        ports.ports.push_back({config.ports[i].name, static_cast<unsigned int>(i), true, config.ports[i].pwm_enabled});
    }
    return std::make_unique<util::PowerPortSwitchDriver<util::OwnedSoftPwmPorts<PwmGpioMiscBackend>>>(
        device_number, std::move(profile), std::move(ports));
}

} // namespace alpacacore::vendor::zwo
