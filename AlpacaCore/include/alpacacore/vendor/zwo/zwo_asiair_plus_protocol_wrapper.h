// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply
// with all SSPL v1 requirements.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace alpacacore::vendor::zwo {

struct AsiairPlusPortConfig {
    std::string name;
    bool pwm_enabled;
};

// Protocol wrapper for the ZWO ASIAIR Plus (Rockchip RK3568 variant).
//
// The ASIAIR Plus ships with a custom ZWO kernel module (pwm_gpio.ko) that
// owns the device tree's "airplus-gpios" node and registers a misc device
// at /dev/pwm-gpio-misc. The four 12V DC power ports are at kernel ioctl
// indices 4, 5, 6, 7. We expose them as wrapper indices 0..3 so the driver
// layer has a consistent surface across ASIAIR Pro and ASIAIR Plus.
//
// open() is **read-only**: it opens /dev/pwm-gpio-misc and writes no
// kernel-side state. The first ioctl write happens lazily on the user's
// first SetSwitch / SetSwitchValue call. Earlier revisions of this wrapper
// drove kernel index 3 (master enable) at open(); both polarities caused
// every DC port to physically drop to 0 V on every ASCOM connect, because
// the kernel module's master-enable semantics are non-obvious and any
// init-time write also collides with whatever state the previous client
// (or stock ZWO daemon) left behind. The accepted policy is therefore:
// open() observes nothing, writes nothing; close() leaves each port in
// its last-set state — same disconnect policy as the Pi 4 ASIAIR Pro
// driver, but with a strict read-only connect contract on top.
class AsiairPlusProtocolWrapper {
public:
    AsiairPlusProtocolWrapper(std::string device_path,
                              std::vector<AsiairPlusPortConfig> ports,
                              std::uint32_t pwm_frequency_hz);
    ~AsiairPlusProtocolWrapper();

    AsiairPlusProtocolWrapper(const AsiairPlusProtocolWrapper&) = delete;
    AsiairPlusProtocolWrapper& operator=(const AsiairPlusProtocolWrapper&) = delete;
    AsiairPlusProtocolWrapper(AsiairPlusProtocolWrapper&&) = delete;
    AsiairPlusProtocolWrapper& operator=(AsiairPlusProtocolWrapper&&) = delete;

    void open();
    void close();
    bool is_open() const;

    std::size_t port_count() const;
    const AsiairPlusPortConfig& port_config(std::size_t index) const;

    // Boolean port: value is 0 (off / level low) or 1 (on / level high).
    // PWM port: value is the duty-cycle percent in [0, 100].
    int get_value(std::size_t index) const;
    void set_value(std::size_t index, int value);

    std::string device_path() const;
    std::uint32_t pwm_frequency_hz() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace alpacacore::vendor::zwo
