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
#include <alpacacore/util/auto_detect.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/ioptron/ioptron_ieaf_focuser_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_ieaf_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>

namespace alpacacore::vendor::ioptron {

namespace {
// Matches INDI's FocusAbsPos max for the iEAF; the protocol's 7-digit move
// field could carry more, but the hardware range is 0..99999 steps.
constexpr int IEAF_MAX_STEP = 99999;
}  // namespace

class IeafFocuserDriver : public FocuserDriver, protected alpacacore::AsyncConnectable {
public:
    IeafFocuserDriver(int device_number, IeafConnectionConfig config)
        : AsyncConnectable("iOptron"),
          device_number_(device_number),
          config_(std::move(config)),
          connected_(false),
          protocol_() {
        model_name_ = model_name_for(config_.model);
    }

    ~IeafFocuserDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        if (connected_.load()) {
            try {
                set_connected(false);  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("iOptron", "Error during iEAF destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override { return device_number_; }

    // Name/Description follow the user-selected model (iEAF vs iAFS2/3 share
    // one protocol and one driver; the config "model" key picks the wording).
    std::string get_name() const override { return "iOptron " + model_name(); }

    DeviceType get_device_type() const override { return DeviceType::Focuser; }

    std::string get_unique_id() const override { return "IOPTRON_IEAF_" + std::to_string(device_number_); }

    std::string get_description() const override { return "iOptron " + model_name() + " Electronic Focuser Driver"; }

    std::string get_driver_info() const override { return "AlpacaCore iOptron iEAF Focuser Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Firmware captured at connect; surfaced in the web UI only (never
    // DriverInfo). Guarded solely by firmware_mutex_ — non-empty exactly while
    // connected, so there is no atomic-vs-mutex ordering to reason about.
    std::optional<std::string> get_device_firmware() const override {
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        if (firmware_.empty()) {
            return std::nullopt;
        }
        return firmware_;
    }

    int get_interface_version() const override { return 4; }

    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }

    void disconnect() override {
        // Disconnect synchronously — trivial (close fd + set flag) and ASCOM
        // clients expect Connected to be false immediately after.
        stop_connection_thread();
        try {
            set_connected(false);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("iOptron", "iEAF disconnect error: " + std::string(e.what()));
        }
    }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        // Base gates BEFORE the idempotency check: a sync disconnect during an
        // in-flight connect looks idempotent and would be silently dropped
        // without the record; a connect must honor a newer pending disconnect.
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) {
            return;
        }
        if (connected && consume_pending_disconnect(connected_.load())) {
            return;
        }
        if (connected == connected_.load()) {
            return;
        }

        if (connected) {
            IeafDeviceInfo info = protocol_.connect(config_);
            {
                std::lock_guard<std::mutex> lock(firmware_mutex_);
                firmware_ = info.firmware > 0 ? std::to_string(info.firmware) : std::string();
            }
            connected_.store(true);
            ALPACA_LOG_INFO("iOptron",
                            model_name_ + " focuser connected (model code " + std::to_string(info.model) + ")");
        } else {
            protocol_.disconnect();
            {
                std::lock_guard<std::mutex> lock(firmware_mutex_);
                firmware_.clear();
            }
            connected_.store(false);
            invalidate_status_cache();
            ALPACA_LOG_INFO("iOptron", "iEAF focuser disconnected");
        }
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

    // --- Focuser interface ---

    bool get_absolute() const override { return true; }

    bool get_is_moving() const override {
        ensure_connected();
        return cached_status().moving;
    }

    int get_max_step() const override { return IEAF_MAX_STEP; }

    int get_max_increment() const override { return IEAF_MAX_STEP; }

    int get_position() const override {
        ensure_connected();
        return cached_status().position;
    }

    double get_step_size() const override {
        // The iEAF protocol does not expose step size in microns — it depends
        // on the focuser drawtube it is mounted to.
        throw AlpacaException("Step size not available for this focuser", AlpacaError::PropertyNotImplemented);
    }

    bool get_temp_comp_available() const override { return false; }

    bool get_temp_comp() const override { return false; }

    void set_temp_comp(bool) override {
        throw AlpacaException("Temperature compensation not supported", AlpacaError::NotImplemented);
    }

    double get_temperature() const override {
        ensure_connected();
        return cached_status().temperature_c;
    }

    void halt() override {
        ensure_connected();
        protocol_.halt();
        invalidate_status_cache();
    }

    void move(int position) override {
        ensure_connected();
        if (position < 0 || position > IEAF_MAX_STEP) {
            throw AlpacaException("Focuser position out of range", AlpacaError::InvalidValue);
        }
        protocol_.move_to(position);
        invalidate_status_cache();
    }

private:
    // One :FI# round trip is ~60 ms on the PL2303 link. DeviceState reads
    // IsMoving + Position + Temperature back to back, which is three round
    // trips (~0.26 s) against a 0.1 s ASCOM FAST target. Serve a burst of
    // reads from one poll: the cache lives for STATUS_CACHE_TTL and is
    // dropped by move()/halt() so the first read after a command is live.
    static constexpr auto STATUS_CACHE_TTL = std::chrono::milliseconds(100);

    IeafStatus cached_status() const {
        std::lock_guard<std::mutex> lock(status_mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (!status_cache_ || now - status_cache_time_ > STATUS_CACHE_TTL) {
            status_cache_ = const_cast<IeafFocuserDriver*>(this)->protocol_.get_status();
            status_cache_time_ = now;
        }
        return *status_cache_;
    }

    void invalidate_status_cache() {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_cache_.reset();
    }

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Focuser not connected", AlpacaError::NotConnected);
        }
    }

    int device_number_;
    IeafConnectionConfig config_;
    std::atomic<bool> connected_;
    IeafProtocolWrapper protocol_;
    // firmware_ has its OWN mutex, NOT the base's connection mutex:
    // set_connected() runs on the connection thread and caches firmware, while
    // the base joins that thread — sharing one mutex would deadlock.
    // Config "model": "iafs2" -> iAFS2/3, anything else -> iEAF (the default).
    // Fixed at construction, so no locking needed.
    const std::string& model_name() const { return model_name_; }

    static std::string model_name_for(const std::string& model) { return model == "iafs2" ? "iAFS2/3" : "iEAF"; }

    mutable std::mutex firmware_mutex_;
    std::string firmware_;    // captured at connect; web-UI only (guarded by firmware_mutex_)
    std::string model_name_;  // "iEAF" / "iAFS2/3" from config; fixed after construction
    mutable std::mutex status_mutex_;
    mutable std::optional<IeafStatus> status_cache_;
    mutable std::chrono::steady_clock::time_point status_cache_time_{};
};

std::unique_ptr<FocuserDriver> create_ieaf_focuser(int device_number, const std::string& serial_port,
                                                   const std::string& model) {
    IeafConnectionConfig config;
    config.serial_port = serial_port;
    config.model = model;
    return std::make_unique<IeafFocuserDriver>(device_number, std::move(config));
}

std::unique_ptr<FocuserDriver> create_ieaf_focuser_by_index(int device_number, int focuser_index,
                                                            const std::string& model) {
    auto ports = enumerate_ieaf_ports();
    if (ports.empty()) {
        throw AlpacaException(util::serial_auto_detect_failed_message("iOptron iEAF focuser"),
                              AlpacaError::NotConnected);
    }
    if (focuser_index < 0 || focuser_index >= static_cast<int>(ports.size())) {
        throw AlpacaException("Focuser index " + std::to_string(focuser_index) + " out of range (detected " +
                                  std::to_string(ports.size()) + ")",
                              AlpacaError::InvalidValue);
    }

    const auto& port = ports[static_cast<std::size_t>(focuser_index)];
    ALPACA_LOG_INFO("iOptron", "Auto-detected iEAF at " + port.port_path + " (firmware " +
                                   std::to_string(port.info.firmware) + ")");

    IeafConnectionConfig config;
    config.serial_port = port.port_path;
    config.model = model;
    return std::make_unique<IeafFocuserDriver>(device_number, std::move(config));
}

}  // namespace alpacacore::vendor::ioptron
