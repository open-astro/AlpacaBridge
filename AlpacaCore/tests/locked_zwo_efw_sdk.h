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

#include <alpacacore/vendor/zwo/zwo_efw_sdk.h>

#include <mutex>
#include <string>
#include <vector>

namespace alpacacore::test {

/**
 * Thread-safe decorator over any ZWOEFWSDK: every call forwards to the inner
 * implementation under one mutex, the way a single-threaded vendor library
 * serialises its entry points. Same shape as LockedZWOSDK; a per-SDK class
 * rather than a shared template, because the interfaces share no signatures.
 *
 * Wraps all 14 entry points; the mutex is a leaf (the inner SDK never calls back into
 * this interface). No EFW call blocks, so there is no blocking-mode caveat.
 */
class LockedZWOEFWSDK final : public vendor::zwo::ZWOEFWSDK {
public:
    using Info = vendor::zwo::ZWOEFWInfo;

    explicit LockedZWOEFWSDK(ZWOEFWSDK& inner) : inner_(inner) {}

    std::vector<Info> enumerate_wheels() override {
        return locked([&] { return inner_.enumerate_wheels(); });
    }
    bool get_wheel_info_by_id(int wheel_id, Info& info) override {
        return locked([&] { return inner_.get_wheel_info_by_id(wheel_id, info); });
    }
    bool get_wheel_info_by_index(int wheel_index, Info& info) override {
        return locked([&] { return inner_.get_wheel_info_by_index(wheel_index, info); });
    }
    void open_wheel(int wheel_id) override {
        locked([&] { inner_.open_wheel(wheel_id); });
    }
    void close_wheel(int wheel_id) override {
        locked([&] { inner_.close_wheel(wheel_id); });
    }
    int get_position(int wheel_id) override {
        return locked([&] { return inner_.get_position(wheel_id); });
    }
    void set_position(int wheel_id, int position) override {
        locked([&] { inner_.set_position(wheel_id, position); });
    }
    void set_unidirectional(int wheel_id, bool enabled) override {
        locked([&] { inner_.set_unidirectional(wheel_id, enabled); });
    }
    bool get_unidirectional(int wheel_id) override {
        return locked([&] { return inner_.get_unidirectional(wheel_id); });
    }
    void calibrate(int wheel_id) override {
        locked([&] { inner_.calibrate(wheel_id); });
    }
    int get_hardware_error_code(int wheel_id) override {
        return locked([&] { return inner_.get_hardware_error_code(wheel_id); });
    }
    std::string get_serial_number(int wheel_id) override {
        return locked([&] { return inner_.get_serial_number(wheel_id); });
    }
    std::string get_firmware_version(int wheel_id) override {
        return locked([&] { return inner_.get_firmware_version(wheel_id); });
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

    ZWOEFWSDK& inner_;
    std::mutex mutex_;
};

}  // namespace alpacacore::test
