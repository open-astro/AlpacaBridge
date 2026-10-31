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
#include <alpacacore/vendor/playerone/playerone_sdk_wrapper.h>
#include <alpacacore/vendor/playerone/playerone_switch_driver.h>
#include <alpacacore/version.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace alpacacore::vendor::playerone {

namespace {

// A camera exposes at most a dew heater and a fan. Used as the switch-ID
// bound while disconnected, before the per-model element list is probed.
constexpr int kMaxThermalElements = 2;

enum class ThermalElementKind : std::uint8_t { DewHeater, Fan };

struct ThermalElement {
    ThermalElementKind kind{};
    std::string name;
    std::string description;
    bool writable{};
    long min_value{};
    long max_value{};
};

constexpr util::ThermalSwitchLabels kThermalLabels{"Player One", "dew heater or fan"};

// The thermal seam over the camera SDK seam: production passes the process-wide
// wrapper, a test either a FakePlayerOneSDK or its own PlayerOneThermalSdk.
class ThermalSdkOverCameraSdk final : public PlayerOneThermalSdk {
public:
    explicit ThermalSdkOverCameraSdk(PlayerOneSDK& sdk) : sdk_(sdk) {}
    static ThermalSdkOverCameraSdk& production() {
        static ThermalSdkOverCameraSdk sdk(PlayerOneSDKWrapper::instance());
        return sdk;
    }
    std::vector<PlayerOneCameraInfo> enumerate_cameras() override { return sdk_.enumerate_cameras(); }
    void open_camera(int id) override { sdk_.open_camera(id); }
    void init_camera(int id) override { sdk_.init_camera(id); }
    void close_camera(int id) override { sdk_.close_camera(id); }
    PlayerOneConfigCaps probe_config_caps(int id) override { return sdk_.probe_config_caps(id); }
    int get_heater_power_percent(int id) override { return sdk_.get_heater_power_percent(id); }
    int get_fan_power_percent(int id) override { return sdk_.get_fan_power_percent(id); }
    void set_heater_power_percent(int id, int p) override { sdk_.set_heater_power_percent(id, p); }
    void set_fan_power_percent(int id, int p) override { sdk_.set_fan_power_percent(id, p); }

private:
    PlayerOneSDK& sdk_;
};

// One refill reads every element's power, so DeviceState costs one burst.
struct ThermalStatus {
    std::array<int, kMaxThermalElements> values{};
    bool closed{};  // a disconnect won the I/O lock first: not a device frame, never a link fault
};

constexpr std::chrono::milliseconds kStatusTtl{100};

// The index is the position in enumerate_cameras(), which is what cameraIndex
// has always meant; the serial comes from the enumeration (POACameraProperties::SN).
// The heater and fan controls are only visible on an open camera, so each
// cooled camera is opened through the ref-counted wrapper (a camera the camera
// driver holds stays open) and probed; an uncooled one is listed without them.
std::vector<util::ThermalCameraCandidate> thermal_candidates(PlayerOneThermalSdk& sdk,
                                                             const std::vector<PlayerOneCameraInfo>& cameras) {
    std::vector<util::ThermalCameraCandidate> candidates;
    candidates.reserve(cameras.size());
    for (std::size_t i = 0; i < cameras.size(); ++i) {
        const auto& info = cameras[i];
        util::ThermalCameraCandidate candidate;
        candidate.index = static_cast<int>(i);
        candidate.name = info.name;
        candidate.serial = info.serial_number;
        if (info.has_cooler) {
            try {
                sdk.open_camera(info.camera_id);
                try {
                    sdk.init_camera(info.camera_id);
                    const auto caps = sdk.probe_config_caps(info.camera_id);
                    candidate.has_thermal = caps.has_heater_power || caps.has_fan_power;
                } catch (const std::exception& e) {
                    ALPACA_LOG_DEBUG("PlayerOne", "Thermal listing of " + info.name + ": " + e.what());
                }
                sdk.close_camera(info.camera_id);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("PlayerOne", "Thermal listing could not open " + info.name + ": " + e.what());
            }
        }
        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

}  // namespace

class PlayerOneSwitchDriver : public SwitchDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    // camera_index: an enumeration index, or util::kAutoThermalCamera for the one
    // camera with a heater or fan. bound_serial, when set, names the camera and wins.
    // owned_sdk, when set, is the adapter `sdk` refers to; the driver keeps it alive.
    PlayerOneSwitchDriver(int device_number, int camera_index, std::string bound_serial, PlayerOneThermalSdk& sdk,
                          std::unique_ptr<ThermalSdkOverCameraSdk> owned_sdk = nullptr)
        : AsyncConnectable("PlayerOne"),
          owned_sdk_(std::move(owned_sdk)),
          sdk_(sdk),
          device_number_(device_number),
          camera_index_(camera_index),
          bound_serial_(std::move(bound_serial)),
          camera_name_("Player One Camera"),
          connected_(false),
          status_cache_("Player One thermal switch", kStatusTtl, 3, "PlayerOne") {}

    ~PlayerOneSwitchDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        if (connected_.load()) {
            try {
                // Qualified: virtual dispatch is gone in a destructor anyway;
                // saying so explicitly keeps clang-analyzer's VirtualCall happy.
                PlayerOneSwitchDriver::set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("PlayerOne", "Error during switch destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override { return "Player One Thermal Switch"; }

    DeviceType get_device_type() const override { return DeviceType::Switch; }

    std::string fallback_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!serial_number_.empty()) {
            return "PLAYERONE_SW_SN_" + serial_number_;
        }
        return "PLAYERONE_SW_" + std::to_string(device_number_);
    }

    std::string get_description() const override { return "Player One camera dew heater and fan switch"; }

    std::string get_driver_info() const override { return "AlpacaCore Player One Thermal Switch"; }

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
                const auto candidates = thermal_candidates(sdk, cameras);
                util::ThermalCameraRequest request;
                request.camera_index = camera_index_;
                request.serial = bound_serial_;
                const auto choice = util::choose_thermal_camera(request, candidates, kThermalLabels);
                if (!choice.chosen.has_value()) {
                    ALPACA_LOG_WARN("PlayerOne", choice.refusal);
                    throw AlpacaException(choice.refusal, choice.error_code);
                }
                const auto& info = cameras[choice.chosen.value()];
                const auto& bound = candidates[choice.chosen.value()];

                sdk.open_camera(info.camera_id);
                try {
                    sdk.init_camera(info.camera_id);
                    auto elements = build_elements(sdk.probe_config_caps(info.camera_id), bound, candidates);
                    // The short state lock publishes identity and the element list at the end of connect.
                    std::lock_guard<std::mutex> lock(mutex_);
                    camera_id_ = info.camera_id;
                    camera_name_ = info.name;
                    serial_number_ = info.serial_number;
                    elements_ = std::move(elements);
                } catch (...) {
                    sdk.close_camera(info.camera_id);
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
        // of hitting a just-closed camera - same fix as the ZWO dew-heater
        // switch, per the driver-lifecycle.instructions.md disconnect rule.
        int close_id = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            close_id = camera_id_;
            camera_id_ = -1;
            elements_.clear();
            connected_.store(false);
        }
        status_cache_.reset();
        if (close_id >= 0) {
            // Waits for an exchange in flight: the camera is never closed under one.
            std::lock_guard<std::mutex> io(io_mutex_);
            sdk.close_camera(close_id);
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
        if (!connected_.load()) {
            return kMaxThermalElements;
        }
        return static_cast<int>(elements_.size());
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
        double value = get_switch_value(id);
        return value > get_min_switch_value(id);
    }

    void set_switch(int id, bool state) override {
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
        // The refill takes the I/O lock inside the cache's own lock (order: transition_mutex_, cache
        // mutex, I/O lock, mutex_), so no caller may hold mutex_ or the I/O lock around the cache.
        const auto status = status_cache_.get([this] {
            std::lock_guard<std::mutex> io(io_mutex_);
            ThermalStatus fresh;
            std::vector<ThermalElement> elements;
            int camera_id = -1;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                elements = elements_;
                camera_id = camera_id_;
            }
            if (camera_id < 0) {
                // A disconnect won the race; its reset() drops this frame, and it is no link fault.
                fresh.closed = true;
                return fresh;
            }
            for (std::size_t i = 0; i < elements.size(); ++i) {
                fresh.values[i] = elements[i].kind == ThermalElementKind::DewHeater
                                      ? sdk_.get_heater_power_percent(camera_id)
                                      : sdk_.get_fan_power_percent(camera_id);
            }
            return fresh;
        });
        if (status.closed) {
            throw AlpacaException("Camera ID not available", AlpacaError::NotConnected);
        }
        return static_cast<double>(status.values[static_cast<std::size_t>(id)]);
    }

    void set_switch_value(int id, double value) override {
        validate_switch_id(id);
        ensure_connected();
        const auto element = element_copy(id);
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
        {
            std::lock_guard<std::mutex> io(io_mutex_);
            // Read under the I/O lock: a disconnect that closed the camera first leaves -1 and throws.
            const int camera_id = camera_id_copy();
            if (element.kind == ThermalElementKind::DewHeater) {
                sdk_.set_heater_power_percent(camera_id, static_cast<int>(value_long));
            } else {
                sdk_.set_fan_power_percent(camera_id, static_cast<int>(value_long));
            }
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
        return element_copy(id).name;
    }

    void set_switch_name(int id, const std::string& name) override {
        validate_switch_id(id);
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        validate_switch_id_locked(id);
        elements_[static_cast<std::size_t>(id)].name = name;
    }

    std::string get_switch_description(int id) const override {
        validate_switch_id(id);
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
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

    // ID validation runs before the connection check (ASCOM: out-of-range ID
    // is InvalidValue even while disconnected). Disconnected, the bound is
    // the potential element count; connected, the probed per-model count.
    void validate_switch_id_locked(int id) const {
        const int limit = connected_.load() ? static_cast<int>(elements_.size()) : kMaxThermalElements;
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
        validate_switch_id_locked(id);
        return elements_[static_cast<std::size_t>(id)];
    }

    int camera_id_copy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_id_ < 0) {
            throw AlpacaException("Camera ID not available", AlpacaError::NotConnected);
        }
        return camera_id_;
    }

    // Pure: the caller publishes the result under mutex_. Throws when the camera has neither control.
    static std::vector<ThermalElement> build_elements(const PlayerOneConfigCaps& caps,
                                                      const util::ThermalCameraCandidate& bound,
                                                      const std::vector<util::ThermalCameraCandidate>& listed) {
        std::vector<ThermalElement> elements;
        if (caps.has_heater_power) {
            ThermalElement e;
            e.kind = ThermalElementKind::DewHeater;
            e.name = "DewHeater";
            e.description = "Player One camera anti-dew lens heater power, percent";
            e.writable = caps.heater_power_writable;
            e.min_value = caps.heater_power_min;
            e.max_value = caps.heater_power_max;
            elements.push_back(std::move(e));
        }
        if (caps.has_fan_power) {
            ThermalElement e;
            e.kind = ThermalElementKind::Fan;
            e.name = "Fan";
            e.description = "Player One camera radiator fan power, percent";
            e.writable = caps.fan_power_writable;
            e.min_value = caps.fan_power_min;
            e.max_value = caps.fan_power_max;
            elements.push_back(std::move(e));
        }
        if (elements.empty()) {
            const std::string message = util::thermal_capability_refusal(bound, listed, kThermalLabels);
            ALPACA_LOG_WARN("PlayerOne", message);
            throw AlpacaException(message, AlpacaError::NotImplemented);
        }
        return elements;
    }

    std::unique_ptr<ThermalSdkOverCameraSdk> owned_sdk_;
    // Injected SDK seam: must outlive the driver (owned_sdk_ when the factory built it).
    PlayerOneThermalSdk& sdk_;
    int device_number_;
    int camera_index_;
    std::string bound_serial_;
    int camera_id_{-1};
    std::string serial_number_;
    std::string camera_name_;
    std::vector<ThermalElement> elements_;
    std::atomic<bool> connected_;
    // Lock order: transition_mutex_, then the status cache's mutex, then io_mutex_, then mutex_ (a leaf:
    // it guards identity and the element list, never held across an SDK call). transition_mutex_ spans
    // set_connected() only and no getter takes it; io_mutex_ serialises every camera call.
    std::mutex transition_mutex_;
    mutable std::mutex io_mutex_;
    mutable std::mutex mutex_;
    mutable util::TtlStatusCache<ThermalStatus> status_cache_;
};

std::unique_ptr<SwitchDriver> create_playerone_switch(int device_number, int camera_index,
                                                      const std::string& camera_serial) {
    return std::make_unique<PlayerOneSwitchDriver>(device_number, camera_index, camera_serial,
                                                   ThermalSdkOverCameraSdk::production());
}

std::unique_ptr<SwitchDriver> create_playerone_switch(int device_number, int camera_index,
                                                      const std::string& camera_serial, PlayerOneSDK& sdk) {
    auto adapter = std::make_unique<ThermalSdkOverCameraSdk>(sdk);
    auto& thermal = *adapter;
    return std::make_unique<PlayerOneSwitchDriver>(device_number, camera_index, camera_serial, thermal,
                                                   std::move(adapter));
}

std::unique_ptr<SwitchDriver> create_playerone_switch(int device_number, int camera_index, PlayerOneThermalSdk& sdk) {
    return std::make_unique<PlayerOneSwitchDriver>(device_number, camera_index, std::string(), sdk);
}

std::vector<util::ThermalCameraCandidate> list_playerone_thermal_cameras(PlayerOneSDK& sdk) {
    ThermalSdkOverCameraSdk thermal(sdk);
    return thermal_candidates(thermal, thermal.enumerate_cameras());
}

std::vector<util::ThermalCameraCandidate> list_playerone_thermal_cameras() {
    return list_playerone_thermal_cameras(PlayerOneSDKWrapper::instance());
}

}  // namespace alpacacore::vendor::playerone
