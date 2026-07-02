// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

#pragma once

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/touptek/touptek_sdk_wrapper.h>

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace alpacacore::test {

/**
 * Scripted fake for the ToupTekSDK seam (issue #104).
 *
 * Capabilities:
 * - Fault injection: add a method name to `throw_from` and that call throws
 *   AlpacaException(DriverException) — e.g. throw_from.insert("put_trigger_mode")
 *   reproduces the PR #99 connect-path handle-leak scenario.
 * - Canned devices: fill `cameras` / `focusers` / `wheels`; enumerations return
 *   them verbatim, opens resolve ids against them.
 * - Ref-counted opens per the ToupTekSDK contract: same id twice returns the
 *   same handle; `physical_opens`/`physical_closes` count the real transitions
 *   and `underflow_closes` counts closes of handles that were not open (must
 *   stay 0 in a correct driver).
 * - Scripted wheel positions: `wheel_position_script` is consumed one read at
 *   a time (last value repeats), so homing/bounce sequences are testable.
 *
 * Not thread-hardened beyond a coarse recursive story: driver code already
 * serializes SDK access; tests drive one connect path at a time.
 */
class FakeToupTekSDK : public vendor::touptek::ToupTekSDK {
public:
    using ToupCameraInfo = vendor::touptek::ToupCameraInfo;
    using ToupFocuserInfo = vendor::touptek::ToupFocuserInfo;
    using ToupFilterWheelInfo = vendor::touptek::ToupFilterWheelInfo;
    using ToupExpRange = vendor::touptek::ToupExpRange;
    using ToupGainRange = vendor::touptek::ToupGainRange;
    using ToupROIFormat = vendor::touptek::ToupROIFormat;
    using ToupGuideDirection = vendor::touptek::ToupGuideDirection;

    // --- scripting knobs ---------------------------------------------------
    std::set<std::string> throw_from;
    std::vector<ToupCameraInfo> cameras;
    std::vector<ToupFocuserInfo> focusers;
    std::vector<ToupFilterWheelInfo> wheels;
    std::deque<int> wheel_position_script;  // consumed by get_filter_wheel_position
    int wheel_slot_count = 5;
    bool taillight_supported = true;

    // --- observability -----------------------------------------------------
    std::map<std::string, int> calls;
    int physical_opens = 0;
    int physical_closes = 0;
    int underflow_closes = 0;
    std::string last_opened_id;

    int ref_count(const std::string& id) const {
        auto it = ref_counts_.find(id);
        return it == ref_counts_.end() ? 0 : it->second;
    }

    // Convenience: a plausible cooled mono camera with every capability the
    // drivers probe, so most tests only tweak what they care about.
    static ToupCameraInfo default_camera(const std::string& id, const std::string& name) {
        ToupCameraInfo info;
        info.id = id;
        info.name = name;
        info.model_name = "FakeCam Pro";
        info.max_width = 1920;
        info.max_height = 1080;
        info.pixel_size_um_x = 3.76F;
        info.pixel_size_um_y = 3.76F;
        info.is_color = false;
        info.supported_bins = {1, 2, 4};
        info.supports_cooler = true;
        info.supports_tec_onoff = true;
        info.supports_trigger_software = true;
        info.supports_heat = true;
        info.supports_fan = true;
        info.max_fan_speed = 3;
        info.bit_depth_max = 16;
        return info;
    }

    // --- ToupTekSDK implementation ------------------------------------------
    std::string get_sdk_version() override { return hit("get_sdk_version"), "fake-1.0"; }

    std::vector<ToupCameraInfo> enumerate_cameras() override {
        hit("enumerate_cameras");
        auto copy = cameras;
        for (std::size_t i = 0; i < copy.size(); ++i) copy[i].index = static_cast<int>(i);
        return copy;
    }
    HToupcam open_camera_by_id(const std::string& id) override {
        hit("open_camera_by_id");
        return open_shared(id);
    }
    void close_camera(HToupcam handle) override {
        hit("close_camera");
        close_shared(handle);
    }

    void start_pull_mode(HToupcam h, void (*)(unsigned, void*), void*) override {
        hit("start_pull_mode");
        require_open(h);
    }
    void stop(HToupcam h) override {
        hit("stop");
        require_open(h);
    }
    void put_trigger_mode(HToupcam h, int) override {
        hit("put_trigger_mode");
        require_open(h);
    }
    void trigger(HToupcam h, unsigned short) override {
        hit("trigger");
        require_open(h);
    }
    bool wait_image(HToupcam h, unsigned, void*, int, int, unsigned& actual_width, unsigned& actual_height) override {
        hit("wait_image");
        require_open(h);
        actual_width = 0;
        actual_height = 0;
        return false;  // "timeout" — exposure tests belong to the poll-loop issue (#105)
    }

    ToupExpRange get_exposure_range(HToupcam h) override {
        hit("get_exposure_range");
        require_open(h);
        return {100U, 3600000000U, 100000U};
    }
    unsigned get_exposure_us(HToupcam h) override {
        hit("get_exposure_us");
        require_open(h);
        return exposure_us_;
    }
    void put_exposure_us(HToupcam h, unsigned v) override {
        hit("put_exposure_us");
        require_open(h);
        exposure_us_ = v;
    }
    void put_auto_exposure(HToupcam h, bool) override {
        hit("put_auto_exposure");
        require_open(h);
    }
    ToupGainRange get_gain_range(HToupcam h) override {
        hit("get_gain_range");
        require_open(h);
        return {100, 300, 100};
    }
    unsigned short get_gain(HToupcam h) override {
        hit("get_gain");
        require_open(h);
        return gain_;
    }
    void put_gain(HToupcam h, unsigned short v) override {
        hit("put_gain");
        require_open(h);
        gain_ = v;
    }

    ToupROIFormat get_roi(HToupcam h) override {
        hit("get_roi");
        require_open(h);
        return roi_;
    }
    void put_roi(HToupcam h, unsigned x, unsigned y, unsigned w, unsigned hh) override {
        hit("put_roi");
        require_open(h);
        roi_ = {x, y, w, hh};
    }
    void put_binning(HToupcam h, int bin) override {
        hit("put_binning");
        require_open(h);
        bin_ = bin;
    }
    int get_binning(HToupcam h) override {
        hit("get_binning");
        require_open(h);
        return bin_;
    }
    void put_bitdepth(HToupcam h, int v) override {
        hit("put_bitdepth");
        require_open(h);
        bitdepth_ = v;
    }
    int get_bitdepth(HToupcam h) override {
        hit("get_bitdepth");
        require_open(h);
        return bitdepth_;
    }
    void put_raw(HToupcam h, int) override {
        hit("put_raw");
        require_open(h);
    }
    int get_option(HToupcam h, unsigned option) override {
        hit("get_option");
        require_open(h);
        return options_[option];
    }
    void put_option(HToupcam h, unsigned option, int value) override {
        hit("put_option");
        require_open(h);
        options_[option] = value;
    }

    void get_size(HToupcam h, int& width, int& height) override {
        hit("get_size");
        require_open(h);
        width = cameras.empty() ? 1920 : cameras.front().max_width;
        height = cameras.empty() ? 1080 : cameras.front().max_height;
    }
    void get_final_size(HToupcam h, int& width, int& height) override {
        hit("get_final_size");
        get_size(h, width, height);
    }
    void get_raw_format(HToupcam h, unsigned& four_cc, unsigned& bits) override {
        hit("get_raw_format");
        require_open(h);
        four_cc = 0x59595959;  // 'YYYY'
        bits = 16;
    }

    int get_temperature_deciC(HToupcam h) override {
        hit("get_temperature_deciC");
        require_open(h);
        return 210;
    }
    void put_tec_enable(HToupcam h, bool v) override {
        hit("put_tec_enable");
        require_open(h);
        tec_on_ = v;
    }
    bool get_tec_enable(HToupcam h) override {
        hit("get_tec_enable");
        require_open(h);
        return tec_on_;
    }
    void put_tec_target_deciC(HToupcam h, int v) override {
        hit("put_tec_target_deciC");
        require_open(h);
        tec_target_ = v;
    }
    int get_tec_target_deciC(HToupcam h) override {
        hit("get_tec_target_deciC");
        require_open(h);
        return tec_target_;
    }
    int get_tec_voltage_deciV(HToupcam h) override {
        hit("get_tec_voltage_deciV");
        require_open(h);
        return 0;
    }
    int get_tec_voltage_max_deciV(HToupcam h) override {
        hit("get_tec_voltage_max_deciV");
        require_open(h);
        return 42;
    }

    int get_high_fullwell(HToupcam h) override {
        hit("get_high_fullwell");
        require_open(h);
        return hfw_;
    }
    void put_high_fullwell(HToupcam h, bool v) override {
        hit("put_high_fullwell");
        require_open(h);
        hfw_ = v ? 1 : 0;
    }
    int get_cg(HToupcam h) override {
        hit("get_cg");
        require_open(h);
        return cg_;
    }
    void put_cg(HToupcam h, int v) override {
        hit("put_cg");
        require_open(h);
        cg_ = v;
    }
    int get_blacklevel(HToupcam h) override {
        hit("get_blacklevel");
        require_open(h);
        return blacklevel_;
    }
    void put_blacklevel(HToupcam h, int v) override {
        hit("put_blacklevel");
        require_open(h);
        blacklevel_ = v;
    }
    int get_blacklevel_max(HToupcam h, int deep_bits) override {
        hit("get_blacklevel_max");
        require_open(h);
        return 31 << (deep_bits - 8);
    }

    int get_heat_max(HToupcam h) override {
        hit("get_heat_max");
        require_open(h);
        return 10;
    }
    int get_heat(HToupcam h) override {
        hit("get_heat");
        require_open(h);
        return heat_;
    }
    void put_heat(HToupcam h, int v) override {
        hit("put_heat");
        require_open(h);
        heat_ = v;
    }
    int get_fan(HToupcam h) override {
        hit("get_fan");
        require_open(h);
        return fan_;
    }
    void put_fan(HToupcam h, int v) override {
        hit("put_fan");
        require_open(h);
        fan_ = v;
    }
    int get_taillight(HToupcam h) override {
        hit("get_taillight");
        require_open(h);
        if (!taillight_supported) {
            throw AlpacaException("fake: taillight unsupported", AlpacaError::DriverException);
        }
        return taillight_;
    }
    void put_taillight(HToupcam h, bool v) override {
        hit("put_taillight");
        require_open(h);
        taillight_ = v ? 1 : 0;
    }

    std::string get_serial_number(HToupcam h) override {
        hit("get_serial_number");
        require_open(h);
        return "FAKESN0001";
    }
    std::string get_firmware_version(HToupcam h) override {
        hit("get_firmware_version");
        require_open(h);
        return "fw-fake";
    }
    void get_pixel_size(HToupcam h, unsigned, float& x, float& y) override {
        hit("get_pixel_size");
        require_open(h);
        x = 3.76F;
        y = 3.76F;
    }

    void pulse_guide(HToupcam h, ToupGuideDirection, unsigned) override {
        hit("pulse_guide");
        require_open(h);
    }
    bool is_guiding(HToupcam h) override {
        hit("is_guiding");
        require_open(h);
        return false;
    }

    std::vector<ToupFocuserInfo> enumerate_focusers() override {
        hit("enumerate_focusers");
        auto copy = focusers;
        for (std::size_t i = 0; i < copy.size(); ++i) copy[i].index = static_cast<int>(i);
        return copy;
    }
    HToupcam open_focuser_by_id(const std::string& id) override {
        hit("open_focuser_by_id");
        return open_shared(id);
    }
    void close_focuser(HToupcam handle) override {
        hit("close_focuser");
        close_shared(handle);
    }
    void aaf_set(HToupcam h, int action, int value, const char*) override {
        hit("aaf_set");
        require_open(h);
        aaf_values_[action] = value;
    }
    int aaf_get(HToupcam h, int action, const char*) override {
        hit("aaf_get");
        require_open(h);
        return aaf_values_[action];
    }
    int aaf_range(HToupcam h, int, int, const char*) override {
        hit("aaf_range");
        require_open(h);
        return 100000;
    }

    std::vector<ToupFilterWheelInfo> enumerate_filter_wheels() override {
        hit("enumerate_filter_wheels");
        auto copy = wheels;
        for (std::size_t i = 0; i < copy.size(); ++i) copy[i].index = static_cast<int>(i);
        return copy;
    }
    HToupcam open_filter_wheel_by_id(const std::string& id) override {
        hit("open_filter_wheel_by_id");
        return open_shared(id);
    }
    void close_filter_wheel(HToupcam handle) override {
        hit("close_filter_wheel");
        close_shared(handle);
    }
    int get_filter_wheel_slot_count(HToupcam h) override {
        hit("get_filter_wheel_slot_count");
        require_open(h);
        return wheel_slot_count;
    }
    void set_filter_wheel_slot_count(HToupcam h, int slot_count) override {
        hit("set_filter_wheel_slot_count");
        require_open(h);
        wheel_slot_count = slot_count;
    }
    void reset_filter_wheel(HToupcam h) override {
        hit("reset_filter_wheel");
        require_open(h);
    }
    int get_filter_wheel_position(HToupcam h) override {
        hit("get_filter_wheel_position");
        require_open(h);
        if (wheel_position_script.empty()) {
            return wheel_position_;
        }
        wheel_position_ = wheel_position_script.front();
        if (wheel_position_script.size() > 1) {
            wheel_position_script.pop_front();  // last entry repeats forever
        }
        return wheel_position_;
    }
    void set_filter_wheel_position(HToupcam h, int position) override {
        hit("set_filter_wheel_position");
        require_open(h);
        wheel_position_ = position;
        wheel_position_script.clear();
    }

private:
    void hit(const char* fn) {
        ++calls[fn];
        if (throw_from.count(fn) != 0) {
            throw AlpacaException(std::string("fake: injected failure in ") + fn, AlpacaError::DriverException);
        }
    }

    HToupcam open_shared(const std::string& id) {
        if (!known_id(id)) {
            throw AlpacaException("fake: unknown device id '" + id + "'", AlpacaError::NotConnected);
        }
        last_opened_id = id;
        auto& count = ref_counts_[id];
        if (count == 0) {
            ++physical_opens;
            handles_by_id_[id] = reinterpret_cast<HToupcam>(static_cast<std::uintptr_t>(next_handle_++));
        }
        ++count;
        return handles_by_id_[id];
    }

    void close_shared(HToupcam handle) {
        for (auto& [id, h] : handles_by_id_) {
            if (h == handle && ref_counts_[id] > 0) {
                if (--ref_counts_[id] == 0) {
                    ++physical_closes;
                    handles_by_id_.erase(id);
                }
                return;
            }
        }
        ++underflow_closes;
    }

    void require_open(HToupcam handle) const {
        for (const auto& [id, h] : handles_by_id_) {
            if (h == handle) return;
        }
        throw AlpacaException("fake: SDK call on a closed handle", AlpacaError::DriverException);
    }

    bool known_id(const std::string& id) const {
        for (const auto& c : cameras) {
            if (c.id == id) return true;
        }
        for (const auto& f : focusers) {
            if (f.id == id) return true;
        }
        for (const auto& w : wheels) {
            if (w.id == id) return true;
        }
        return false;
    }

    std::map<std::string, int> ref_counts_;
    std::map<std::string, HToupcam> handles_by_id_;
    std::uintptr_t next_handle_ = 1;

    unsigned exposure_us_ = 100000;
    unsigned short gain_ = 100;
    ToupROIFormat roi_{0, 0, 1920, 1080};
    int bin_ = 1;
    int bitdepth_ = 1;
    std::map<unsigned, int> options_;
    bool tec_on_ = false;
    int tec_target_ = 0;
    int hfw_ = 0;
    int cg_ = 0;
    int blacklevel_ = 0;
    int heat_ = 0;
    int fan_ = 0;
    int taillight_ = 1;
    std::map<int, int> aaf_values_;
    int wheel_position_ = 0;
};

}  // namespace alpacacore::test
