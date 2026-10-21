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
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/thermal_camera_choice.h>
#include <alpacacore/util/ttl_status_cache.h>
#include <alpacacore/vendor/zwo/zwo_camera_identity.h>
#include <alpacacore/vendor/zwo/zwo_sdk_wrapper.h>
#include <alpacacore/vendor/zwo/zwo_switch_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <thread>

namespace alpacacore::vendor::zwo {

namespace {

constexpr int kDewHeaterSwitchId = 0;

constexpr util::ThermalSwitchLabels kDewHeaterLabels{"ZWO", "dew heater"};

constexpr std::chrono::milliseconds kStatusTtl{100};

struct DewStatus {
    bool closed{};  // a disconnect won the I/O lock: not a frame, not a link failure
    long value{};
};

// Production SDK access: forwards to the process-wide wrapper.
class SingletonDewHeaterSdk final : public ZWODewHeaterSdk {
public:
    static SingletonDewHeaterSdk& instance() {
        static SingletonDewHeaterSdk sdk;
        return sdk;
    }
    std::vector<ZWOCameraInfo> enumerate_cameras() override { return ZWOSDKWrapper::instance().enumerate_cameras(); }
    std::vector<ZWOCameraInfo> enumerate_cameras_reporting_unreadable(
        std::vector<ZwoUnreadableSlot>* unreadable) override {
        return ZWOSDKWrapper::instance().enumerate_cameras(unreadable);
    }
    bool get_camera_info_by_id(int id, ZWOCameraInfo& info) override {
        return ZWOSDKWrapper::instance().get_camera_info_by_id(id, info);
    }
    void open_camera(int id) override { ZWOSDKWrapper::instance().open_camera(id); }
    void init_camera(int id) override { ZWOSDKWrapper::instance().init_camera(id); }
    void close_camera(int id) override { ZWOSDKWrapper::instance().close_camera(id); }
    std::vector<ZWOControlCaps> get_control_caps(int id) override {
        return ZWOSDKWrapper::instance().get_control_caps(id);
    }
    bool get_control_value(int id, ZWOControlType type, long& value, bool& is_auto) override {
        return ZWOSDKWrapper::instance().get_control_value(id, type, value, is_auto);
    }
    void set_control_value(int id, ZWOControlType type, long value, bool is_auto) override {
        ZWOSDKWrapper::instance().set_control_value(id, type, value, is_auto);
    }
    std::string get_serial_number(int id) override { return ZWOSDKWrapper::instance().get_serial_number(id); }
};

/// One attached camera as the dew heater sees it: what the picker shows, and
/// the SDK camera id that opens it.
struct ZwoDewHeaterCamera {
    util::ThermalCameraCandidate candidate;
    int camera_id{};
};

/// Every readable camera, each opened through the ref-counted wrapper (a camera
/// the camera driver holds stays open) to read its controls and serial. A
/// camera that will not open is listed without a heater or serial.
// TODO: confirm on hardware that ASIGetControlCaps and ASIGetSerialNumber on a
// camera the camera driver is exposing leave the frame intact; the web UI's
// picker can list cameras mid-session.
std::vector<ZwoDewHeaterCamera> list_dew_heater_cameras(ZWODewHeaterSdk& sdk,
                                                        std::vector<ZwoUnreadableSlot>* unreadable) {
    std::vector<ZwoDewHeaterCamera> result;
    for (const auto& info : sdk.enumerate_cameras_reporting_unreadable(unreadable)) {
        ZwoDewHeaterCamera camera;
        camera.camera_id = info.camera_id;
        camera.candidate.index = info.slot_index;
        camera.candidate.name = trim_zwo_name(info.name);
        try {
            sdk.open_camera(info.camera_id);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("ZWO", "Dew heater listing could not open " + camera.candidate.name + ": " + e.what());
            result.push_back(std::move(camera));
            continue;
        }
        try {
            for (const auto& cap : sdk.get_control_caps(info.camera_id)) {
                if (cap.type == ZWOControlType::AntiDewHeater) {
                    camera.candidate.has_thermal = true;
                }
            }
            // ASI120MM Mini and some older bodies report no serial; listed without one.
            camera.candidate.serial = sdk.get_serial_number(info.camera_id);
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG("ZWO", "Dew heater listing of " + camera.candidate.name + ": " + e.what());
        }
        try {
            sdk.close_camera(info.camera_id);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("ZWO", "Dew heater listing could not close " + camera.candidate.name + ": " + e.what());
        }
        result.push_back(std::move(camera));
    }
    return result;
}

std::vector<util::ThermalCameraCandidate> candidates_of(const std::vector<ZwoDewHeaterCamera>& cameras) {
    std::vector<util::ThermalCameraCandidate> candidates;
    candidates.reserve(cameras.size());
    for (const auto& camera : cameras) {
        candidates.push_back(camera.candidate);
    }
    return candidates;
}

} // namespace

class ZWODewHeaterSwitchDriver : public SwitchDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    // camera_index: an SDK slot, or util::kAutoThermalCamera for the one camera
    // with a heater. bound_serial, when set, names the camera and wins over both.
    ZWODewHeaterSwitchDriver(int device_number, std::optional<int> camera_id, std::optional<int> camera_index,
                             std::string bound_serial, ZWODewHeaterSdk& sdk)
        : AsyncConnectable("ZWO"),
          sdk_(sdk),
          status_cache_("ZWO dew heater", kStatusTtl, 3, "ZWO"),
          device_number_(device_number),
          camera_id_(camera_id),
          camera_index_(camera_index),
          bound_serial_(std::move(bound_serial)),
          serial_number_(),
          camera_name_("ZWO Camera"),
          dew_caps_(),
          connected_(false),
          switch_name_("DewHeater"),
          switch_description_("ZWO camera anti-dew heater control") {}

    ~ZWODewHeaterSwitchDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        if (connected_.load()) {
            try {
                set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("ZWO", "Error during dew heater destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override {
        return device_number_;
    }

    std::string get_name() const override {
        return "ZWO Dew Heater";
    }

    DeviceType get_device_type() const override {
        return DeviceType::Switch;
    }

    std::string fallback_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!serial_number_.empty()) {
            return "ZWO_DEW_SN_" + serial_number_;
        }
        return "ZWO_DEW_" + std::to_string(device_number_);
    }

    std::string get_description() const override {
        return "ZWO camera dew heater switch";
    }

    std::string get_driver_info() const override {
        return "AlpacaCore ZWO Dew Heater Switch";
    }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override {
        return 3;
    }

    bool get_connected() const override {
        return connected_.load();
    }

    void connect() override {
        start_connection_task(true);
    }

    void disconnect() override {
        start_connection_task(false);
    }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        // transition_mutex_ spans the whole call and no getter takes it: the connect handshake
        // below runs under it and the I/O lock, never under mutex_ (decision 0016).
        std::lock_guard<std::mutex> transition(transition_mutex_);
        // Base gates BEFORE the idempotency check: a sync disconnect during an
        // in-flight connect looks idempotent (both sides see disconnected) and
        // would be silently dropped without the record; a connect must honor a
        // newer pending disconnect by staying down.
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) {
            return;
        }
        if (connected && consume_pending_disconnect(connected_.load())) {
            return;
        }
        if (connected == connected_.load()) {
            return;
        }

        auto& sdk = sdk_;
        if (connected) {
            {
                std::lock_guard<std::mutex> io(io_mutex_);
                std::vector<ZwoDewHeaterCamera> listing;
                const int resolved_id = resolve_camera_id(listing);
                try {
                    sdk.open_camera(resolved_id);
                } catch (...) {
                    forget_unbound_camera_id();
                    throw;
                }
                // Guard everything after the open, init included: the ZWO open is
                // ref-counted, so an open left unclosed by a throw is never balanced.
                try {
                    sdk.init_camera(resolved_id);
                    publish_connect_state(resolved_id, listing);
                } catch (...) {
                    try {
                        sdk.close_camera(resolved_id);
                    } catch (const std::exception& e) {
                        ALPACA_LOG_WARN("ZWO",
                                        "close_camera after failed dew heater connect: " + std::string(e.what()));
                    }
                    forget_unbound_camera_id();
                    throw;
                }
            }
            // After the I/O lock is released: the cache lock is taken before it, never inside it.
            status_cache_.reset();
            connected_.store(true);
            return;
        }

        // Clear driver state and publish disconnected BEFORE the SDK close so
        // a racing operational call fails fast at ensure_connected() instead
        // of hitting a just-closed camera; one already past that check holds
        // the I/O lock, which the close below waits for -- same order as the
        // EFW/CAA/EAF siblings, per the driver-lifecycle.instructions.md
        // disconnect rule.
        std::optional<int> close_id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            close_id = camera_id_;
            if (!camera_bound_by_id_only()) {
                camera_id_.reset();
                camera_name_ = "ZWO Camera";
                serial_number_.clear();
            }
            dew_caps_.reset();
            connected_.store(false);
        }
        status_cache_.reset();
        if (close_id.has_value()) {
            std::lock_guard<std::mutex> io(io_mutex_);
            sdk.close_camera(close_id.value());
        }
    }

    std::vector<std::string> get_supported_actions() const override {
        return {};
    }

    std::string action(std::string_view action_name, std::string_view) override {
        throw AlpacaException("Action not supported: " + std::string(action_name),
                              AlpacaError::ActionNotImplemented);
    }

    bool can_action(std::string_view) const override {
        return false;
    }

    std::string command_blind(std::string_view, bool) override {
        throw AlpacaException("CommandBlind not supported", AlpacaError::NotImplemented);
    }

    bool command_bool(std::string_view, bool) override {
        throw AlpacaException("CommandBool not supported", AlpacaError::NotImplemented);
    }

    std::string command_string(std::string_view, bool) override {
        throw AlpacaException("CommandString not supported", AlpacaError::NotImplemented);
    }

    int get_max_switch() const override {
        return 1;
    }

    bool get_can_write(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        const auto caps = dew_caps_or_throw();
        return caps.is_writable;
    }

    bool get_can_async(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return false;
    }

    bool get_switch(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        double value = get_switch_value(id);
        return value > get_min_switch_value(id);
    }

    void set_switch(int id, bool state) override {
        validate_switch_id(id);
        ensure_connected();
        double value = state ? get_max_switch_value(id) : get_min_switch_value(id);
        set_switch_value(id, value);
    }

    void set_async(int id, bool /*state*/) override {
        validate_switch_id(id);
        ensure_connected();
        throw AlpacaException("Async switch control not supported", AlpacaError::NotImplemented);
    }

    double get_switch_value(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        dew_caps_or_throw();
        const int camera_id = camera_id_value();
        // The refill runs under the cache's own mutex, then the I/O lock (lock order: cache mutex, I/O lock,
        // mutex_), so it must not take mutex_ and no caller may hold mutex_ or the I/O lock around the cache.
        const DewStatus status = status_cache_.get([this, camera_id] {
            std::lock_guard<std::mutex> io(io_mutex_);
            DewStatus fresh;
            if (!connected_.load()) {
                // A disconnect that won the I/O lock has closed the camera. Returning (not throwing) keeps
                // it out of the link-health count; the disconnect's reset() drops this frame.
                fresh.closed = true;
                return fresh;
            }
            bool is_auto = false;
            if (!sdk_.get_control_value(camera_id, ZWOControlType::AntiDewHeater, fresh.value, is_auto)) {
                throw AlpacaException("Failed to read dew heater value", AlpacaError::DriverException);
            }
            return fresh;
        });
        if (status.closed) {
            throw AlpacaException("Dew heater not connected", AlpacaError::NotConnected);
        }
        return static_cast<double>(status.value);
    }

    void set_switch_value(int id, double value) override {
        validate_switch_id(id);
        ensure_connected();
        const auto caps = dew_caps_or_throw();
        if (!caps.is_writable) {
            throw AlpacaException("Dew heater is read-only", AlpacaError::InvalidOperation);
        }
        // std::lround on NaN/Inf is undefined behaviour; reject non-finite
        // values from the HTTP API as InvalidValue first.
        if (!std::isfinite(value)) {
            throw AlpacaException("Dew heater value must be a finite number", AlpacaError::InvalidValue);
        }
        long value_long = static_cast<long>(std::lround(value));
        if (value_long < caps.min_value || value_long > caps.max_value) {
            throw AlpacaException("Dew heater value out of range", AlpacaError::InvalidValue);
        }
        {
            std::lock_guard<std::mutex> io(io_mutex_);
            ensure_connected();  // a disconnect that won the I/O lock has closed the camera
            sdk_.set_control_value(camera_id_value(), ZWOControlType::AntiDewHeater, value_long, false);
        }
        status_cache_.invalidate();
    }

    void set_async_value(int id, double /*value*/) override {
        validate_switch_id(id);
        ensure_connected();
        throw AlpacaException("Async switch control not supported", AlpacaError::NotImplemented);
    }

    bool get_state_change_complete(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return true;
    }

    std::string get_switch_name(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return switch_name_;
    }

    void set_switch_name(int id, const std::string& name) override {
        validate_switch_id(id);
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        switch_name_ = name;
    }

    std::string get_switch_description(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return switch_description_ + " (" + camera_name_ + ")";
    }

    double get_min_switch_value(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return static_cast<double>(dew_caps_or_throw().min_value);
    }

    double get_max_switch_value(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return static_cast<double>(dew_caps_or_throw().max_value);
    }

    double get_switch_step(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return 1.0;
    }

private:
    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Dew heater not connected", AlpacaError::NotConnected);
        }
    }

    void validate_switch_id(int id) const {
        if (id != kDewHeaterSwitchId) {
            throw AlpacaException("Switch ID out of range", AlpacaError::InvalidValue);
        }
    }

    // Return by VALUE: a reference into the mutex-guarded optional dangles as
    // soon as the lock releases if a concurrent disconnect .reset()s it —
    // same fix as the camera's control-caps accessor (issue #116).
    ZWOControlCaps dew_caps_or_throw() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!dew_caps_.has_value()) {
            throw AlpacaException("Dew heater not supported", AlpacaError::NotImplemented);
        }
        return dew_caps_.value();
    }

    // A cameraId with no index or serial is the legacy binding: that SDK id, as is.
    bool camera_bound_by_id_only() const { return !camera_index_.has_value() && bound_serial_.empty(); }

    // Picks the camera this switch means. The legacy id-only binding returns its
    // id without listing (a listing opens every attached camera, and an id-only
    // switch must open only its own); an index or serial binding lists the
    // attached cameras into `listing`, which the capability refusal names too.
    // Caller holds io_mutex_ (the listing opens cameras); the bound id and name publish under mutex_.
    int resolve_camera_id(std::vector<ZwoDewHeaterCamera>& listing) {
        if (camera_bound_by_id_only()) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (camera_id_.has_value()) {
                return camera_id_.value();
            }
            throw AlpacaException("Camera ID not specified", AlpacaError::InvalidValue);
        }

        std::vector<ZwoUnreadableSlot> unreadable;
        listing = list_dew_heater_cameras(sdk_, &unreadable);
        util::ThermalCameraRequest request;
        request.camera_index = camera_index_.value_or(util::kAutoThermalCamera);
        request.serial = bound_serial_;
        const auto choice = util::choose_thermal_camera(request, candidates_of(listing), kDewHeaterLabels);
        if (!choice.chosen.has_value()) {
            // The camera this entry means may be the slot the SDK could not read: say so.
            std::string message = choice.refusal;
            const std::string hint = describe_unreadable_zwo_slots(unreadable);
            if (!hint.empty()) {
                message += " " + hint;
            }
            ALPACA_LOG_WARN("ZWO", message);
            throw AlpacaException(message, choice.error_code);
        }
        const auto& camera = listing[choice.chosen.value()];
        ALPACA_LOG_INFO("ZWO", "Dew heater using " + util::describe_thermal_camera(camera.candidate));
        std::lock_guard<std::mutex> lock(mutex_);
        camera_id_ = camera.camera_id;
        camera_name_ = camera.candidate.name;
        return camera.camera_id;
    }

    // A failed connect forgets a camera it resolved by index or serial; an id-only binding keeps its id.
    void forget_unbound_camera_id() {
        if (!camera_bound_by_id_only()) {
            std::lock_guard<std::mutex> lock(mutex_);
            camera_id_.reset();
        }
    }

    // Reads the camera name, heater capability and serial from the open camera, then publishes them under
    // mutex_ (short). Caller holds io_mutex_ and not mutex_.
    void publish_connect_state(int camera_id, std::vector<ZwoDewHeaterCamera>& listing) {
        std::string name;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            name = camera_name_;
        }
        ZWOCameraInfo info;
        if (sdk_.get_camera_info_by_id(camera_id, info) && !info.name.empty()) {
            name = info.name;
        }
        const ZWOControlCaps caps = find_dew_caps(camera_id, listing, name);
        const std::string serial = sdk_.get_serial_number(camera_id);
        std::lock_guard<std::mutex> lock(mutex_);
        camera_name_ = name;
        dew_caps_ = caps;
        serial_number_ = serial;
    }

    // The heater control of the open camera, or the refusal that names the cameras that have one.
    ZWOControlCaps find_dew_caps(int camera_id, std::vector<ZwoDewHeaterCamera>& listing,
                                 const std::string& camera_name) {
        for (const auto& cap : sdk_.get_control_caps(camera_id)) {
            if (cap.type == ZWOControlType::AntiDewHeater) {
                return cap;
            }
        }
        // An id-only binding connected without a listing; the refusal names the
        // other cameras, so list them now (a failed connect, not the usual path).
        if (listing.empty() && camera_bound_by_id_only()) {
            listing = list_dew_heater_cameras(sdk_, nullptr);
        }
        util::ThermalCameraCandidate bound;
        bound.name = camera_name;
        bound.index = -1;
        for (const auto& camera : listing) {
            if (camera.camera_id == camera_id) {
                bound = camera.candidate;
            }
        }
        const std::string message = util::thermal_capability_refusal(bound, candidates_of(listing), kDewHeaterLabels);
        ALPACA_LOG_WARN("ZWO", message);
        throw AlpacaException(message, AlpacaError::NotImplemented);
    }

    int camera_id_value() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_id_.has_value()) {
            throw AlpacaException("Camera ID not set", AlpacaError::NotConnected);
        }
        return camera_id_.value();
    }

    ZWODewHeaterSdk& sdk_;
    mutable util::TtlStatusCache<DewStatus> status_cache_;
    int device_number_;
    std::optional<int> camera_id_;
    std::optional<int> camera_index_;
    std::string bound_serial_;
    std::string serial_number_;
    std::string camera_name_;
    std::optional<ZWOControlCaps> dew_caps_;
    std::atomic<bool> connected_;
    // Lock order: transition_mutex_, then the status cache's mutex, then io_mutex_, then mutex_ (a leaf:
    // short state reads and writes, never held across an SDK call), then the SDK wrapper's own mutex.
    // transition_mutex_ spans set_connected() only and no getter takes it; io_mutex_ serialises the
    // device transactions (connect, close, the heater write, the status refill). Never take the cache
    // mutex (reset/invalidate) while holding io_mutex_ or mutex_.
    std::mutex transition_mutex_;
    mutable std::mutex io_mutex_;
    mutable std::mutex mutex_;
    std::string switch_name_;
    std::string switch_description_;
};

std::unique_ptr<SwitchDriver> create_zwo_dew_heater_switch(int device_number, int camera_id) {
    return std::make_unique<ZWODewHeaterSwitchDriver>(device_number, camera_id, std::nullopt, std::string(),
                                                      SingletonDewHeaterSdk::instance());
}

std::unique_ptr<SwitchDriver> create_zwo_dew_heater_switch(int device_number, int camera_id, ZWODewHeaterSdk& sdk) {
    return std::make_unique<ZWODewHeaterSwitchDriver>(device_number, camera_id, std::nullopt, std::string(), sdk);
}

std::unique_ptr<SwitchDriver> create_zwo_dew_heater_switch_by_index(int device_number, int camera_index,
                                                                    ZWODewHeaterSdk& sdk,
                                                                    const std::string& camera_serial) {
    return std::make_unique<ZWODewHeaterSwitchDriver>(device_number, std::nullopt, camera_index, camera_serial, sdk);
}

std::unique_ptr<SwitchDriver> create_zwo_dew_heater_switch_by_index(int device_number, int camera_index,
                                                                    const std::string& camera_serial) {
    return std::make_unique<ZWODewHeaterSwitchDriver>(device_number, std::nullopt, camera_index, camera_serial,
                                                      SingletonDewHeaterSdk::instance());
}

std::vector<util::ThermalCameraCandidate> list_zwo_dew_heater_cameras() {
    return candidates_of(list_dew_heater_cameras(SingletonDewHeaterSdk::instance(), nullptr));
}

} // namespace alpacacore::vendor::zwo
