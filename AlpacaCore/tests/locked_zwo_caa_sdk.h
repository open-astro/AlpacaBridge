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

#include <alpacacore/vendor/zwo/zwo_caa_sdk.h>

#include <mutex>
#include <string>
#include <vector>

namespace alpacacore::test {

/**
 * Thread-safe decorator over any ZWOCAASDK: every call forwards to the inner
 * implementation under one mutex, the way a single-threaded vendor library
 * serialises its entry points. Same shape as LockedZWOSDK; a per-SDK class
 * rather than a shared template, because the interfaces share no signatures.
 *
 * Wraps all 20 entry points; the mutex is a leaf (the inner SDK never calls back into
 * this interface). No CAA call blocks, so there is no blocking-mode caveat.
 */
class LockedZWOCAASDK final : public vendor::zwo::ZWOCAASDK {
public:
    using Info = vendor::zwo::ZWOCAARotatorInfo;
    using Status = vendor::zwo::ZWOCAAMotionStatus;

    explicit LockedZWOCAASDK(ZWOCAASDK& inner) : inner_(inner) {}

    std::vector<Info> enumerate_rotators() override {
        return locked([&] { return inner_.enumerate_rotators(); });
    }
    bool get_rotator_info_by_id(int rotator_id, Info& info) override {
        return locked([&] { return inner_.get_rotator_info_by_id(rotator_id, info); });
    }
    bool get_rotator_info_by_index(int rotator_index, Info& info) override {
        return locked([&] { return inner_.get_rotator_info_by_index(rotator_index, info); });
    }
    void open_rotator(int rotator_id) override {
        locked([&] { inner_.open_rotator(rotator_id); });
    }
    void close_rotator(int rotator_id) override {
        locked([&] { inner_.close_rotator(rotator_id); });
    }
    Status get_motion_status(int rotator_id) override {
        return locked([&] { return inner_.get_motion_status(rotator_id); });
    }
    double get_degree(int rotator_id) override {
        return locked([&] { return inner_.get_degree(rotator_id); });
    }
    void move_relative(int rotator_id, double angle) override {
        locked([&] { inner_.move_relative(rotator_id, angle); });
    }
    void move_absolute(int rotator_id, double angle) override {
        locked([&] { inner_.move_absolute(rotator_id, angle); });
    }
    void move_mechanical(int rotator_id, double angle) override {
        locked([&] { inner_.move_mechanical(rotator_id, angle); });
    }
    void stop(int rotator_id) override {
        locked([&] { inner_.stop(rotator_id); });
    }
    void sync_degree(int rotator_id, double angle) override {
        locked([&] { inner_.sync_degree(rotator_id, angle); });
    }
    double get_max_degree(int rotator_id) override {
        return locked([&] { return inner_.get_max_degree(rotator_id); });
    }
    double get_temperature(int rotator_id) override {
        return locked([&] { return inner_.get_temperature(rotator_id); });
    }
    bool get_reverse(int rotator_id) override {
        return locked([&] { return inner_.get_reverse(rotator_id); });
    }
    void set_reverse(int rotator_id, bool reverse) override {
        locked([&] { inner_.set_reverse(rotator_id, reverse); });
    }
    std::string get_serial_number(int rotator_id) override {
        return locked([&] { return inner_.get_serial_number(rotator_id); });
    }
    std::string get_firmware_version(int rotator_id) override {
        return locked([&] { return inner_.get_firmware_version(rotator_id); });
    }
    std::string get_rotator_type(int rotator_id) override {
        return locked([&] { return inner_.get_rotator_type(rotator_id); });
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

    ZWOCAASDK& inner_;
    std::mutex mutex_;
};

}  // namespace alpacacore::test
