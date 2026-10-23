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
#include <alpacacore/vendor/touptek/touptek_sdk_wrapper.h>
#include <alpacacore/vendor/touptek/touptek_thermal_switch_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace alpacacore::vendor::touptek {

namespace {

constexpr const char* kLogTag = "ToupTek";

// A camera exposes at most a dew heater, a fan, and the tail LED. Used as the
// switch-ID bound while disconnected, before the per-model element list is probed.
constexpr int kMaxThermalElements = 3;

enum class ThermalElementKind : std::uint8_t { DewHeater, Fan, TailLight };

struct ThermalElement {
    ThermalElementKind kind{};
    std::string name;
    std::string description;
    bool writable{};
    long min_value{};
    long max_value{};
};

constexpr util::ThermalSwitchLabels kThermalLabels{"ToupTek", "dew heater or fan"};

// From the enumeration flags alone, no open: the index is the position in
// enumerate_cameras(), which is what cameraIndex has always meant. The tail
// light has no flag, so a camera with only that is listed without controls
// (Auto skips it; a pinned index still binds it and the probe decides).
std::vector<util::ThermalCameraCandidate> thermal_candidates(const std::vector<ToupCameraInfo>& cameras) {
    std::vector<util::ThermalCameraCandidate> candidates;
    candidates.reserve(cameras.size());
    for (std::size_t i = 0; i < cameras.size(); ++i) {
        util::ThermalCameraCandidate candidate;
        candidate.index = static_cast<int>(i);
        candidate.name = cameras[i].name;
        candidate.has_thermal = cameras[i].supports_heat || (cameras[i].supports_fan && cameras[i].max_fan_speed > 0);
        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

}  // namespace

class ToupTekThermalSwitchDriver : public SwitchDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    ToupTekThermalSwitchDriver(int device_number, int camera_index, ToupTekSDK& sdk)
        : AsyncConnectable(kLogTag),
          sdk_(sdk),
          device_number_(device_number),
          camera_index_(camera_index),
          camera_name_("ToupTek Camera"),
          connected_(false),
          status_cache_("ToupTek thermal switch", kStatusTtl, 3, kLogTag) {}

    ~ToupTekThermalSwitchDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        if (connected_.load()) {
            try {
                ToupTekThermalSwitchDriver::set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(kLogTag, "Error during thermal switch destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override { return "ToupTek Thermal Switch"; }

    DeviceType get_device_type() const override { return DeviceType::Switch; }

    std::string fallback_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!serial_number_.empty()) {
            return "TOUPTEK_THERMALSW_SN_" + serial_number_;
        }
        return "TOUPTEK_THERMALSW_" + std::to_string(device_number_);
    }

    std::string get_description() const override { return "ToupTek camera dew heater and fan switch"; }

    std::string get_driver_info() const override { return "AlpacaCore ToupTek Thermal Switch"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 3; }

    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }
    void disconnect() override { start_connection_task(false); }
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
                const auto cameras = sdk.enumerate_cameras();
                const auto candidates = thermal_candidates(cameras);
                util::ThermalCameraRequest request;
                request.camera_index = camera_index_;
                const auto choice = util::choose_thermal_camera(request, candidates, kThermalLabels);
                if (!choice.chosen.has_value()) {
                    ALPACA_LOG_WARN(kLogTag, choice.refusal);
                    throw AlpacaException(choice.refusal, choice.error_code);
                }
                const auto& info = cameras[choice.chosen.value()];
                bound_ = candidates[choice.chosen.value()];
                listed_ = candidates;

                // Shared, reference-counted open — coexists with the camera device.
                HToupcam handle = sdk.open_camera_by_id(info.id);
                try {
                    std::string serial_number = sdk.get_serial_number(handle);
                    std::vector<ThermalElement> elements = build_elements(sdk, handle, info);
                    handle_ = handle;
                    // The short state lock publishes identity and the element list at the end of connect.
                    std::lock_guard<std::mutex> lock(mutex_);
                    camera_id_ = info.id;
                    camera_name_ = info.name;
                    serial_number_ = std::move(serial_number);
                    probed_element_count_ = static_cast<int>(elements.size());
                    elements_ = std::move(elements);
                } catch (...) {
                    handle_ = nullptr;
                    sdk.close_camera(handle);
                    throw;
                }
            }
            // After the I/O lock is released: the cache lock is taken before it, never inside it.
            status_cache_.reset();
            connected_.store(true);
            return;
        }

        // Clear driver state before the SDK close: if the close throws the error still surfaces,
        // but the driver must not stay half-connected.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connected_.store(false);
            camera_id_.clear();
            elements_.clear();
            // Clear the serial so get_unique_id() falls back to the device-number
            // form while disconnected instead of reporting the previous camera's
            // SN-based ID (matches the camera driver's disconnect path).
            serial_number_.clear();
            camera_name_ = "ToupTek Camera";  // ctor default; full cleanup symmetry
        }
        status_cache_.reset();
        // Waits for a read in flight: the shared handle is never closed under one.
        std::lock_guard<std::mutex> io(io_mutex_);
        if (handle_) {
            const HToupcam handle = handle_;
            handle_ = nullptr;
            sdk.close_camera(handle);
        }
    }

    std::vector<std::string> get_supported_actions() const override { return {}; }

    std::string action(std::string_view action_name, std::string_view) override {
        throw AlpacaException("Action not supported: " + std::string(action_name), AlpacaError::ActionNotImplemented);
    }

    bool can_action(std::string_view) const override { return false; }

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
        std::lock_guard<std::mutex> lock(mutex_);
        if (connected_.load()) {
            return static_cast<int>(elements_.size());
        }
        // Once we have probed this camera, keep reporting that count so MaxSwitch
        // doesn't jump between connect states. Before the first connect the real
        // count is unknown, so report the worst-case upper bound.
        return probed_element_count_ > 0 ? probed_element_count_ : kMaxThermalElements;
    }

    bool get_can_write(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return element_copy(id).writable;
    }

    bool get_can_async(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return false;
    }

    bool get_switch(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        const ThermalElement element = element_copy(id);
        return static_cast<double>(read_value(id)) > static_cast<double>(element.min_value);
    }

    void set_switch(int id, bool state) override {
        validate_switch_id(id);
        ensure_connected();
        // Resolve the target level (max for on / min for off) from one element copy and write it
        // against that copy, so the level always belongs to the element it is written to.
        const ThermalElement element = element_copy(id);
        if (!element.writable) {
            throw AlpacaException(element.name + " is read-only", AlpacaError::InvalidOperation);
        }
        // The element's own min_value/max_value are trivially in range, so no
        // separate bounds check is needed (unlike the free-value set path).
        write_element_value(id, element, state ? element.max_value : element.min_value);
    }

    void set_async(int id, bool /*state*/) override {
        validate_switch_id(id);
        ensure_connected();
        throw AlpacaException("Async switch control not supported", AlpacaError::NotImplemented);
    }

    double get_switch_value(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        (void)element_copy(id);  // NotConnected / InvalidValue, as before the cache
        return static_cast<double>(read_value(id));
    }

    // Writes value_long to the element's SDK control under the I/O lock, which a disconnect takes before
    // it closes the shared handle (no use-after-close). `element` is the copy value_long was resolved
    // against; if a reconnect to another model replaced the element since, the write is refused rather
    // than sent to a different control.
    void write_element_value(int id, const ThermalElement& element, long value_long) {
        {
            std::lock_guard<std::mutex> io(io_mutex_);
            if (!connected_.load() || !handle_) {
                throw AlpacaException("Camera handle not available", AlpacaError::NotConnected);
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto index = static_cast<std::size_t>(id);
                if (index >= elements_.size() || elements_[index].kind != element.kind ||
                    elements_[index].max_value != element.max_value) {
                    throw AlpacaException("Camera handle not available", AlpacaError::NotConnected);
                }
            }
            auto& sdk = sdk_;
            switch (element.kind) {
                case ThermalElementKind::DewHeater:
                    sdk.put_heat(handle_, static_cast<int>(value_long));
                    break;
                case ThermalElementKind::Fan:
                    sdk.put_fan(handle_, static_cast<int>(value_long));
                    break;
                case ThermalElementKind::TailLight:
                    sdk.put_taillight(handle_, value_long != 0);
                    break;
            }
        }
        status_cache_.invalidate();
    }

    void set_switch_value(int id, double value) override {
        validate_switch_id(id);
        ensure_connected();
        const ThermalElement element = element_copy(id);
        if (!element.writable) {
            throw AlpacaException(element.name + " is read-only", AlpacaError::InvalidOperation);
        }
        // std::lround on NaN/Inf is undefined behaviour; reject non-finite
        // values from the HTTP API as InvalidValue first.
        if (!std::isfinite(value)) {
            throw AlpacaException(element.name + " value must be a finite number", AlpacaError::InvalidValue);
        }
        long value_long = std::lround(value);
        if (value_long < element.min_value || value_long > element.max_value) {
            throw AlpacaException(element.name + " value out of range", AlpacaError::InvalidValue);
        }
        write_element_value(id, element, value_long);
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
        return element_copy(id).name;
    }

    void set_switch_name(int id, const std::string& name) override {
        validate_switch_id(id);
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        validate_switch_id_locked(id);
        elements_[static_cast<std::size_t>(id)].name = name;
    }

    std::string get_switch_description(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        validate_switch_id_locked(id);
        return elements_[static_cast<std::size_t>(id)].description + " (" + camera_name_ + ")";
    }

    double get_min_switch_value(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return static_cast<double>(element_copy(id).min_value);
    }

    double get_max_switch_value(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return static_cast<double>(element_copy(id).max_value);
    }

    double get_switch_step(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        return 1.0;
    }

private:
    // One refill reads every element's live value; every getter reads one of them. 100 ms as the other
    // cached request/response drivers: a write drops the frame, so the TTL only bounds how stale a change
    // made elsewhere can look.
    struct Status {
        std::vector<long> values;  // one per element, in element order
        bool closed{};             // a disconnect won the I/O lock first: not a device frame, never a link fault
    };

    static constexpr std::chrono::milliseconds kStatusTtl{100};

    // The live value of element `id`, from the cached frame. The refill runs under the cache's own mutex,
    // then the I/O lock (lock order: transition_mutex_, cache mutex, I/O lock, mutex_), so it takes no
    // driver lock and no caller may hold mutex_ or the I/O lock around the cache.
    long read_value(int id) const {
        Status status = status_cache_.get([this] {
            std::lock_guard<std::mutex> io(io_mutex_);
            Status fresh;
            std::vector<ThermalElement> elements;
            {
                // One hold: a disconnect clears elements_ under mutex_ without the I/O lock, so an
                // empty copy means it won the race and this is not a device frame.
                std::lock_guard<std::mutex> lock(mutex_);
                if (connected_.load() && handle_) {
                    elements = elements_;
                }
            }
            if (elements.empty()) {
                // A disconnect that won the race has cleared the state. Returning (not throwing) keeps it
                // out of the link-health count; the disconnect's reset() drops this frame.
                fresh.closed = true;
                return fresh;
            }
            fresh.values.reserve(elements.size());
            for (const auto& element : elements) {
                fresh.values.push_back(read_element_value(element));
            }
            return fresh;
        });
        if (status.closed) {
            throw AlpacaException("Camera handle not available", AlpacaError::NotConnected);
        }
        if (id < 0 || static_cast<std::size_t>(id) >= status.values.size()) {
            throw AlpacaException("Switch ID out of range", AlpacaError::InvalidValue);
        }
        return status.values[static_cast<std::size_t>(id)];
    }

    // Reads the live SDK value for an element. Caller holds io_mutex_ and has verified handle_ is
    // non-null (so the read is serialised against a concurrent disconnect/close).
    int read_element_value(const ThermalElement& element) const {
        auto& sdk = sdk_;
        switch (element.kind) {
            case ThermalElementKind::DewHeater:
                return sdk.get_heat(handle_);
            case ThermalElementKind::Fan:
                return sdk.get_fan(handle_);
            case ThermalElementKind::TailLight:
                return sdk.get_taillight(handle_);
        }
        return 0;
    }

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Switch not connected", AlpacaError::NotConnected);
        }
    }

    // Caller holds mutex_. Re-checks the connection under the lock so a
    // disconnect that completed between the pre-lock ensure_connected() and
    // acquiring mutex_ cannot lead to indexing an already-cleared elements_
    // (disconnect clears elements_ and stores connected_=false under mutex_).
    void ensure_connected_locked() const {
        if (!connected_.load() || elements_.empty()) {
            throw AlpacaException("Switch not connected", AlpacaError::NotConnected);
        }
    }

    // ID validation runs before the connection check (ASCOM: out-of-range ID
    // is InvalidValue even while disconnected). Disconnected, the bound is the
    // potential element count; connected, the probed per-model count.
    void validate_switch_id_locked(int id) const {
        // Disconnected, use the cached probed count when known so the ID bound
        // matches what get_max_switch() reports (else the worst-case bound).
        const int limit = connected_.load() ? static_cast<int>(elements_.size())
                                            : (probed_element_count_ > 0 ? probed_element_count_ : kMaxThermalElements);
        if (id < 0 || id >= limit) {
            throw AlpacaException("Switch ID out of range", AlpacaError::InvalidValue);
        }
    }

    void validate_switch_id(int id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        validate_switch_id_locked(id);
    }

    ThermalElement element_copy(int id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        validate_switch_id_locked(id);
        return elements_[static_cast<std::size_t>(id)];
    }

    // Caller holds io_mutex_ (the probes are device calls). Returns the list; the caller publishes it.
    std::vector<ThermalElement> build_elements(ToupTekSDK& sdk, HToupcam handle, const ToupCameraInfo& info) {
        std::vector<ThermalElement> elements;
        if (info.supports_heat) {
            int heat_max = sdk.get_heat_max(handle);
            if (heat_max > 0) {
                ThermalElement e;
                e.kind = ThermalElementKind::DewHeater;
                e.name = "DewHeater";
                e.description = "ToupTek camera anti-fog dew heater level";
                e.writable = true;
                e.min_value = 0;
                e.max_value = heat_max;
                elements.push_back(std::move(e));
            }
        }
        if (info.supports_fan && info.max_fan_speed > 0) {
            ThermalElement e;
            e.kind = ThermalElementKind::Fan;
            e.name = "Fan";
            e.description = "ToupTek camera radiator fan speed";
            e.writable = true;
            e.min_value = 0;
            e.max_value = static_cast<long>(info.max_fan_speed);
            elements.push_back(std::move(e));
        }
        // The tail indicator LED has no capability flag — probe by reading the
        // option, and expose it as a boolean switch only if the camera accepts
        // it. Astro users typically turn it off to avoid reflections/light leaks.
        try {
            (void)sdk.get_taillight(handle);
            ThermalElement e;
            e.kind = ThermalElementKind::TailLight;
            e.name = "TailLight";
            e.description = "ToupTek camera tail indicator LED (turn off to avoid light leaks)";
            e.writable = true;
            e.min_value = 0;
            e.max_value = 1;
            elements.push_back(std::move(e));
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
            // Camera doesn't support the tail-light option; skip it.
        }
        if (elements.empty()) {
            const std::string message = util::thermal_capability_refusal(bound_, listed_, kThermalLabels);
            ALPACA_LOG_WARN(kLogTag, message);
            throw AlpacaException(message, AlpacaError::NotImplemented);
        }
        return elements;
    }

    // Injected SDK seam (issue #104); see touptek_sdk_wrapper.h.
    ToupTekSDK& sdk_;
    int device_number_;
    int camera_index_;
    std::string camera_id_;
    std::string serial_number_;
    std::string camera_name_;
    // The connect's camera listing, for the refusal when the bound camera has no controls.
    util::ThermalCameraCandidate bound_;
    std::vector<util::ThermalCameraCandidate> listed_;
    // handle_ is guarded by io_mutex_. The identity fields above (camera_id_, serial_number_, camera_name_),
    // elements_ and probed_element_count_ are guarded by mutex_ (written at the end of connect and by
    // disconnect); bound_ and listed_ are touched by set_connected() only.
    HToupcam handle_{nullptr};
    std::vector<ThermalElement> elements_;
    int probed_element_count_ = 0;  // guarded by mutex_; cached MaxSwitch after first probe
    std::atomic<bool> connected_;
    // Lock order: transition_mutex_, then the status cache's mutex, then io_mutex_, then mutex_ (a leaf).
    // transition_mutex_ spans set_connected() only and no getter takes it; io_mutex_ serialises the wire.
    std::mutex transition_mutex_;
    mutable std::mutex io_mutex_;
    mutable std::mutex mutex_;
    mutable util::TtlStatusCache<Status> status_cache_;
};

std::unique_ptr<SwitchDriver> create_touptek_thermal_switch(int device_number, int camera_index) {
    return std::make_unique<ToupTekThermalSwitchDriver>(device_number, camera_index, ToupTekSDKWrapper::instance());
}

std::unique_ptr<SwitchDriver> create_touptek_thermal_switch(int device_number, int camera_index, ToupTekSDK& sdk) {
    return std::make_unique<ToupTekThermalSwitchDriver>(device_number, camera_index, sdk);
}

std::vector<util::ThermalCameraCandidate> list_touptek_thermal_cameras(ToupTekSDK& sdk) {
    return thermal_candidates(sdk.enumerate_cameras());
}

std::vector<util::ThermalCameraCandidate> list_touptek_thermal_cameras() {
    return list_touptek_thermal_cameras(ToupTekSDKWrapper::instance());
}

}  // namespace alpacacore::vendor::touptek
