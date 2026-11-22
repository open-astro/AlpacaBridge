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

#include <alpacacore/vendor/gphoto/gphoto_sdk_wrapper.h>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace alpacacore::test {

/**
 * Thread-safe decorator over any GPhotoSDK -- every call forwards to the
 * inner implementation under one mutex, except get_gphoto_version(), which has
 * its own (see below). Mirrors LockedToupTekSDK.
 *
 * Exists for the concurrency stress harness (issue #489, following #241/#101):
 * FakeGPhotoSDK is deliberately NOT thread-hardened, but the stress tests
 * hammer one driver from many threads, so unguarded fake state would light
 * up ThreadSanitizer with races in TEST code rather than the driver races
 * the harness exists to catch. The real SDK wrapper serializes internally,
 * so production drivers never need this.
 *
 * The mutex is a leaf: the inner SDK never calls back into this interface.
 *
 * Since open-astro#2148 get_gphoto_version() takes its own version_mutex_,
 * because GPhotoSDKWrapper::get_gphoto_version() reads gp_library_version()
 * with no lock: the SDK-version hook overtakes an in-flight open_camera(), as
 * in production. That is sound only because the fake's get_gphoto_version()
 * reads just its gphoto_version member (the LockedQHYSDK get_sdk_version()
 * shape, #1646).
 */
class LockedGPhotoSDK : public vendor::gphoto::GPhotoSDK {
public:
    explicit LockedGPhotoSDK(vendor::gphoto::GPhotoSDK& inner) : inner_(inner) {}

    std::vector<vendor::gphoto::GPhotoCameraInfo> enumerate_cameras() override {
        return locked([&] { return inner_.enumerate_cameras(); });
    }

    int open_camera(const std::string& model, const std::string& port) override {
        return locked([&] { return inner_.open_camera(model, port); });
    }

    void close_camera(int handle) override {
        locked([&] { inner_.close_camera(handle); });
    }

    std::string get_gphoto_version() override {
        // open-astro#2148: its own mutex, like the real wrapper's lock-free version read, so the hook never queues
        // behind an in-flight open_camera() (the "cheap and non-blocking" hook contract).
        std::lock_guard<std::mutex> lock(version_mutex_);
        return inner_.get_gphoto_version();
    }

    std::string get_camera_summary(int handle) override {
        return locked([&] { return inner_.get_camera_summary(handle); });
    }

    bool has_widget(int handle, const std::string& name) override {
        return locked([&] { return inner_.has_widget(handle, name); });
    }

    std::vector<std::string> get_choices(int handle, const std::string& name) override {
        return locked([&] { return inner_.get_choices(handle, name); });
    }

    std::string get_choice_value(int handle, const std::string& name) override {
        return locked([&] { return inner_.get_choice_value(handle, name); });
    }

    void set_choice_value(int handle, const std::string& name, const std::string& value) override {
        locked([&] { inner_.set_choice_value(handle, name, value); });
    }

    bool get_toggle_value(int handle, const std::string& name) override {
        return locked([&] { return inner_.get_toggle_value(handle, name); });
    }

    void set_toggle_value(int handle, const std::string& name, bool on) override {
        locked([&] { inner_.set_toggle_value(handle, name, on); });
    }

    std::string get_text_value(int handle, const std::string& name) override {
        return locked([&] { return inner_.get_text_value(handle, name); });
    }

    vendor::gphoto::GPhotoCaptureResult capture_and_download(int handle) override {
        return locked([&] { return inner_.capture_and_download(handle); });
    }

    void drain_events(int handle, std::chrono::milliseconds budget) override {
        locked([&] { inner_.drain_events(handle, budget); });
    }

    std::optional<vendor::gphoto::GPhotoCaptureResult> poll_bulb_file_and_download(
        int handle, std::chrono::milliseconds timeout) override {
        return locked([&] { return inner_.poll_bulb_file_and_download(handle, timeout); });
    }

private:
    template <typename Fn>
    auto locked(Fn&& fn) -> decltype(fn()) {
        std::lock_guard<std::mutex> lock(mutex_);
        return fn();
    }

    vendor::gphoto::GPhotoSDK& inner_;
    std::mutex mutex_;
    std::mutex version_mutex_;
};

}  // namespace alpacacore::test
