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

#include <alpacacore/vendor/ioptron/ioptron_iguider_v4l2.h>

#include <map>
#include <memory>
#include <mutex>

namespace alpacacore::vendor::ioptron {

/**
 * Production IGuiderV4L2 over the V4L2 ioctls (memory-mapped streaming I/O).
 *
 * A handle is the open file descriptor. The handle table has its own mutex,
 * held only for lookups; each device has a mutex of its own that serialises
 * that device's calls, so one camera's read_frame() never delays another's.
 */
class IGuiderV4L2Wrapper final : public IGuiderV4L2 {
public:
    static IGuiderV4L2Wrapper& instance();

    std::vector<IGuiderDeviceInfo> enumerate() override;
    int open(const std::string& path) override;
    void close(int handle) override;
    void set_control(int handle, IGuiderControl control, std::int32_t value) override;
    void start_stream(int handle, int width, int height) override;
    void stop_stream(int handle) override;
    bool read_frame(int handle, std::vector<std::uint8_t>& luma, std::chrono::milliseconds timeout) override;

private:
    struct Device;

    IGuiderV4L2Wrapper() = default;
    ~IGuiderV4L2Wrapper();

    std::shared_ptr<Device> find(int handle) const;

    mutable std::mutex devices_mutex_;
    std::map<int, std::shared_ptr<Device>> devices_;
};

}  // namespace alpacacore::vendor::ioptron
