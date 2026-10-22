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

#include <alpacacore/vendor/zwo/zwo_eaf_sdk.h>

#include <mutex>
#include <string>
#include <vector>

namespace alpacacore::test {

/**
 * Thread-safe decorator over any ZWOEAFSDK: every call forwards to the inner
 * implementation under one mutex, the way a single-threaded vendor library
 * serialises its entry points. Same shape as LockedZWOSDK; a per-SDK class
 * rather than a shared template, because the interfaces share no signatures.
 *
 * Wraps all 16 entry points; the mutex is a leaf (the inner SDK never calls back into
 * this interface). No EAF call blocks, so there is no blocking-mode caveat (the fake's opt-in
 * delays are not used behind this decorator).
 */
class LockedZWOEAFSDK final : public vendor::zwo::ZWOEAFSDK {
public:
    using Info = vendor::zwo::ZWOEAFFocuserInfo;

    explicit LockedZWOEAFSDK(ZWOEAFSDK& inner) : inner_(inner) {}

    std::vector<Info> enumerate_focusers() override {
        return locked([&] { return inner_.enumerate_focusers(); });
    }
    bool get_focuser_info_by_id(int focuser_id, Info& info) override {
        return locked([&] { return inner_.get_focuser_info_by_id(focuser_id, info); });
    }
    bool get_focuser_info_by_index(int focuser_index, Info& info) override {
        return locked([&] { return inner_.get_focuser_info_by_index(focuser_index, info); });
    }
    void open_focuser(int focuser_id) override {
        locked([&] { inner_.open_focuser(focuser_id); });
    }
    void close_focuser(int focuser_id) override {
        locked([&] { inner_.close_focuser(focuser_id); });
    }
    bool is_moving(int focuser_id) override {
        return locked([&] { return inner_.is_moving(focuser_id); });
    }
    int get_position(int focuser_id) override {
        return locked([&] { return inner_.get_position(focuser_id); });
    }
    void move(int focuser_id, int position) override {
        locked([&] { inner_.move(focuser_id, position); });
    }
    void stop(int focuser_id) override {
        locked([&] { inner_.stop(focuser_id); });
    }
    int get_max_step(int focuser_id) override {
        return locked([&] { return inner_.get_max_step(focuser_id); });
    }
    void set_max_step(int focuser_id, int max_step) override {
        locked([&] { inner_.set_max_step(focuser_id, max_step); });
    }
    int get_step_range(int focuser_id) override {
        return locked([&] { return inner_.get_step_range(focuser_id); });
    }
    double get_temperature(int focuser_id) override {
        return locked([&] { return inner_.get_temperature(focuser_id); });
    }
    std::string get_serial_number(int focuser_id) override {
        return locked([&] { return inner_.get_serial_number(focuser_id); });
    }
    std::string get_firmware_version(int focuser_id) override {
        return locked([&] { return inner_.get_firmware_version(focuser_id); });
    }
    std::string get_sdk_version() override {
        return locked([&] { return inner_.get_sdk_version(); });
    }

private:
    template <typename Fn>
    auto locked(Fn&& fn) -> decltype(fn()) {
        std::lock_guard<std::mutex> lock(mutex_);
        return fn();
    }

    ZWOEAFSDK& inner_;
    std::mutex mutex_;
};

}  // namespace alpacacore::test
