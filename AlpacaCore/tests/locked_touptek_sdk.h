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

#include <alpacacore/vendor/touptek/touptek_sdk_wrapper.h>

#include <mutex>
#include <string>
#include <vector>

namespace alpacacore::test {

/**
 * Thread-safe decorator over any ToupTekSDK — every call forwards to the
 * inner implementation under one mutex.
 *
 * Exists for the concurrency stress harness (issue #101): FakeToupTekSDK is
 * deliberately NOT thread-hardened (single-connect-path tests don't need it),
 * but the stress tests hammer one driver from many threads, so unguarded fake
 * state would light up ThreadSanitizer with races in TEST code and drown out
 * the driver races the harness exists to catch. The real SDK wrapper
 * serializes internally, so production drivers never need this.
 *
 * The mutex is a leaf: the inner SDK never calls back into this interface.
 */
class LockedToupTekSDK : public vendor::touptek::ToupTekSDK {
public:
    explicit LockedToupTekSDK(ToupTekSDK& inner) : inner_(inner) {}

    std::string get_sdk_version() override {
        return locked([&] { return inner_.get_sdk_version(); });
    }

    std::vector<vendor::touptek::ToupCameraInfo> enumerate_cameras() override {
        return locked([&] { return inner_.enumerate_cameras(); });
    }
    HToupcam open_camera_by_id(const std::string& id) override {
        return locked([&] { return inner_.open_camera_by_id(id); });
    }
    void close_camera(HToupcam h) override {
        locked([&] { inner_.close_camera(h); });
    }

    void start_pull_mode(HToupcam h, void (*cb)(unsigned, void*), void* ctx) override {
        locked([&] { inner_.start_pull_mode(h, cb, ctx); });
    }
    void stop(HToupcam h) override {
        locked([&] { inner_.stop(h); });
    }
    void put_trigger_mode(HToupcam h, int mode) override {
        locked([&] { inner_.put_trigger_mode(h, mode); });
    }
    void trigger(HToupcam h, unsigned short n) override {
        locked([&] { inner_.trigger(h, n); });
    }
    bool wait_image(HToupcam h, unsigned timeout_ms, void* buf, int bits, int pitch, unsigned& w,
                    unsigned& hgt) override {
        return locked([&] { return inner_.wait_image(h, timeout_ms, buf, bits, pitch, w, hgt); });
    }

    vendor::touptek::ToupExpRange get_exposure_range(HToupcam h) override {
        return locked([&] { return inner_.get_exposure_range(h); });
    }
    unsigned get_exposure_us(HToupcam h) override {
        return locked([&] { return inner_.get_exposure_us(h); });
    }
    void put_exposure_us(HToupcam h, unsigned us) override {
        locked([&] { inner_.put_exposure_us(h, us); });
    }
    void put_auto_exposure(HToupcam h, bool en) override {
        locked([&] { inner_.put_auto_exposure(h, en); });
    }
    vendor::touptek::ToupGainRange get_gain_range(HToupcam h) override {
        return locked([&] { return inner_.get_gain_range(h); });
    }
    unsigned short get_gain(HToupcam h) override {
        return locked([&] { return inner_.get_gain(h); });
    }
    void put_gain(HToupcam h, unsigned short g) override {
        locked([&] { inner_.put_gain(h, g); });
    }

    vendor::touptek::ToupROIFormat get_roi(HToupcam h) override {
        return locked([&] { return inner_.get_roi(h); });
    }
    void put_roi(HToupcam h, unsigned x, unsigned y, unsigned w, unsigned hgt) override {
        locked([&] { inner_.put_roi(h, x, y, w, hgt); });
    }
    void put_binning(HToupcam h, int bin) override {
        locked([&] { inner_.put_binning(h, bin); });
    }
    int get_binning(HToupcam h) override {
        return locked([&] { return inner_.get_binning(h); });
    }
    void put_bitdepth(HToupcam h, int bd) override {
        locked([&] { inner_.put_bitdepth(h, bd); });
    }
    int get_bitdepth(HToupcam h) override {
        return locked([&] { return inner_.get_bitdepth(h); });
    }
    void put_raw(HToupcam h, int en) override {
        locked([&] { inner_.put_raw(h, en); });
    }
    int get_option(HToupcam h, unsigned opt) override {
        return locked([&] { return inner_.get_option(h, opt); });
    }
    void put_option(HToupcam h, unsigned opt, int v) override {
        locked([&] { inner_.put_option(h, opt, v); });
    }

    void get_size(HToupcam h, int& w, int& hgt) override {
        locked([&] { inner_.get_size(h, w, hgt); });
    }
    void get_final_size(HToupcam h, int& w, int& hgt) override {
        locked([&] { inner_.get_final_size(h, w, hgt); });
    }
    void get_raw_format(HToupcam h, unsigned& fourcc, unsigned& bpp) override {
        locked([&] { inner_.get_raw_format(h, fourcc, bpp); });
    }

    int get_temperature_deciC(HToupcam h) override {
        return locked([&] { return inner_.get_temperature_deciC(h); });
    }
    void put_tec_enable(HToupcam h, bool en) override {
        locked([&] { inner_.put_tec_enable(h, en); });
    }
    bool get_tec_enable(HToupcam h) override {
        return locked([&] { return inner_.get_tec_enable(h); });
    }
    void put_tec_target_deciC(HToupcam h, int t) override {
        locked([&] { inner_.put_tec_target_deciC(h, t); });
    }
    int get_tec_target_deciC(HToupcam h) override {
        return locked([&] { return inner_.get_tec_target_deciC(h); });
    }
    int get_tec_voltage_deciV(HToupcam h) override {
        return locked([&] { return inner_.get_tec_voltage_deciV(h); });
    }
    int get_tec_voltage_max_deciV(HToupcam h) override {
        return locked([&] { return inner_.get_tec_voltage_max_deciV(h); });
    }

    int get_high_fullwell(HToupcam h) override {
        return locked([&] { return inner_.get_high_fullwell(h); });
    }
    void put_high_fullwell(HToupcam h, bool en) override {
        locked([&] { inner_.put_high_fullwell(h, en); });
    }
    int get_cg(HToupcam h) override {
        return locked([&] { return inner_.get_cg(h); });
    }
    void put_cg(HToupcam h, int cg) override {
        locked([&] { inner_.put_cg(h, cg); });
    }
    int get_blacklevel(HToupcam h) override {
        return locked([&] { return inner_.get_blacklevel(h); });
    }
    void put_blacklevel(HToupcam h, int v) override {
        locked([&] { inner_.put_blacklevel(h, v); });
    }
    int get_blacklevel_max(HToupcam h, int bits) override {
        return locked([&] { return inner_.get_blacklevel_max(h, bits); });
    }

    int get_heat_max(HToupcam h) override {
        return locked([&] { return inner_.get_heat_max(h); });
    }
    int get_heat(HToupcam h) override {
        return locked([&] { return inner_.get_heat(h); });
    }
    void put_heat(HToupcam h, int level) override {
        locked([&] { inner_.put_heat(h, level); });
    }
    int get_fan(HToupcam h) override {
        return locked([&] { return inner_.get_fan(h); });
    }
    void put_fan(HToupcam h, int speed) override {
        locked([&] { inner_.put_fan(h, speed); });
    }
    int get_taillight(HToupcam h) override {
        return locked([&] { return inner_.get_taillight(h); });
    }
    void put_taillight(HToupcam h, bool on) override {
        locked([&] { inner_.put_taillight(h, on); });
    }

    std::string get_serial_number(HToupcam h) override {
        return locked([&] { return inner_.get_serial_number(h); });
    }
    std::string get_firmware_version(HToupcam h) override {
        return locked([&] { return inner_.get_firmware_version(h); });
    }
    void get_pixel_size(HToupcam h, unsigned idx, float& x, float& y) override {
        locked([&] { inner_.get_pixel_size(h, idx, x, y); });
    }

    void pulse_guide(HToupcam h, vendor::touptek::ToupGuideDirection dir, unsigned ms) override {
        locked([&] { inner_.pulse_guide(h, dir, ms); });
    }
    bool is_guiding(HToupcam h) override {
        return locked([&] { return inner_.is_guiding(h); });
    }

    std::vector<vendor::touptek::ToupFocuserInfo> enumerate_focusers() override {
        return locked([&] { return inner_.enumerate_focusers(); });
    }
    HToupcam open_focuser_by_id(const std::string& id) override {
        return locked([&] { return inner_.open_focuser_by_id(id); });
    }
    void close_focuser(HToupcam h) override {
        locked([&] { inner_.close_focuser(h); });
    }
    void aaf_set(HToupcam h, int action, int value, const char* ctx) override {
        locked([&] { inner_.aaf_set(h, action, value, ctx); });
    }
    int aaf_get(HToupcam h, int action, const char* ctx) override {
        return locked([&] { return inner_.aaf_get(h, action, ctx); });
    }
    int aaf_range(HToupcam h, int range_action, int target_action, const char* ctx) override {
        return locked([&] { return inner_.aaf_range(h, range_action, target_action, ctx); });
    }

    std::vector<vendor::touptek::ToupFilterWheelInfo> enumerate_filter_wheels() override {
        return locked([&] { return inner_.enumerate_filter_wheels(); });
    }
    HToupcam open_filter_wheel_by_id(const std::string& id) override {
        return locked([&] { return inner_.open_filter_wheel_by_id(id); });
    }
    void close_filter_wheel(HToupcam h) override {
        locked([&] { inner_.close_filter_wheel(h); });
    }
    int get_filter_wheel_slot_count(HToupcam h) override {
        return locked([&] { return inner_.get_filter_wheel_slot_count(h); });
    }
    void set_filter_wheel_slot_count(HToupcam h, int slots) override {
        locked([&] { inner_.set_filter_wheel_slot_count(h, slots); });
    }
    void reset_filter_wheel(HToupcam h) override {
        locked([&] { inner_.reset_filter_wheel(h); });
    }
    int get_filter_wheel_position(HToupcam h) override {
        return locked([&] { return inner_.get_filter_wheel_position(h); });
    }
    void set_filter_wheel_position(HToupcam h, int pos) override {
        locked([&] { inner_.set_filter_wheel_position(h, pos); });
    }

private:
    template <typename Fn>
    auto locked(Fn&& fn) -> decltype(fn()) {
        std::lock_guard<std::mutex> lock(mutex_);
        return fn();
    }

    ToupTekSDK& inner_;
    std::mutex mutex_;
};

}  // namespace alpacacore::test
