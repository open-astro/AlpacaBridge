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

// Scripted fake of the iGuider V4L2 seam (ioptron_iguider_v4l2.h), for
// hardware-free connects. One iGuider at /dev/video0 by default; it streams a
// frame every frame_interval() while streaming, whose luma at (x, y) is
// pixel_value(x, y), so a test can check a subframe crop, except pixel (0, 0),
// which carries exposure_marker() of the exposure the frame was taken at.
//
// Like the camera, an exposure write reaches the frames late: the 4th frame
// delivered after the write is the first at the new value (measured on the rig,
// 2026-10-21: the 3rd or 4th; the fake takes the slower). A write before
// start_stream() applies from the first frame.
//
// Thread rules: the seam methods are not locked here; wrap the fake in
// LockedIGuiderV4L2 wherever a driver runs on it (locked_iguider_v4l2.h). The
// knobs a test turns while the driver's stream worker runs (stall, unplug,
// open delay) and the counters it reads are atomics; close_delay, like
// open_delay, is set before the driver connects.
//
// KNOWN PARITY GAPS (the fake against uvcvideo on the iGuider):
//  * Frames arrive every frame_interval() (5 ms by default) whatever the
//    exposure ticks; the camera runs at 9 fps and slows to the exposure.
//    The driver's frame selection is still exercised, since it is keyed on
//    arrival time and on frames counted since a control write.
//  * read_frame() waits at most one frame interval when stalled, not the
//    whole timeout, so a disconnect never waits on the fake.
//  * A gain write takes effect at once; only the exposure write is delayed.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/ioptron/ioptron_iguider_v4l2.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace alpacacore::test {

class FakeIGuiderV4L2 final : public vendor::ioptron::IGuiderV4L2 {
public:
    using Control = vendor::ioptron::IGuiderControl;
    static constexpr int kWidth = 1280;
    static constexpr int kHeight = 960;
    static constexpr int kHandle = 7;

    static std::uint8_t pixel_value(int x, int y) { return static_cast<std::uint8_t>((x + 3 * y) % 251); }
    /// What pixel (0, 0) reads for a frame taken at exposure tick @p ticks.
    static std::uint8_t exposure_marker(std::int32_t ticks) { return static_cast<std::uint8_t>(ticks & 0xFF); }
    /// Frames delivered at the old exposure after an exposure write, before the new one.
    static constexpr int kExposureLagFrames = 3;

    // ---- Knobs (set before connecting unless noted) ------------------------

    std::vector<vendor::ioptron::IGuiderDeviceInfo> devices{
        {"/dev/video0", "iOptron iGuider External: iOptr", "usb-0000:01:02.0-1"}};
    /// Seam calls (by name) that throw DriverException.
    std::set<std::string> throw_from;
    /// Atomic: while true, read_frame() delivers nothing.
    std::atomic<bool> stalled{false};
    /// Atomic: while true, enumerate() finds nothing and every other call but
    /// close() fails as a removed camera does (ENODEV, thrown as
    /// IGuiderDeviceGone like the wrapper).
    std::atomic<bool> unplugged{false};
    /// Explicit hook (decision 0015, point 5): open() sleeps this long.
    std::chrono::milliseconds open_delay{0};
    /// Explicit hook (decision 0015, point 5): close() sleeps this long before
    /// it counts the close, so a test can hold open the window between the
    /// driver publishing a lost link and the transport actually closing (#1487).
    std::chrono::milliseconds close_delay{0};
    std::chrono::milliseconds frame_interval{5};
    /// Atomic: this many reads return no frame at once, as the wrapper does for
    /// an incomplete or error-flagged buffer (it drops it without waiting).
    std::atomic<int> drop_frames{0};

    // ---- Counters (atomic, safe to read while the driver runs) -------------

    std::atomic<int> opens{0};
    std::atomic<int> closes{0};
    std::atomic<int> stream_starts{0};
    std::atomic<int> stream_stops{0};
    std::atomic<int> frames{0};
    std::array<std::atomic<std::int32_t>, 3> last_control{};
    std::atomic<int> control_writes{0};

    std::int32_t control(Control c) const { return last_control[static_cast<std::size_t>(c)].load(); }
    bool streaming() const { return streaming_.load(); }
    int open_handles() const { return opens.load() - closes.load(); }

    bool wait_for_frames(int count, std::chrono::milliseconds timeout) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (frames.load() < count) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    // ---- Seam ---------------------------------------------------------------

    std::vector<vendor::ioptron::IGuiderDeviceInfo> enumerate() override {
        hit("enumerate");
        return unplugged ? std::vector<vendor::ioptron::IGuiderDeviceInfo>{} : devices;
    }

    int open(const std::string& path) override {
        hit("open");
        if (open_delay.count() > 0) std::this_thread::sleep_for(open_delay);
        const vendor::ioptron::IGuiderDeviceInfo* known = nullptr;
        for (const auto& d : devices) {
            if (d.path == path) known = &d;
        }
        if (known == nullptr) fail("open of " + path + " failed: No such file or directory");
        // As the wrapper: the card name is checked at open (decision 0009, point 6).
        if (known->card.find("iGuider") == std::string::npos) {
            fail(path + " is not an iOptron iGuider (V4L2 card \"" + known->card + "\")");
        }
        if (open_) fail("open of " + path + " failed: Device or resource busy");
        open_ = true;
        ++opens;
        return kHandle;
    }

    void close(int handle) override {
        if (handle != kHandle || !open_) return;  // unknown handles are ignored, as by the wrapper
        if (close_delay.count() > 0) std::this_thread::sleep_for(close_delay);
        streaming_ = false;
        open_ = false;
        ++closes;
    }

    void set_control(int handle, Control control, std::int32_t value) override {
        hit("set_control");
        check(handle);
        last_control[static_cast<std::size_t>(control)] = value;
        ++control_writes;
        if (control == Control::ExposureAbsolute) {
            pending_ticks_ = value;
            lag_frames_left_ = streaming_ ? kExposureLagFrames : 0;
        }
    }

    void start_stream(int handle, int width, int height) override {
        hit("start_stream");
        check(handle);
        if (width != kWidth || height != kHeight) fail("did not accept the requested mode");
        streaming_ = true;
        lag_frames_left_ = 0;  // a value written before the stream starts applies from its first frame
        ++stream_starts;
    }

    void stop_stream(int handle) override {
        hit("stop_stream");
        check(handle);
        if (streaming_.exchange(false)) ++stream_stops;
    }

    bool read_frame(int handle, std::vector<std::uint8_t>& luma, std::chrono::milliseconds timeout) override {
        hit("read_frame");
        check(handle);
        // Harsher than "no frame": the wrapper throws when it is not streaming.
        if (!streaming_) fail("is not streaming");
        if (drop_frames > 0) {
            --drop_frames;
            return false;
        }
        std::this_thread::sleep_for(std::min(timeout, frame_interval));
        if (stalled) return false;
        if (luma.size() != static_cast<std::size_t>(kWidth) * kHeight) {
            luma.resize(static_cast<std::size_t>(kWidth) * kHeight);
            for (int y = 0; y < kHeight; ++y) {
                for (int x = 0; x < kWidth; ++x) luma[static_cast<std::size_t>(y) * kWidth + x] = pixel_value(x, y);
            }
        }
        if (lag_frames_left_ > 0) {
            --lag_frames_left_;
        } else {
            effective_ticks_ = pending_ticks_;
        }
        luma[0] = exposure_marker(effective_ticks_);
        ++frames;
        return true;
    }

private:
    bool open_ = false;
    std::atomic<bool> streaming_{false};
    // The exposure the next frame is taken at, and the write still on its way.
    std::int32_t effective_ticks_ = 0;
    std::int32_t pending_ticks_ = 0;
    int lag_frames_left_ = 0;

    [[noreturn]] static void fail(const std::string& what) {
        throw AlpacaException("iGuider " + what, AlpacaError::DriverException);
    }

    void hit(const std::string& name) {
        if (unplugged && name != "enumerate") {
            throw vendor::ioptron::IGuiderDeviceGone("iGuider " + name + " failed: No such device",
                                                     AlpacaError::DriverException);
        }
        if (throw_from.count(name) != 0) fail(name + " failed (scripted)");
    }

    void check(int handle) const {
        if (handle != kHandle || !open_) fail("handle is not open");
    }
};

}  // namespace alpacacore::test
