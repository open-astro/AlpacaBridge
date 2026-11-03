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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/zwo/shared_open_table.h>
#include <alpacacore/vendor/zwo/zwo_sdk_wrapper.h>
#include <alpacacore/vendor/zwo/zwo_switch_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace alpacacore::test {

// Opens go through the production SharedOpenTable (issue #1667), so the camera driver and the dew heater switch
// share one fake exactly as they share the wrapper: a per-device call outside with_link() is refused, and a retired
// link is NotConnected before the fake is touched. The raw_* bodies below are the unchecked fake; the public methods of
// the same name without the prefix run them under the table. get_data_after_exposure() runs unserialised, so a held
// download never blocks the table. The attached units are camera.camera_id with `serial`, or the id -> serial map of
// set_units() and replug(); every unit answers with the one `camera` state.
class FakeZWOSDK final : public vendor::zwo::ZWOSDK {
public:
    using CameraInfo = vendor::zwo::ZWOCameraInfo;
    using ControlCaps = vendor::zwo::ZWOControlCaps;
    using ControlType = vendor::zwo::ZWOControlType;
    using ExposureStatus = vendor::zwo::ZWOExposureStatus;
    using ImageType = vendor::zwo::ZWOImageType;
    using ROIDescription = vendor::zwo::ZWOROIFormat;
    using StartPosition = vendor::zwo::ZWOStartPos;
    using GuideDirection = vendor::zwo::ZWOGuideDirection;

    CameraInfo camera = default_camera();
    std::vector<std::uint8_t> frame_bytes;
    std::string download_error;
    std::optional<ROIDescription> roi_readback_override;
    std::optional<StartPosition> start_readback_override;
    std::function<void(const std::string&)> before_call;
    bool fail_open{false};
    bool fail_close{false};
    // The cable is pulled: every per-device call on an open camera throws NotConnected, as the wrapper maps
    // ASI_ERROR_CAMERA_REMOVED, except the exposure status read, which answers Failed (ASI_EXP_FAILED, what the SDK
    // gave on the rig). A close still drops the handle and then throws the same; no unit enumerates or opens.
    std::atomic<bool> removed{false};
    // A bare cable pull as ASI SDK 1.41 answered it on the rig (#1667): no removal code. Every per-device call on an
    // open camera throws a generic DriverException (ASI_ERROR_GENERAL_ERROR), the exposure status read answers Failed,
    // a control read answers not readable, and the camera no longer enumerates or opens. replug() clears it.
    std::atomic<bool> pulled{false};
    // The serial the one camera reports, read at every open (a different unit behind the same id changes it).
    std::string serial{"FAKE-ZWO-17"};
    bool stop_releases_download{true};
    bool auto_complete_exposure{true};
    bool fail_exposure_register_write{false};
    bool fail_exposure_start{false};
    // Every exposure start reports Failed until cleared: a driver retry meets the fault again (sweep tier 3).
    std::atomic<bool> fail_every_exposure{false};
    // false models a camera without a cooler: get_control_caps omits CoolerOn and CoolerPower.
    bool report_cooler_controls{true};
    std::optional<ControlCaps> bandwidth_caps =
        ControlCaps{ControlType::BandwidthOverload, "BandWidth", "USB traffic", 40, 100, 80, true, true};
    bool fail_bandwidth_write{false};
    bool fail_bandwidth_read{false};
    // The anti-dew heater read answers not readable (a generic code on a camera that still enumerates).
    std::atomic<bool> fail_heater_read{false};
    std::optional<long> bandwidth_readback;
    bool bandwidth_auto_readback{false};
    // Issue #722: the real SDK answers 0 (success) on the first temperature reads after open.
    int zero_temperature_reads_after_open{0};
    // Issue #1349: the ASI120MM (USB 2.0) fails an exposure longer than 3 s at its end when it starts
    // inside ~0.5 s of the previous download (measured: 5 s and 10 s fail at 0.3-0.45 s, clean at
    // 0.5 s; 1-3 s never failed). Set a clock to model it on a camera whose is_usb3 is false: such a
    // start reports Failed at once, where the camera reports it only at the end of the exposure. The
    // fake also fails the whole 0-0.5 s window, harsher than the rig, where a start below 0.3 s joins
    // a frame the camera began itself. USB 3.0 cameras, short exposures, and any fake without a clock
    // keep the plain behaviour.
    const util::TaskClock* usb2_settle_clock{nullptr};
    static constexpr std::chrono::milliseconds kUsb2FailWindow{500};
    static constexpr long kUsb2FailMinExposureUs{3'000'000};

    static CameraInfo default_camera() {
        CameraInfo info;
        info.camera_id = 17;
        info.name = "Fake ZWO Camera";
        info.max_width = 16;
        info.max_height = 8;
        info.is_color = false;
        info.supported_bins = {1, 2, 4};
        info.supported_formats = {ImageType::Raw16, ImageType::Raw8, ImageType::Y8, ImageType::Rgb24};
        info.pixel_size_um = 3.76;
        info.has_st4_port = true;
        info.has_cooler = true;
        info.bit_depth = 16;
        info.is_usb3 = true;
        return info;
    }

    void hold_download() {
        std::lock_guard lock(download_mutex_);
        download_held_ = true;
        download_entered_ = false;
    }

    void release_download() {
        {
            std::lock_guard lock(download_mutex_);
            download_held_ = false;
        }
        download_cv_.notify_all();
    }

    bool wait_for_download(std::chrono::milliseconds timeout) {
        std::unique_lock lock(download_mutex_);
        return download_cv_.wait_for(lock, timeout, [this] { return download_entered_; });
    }

    int call_count(const std::string& name) const {
        std::lock_guard lock(mutex_);
        const auto it = calls_.find(name);
        return it == calls_.end() ? 0 : it->second;
    }

    bool wait_for_call_count(const std::string& name, int count, std::chrono::milliseconds timeout) const {
        std::unique_lock lock(mutex_);
        return calls_cv_.wait_for(lock, timeout, [&] {
            const auto it = calls_.find(name);
            return it != calls_.end() && it->second >= count;
        });
    }

    void script_exposure_status(std::vector<ExposureStatus> statuses) {
        std::lock_guard lock(mutex_);
        status_script_.assign(statuses.begin(), statuses.end());
    }

    void set_exposure_status_error(std::string message) {
        std::lock_guard lock(mutex_);
        exposure_status_error_ = std::move(message);
    }

    void set_exposure_status(ExposureStatus status) {
        std::lock_guard lock(mutex_);
        status_ = status;
    }

    /// Clock time of every start_exposure call, in call order (empty without usb2_settle_clock).
    std::vector<util::TaskClock::clock::time_point> start_times() const {
        std::lock_guard lock(mutex_);
        return start_times_;
    }

    /// Holders of the table's open for @p camera_id (the camera driver and the dew heater each hold one).
    std::size_t holders(int camera_id) const { return table_.open_count(camera_id); }

    int open_count() const {
        std::lock_guard lock(mutex_);
        return static_cast<int>(open_ids_.size());
    }

    /// Replaces the attached units (SDK id -> serial; "" is a camera that reports none).
    void set_units(std::map<int, std::string> units) {
        std::lock_guard lock(mutex_);
        units_ = std::move(units);
    }

    /// The cable is back with these units: `removed` clears, and the handles opened before the pull are gone.
    void replug(std::map<int, std::string> units) {
        std::lock_guard lock(mutex_);
        units_ = std::move(units);
        open_ids_.clear();
        removed = false;
        pulled = false;
    }

    /// Per-device calls that reached a unit (a raw_* body); one the table refused first is not counted.
    int unit_touches() const {
        std::lock_guard lock(mutex_);
        return unit_touches_;
    }

    /// Directions of the pulse_guide_off calls an open unit accepted, in call order.
    std::vector<GuideDirection> guide_offs() const {
        std::lock_guard lock(mutex_);
        return guide_offs_;
    }

    bool closed_during_download() const {
        std::lock_guard lock(download_mutex_);
        return closed_during_download_;
    }

    std::vector<CameraInfo> enumerate_cameras(std::vector<vendor::zwo::ZwoUnreadableSlot>* = nullptr) override {
        hit("enumerate_cameras");
        std::vector<CameraInfo> cameras;
        std::lock_guard lock(mutex_);
        for (const auto& [id, unit_serial] : listed_units_locked()) {
            cameras.push_back(camera);
            cameras.back().camera_id = id;
        }
        return cameras;
    }

    std::vector<vendor::zwo::ZwoEnumeratedCamera> enumerate_identified_cameras(
        const std::string& only_model_name, std::vector<vendor::zwo::ZwoUnreadableSlot>*) override {
        hit("enumerate_identified_cameras");
        if (!only_model_name.empty() && only_model_name != camera.name) return {};
        std::vector<vendor::zwo::ZwoEnumeratedCamera> found;
        std::lock_guard lock(mutex_);
        for (const auto& [id, unit_serial] : listed_units_locked()) {
            found.push_back({static_cast<int>(found.size()), id, camera.name, unit_serial});
        }
        return found;
    }

    bool get_camera_info_by_id(int camera_id, CameraInfo& info) override {
        hit("get_camera_info_by_id");
        const auto unchecked = table_.lock_unchecked();
        std::lock_guard lock(mutex_);
        if (listed_units_locked().count(camera_id) == 0) return false;
        require_open_locked(camera_id);
        info = camera;
        info.camera_id = camera_id;
        return true;
    }

    bool get_camera_info_by_index(int camera_index, CameraInfo& info) override {
        hit("get_camera_info_by_index");
        const auto unchecked = table_.lock_unchecked();
        std::lock_guard lock(mutex_);
        const auto units = listed_units_locked();
        if (camera_index < 0 || camera_index >= static_cast<int>(units.size())) return false;
        info = camera;
        info.camera_id = std::next(units.begin(), camera_index)->first;
        return true;
    }

    vendor::zwo::SharedOpenTable::Opened open_camera_link(int camera_id) override { return table_.open(camera_id); }

    void close_camera_link(int camera_id, std::uint64_t link) override { table_.close(camera_id, link); }

    void with_link(int camera_id, std::uint64_t link, const std::function<void()>& fn) override {
        table_.with_link(camera_id, link, fn);
    }

    std::vector<ControlCaps> raw_get_control_caps(int camera_id) {
        std::lock_guard lock(mutex_);
        require_open_locked(camera_id);
        std::vector<ControlCaps> caps = {
            {ControlType::Exposure, "Exposure", "Exposure time", 1, 3'600'000'000L, 100'000, false, true},
            {ControlType::Gain, "Gain", "Gain", 0, 500, 100, false, true},
            {ControlType::Offset, "Offset", "Offset", 0, 100, 10, false, true},
            {ControlType::Temperature, "Temperature", "Temperature", -500, 500, 0, false, false},
            {ControlType::CoolerOn, "Cooler", "Cooler", 0, 1, 0, false, true},
            {ControlType::CoolerPower, "Cooler Power", "Cooler power", 0, 100, 0, false, false},
            {ControlType::TargetTemperature, "Target Temperature", "Target temperature", -500, 300, 0, false, true},
            {ControlType::HighSpeedMode, "High Speed", "High speed", 0, 1, 0, false, true},
            {ControlType::AntiDewHeater, "Anti Dew", "Anti-dew heater", 0, 100, 0, false, true}};
        if (!report_cooler_controls) {
            std::erase_if(caps, [](const ControlCaps& c) {
                return c.type == ControlType::CoolerOn || c.type == ControlType::CoolerPower;
            });
        }
        if (bandwidth_caps) caps.push_back(*bandwidth_caps);
        return caps;
    }

    bool raw_get_control_value(int camera_id, ControlType type, long& value, bool& is_auto) {
        std::lock_guard lock(mutex_);
        last_read_generic_ = false;
        if (pulled && open_ids_.count(camera_id) != 0) {
            ++unit_touches_;
            last_read_generic_ = true;
            return false;  // the wrapper answers a generic code on this read as not readable
        }
        require_open_locked(camera_id);
        value = controls_[type];
        if (type == ControlType::Temperature && temperature_reads_since_open_++ < zero_temperature_reads_after_open) {
            value = 0;
        }
        is_auto = false;
        if (type == ControlType::AntiDewHeater && fail_heater_read) {
            last_read_generic_ = true;
            return false;
        }
        if (type == ControlType::BandwidthOverload) {
            if (fail_bandwidth_read) return false;
            value = bandwidth_readback.value_or(value);
            is_auto = bandwidth_auto_readback || bandwidth_auto_;
        }
        return true;
    }

    void raw_set_control_value(int camera_id, ControlType type, long value, bool is_auto) {
        std::lock_guard lock(mutex_);
        require_open_locked(camera_id);
        if (type == ControlType::Exposure && fail_exposure_register_write) {
            fail_exposure_register_write = false;
            throw AlpacaException("scripted exposure-register failure", AlpacaError::DriverException);
        }
        if (type == ControlType::BandwidthOverload) {
            if (fail_bandwidth_write || !bandwidth_caps || !bandwidth_caps->is_writable ||
                value < bandwidth_caps->min_value || value > bandwidth_caps->max_value) {
                throw AlpacaException("scripted USB bandwidth write failure", AlpacaError::DriverException);
            }
            ++calls_["set_bandwidth"];
            bandwidth_auto_ = is_auto;
        }
        controls_[type] = value;
    }

    ROIDescription raw_get_roi_format(int camera_id) {
        std::lock_guard lock(mutex_);
        require_open_locked(camera_id);
        return roi_readback_override.value_or(roi_);
    }

    void raw_set_roi_format(int camera_id, int width, int height, int bin, ImageType type) {
        std::lock_guard lock(mutex_);
        require_open_locked(camera_id);
        roi_ = {width, height, bin, type};
    }

    StartPosition raw_get_start_pos(int camera_id) {
        std::lock_guard lock(mutex_);
        require_open_locked(camera_id);
        return start_readback_override.value_or(start_pos_);
    }

    void raw_set_start_pos(int camera_id, int start_x, int start_y) {
        std::lock_guard lock(mutex_);
        require_open_locked(camera_id);
        start_pos_ = {start_x, start_y};
    }

    void raw_start_exposure(int camera_id, bool) {
        std::lock_guard lock(mutex_);
        require_open_locked(camera_id);
        if (fail_exposure_start) {
            fail_exposure_start = false;
            throw AlpacaException("scripted exposure-trigger failure", AlpacaError::DriverException);
        }
        status_ = auto_complete_exposure ? ExposureStatus::Success : ExposureStatus::Working;
        if (fail_every_exposure) status_ = ExposureStatus::Failed;
        if (usb2_settle_clock != nullptr) {
            const auto now = usb2_settle_clock->now();
            start_times_.push_back(now);
            const auto exposure = controls_.find(ControlType::Exposure);
            const bool long_exposure = exposure != controls_.end() && exposure->second > kUsb2FailMinExposureUs;
            if (!camera.is_usb3 && long_exposure && last_download_time_.has_value() &&
                now - *last_download_time_ < kUsb2FailWindow) {
                status_ = ExposureStatus::Failed;
            }
        }
    }

    void raw_stop_exposure(int camera_id) {
        {
            std::lock_guard lock(mutex_);
            require_open_locked(camera_id);
            status_ = ExposureStatus::Idle;
        }
        if (stop_releases_download) release_download();
    }

    ExposureStatus raw_get_exposure_status(int camera_id) {
        std::lock_guard lock(mutex_);
        if (removed || pulled) {
            ++unit_touches_;
            return ExposureStatus::Failed;
        }
        require_open_locked(camera_id);
        if (!exposure_status_error_.empty()) {
            const auto message = std::exchange(exposure_status_error_, {});
            throw AlpacaException(message, AlpacaError::DriverException);
        }
        if (!status_script_.empty()) {
            const auto status = status_script_.front();
            status_script_.pop_front();
            return status;
        }
        return status_;
    }

    void raw_get_data_after_exposure(int camera_id, std::uint8_t* buffer, long buffer_size) {
        {
            std::unique_lock lock(download_mutex_);
            {
                std::lock_guard sdk_lock(mutex_);
                require_open_locked(camera_id);
            }
            download_entered_ = true;
            download_in_progress_ = true;
            download_cv_.notify_all();
            download_cv_.wait(lock, [this] { return !download_held_; });
        }
        std::string error;
        {
            std::lock_guard lock(mutex_);
            error = download_error;
            if (error.empty()) {
                const auto copy_size = std::min(static_cast<std::size_t>(buffer_size), frame_bytes.size());
                if (copy_size > 0) std::memcpy(buffer, frame_bytes.data(), copy_size);
            }
        }
        {
            std::lock_guard lock(download_mutex_);
            download_in_progress_ = false;
        }
        if (!error.empty()) throw AlpacaException(error, AlpacaError::DriverException);
        if (usb2_settle_clock != nullptr) {
            const auto now = usb2_settle_clock->now();
            std::lock_guard lock(mutex_);
            last_download_time_ = now;
        }
    }

    void raw_pulse_guide_on(int camera_id, GuideDirection) {
        std::lock_guard lock(mutex_);
        require_open_locked(camera_id);
    }

    void raw_pulse_guide_off(int camera_id, GuideDirection direction) {
        std::lock_guard lock(mutex_);
        require_open_locked(camera_id);
        guide_offs_.push_back(direction);
    }

    std::string raw_get_serial_number(int camera_id) {
        std::lock_guard lock(mutex_);
        require_open_locked(camera_id);
        const std::string unit_serial = units_locked().at(camera_id);
        if (unit_serial.empty()) throw AlpacaException("fake ZWO camera has no serial", AlpacaError::DriverException);
        return unit_serial;
    }

    // The per-device interface: each runs its raw_* body under the caller's link (hit() first, so a before_call hook
    // never holds the table's mutex).
    std::vector<ControlCaps> get_control_caps(int camera_id) override {
        hit("get_control_caps");
        return table_.call(camera_id, [&] { return raw_get_control_caps(camera_id); });
    }
    // As the wrapper: the enumeration is checked after a not-readable answer that a generic code gave (a pulled
    // camera, fail_heater_read), not after one for a control the camera cannot read (fail_bandwidth_read).
    bool get_control_value(int camera_id, ControlType type, long& value, bool& is_auto) override {
        hit("get_control_value");
        return table_.call(
            camera_id, [&] { return raw_get_control_value(camera_id, type, value, is_auto); },
            [this](bool read) { return !read && last_read_generic_; });
    }
    void set_control_value(int camera_id, ControlType type, long value, bool is_auto) override {
        hit("set_control_value");
        table_.call(camera_id, [&] { raw_set_control_value(camera_id, type, value, is_auto); });
    }
    ROIDescription get_roi_format(int camera_id) override {
        hit("get_roi_format");
        return table_.call(camera_id, [&] { return raw_get_roi_format(camera_id); });
    }
    void set_roi_format(int camera_id, int width, int height, int bin, ImageType type) override {
        hit("set_roi_format");
        table_.call(camera_id, [&] { raw_set_roi_format(camera_id, width, height, bin, type); });
    }
    StartPosition get_start_pos(int camera_id) override {
        hit("get_start_pos");
        return table_.call(camera_id, [&] { return raw_get_start_pos(camera_id); });
    }
    void set_start_pos(int camera_id, int start_x, int start_y) override {
        hit("set_start_pos");
        table_.call(camera_id, [&] { raw_set_start_pos(camera_id, start_x, start_y); });
    }
    void start_exposure(int camera_id, bool is_dark) override {
        hit("start_exposure");
        table_.call(camera_id, [&] { raw_start_exposure(camera_id, is_dark); });
    }
    void stop_exposure(int camera_id) override {
        hit("stop_exposure");
        table_.call(camera_id, [&] { raw_stop_exposure(camera_id); });
    }
    ExposureStatus get_exposure_status(int camera_id) override {
        hit("get_exposure_status");
        return table_.call(
            camera_id, [&] { return raw_get_exposure_status(camera_id); },
            [](ExposureStatus status) { return status == ExposureStatus::Failed; });
    }
    void get_data_after_exposure(int camera_id, std::uint8_t* buffer, long buffer_size) override {
        hit("get_data_after_exposure");
        table_.call_unserialised(camera_id, [&] { raw_get_data_after_exposure(camera_id, buffer, buffer_size); });
    }
    void pulse_guide_on(int camera_id, GuideDirection direction) override {
        hit("pulse_guide_on");
        table_.call(camera_id, [&] { raw_pulse_guide_on(camera_id, direction); });
    }
    void pulse_guide_off(int camera_id, GuideDirection direction) override {
        // Counted after the raw body, so a waiter on the count sees the direction in guide_offs().
        if (before_call) before_call("pulse_guide_off");
        try {
            table_.call(camera_id, [&] { raw_pulse_guide_off(camera_id, direction); });
        } catch (...) {
            count_call("pulse_guide_off");
            throw;
        }
        count_call("pulse_guide_off");
    }
    std::string get_serial_number(int camera_id) override {
        hit("get_serial_number");
        return table_.call(camera_id, [&] { return raw_get_serial_number(camera_id); });
    }

    std::string get_sdk_version() override {
        hit("get_sdk_version");
        return "fake-zwo-sdk-1.0";
    }

private:
    void hit(const std::string& name) {
        if (before_call) before_call(name);
        count_call(name);
    }

    void count_call(const std::string& name) {
        std::lock_guard lock(mutex_);
        ++calls_[name];
        calls_cv_.notify_all();
    }

    std::map<int, std::string> units_locked() const {
        return units_.value_or(std::map<int, std::string>{{camera.camera_id, serial}});
    }

    // The units the SDK enumerates: none once the cable is pulled.
    std::map<int, std::string> listed_units_locked() const {
        return pulled ? std::map<int, std::string>{} : units_locked();
    }

    // The table's real open (ASIOpenCamera, then ASIInitCamera) and close. An id that is not attached is
    // InvalidValue on the open (the wrapper's ASIOpenCamera mapping).
    void real_open(int camera_id) {
        hit("open_camera");
        {
            std::lock_guard lock(mutex_);
            if (removed || listed_units_locked().count(camera_id) == 0) {
                throw AlpacaException("fake ZWO camera id not attached", AlpacaError::InvalidValue);
            }
        }
        if (fail_open) throw AlpacaException("scripted open failure", AlpacaError::DriverException);
        {
            std::lock_guard lock(mutex_);
            open_ids_.insert(camera_id);
            last_download_time_.reset();  // a reopened camera has no download behind it
            temperature_reads_since_open_ = 0;
        }
        hit("init_camera");
    }

    void real_close(int camera_id) {
        hit("close_camera");
        {
            std::lock_guard lock(download_mutex_);
            closed_during_download_ = closed_during_download_ || download_in_progress_;
        }
        std::lock_guard lock(mutex_);
        if (open_ids_.erase(camera_id) == 0) {
            throw AlpacaException("fake ZWO camera is not open", AlpacaError::DriverException);
        }
        if (removed || pulled) throw AlpacaException("fake ZWO camera removed", AlpacaError::NotConnected);
        if (fail_close) throw AlpacaException("scripted close failure", AlpacaError::DriverException);
    }

    // An id that is no longer attached answers ASI_ERROR_INVALID_ID, which the wrapper maps to NotConnected on a
    // per-device call, as it does ASI_ERROR_CAMERA_REMOVED.
    void require_open_locked(int camera_id) const {
        ++unit_touches_;
        if (units_locked().count(camera_id) == 0) {
            throw AlpacaException("fake ZWO camera id not attached", AlpacaError::NotConnected);
        }
        if (open_ids_.count(camera_id) == 0) {
            throw AlpacaException("fake ZWO operation requires an open camera", AlpacaError::DriverException);
        }
        if (removed) {
            throw AlpacaException("fake ZWO camera removed", AlpacaError::NotConnected);
        }
        if (pulled) {
            throw AlpacaException("fake ZWO camera: ASI_ERROR_GENERAL_ERROR", AlpacaError::DriverException);
        }
    }

    mutable std::mutex mutex_;
    mutable std::mutex download_mutex_;
    std::condition_variable download_cv_;
    std::map<std::string, int> calls_;
    mutable std::condition_variable calls_cv_;
    // Non-zero so a default connect+read takes the plain path; 0 is the #722 placeholder.
    std::map<ControlType, long> controls_{{ControlType::Temperature, 150}};
    bool bandwidth_auto_{false};
    ROIDescription roi_{};
    StartPosition start_pos_{};
    ExposureStatus status_{ExposureStatus::Idle};
    std::deque<ExposureStatus> status_script_;
    std::string exposure_status_error_;
    std::vector<util::TaskClock::clock::time_point> start_times_;
    std::optional<util::TaskClock::clock::time_point> last_download_time_;
    std::optional<std::map<int, std::string>> units_;
    std::vector<GuideDirection> guide_offs_;
    std::set<int> open_ids_;
    mutable int unit_touches_{0};
    int temperature_reads_since_open_{0};
    // Whether the last not-readable control answer came from a generic code; written and read under the table's
    // mutex (in one call()), so no other call sees it between the two.
    bool last_read_generic_{false};
    bool download_held_{false};
    bool download_entered_{false};
    bool download_in_progress_{false};
    bool closed_during_download_{false};
    vendor::zwo::SharedOpenTable table_{vendor::zwo::SharedOpenTable::Ops{
        [this](int id) { real_open(id); },
        [this](int id) { real_close(id); },
        [this](int id) { return raw_get_serial_number(id); },
        [this](int id) {
            count_call("listed");  // under the table's mutex: no before_call hook
            std::lock_guard lock(mutex_);
            return listed_units_locked().count(id) != 0;
        },
    }};
};

// Scripted ZWODewHeaterSdk for the dew heater switch. Thread-safe: the driver
// calls the SDK without its own lock, so the stress registration reaches this
// fake from several threads. Setup fields (cameras, heater_by_id,
// serial_by_id) are written before the driver exists and only read after; the
// rest is atomic or behind call_mutex_. No method blocks except the explicit delays:
// open_delay_ms holds open_camera_link(), read_delay_ms a get_control_value() and write_delay_ms
// a set_control_value() in flight (getters-never-wait tests). The *_entered flags and `busy`
// are polled from another thread; close_camera_link() sets closed_while_busy when a delayed call is
// still in flight. Opens and links go through a SharedOpenTable: the caps, value and serial calls are checked
// against the caller's link before the fake is touched (DriverException outside with_link(), NotConnected on a
// closed link). KNOWN PARITY GAPS: the call bodies run outside the table, so they are not serialised and a loss
// code retires nothing (the shared-open cases wrap FakeZWOSDK in ZWOSdkDewHeaterSdk for that).
class FakeDewHeaterSdk final : public vendor::zwo::ZWODewHeaterSdk {
public:
    std::atomic<int> reads{0};
    std::atomic<bool> fail_reads{false};
    std::atomic<int> last_read_id{-1};
    std::atomic<int> enumerations{0};
    std::atomic<int> serial_reads{0};
    std::atomic<long> heater{40};
    std::atomic<int> open_delay_ms{0};
    std::atomic<int> read_delay_ms{0};
    std::atomic<int> write_delay_ms{0};
    std::atomic<bool> open_entered{false};
    std::atomic<bool> read_entered{false};
    std::atomic<bool> write_entered{false};
    std::atomic<int> busy{0};
    std::atomic<bool> closed_while_busy{false};

    std::vector<vendor::zwo::ZWOCameraInfo> cameras;
    // Per SDK id; an id absent from a non-empty map has no heater / reads "FAKE1".
    std::map<int, bool> heater_by_id;
    std::map<int, std::string> serial_by_id;

    /// Every camera id any per-camera call (info, open, init, close, caps,
    /// value read, serial) was made with.
    std::set<int> touched_ids() const {
        std::lock_guard<std::mutex> lock(call_mutex_);
        return touched_;
    }

    std::vector<vendor::zwo::ZWOCameraInfo> enumerate_cameras() override {
        ++enumerations;
        return cameras;
    }
    bool get_camera_info_by_id(int id, vendor::zwo::ZWOCameraInfo&) override {
        touch(id);
        return false;
    }
    vendor::zwo::SharedOpenTable::Opened open_camera_link(int id) override {
        const InFlight in_flight(*this, open_delay_ms, open_entered);
        touch(id);
        return table_.open(id);
    }
    void close_camera_link(int id, std::uint64_t link) override {
        if (busy.load() > 0) closed_while_busy = true;
        touch(id);
        table_.close(id, link);
    }
    void with_link(int id, std::uint64_t link, const std::function<void()>& fn) override {
        table_.with_link(id, link, fn);
    }
    std::vector<vendor::zwo::ZWOControlCaps> get_control_caps(int id) override {
        table_.call(id, [] {});
        touch(id);
        if (!heater_by_id.empty()) {
            const auto it = heater_by_id.find(id);
            if (it == heater_by_id.end() || !it->second) {
                return {};
            }
        }
        vendor::zwo::ZWOControlCaps cap;
        cap.type = vendor::zwo::ZWOControlType::AntiDewHeater;
        cap.max_value = 100;
        cap.is_writable = true;
        return {cap};
    }
    bool get_control_value(int id, vendor::zwo::ZWOControlType, long& value, bool& is_auto) override {
        table_.call(id, [] {});
        const InFlight in_flight(*this, read_delay_ms, read_entered);
        touch(id);
        ++reads;
        last_read_id = id;
        if (fail_reads) {
            return false;
        }
        value = heater;
        is_auto = false;
        return true;
    }
    void set_control_value(int id, vendor::zwo::ZWOControlType, long value, bool) override {
        table_.call(id, [] {});
        const InFlight in_flight(*this, write_delay_ms, write_entered);
        touch(id);
        heater = value;
    }
    std::string get_serial_number(int id) override {
        table_.call(id, [] {});
        touch(id);
        ++serial_reads;
        const auto it = serial_by_id.find(id);
        return it == serial_by_id.end() ? "FAKE1" : it->second;
    }

private:
    // Marks a call in flight for its whole duration, sleeping `delay_ms` first.
    struct InFlight {
        InFlight(FakeDewHeaterSdk& fake, const std::atomic<int>& delay_ms, std::atomic<bool>& entered) : fake_(fake) {
            ++fake_.busy;
            entered = true;
            if (delay_ms.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms.load()));
        }
        ~InFlight() { --fake_.busy; }
        InFlight(const InFlight&) = delete;
        InFlight& operator=(const InFlight&) = delete;
        FakeDewHeaterSdk& fake_;
    };

    void touch(int id) {
        std::lock_guard<std::mutex> lock(call_mutex_);
        touched_.insert(id);
    }

    mutable std::mutex call_mutex_;
    std::set<int> touched_;
    vendor::zwo::SharedOpenTable table_{vendor::zwo::SharedOpenTable::Ops{
        [](int) {},
        [](int) {},
        [this](int id) {
            const auto it = serial_by_id.find(id);
            return it == serial_by_id.end() ? std::string("FAKE1") : it->second;
        },
    }};
};

}  // namespace alpacacore::test
