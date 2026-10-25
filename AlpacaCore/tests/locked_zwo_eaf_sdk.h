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

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace alpacacore::test {

/**
 * Thread-safe decorator over any ZWOEAFSDK: every call forwards to the inner
 * implementation under one mutex, the way a single-threaded vendor library
 * serialises its entry points. Same shape as LockedZWOSDK; a per-SDK class
 * rather than a shared template, because the interfaces share no signatures.
 *
 * Wraps all 18 entry points, each under the mutex (decision 0015 rule 2). The mutex is recursive
 * because with_link() holds it across its body, which calls back into this decorator's per-device
 * methods on the same thread (issue #1500); the body may also take the driver's leaf mutex_, which
 * the driver never holds across an SDK call, so no order inverts. Every other entry point holds the
 * mutex across the inner call only (the inner SDK never calls back into this interface). No EAF
 * call blocks, so there is no blocking-mode caveat (the fake's opt-in delays are not used behind
 * this decorator).
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
    vendor::zwo::SharedOpenTable::Opened open_focuser_link(int focuser_id) override {
        return locked([&] { return inner_.open_focuser_link(focuser_id); });
    }
    std::optional<vendor::zwo::SharedOpenTable::Opened> open_focuser_candidate(int focuser_id) override {
        return locked([&] { return inner_.open_focuser_candidate(focuser_id); });
    }
    void close_focuser_link(int focuser_id, std::uint64_t link) override {
        locked([&] { inner_.close_focuser_link(focuser_id, link); });
    }
    void with_link(int focuser_id, std::uint64_t link, const std::function<void()>& fn) override {
        locked([&] { inner_.with_link(focuser_id, link, fn); });
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
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return fn();
    }

    ZWOEAFSDK& inner_;
    std::recursive_mutex mutex_;
};

}  // namespace alpacacore::test
