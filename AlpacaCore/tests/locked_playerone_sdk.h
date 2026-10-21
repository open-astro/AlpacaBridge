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

#include <alpacacore/vendor/playerone/playerone_sdk_wrapper.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace alpacacore::test {

/**
 * Thread-safe decorator over any PlayerOneSDK: every call forwards to the
 * inner implementation under one mutex (issue #1256).
 *
 * FakePlayerOneSDK locks its own state, so the decorator is not needed to keep
 * the fake race-free; it serialises the SDK the way a single-threaded vendor
 * library would, for tests that want that shape. All 39 entry points are
 * wrapped. The mutex is a leaf: the inner SDK never calls back into this
 * interface. The defaulted arguments repeat the interface's defaults, so a call
 * through the decorator type behaves like one through the interface.
 *
 * Do not use it over a FakePlayerOneSDK configured to block: get_image_data()
 * waits in hold_image_data mode (up to its timeout, or until stop_exposure()
 * releases it when set_hold_image_data was given ignore_timeout), and behind
 * this mutex the abort's stop_exposure() would queue behind the very read it
 * is meant to wake.
 */
class LockedPlayerOneSDK final : public vendor::playerone::PlayerOneSDK {
public:
    using PlayerOneCameraInfo = vendor::playerone::PlayerOneCameraInfo;
    using PlayerOneConfigCaps = vendor::playerone::PlayerOneConfigCaps;
    using PlayerOneImageFormat = vendor::playerone::PlayerOneImageFormat;
    using PlayerOneCameraState = vendor::playerone::PlayerOneCameraState;
    using PlayerOneGuideDirection = vendor::playerone::PlayerOneGuideDirection;

    explicit LockedPlayerOneSDK(PlayerOneSDK& inner) : inner_(inner) {}

    std::string get_sdk_version() override {
        return locked([&] { return inner_.get_sdk_version(); });
    }
    std::vector<PlayerOneCameraInfo> enumerate_cameras() override {
        return locked([&] { return inner_.enumerate_cameras(); });
    }
    void open_camera(int camera_id) override {
        locked([&] { inner_.open_camera(camera_id); });
    }
    void init_camera(int camera_id) override {
        locked([&] { inner_.init_camera(camera_id); });
    }
    void close_camera(int camera_id) override {
        locked([&] { inner_.close_camera(camera_id); });
    }
    PlayerOneCameraInfo get_camera_properties_by_id(int camera_id) override {
        return locked([&] { return inner_.get_camera_properties_by_id(camera_id); });
    }
    PlayerOneConfigCaps probe_config_caps(int camera_id) override {
        return locked([&] { return inner_.probe_config_caps(camera_id); });
    }
    long get_config_int(int camera_id, int config_id, bool* is_auto = nullptr) override {
        return locked([&] { return inner_.get_config_int(camera_id, config_id, is_auto); });
    }
    void set_config_int(int camera_id, int config_id, long value, bool is_auto = false) override {
        locked([&] { inner_.set_config_int(camera_id, config_id, value, is_auto); });
    }
    double get_config_float(int camera_id, int config_id, bool* is_auto = nullptr) override {
        return locked([&] { return inner_.get_config_float(camera_id, config_id, is_auto); });
    }
    bool get_config_bool(int camera_id, int config_id, bool* is_auto = nullptr) override {
        return locked([&] { return inner_.get_config_bool(camera_id, config_id, is_auto); });
    }
    void set_config_float(int camera_id, int config_id, double value, bool is_auto = false) override {
        locked([&] { inner_.set_config_float(camera_id, config_id, value, is_auto); });
    }
    void set_config_bool(int camera_id, int config_id, bool value, bool is_auto = false) override {
        locked([&] { inner_.set_config_bool(camera_id, config_id, value, is_auto); });
    }
    PlayerOneImageFormat get_image_format(int camera_id) override {
        return locked([&] { return inner_.get_image_format(camera_id); });
    }
    void set_image_format(int camera_id, PlayerOneImageFormat format) override {
        locked([&] { inner_.set_image_format(camera_id, format); });
    }
    void get_image_size(int camera_id, int& width, int& height) override {
        locked([&] { inner_.get_image_size(camera_id, width, height); });
    }
    void set_image_size(int camera_id, int width, int height) override {
        locked([&] { inner_.set_image_size(camera_id, width, height); });
    }
    void get_image_start_pos(int camera_id, int& start_x, int& start_y) override {
        locked([&] { inner_.get_image_start_pos(camera_id, start_x, start_y); });
    }
    void set_image_start_pos(int camera_id, int start_x, int start_y) override {
        locked([&] { inner_.set_image_start_pos(camera_id, start_x, start_y); });
    }
    int get_image_bin(int camera_id) override {
        return locked([&] { return inner_.get_image_bin(camera_id); });
    }
    void set_image_bin(int camera_id, int bin) override {
        locked([&] { inner_.set_image_bin(camera_id, bin); });
    }
    void start_exposure(int camera_id, bool single_frame) override {
        locked([&] { inner_.start_exposure(camera_id, single_frame); });
    }
    void stop_exposure(int camera_id) override {
        locked([&] { inner_.stop_exposure(camera_id); });
    }
    PlayerOneCameraState get_camera_state(int camera_id) override {
        return locked([&] { return inner_.get_camera_state(camera_id); });
    }
    bool image_ready(int camera_id) override {
        return locked([&] { return inner_.image_ready(camera_id); });
    }
    bool get_image_data(int camera_id, std::uint8_t* buffer, std::size_t buffer_size, int timeout_ms) override {
        return locked([&] { return inner_.get_image_data(camera_id, buffer, buffer_size, timeout_ms); });
    }
    void pulse_guide_on(int camera_id, PlayerOneGuideDirection direction) override {
        locked([&] { inner_.pulse_guide_on(camera_id, direction); });
    }
    void pulse_guide_off(int camera_id, PlayerOneGuideDirection direction) override {
        locked([&] { inner_.pulse_guide_off(camera_id, direction); });
    }
    double get_temperature_c(int camera_id) override {
        return locked([&] { return inner_.get_temperature_c(camera_id); });
    }
    bool get_cooler_on(int camera_id) override {
        return locked([&] { return inner_.get_cooler_on(camera_id); });
    }
    void set_cooler_on(int camera_id, bool on) override {
        locked([&] { inner_.set_cooler_on(camera_id, on); });
    }
    int get_target_temp_c(int camera_id) override {
        return locked([&] { return inner_.get_target_temp_c(camera_id); });
    }
    void set_target_temp_c(int camera_id, int target_c) override {
        locked([&] { inner_.set_target_temp_c(camera_id, target_c); });
    }
    int get_cooler_power_percent(int camera_id) override {
        return locked([&] { return inner_.get_cooler_power_percent(camera_id); });
    }
    double get_egain(int camera_id) override {
        return locked([&] { return inner_.get_egain(camera_id); });
    }
    int get_heater_power_percent(int camera_id) override {
        return locked([&] { return inner_.get_heater_power_percent(camera_id); });
    }
    void set_heater_power_percent(int camera_id, int percent) override {
        locked([&] { inner_.set_heater_power_percent(camera_id, percent); });
    }
    int get_fan_power_percent(int camera_id) override {
        return locked([&] { return inner_.get_fan_power_percent(camera_id); });
    }
    void set_fan_power_percent(int camera_id, int percent) override {
        locked([&] { inner_.set_fan_power_percent(camera_id, percent); });
    }

private:
    template <typename Fn>
    auto locked(Fn&& fn) -> decltype(fn()) {
        std::lock_guard<std::mutex> lock(mutex_);
        return fn();
    }

    PlayerOneSDK& inner_;
    std::mutex mutex_;
};

}  // namespace alpacacore::test
