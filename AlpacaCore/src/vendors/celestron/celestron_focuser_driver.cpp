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

// Celestron Focus Motor on its own USB port (issue #896). The motor answers AUX
// frames as device 0x12 at 9600 baud, over the same framing and serial
// transport as the CGX direct-USB mount (CelestronCgxProtocol). Command set
// from the reporter's tested script (issue #896) and INDI's celestron SCT
// focuser driver, which agree:
//
//   0x01 MC_GET_POSITION       -> 24-bit raw encoder count
//   0x02 MC_GOTO_FAST  u24     -> empty ack
//   0x13 MC_SLEW_DONE          -> 0xFF when idle, anything else while moving
//   0x24 MC_MOVE_POS   rate 0  -> stops the motor (Halt)
//   0x2A FOC_CALIB_ENABLE u8   -> 1 starts a calibration, 0 aborts it
//   0x2B FOC_CALIB_DONE        -> [complete, state]; state 0 means idle
//   TODO(#896): the 0x2B layout is read from INDI's code (its header comment
//   says the opposite for 0x2A, the code and the reporter's capture agree on
//   1 = start); confirm both on hardware.
//   0x2C FOC_GET_HS_POSITIONS  -> two big-endian u32: low and high limit;
//                                 high <= low means not calibrated
//   0xFE GET_VER               -> major, minor[, build hi, build lo]
//
// The raw count rises as the mirror moves toward the corrector, the reverse of
// a refractor's drawtube, so Position = high - raw (INDI's absPos): 0 is the
// high raw limit and MaxStep is high - low.
//
// Calibration (issue #896): the Celestron utilities send 0x2A 00 on exit,
// which the reporter saw wipe a good calibration, so an uncalibrated motor
// connects and the Calibrate action runs it here (INDI's flow). This driver
// sends 0x2A 00 only while the motor is calibrating: one it started, or one
// from an earlier session that the connect found still running.
// TODO(#896): the wipe is the reporter's observation on one firmware; whether
// the motor sweeps the encoder (and what 0x01 reads) during a calibration is
// not known yet.

#include <alpacacore/async_connectable.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/state_snapshot.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/celestron/celestron_cgx_protocol_wrapper.h>
#include <alpacacore/vendor/celestron/celestron_focuser_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace alpacacore::vendor::celestron {

namespace {

constexpr std::uint8_t kApp = 0x20;
constexpr std::uint8_t kFocuser = 0x12;
constexpr std::uint8_t kGetPosition = 0x01;
constexpr std::uint8_t kGotoFast = 0x02;
constexpr std::uint8_t kSlewDone = 0x13;
constexpr std::uint8_t kMovePositive = 0x24;
constexpr std::uint8_t kCalibrationEnable = 0x2a;
constexpr std::uint8_t kCalibrationDone = 0x2b;
constexpr std::uint8_t kGetLimits = 0x2c;
constexpr std::uint8_t kGetVersion = 0xfe;
constexpr std::uint32_t kMaxRawPosition = 0x00ffffffU;

constexpr auto kExchangeTimeout = std::chrono::milliseconds(500);
// One poll is two exchanges of about 10 bytes each way at 9600 baud, so the
// period leaves the wire mostly free for Move/Halt (same shape as Gemini).
constexpr auto kStatePollPeriod = std::chrono::milliseconds(250);
constexpr auto kSnapshotMaxAge = 2 * kStatePollPeriod;
constexpr int kFaultThreshold = 3;
// INDI waits about a second after the start before it reads 0x2B; a status
// read sooner could still say idle and end a calibration the motor runs.
constexpr auto kCalibrationStartGrace = std::chrono::seconds(1);

constexpr const char* kActionCalibrate = "Calibrate";
constexpr const char* kActionAbortCalibration = "AbortCalibration";
constexpr const char* kActionCalibrationStatus = "CalibrationStatus";

bool same_action(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

std::uint32_t read_be(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::size_t count) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        value = (value << 8) | bytes[offset + i];
    }
    return value;
}

}  // namespace

class CelestronFocuserDriver final : public FocuserDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    CelestronFocuserDriver(int device_number, std::string port_path,
                           util::TaskClock& clock = util::default_task_clock())
        : AsyncConnectable("Celestron Focuser"),
          clock_(clock),
          device_number_(device_number),
          port_path_(std::move(port_path)) {}

    ~CelestronFocuserDriver() override {
        // Blocks new connection tasks, then joins the in-flight one; MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        state_poll_.cancel_all_and_join();
        if (connected_.load()) {
            try {
                // Qualified: virtual dispatch is gone in a destructor anyway;
                // saying so explicitly keeps clang-analyzer's VirtualCall happy.
                CelestronFocuserDriver::set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("Celestron", "Error during focuser destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override { return device_number_; }
    std::string get_name() const override { return "Celestron Focus Motor"; }
    DeviceType get_device_type() const override { return DeviceType::Focuser; }
    std::string fallback_unique_id() const override { return "CELESTRON_FOCUSER_" + std::to_string(device_number_); }
    std::string get_description() const override { return "Celestron Focus Motor (USB)"; }
    std::string get_driver_info() const override { return "AlpacaCore Celestron Focuser Driver"; }
    std::string get_driver_version() const override { return alpacacore::kVersion; }
    int get_interface_version() const override { return 4; }

    // Motor firmware captured at connect; web UI only, never DriverInfo.
    std::optional<std::string> get_device_firmware() const override {
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        if (firmware_.empty()) {
            return std::nullopt;
        }
        return firmware_;
    }

    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }

    void disconnect() override {
        stop_connection_thread();
        try {
            set_connected(false);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("Celestron", "Focuser disconnect error: " + std::string(e.what()));
        }
    }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        // One transition at a time (open-astro#333); getters never take this.
        std::lock_guard<std::mutex> transition(transition_mutex_);
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
            protocol_.connect_serial(port_path_, AuxLinkSpeed::Baud9600);
            {
                std::lock_guard<std::mutex> lock(calibration_mutex_);
                calibration_ = CalibrationState{};
            }
            try {
                if (!read_limits()) {
                    ALPACA_LOG_WARN("Celestron",
                                    "Focus motor is not calibrated; run the Calibrate action before moving it");
                }
                resume_running_calibration();
            } catch (...) {
                protocol_.disconnect();
                throw;
            }
            read_firmware();
            snapshot_.reset();
            {
                std::lock_guard<std::mutex> lock(health_mutex_);
                health_.reset();
            }
            // First frame before Connected reads true, so a getter has an
            // answer the moment Connect returns. Connect may do I/O (decision
            // 0016 rule 4); a getter may not. A lost first reply gets one
            // retry here. A connect whose retry also publishes nothing still
            // connects, and getters answer "not available yet" until the
            // 250 ms poll publishes.
            poll_state_once();
            if (!snapshot_.read().has_value()) {
                poll_state_once();
            }
            connected_.store(true);
            start_state_poll();
            const auto limits = current_limits();
            ALPACA_LOG_INFO("Celestron", "Focuser connected (limits " + std::to_string(limits.low) + ".." +
                                             std::to_string(limits.high) + ")");
        } else {
            // Driver state first, transport close second (driver-lifecycle.instructions.md, issue #387).
            {
                std::lock_guard<std::mutex> lock(firmware_mutex_);
                firmware_.clear();
            }
            connected_.store(false);
            state_poll_.cancel_all_and_join();
            snapshot_.reset();
            {
                std::lock_guard<std::mutex> lock(health_mutex_);
                health_.reset();
            }
            protocol_.disconnect();
            ALPACA_LOG_INFO("Celestron", "Focuser disconnected");
        }
    }

    std::vector<std::string> get_supported_actions() const override {
        return {kActionCalibrate, kActionAbortCalibration, kActionCalibrationStatus};
    }

    std::string action(std::string_view action_name, std::string_view) override {
        if (same_action(action_name, kActionCalibrate)) {
            start_calibration();
            return "";
        }
        if (same_action(action_name, kActionAbortCalibration)) {
            ensure_connected();
            resync_if_needed();
            abort_calibration_if_running();
            return "";
        }
        if (same_action(action_name, kActionCalibrationStatus)) {
            std::lock_guard<std::mutex> lock(calibration_mutex_);
            // Under the lock, as usable_limits(): a call that overlaps a
            // disconnect and reconnect would read the next connect's cleared
            // calibration; connected_ is set only after that connect has
            // read the limits.
            ensure_connected();
            // Until the poll reads the limits after it ends, the old ones are
            // not the answer (an abort can wipe them).
            if (calibration_.running || calibration_.refresh_limits) return "Calibrating";
            return calibration_.limits.calibrated() ? "Calibrated" : "NotCalibrated";
        }
        throw AlpacaException("Action not supported: " + std::string(action_name), AlpacaError::ActionNotImplemented);
    }

    bool can_action(std::string_view action_name) const override {
        return same_action(action_name, kActionCalibrate) || same_action(action_name, kActionAbortCalibration) ||
               same_action(action_name, kActionCalibrationStatus);
    }

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

    bool get_is_moving() const override { return served_frame().moving; }

    int get_max_step() const override {
        ensure_connected();
        const auto limits = usable_limits();
        return static_cast<int>(limits.high - limits.low);
    }

    int get_max_increment() const override { return get_max_step(); }

    int get_position() const override {
        const auto frame = served_frame();
        static_cast<void>(usable_limits());
        return frame.position;
    }

    double get_step_size() const override {
        // The motor reports encoder counts only; microns per count depend on
        // the telescope it drives.
        ensure_connected();
        throw AlpacaException("Step size not available for this focuser", AlpacaError::PropertyNotImplemented);
    }

    bool get_temp_comp_available() const override {
        ensure_connected();
        return false;
    }

    bool get_temp_comp() const override {
        ensure_connected();
        return false;
    }

    void set_temp_comp(bool) override {
        ensure_connected();
        throw AlpacaException("Temperature compensation not supported", AlpacaError::NotImplemented);
    }

    double get_temperature() const override {
        ensure_connected();
        throw AlpacaException("The Celestron focus motor has no temperature sensor",
                              AlpacaError::PropertyNotImplemented);
    }

    void halt() override {
        ensure_connected();
        // Never refused for a link fault: Halt is the safety stop. A link left
        // desynchronized by an earlier timeout is resynchronized first (no
        // reopen); this sends a stop, never a repeat of the motion that timed out.
        resync_if_needed();
        // During a calibration the stop is its abort (INDI); 0x24 is not.
        if (!abort_calibration_if_running()) {
            exchange(kMovePositive, {0x00});
        }
        snapshot_.write([](StateFrame& frame) { frame.moving = false; });
    }

    void move(int position) override {
        ensure_connected();
        throw_if_faulted();
        const auto limits = usable_limits();
        const int max_step = static_cast<int>(limits.high - limits.low);
        // ConformU expects graceful clamping, not an exception (as Gemini).
        if (position < 0) {
            position = 0;
        } else if (position > max_step) {
            position = max_step;
        }
        const std::uint32_t raw = limits.high - static_cast<std::uint32_t>(position);
        // A lost poll reply must not fail this Move until the next poll; the
        // resync only flushes, so a timed-out GOTO is still never resent.
        resync_if_needed();
        exchange(kGotoFast, {static_cast<std::uint8_t>(raw >> 16), static_cast<std::uint8_t>(raw >> 8),
                             static_cast<std::uint8_t>(raw)});
        // Write-through: IsMoving reads true right after a successful Move.
        snapshot_.write([](StateFrame& frame) { frame.moving = true; });
    }

private:
    struct StateFrame {
        int position = 0;
        bool moving = false;
    };

    struct Limits {
        std::uint32_t low = 0;
        std::uint32_t high = 0;
        bool calibrated() const { return high > low && high <= kMaxRawPosition; }
    };

    struct CalibrationState {
        Limits limits;
        bool running = false;
        util::TaskClock::clock::time_point started{};
        // Bumped by every start and abort, so a poll that read the status
        // before one of them does not end the newer calibration.
        std::uint64_t generation = 0;
        bool refresh_limits = false;
    };

    std::vector<std::uint8_t> exchange(std::uint8_t command, std::vector<std::uint8_t> payload = {}) {
        return protocol_
            .exchange(
                AuxFrame{.source = kApp, .destination = kFocuser, .command = command, .payload = std::move(payload)},
                kExchangeTimeout)
            .payload;
    }

    // A timed-out exchange leaves the AUX stream desynchronized; resync()
    // flushes stale input on the port already held. It never reopens one, so a
    // Halt racing a disconnect cannot leave the port open behind it.
    void resync_if_needed() {
        if (!protocol_.is_healthy()) {
            protocol_.resync();
        }
    }

    // Reads 0x2C into the calibration state; returns whether it is calibrated.
    bool read_limits() {
        const auto reply = exchange(kGetLimits);
        if (reply.size() != 8) {
            throw AlpacaException(
                "Celestron focuser: limit query returned " + std::to_string(reply.size()) + " bytes, expected 8",
                AlpacaError::DriverException);
        }
        const Limits limits{.low = read_be(reply, 0, 4), .high = read_be(reply, 4, 4)};
        std::lock_guard<std::mutex> lock(calibration_mutex_);
        calibration_.limits = limits;
        return limits.calibrated();
    }

    Limits current_limits() const {
        std::lock_guard<std::mutex> lock(calibration_mutex_);
        return calibration_.limits;
    }

    // The limits Position, MaxStep and Move work from, or InvalidOperation
    // while the motor has none.
    Limits usable_limits() const {
        std::lock_guard<std::mutex> lock(calibration_mutex_);
        // A getter that passed its own check before a disconnect and reconnect
        // would read the next connect's cleared calibration; connected_ is set
        // only after that connect has read the limits.
        ensure_connected();
        if (calibration_.running) {
            throw AlpacaException("Celestron focus motor calibration is in progress", AlpacaError::InvalidOperation);
        }
        // A calibration that just ended or was aborted (which can wipe it)
        // left limits the motor may no longer have until the poll reads 0x2C.
        if (calibration_.refresh_limits) {
            throw AlpacaException("Celestron focus motor calibration has ended; its limits are being read again",
                                  AlpacaError::InvalidOperation);
        }
        if (!calibration_.limits.calibrated()) {
            throw AlpacaException(
                "Celestron focus motor is not calibrated; run the Calibrate action (about 5 minutes) first",
                AlpacaError::InvalidOperation);
        }
        return calibration_.limits;
    }

    void start_calibration() {
        ensure_connected();
        throw_if_faulted();
        {
            std::lock_guard<std::mutex> lock(calibration_mutex_);
            if (calibration_.running) {
                throw AlpacaException("Celestron focus motor calibration is already in progress",
                                      AlpacaError::InvalidOperation);
            }
        }
        if (served_frame().moving) {
            throw AlpacaException("Celestron focus motor is moving; Halt it before calibrating",
                                  AlpacaError::InvalidOperation);
        }
        resync_if_needed();
        std::lock_guard<std::mutex> command(calibration_command_mutex_);
        {
            std::lock_guard<std::mutex> lock(calibration_mutex_);
            if (calibration_.running) {
                throw AlpacaException("Celestron focus motor calibration is already in progress",
                                      AlpacaError::InvalidOperation);
            }
        }
        const auto session = protocol_.session_generation();
        try {
            exchange(kCalibrationEnable, {0x01});
        } catch (const std::exception&) {
            // The frame may have reached the motor even though no reply came back.
            ALPACA_LOG_WARN("Celestron",
                            "Focus motor calibration start was not confirmed; the motor may be calibrating. "
                            "Check CalibrationStatus or power-cycle the motor before moving it");
            throw;
        }
        {
            std::lock_guard<std::mutex> lock(calibration_mutex_);
            // A reconnect in between reset the state for its own session.
            if (protocol_.session_generation() != session) {
                throw AlpacaException("Focuser not connected", AlpacaError::NotConnected);
            }
            calibration_.running = true;
            calibration_.started = clock_.now();
            ++calibration_.generation;
        }
        snapshot_.write([](StateFrame& frame) { frame.moving = true; });
        ALPACA_LOG_INFO("Celestron", "Focus motor calibration started");
    }

    // Connect half of a calibration: a disconnect does not abort one, so a
    // motor still calibrating from an earlier session is tracked again
    // instead of being served as idle. A failed status read leaves it idle.
    void resume_running_calibration() {
        std::vector<std::uint8_t> reply;
        try {
            reply = exchange(kCalibrationDone);
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG("Celestron",
                             std::string("Focus motor calibration status not read at connect: ") + e.what());
            return;
        }
        if (reply.size() < 2 || reply[0] != 0 || reply[1] == 0) return;
        {
            std::lock_guard<std::mutex> lock(calibration_mutex_);
            calibration_.running = true;
            calibration_.started = clock_.now() - kCalibrationStartGrace;
            ++calibration_.generation;
        }
        ALPACA_LOG_WARN("Celestron",
                        "Focus motor is still calibrating from an earlier session; waiting for it to finish");
    }

    // Sends 0x2A 00 only while a calibration runs: on an idle motor it would
    // wipe the calibration. Returns whether it sent the abort.
    bool abort_calibration_if_running() {
        // Held across the check and the send: the poll's 0x2B can otherwise
        // see the calibration finish in between, and the abort then wipes it.
        std::lock_guard<std::mutex> command(calibration_command_mutex_);
        {
            std::lock_guard<std::mutex> lock(calibration_mutex_);
            if (!calibration_.running) return false;
        }
        exchange(kCalibrationEnable, {0x00});
        {
            std::lock_guard<std::mutex> lock(calibration_mutex_);
            calibration_.running = false;
            calibration_.refresh_limits = true;
            ++calibration_.generation;
        }
        snapshot_.write([](StateFrame& frame) { frame.moving = false; });
        ALPACA_LOG_INFO("Celestron", "Focus motor calibration aborted");
        return true;
    }

    // Poll-thread half of a calibration: reads 0x2B once the start grace has
    // passed and reads the limits again when it has ended.
    void poll_calibration() {
        std::unique_lock<std::mutex> command(calibration_command_mutex_);
        CalibrationState state;
        {
            std::lock_guard<std::mutex> lock(calibration_mutex_);
            state = calibration_;
        }
        if (state.running && clock_.now() - state.started >= kCalibrationStartGrace) {
            const auto reply = exchange(kCalibrationDone);
            if (reply.size() < 2) {
                throw AlpacaException("calibration status reply is shorter than 2 bytes", AlpacaError::DriverException);
            }
            const bool complete = reply[0] != 0;
            if (complete || reply[1] == 0) {
                std::lock_guard<std::mutex> lock(calibration_mutex_);
                if (calibration_.generation == state.generation) {
                    calibration_.running = false;
                    calibration_.refresh_limits = true;
                    state.refresh_limits = true;
                }
                ALPACA_LOG_INFO("Celestron", complete ? "Focus motor calibration complete"
                                                      : "Focus motor calibration ended without completing");
            }
        }
        command.unlock();
        if (state.refresh_limits) {
            const bool calibrated = read_limits();
            {
                std::lock_guard<std::mutex> lock(calibration_mutex_);
                calibration_.refresh_limits = false;
            }
            const auto limits = current_limits();
            const std::string text = std::to_string(limits.low) + ".." + std::to_string(limits.high);
            if (calibrated) {
                ALPACA_LOG_INFO("Celestron", "Focus motor limits " + text);
            } else {
                ALPACA_LOG_WARN("Celestron", "Focus motor is not calibrated (limits " + text + ")");
            }
        }
    }

    // A failed version query must not fail the connect.
    void read_firmware() {
        std::string text;
        try {
            const auto reply = exchange(kGetVersion);
            if (reply.size() >= 2) {
                text = std::to_string(reply[0]) + "." + std::to_string(reply[1]);
                if (reply.size() >= 4) {
                    text += "." + std::to_string(read_be(reply, 2, 2));
                }
            }
        } catch (const std::exception&) {
            ALPACA_LOG_WARN("Celestron", "Focuser firmware query failed; resynchronizing the link");
            resync_if_needed();  // a failed flush closes the link and fails the connect
        }
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        firmware_ = text;
    }

    void throw_if_faulted() const {
        std::lock_guard<std::mutex> lock(health_mutex_);
        if (health_.faulted()) {
            throw AlpacaException("Celestron focuser communications compromised: " + health_.fault(),
                                  AlpacaError::DriverException);
        }
    }

    StateFrame served_frame() const {
        ensure_connected();
        throw_if_faulted();
        auto reading = snapshot_.read();
        if (!reading.has_value()) {
            // Connected but no frame: the connect's polls were lost, or a
            // racing Move/Halt write dropped it. Getters never touch the
            // device (decisions 0010 and 0016): answer "not available yet"
            // until the next poll, at most one poll period away.
            ensure_connected();
            throw_if_faulted();
            throw AlpacaException("Focuser state not available yet", AlpacaError::DriverException);
        }
        return reading->value;
    }

    void poll_state_once() {
        const auto token = snapshot_.begin_poll();
        StateFrame frame;
        try {
            resync_if_needed();
            const auto position = exchange(kGetPosition);
            if (position.size() != 3) {
                throw AlpacaException("position reply is not 24 bits", AlpacaError::DriverException);
            }
            const auto done = exchange(kSlewDone);
            if (done.size() != 1) {
                throw AlpacaException("slew-done reply is not one byte", AlpacaError::DriverException);
            }
            poll_calibration();
            CalibrationState state;
            {
                std::lock_guard<std::mutex> lock(calibration_mutex_);
                state = calibration_;
            }
            // Meaningless while uncalibrated; the getters refuse it then.
            frame.position = static_cast<int>(static_cast<std::int64_t>(state.limits.high) -
                                              static_cast<std::int64_t>(read_be(position, 0, 3)));
            frame.moving = done[0] != 0xff || state.running;
        } catch (const std::exception&) {
            std::lock_guard<std::mutex> lock(health_mutex_);
            if (health_.note_failure("no valid reply to the state poll", kFaultThreshold, clock_.now())) {
                ALPACA_LOG_ERROR("Celestron", "Focuser communications compromised: " + health_.fault());
            } else {
                ALPACA_LOG_DEBUG("Celestron", "Focuser state poll failed; the frame keeps its age");
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lock(health_mutex_);
            if (health_.on_reply()) {
                ALPACA_LOG_INFO("Celestron", "Focuser communications restored");
            }
        }
        snapshot_.publish(token, frame);
    }

    void run_state_poll(util::OperationContext& ctx) {
        if (!ctx.wait_for(kStatePollPeriod)) {
            return;
        }
        for (;;) {
            const auto started = clock_.now();
            poll_state_once();
            const auto spent = clock_.now() - started;
            const auto ticks = spent / kStatePollPeriod + 1;
            if (!ctx.wait_for(ticks * kStatePollPeriod - spent)) {
                return;
            }
        }
    }

    void start_state_poll() {
        try {
            state_poll_.start([this](util::OperationContext& ctx) { run_state_poll(ctx); });
        } catch (const std::exception&) {
            ALPACA_LOG_ERROR("Celestron", "Focuser state poll did not start");
        }
    }

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Focuser not connected", AlpacaError::NotConnected);
        }
    }

    util::TaskClock& clock_;
    int device_number_;
    std::string port_path_;
    std::atomic<bool> connected_{false};
    CelestronCgxProtocol protocol_{"Celestron focuser"};
    // Serializes the calibration commands (0x2A start and abort) with the
    // poll's 0x2B read and its decision. Held across an exchange; taken before
    // the protocol's mutex and before calibration_mutex_.
    mutable std::mutex calibration_command_mutex_;
    // Limits and calibration progress; never held across an exchange.
    mutable std::mutex calibration_mutex_;
    CalibrationState calibration_;
    mutable util::StateSnapshot<StateFrame> snapshot_{clock_, kSnapshotMaxAge};
    mutable std::mutex health_mutex_;
    util::PolledLinkHealth health_;
    util::OperationGeneration state_poll_generation_;
    util::AsyncOperation state_poll_{"Celestron focuser state poll", state_poll_generation_, clock_};
    // Own mutex, not the base's connection mutex (see the Gemini focuser).
    mutable std::mutex firmware_mutex_;
    std::string firmware_;
    std::mutex transition_mutex_;
};

std::unique_ptr<FocuserDriver> create_celestron_focuser(int device_number, const std::string& port_path) {
    return std::make_unique<CelestronFocuserDriver>(device_number, port_path);
}

std::unique_ptr<FocuserDriver> create_celestron_focuser(int device_number, const std::string& port_path,
                                                        util::TaskClock& clock) {
    return std::make_unique<CelestronFocuserDriver>(device_number, port_path, clock);
}

}  // namespace alpacacore::vendor::celestron
