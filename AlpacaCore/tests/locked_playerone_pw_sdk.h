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

#include <alpacacore/vendor/playerone/playerone_pw_sdk.h>

#include <mutex>
#include <string>
#include <vector>

namespace alpacacore::test {

/**
 * Thread-safe decorator over any PlayerOnePWSDK: every call forwards to the
 * inner implementation under one mutex (issue #1256).
 *
 * FakePlayerOnePWSDK locks its own state, so the decorator is not needed to
 * keep the fake race-free; it serialises the SDK the way the real wrapper does
 * (one mutex around every POA*PW call). All 14 entry points are wrapped. The
 * mutex is a leaf: the inner SDK never calls back into this interface. No PW
 * call blocks, so unlike the camera decorator there is no blocking-mode caveat.
 */
class LockedPlayerOnePWSDK final : public vendor::playerone::PlayerOnePWSDK {
public:
    using Info = vendor::playerone::PlayerOnePWInfo;

    explicit LockedPlayerOnePWSDK(PlayerOnePWSDK& inner) : inner_(inner) {}

    std::vector<Info> enumerate_wheels() override {
        return locked([&] { return inner_.enumerate_wheels(); });
    }
    bool get_wheel_info_by_index(int wheel_index, Info& info) override {
        return locked([&] { return inner_.get_wheel_info_by_index(wheel_index, info); });
    }
    bool get_wheel_info_by_handle(int handle, Info& info) override {
        return locked([&] { return inner_.get_wheel_info_by_handle(handle, info); });
    }

    void open_wheel(int handle) override {
        locked([&] { inner_.open_wheel(handle); });
    }
    void close_wheel(int handle) override {
        locked([&] { inner_.close_wheel(handle); });
    }

    int get_position(int handle) override {
        return locked([&] { return inner_.get_position(handle); });
    }
    void goto_position(int handle, int position) override {
        locked([&] { inner_.goto_position(handle, position); });
    }
    bool is_moving(int handle) override {
        return locked([&] { return inner_.is_moving(handle); });
    }

    bool get_one_way(int handle) override {
        return locked([&] { return inner_.get_one_way(handle); });
    }
    void set_one_way(int handle, bool one_way) override {
        locked([&] { inner_.set_one_way(handle, one_way); });
    }

    std::string get_filter_alias(int handle, int position) override {
        return locked([&] { return inner_.get_filter_alias(handle, position); });
    }
    int get_focus_offset(int handle, int position) override {
        return locked([&] { return inner_.get_focus_offset(handle, position); });
    }

    void reset_wheel(int handle) override {
        locked([&] { inner_.reset_wheel(handle); });
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

    PlayerOnePWSDK& inner_;
    std::mutex mutex_;
};

}  // namespace alpacacore::test
