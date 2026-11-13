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

#include <alpacacore/async_connectable.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/capture_session.h>
#include <alpacacore/util/device_name.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/image_validation.h>
#include <alpacacore/util/link_epoch.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/util/version_format.h>
#include <alpacacore/vendor/zwo/zwo_camera_driver.h>
#include <alpacacore/vendor/zwo/zwo_sdk_wrapper.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "zwo_fields.h"

namespace alpacacore::vendor::zwo {

namespace {

std::pair<int, int> bayer_offsets(ZWOBayerPattern pattern) {
    switch (pattern) {
        case ZWOBayerPattern::RG:
            return {0, 0};
        case ZWOBayerPattern::BG:
            return {1, 1};
        case ZWOBayerPattern::GR:
            return {1, 0};
        case ZWOBayerPattern::GB:
            return {0, 1};
        default:
            return {0, 0};
    }
}

bool supports_format(const std::vector<ZWOImageType>& formats, ZWOImageType type) {
    return std::find(formats.begin(), formats.end(), type) != formats.end();
}

bool supports_bin(const std::vector<int>& bins, int bin) {
    return std::find(bins.begin(), bins.end(), bin) != bins.end();
}

// CWE-117: the serial and SDK error texts are device text; control characters never reach the log or a client.
std::string printable(std::string text) {
    std::replace_if(
        text.begin(), text.end(), [](char ch) { return static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f; }, '?');
    return text;
}

}  // namespace

class ZWOCameraDriver : public CameraDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    ZWOCameraDriver(int device_number, ZwoCameraBinding binding, ZWOSDK& sdk, util::TaskClock& clock,
                    ZwoCameraTestHooks hooks = {})
        : AsyncConnectable("ZWO"),
          device_number_(device_number),
          binding_(std::move(binding)),
          camera_id_(binding_.identity.camera_id),
          sdk_(sdk),
          clock_(clock),
          test_hooks_(std::move(hooks)),
          serial_number_(),
          camera_info_(),
          camera_info_valid_(false),
          control_caps_(),
          image_type_(ZWOImageType::Raw8),
          bin_x_(1),
          bin_y_(1),
          num_x_(0),
          num_y_(0),
          roi_width_effective_(0),
          roi_height_effective_(0),
          roi_crop_x_(0),
          roi_crop_y_(0),
          start_x_(0),
          start_y_(0),
          pulse_guiding_(false),
          pulse_guiding_end_(util::TaskClock::clock::time_point{}),
          dev_(*this) {
        preload_camera_info_locked();
    }

    ~ZWOCameraDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        // The SDK stop first, then the join: the exposure body is gone before the camera closes (decision 0020 rule 5).
        // A stop on a camera that is not open is refused by on_link() without touching the SDK.
        stop_sdk_exposure(current_camera_id());
        session_.disconnect_and_join();
        if (link_.is_open()) {
            try {
                apply_connected(false);  // non-virtual: no virtual dispatch during destruction
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ZWO", "Error during destruction: " + std::string(e.what()));
            }
        }
        stop_pulse_timer();  // a lost link skipped the Disconnect above
    }

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override {
        const_cast<ZWOCameraDriver*>(this)->refresh_cached_camera_info_if_needed();
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_info_valid_ && !camera_info_.name.empty()) {
            return util::branded_name("ZWO", camera_info_.name);
        }
        // Never read from the SDK: the model the entry is bound to (issue #1958).
        const std::string expected = expected_model();
        return expected.empty() ? "ZWO Camera" : util::branded_name("ZWO", expected);
    }

    DeviceType get_device_type() const override { return DeviceType::Camera; }

    std::string fallback_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        // Never derived from the device number or the enumeration index: both
        // move between starts, and the ASCOM UniqueID must not.
        return zwo_unique_id(serial_number_.empty() ? binding_.identity.serial : serial_number_, binding_.unique_id);
    }

    std::string get_description() const override { return "ZWO ASI Camera Driver"; }

    std::string get_driver_info() const override { return "AlpacaCore ZWO Camera Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Vendor SDK (library) version, surfaced in the web UI only (never in
    // DriverInfo). ASIGetSDKVersion() returns "1, 7, 7, 0"; render as "1.7.7.0".
    std::optional<std::string> get_device_sdk_version() const override {
        auto version = sdk_.get_sdk_version();
        // get_sdk_version() returns the literal "unknown" when ASIGetSDKVersion()
        // yields nullptr — suppress the row rather than show "unknown".
        if (version.empty() || version == "unknown") {
            return std::nullopt;
        }
        return util::normalize_dotted_version(version);
    }

    int get_interface_version() const override {
        return 4;  // ICameraV4 (Platform 7)
    }

    bool get_connected() const override { return link_.is_open(); }

    // Decision 0009 rule 5: why the link was lost, kept until the next Connect or Disconnect.
    std::string get_link_fault() const override {
        std::lock_guard<std::mutex> lost(lost_mutex_);
        return lost_reason_;
    }

    void connect() override { start_connection_task(true); }

    void disconnect() override { start_connection_task(false); }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override { apply_connected(connected); }

    void apply_connected(bool connected) {
        std::lock_guard<std::mutex> transition_lock(transition_mutex_);
        // Base gates BEFORE the idempotency check: a sync disconnect during an
        // in-flight connect looks idempotent (both sides see disconnected) and
        // would be silently dropped without the record; a connect must honor a
        // newer pending disconnect by staying down.
        if (!connected && record_disconnect_if_connect_in_flight(link_.is_open())) {
            return;
        }
        if (connected && consume_pending_disconnect(link_.is_open())) {
            return;
        }
        if (connected == link_.is_open()) {
            // Idempotent (ASCOM): a redundant Connect/Disconnect is a no-op and must
            // NOT reset exposure state. The Platform-7 `connect` endpoint calls
            // connect() unconditionally, so wiping here would abort an in-flight
            // exposure or discard a just-completed image. Matches the QHY driver.
            // A lost link reads as disconnected, so a Disconnect after a loss lands here and completes: it
            // clears the loss text (decision 0009 rule 5) but keeps the loss stamp (Alternative E), and the next
            // Connect releases what the loss left.
            if (!connected) {
                stop_pulse_timer();  // the timer a loss cancelled
                std::lock_guard<std::mutex> lost(lost_mutex_);
                lost_reason_.clear();
            }
            return;
        }
        // A new epoch first: Connected reads false from here on, before anything that can wait on the SDK, so
        // an exposure body stuck in a hung SDK call cannot keep a Disconnect from showing (issue #1667), and a
        // call that captured the old link can no longer drop the new one (decision 0009 rule 3).
        link_.begin_transition();

        if (!connected) {
            // Decision 0020 rules 5 and 6: stop the SDK exposure, then join the body, both before mutex_ is taken and
            // before the close. A body in ASIGetDataAfterExp is joined when its transfer returns (that call takes no
            // timeout, the 0020 `partial` note). The join is last and closes the session, so a StartExposure that
            // passed its connection check before this Disconnect raises NotConnected and runs no body.
            stop_sdk_exposure(current_camera_id());
            session_.disconnect_and_join();
        } else {
            // Joins a body left from a link loss and clears its kept failure and the last exposure; the session
            // reopens when the connect publishes.
            session_.disconnect_and_join();
        }
        std::unique_lock<std::mutex> lock(mutex_);

        if (connected) {
            // Decision 0016 rule 4: the handshake runs under transition_mutex_ with mutex_
            // released, so Name and the other identity getters answer while the SDK opens.
            // Everything it learns lands in locals and is published under mutex_ at the end.
            // A preloaded name stands when the SDK cannot answer by id.
            ZWOCameraInfo info = camera_info_;
            bool info_valid = camera_info_valid_;
            connect_in_flight_.store(true);
            lock.unlock();
            struct ClearInFlight {
                std::atomic<bool>& flag;
                ~ClearInFlight() { flag.store(false); }
            } clear_in_flight{connect_in_flight_};

            // Before the open below, so no turn-off from the old link outlives it.
            stop_pulse_timer();
            release_open();  // an open left from a lost link: the table retired it, so this only forgets it
            {
                std::lock_guard<std::mutex> lost(lost_mutex_);
                lost_reason_.clear();
            }
            // The relink consumes the loss stamp, so the next loss is stamped afresh; a camera times no motion by it.
            const auto lost_at = link_lost_at_.consume();
            if (test_hooks_.on_relink) test_hooks_.on_relink(lost_at);
            const int resolved_id = resolve_camera_id();
            const auto opened = sdk_.open_camera_link(resolved_id);
            open_id_.store(resolved_id);
            open_link_.store(opened.link);
            // Guard the ENTIRE remaining init: an open left unreleased on a throw is held for ever
            // (driver-lifecycle.instructions.md connect rule, same shape as the EAF/EFW/CAA siblings).
            std::unordered_map<ZWOControlType, ZWOControlCaps> caps;
            ZWOImageType image_type{};
            int max_width = 0;
            int max_height = 0;
            int roi_width = 0;
            int roi_height = 0;
            try {
                sdk_.with_link(resolved_id, opened.link, [&] {
                    ZWOCameraInfo fresh;
                    if (sdk_.get_camera_info_by_id(resolved_id, fresh)) {
                        info = std::move(fresh);
                        info_valid = true;
                    }
                    for (const auto& cap : dev_.get_control_caps(resolved_id)) {
                        caps[cap.type] = cap;
                    }
                    apply_usb_bandwidth(resolved_id, caps, info.name);
                    image_type = select_default_image_type(info, info_valid);

                    max_width = info_valid ? info.max_width : 0;
                    max_height = info_valid ? info.max_height : 0;
                    roi_width = max_width - (max_width % 8);
                    roi_height = max_height - (max_height % 2);

                    if (roi_width > 0 && roi_height > 0) {
                        dev_.set_roi_format(resolved_id, roi_width, roi_height, 1, image_type);
                        dev_.set_start_pos(resolved_id, 0, 0);
                    }
                });
                // Published after with_link returns, so mutex_ is never taken under the SDK lock.
                // The serial was read at the open. Some cameras have none: non-fatal, connect with an
                // empty serial (same policy as the EAF/EFW/CAA siblings).
                lock.lock();
                camera_id_ = resolved_id;
                camera_info_ = info;
                camera_info_valid_ = info_valid;
                control_caps_ = std::move(caps);
                image_type_ = image_type;
                bin_x_ = 1;
                bin_y_ = 1;
                start_x_ = 0;
                start_y_ = 0;
                num_x_ = max_width;
                num_y_ = max_height;
                roi_width_effective_ = roi_width;
                roi_height_effective_ = roi_height;
                roi_crop_x_ = 0;
                roi_crop_y_ = 0;
                serial_number_ = opened.serial;
            } catch (...) {
                if (lock.owns_lock()) {
                    lock.unlock();
                }
                try {
                    release_open();
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("ZWO", "close of the camera after failed connect: " + std::string(e.what()));
                }
                throw;
            }
            if (opened.serial.empty()) {
                ALPACA_LOG_WARN("ZWO", "Camera serial number unavailable");
            } else {
                // Every later Connect scans for this serial, so a replug that moved the camera to another SDK id,
                // or swapped two cameras' ids, still opens the same camera and never another (issue #1667).
                bound_serial_ = opened.serial;
            }
            {
                std::lock_guard<std::mutex> lost(lost_mutex_);
                unit_label_ = opened.serial.empty() ? "ZWO camera id " + std::to_string(resolved_id)
                                                    : "ZWO camera " + printable(opened.serial);
            }
            reset_exposure_state_locked();
            first_temperature_read_pending_.store(true);
            session_.open();
            link_.publish_open();
            if (lock.owns_lock()) lock.unlock();
            report_identity_learned(opened.serial, info.name);
            return;
        }

        // Clear driver state before the SDK close so a racing operational call fails fast at its
        // connection check instead of hitting a just-closed camera, and a throwing close can't trap the
        // driver half-connected — same order as the switch/EFW/CAA/EAF
        // siblings, per the driver-lifecycle.instructions.md disconnect rule (issue #116). The
        // disconnected state itself was published by begin_transition() above.
        const std::optional<int> close_id = camera_id_;
        serial_number_.clear();
        camera_id_.reset();
        reset_exposure_state_locked();
        lock.unlock();
        // A pulse in flight ends now, its turn-off sent on the open the close below drops (async_operation.h Rule 8).
        stop_pulse_timer();
        if (close_id.has_value()) {
            release_open();
        }
        // A loss may win after the non-idempotent branch decision but before begin_transition().
        std::lock_guard<std::mutex> lost(lost_mutex_);
        lost_reason_.clear();
    }

    std::vector<std::string> get_supported_actions() const override { return {}; }

    std::string action(std::string_view action_name, std::string_view) override {
        throw AlpacaException("Action not supported: " + std::string(action_name), AlpacaError::ActionNotImplemented);
    }

    bool can_action(std::string_view) const override { return false; }

    std::string command_blind(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }

    bool command_bool(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }

    std::string command_string(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }

    int get_bayer_offset_x() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.is_color) {
            throw AlpacaException("Bayer offsets not supported", AlpacaError::PropertyNotImplemented);
        }
        return bayer_offsets(camera_info_.bayer_pattern).first;
    }

    int get_bayer_offset_y() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.is_color) {
            throw AlpacaException("Bayer offsets not supported", AlpacaError::PropertyNotImplemented);
        }
        return bayer_offsets(camera_info_.bayer_pattern).second;
    }

    int get_bin_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return bin_x_;
    }

    void set_bin_x(int bin_x) override { set_bin_locked(bin_x, bin_x); }

    int get_bin_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return bin_y_;
    }

    void set_bin_y(int bin_y) override { set_bin_locked(bin_y, bin_y); }

    // Decision 0020 rule 3: the state is the session's; a failed exposure reads Idle, and ImageReady and ImageArray
    // raise its failure (a sticky Error state once cascaded one transient ASI_EXP_FAILED into 18 ConformU issues).
    CameraState get_camera_state() const override {
        ensure_connected();
        return session_.camera_state();
    }

    int get_camera_x_size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.max_width : 0;
    }

    int get_camera_y_size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.max_height : 0;
    }

    bool get_can_abort_exposure() const override { return true; }

    bool get_can_asymmetric_bin() const override { return false; }

    bool get_can_fast_readout() const override { return can_get_control(ZWOControlType::HighSpeedMode); }

    bool get_can_get_cooler_power() const override { return can_get_control(ZWOControlType::CoolerPower); }

    bool get_can_pulse_guide() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ && camera_info_.has_st4_port;
    }

    bool get_can_set_ccd_temperature() const override { return can_get_control(ZWOControlType::TargetTemperature); }

    bool get_can_stop_exposure() const override { return true; }

    double get_ccd_temperature() const override {
        ensure_connected();
        long value = get_control_value_or_throw(ZWOControlType::Temperature);
        // Issue #722: the SDK answers success with 0 on the first temperature read
        // after open (an ASI533MC Pro shows the real value by ~250 ms). A genuine
        // 0.0 C must stay reportable, so only the first read of a connection, and
        // only while it reads 0, re-reads on a bounded budget (5 x 50 ms), then
        // returns what it has. Later reads are never delayed or cached.
        if (first_temperature_read_pending_.exchange(false) && value == 0) {
            for (int attempt = 0; attempt < kFirstTemperatureRetries && value == 0; ++attempt) {
                clock_.sleep_for(kFirstTemperatureRetryDelay);
                value = get_control_value_or_throw(ZWOControlType::Temperature);
            }
        }
        return static_cast<double>(value) / 10.0;
    }

    bool get_cooler_on() const override {
        ensure_connected();
        if (!can_get_control(ZWOControlType::CoolerOn)) {
            throw AlpacaException("This ZWO camera has no cooler", AlpacaError::PropertyNotImplemented);
        }
        long value = get_control_value_or_throw(ZWOControlType::CoolerOn);
        return value != 0;
    }

    void set_cooler_on(bool cooler_on) override {
        ensure_connected();
        if (!can_get_control(ZWOControlType::CoolerOn)) {
            throw AlpacaException("This ZWO camera has no cooler", AlpacaError::PropertyNotImplemented);
        }
        set_control_value_or_throw(ZWOControlType::CoolerOn, cooler_on ? 1 : 0);
    }

    double get_cooler_power() const override {
        ensure_connected();
        if (!can_get_control(ZWOControlType::CoolerPower)) {
            throw AlpacaException("This ZWO camera cannot report cooler power", AlpacaError::PropertyNotImplemented);
        }
        long value = get_control_value_or_throw(ZWOControlType::CoolerPower);
        return static_cast<double>(value);
    }

    double get_electrons_per_adu() const override {
        ensure_connected();
        // ASI_CAMERA_INFO.ElecPerADU is one static figure; ASCOM wants e-/ADU in the current mode, which follows
        // gain. The SDK has no per-gain query, so there is no honest value (issue #1277).
        throw AlpacaException("ElectronsPerADU is not available for ZWO cameras", AlpacaError::PropertyNotImplemented);
    }

    double get_exposure_max() const override {
        ensure_connected();
        auto caps = get_control_caps_or_throw(ZWOControlType::Exposure);
        return static_cast<double>(caps.max_value) / 1'000'000.0;
    }

    double get_exposure_min() const override {
        ensure_connected();
        auto caps = get_control_caps_or_throw(ZWOControlType::Exposure);
        return static_cast<double>(caps.min_value) / 1'000'000.0;
    }

    double get_exposure_resolution() const override { return 0.000001; }

    bool get_fast_readout() const override {
        ensure_connected();
        if (!can_get_control(ZWOControlType::HighSpeedMode)) {
            throw AlpacaException("Fast readout not supported", AlpacaError::NotImplemented);
        }
        long value = get_control_value_or_throw(ZWOControlType::HighSpeedMode);
        return value != 0;
    }

    void set_fast_readout(bool fast_readout) override {
        ensure_connected();
        if (!can_get_control(ZWOControlType::HighSpeedMode)) {
            throw AlpacaException("Fast readout not supported", AlpacaError::NotImplemented);
        }
        set_control_value_or_throw(ZWOControlType::HighSpeedMode, fast_readout ? 1 : 0);
    }

    double get_full_well_capacity() const override {
        ensure_connected();
        // The SDK has no full-well field; e-/ADU x max ADU would inherit the static ElecPerADU (see
        // get_electrons_per_adu), so there is no honest value (issue #1277).
        throw AlpacaException("FullWellCapacity is not available for ZWO cameras", AlpacaError::PropertyNotImplemented);
    }

    int get_gain() const override {
        ensure_connected();
        return static_cast<int>(get_control_value_or_throw(ZWOControlType::Gain));
    }

    void set_gain(int gain) override {
        ensure_connected();
        set_control_value_or_throw(ZWOControlType::Gain, gain);
    }

    int get_gain_max() const override {
        ensure_connected();
        auto caps = get_control_caps_or_throw(ZWOControlType::Gain);
        return static_cast<int>(caps.max_value);
    }

    int get_gain_min() const override {
        ensure_connected();
        auto caps = get_control_caps_or_throw(ZWOControlType::Gain);
        return static_cast<int>(caps.min_value);
    }

    std::vector<std::string> get_gains() const override {
        ensure_connected();
        throw AlpacaException("Gain descriptions not supported", AlpacaError::PropertyNotImplemented);
    }

    bool get_has_shutter() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ && camera_info_.has_shutter;
    }

    double get_heat_sink_temperature() const override { return get_ccd_temperature(); }

    // A copy of the frame the body downloaded; no SDK call (decision 0016 rule 1).
    ImageArray get_image_array() const override {
        ensure_connected();
        std::shared_ptr<const ImageArray> image;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ensure_connected();
            if (exposure_starting_) {
                throw AlpacaException::asked_too_early("Exposure start is in progress");
            }
            image = session_.image();
        }
        return *image;
    }

    std::string get_image_array_variant() const override { return "Int32"; }

    bool get_image_ready() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        if (exposure_starting_) {
            return false;  // the new start discards the previous frame
        }
        return session_.image_ready();
    }

    bool get_is_pulse_guiding() const override {
        if (!pulse_guiding_.load()) {
            return false;
        }
        // Expiry check and clear under ONE mutex_ hold: an unlocked
        // store(false) after the check could overwrite a concurrent
        // pulse_guide's fresh flag/end-time write and falsely report
        // "not guiding" mid-pulse (PR #119 round 4).
        std::lock_guard<std::mutex> lock(mutex_);
        if (clock_.now() >= pulse_guiding_end_) {
            pulse_guiding_.store(false);
            return false;
        }
        return true;
    }

    double get_last_exposure_duration() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        return session_.last_exposure_duration();
    }

    std::chrono::system_clock::time_point get_last_exposure_start_time() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        return session_.last_exposure_start_time();
    }

    int get_max_adu() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || camera_info_.bit_depth <= 0) {
            return 0;
        }
        return static_cast<int>((1ULL << camera_info_.bit_depth) - 1ULL);
    }

    int get_max_bin_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_info_.supported_bins.empty()) {
            return 1;
        }
        return *std::max_element(camera_info_.supported_bins.begin(), camera_info_.supported_bins.end());
    }

    int get_max_bin_y() const override { return get_max_bin_x(); }

    int get_num_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return num_x_;
    }

    void set_num_x(int num_x) override { set_roi_size_locked(num_x, std::nullopt); }

    int get_num_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return num_y_;
    }

    void set_num_y(int num_y) override { set_roi_size_locked(std::nullopt, num_y); }

    int get_offset() const override {
        ensure_connected();
        return static_cast<int>(get_control_value_or_throw(ZWOControlType::Offset));
    }

    void set_offset(int offset) override {
        ensure_connected();
        set_control_value_or_throw(ZWOControlType::Offset, offset);
    }

    int get_offset_max() const override {
        ensure_connected();
        auto caps = get_control_caps_or_throw(ZWOControlType::Offset);
        return static_cast<int>(caps.max_value);
    }

    int get_offset_min() const override {
        ensure_connected();
        auto caps = get_control_caps_or_throw(ZWOControlType::Offset);
        return static_cast<int>(caps.min_value);
    }

    std::vector<std::string> get_offsets() const override {
        ensure_connected();
        throw AlpacaException("Offset descriptions not supported", AlpacaError::PropertyNotImplemented);
    }

    double get_percent_completed() const override {
        ensure_connected();
        return session_.percent_completed();
    }

    double get_pixel_size_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.pixel_size_um : 0.0;
    }

    double get_pixel_size_y() const override { return get_pixel_size_x(); }

    int get_readout_mode() const override {
        if (!can_get_control(ZWOControlType::HighSpeedMode)) {
            return 0;
        }
        return get_fast_readout() ? 1 : 0;
    }

    void set_readout_mode(int mode) override {
        if (!can_get_control(ZWOControlType::HighSpeedMode)) {
            if (mode != 0) {
                ensure_connected();  // the caps are empty while disconnected (#1919)
                throw AlpacaException("Readout mode not supported", AlpacaError::NotImplemented);
            }
            return;
        }
        if (mode != 0 && mode != 1) {
            throw AlpacaException("Invalid readout mode", AlpacaError::InvalidValue);
        }
        set_fast_readout(mode == 1);
    }

    std::vector<std::string> get_readout_modes() const override {
        if (can_get_control(ZWOControlType::HighSpeedMode)) {
            return {"Normal", "High Speed"};
        }
        return {"Normal"};
    }

    std::string get_sensor_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.name : std::string();  // unknown: "" (ICameraV4 SensorName)
    }

    SensorType get_sensor_type() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_info_valid_ || !camera_info_.is_color) {
            return SensorType::Monochrome;
        }
        return SensorType::RGGB;
    }

    double get_set_ccd_temperature() const override {
        ensure_connected();
        long value = get_control_value_or_throw(ZWOControlType::TargetTemperature);
        return static_cast<double>(value);
    }

    void set_set_ccd_temperature(double temperature) override {
        ensure_connected();
        set_control_value_or_throw(ZWOControlType::TargetTemperature, static_cast<long>(std::lround(temperature)));
    }

    int get_start_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return start_x_;
    }

    void set_start_x(int start_x) override { set_start_pos_locked(start_x, std::nullopt); }

    int get_start_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return start_y_;
    }

    void set_start_y(int start_y) override { set_start_pos_locked(std::nullopt, start_y); }

    double get_sub_exposure_duration() const override {
        ensure_connected();
        throw AlpacaException("Sub-exposure duration not supported", AlpacaError::NotImplemented);
    }

    void set_sub_exposure_duration(double) override {
        ensure_connected();
        throw AlpacaException("Sub-exposure duration not supported", AlpacaError::NotImplemented);
    }

    void abort_exposure() override { stop_exposure_impl(true); }

    void pulse_guide(int direction, int duration) override {
        ensure_connected();
        if (!get_can_pulse_guide()) {
            throw AlpacaException("Pulse guide not supported", AlpacaError::NotImplemented);
        }

        if (direction < 0 || direction > 3) {
            throw AlpacaException("Invalid pulse guide direction", AlpacaError::InvalidValue);
        }
        if (duration <= 0) {
            throw AlpacaException("Invalid pulse guide duration", AlpacaError::InvalidValue);
        }

        ZWOGuideDirection guide_direction = ZWOGuideDirection::North;
        switch (direction) {
            case 0:
                guide_direction = ZWOGuideDirection::North;
                break;
            case 1:
                guide_direction = ZWOGuideDirection::South;
                break;
            case 2:
                guide_direction = ZWOGuideDirection::East;
                break;
            case 3:
                guide_direction = ZWOGuideDirection::West;
                break;
            default:
                break;
        }

        // Serialises the SDK write and the timer start against the stop paths (stop_pulse_timer), so a pulse either
        // lands before a Disconnect's join, which turns it off, or finds the camera closed.
        std::lock_guard<std::mutex> pulse_lifecycle(pulse_lifecycle_mutex_);
        {
            // The claim (decision 0006 rule 3): the SDK write, the generation bump and the flags under pulse_mutex_,
            // which the timer body holds from taking a turn-off to sending it, so a turn-off due from the running
            // body goes out before this pulse starts, or the body sees itself superseded and leaves it to the new one.
            // The bump then the flags under the driver mutex_, as the rule says.
            std::lock_guard<std::mutex> plock(pulse_mutex_);
            std::uint64_t pulse_link = 0;
            const int pulse_camera_id = with_camera([&](int id) {
                dev_.pulse_guide_on(id, guide_direction);
                pulse_link = open_link_.load();
                return id;
            });
            std::lock_guard<std::mutex> lock(mutex_);
            pulse_generation_.bump();
            pending_offs_.push_back(
                {clock_.now() + std::chrono::milliseconds(duration), pulse_camera_id, pulse_link, guide_direction});
            // End timestamp BEFORE the flag: a reader that observes the flag as
            // true must never see a stale end time from the previous pulse (or
            // epoch), or the self-clearing getter would clear this pulse
            // immediately. Pre-existing ordering, swept in the PR #119 round-2
            // fix alongside the Player One instance.
            pulse_guiding_end_ = latest_pending_deadline();
            pulse_guiding_.store(true);
        }
        // The new body supersedes the running one (decision 0006 rule 2) and takes over its pending turn-offs, so a
        // pulse on another axis still ends at its own time.
        try {
            pulse_slot_.start([this](util::OperationContext& ctx) { run_pulse_timer(ctx); });
        } catch (...) {
            end_pulses(util::TaskClock::clock::time_point::max(), nullptr);  // no timer will end them
            throw;
        }
    }

    void start_exposure(double duration, bool light) override {
        if (!std::isfinite(duration) || duration <= 0.0) {
            throw AlpacaException("Exposure duration must be positive", AlpacaError::InvalidValue);
        }
        ensure_connected();
        auto caps = get_control_caps_or_throw(ZWOControlType::Exposure);
        const double exposure_us_value = duration * 1'000'000.0;
        if (!std::isfinite(exposure_us_value) || exposure_us_value < static_cast<double>(caps.min_value) ||
            exposure_us_value > static_cast<double>(caps.max_value)) {
            throw AlpacaException("Exposure duration out of range", AlpacaError::InvalidValue);
        }
        const long exposure_us = static_cast<long>(std::lround(exposure_us_value));

        ExposureRequest request;
        request.exposure_us = exposure_us;
        request.is_dark = !light;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ensure_connected();
            if (!is_roi_valid_locked(num_x_, num_y_, start_x_, start_y_)) {
                throw AlpacaException("ROI is not valid for exposure", AlpacaError::InvalidValue);
            }
            if (!camera_id_.has_value()) {
                throw AlpacaException("Camera ID not set", AlpacaError::NotConnected);
            }
            if (exposure_starting_) {
                throw AlpacaException("An exposure start is already in progress", AlpacaError::InvalidOperation);
            }
            request.camera_id = camera_id_.value();
            request.frame = ExposureFrame{num_x_,
                                          num_y_,
                                          roi_width_effective_,
                                          roi_height_effective_,
                                          bin_x_,
                                          start_x_ - roi_crop_x_,
                                          start_y_ - roi_crop_y_,
                                          roi_crop_x_,
                                          roi_crop_y_,
                                          image_type_};
            request.formats = camera_info_.supported_formats;
            request.usb3 = camera_info_valid_ && camera_info_.is_usb3;
            // Holds the geometry setters (ensure_frame_geometry_mutable_locked) from this snapshot until
            // session_.start() has published Exposing.
            exposure_starting_ = true;
        }
        struct StartingDone {
            ZWOCameraDriver& driver;
            ~StartingDone() {
                std::lock_guard<std::mutex> lock(driver.mutex_);
                driver.exposure_starting_ = false;
            }
        } starting_done{*this};

        const double exposure_s = static_cast<double>(exposure_us) / 1'000'000.0;
        // Decision 0020 rule 4: the deadline holds the first attempt, both retries, their pacing, the USB 2.0 settle
        // and the download (start + 3 x duration + 15 s).
        const auto margin = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::duration<double>(kExposureRetries * exposure_s)) +
                            kWatchdogGrace;
        session_.start(exposure_s, [this, request](util::CaptureControl& c) { run_exposure(c, request); }, margin);
    }

    void stop_exposure() override { stop_exposure_impl(false); }

private:
    struct ExposureFrame {
        int width;
        int height;
        int effective_width;
        int effective_height;
        int bin;
        int start_x;
        int start_y;
        int crop_x;
        int crop_y;
        ZWOImageType image_type;
    };

    // What one exposure body needs, snapshotted by start_exposure under mutex_: the body reads no driver geometry.
    struct ExposureRequest {
        int camera_id{-1};
        long exposure_us{0};
        bool is_dark{false};
        ExposureFrame frame{};
        std::vector<ZWOImageType> formats;
        bool usb3{false};
    };

    // The exposure body (decision 0020 rule 4), on the session's thread with no driver lock held across an SDK call:
    // the USB 2.0 settle, the trigger, the status poll with the retries, the readback check, the download and the
    // frame. A NotConnected from a device call (a removal; on_link() has dropped the link) ends it at once.
    void run_exposure(util::CaptureControl& c, const ExposureRequest& request) {
        const int id = request.camera_id;
        try {
            if (!settle_after_usb2_download(c, request)) {
                return;
            }
            const auto first_trigger = clock_.now();
            if (!trigger_exposure(c, request, false)) {
                return;
            }
            // Issue #2187: a retry is one more full exposure, and a guide client waits for the frame for the exposure
            // plus a fixed grace before it drops the camera (PHD2: 15 s). A retry starts only when it can be expected
            // to end within kRetryBudget of the exposure's own end, the expectation being the length of the attempt
            // that just failed, or a full exposure when the failure came earlier (the ASI120MM Mini reports
            // ASI_EXP_FAILED 1.9-2.3 s after the trigger of a 1 s exposure); otherwise the failure is kept at once,
            // so the client hears of it and retakes its frame.
            const auto retry_ends_by = first_trigger + std::chrono::microseconds(request.exposure_us) + kRetryBudget;
            auto attempt_started = first_trigger;
            int retries_left = kExposureRetries;
            std::optional<util::TaskClock::clock::time_point> last_retry;
            for (;;) {
                ZWOExposureStatus status = ZWOExposureStatus::Idle;
                try {
                    status = dev_.get_exposure_status(id);
                } catch (const AlpacaException& e) {
                    if (e.error_code() == AlpacaError::NotConnected) throw;
                    throw AlpacaException("exposure status read failed: " + std::string(e.what()),
                                          AlpacaError::DriverException);
                }
                if (status == ZWOExposureStatus::Success) {
                    break;
                }
                if (status == ZWOExposureStatus::Failed) {
                    // The transient ASI_EXP_FAILED class (USB timing hiccups, the INDI ASI driver's policy): re-trigger
                    // up to kExposureRetries times before the failure is kept. Found by ConformU 4.4 on an ASI2600MM
                    // Pro. The retries are paced so a transient USB condition has a chance to clear.
                    if (retries_left <= 0) {
                        ALPACA_LOG_ERROR("ZWO", "exposure failed after " + std::to_string(kExposureRetries) +
                                                    " retries; reporting failure to the client");
                        throw AlpacaException("camera exposure failed after SDK retries", AlpacaError::DriverException);
                    }
                    // A retry is at least a full exposure, and takes as long as the attempt that just failed when
                    // the failure came late (the SDK reports it after the exposure's end on the rig).
                    const auto failed_at = clock_.now();
                    const auto attempt_took = failed_at - attempt_started;
                    const auto retry_length = std::max<util::TaskClock::clock::duration>(
                        attempt_took, std::chrono::microseconds(request.exposure_us));
                    if (failed_at + kExposureRetryInterval + retry_length > retry_ends_by) {
                        const auto took_ms =
                            std::chrono::duration_cast<std::chrono::milliseconds>(attempt_took).count();
                        ALPACA_LOG_ERROR("ZWO",
                                         "exposure failed (transient SDK/USB error); a retry would not end within " +
                                             std::to_string(kRetryBudget.count()) +
                                             " s of the exposure's end (this attempt took " + std::to_string(took_ms) +
                                             " ms); reporting failure to the client");
                        throw AlpacaException(
                            "camera exposure failed; a retry would run past the client's exposure wait",
                            AlpacaError::DriverException);
                    }
                    if (last_retry.has_value()) {
                        const auto since = clock_.now() - *last_retry;
                        if (since < kExposureRetryInterval && !c.wait_for(kExposureRetryInterval - since)) {
                            stop_left_running(id);
                            return;
                        }
                    }
                    last_retry = clock_.now();
                    --retries_left;
                    ALPACA_LOG_WARN("ZWO", "exposure failed (transient SDK/USB error); retrying, " +
                                               std::to_string(retries_left) + " retries left");
                    bool retried = false;
                    try {
                        attempt_started = clock_.now();
                        retried = trigger_exposure(c, request, true);
                    } catch (const AlpacaException& e) {
                        if (e.error_code() == AlpacaError::NotConnected) throw;
                        ALPACA_LOG_WARN("ZWO", "exposure retry re-trigger failed: " + printable(e.what()));
                        throw AlpacaException("exposure retry re-trigger failed: " + std::string(e.what()),
                                              AlpacaError::DriverException);
                    }
                    if (!retried) {
                        return;
                    }
                    continue;
                }
                if (!c.wait_for(kStatusPollInterval)) {
                    stop_left_running(id);
                    return;
                }
            }

            // Success: the frame is read only if the camera still reports the geometry this exposure was taken with.
            validate_frame_format(id, request.frame, request.formats);
            std::vector<std::uint8_t> buffer(image_buffer_size(request.frame));
            c.reading_out();
            // ASIGetDataAfterExp is the lazy, potentially multi-second USB transfer. It takes no timeout, so an
            // AbortExposure is refused in Download and a Disconnect waits for it (the 0020 `partial` note).
            dev_.get_data_after_exposure(id, buffer.data(), static_cast<long>(buffer.size()));
            {
                std::lock_guard<std::mutex> sdk_lock(exposure_sdk_mutex_);
                sdk_running_ = false;
            }
            last_download_ns_.store(clock_.now().time_since_epoch().count());
            ImageArray image = build_image_array(buffer, request.frame);
            util::validate_image_array(image);
            static_cast<void>(c.image_ready(std::make_shared<const ImageArray>(std::move(image))));
        } catch (const std::exception& e) {
            stop_left_running(id);
            if (c.stop_reason() != util::StopReason::None) {
                ALPACA_LOG_DEBUG("ZWO", "exposure body cancelled: " + printable(e.what()));
                return;
            }
            throw AlpacaException("ZWO image acquisition failed: " + std::string(e.what()),
                                  AlpacaError::DriverException);
        }
    }

    // Issue #1349. On a USB 2.0 camera (ASI120MM) a long exposure started 0.3-0.45 s after the
    // previous ASIGetDataAfterExp ends in ASI_EXP_FAILED, which the retry then repairs at the cost of a
    // second full exposure. Rig-measured on an ASI120MM Mini (beta5, SDK 1.40, 4-9 frames per point):
    // 5 s and 10 s exposures fail at those gaps and are clean from 0.5 s; 1, 2 and 3 s exposures never
    // failed at any gap from 0.15 s to 0.5 s. So only an exposure longer than
    // kUsb2SettleMinExposure waits, until kUsb2PostDownloadSettle after the download, and a guide
    // loop of short exposures restarts at once. Below 0.3 s the camera also delivers every other
    // frame early: the next frame becomes ready a fixed time after the previous download whatever the
    // start time, so it looks started by the camera itself; the wait avoids that for long exposures
    // and leaves short ones as they were. USB 3.0 cameras show neither.
    // The wait runs in the body (decision 0020 rule 4), so StartExposure returns at once. False when the body must
    // stop.
    bool settle_after_usb2_download(util::CaptureControl& c, const ExposureRequest& request) {
        const double duration_s = static_cast<double>(request.exposure_us) / 1'000'000.0;
        if (request.usb3 || duration_s <= kUsb2SettleMinExposure.count()) return true;
        const auto downloaded_ns = last_download_ns_.load();
        if (downloaded_ns == kNoDownload) return true;
        const util::TaskClock::clock::time_point downloaded_at{util::TaskClock::clock::duration(downloaded_ns)};
        const auto remaining = kUsb2PostDownloadSettle - (clock_.now() - downloaded_at);
        if (remaining <= util::TaskClock::clock::duration::zero()) return true;
        return c.wait_for(remaining);
    }

    // Writes the exposure register and triggers the exposure (a retry stops the failed one first), under
    // exposure_sdk_mutex_ so it is ordered with the driver's own stop: an exposure the driver already stopped is not
    // triggered (false). The stamp is the trigger, not setup latency (zwo.instructions.md). The register is written
    // again on a retry: the SDK is not guaranteed to keep it across an ASI_EXP_FAILED.
    bool trigger_exposure(util::CaptureControl& c, const ExposureRequest& request, bool retry) {
        std::lock_guard<std::mutex> sdk_lock(exposure_sdk_mutex_);
        if (stop_token_ == c.token()) {
            return false;
        }
        if (retry) {
            // A stop on a failed camera is a no-op; skipping it could make every retry fail at once.
            try {
                dev_.stop_exposure(request.camera_id);
            } catch (const std::exception& e) {
                if (!link_.is_open()) throw;  // a removal: no retry on a camera that is gone (issue #1667)
                ALPACA_LOG_DEBUG("ZWO", "best-effort stop before retry failed: " + printable(e.what()));
            }
            sdk_running_ = false;
        }
        dev_.set_control_value(request.camera_id, ZWOControlType::Exposure, request.exposure_us, false);
        c.exposure_started();
        dev_.start_exposure(request.camera_id, request.is_dark);
        sdk_running_ = true;
        return true;
    }

    // A body that ends early (a cancel, the watchdog, a throw) stops the SDK exposure it started, unless the driver
    // stopped it already or the camera is gone.
    void stop_left_running(int camera_id) {
        std::lock_guard<std::mutex> sdk_lock(exposure_sdk_mutex_);
        if (!sdk_running_) return;
        sdk_running_ = false;
        if (!link_.is_open()) return;
        try {
            dev_.stop_exposure(camera_id);
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG("ZWO", "stop_exposure on body exit failed: " + printable(e.what()));
        }
    }

    // The driver's SDK stop for a Disconnect or the destructor: marks the exposure that owns the session as
    // stopping (its failure is dropped, not kept) and records its token, so a body that has not triggered yet
    // never does, then stops the camera. Best effort: a failure is logged. Called without mutex_.
    void stop_sdk_exposure(std::optional<int> camera_id) {
        std::lock_guard<std::mutex> sdk_lock(exposure_sdk_mutex_);
        stop_token_ = session_.begin_stop();
        sdk_running_ = false;
        if (!camera_id.has_value()) return;
        try {
            dev_.stop_exposure(camera_id.value());
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("ZWO", "stop_exposure during disconnect failed: " + printable(e.what()));
        }
    }

    std::optional<int> current_camera_id() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_id_;
    }

    // Decision 0020 rule 5. AbortExposure and StopExposure do the same: nothing (and no SDK call) while Idle or Ready;
    // in Download, AbortExposure is refused and StopExposure ignored (ASIGetDataAfterExp cannot be cancelled); while
    // Exposing, the SDK stop with no driver mutex held, then the session's cancel and join. A failed SDK stop
    // leaves the exposure running and raises.
    void stop_exposure_impl(bool abort) {
        ensure_connected();
        int stop_id = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ensure_connected();
            if (!camera_id_.has_value()) {
                throw AlpacaException("Camera ID not set", AlpacaError::NotConnected);
            }
            stop_id = camera_id_.value();
        }
        const CameraState state = session_.camera_state();
        if (state == CameraState::Download) {
            if (abort) {
                throw AlpacaException("Cannot abort while image download is in progress",
                                      AlpacaError::InvalidOperation);
            }
            return;  // StopExposure is ignored once readout is in progress.
        }
        if (state != CameraState::Exposing) {
            return;
        }
        {
            std::lock_guard<std::mutex> sdk_lock(exposure_sdk_mutex_);
            // The exposure that owns the session is the one stopped, and its stopping mark (a body failure the
            // stop causes is dropped, not kept, #1745) is set in the same session hold as its token is read
            // (#2043). Before, the mark was the newest StartExposure's token, minted before its session start,
            // and set_stopping(true) ran apart from it: a StartExposure that began between this stop's state read
            // and its mark found the mark against its own token with no stopping flag, refused its trigger and
            // returned, and the session kept "the exposure ended without an image".
            stop_token_ = session_.begin_stop();
            try {
                dev_.stop_exposure(stop_id);
            } catch (...) {
                stop_token_ = 0;
                session_.set_stopping(false);  // the exposure runs on
                throw;
            }
            sdk_running_ = false;
        }
        // Cancels and joins the body: bounded by one status poll step or one SDK call.
        session_.abort();
    }

    int device_number_;
    ZwoCameraBinding binding_;
    std::optional<int> camera_id_;
    ZWOSDK& sdk_;
    util::TaskClock& clock_;
    const ZwoCameraTestHooks test_hooks_;
    std::string serial_number_;
    ZWOCameraInfo camera_info_;
    bool camera_info_valid_;

    std::unordered_map<ZWOControlType, ZWOControlCaps> control_caps_;

    // Connected, as epoch << 1 | open bit: begin_transition() and publish_open() in set_connected(), mark_lost() when a
    // device call shows the camera is gone (decision 0009 rule 2a).
    mutable util::LinkEpoch link_;
    // The open this driver holds (open_link_ 0 = none): written by set_connected() under transition_mutex_ only,
    // read by any thread running a device call. Both are set before publish_open() and reset (0, -1) by
    // release_open() before its close.
    std::atomic<int> open_id_{-1};
    std::atomic<std::uint64_t> open_link_{0};
    // A leaf lock (taken under mutex_ or with none held, takes no other): why a lost link refuses, and the log label.
    mutable std::mutex lost_mutex_;
    mutable std::string lost_reason_;
    std::string unit_label_{"ZWO camera"};
    // When the link was lost, on the task clock (decision 0009 rule 3): stamped once by the call that latched the loss,
    // consumed by the next Connect; a Disconnect keeps it for that Connect (0009 Alternative E). Its own lock is a leaf
    // (taken under mutex_ or with none held), never taken under lost_mutex_.
    mutable util::LinkLostStamp link_lost_at_;
    // Under transition_mutex_: the serial every Connect binds once one is known (in memory, not persisted).
    std::string bound_serial_;
    std::string reported_serial_;  // connect path only, under transition_mutex_
    std::string reported_name_;    // connect path only, under transition_mutex_
    // True while set_connected(true) runs its SDK handshake with mutex_ released.
    std::atomic<bool> connect_in_flight_{false};
    // True from connect until the first CCDTemperature read of the connection (#722).
    mutable std::atomic<bool> first_temperature_read_pending_{false};
    static constexpr int kFirstTemperatureRetries = 5;
    static constexpr std::chrono::milliseconds kFirstTemperatureRetryDelay{50};
    std::mutex transition_mutex_;
    mutable std::mutex mutex_;

    ZWOImageType image_type_;
    int bin_x_;
    int bin_y_;
    int num_x_;
    int num_y_;
    int roi_width_effective_;
    int roi_height_effective_;
    // Offset of the client-requested start inside the padded SDK ROI (the
    // applied start is shifted left/up when the pad would overflow the
    // sensor); build_image_array crops at this offset.
    int roi_crop_x_;
    int roi_crop_y_;
    int start_x_;
    int start_y_;

    // True from a StartExposure's settings snapshot until session_.start() has published Exposing (under mutex_).
    bool exposure_starting_{false};
    static constexpr int kExposureRetries = 2;
    // See settle_after_usb2_download(): the body waits up to this long before the trigger.
    static constexpr std::chrono::milliseconds kUsb2PostDownloadSettle{600};
    static constexpr std::chrono::duration<double> kUsb2SettleMinExposure{3.0};
    static constexpr std::int64_t kNoDownload = std::numeric_limits<std::int64_t>::min();
    // TaskClock time (ns since its epoch) of the last successful ASIGetDataAfterExp on this connection, or
    // kNoDownload. Written by the body, read by the next body; reset by a connect and a disconnect.
    mutable std::atomic<std::int64_t> last_download_ns_{kNoDownload};
    static constexpr std::chrono::milliseconds kExposureRetryInterval{250};
    // Issue #2187: a retry starts only when it is expected to end within this of the exposure's own end, measured
    // from the first trigger. PHD2 drops a camera whose frame is not ready 15 s past the exposure; 10 s leaves the
    // download and the client's poll inside that. A 1 s guide exposure keeps both retries (each attempt ~2.2 s on
    // the rig); a 5 s one keeps one. Since a retry counts at least one full exposure, a first failure reported more
    // than ~10 s after the trigger is never retried, whatever the exposure length, so a 10 s or longer exposure is
    // retried only when its failure comes within its first ~10 s.
    static constexpr std::chrono::seconds kRetryBudget{10};
    // The status poll's step (Player One's readiness step) and the session's default margin (Player One's grace,
    // the SVBONY margin); each start passes kExposureRetries x duration + this (decision 0020 rule 4).
    static constexpr std::chrono::milliseconds kStatusPollInterval{50};
    static constexpr std::chrono::seconds kWatchdogGrace{15};
    // A leaf of the exposure path, never held with mutex_: orders the body's trigger with the driver's SDK stop, so an
    // exposure the driver stopped is never triggered after it, and a stopped one is not stopped twice. Lock order:
    // exposure_sdk_mutex_ -> session mutex -> SDK-wrapper mutex.
    mutable std::mutex exposure_sdk_mutex_;
    // The session token of the exposure the driver stopped last (CaptureSession::begin_stop(), #2043); 0 when
    // the stop found no exposure. A body compares it with its own CaptureControl::token().
    std::uint64_t stop_token_{0};
    bool sdk_running_{false};  // the body triggered an SDK exposure nothing has stopped yet

    // Cleared by the timer after the last pending turn-off, so a fire-and-forget client that never polls
    // IsPulseGuiding does not leave it latched true (round 9); the getter also self-clears at pulse_guiding_end_, the
    // last pending deadline. Both written under mutex_.
    mutable std::atomic<bool> pulse_guiding_;
    util::TaskClock::clock::time_point pulse_guiding_end_;
    // A started pulse's turn-off: sent at `deadline` on the open it was started on, so it never reaches a camera
    // reopened since (decision 0009 rule 3).
    struct PendingOff {
        util::TaskClock::clock::time_point deadline;
        int camera_id;
        std::uint64_t link;
        ZWOGuideDirection direction;
    };
    // Serialises pulse_guide() with stop_pulse_timer(). Lock order: transition_mutex_, pulse_lifecycle_mutex_,
    // pulse_mutex_, mutex_; the timer body takes pulse_mutex_, then mutex_ for the flag.
    std::mutex pulse_lifecycle_mutex_;
    // The claim lock for pulse_slot_ (decision 0006 rule 3): guards pending_offs_, and pulse_guide() bumps
    // pulse_generation_ under it and mutex_.
    std::mutex pulse_mutex_;
    std::vector<PendingOff> pending_offs_;
    util::OperationGeneration pulse_generation_;
    // The pulse-guide timer body; Disconnect, Connect and the destructor cancel and join it (stop_pulse_timer), a
    // link loss cancels it.
    mutable util::AsyncOperation pulse_slot_{"zwo-camera-pulse", pulse_generation_, clock_};

    // Sends each pulse's turn-off when it is due. Superseded, it returns and the new body takes over the list;
    // cancelled, it ends every pending pulse now.
    void run_pulse_timer(util::OperationContext& ctx) {
        for (;;) {
            util::TaskClock::clock::time_point next = util::TaskClock::clock::time_point::max();
            {
                std::lock_guard<std::mutex> plock(pulse_mutex_);
                if (pending_offs_.empty()) {
                    return;
                }
                for (const auto& off : pending_offs_) {
                    next = std::min(next, off.deadline);
                }
            }
            const auto now = clock_.now();
            if (!ctx.wait_for(next > now ? next - now : util::TaskClock::clock::duration::zero())) {
                end_pulses(util::TaskClock::clock::time_point::max(), &ctx);
                return;
            }
            end_pulses(clock_.now(), &ctx);
        }
    }

    // IsPulseGuiding's end time: the last deadline of all pending pulses, so a short pulse on one axis never ends the
    // reported state of a longer one on the other. Caller holds pulse_mutex_.
    util::TaskClock::clock::time_point latest_pending_deadline() const {
        util::TaskClock::clock::time_point latest{};
        for (const auto& off : pending_offs_) {
            latest = std::max(latest, off.deadline);
        }
        return latest;
    }

    // Sends the turn-off of every pending pulse due by `due_by`, on its own link, and clears the flag once no pulse is
    // pending. From a body, only while the body is not superseded (decision 0006 rule 6). Takes pulse_mutex_, then
    // mutex_ for the flag.
    void end_pulses(util::TaskClock::clock::time_point due_by, const util::OperationContext* ctx) {
        std::lock_guard<std::mutex> plock(pulse_mutex_);
        if (ctx != nullptr && ctx->stop_reason() == util::StopReason::Superseded) {
            return;
        }
        auto due = std::stable_partition(pending_offs_.begin(), pending_offs_.end(),
                                         [&](const PendingOff& off) { return off.deadline > due_by; });
        if (due == pending_offs_.end()) {
            return;
        }
        std::vector<PendingOff> offs(std::make_move_iterator(due), std::make_move_iterator(pending_offs_.end()));
        pending_offs_.erase(due, pending_offs_.end());
        std::stable_sort(offs.begin(), offs.end(),
                         [](const PendingOff& a, const PendingOff& b) { return a.deadline < b.deadline; });
        for (const auto& off : offs) {
            try {
                // A stale link is refused by the table without touching the camera.
                sdk_.with_link(off.camera_id, off.link, [&] { sdk_.pulse_guide_off(off.camera_id, off.direction); });
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ZWO", "pulse guide off failed: " + printable(e.what()));
            }
        }
        if (pending_offs_.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            pulse_guiding_.store(false);
            pulse_guiding_end_ = {};
        }
    }

    // async_operation.h Rule 8: cancels and joins the timer, which sends every pending turn-off first. Called without
    // mutex_ held; Disconnect and Connect call it under transition_mutex_.
    void stop_pulse_timer() {
        std::lock_guard<std::mutex> pulse_lifecycle(pulse_lifecycle_mutex_);
        pulse_slot_.cancel_all_and_join();
    }

    void ensure_connected() const {
        if (!link_.is_open()) {
            throw_not_connected();
        }
    }

    [[noreturn]] void throw_not_connected() const {
        std::lock_guard<std::mutex> lost(lost_mutex_);
        throw AlpacaException(lost_reason_.empty() ? "Camera not connected" : lost_reason_, AlpacaError::NotConnected);
    }

    // Runs one device call under the open this driver holds. A call for another id than the open's is a stale
    // snapshot from before a reconnect (or any id after Disconnect): refused without touching the new link. A
    // NotConnected from the call (a removal code; the table already retired the open and closed the dead handle, or
    // refused a link it retired) drops the link at once, per decision 0009 rule 2a and #1656; any other error
    // surfaces as itself. Takes no driver lock but the leaves lost_mutex_ and the loss stamp's, one after the other, so
    // it may run with mutex_ held.
    template <typename Fn>
    auto on_link(int camera_id, Fn&& fn) const {
        using Result = decltype(fn());
        const auto captured = link_.load();
        if (camera_id != open_id_.load()) {
            throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
        }
        try {
            if constexpr (std::is_void_v<Result>) {
                sdk_.with_link(camera_id, open_link_.load(), [&] { fn(); });
            } else {
                std::optional<Result> result;
                sdk_.with_link(camera_id, open_link_.load(), [&] { result.emplace(fn()); });
                if (!result) {
                    throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
                }
                return std::move(*result);
            }
        } catch (const std::exception& e) {
            if (!util::is_link_loss(e)) {
                throw;
            }
            note_loss(captured, e);
            throw_not_connected();
        }
    }

    void note_loss(util::LinkEpoch::Word captured, const std::exception& e) const {
        std::string label;
        {
            std::lock_guard<std::mutex> lost(lost_mutex_);
            if (!link_.mark_lost(captured)) {
                return;  // a disconnect, a relink or another call got there first
            }
            // Publish with the loss latch, so a lifecycle clear cannot be followed by this old reason.
            label = unit_label_;
            lost_reason_ = label + " was removed from USB; Connect again when it is back";
        }
        // Decision 0009 rule 3: stamped once, on the task clock, by the call that latched the loss. After lost_mutex_,
        // which stays a leaf: a report held past a relink would stamp the new session, only read by its next Connect.
        link_lost_at_.note(clock_.now());
        // May run with mutex_ held, so cancel only (async_operation.h Rule 7); the next Disconnect, Connect or the
        // destructor joins the timer. Neither the slot nor the test callback may run under the leaf lost_mutex_.
        pulse_slot_.cancel();
        if (test_hooks_.after_loss_cancel) test_hooks_.after_loss_cancel();
        ALPACA_LOG_WARN("ZWO", label + " lost (" + printable(e.what()) + ")");
    }

    // Drops this driver's open, if any. A close that fails because the camera is gone is logged and swallowed (a
    // Disconnect after a replug must complete); any other close error surfaces. Caller holds transition_mutex_.
    void release_open() {
        const std::uint64_t link = open_link_.exchange(0);
        const int id = open_id_.exchange(-1);
        if (link == 0) {
            return;
        }
        try {
            sdk_.close_camera_link(id, link);
        } catch (const std::exception& e) {
            if (!util::is_link_loss(e)) {
                throw;
            }
            ALPACA_LOG_WARN("ZWO", "close of a removed camera: " + printable(e.what()));
        }
    }

    // The per-device SDK calls, each under this driver's open (on_link). Same names as ZWOSDK, so a call site reads
    // as before. Never used for enumeration or info lookups, which need no link.
    class OpenCalls {
    public:
        explicit OpenCalls(ZWOCameraDriver& driver) : driver_(driver) {}
        std::vector<ZWOControlCaps> get_control_caps(int id) {
            return driver_.on_link(id, [&] { return driver_.sdk_.get_control_caps(id); });
        }
        bool get_control_value(int id, ZWOControlType type, long& value, bool& is_auto) {
            return driver_.on_link(id, [&] { return driver_.sdk_.get_control_value(id, type, value, is_auto); });
        }
        void set_control_value(int id, ZWOControlType type, long value, bool is_auto) {
            driver_.on_link(id, [&] { driver_.sdk_.set_control_value(id, type, value, is_auto); });
        }
        ZWOROIFormat get_roi_format(int id) {
            return driver_.on_link(id, [&] { return driver_.sdk_.get_roi_format(id); });
        }
        void set_roi_format(int id, int width, int height, int bin, ZWOImageType type) {
            driver_.on_link(id, [&] { driver_.sdk_.set_roi_format(id, width, height, bin, type); });
        }
        ZWOStartPos get_start_pos(int id) {
            return driver_.on_link(id, [&] { return driver_.sdk_.get_start_pos(id); });
        }
        void set_start_pos(int id, int start_x, int start_y) {
            driver_.on_link(id, [&] { driver_.sdk_.set_start_pos(id, start_x, start_y); });
        }
        void start_exposure(int id, bool is_dark) {
            driver_.on_link(id, [&] { driver_.sdk_.start_exposure(id, is_dark); });
        }
        void stop_exposure(int id) {
            driver_.on_link(id, [&] { driver_.sdk_.stop_exposure(id); });
        }
        ZWOExposureStatus get_exposure_status(int id) {
            return driver_.on_link(id, [&] { return driver_.sdk_.get_exposure_status(id); });
        }
        void get_data_after_exposure(int id, std::uint8_t* buffer, long buffer_size) {
            driver_.on_link(id, [&] { driver_.sdk_.get_data_after_exposure(id, buffer, buffer_size); });
        }
        void pulse_guide_on(int id, ZWOGuideDirection direction) {
            driver_.on_link(id, [&] { driver_.sdk_.pulse_guide_on(id, direction); });
        }

    private:
        ZWOCameraDriver& driver_;
    };
    mutable OpenCalls dev_;
    // Declared after every member the exposure body touches: destroyed first, so the body is joined before they die.
    util::CaptureSession session_{"ZWO", clock_, kWatchdogGrace};

    // Decision 0020 rule 8: geometry is refused while an exposure starts, exposes or downloads, and while a cancelled
    // or failed body is still returning (it may still be in the SDK); a Ready frame is dropped by the write
    // (session_.discard_image(), which keeps LastExposure*). Caller holds mutex_.
    void ensure_frame_geometry_mutable_locked() const {
        const CameraState state = session_.camera_state();
        bool frame_ready = false;
        try {
            frame_ready = session_.image_ready();
        } catch (const AlpacaException&) {
            frame_ready = false;  // a kept failure: not ready
        }
        if (exposure_starting_ || state == CameraState::Exposing || state == CameraState::Download ||
            (session_.busy() && !frame_ready)) {
            throw AlpacaException("Cannot change camera ROI during an exposure", AlpacaError::InvalidOperation);
        }
    }

    // The SDK's ROI and start readback must match the exposure's frame. Takes no driver lock (the body calls it).
    void validate_frame_format(int camera_id, const ExposureFrame& frame, const std::vector<ZWOImageType>& formats) {
        const ZWOROIFormat actual = dev_.get_roi_format(camera_id);
        const ZWOStartPos actual_start = dev_.get_start_pos(camera_id);
        if (actual.width <= 0 || actual.height <= 0 || actual.bin <= 0 || actual.image_type == ZWOImageType::Unknown ||
            actual.width != frame.effective_width || actual.height != frame.effective_height ||
            actual.bin != frame.bin || actual.image_type != frame.image_type || actual_start.start_x != frame.start_x ||
            actual_start.start_y != frame.start_y || !supports_format(formats, actual.image_type)) {
            util::throw_invalid_camera_image("ZWO ROI readback does not match the exposure frame");
        }
        if (frame.width <= 0 || frame.height <= 0 || frame.crop_x < 0 || frame.crop_y < 0) {
            util::throw_invalid_camera_image("ZWO ROI has invalid requested geometry");
        }
    }

    void reset_exposure_state_locked() {
        last_download_ns_.store(kNoDownload);
        exposure_starting_ = false;
        // A disconnect racing an in-flight pulse must not leave
        // IsPulseGuiding=true for a freshly reconnected client.
        pulse_guiding_.store(false);
        pulse_guiding_end_ = {};
    }

    bool is_roi_valid_locked(int width, int height, int start_x, int start_y) const {
        if (!camera_info_valid_) {
            return false;
        }
        if (width <= 0 || height <= 0) {
            return false;
        }
        int max_width = camera_info_.max_width / bin_x_;
        int max_height = camera_info_.max_height / bin_y_;
        if (width > max_width || height > max_height) {
            return false;
        }
        if (start_x < 0 || start_y < 0) {
            return false;
        }
        if (start_x + width > max_width || start_y + height > max_height) {
            return false;
        }
        int adjusted_width = 0;
        int adjusted_height = 0;
        if (!adjust_roi_size_locked(width, height, adjusted_width, adjusted_height)) {
            return false;
        }
        return true;
    }

    // Align UP to the SDK divisor (pad), per the ascom-contract.instructions.md "Camera ROI
    // alignment" rule: keep the client-requested geometry for the interface,
    // pad the SDK ROI so it fully COVERS the request, and stride-crop the
    // delivered frame back to the requested size (ToupTek reference
    // approach) — never align down and fabricate black edge columns.
    int align_roi_dimension(int value, int multiple) const {
        if (multiple <= 1) {
            return value;
        }
        const int rem = value % multiple;
        return rem == 0 ? value : value + (multiple - rem);
    }

    bool adjust_roi_size_locked(int requested_width, int requested_height, int& adjusted_width,
                                int& adjusted_height) const {
        if (!camera_info_valid_) {
            return false;
        }
        if (requested_width <= 0 || requested_height <= 0) {
            return false;
        }

        int max_width = camera_info_.max_width / bin_x_;
        int max_height = camera_info_.max_height / bin_y_;
        if (requested_width > max_width || requested_height > max_height) {
            return false;
        }

        int candidate_width = align_roi_dimension(requested_width, 8);
        int candidate_height = align_roi_dimension(requested_height, 2);
        // Defensive: ZWO sensors have %8/%2 binned dimensions, so the padded
        // ROI normally still fits. If a pathological sensor size makes the
        // pad overflow, fall back to the largest aligned size that fits.
        if (candidate_width > max_width) {
            candidate_width = max_width - (max_width % 8);
        }
        if (candidate_height > max_height) {
            candidate_height = max_height - (max_height % 2);
        }
        if (candidate_width <= 0 || candidate_height <= 0) {
            return false;
        }

        adjusted_width = candidate_width;
        adjusted_height = candidate_height;
        return true;
    }

    // The padded ROI can push start+width past the sensor when the client
    // requests an edge-touching sub-frame; shift the applied start left/up so
    // the SDK window stays on-sensor while still covering the requested
    // region, and record the crop offset so build_image_array reads
    // the requested pixels out of the padded buffer (ToupTek approach).
    void apply_start_pos_locked(int active_camera_id) {
        int max_width = camera_info_.max_width / bin_x_;
        int max_height = camera_info_.max_height / bin_y_;
        int applied_x = std::min(start_x_, std::max(0, max_width - roi_width_effective_));
        int applied_y = std::min(start_y_, std::max(0, max_height - roi_height_effective_));
        applied_x = std::max(0, applied_x);
        applied_y = std::max(0, applied_y);
        roi_crop_x_ = start_x_ - applied_x;
        roi_crop_y_ = start_y_ - applied_y;
        dev_.set_start_pos(active_camera_id, applied_x, applied_y);
    }

    // Runs with mutex_ released: the sink writes the stored entry. A failed save never fails the connect.
    void report_identity_learned(const std::string& serial, const std::string& reported_name) {
        // A camera with no serial still stores its name, so later connects open only that model.
        if (!serial.empty() && binding_.connect_claims) binding_.connect_claims->claim(device_number_, serial);
        const std::string name = trim_zwo_name(reported_name);
        const bool new_serial = !serial.empty() && binding_.identity.serial.empty() && serial != reported_serial_;
        const bool new_name = binding_.identity.camera_name.empty() && !name.empty() && name != reported_name_;
        if ((!new_serial && !new_name) || !binding_.on_identity_learned) return;
        try {
            binding_.on_identity_learned(new_serial ? serial : std::string(), new_name ? name : std::string());
            reported_serial_ = serial;
            reported_name_ = name;
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("ZWO", "Could not store the learned camera identity: " + std::string(e.what()));
        }
    }

    // Touches no driver state, so it runs with mutex_ released.
    int resolve_camera_id() {
        std::vector<ZwoUnreadableSlot> unreadable;
        const auto found = sdk_.enumerate_identified_cameras(trim_zwo_name(binding_.identity.camera_name), &unreadable);
        // Once a serial is known, every Connect binds it, never the id or index the entry started from.
        ZwoConfiguredIdentity identity = binding_.identity;
        if (identity.serial.empty() && !bound_serial_.empty()) {
            identity.serial = bound_serial_;
        }
        std::set<std::string> claimed = binding_.claimed_serials;
        if (binding_.connect_claims) claimed.merge(binding_.connect_claims->others(device_number_));
        const auto result = resolve_zwo_camera(identity, found, claimed);
        if (!result.camera.has_value()) {
            // The camera this entry means may be the slot the SDK could not read: say so.
            std::string message = result.message;
            if (identity.serial.empty() && trim_zwo_name(identity.camera_name).empty()) {
                message = describe_unbound_zwo_camera(identity, message, &found);
            }
            const std::string hint = describe_unreadable_zwo_slots(unreadable);
            if (!hint.empty()) {
                message += ". " + hint;
            }
            ALPACA_LOG_WARN("ZWO", message);
            throw AlpacaException(message, result.failure == ZwoResolveFailure::IndexOutOfRange
                                               ? AlpacaError::InvalidValue
                                               : AlpacaError::NotConnected);
        }
        const auto& camera = result.camera.value();
        ALPACA_LOG_INFO("ZWO", "Using camera index " + std::to_string(camera.index) + ": " + camera.name + " (ID " +
                                   std::to_string(camera.camera_id) + ")");
        return camera.camera_id;
    }

    // The SDK read runs with mutex_ released: under LockedZWOSDK the decorator lock is taken before mutex_ on the
    // connect path, so holding mutex_ across the read would invert the order.
    void preload_camera_info_locked() {
        std::optional<int> camera_id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (camera_info_valid_) {
                return;
            }
            camera_id = camera_id_;
        }

        try {
            if (camera_id.has_value()) {
                ZWOCameraInfo info;
                if (sdk_.get_camera_info_by_id(camera_id.value(), info)) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    // Same rule as the refresh below: another camera at this id is not this entry (#1958).
                    const std::string expected = expected_model();
                    if (!expected.empty() && trim_zwo_name(info.name) != expected) {
                        return;
                    }
                    camera_info_ = info;
                    camera_info_valid_ = true;
                }
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG("ZWO", "Unable to preload camera info: " + std::string(e.what()));
        }
    }

    // The model this entry is bound to: its stored cameraName, trimmed. Empty when the entry has none, and then a
    // by-id read is taken as it is. Only an entry that has not connected since it was registered reads by id: a
    // Disconnect clears camera_id_, and the camera_info_ of the last Connect stays. binding_ is written only by the
    // constructor, so this needs no lock.
    std::string expected_model() const { return trim_zwo_name(binding_.identity.camera_name); }

    void refresh_cached_camera_info_if_needed() {
        // A connect in flight owns the SDK: the by-id read would wait out its open.
        if (link_.is_open() || connect_in_flight_.load()) {
            return;
        }

        std::optional<int> camera_id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            camera_id = camera_id_;
        }

        // By id only, and cheap (no camera is opened): the name is shown on
        // every management poll. The id is a hint; the next connect resolves
        // the camera again by serial, so a stale id is never rebound here.
        try {
            if (camera_id.has_value()) {
                ZWOCameraInfo info;
                if (sdk_.get_camera_info_by_id(camera_id.value(), info)) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    // A connect ran while the by-id read was out: it owns camera_info_ now
                    // (decision 0016 rule 6), and this read may come from a stale id.
                    if (link_.is_open() || connect_in_flight_.load() || camera_id_ != camera_id) {
                        return;
                    }
                    // The id is only a hint: while the bound camera is away the SDK can give the same id to
                    // another camera, and its model must not become this entry's name (issue #1958).
                    const std::string expected = expected_model();
                    if (!expected.empty() && trim_zwo_name(info.name) != expected) {
                        return;
                    }
                    camera_info_ = info;
                    camera_info_valid_ = true;
                }
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG("ZWO", "Unable to refresh camera info: " + std::string(e.what()));
        }
    }

    // Runs with mutex_ released: the caps and the camera name come from the connect in flight.
    void apply_usb_bandwidth(int camera_id, const std::unordered_map<ZWOControlType, ZWOControlCaps>& control_caps,
                             const std::string& camera_name) {
        const auto it = control_caps.find(ZWOControlType::BandwidthOverload);
        if (it == control_caps.end() || !it->second.is_writable) {
            if (binding_.usb_bandwidth) {
                throw AlpacaException("USB Limit is not writable on this camera; clear usbBandwidth in setup",
                                      AlpacaError::DriverException);
            }
            ALPACA_LOG_INFO("ZWO", camera_name + ": USB Limit unavailable; leaving SDK setting unchanged");
            return;
        }
        const auto& caps = it->second;
        if (caps.min_value > caps.max_value) {
            throw AlpacaException("USB Limit: camera reported an invalid range", AlpacaError::DriverException);
        }
        const auto requested = binding_.usb_bandwidth;
        if (requested &&
            (*requested < 0 || *requested > 100 || *requested < caps.min_value || *requested > caps.max_value)) {
            throw AlpacaException("USB Limit usbBandwidth must be within the camera range " +
                                      std::to_string(caps.min_value) + ".." + std::to_string(caps.max_value) +
                                      " (setup accepts 0..100)",
                                  AlpacaError::DriverException);
        }
        const auto default_bandwidth = static_cast<long>(catalog::kZwoCamUsbBandwidth.default_value);
        const long applied =
            requested ? static_cast<long>(*requested) : std::clamp(default_bandwidth, caps.min_value, caps.max_value);
        try {
            dev_.set_control_value(camera_id, ZWOControlType::BandwidthOverload, applied, false);
            long actual = 0;
            bool is_auto = false;
            if (!dev_.get_control_value(camera_id, ZWOControlType::BandwidthOverload, actual, is_auto)) {
                throw AlpacaException("camera did not return the USB Limit readback", AlpacaError::DriverException);
            }
            if (actual != applied || is_auto) {
                if (requested) {
                    throw AlpacaException("camera did not confirm the requested value in manual mode",
                                          AlpacaError::DriverException);
                }
                ALPACA_LOG_WARN("ZWO", camera_name + ": USB Limit default requested " +
                                           std::to_string(default_bandwidth) + " (clamped to " +
                                           std::to_string(applied) + "), read back " + std::to_string(actual) +
                                           (is_auto ? " auto" : " manual") + "; continuing connection");
                return;
            }
        } catch (const std::exception& e) {
            throw AlpacaException("USB Limit: " + std::string(e.what()), AlpacaError::DriverException);
        }
        ALPACA_LOG_INFO(
            "ZWO", camera_name + " (camera id " + std::to_string(camera_id) + "): USB Limit " +
                       (requested ? std::to_string(*requested) : "default " + std::to_string(default_bandwidth)) +
                       " applied " + std::to_string(applied) + " manual; range " + std::to_string(caps.min_value) +
                       ".." + std::to_string(caps.max_value) + ", SDK default " + std::to_string(caps.default_value));
    }

    static ZWOImageType select_default_image_type(const ZWOCameraInfo& camera_info, bool camera_info_valid) {
        if (!camera_info_valid) {
            return ZWOImageType::Raw8;
        }
        if (supports_format(camera_info.supported_formats, ZWOImageType::Raw16)) {
            return ZWOImageType::Raw16;
        }
        if (supports_format(camera_info.supported_formats, ZWOImageType::Raw8)) {
            return ZWOImageType::Raw8;
        }
        if (supports_format(camera_info.supported_formats, ZWOImageType::Y8)) {
            return ZWOImageType::Y8;
        }
        if (supports_format(camera_info.supported_formats, ZWOImageType::Rgb24)) {
            return ZWOImageType::Rgb24;
        }
        util::throw_invalid_camera_image("camera reports no supported image format");
    }

    bool can_get_control(ZWOControlType type) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return control_caps_.find(type) != control_caps_.end();
    }

    // Requires mutex_ held. The reference is only valid while the lock is.
    const ZWOControlCaps& control_caps_or_throw_locked(ZWOControlType type) const {
        auto it = control_caps_.find(type);
        if (it == control_caps_.end()) {
            throw AlpacaException("Control not supported", AlpacaError::NotImplemented);
        }
        return it->second;
    }

    // By value: returning a reference out of the locked map was a dangling
    // read once a reconnect reloaded control_caps_ (same audit as issue #116).
    ZWOControlCaps get_control_caps_or_throw(ZWOControlType type) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return control_caps_or_throw_locked(type);
    }

    // Caps check, id read, and SDK call under ONE mutex_ hold (driver-lifecycle.instructions.md
    // shape (a)) — the previous shape snapshotted the id and called the SDK
    // unlocked, racing a concurrent disconnect's close (issue #116).
    long get_control_value_or_throw(ZWOControlType type) const {
        std::lock_guard<std::mutex> lock(mutex_);
        control_caps_or_throw_locked(type);
        if (!camera_id_.has_value()) {
            throw AlpacaException("Camera ID not set", AlpacaError::NotConnected);
        }
        bool is_auto = false;
        long value = 0;
        if (!dev_.get_control_value(camera_id_.value(), type, value, is_auto)) {
            throw AlpacaException("Failed to get control value", AlpacaError::DriverException);
        }
        return value;
    }

    void set_control_value_or_throw(ZWOControlType type, long value) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto& caps = control_caps_or_throw_locked(type);
        if (!caps.is_writable) {
            throw AlpacaException("Control is read-only", AlpacaError::InvalidOperation);
        }
        if (value < caps.min_value || value > caps.max_value) {
            throw AlpacaException("Control value out of range", AlpacaError::InvalidValue);
        }
        if (!camera_id_.has_value()) {
            throw AlpacaException("Camera ID not set", AlpacaError::NotConnected);
        }
        dev_.set_control_value(camera_id_.value(), type, value, false);
    }

    // Runs fn(camera_id) while holding mutex_, so a concurrent disconnect
    // cannot close the camera underneath the SDK call (driver-lifecycle.instructions.md shape (a)).
    // Fast register/control calls only — never blocking waits or downloads.
    // Must NOT be called with mutex_ already held (non-recursive mutex).
    template <typename Fn>
    auto with_camera(Fn&& fn) const -> decltype(fn(0)) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_id_.has_value()) {
            throw AlpacaException("Camera ID not set", AlpacaError::NotConnected);
        }
        return fn(camera_id_.value());
    }

    void set_bin_locked(int bin_x, int bin_y) {
        ensure_connected();
        if (bin_x != bin_y) {
            throw AlpacaException("Asymmetric binning not supported", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_id_.has_value()) {
            throw AlpacaException("Camera ID not set", AlpacaError::NotConnected);
        }
        ensure_frame_geometry_mutable_locked();
        const int active_camera_id = camera_id_.value();
        if (!camera_info_valid_ || !supports_bin(camera_info_.supported_bins, bin_x)) {
            throw AlpacaException("Bin value not supported", AlpacaError::InvalidValue);
        }
        bin_x_ = bin_x;
        bin_y_ = bin_y;
        int max_width = camera_info_.max_width / bin_x_;
        int max_height = camera_info_.max_height / bin_y_;
        int width = 0;
        int height = 0;
        if (!adjust_roi_size_locked(max_width, max_height, width, height)) {
            throw AlpacaException("ROI size invalid for binning", AlpacaError::InvalidValue);
        }
        dev_.set_roi_format(active_camera_id, width, height, bin_x_, image_type_);
        dev_.set_start_pos(active_camera_id, 0, 0);
        num_x_ = max_width;
        num_y_ = max_height;
        roi_width_effective_ = width;
        roi_height_effective_ = height;
        roi_crop_x_ = 0;
        roi_crop_y_ = 0;
        start_x_ = 0;
        start_y_ = 0;
        session_.discard_image();
    }

    // width/height (or sx/sy) of std::nullopt means "leave that axis unchanged",
    // resolved UNDER mutex_: each public setter passes only its own axis, so a
    // concurrent setter for the other axis can no longer be clobbered by a stale
    // pre-lock get_num_x()/get_num_y() snapshot (lost-update TOCTOU).
    void set_roi_size_locked(std::optional<int> width_opt, std::optional<int> height_opt) {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_id_.has_value()) {
            throw AlpacaException("Camera ID not set", AlpacaError::NotConnected);
        }
        ensure_frame_geometry_mutable_locked();
        const int active_camera_id = camera_id_.value();
        const int width = width_opt.value_or(num_x_);
        const int height = height_opt.value_or(num_y_);
        if (width <= 0 || height <= 0) {
            throw AlpacaException("ROI size must be positive", AlpacaError::InvalidValue);
        }
        int adjusted_width = 0;
        int adjusted_height = 0;
        bool valid = adjust_roi_size_locked(width, height, adjusted_width, adjusted_height);
        num_x_ = width;
        num_y_ = height;
        if (valid && is_roi_valid_locked(width, height, start_x_, start_y_)) {
            roi_width_effective_ = adjusted_width;
            roi_height_effective_ = adjusted_height;
            dev_.set_roi_format(active_camera_id, adjusted_width, adjusted_height, bin_x_, image_type_);
            // The padded size may change how far the applied start must be
            // shifted to keep the SDK window on-sensor — re-apply it so the
            // crop offset stays consistent with the new effective size.
            apply_start_pos_locked(active_camera_id);
        }
        session_.discard_image();
    }

    void set_start_pos_locked(std::optional<int> start_x_opt, std::optional<int> start_y_opt) {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_id_.has_value()) {
            throw AlpacaException("Camera ID not set", AlpacaError::NotConnected);
        }
        ensure_frame_geometry_mutable_locked();
        const int active_camera_id = camera_id_.value();
        const int start_x = start_x_opt.value_or(start_x_);
        const int start_y = start_y_opt.value_or(start_y_);
        if (start_x < 0 || start_y < 0) {
            throw AlpacaException("Start position must be non-negative", AlpacaError::InvalidValue);
        }
        bool valid = is_roi_valid_locked(num_x_, num_y_, start_x, start_y);
        start_x_ = start_x;
        start_y_ = start_y;
        if (valid) {
            apply_start_pos_locked(active_camera_id);
        }
        session_.discard_image();
    }

    std::size_t image_buffer_size(const ExposureFrame& frame) const {
        if (frame.effective_width <= 0 || frame.effective_height <= 0) {
            util::throw_invalid_camera_image("ZWO frame dimensions are not positive");
        }
        std::size_t bytes_per_pixel = 0;
        switch (frame.image_type) {
            case ZWOImageType::Raw8:
            case ZWOImageType::Y8:
                bytes_per_pixel = 1;
                break;
            case ZWOImageType::Raw16:
                bytes_per_pixel = 2;
                break;
            case ZWOImageType::Rgb24:
                bytes_per_pixel = 3;
                break;
            case ZWOImageType::Unknown:
                util::throw_invalid_camera_image("ZWO frame format is unsupported");
        }
        const auto width = static_cast<std::size_t>(frame.effective_width);
        const auto height = static_cast<std::size_t>(frame.effective_height);
        constexpr std::size_t kMaxSize = std::numeric_limits<std::size_t>::max();
        if (width > kMaxSize / height || width * height > kMaxSize / bytes_per_pixel) {
            util::throw_invalid_camera_image("ZWO frame size overflows addressable storage");
        }
        const std::size_t bytes = width * height * bytes_per_pixel;
        if (bytes > static_cast<std::size_t>(std::numeric_limits<long>::max())) {
            util::throw_invalid_camera_image("ZWO frame size exceeds the SDK buffer limit");
        }
        return bytes;
    }

    ImageArray build_image_array(const std::vector<std::uint8_t>& buffer, const ExposureFrame& frame) const {
        const std::size_t required = image_buffer_size(frame);
        if (buffer.size() < required) {
            util::throw_invalid_camera_image("ZWO frame buffer is shorter than the reported ROI");
        }
        ImageArray image;
        image.width = frame.width;
        image.height = frame.height;
        const int out_width = frame.width;
        const int out_height = frame.height;
        const int eff_width = frame.effective_width;
        const int eff_height = frame.effective_height;
        if (frame.image_type == ZWOImageType::Rgb24) {
            image.rank = 3;
            image.data.resize(util::validate_image_shape(image).element_count);
            std::size_t buffer_stride = static_cast<std::size_t>(eff_width) * 3;
            for (int row = 0; row < out_height; ++row) {
                const std::int64_t src_row = static_cast<std::int64_t>(row) + frame.crop_y;
                for (int col = 0; col < out_width; ++col) {
                    const std::int64_t src_col = static_cast<std::int64_t>(col) + frame.crop_x;
                    std::size_t out_base = (static_cast<std::size_t>(row) * static_cast<std::size_t>(out_width) +
                                            static_cast<std::size_t>(col)) *
                                           3;
                    if (src_row < eff_height && src_col < eff_width) {
                        const std::size_t src_base =
                            static_cast<std::size_t>(src_row) * buffer_stride + static_cast<std::size_t>(src_col) * 3;
                        if (src_base + 2 < buffer.size()) {
                            // ASI RGB24 frames are BGR; Alpaca channels are RGB.
                            image.data[out_base] = buffer[src_base + 2];
                            image.data[out_base + 1] = buffer[src_base + 1];
                            image.data[out_base + 2] = buffer[src_base];
                            continue;
                        }
                    }
                    image.data[out_base] = 0;
                    image.data[out_base + 1] = 0;
                    image.data[out_base + 2] = 0;
                }
            }
            return image;
        }

        image.rank = 2;
        image.data.resize(util::validate_image_shape(image).element_count);
        if (frame.image_type == ZWOImageType::Raw16) {
            for (int row = 0; row < out_height; ++row) {
                for (int col = 0; col < out_width; ++col) {
                    std::size_t out_index = static_cast<std::size_t>(row) * static_cast<std::size_t>(out_width) +
                                            static_cast<std::size_t>(col);
                    const std::int64_t src_row = static_cast<std::int64_t>(row) + frame.crop_y;
                    const std::int64_t src_col = static_cast<std::int64_t>(col) + frame.crop_x;
                    if (src_row < eff_height && src_col < eff_width) {
                        const std::size_t offset =
                            (static_cast<std::size_t>(src_row) * static_cast<std::size_t>(eff_width) +
                             static_cast<std::size_t>(src_col)) *
                            2;
                        if (offset + 1 < buffer.size()) {
                            const std::uint16_t value = static_cast<std::uint16_t>(buffer[offset]) |
                                                        static_cast<std::uint16_t>(buffer[offset + 1] << 8);
                            image.data[out_index] = static_cast<std::int32_t>(value);
                            continue;
                        }
                    }
                    image.data[out_index] = 0;
                }
            }
            return image;
        }

        if (frame.image_type != ZWOImageType::Raw8 && frame.image_type != ZWOImageType::Y8) {
            util::throw_invalid_camera_image("ZWO frame format is unsupported");
        }
        for (int row = 0; row < out_height; ++row) {
            for (int col = 0; col < out_width; ++col) {
                std::size_t out_index =
                    static_cast<std::size_t>(row) * static_cast<std::size_t>(out_width) + static_cast<std::size_t>(col);
                const std::int64_t src_row = static_cast<std::int64_t>(row) + frame.crop_y;
                const std::int64_t src_col = static_cast<std::int64_t>(col) + frame.crop_x;
                if (src_row < eff_height && src_col < eff_width) {
                    const std::size_t buffer_index =
                        static_cast<std::size_t>(src_row) * static_cast<std::size_t>(eff_width) +
                        static_cast<std::size_t>(src_col);
                    if (buffer_index < buffer.size()) {
                        image.data[out_index] = buffer[buffer_index];
                        continue;
                    }
                }
                image.data[out_index] = 0;
            }
        }
        return image;
    }
};

std::unique_ptr<CameraDriver> create_zwo_camera(int device_number, int camera_id) {
    ZwoCameraBinding binding;
    binding.identity.camera_id = camera_id;
    binding.unique_id = generate_zwo_unique_id();
    return create_zwo_camera_bound(device_number, binding);
}

std::unique_ptr<CameraDriver> create_zwo_camera(int device_number, int camera_id, ZWOSDK& sdk, util::TaskClock& clock) {
    ZwoCameraBinding binding;
    binding.identity.camera_id = camera_id;
    binding.unique_id = generate_zwo_unique_id();
    return create_zwo_camera_bound(device_number, binding, sdk, clock);
}

std::unique_ptr<CameraDriver> create_zwo_camera(int device_number, int camera_id, ZWOSDK& sdk, util::TaskClock& clock,
                                                ZwoCameraTestHooks hooks) {
    ZwoCameraBinding binding;
    binding.identity.camera_id = camera_id;
    binding.unique_id = generate_zwo_unique_id();
    return std::make_unique<ZWOCameraDriver>(device_number, std::move(binding), sdk, clock, std::move(hooks));
}

std::unique_ptr<CameraDriver> create_zwo_camera_by_index(int device_number, int camera_index) {
    ZwoCameraBinding binding;
    binding.identity.camera_index = camera_index;
    binding.unique_id = generate_zwo_unique_id();
    return create_zwo_camera_bound(device_number, binding);
}

std::unique_ptr<CameraDriver> create_zwo_camera_bound(int device_number, const ZwoCameraBinding& binding) {
    return create_zwo_camera_bound(device_number, binding, ZWOSDKWrapper::instance());
}

std::vector<ZwoEnumeratedCamera> enumerate_zwo_cameras(const std::string& only_model_name) {
    return ZWOSDKWrapper::instance().enumerate_identified_cameras(trim_zwo_name(only_model_name));
}

std::unique_ptr<CameraDriver> create_zwo_camera_by_index(int device_number, int camera_index, ZWOSDK& sdk,
                                                         util::TaskClock& clock) {
    ZwoCameraBinding binding;
    binding.identity.camera_index = camera_index;
    binding.unique_id = generate_zwo_unique_id();
    return create_zwo_camera_bound(device_number, binding, sdk, clock);
}

std::unique_ptr<CameraDriver> create_zwo_camera_bound(int device_number, const ZwoCameraBinding& binding, ZWOSDK& sdk,
                                                      util::TaskClock& clock) {
    return std::make_unique<ZWOCameraDriver>(device_number, binding, sdk, clock);
}

}  // namespace alpacacore::vendor::zwo
