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

#pragma once

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/touptek/touptek_powerbox_sdk.h>
#include <alpacacore/vendor/touptek/touptek_switch_driver.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace alpacacore::test {

// Scripted StellaVita power box: in-memory port values, no GPIO chip. Shared by the
// switch seam cases, the connected stress case and the contract sweep.
// Every method but open_delay's sleep runs under one internal mutex, and is_open() is a
// lock-free atomic like the real wrapper's (issue #382), so the fake is race-free under
// a concurrent storm. The public knobs and counters are plain fields for single-threaded
// tests, set before the driver starts and read after it stops; the call counters are
// guarded and read through the accessors.
//
// KNOWN PARITY GAPS (the real wrapper is harsher here; prefer closing a gap):
// - No PWM worker thread: a PWM duty is a stored number, never a waveform.
// - No line-write failure: the real set_value() throws DriverException when
//   gpiod_line_request_set_value() fails; set fail_open for the connect side only.
// - open() is instant unless open_delay is set, where the real one requests the chip and lines.
// - gpio_chip_path() and pwm_frequency_hz() answer the constructor values unchecked; the real
//   wrapper rejects a non-/dev/ path and a frequency outside 1..100000 at construction.
class FakeTouptekPowerboxSDK final : public vendor::touptek::TouptekPowerboxSDK {
public:
    using Port = vendor::touptek::StellaVitaPortConfig;

    explicit FakeTouptekPowerboxSDK(std::vector<Port> port_table = vendor::touptek::default_stellavita_config().ports,
                                    std::string chip_path = "/dev/gpiochip0", std::uint32_t frequency_hz = 100)
        : ports_(std::move(port_table)), chip_path_(std::move(chip_path)), frequency_hz_(frequency_hz) {
        // Ports start on, as the real wrapper's constructor sets them (100 for PWM, 1 otherwise).
        for (const auto& p : ports_) values_.push_back(p.pwm_enabled ? 100 : 1);
    }

    // While true every open() throws (a connect that must fail); the flag is never cleared.
    bool fail_open{false};
    // open() sleeps this long before returning (a connect held open).
    std::chrono::milliseconds open_delay{0};

    void open() override {
        if (open_delay.count() > 0) std::this_thread::sleep_for(open_delay);
        std::lock_guard<std::mutex> lock(mutex_);
        ++open_calls_;
        if (fail_open) throw AlpacaException("scripted open failure", AlpacaError::DriverException);
        // Values are kept: the real wrapper keeps its cached levels across a reconnect.
        open_.store(true, std::memory_order_release);
    }
    void close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++close_calls_;
        open_.store(false, std::memory_order_release);
    }
    bool is_open() const override {
        ++is_open_calls_;
        return open_.load(std::memory_order_acquire);
    }

    std::size_t port_count() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++port_count_calls_;
        return ports_.size();
    }
    Port port_config(std::size_t index) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++port_config_calls_;
        check_index(index);
        return ports_[index];
    }
    int get_value(std::size_t index) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++get_value_calls_;
        check_index(index);
        return values_[index];
    }
    void set_value(std::size_t index, int value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++set_value_calls_;
        check_index(index);
        const Port& cfg = ports_[index];
        if (!cfg.writable) {
            throw AlpacaException("Port '" + cfg.name + "' is read-only", AlpacaError::NotImplemented);
        }
        if (cfg.pwm_enabled ? (value < 0 || value > 100) : (value != 0 && value != 1)) {
            throw AlpacaException("value out of range", AlpacaError::InvalidValue);
        }
        if (!open_.load(std::memory_order_acquire)) {
            throw AlpacaException("GPIO chip is not open", AlpacaError::NotConnected);
        }
        values_[index] = value;
    }

    std::string gpio_chip_path() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++gpio_chip_path_calls_;
        return chip_path_;
    }
    std::uint32_t pwm_frequency_hz() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++pwm_frequency_hz_calls_;
        return frequency_hz_;
    }

    int open_calls() const { return read(open_calls_); }
    int close_calls() const { return read(close_calls_); }
    int is_open_calls() const { return is_open_calls_.load(); }
    int port_count_calls() const { return read(port_count_calls_); }
    int port_config_calls() const { return read(port_config_calls_); }
    int get_value_calls() const { return read(get_value_calls_); }
    int set_value_calls() const { return read(set_value_calls_); }
    int gpio_chip_path_calls() const { return read(gpio_chip_path_calls_); }
    int pwm_frequency_hz_calls() const { return read(pwm_frequency_hz_calls_); }

private:
    int read(const int& counter) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return counter;
    }
    // The real wrapper throws InvalidValue for an index past the table.
    void check_index(std::size_t index) const {
        if (index >= ports_.size()) throw AlpacaException("Port index out of range", AlpacaError::InvalidValue);
    }

    mutable std::mutex mutex_;
    std::vector<Port> ports_;
    std::vector<int> values_;
    std::string chip_path_;
    std::uint32_t frequency_hz_;
    std::atomic<bool> open_{false};
    mutable int open_calls_{0};
    mutable int close_calls_{0};
    mutable std::atomic<int> is_open_calls_{0};
    mutable int port_count_calls_{0};
    mutable int port_config_calls_{0};
    mutable int get_value_calls_{0};
    mutable int set_value_calls_{0};
    mutable int gpio_chip_path_calls_{0};
    mutable int pwm_frequency_hz_calls_{0};
};

}  // namespace alpacacore::test
