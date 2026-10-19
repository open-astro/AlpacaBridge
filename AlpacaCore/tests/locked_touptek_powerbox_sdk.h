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

#include <alpacacore/vendor/touptek/touptek_powerbox_sdk.h>

#include <mutex>
#include <string>

namespace alpacacore::test {

/**
 * Thread-safe decorator over any TouptekPowerboxSDK: every call forwards to the inner
 * implementation under one mutex, the way a single GPIO line request serialises its
 * writes. Same shape as LockedZWOEFWSDK; the getters are const in this interface, so
 * the mutex is mutable.
 *
 * Wraps all 9 entry points. is_open() is the one exception: its contract is
 * lock-free (issue #382), so it forwards WITHOUT the mutex and the inner object
 * must make it safe (FakeTouptekPowerboxSDK reads an atomic). Taking the lock
 * there would let a held open() stall get_connected(), which the real wrapper
 * never does. The mutex is a leaf (the inner object never calls back into this one).
 */
class LockedTouptekPowerboxSDK final : public vendor::touptek::TouptekPowerboxSDK {
public:
    using Port = vendor::touptek::StellaVitaPortConfig;

    explicit LockedTouptekPowerboxSDK(TouptekPowerboxSDK& inner) : inner_(inner) {}

    void open() override {
        locked([&] { inner_.open(); });
    }
    void close() override {
        locked([&] { inner_.close(); });
    }
    bool is_open() const override { return inner_.is_open(); }

    std::size_t port_count() const override {
        return locked([&] { return inner_.port_count(); });
    }
    Port port_config(std::size_t index) const override {
        return locked([&] { return inner_.port_config(index); });
    }
    int get_value(std::size_t index) const override {
        return locked([&] { return inner_.get_value(index); });
    }
    void set_value(std::size_t index, int value) override {
        locked([&] { inner_.set_value(index, value); });
    }
    std::string gpio_chip_path() const override {
        return locked([&] { return inner_.gpio_chip_path(); });
    }
    std::uint32_t pwm_frequency_hz() const override {
        return locked([&] { return inner_.pwm_frequency_hz(); });
    }

private:
    template <typename Fn>
    auto locked(Fn&& fn) const -> decltype(fn()) {
        std::lock_guard<std::mutex> lock(mutex_);
        return fn();
    }

    TouptekPowerboxSDK& inner_;
    mutable std::mutex mutex_;
};

}  // namespace alpacacore::test
