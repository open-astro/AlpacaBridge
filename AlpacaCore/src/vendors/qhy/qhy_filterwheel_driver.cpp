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
#include <alpacacore/vendor/qhy/qhy_filterwheel_driver.h>
#include <alpacacore/vendor/qhy/qhy_sdk_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <string_view>

namespace alpacacore::vendor::qhy {

class QHYFilterWheelDriver : public FilterWheelDriver, protected alpacacore::AsyncConnectable {
public:
    QHYFilterWheelDriver(int device_number, std::optional<std::string> camera_id, std::optional<int> camera_index)
        : AsyncConnectable("QHY"),
          device_number_(device_number),
          camera_id_(std::move(camera_id)),
          camera_index_(camera_index),
          camera_model_(),
          slot_count_(0),
          slot_count_valid_(false),
          filter_names_(),
          focus_offsets_(),
          cached_position_(),
          pending_target_(),
          connected_(false) {}

    ~QHYFilterWheelDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        if (connected_.load()) {
            try {
                // Qualified: virtual dispatch is gone in a destructor anyway;
                // saying so explicitly keeps clang-analyzer's VirtualCall happy.
                QHYFilterWheelDriver::set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("QHY", "Error during CFW destruction: " + std::string(e.what()));
            }
        }
    }

    // ── AlpacaDriver ─────────────────────────────────────────────────────────

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_model_.empty()) {
            return camera_model_ + " CFW";
        }
        return "QHY CFW";
    }

    DeviceType get_device_type() const override { return DeviceType::FilterWheel; }

    std::string get_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_id_.has_value()) {
            return "QHY_CFW_" + camera_id_.value();
        }
        return "QHY_CFW_" + std::to_string(device_number_);
    }

    std::string get_description() const override { return "QHY integrated color filter wheel driver"; }

    std::string get_driver_info() const override { return "AlpacaCore QHY Filter Wheel Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 3; }

    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }
    void disconnect() override { start_connection_task(false); }
    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        std::lock_guard<std::mutex> lock(mutex_);
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

        auto& sdk = QHYSDKWrapper::instance();
        if (connected) {
            const std::string& id = resolve_camera_id_locked();

            // Shared, reference-counted open — coexists with the paired camera
            // driver on the same physical handle (see qhy_sdk_wrapper.h).
            // Deliberately no init_camera(): the CFW port works on the raw
            // opened handle (per the 25.09.29 SDK's ControlCFW.cpp sample,
            // which never calls InitQHYCCD; validated on real miniCam8M
            // hardware -- see AGENTS.md), so the wheel doesn't need
            // to wait on -- or trigger -- the imaging chip's init sequence.
            sdk.open_camera(id);
            try {
                if (!sdk.is_control_available(id, control::CFWPORT)) {
                    throw AlpacaException("No CFW detected on this QHY camera", AlpacaError::NotConnected);
                }
                int slots = static_cast<int>(sdk.get_param(id, control::CFWSLOTSNUM));
                if (slots <= 0) {
                    throw AlpacaException("QHY CFW reported an invalid slot count", AlpacaError::DriverException);
                }
                slot_count_ = slots;
                slot_count_valid_ = true;
                normalize_slot_data_locked();

                std::string model;
                if (sdk.get_camera_model(id, model) && !model.empty()) {
                    camera_model_ = model;
                }

                // Warm-up read: GetQHYCCDCFWStatus is a genuine hardware round
                // trip on this SDK (~100-130ms measured on real miniCam8M
                // hardware -- consistent per-call, not a one-time cold-start
                // cost), which blows the ASCOM FAST (0.1s) target for the
                // first Position/DeviceState read after Connect if done
                // lazily. Doing it here spends that cost against Connect's
                // STANDARD (1.0s) budget instead, and seeds cached_position_
                // (see get_position) so the read right after connect is fast.
                int initial_pos = sdk.get_cfw_position(id);
                if (initial_pos >= 0) {
                    cached_position_ = initial_pos;
                } else {
                    // mid-move at connect time -- rare; get_position() falls back to a live read
                    cached_position_.reset();
                }
            } catch (...) {
                // Copy before the reset below: `id` is a reference into
                // camera_id_ and must stay valid for the close call.
                const std::string close_id = id;
                if (camera_index_.has_value()) {
                    // A failed index-based connect must not pin the resolved
                    // ID: the next connect re-resolves from the index (same
                    // rule as the disconnect path below), so a USB
                    // re-enumeration between attempts can't leave us reusing
                    // a stale ID.
                    camera_id_.reset();
                }
                sdk.close_camera(close_id);
                throw;
            }
            connected_.store(true);
            return;
        }

        // Clear driver state before the SDK close: if the close throws (e.g.
        // device unplugged) the error still surfaces, but the driver must not
        // stay half-connected.
        const std::optional<std::string> close_id = camera_id_;
        if (camera_index_.has_value()) {
            // Re-resolved from the index on the next connect (matches the ZWO
            // EFW / QHY camera pattern); an explicit camera_id_ config stays
            // fixed across reconnects instead.
            camera_id_.reset();
        }
        slot_count_valid_ = false;
        cached_position_.reset();
        pending_target_.reset();
        connected_.store(false);
        if (close_id.has_value()) {
            sdk.close_camera(close_id.value());
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

    // ── FilterWheelDriver ────────────────────────────────────────────────────

    int get_position() const override {
        ensure_connected();
        // Hold mutex_ across the SDK call: set_connected(false) closes the
        // shared handle under mutex_, so serialising here prevents a
        // concurrent disconnect from invalidating camera_id_ mid-call
        // (matches the ZWO EFW / ToupTek thermal-switch pattern).
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_id_.has_value()) {
            throw AlpacaException("Camera ID not set", AlpacaError::NotConnected);
        }
        // GetQHYCCDCFWStatus is a genuine ~100-130ms hardware round trip on
        // this SDK (measured on real miniCam8M hardware, consistent per call
        // -- ConformU flags it OUTSIDE the FAST 0.1s target). Only serve the
        // cache when NO move is outstanding (pending_target_ empty): while a
        // move is pending, this SDK call does not report a distinct "moving"
        // sentinel the way ToupTek's does -- it keeps returning the PRE-move
        // digit until the wheel physically arrives -- so caching the first
        // post-move reading unconditionally (an earlier version of this code
        // did exactly that) freezes Position at the stale old slot forever
        // and the wheel never appears to arrive. Every read taken while
        // pending_target_ is set is therefore always live and passed through
        // unchanged; only once a live read matches the commanded target do
        // we consider the wheel settled and start serving it from cache.
        if (cached_position_.has_value() && !pending_target_.has_value()) {
            return cached_position_.value();
        }
        int pos = QHYSDKWrapper::instance().get_cfw_position(camera_id_.value());
        if (pending_target_.has_value()) {
            if (pos == pending_target_.value()) {
                cached_position_ = pos;
                pending_target_.reset();
            }
            return pos;
        }
        if (pos >= 0) {
            cached_position_ = pos;
        }
        return pos;
    }

    void set_position(int position) override {
        // Range validation precedes the connection check (ASCOM precedence,
        // per AGENTS.md): a negative position is unconditionally invalid, so
        // it is InvalidValue even while disconnected. The upper bound depends
        // on the slot count (unknown until connect), so it is checked below.
        if (position < 0) {
            throw AlpacaException("Filter position out of range", AlpacaError::InvalidValue);
        }
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!camera_id_.has_value()) {
            throw AlpacaException("Camera ID not set", AlpacaError::NotConnected);
        }
        if (!slot_count_valid_ || slot_count_ <= 0) {
            throw AlpacaException("Filter wheel slot count unavailable", AlpacaError::DriverException);
        }
        if (position >= slot_count_) {
            throw AlpacaException("Filter position out of range", AlpacaError::InvalidValue);
        }
        // Set BEFORE issuing the move (not after): if move_cfw throws, we
        // can't be sure whether the wheel physically started moving, so
        // get_position() must fall back to live reads either way until a
        // read confirms where the wheel actually ended up.
        cached_position_.reset();
        pending_target_ = position;
        QHYSDKWrapper::instance().move_cfw(camera_id_.value(), position);
    }

    std::vector<int> get_focus_offsets() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return focus_offsets_;
    }

    void set_focus_offsets(const std::vector<int>& offsets) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slot_count_valid_ && slot_count_ > 0) {
            validate_slot_count_locked(static_cast<int>(offsets.size()), "focusOffsets");
        }
        focus_offsets_ = offsets;
    }

    std::vector<std::string> get_names() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return filter_names_;
    }

    void set_names(const std::vector<std::string>& names) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> staged = names;
        expand_shorthand_locked(staged);
        if (slot_count_valid_ && slot_count_ > 0) {
            validate_slot_count_locked(static_cast<int>(staged.size()), "names");
        }
        filter_names_ = std::move(staged);
        apply_default_names_locked();
    }

private:
    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Filter wheel not connected", AlpacaError::NotConnected);
        }
    }

    const std::string& resolve_camera_id_locked() {
        if (camera_index_.has_value() && !camera_id_.has_value()) {
            auto cameras = QHYSDKWrapper::instance().enumerate_cameras();
            if (cameras.empty()) {
                throw AlpacaException("No QHY cameras detected", AlpacaError::NotConnected);
            }
            int index = camera_index_.value();
            if (index < 0 || index >= static_cast<int>(cameras.size())) {
                throw AlpacaException("Camera index out of range: " + std::to_string(index), AlpacaError::InvalidValue);
            }
            const auto& info = cameras[static_cast<std::size_t>(index)];
            ALPACA_LOG_INFO("QHY", "CFW resolved camera index " + std::to_string(index) + " → ID: " + info.camera_id);
            camera_id_ = info.camera_id;
            if (!info.model.empty()) {
                camera_model_ = info.model;
            }
        }
        if (!camera_id_.has_value()) {
            throw AlpacaException("Camera ID not specified", AlpacaError::InvalidValue);
        }
        return camera_id_.value();
    }

    void normalize_slot_data_locked() {
        if (!slot_count_valid_ || slot_count_ <= 0) {
            return;
        }
        const std::size_t slots = static_cast<std::size_t>(slot_count_);
        expand_shorthand_locked(filter_names_);
        if (filter_names_.empty()) {
            filter_names_.assign(slots, std::string());
        } else if (filter_names_.size() != slots) {
            ALPACA_LOG_WARN("QHY", "Configured filterNames count (" + std::to_string(filter_names_.size()) +
                                       ") does not match CFW slot count (" + std::to_string(slots) +
                                       "); resizing to match the wheel");
            filter_names_.resize(slots);
        }
        apply_default_names_locked();
        if (focus_offsets_.empty()) {
            focus_offsets_.assign(slots, 0);
        } else if (focus_offsets_.size() != slots) {
            ALPACA_LOG_WARN("QHY", "Configured focusOffsets count (" + std::to_string(focus_offsets_.size()) +
                                       ") does not match CFW slot count (" + std::to_string(slots) +
                                       "); resizing to match the wheel");
            focus_offsets_.resize(slots);
        }
    }

    // Expand a single delimiter-less token into per-slot single-character names
    // ("LRGB" -> L,R,G,B) only when it looks like a shorthand code (no lowercase
    // letters), so ordinary names like "Clear" or "Ha_NB" that happen to match
    // the slot count are left intact. No-op until the slot count is known.
    void expand_shorthand_locked(std::vector<std::string>& names) const {
        if (!slot_count_valid_ || slot_count_ <= 0 || names.size() != 1) {
            return;
        }
        const std::size_t slots = static_cast<std::size_t>(slot_count_);
        const std::string& candidate = names[0];
        const bool has_lowercase = candidate.find_first_of("abcdefghijklmnopqrstuvwxyz") != std::string::npos;
        if (candidate.size() == slots && !has_lowercase && candidate.find_first_of(",; \t") == std::string::npos) {
            std::vector<std::string> expanded;
            expanded.reserve(slots);
            for (char ch : candidate) {
                expanded.emplace_back(1, ch);
            }
            names = std::move(expanded);
        }
    }

    void apply_default_names_locked() {
        for (std::size_t i = 0; i < filter_names_.size(); ++i) {
            if (filter_names_[i].empty()) {
                filter_names_[i] = "Filter " + std::to_string(i + 1);
            }
        }
    }

    void validate_slot_count_locked(int provided_size, std::string_view field_name) const {
        if (!slot_count_valid_ || slot_count_ <= 0) {
            throw AlpacaException("Filter wheel slot count unavailable", AlpacaError::DriverException);
        }
        if (provided_size != slot_count_) {
            throw AlpacaException(
                "Invalid " + std::string(field_name) + " length: expected " + std::to_string(slot_count_),
                AlpacaError::InvalidValue);
        }
    }

    int device_number_;
    std::optional<std::string> camera_id_;
    std::optional<int> camera_index_;
    std::string camera_model_;
    int slot_count_;
    bool slot_count_valid_;
    std::vector<std::string> filter_names_;
    std::vector<int> focus_offsets_;
    // Settled-position cache: valid until the next set_position() invalidates
    // it (see get_position()/set_position() for the reasoning). Declared
    // mutable so the const get_position() can populate it on a cache miss.
    mutable std::optional<int> cached_position_;
    // Set by set_position() to the commanded slot while a move is in flight;
    // cleared once a live read confirms arrival. See get_position().
    mutable std::optional<int> pending_target_;
    std::atomic<bool> connected_;
    mutable std::mutex mutex_;
};

std::unique_ptr<FilterWheelDriver> create_qhy_filterwheel(int device_number, const std::string& camera_id) {
    return std::make_unique<QHYFilterWheelDriver>(device_number, camera_id, std::nullopt);
}

std::unique_ptr<FilterWheelDriver> create_qhy_filterwheel_by_index(int device_number, int camera_index) {
    return std::make_unique<QHYFilterWheelDriver>(device_number, std::nullopt, camera_index);
}

}  // namespace alpacacore::vendor::qhy
