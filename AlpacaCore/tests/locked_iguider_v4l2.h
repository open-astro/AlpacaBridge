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

// Test-only decorator over the iGuider V4L2 seam: every forward runs under one
// leaf mutex, so the unlocked FakeIGuiderV4L2 can sit behind a driver whose
// connect and stream worker both call it, and a ThreadSanitizer report points
// at driver code rather than at the fake (decision 0015). The production
// wrapper serialises its own calls.

#include <alpacacore/vendor/ioptron/ioptron_iguider_v4l2.h>

#include <mutex>

namespace alpacacore::test {

class LockedIGuiderV4L2 final : public vendor::ioptron::IGuiderV4L2 {
public:
    explicit LockedIGuiderV4L2(vendor::ioptron::IGuiderV4L2& inner) : inner_(inner) {}

    std::vector<vendor::ioptron::IGuiderDeviceInfo> enumerate() override {
        auto lock = locked();
        return inner_.enumerate();
    }
    int open(const std::string& path) override {
        auto lock = locked();
        return inner_.open(path);
    }
    void close(int handle) override {
        auto lock = locked();
        inner_.close(handle);
    }
    void set_control(int handle, vendor::ioptron::IGuiderControl control, std::int32_t value) override {
        auto lock = locked();
        inner_.set_control(handle, control, value);
    }
    void start_stream(int handle, int width, int height) override {
        auto lock = locked();
        inner_.start_stream(handle, width, height);
    }
    void stop_stream(int handle) override {
        auto lock = locked();
        inner_.stop_stream(handle);
    }
    bool read_frame(int handle, std::vector<std::uint8_t>& luma, std::chrono::milliseconds timeout) override {
        auto lock = locked();
        return inner_.read_frame(handle, luma, timeout);
    }

private:
    std::unique_lock<std::mutex> locked() { return std::unique_lock<std::mutex>(mutex_); }

    vendor::ioptron::IGuiderV4L2& inner_;
    std::mutex mutex_;
};

}  // namespace alpacacore::test
