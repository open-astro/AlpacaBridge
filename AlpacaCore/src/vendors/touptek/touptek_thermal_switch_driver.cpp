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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/touptek/touptek_sdk_wrapper.h>
#include <alpacacore/vendor/touptek/touptek_thermal_switch_driver.h>
#include <alpacacore/version.h>

#include <atomic>
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

}  // namespace

class ToupTekThermalSwitchDriver : public SwitchDriver {
public:
    ToupTekThermalSwitchDriver(int device_number, int camera_index)
        : device_number_(device_number),
          camera_index_(camera_index),
          camera_name_("ToupTek Camera"),
          connected_(false),
          connecting_(false) {}

    ~ToupTekThermalSwitchDriver() override {
        {
            // Block any new connection task from spawning a thread that would
            // outlive this object (destructor race -> std::terminate on an
            // unjoined connection_thread_).
            std::lock_guard<std::mutex> lock(connection_mutex_);
            shutting_down_ = true;
        }
        stop_connection_thread();
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

    std::string get_unique_id() const override {
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
    bool get_connecting() const override { return connecting_.load(); }

    void set_connected(bool connected) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected && !connected_.load() && connecting_.load() && inflight_is_connect_.load()) {
            // Disconnect requested while an async connect is in flight but before
            // its thread has run (connected_ still false) — the idempotency
            // early-return below would silently drop it and the device would come
            // up Connected despite the explicit request. Record the intent; the
            // in-flight connect consumes it at entry or the connection task's
            // post-connect re-check disconnects. Same pattern as the AFW driver.
            pending_disconnect_ = true;
            return;
        }
        if (connected && pending_disconnect_) {
            // A disconnect arrived after this connect was requested (in the window
            // before this thread acquired mutex_). It is newer — honor it: consume
            // the flag and stay disconnected instead of opening the camera.
            pending_disconnect_ = false;
            return;
        }
        if (connected == connected_.load()) {
            return;
        }

        auto& sdk = ToupTekSDKWrapper::instance();
        if (connected) {
            auto cameras = sdk.enumerate_cameras();
            if (cameras.empty()) {
                throw AlpacaException("No ToupTek cameras detected", AlpacaError::NotConnected);
            }
            if (camera_index_ < 0 || camera_index_ >= static_cast<int>(cameras.size())) {
                throw AlpacaException("Camera index out of range", AlpacaError::InvalidValue);
            }
            const auto& info = cameras[static_cast<std::size_t>(camera_index_)];
            camera_id_ = info.id;
            camera_name_ = info.name;

            // Shared, reference-counted open — coexists with the camera device.
            HToupcam handle = sdk.open_camera_by_id(camera_id_);
            try {
                serial_number_ = sdk.get_serial_number(handle);
                build_elements_locked(sdk, handle, info);
                handle_ = handle;
            } catch (...) {
                sdk.close_camera(handle);
                // build_elements_locked clears elements_ at its top then push_backs;
                // if it threw mid-build (e.g. OOM) elements_ holds a partial list.
                // Clear it so the disconnected state is fully clean, consistent with
                // the other reset fields (handle_ stays null — never published).
                elements_.clear();
                camera_id_.clear();
                serial_number_.clear();
                throw;
            }
            connected_.store(true);
            return;
        }

        if (handle_) {
            sdk.close_camera(handle_);
            handle_ = nullptr;
        }
        camera_id_.clear();
        elements_.clear();
        connected_.store(false);
    }

    std::vector<DeviceState> get_device_state() const override {
        std::vector<DeviceState> state;
        if (!connected_.load()) {
            return state;
        }
        const int count = get_max_switch();
        for (int id = 0; id < count; ++id) {
            try {
                state.push_back({"GetSwitch" + std::to_string(id), get_switch(id)});
                state.push_back({"GetSwitchValue" + std::to_string(id), get_switch_value(id)});
                state.push_back({"StateChangeComplete" + std::to_string(id), get_state_change_complete(id)});
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
                // Omit unavailable members per the DeviceState contract.
            }
        }
        return state;
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
        // Read the value AND compare against the element min under a single lock,
        // so a concurrent disconnect can't clear elements_ between two separate
        // lock acquisitions and surface a spurious NotConnected after the value
        // already read successfully.
        std::lock_guard<std::mutex> lock(mutex_);
        validate_switch_id_locked(id);
        if (!handle_) {
            throw AlpacaException("Camera handle not available", AlpacaError::NotConnected);
        }
        const auto& element = elements_[static_cast<std::size_t>(id)];
        return static_cast<double>(read_element_value_locked(element)) > static_cast<double>(element.min_value);
    }

    void set_switch(int id, bool state) override {
        validate_switch_id(id);
        ensure_connected();
        // Resolve the target level (max for on / min for off) AND write it under a
        // SINGLE lock hold. Going through get_max/min_switch_value + set_switch_value
        // took three separate lock acquisitions, so a disconnect+reconnect to a
        // different camera model in between could change elements_[id]'s range —
        // making the resolved value stale when set_switch_value range-checks it
        // (spurious InvalidValue, or writing the wrong level to a different control).
        std::lock_guard<std::mutex> lock(mutex_);
        validate_switch_id_locked(id);
        if (!handle_) {
            throw AlpacaException("Camera handle not available", AlpacaError::NotConnected);
        }
        const auto& element = elements_[static_cast<std::size_t>(id)];
        if (!element.writable) {
            throw AlpacaException(element.name + " is read-only", AlpacaError::InvalidOperation);
        }
        // The element's own min_value/max_value are trivially in range, so no
        // separate bounds check is needed (unlike the free-value set path).
        write_element_value_locked(element, state ? element.max_value : element.min_value);
    }

    void set_async(int id, bool /*state*/) override {
        validate_switch_id(id);
        ensure_connected();
        throw AlpacaException("Async switch control not supported", AlpacaError::NotImplemented);
    }

    double get_switch_value(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        // Hold mutex_ across the SDK call: set_connected(false) takes mutex_
        // before it may Toupcam_Close the shared handle (when this switch is the
        // sole opener), so serialising here prevents a use-after-close.
        std::lock_guard<std::mutex> lock(mutex_);
        validate_switch_id_locked(id);
        if (!handle_) {
            throw AlpacaException("Camera handle not available", AlpacaError::NotConnected);
        }
        return static_cast<double>(read_element_value_locked(elements_[static_cast<std::size_t>(id)]));
    }

    // Reads the live SDK value for an element. Caller holds mutex_ and has
    // verified handle_ is non-null (so the read is serialised against a
    // concurrent disconnect/close).
    int read_element_value_locked(const ThermalElement& element) const {
        auto& sdk = ToupTekSDKWrapper::instance();
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

    // Writes value_long to the element's SDK control. Caller holds mutex_, has
    // verified handle_ is non-null and the element is writable, and has ensured
    // value_long is within [min_value, max_value] (the element's own bounds are
    // trivially in range). Shared by set_switch and set_switch_value so both
    // resolve-and-write under a single lock (no cross-call TOCTOU).
    void write_element_value_locked(const ThermalElement& element, long value_long) {
        auto& sdk = ToupTekSDKWrapper::instance();
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

    void set_switch_value(int id, double value) override {
        validate_switch_id(id);
        ensure_connected();
        // Hold mutex_ across validation and the SDK write for the same reason as
        // get_switch_value — no use-after-close if a disconnect races in.
        std::lock_guard<std::mutex> lock(mutex_);
        validate_switch_id_locked(id);
        if (!handle_) {
            throw AlpacaException("Camera handle not available", AlpacaError::NotConnected);
        }
        const auto& element = elements_[static_cast<std::size_t>(id)];
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
        write_element_value_locked(element, value_long);
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

    void build_elements_locked(ToupTekSDKWrapper& sdk, HToupcam handle, const ToupCameraInfo& info) {
        elements_.clear();
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
                elements_.push_back(std::move(e));
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
            elements_.push_back(std::move(e));
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
            elements_.push_back(std::move(e));
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
            // Camera doesn't support the tail-light option; skip it.
        }
        if (elements_.empty()) {
            throw AlpacaException("Camera has no dew heater, fan, or tail light (uncooled model?)",
                                  AlpacaError::NotImplemented);
        }
        // Remember the probed count so MaxSwitch stays stable across a later
        // disconnect (ASCOM expects MaxSwitch not to change once known).
        probed_element_count_ = static_cast<int>(elements_.size());
    }

    void start_connection_task(bool connect) {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        if (shutting_down_) {
            return;  // Destruction in progress; never spawn a new thread.
        }
        if (connecting_.load()) {
            // An async disconnect racing an in-flight CONNECT must not be dropped
            // (the device would come up Connected despite the explicit request).
            // Record the intent; the connect consumes it at entry or the re-check
            // below runs the disconnect. A disconnect racing an in-flight
            // DISCONNECT needs nothing (idempotent). Same pattern as the AFW.
            if (!connect && inflight_is_connect_.load()) {
                std::lock_guard<std::mutex> state_lock(mutex_);
                pending_disconnect_ = true;
            }
            return;
        }
        if (connection_thread_.joinable()) {
            connection_thread_.join();
        }
        inflight_is_connect_.store(connect);
        connecting_.store(true);
        connection_thread_ = std::thread([this, connect]() {
            try {
                set_connected(connect);
                if (connect) {
                    // Consume a disconnect that landed while this connect held
                    // mutex_ (the recorder blocks on the lock, so the flag can be
                    // set right after the connect publishes).
                    bool need_disconnect = false;
                    {
                        std::lock_guard<std::mutex> state_lock(mutex_);
                        if (pending_disconnect_) {
                            pending_disconnect_ = false;
                            need_disconnect = connected_.load();
                        }
                    }
                    if (need_disconnect) {
                        set_connected(false);
                    }
                }
            } catch (const std::exception& e) {
                ALPACA_LOG_ERROR(kLogTag, "Thermal switch connection failed: " + std::string(e.what()));
            }
            {
                // Discard any pending_disconnect_ that landed after the re-check
                // above. That disconnect is dropped (instructions-wide window; the
                // client retries) — but the flag must NOT leak into the next
                // connect, which would consume it at entry and silently no-op.
                std::lock_guard<std::mutex> state_lock(mutex_);
                pending_disconnect_ = false;
            }
            connecting_.store(false);
        });
    }

    void stop_connection_thread() {
        std::lock_guard<std::mutex> lock(connection_mutex_);
        if (connection_thread_.joinable()) {
            connection_thread_.join();
        }
    }

    int device_number_;
    int camera_index_;
    std::string camera_id_;
    std::string serial_number_;
    std::string camera_name_;
    HToupcam handle_{nullptr};
    std::vector<ThermalElement> elements_;
    int probed_element_count_ = 0;  // guarded by mutex_; cached MaxSwitch after first probe
    std::atomic<bool> connected_;
    std::atomic<bool> connecting_;
    // Direction of the task connecting_ refers to (true = connect). Written under
    // connection_mutex_ before connecting_ goes true; atomic so the sync
    // set_connected gate (which holds mutex_, never connection_mutex_) can read
    // it. Only meaningful while connecting_ is true.
    std::atomic<bool> inflight_is_connect_{false};
    // Set when a disconnect arrives while a connect is in flight (before its
    // thread runs, connected_ still false — the idempotency early-return would
    // otherwise drop it). Consumed at the connect's entry (stays disconnected)
    // or by the connection task's post-connect re-check. Guarded by mutex_.
    bool pending_disconnect_ = false;
    bool shutting_down_ = false;  // guarded by connection_mutex_
    mutable std::mutex mutex_;
    std::mutex connection_mutex_;
    std::thread connection_thread_;
};

std::unique_ptr<SwitchDriver> create_touptek_thermal_switch(int device_number, int camera_index) {
    return std::make_unique<ToupTekThermalSwitchDriver>(device_number, camera_index);
}

}  // namespace alpacacore::vendor::touptek
