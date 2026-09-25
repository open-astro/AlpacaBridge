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
#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/auto_detect.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/host_clock.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/motion_policy.h>
#include <alpacacore/vendor/skywatcher/skywatcher_protocol_wrapper.h>
#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <numbers>
#include <optional>
#include <thread>

namespace alpacacore::vendor::skywatcher {

namespace {

constexpr double kHoursToDegrees = 15.0;
// Axis counts value that represents the home position (pointing at the pole,
// counterweight side down). The controller's position register is initialized
// to this offset so signed axis angles fit in the unsigned 24-bit counter.
constexpr uint32_t kHomeCounts = 0x800000;
// Hour angle of the dec-axis sweep at the counterweight-down home: the dec
// axis lies in the meridian plane there, so rotating it alone moves the OTA
// along the HA = +/-6 h circle (see the pointing-model comment in the driver).
constexpr double kHomeHourAngleOffsetHours = 6.0;

// The signed home term itself is `home_term_sign_locked() *
// kHomeHourAngleOffsetHours * branch`, with the branch read back through
// branch_from_axis_locked() (open-astro#459, #458).

// open-astro#458: the dec-axis count sense of a motor board, by its ":e" mount
// code: +1 or -1 where it was MEASURED on hardware (the rows in the
// pointing-model comment), 0 for every other board. The sense is wiring, not
// latitude (the board is never told the latitude), and it decides which side
// of the meridian the tube swings to for a positive dec-axis angle.
// Caveat: indi-eqmod and GSServer apply eps = +1 to every board, and only the
// EQM-35 Pro contradicts that here -- the EQ-AL55i Pro, measured independently
// on a reporter's mount, agrees with them (open-astro#306), so the EQM-35 Pro
// is the outlier among three boards rather than one of two readings. If the
// cause turns out to be a driver-side a2 zero or direction convention rather
// than per-board wiring, this table has to be reworked, not extended
// (open-astro#579).
constexpr int measured_dec_axis_sense(std::uint8_t mount_code) {
    switch (mount_code) {
        case 0x09:  // EQ-AL55i Pro: dovetail west at a2 = +89.4, north (+40), 2026-09-20
            return +1;
        case 0x32:  // EQM-35 Pro: -37.2 on 2026-09-12, and at +37.2 on 2026-09-19
            return -1;
        case 0x45:  // Wave 150i: the #432 report, +45.45
            return +1;
        default:
            return 0;
    }
}

constexpr uint32_t kCountsMask = 0xFFFFFF;
constexpr double kSiderealDegPerSec = 360.0 / 86164.0905;
constexpr double kDefaultGuideRateDegPerSec = 0.5 * kSiderealDegPerSec;
// ASCOM DriveRates: 0 = Sidereal, 1 = Lunar, 2 = Solar (3 = King unsupported).
// Standard drive rates (INDI TRACKRATE_* constants), arcsec/s over 3600.
constexpr double kLunarDegPerSec = 14.511415 / 3600.0;
// RightAscensionRate is in seconds of RA per SIDEREAL second (ASCOM):
// 1 s RA = 15 arcsec, scaled sidereal->SI by 86164.1/86400.
constexpr double kRaRateSecondsToDegPerSec = 15.0 / (0.9972695663 * 3600.0);
// Dec rates within this factor of the slow-mode floor are duty-cycled rather
// than run continuously (":I" already pinned at 0xFFFFFF cannot go slower).
constexpr double kSlowModeFloorPad = 1.02;
constexpr double kSolarDegPerSec = 15.0 / 3600.0;
// ~800x sidereal, the classic Sky-Watcher maximum slew rate.
constexpr double kMaxMoveAxisRateDegPerSec = 800.0 * kSiderealDegPerSec;
// Above ~128x sidereal the controller needs high-speed (fast) mode.
constexpr double kFastModeThresholdDegPerSec = 128.0 * kSiderealDegPerSec;
constexpr auto kPositionCacheTtl = std::chrono::seconds(2);
// Longest a comms fault may serve last-known position/slewing state.
constexpr auto kStaleCacheLimit = std::chrono::seconds(10);
constexpr auto kOffsetModelHold = std::chrono::minutes(30);  // offset sessions serve the model this long
constexpr auto kPulseGuideCompletionDelay = std::chrono::milliseconds(1000);
constexpr auto kAxisStopTimeout = std::chrono::seconds(5);

// How long the CONNECT path may spend confirming that axes it just stopped
// have actually come to rest -- shared across BOTH axes, not per axis.
// Deliberately far shorter than kAxisStopTimeout, for two reasons. The whole
// connect has to fit inside the ~5 s a Platform 7 client (ConformU included)
// allows Connect() before abandoning it, and this phase is only one item in a
// sequence that also does ":e", ":a"/":b"/":g" and ":f" per axis, sometimes
// ":F"/":E", and a position-cache warm. And the confirmation is best effort
// by construction: ":K" is already on the wire before any polling starts, and
// running out of budget logs and continues rather than failing the connect --
// so a tight bound costs a log line, while a loose one costs the connect.
// One deceleration ramp is documented elsewhere here as able to exceed 1 s;
// 2 s covers that with margin and still leaves the rest of the budget free.
constexpr auto kConnectStopConfirmBudget = std::chrono::seconds(2);

// open-astro#521's relink window now lives in util/motion_policy.h,
// alongside open-astro#547's client-silence interval -- the two read as
// one policy (see that header for the full rationale) rather than two
// unrelated numbers.
using alpacacore::util::kRelinkMotionPreserveWindow;
// A goto the controller reports stopped can still be finishing its approach:
// EQM-35 Pro (MC fw 3.39), 2026-09-12, the third landing refinement read 11
// counts short of its target while ":f" already said stopped, and the
// tracking restart sent into that window left the RA axis running at ~2x
// sidereal for the rest of the session (ConformU "PulseGuide +9.0 North:
// East-West movement outside tolerance", RA change -5.68 s). Every clean
// landing in the same log read exactly on target. A slew is therefore
// complete only when the axis is stopped AND its count is unchanged across
// kLandingSettle; the post-slew tracking restart is then rate-checked.
constexpr auto kLandingSettle = std::chrono::milliseconds(60);
constexpr auto kLandingSettleTimeout = std::chrono::seconds(2);
constexpr auto kPostSlewRateWindow = std::chrono::milliseconds(300);
constexpr double kPostSlewRateTolerance = 0.25;
// Below this rate an in-place ":I" pulse adjustment is unreliable (and a
// non-positive rate needs a direction change ":I" cannot deliver).
constexpr double kMinInPlacePulseRateDegPerSec = 0.05 * kSiderealDegPerSec;
// Shortest RA pulse that still gets the sample-based "did the rate actually
// apply" check (see verify_live_rate_or_rekick). The check costs >= ~450 ms
// of wall time DURING the pulse (more at small rate deltas, where the sample
// window stretches to stay resolvable); on a typical 50-500 ms autoguider
// pulse that would dominate the pulse itself, so those rely on the ":J" kick
// alone.
constexpr int kMinPulseForRateVerifyMs = 1500;
// verify_live_rate_or_rekick timing. kRateVerifyMaxWindow is the ceiling
// used by callers with no duration budget to respect (the RightAscensionRate
// /TrackingRate background task, and the pulse's own end-of-pulse restore
// check, which runs after the axis already stopped and so only adds
// latency, never overshoot). The pulse DISPATCH check is different: it
// samples while the axis is already running at the pulse rate, and its
// result only ever SHRINKS the remaining hold (never extends it -- see the
// call site), so a window bigger than the pulse's own duration would let
// the axis run at the pulse rate for longer than commanded. That call site
// caps its window at (duration - kRateVerifySettle) instead of this ceiling
// (bot review round 1 on open-astro/AlpacaBridge#248: a low-guide-rate pulse
// right at kMinPulseForRateVerifyMs could stretch to ~3.15 s, over 2x its
// commanded duration).
constexpr auto kRateVerifySettle = std::chrono::milliseconds(150);
constexpr auto kRateVerifyMinWindow = std::chrono::milliseconds(300);
constexpr auto kRateVerifyMaxWindow = std::chrono::milliseconds(3000);
// AutoHome (home index sensor) constants — SynScan/EQMod ":q"/":W" extended
// commands. Indexer reads: 0 = armed below the index, 0xFFFFFF = armed above,
// anything else = the count at which the sensor edge latched.
constexpr uint32_t kFeatureInquiry = 0x000001;
constexpr uint32_t kIndexerInquiry = 0x000000;
constexpr uint32_t kIndexerReset = 0x000008;
constexpr uint32_t kIndexerAbove = 0xFFFFFF;
// Hunt speeds (EQMod uses 800x for the coarse pass, 400x for the detect pass).
constexpr double kAutoHomeCoarseRateDegPerSec = 800.0 * kSiderealDegPerSec;
constexpr double kAutoHomeDetectRateDegPerSec = 400.0 * kSiderealDegPerSec;

double wrap_degrees(double deg) {
    double wrapped = std::fmod(deg, 360.0);
    if (wrapped < 0.0) {
        wrapped += 360.0;
    }
    return wrapped;
}

double wrap_hours(double hours) {
    double wrapped = std::fmod(hours, 24.0);
    if (wrapped < 0.0) {
        wrapped += 24.0;
    }
    return wrapped;
}

// Wrap an hour angle into [-12, +12).
double wrap_hour_angle(double hours) {
    double wrapped = std::fmod(hours, 24.0);
    if (wrapped < -12.0) {
        wrapped += 24.0;
    }
    if (wrapped >= 12.0) {
        wrapped -= 24.0;
    }
    return wrapped;
}

double compute_local_sidereal_time_hours(std::chrono::system_clock::time_point utc_time, double longitude_degrees) {
    using namespace std::chrono;
    // Sub-second resolution matters: whole-second truncation stepped LST (and
    // so RA) in 15 arcsec jumps, +-0.1 RA-s/s of jitter over a 10 s window.
    double days_since_epoch = duration<double>(utc_time.time_since_epoch()).count() / 86400.0;
    double jd = 2440587.5 + days_since_epoch;
    double t = (jd - 2451545.0) / 36525.0;
    double gmst = 280.46061837 + 360.98564736629 * (jd - 2451545.0) + 0.000387933 * t * t - (t * t * t) / 38710000.0;
    double lst = gmst + longitude_degrees;
    lst = std::fmod(lst, 360.0);
    if (lst < 0.0) {
        lst += 360.0;
    }
    return lst / kHoursToDegrees;
}

// Signed axis angle in degrees from the raw 24-bit counter.
double counts_to_degrees(uint32_t counts, uint32_t cpr) {
    int32_t delta = static_cast<int32_t>(counts) - static_cast<int32_t>(kHomeCounts);
    return static_cast<double>(delta) * 360.0 / static_cast<double>(cpr);
}

uint32_t degrees_to_counts(double degrees, uint32_t cpr) {
    int32_t delta = static_cast<int32_t>(std::lround(degrees / 360.0 * static_cast<double>(cpr)));
    return (kHomeCounts + static_cast<uint32_t>(delta)) & kCountsMask;
}

}  // namespace

class SkyWatcherTelescopeDriver : public TelescopeDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    SkyWatcherTelescopeDriver(int device_number, const ConnectionInfo& connection_info,
                              std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
                              std::optional<double> site_elevation_m,
                              std::unique_ptr<SkyWatcherProtocolWrapper> protocol)
        : AsyncConnectable("SkyWatcher"),
          device_number_(device_number),
          connection_info_(connection_info),
          protocol_(protocol ? std::move(protocol) : std::make_unique<SkyWatcherProtocolWrapper>()),
          site_latitude_(site_latitude_deg.value_or(0.0)),
          site_longitude_(site_longitude_deg.value_or(0.0)),
          site_elevation_m_(site_elevation_m.value_or(0.0)),
          // open-astro#274: survive the .value_or(0.0) collapse. 0.0/0.0 is a
          // real place, so the magic value cannot stand in for "never set";
          // only the optionals know, and only here.
          site_latitude_set_(site_latitude_deg.has_value()),
          site_longitude_set_(site_longitude_deg.has_value()) {
        guide_rate_.ra = kDefaultGuideRateDegPerSec;
        guide_rate_.dec = kDefaultGuideRateDegPerSec;
    }

    ~SkyWatcherTelescopeDriver() override {
        // Base contract: block/join the connection task FIRST, then cancel and
        // join the slew/pulse task threads, all before members are destroyed.
        shutdown_connection();
        cancel_async_tasks();
        if (connected_) {
            try {
                // Qualified: virtual dispatch is already gone in a destructor.
                SkyWatcherTelescopeDriver::set_connected(false);
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                // Destructor: nothing useful to do with a disconnect failure.
            }
        }
    }

    int get_device_number() const override { return device_number_; }

    // Model comes from the ":e" mount-code byte captured at connect. Falls back
    // to the generic name while disconnected. Served from the narrow firmware
    // mutex so it stays inside ConformU's 0.1 s FAST target. The "(EQMOD)"
    // suffix disambiguates from the synscan driver's get_name(), which
    // resolves to the identical model string for a mount reachable over both
    // its hand controller and its own USB/EQDIR port (hardware-confirmed
    // 2026-09-10: both connections reported plain "Sky-Watcher EQM-35 Pro"
    // for the same physical mount, indistinguishable in a client's chooser).
    // "EQMOD" is the ecosystem's name for the direct motor-controller path
    // (INDI "EQMod Mount" covers its AZ-GTi/Star Adventurer variants too;
    // ASCOM "EQMOD"), and it holds for every transport this driver speaks --
    // built-in USB, EQDIR cable, a handset in PC Direct Mode, and Wi-Fi --
    // which "USB"/"EQDIR" would not.
    std::string get_name() const override {
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        if (model_cache_.empty()) {
            return "Sky-Watcher Mount (EQMOD)";
        }
        return "Sky-Watcher " + model_cache_ + " (EQMOD)";
    }

    DeviceType get_device_type() const override { return DeviceType::Telescope; }

    std::string get_unique_id() const override { return "SkyWatcher_" + std::to_string(device_number_); }

    std::string get_description() const override { return "Sky-Watcher Motor Controller Mount Driver"; }

    std::string get_driver_info() const override { return "AlpacaCore SkyWatcher Driver v0.1"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Motor board firmware captured at connect; surfaced in the web UI only,
    // under its own narrow mutex so a configureddevices poll never blocks on
    // the coarse mutex_ held across the multi-second connect.
    std::optional<std::string> get_device_firmware() const override {
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        if (firmware_cache_.empty()) {
            return std::nullopt;
        }
        return firmware_cache_;
    }

    int get_interface_version() const override { return 4; }

    bool get_connected() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        // open-astro#445: connected_ records that a connect succeeded; the
        // link is asked too, so a pulled adapter reads false at once.
        return connected_ && protocol_->link_alive();
    }

    void connect() override { start_connection_task(true); }
    void disconnect() override { start_connection_task(false); }
    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        auto& protocol = *protocol_;
        bool relink = false;
        if (connected) {
            std::lock_guard<std::mutex> lock(mutex_);
            relink = connected_ && !protocol.link_alive();
        }
        if (!connected || relink) {
            // Join background task threads BEFORE taking mutex_ (they take it).
            // A relink tears a session down too: a slew task left over from
            // the lost link must not resume against the new one.
            cancel_async_tasks();
        }
        std::unique_lock<std::mutex> lock(mutex_);
        if (connected && !relink && connected_ && !protocol.link_alive()) {
            // The loss became visible after the probe above, so the tasks were
            // not cancelled. A slew task blocked on mutex_ would wake to the
            // reconnected session and resume against the new link: drop the
            // lock and join it first, as the probe path does.
            lock.unlock();
            cancel_async_tasks();
            lock.lock();
        }
        if (connected && connected_ && !protocol.link_alive()) {
            // open-astro#445: a connect against a dead link used to hit the
            // idempotency return below and report success with no reconnect.
            // Settle the dead session first, then run the gates as for any
            // disconnected device.
            ALPACA_LOG_WARN("SkyWatcher", "Connect requested on a lost link; reconnecting");
            protocol.disconnect();
            connected_ = false;
            reset_runtime_state_locked();
        }
        if (!connected && record_disconnect_if_connect_in_flight(connected_)) {
            return;
        }
        if (connected && consume_pending_disconnect(connected_)) {
            return;
        }
        if (connected == connected_) {
            return;
        }

        if (connected) {
            // open-astro#274: the mount stores no site of its own, so an
            // unconfigured device would run on 0.0/0.0. hemisphere_south_locked()
            // is site_latitude_ < 0.0, which silently puts a southern rig on
            // northern pointing math: the #432 sky frame (both the a1 term and
            // dec), the RA tracking direction (#250, restored by #432) and the
            // Dec rate / pulse-guide sign (#253). NOT #261: the pier-side
            // label is picked from the sky hour angle and is
            // hemisphere-independent (#432); the mechanical branch is
            // `k * side` and mirrors with the hemisphere only on a board with
            // a measured sense (#458). Refuse rather than point wrongly.
            if (!site_coordinates_known_locked()) {
                throw AlpacaException(
                    "Site latitude and longitude must be set before connecting: this mount stores no site of its "
                    "own, and pointing math (tracking direction, guide sign, pier side) is hemisphere-dependent. "
                    "Set them in the web UI's device configuration, or write SiteLatitude and SiteLongitude "
                    "before Connected.",
                    AlpacaError::InvalidOperation);
            }
            if (!protocol.connect(connection_info_)) {
                throw AlpacaException("Failed to connect to Sky-Watcher motor controller");
            }
            connected_ = true;
            reset_runtime_state_locked();

            try {
                MotorBoardInfo board = protocol.get_motor_board_info();
                {
                    std::lock_guard<std::mutex> fwlock(firmware_mutex_);
                    firmware_cache_ = board.firmware_version;
                    model_cache_ = board.model_name;
                }
                // The code in hex as well: comments, docs and
                // measured_dec_axis_sense() name boards as 0x32, 0x45, ...
                char code_hex[8];
                std::snprintf(code_hex, sizeof(code_hex), "0x%02X", static_cast<unsigned>(board.mount_code));
                ALPACA_LOG_INFO("SkyWatcher", "Motor board: " + board.model_name + " (mount code " + code_hex + ", " +
                                                  std::to_string(static_cast<int>(board.mount_code)) + "), firmware " +
                                                  board.firmware_version);
                dec_axis_sense_ = measured_dec_axis_sense(board.mount_code);  // open-astro#458
            } catch (...) {
                // A board that will not answer ":e" is still usable, so never
                // fail the connect over it -- but do not keep a previous
                // connection's identity either. Identity is not cosmetic: it
                // picks the measured dec-axis sense, and without it the session
                // runs on the unmeasured model (open-astro#458).
                ALPACA_LOG_WARN("SkyWatcher",
                                "Motor board not identified; pointing falls back to the unmeasured-board model, "
                                "which is 12 h out in hour angle on a board whose measured dec-axis sense differs "
                                "(open-astro#458)");
                std::lock_guard<std::mutex> fwlock(firmware_mutex_);
                firmware_cache_.clear();
                model_cache_.clear();
            }

            // open-astro#445: the sequence below is the first thing the board
            // is really asked (":e" above may fail by design). If it throws,
            // the port leads nowhere useful: do not stay latched as connected.
            try {
                axis_params_[0] = protocol.get_axis_parameters(kAxisRa);
                axis_params_[1] = protocol.get_axis_parameters(kAxisDec);

                // Feature inquiry (":q" data 0x000001): bit 0x04 = home index
                // sensor. Wave 100i reports it on both axes (0x100C); older
                // boards reject ":q" entirely, so failure just disables AutoHome.
                has_home_indexer_ = false;
                try {
                    uint32_t ra_features = protocol.get_feature(kAxisRa, kFeatureInquiry);
                    uint32_t dec_features = protocol.get_feature(kAxisDec, kFeatureInquiry);
                    has_home_indexer_ = (ra_features & 0x04) && (dec_features & 0x04);
                } catch (...) {  // NOLINT(bugprone-empty-catch)
                    // ":q" unsupported on this motor board.
                }

                // First power-up: position registers default to the home offset but
                // the controller reports "not initialized" and rejects motion until
                // ":F" is sent. Only stamp the home position when uninitialized so a
                // reconnect never clobbers an aligned session.
                AxisStatus entry_status[2];
                for (int axis = kAxisRa; axis <= kAxisDec; ++axis) {
                    AxisStatus status = protocol.inquire_status(axis);
                    entry_status[axis - 1] = status;
                    if (!status.init_done) {
                        protocol.set_position(axis, kHomeCounts);
                        protocol.initialization_done(axis);
                    }
                }
                // open-astro#521: the connect sequence used to consume only the
                // init_done bit above and discard `running` in the same reply,
                // so an axis still turning after a cable pull was neither
                // stopped nor reported.
                adopt_surviving_motion_locked(lock, entry_status);
            } catch (...) {
                protocol.disconnect();
                connected_ = false;
                reset_runtime_state_locked();
                throw;
            }

            // Warm the position cache so first property reads stay inside
            // ConformU fast-response targets.
            try {
                refresh_position_cache_locked(true);
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                // Cache warm-up only; reads will retry on demand.
            }
        } else {
            // Best effort: never leave an axis moving on disconnect.
            try {
                protocol.stop_motion(kAxisRa);
                protocol.stop_motion(kAxisDec);
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                // Best effort; status polling still reports the true state.
            }
            protocol.disconnect();
            connected_ = false;
            // Identity (model/firmware) is left as last known-good: a clean
            // disconnect doesn't change what mount this is, and clearing it
            // here made the web UI / configureddevices listing revert to a
            // generic name for any not-currently-connected device even after
            // a successful identify -- unlike the synscan driver, which
            // never clears mount_model_id_ on disconnect. The connect
            // sequence's own ":e" failure handler above still clears both on
            // a failed re-identify, so a genuinely different or unreachable
            // board on reconnect is never shown under a stale name.
            reset_runtime_state_locked();
        }
    }

    std::vector<std::string> get_supported_actions() const override { return {}; }

    std::string action(std::string_view action_name, std::string_view action_parameters) override {
        (void)action_parameters;
        throw AlpacaException("Action not supported: " + std::string(action_name), AlpacaError::ActionNotImplemented);
    }

    bool can_action(std::string_view action_name) const override {
        (void)action_name;
        return false;
    }

    std::string command_blind(std::string_view command, bool raw) override {
        (void)raw;
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        protocol_->send_raw_command(std::string(command) + "\r");
        return "";
    }

    bool command_bool(std::string_view command, bool raw) override {
        (void)raw;
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        std::string reply = protocol_->send_raw_command(std::string(command) + "\r");
        return !reply.empty() && reply[0] == '=';
    }

    std::string command_string(std::string_view command, bool raw) override {
        (void)raw;
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        return protocol_->send_raw_command(std::string(command) + "\r");
    }

    AlignmentMode get_alignment_mode() const override {
        // The Wave is a strain-wave EQ mount; it points and flips like a GEM
        // (two pier sides), so GermanPolar matches client expectations.
        return AlignmentMode::GermanPolar;
    }

    double get_altitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_position_cache_locked(false);
        auto [alt, az] = compute_alt_az_locked();
        (void)az;
        return alt;
    }

    double get_aperture_diameter() const override { return aperture_diameter_m_; }

    void set_aperture_diameter(double meters) override {
        if (meters < 0.0) {
            throw AlpacaException("Aperture diameter must be non-negative", AlpacaError::InvalidValue);
        }
        aperture_diameter_m_ = meters;
        if (meters > 0.0) {
            double radius = meters / 2.0;
            aperture_area_m2_ = radius * radius * std::numbers::pi;
        } else {
            aperture_area_m2_ = 0.0;
        }
    }

    double get_aperture_area() const override { return aperture_area_m2_; }

    bool get_at_home() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return at_home_;
    }

    bool get_at_park() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        return parked_;
    }

    double get_azimuth() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_position_cache_locked(false);
        auto [alt, az] = compute_alt_az_locked();
        (void)alt;
        return az;
    }

    bool get_can_find_home() const override { return true; }
    bool get_can_park() const override { return true; }
    bool get_can_pulse_guide() const override { return true; }

    bool get_is_pulse_guiding() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        const auto now = std::chrono::steady_clock::now();
        for (int i = 0; i < 2; ++i) {
            if (pulse_axis_active_[i] && now >= pulse_guide_end_time_[i]) {
                pulse_axis_active_[i] = false;
            }
        }
        // open-astro#559: the ASCOM property follows each pulse task's
        // actual end, not the ownership deadline above, which runs a fixed
        // second past the commanded duration and is now only a backstop.
        // open-astro#620: IsPulseGuiding is true while EITHER axis pulses --
        // ASCOM allows RA and Dec PulseGuide to run concurrently.
        return pulse_axis_in_motion_[0] || pulse_axis_in_motion_[1];
    }

    bool get_can_set_declination_rate() const override { return true; }
    bool get_can_set_guide_rates() const override { return true; }
    bool get_can_set_park() const override { return true; }
    bool get_can_set_pier_side() const override { return false; }
    bool get_can_set_right_ascension_rate() const override { return true; }
    bool get_can_set_tracking() const override { return true; }
    bool get_can_slew_alt_az() const override { return false; }
    bool get_can_slew_alt_az_async() const override { return false; }
    bool get_can_sync_alt_az() const override { return false; }
    bool get_can_slew() const override { return true; }
    bool get_can_slew_async() const override { return true; }
    bool get_can_sync() const override { return true; }
    bool get_can_unpark() const override { return true; }

    double get_declination() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_position_cache_locked(false);
        auto [ra, dec] = compute_ra_dec_locked();
        (void)ra;
        return std::clamp(dec, -90.0, 90.0);
    }

    double get_declination_rate() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return dec_rate_arcsec_per_sec_;
    }

    // DeclinationRate (arcsec/s): while tracking, the Dec axis runs a
    // speed-mode motion at the offset rate; rates below the slow-mode floor
    // (~0.26 arcsec/s, ":I" clamps at 0xFFFFFF) are produced by duty-cycling.
    // Issue #214 re-attempt: the previously-suspected hardware anomalies were
    // bench-disproven (axis tracks 5-320 as/s within 0.2%; no ":I" read
    // glitches) — both were artifacts of the pre-#216 refinement-goto races.
    void set_declination_rate(double rate) override {
        {
            // Idempotence check BEFORE reaping: a same-value rewrite must not
            // tear down a running duty worker it would never restart.
            std::unique_lock<std::mutex> lock(mutex_);
            check_connected();
            if (tracking_rate_ != 0) {
                throw AlpacaException("DeclinationRate can only be set at the Sidereal drive rate",
                                      AlpacaError::InvalidOperation);
            }
            if (rate == dec_rate_arcsec_per_sec_) {
                return;  // idempotent rewrite: leave the running motion alone
            }
        }
        reap_duty_task();
        bool need_duty = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            check_connected();
            if (tracking_rate_ != 0) {
                throw AlpacaException("DeclinationRate can only be set at the Sidereal drive rate",
                                      AlpacaError::InvalidOperation);
            }
            dec_rate_arcsec_per_sec_ = rate;
            // Per axis, for the same reason set_right_ascension_rate() and
            // set_site_latitude() ask per axis: only an operation that owns
            // the DEC axis re-applies the Dec offset when it releases it. An
            // East/West pulse or an RA MoveAxis made the whole-mount
            // predicate true while owning only RA, and its restore rewrites
            // the RA step period and never calls apply_dec_rate_offset_locked(),
            // so a continuous (at-or-above-floor) offset was stranded until
            // the next DeclinationRate write, tracking toggle or slew --
            // comet or satellite tracking while autoguiding is the way in
            // (round-3 review finding). The sub-floor duty path recovered on
            // its own through the duty worker's start gate; the continuous
            // one did not.
            const bool busy = axis_busy_locked(kAxisDec);
            if (!busy) {
                anchor_model_locked();  // continuous anchor: no position jump on a rate change
            }
            if (tracking_) {
                apply_dec_rate_offset_locked(lock, /*defer_motion=*/busy);
            }
            need_duty = ra_duty_rate_deg_s_ != 0.0 || dec_duty_rate_deg_s_ != 0.0;
        }
        if (need_duty) {
            start_duty_thread();
        }
    }

    bool get_tracking() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        return tracking_;
    }

    void set_tracking(bool tracking) override {
        bool need_duty = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            check_connected();
            if (tracking) {
                check_not_parked_locked("Tracking");
            }
            if (tracking == tracking_) {
                // Keep-alive reassertion (many clients poll-set Tracking):
                // the requested state already holds — do not stop/restart
                // the axes or churn the duty worker. MoveAxis restore needs
                // the restart and calls set_tracking_locked directly.
                return;
            }
            set_tracking_locked(lock, tracking);
            need_duty = tracking_ && (ra_duty_rate_deg_s_ != 0.0 || dec_duty_rate_deg_s_ != 0.0);
        }
        if (need_duty) {
            start_duty_thread();
        }
    }

    // (Re)start the duty-cycle worker for a sub-floor DeclinationRate. Call
    // with no mutexes held. The whole reap+create sequence is serialized by
    // duty_lifecycle_mutex_ so two concurrent setters can never reassign a
    // still-joinable std::thread (std::terminate). The join itself must NOT
    // happen under task_mutex_ — the worker's task_wait_for reacquires it on
    // wake, so joining while holding it deadlocks; the lifecycle mutex is
    // never taken by the worker, only by setters.
    void start_duty_thread() {
        std::lock_guard<std::mutex> lifecycle(duty_lifecycle_mutex_);
        reap_duty_locked_lifecycle();
        std::lock_guard<std::mutex> tlock(task_mutex_);
        duty_thread_ = std::thread([this]() { duty_loop(); });
    }

    double get_focal_length() const override { return focal_length_m_; }

    void set_focal_length(double meters) override {
        if (meters < 0.0) {
            throw AlpacaException("Focal length must be non-negative", AlpacaError::InvalidValue);
        }
        focal_length_m_ = meters;
    }

    GuideRate get_guide_rate() const override { return guide_rate_; }

    void set_guide_rate(const GuideRate& rate) override {
        if (!std::isfinite(rate.ra) || !std::isfinite(rate.dec)) {
            throw AlpacaException("GuideRate must be a finite number", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        double ra_fraction = rate.ra / kSiderealDegPerSec;
        double dec_fraction = rate.dec / kSiderealDegPerSec;
        if (ra_fraction < 0.0 || ra_fraction > 1.0 || dec_fraction < 0.0 || dec_fraction > 1.0) {
            throw AlpacaException("Guide rate must be between 0 and 1x sidereal", AlpacaError::InvalidValue);
        }
        guide_rate_ = rate;
    }

    double get_right_ascension() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_position_cache_locked(false);
        auto [ra, dec] = compute_ra_dec_locked();
        (void)dec;
        return ra;
    }

    double get_right_ascension_rate() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return ra_rate_sec_per_sidereal_sec_;
    }

    // RightAscensionRate (seconds of RA per sidereal second): folded into the
    // RA drive rate. Positive rate = RA increasing = axis advancing SLOWER
    // (RA = LST - HA), so the offset is SUBTRACTED; a large offset reverses
    // the axis, which needs a stop-and-restart (":I" cannot change direction).
    void set_right_ascension_rate(double rate) override {
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        if (tracking_rate_ != 0) {
            throw AlpacaException("RightAscensionRate can only be set at the Sidereal drive rate",
                                  AlpacaError::InvalidOperation);
        }
        if (rate == ra_rate_sec_per_sidereal_sec_) {
            return;  // idempotent rewrite
        }
        if (axis_busy_locked(kAxisRa)) {
            // An operation that owns the RA AXIS is in flight: store the rate
            // only — its restore path re-applies the effective (offset-folded)
            // drive rate when it releases the axis. Per axis, not the
            // whole-mount axes_busy_locked() this used to ask: a Dec pulse or
            // a Dec MoveAxis owns only the Dec axis and its restore never
            // touches RA, so the rate write was stranded with nothing
            // scheduled to apply it (round-2 review note; the same shape as
            // the set_site_latitude() defect fixed in the commit before this).
            ra_rate_sec_per_sidereal_sec_ = rate;
            double eff = effective_ra_rate_locked();
            bool defer_duty =
                tracking_ && eff != 0.0 && std::abs(eff) < slow_mode_floor_rate_locked(kAxisRa) * kSlowModeFloorPad;
            if (defer_duty) {
                // Pre-arm the duty rate (no hardware touch) so the worker
                // started below takes over once the axes are free; the busy
                // operation's restore re-derives the same regime.
                ra_duty_rate_deg_s_ = eff;
                lock.unlock();
                start_duty_thread();
            }
            return;
        }
        double previous = effective_ra_rate_locked();
        ra_rate_sec_per_sidereal_sec_ = rate;
        anchor_model_locked();  // continuous anchor: no position jump on a rate change
        bool need_duty = false;
        if (tracking_) {
            apply_ra_tracking_rate_locked(lock, previous);
            need_duty = ra_duty_rate_deg_s_ != 0.0;
        }
        if (need_duty) {
            lock.unlock();
            start_duty_thread();
        }
    }

    int get_side_of_pier() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_position_cache_locked(false);
        // ASCOM convention derived from the dec-axis branch: the side the goto
        // picks for HA >= 0 targets is pierEast (0), the mirror side
        // pierWest (1). The goto picks the branch with `k * branch` equal to
        // the sign of the hour angle (see ra_dec_to_axis_degrees_locked), so
        // reading `k * branch` back off the axis reproduces the side that
        // get_destination_side_of_pier() computes from hour angle. `k` is +1
        // on every board without a measured sense (open-astro#458). At the
        // pole the axis cannot say which branch it is on; the remembered one
        // answers (open-astro#459).
        return home_term_sign_locked() * branch_from_axis_locked(cached_dec_axis_deg_) > 0 ? 0 : 1;
    }

    void set_side_of_pier(int side) override {
        (void)side;
        throw AlpacaException("Pier side not supported", AlpacaError::PropertyNotImplemented);
    }

    int get_destination_side_of_pier(double ra, double dec) const override {
        (void)dec;
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        double lst = compute_local_sidereal_time_hours(utc_now_locked(), site_longitude_);
        double ha = wrap_hour_angle(lst - ra);
        return ha >= 0.0 ? 0 : 1;
    }

    EquatorialSystem get_equatorial_system() const override {
        // Coordinates are derived from local sidereal time — topocentric
        // apparent (JNow), not J2000.
        return EquatorialSystem::Topocentric;
    }

    bool get_does_refraction() const override { return does_refraction_; }
    void set_does_refraction(bool does_refraction) override { does_refraction_ = does_refraction; }

    int get_slew_settle_time() const override { return slew_settle_time_seconds_; }

    void set_slew_settle_time(int seconds) override {
        if (seconds < 0) {
            throw AlpacaException("Slew settle time must be >= 0 seconds", AlpacaError::InvalidValue);
        }
        slew_settle_time_seconds_ = seconds;
    }

    double get_sidereal_time() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return compute_local_sidereal_time_hours(utc_now_locked(), site_longitude_);
    }

    double get_site_elevation() const override { return site_elevation_m_; }

    void set_site_elevation(double elevation) override {
        if (!std::isfinite(elevation) || elevation < -300.0 || elevation > 10000.0) {
            throw AlpacaException("SiteElevation must be in range -300 to 10000 meters", AlpacaError::InvalidValue);
        }
        site_elevation_m_ = elevation;
    }

    double get_site_latitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return site_latitude_;
    }

    void set_site_latitude(double latitude) override {
        if (!std::isfinite(latitude) || latitude < -90.0 || latitude > 90.0) {
            throw AlpacaException("SiteLatitude must be in range -90 to 90 degrees", AlpacaError::InvalidValue);
        }
        // A latitude write that crosses the equator reverses the RA drive and
        // flips the Dec offset's sign (effective_ra_rate_locked() and
        // apply_dec_rate_offset_locked() both read hemisphere_south_locked()),
        // so a drive applied under the old latitude is now running backwards.
        // The client pushing its own site after connect is the ordinary way in,
        // and nothing else re-applies until Tracking, TrackingRate,
        // RightAscensionRate or a slew happens to: the axis holds the wrong
        // direction at 1x and the star trails at 2x, which is the #250
        // signature. Re-apply here.
        //
        // The skip is decided PER AXIS (axis_busy_locked()), not from the
        // whole-mount axes_busy_locked(): only an operation that owns a given
        // axis re-derives that axis's drive when it releases it. A goto/park/
        // home/slew owns both and restores both via set_tracking_locked() ->
        // apply_ra_drive_locked(); a pulse or a manual MoveAxis owns only its
        // own, and its restore touches only that one -- the pulse end's
        // stop_axis() re-derives RA through effective_ra_rate_locked() (it
        // used to write the rate captured at dispatch, which made even the
        // RA skip unsafe while autoguiding: PHD2 kept the old whole-mount
        // pulse_guiding_active_ flag (then shared by both axes) true for
        // most of every guide cycle), and the MoveAxis stop task
        // re-applies the Dec offset for a Dec nudge. So a whole-mount skip
        // let a DEC-axis operation in flight (a North/South pulse, or
        // MoveAxis(Dec, r) -> MoveAxis(Dec, 0)) block the RA re-apply while
        // nothing on the Dec side ever touched RA: the RA axis kept the old
        // hemisphere's direction indefinitely -- the same 2x-trailing
        // signature this setter exists to prevent, reached through the other
        // axis (round-2 review finding). The mirror case (an RA-axis
        // operation blocking the Dec offset flip) is closed the same way.
        bool crossing = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            crossing = connected_ && tracking_ && hemisphere_south_locked() != (latitude < 0.0) &&
                       !(axis_busy_locked(kAxisRa) && axis_busy_locked(kAxisDec));
            if (!crossing) {
                site_latitude_ = latitude;
                site_latitude_set_ = true;
                return;
            }
        }
        reap_duty_task();
        bool need_duty = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Re-checked under the new lock: another thread may have stopped
            // tracking or taken the axes while the mutex was released.
            const bool ra_busy = axis_busy_locked(kAxisRa);
            const bool dec_busy = axis_busy_locked(kAxisDec);
            const bool still_crossing =
                connected_ && tracking_ && hemisphere_south_locked() != (latitude < 0.0) && !(ra_busy && dec_busy);
            const double previous = effective_ra_rate_locked();
            site_latitude_ = latitude;
            site_latitude_set_ = true;
            if (still_crossing) {
                anchor_model_locked();  // the counts are unchanged; only their reading flips
                if (!ra_busy) {
                    apply_ra_tracking_rate_locked(lock, previous);
                } else if (ra_duty_rate_deg_s_ != 0.0) {
                    // Sub-floor RA drive: the axis is stopped between bursts,
                    // so the owner's restore only has to stop it -- the duty
                    // worker resumes from this stored rate, which no restore
                    // path re-derives. Pre-arm it (no hardware touch) the way
                    // set_right_ascension_rate()'s busy branch does.
                    ra_duty_rate_deg_s_ = effective_ra_rate_locked();
                }
                if (!dec_busy) {
                    apply_dec_rate_offset_locked(lock);
                }
            }
            // Unconditional, exactly as set_declination_rate() computes it:
            // reap_duty_task() above JOINED the worker, so a still_crossing
            // that went false in the window (a goto or park taking both axes
            // while the mutex was released) would otherwise leave a live
            // sub-floor rate with no worker until the next rate write or
            // tracking toggle (round-2 review note).
            need_duty = ra_duty_rate_deg_s_ != 0.0 || dec_duty_rate_deg_s_ != 0.0;
        }
        if (need_duty) {
            start_duty_thread();
        }
    }

    double get_site_longitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return site_longitude_;
    }

    void set_site_longitude(double longitude) override {
        if (!std::isfinite(longitude) || longitude < -180.0 || longitude > 180.0) {
            throw AlpacaException("SiteLongitude must be in range -180 to 180 degrees", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        site_longitude_ = longitude;
        site_longitude_set_ = true;
    }

    bool get_slewing() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        // open-astro#575: surface a stored async-slew failure as an error
        // instead of a silent false, until AbortSlew or a new slew initiator
        // clears it (see last_slew_error_).
        if (!last_slew_error_.empty()) {
            throw AlpacaException(last_slew_error_);
        }
        return get_slewing_locked();
    }

    // open-astro#391: the target pair is written under mutex_ by the slew,
    // sync and reset paths, so the accessors take it too (no I/O behind
    // them), and the getters answer NotConnected before ValueNotSet like
    // every other property here.
    double get_target_declination() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (!target_dec_set_) {
            throw AlpacaException("Target declination has not been set", AlpacaError::ValueNotSet);
        }
        return target_dec_degrees_;
    }

    void set_target_declination(double dec) override {
        if (!std::isfinite(dec) || dec < -90.0 || dec > 90.0) {
            throw AlpacaException("TargetDeclination must be in range -90 to 90 degrees", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        target_dec_degrees_ = dec;
        target_dec_set_ = true;
    }

    double get_target_right_ascension() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (!target_ra_set_) {
            throw AlpacaException("Target right ascension has not been set", AlpacaError::ValueNotSet);
        }
        return target_ra_hours_;
    }

    void set_target_right_ascension(double ra) override {
        if (!std::isfinite(ra) || ra < 0.0 || ra >= 24.0) {
            throw AlpacaException("TargetRightAscension must be in range 0 to <24 hours", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        target_ra_hours_ = ra;
        target_ra_set_ = true;
    }

    int get_tracking_rate() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return tracking_rate_;
    }

    void set_tracking_rate(int rate) override {
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        if (rate < 0 || rate > 2) {
            throw AlpacaException("Unsupported tracking rate", AlpacaError::InvalidValue);
        }
        double previous = effective_ra_rate_locked();
        tracking_rate_ = rate;
        // ASCOM contract: changing the drive rate zeroes the rate offsets.
        ra_rate_sec_per_sidereal_sec_ = 0.0;
        dec_rate_arcsec_per_sec_ = 0.0;
        ra_duty_rate_deg_s_ = 0.0;
        dec_duty_rate_deg_s_ = 0.0;
        if (tracking_) {
            apply_ra_tracking_rate_locked(lock, previous);
            apply_dec_rate_offset_locked(lock);
        }
    }

    std::vector<int> get_tracking_rates() const override { return {0, 1, 2}; }

    std::chrono::system_clock::time_point get_utc_date() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        // The motor controller has no clock; the host clock plus the
        // client-set offset is what the client asked to read back, whatever
        // the pointing math uses (see client_utc_now_locked()).
        return client_utc_now_locked();
    }

    void set_utc_date(std::chrono::system_clock::time_point utc) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        utc_offset_ = utc - std::chrono::system_clock::now();
        utc_anchor_system_ = std::chrono::system_clock::now();
        utc_anchor_steady_ = std::chrono::steady_clock::now();
        has_utc_offset_ = true;
        // open-astro#301: sampled here at the write; while the host was
        // undisciplined, resample_host_discipline_locked() re-checks it at
        // most once per interval on the pointing path (open-astro#405). A
        // host the kernel reports as disciplined has a better clock than the
        // client does, and the router has already refused to step it.
        utc_offset_host_was_synchronized_ = detail::host_synchronized_probe();
        next_discipline_resample_ = std::chrono::steady_clock::now() + detail::host_discipline_resample_interval();
        // open-astro#400: once per connection. The disagreement is a
        // configuration fact, not an event, and a client that re-writes
        // UTCDate on a poll interval would otherwise repeat the same line for
        // the whole session; reset_runtime_state_locked() re-arms it.
        if (utc_offset_host_was_synchronized_ && !client_disagreement_warned_ &&
            (utc_offset_ > alpacacore::util::HostClock::kClientDisagreementWarn ||
             utc_offset_ < -alpacacore::util::HostClock::kClientDisagreementWarn)) {
            client_disagreement_warned_ = true;
            ALPACA_LOG_WARN(
                "SkyWatcher",
                "Client UTCDate disagrees with an NTP-disciplined host clock by " +
                    std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(utc_offset_).count()) +
                    " ms; honouring it for the UTCDate readback but pointing by the host clock (logged once per "
                    "connection)");
        }
        // Unconditional on purpose. The cache holds only the axis angles;
        // compute_ra_dec_locked() recomputes LST from utc_now_locked() on
        // every read, so reported RA follows a UTCDate write with or without
        // this call. It stays because it is cheap (one refetch of the counts)
        // and keeps the first read after a time change on fresh hardware
        // state, on either #301 branch.
        invalidate_position_cache_locked();
    }

    // FindHome is an asynchronous initiator (ITelescopeV4). Boards without a
    // home index (has_home_indexer_ false) treat the controller's power-on
    // index (counts 0x800000, counterweight down pointing at the pole) as the
    // home position, so homing is a goto to axis angles 0,0. AtHome flips true (and Slewing false) in
    // the same locked step when the goto lands.
    void find_home() override {
        reap_slew_task();
        reap_pulse_task();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("FindHome");
            if (homing_) {
                return;  // already homing
            }
            if (at_home_ && !get_hardware_slewing_locked()) {
                return;  // already at home
            }
            invalidate_position_cache_locked();
            slewing_cached_ = true;
            slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            restore_tracking_after_slew_ = false;
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            homing_ = true;
            // open-astro#575: a fresh initiator is a clean start -- a client
            // that calls FindHome after a failed GOTO must not be told the
            // OLD goto failed while it's homing.
            clear_last_slew_error_locked();
        }

        // Join any task that raced in between the reap above and this lock,
        // WITHOUT task_mutex_ held: the task's task_wait_for() must acquire it
        // to observe the cancel and exit, so joining under the lock deadlocks.
        std::unique_lock<std::mutex> tlock(task_mutex_);
        while (slew_task_thread_.joinable()) {
            std::thread stale = std::move(slew_task_thread_);
            tlock.unlock();
            slew_task_cancel_.store(true);
            task_cv_.notify_all();
            stale.join();
            slew_task_cancel_.store(false);
            tlock.lock();
        }
        slew_task_thread_ = std::thread([this]() {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!connected_ || slew_task_cancel_.load()) {
                homing_ = false;
                return;
            }
            try {
                if (has_home_indexer_) {
                    // True AutoHome: hunt the home index sensors and re-anchor
                    // the count frame to the physical home mark.
                    run_autohome(lock);
                    set_tracking_locked(lock, false);
                    refresh_position_cache_locked(true);
                    at_home_ = true;
                } else {
                    // No sensors: goto the power-on index (counts kHomeCounts).
                    dispatch_goto_locked(lock, 0.0, 0.0);
                    wait_for_slew_complete(lock);
                    set_tracking_locked(lock, false);
                    // Verify we actually landed at home (an AbortSlew mid-home
                    // stops the axes wherever they are) before claiming AtHome.
                    refresh_position_cache_locked(true);
                    at_home_ = std::abs(cached_ra_axis_deg_) < 0.1 && std::abs(cached_dec_axis_deg_) < 0.1;
                }
                homing_ = false;
            } catch (const std::exception& ex) {
                homing_ = false;
                slewing_cached_ = false;
                slew_force_until_ = std::chrono::steady_clock::time_point::min();
                stop_axes_if_cancelled_locked();
                ALPACA_LOG_WARN("SkyWatcher", std::string("FindHome failed: ") + ex.what());
            } catch (...) {
                homing_ = false;
                slewing_cached_ = false;
                slew_force_until_ = std::chrono::steady_clock::time_point::min();
                stop_axes_if_cancelled_locked();
                ALPACA_LOG_WARN("SkyWatcher", "FindHome failed with unknown exception");
            }
        });
    }

    // Park is an asynchronous initiator (ITelescopeV4): dispatch the park slew
    // in the background and return inside the STANDARD response target; AtPark
    // turns true when the slew completes and tracking is stopped.
    void park() override {
        // Serialize against other initiators (SlewToCoordinatesAsync): the
        // check-then-reap-then-spawn sequence must not interleave with
        // another initiator's, or a park could be cancelled and restarted
        // (or a slew could clobber a park) in the gap.
        std::lock_guard<std::mutex> ilock(initiator_mutex_);
        {
            // Check BEFORE reaping: reap_slew_task() would cancel a park in
            // flight (its task clears parking_), so a second Park would
            // restart the slew instead of being the documented no-op.
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            if (parked_ || parking_) {
                return;  // calling Park twice (or while parking) is harmless
            }
        }
        reap_slew_task();
        reap_pulse_task();
        double target_ra_axis = 0.0;
        double target_dec_axis = 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            if (parked_ || parking_) {
                return;  // calling Park twice is harmless
            }
            if (!park_position_set_) {
                // Default park = the mount's home position (pole, weights down).
                park_ra_axis_deg_ = 0.0;
                park_dec_axis_deg_ = 0.0;
                park_position_set_ = true;
            }
            target_ra_axis = park_ra_axis_deg_;
            target_dec_axis = park_dec_axis_deg_;
            invalidate_position_cache_locked();
            slewing_cached_ = true;
            slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            restore_tracking_after_slew_ = false;
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            parking_ = true;
            // open-astro#575: a fresh initiator is a clean start -- a client
            // that calls Park after a failed GOTO must not be told the OLD
            // goto failed while it's parking.
            clear_last_slew_error_locked();
        }

        // Join any task that raced in between the reap above and this lock,
        // WITHOUT task_mutex_ held: the task's task_wait_for() must acquire it
        // to observe the cancel and exit, so joining under the lock deadlocks.
        std::unique_lock<std::mutex> tlock(task_mutex_);
        while (slew_task_thread_.joinable()) {
            std::thread stale = std::move(slew_task_thread_);
            tlock.unlock();
            slew_task_cancel_.store(true);
            task_cv_.notify_all();
            stale.join();
            slew_task_cancel_.store(false);
            tlock.lock();
        }
        slew_task_thread_ = std::thread([this, target_ra_axis, target_dec_axis]() {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!connected_ || slew_task_cancel_.load()) {
                parking_ = false;
                return;
            }
            try {
                dispatch_goto_locked(lock, target_ra_axis, target_dec_axis);
                wait_for_slew_complete(lock);
                set_tracking_locked(lock, false);
                // AtPark and Slewing flip in the same locked step: no window
                // where a poller can see Slewing false with AtPark false.
                parked_ = true;
                parking_ = false;
                at_home_ = target_ra_axis == 0.0 && target_dec_axis == 0.0;
            } catch (const std::exception& ex) {
                parking_ = false;
                slewing_cached_ = false;
                slew_force_until_ = std::chrono::steady_clock::time_point::min();
                stop_axes_if_cancelled_locked();
                ALPACA_LOG_WARN("SkyWatcher", std::string("Park failed: ") + ex.what());
            } catch (...) {
                parking_ = false;
                slewing_cached_ = false;
                slew_force_until_ = std::chrono::steady_clock::time_point::min();
                stop_axes_if_cancelled_locked();
                ALPACA_LOG_WARN("SkyWatcher", "Park failed with unknown exception");
            }
        });
    }

    void pulse_guide(int direction, int duration) override {
        // open-astro#620: serialize against the other async initiators
        // (Park, SlewToCoordinatesAsync) so PulseGuide's own
        // check -> reap -> spawn sequence cannot interleave with theirs.
        // Taken BEFORE mutex_ (precedent: park(), slew_to_coordinates_async()) --
        // never taken while mutex_ is held (see initiator_mutex_'s comment).
        std::lock_guard<std::mutex> ilock(initiator_mutex_);
        int axis = -1;
        bool ra_rate_adjust = false;
        bool ra_pulse_restart = false;  // pulse opposes the tracking sense
        double dec_rate_deg_per_sec = 0.0;
        double ra_pulse_rate_deg_per_sec = 0.0;
        double ra_restore_rate_deg_per_sec = kSiderealDegPerSec;
        std::uint64_t my_seq = 0;  // this pulse's pulse_seq_[axis-1], taken under the dispatch lock
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("PulseGuide");
            if (duration < 0) {
                throw AlpacaException("PulseGuide duration must be >= 0", AlpacaError::InvalidValue);
            }
            if (direction < 0 || direction > 3) {
                throw AlpacaException("Invalid PulseGuide direction", AlpacaError::InvalidValue);
            }

            const auto now = std::chrono::steady_clock::now();

            // Reads are live (dead-reckoned) during pulses — no frozen-target
            // accumulation, and pulses never rewrite the slew Target
            // properties. The pier-side sign for Dec needs a fresh position.
            refresh_position_cache_locked(false);

            if (direction == 0 || direction == 1) {
                // North/South: DEC axis speed-mode nudge. Freeze the RA value —
                // the RA axis keeps tracking, so only DEC accumulates.
                axis = kAxisDec;
                dec_rate_deg_per_sec = direction == 0 ? guide_rate_.dec : -guide_rate_.dec;
                // dec = 90 - a2 on the east-pointing branch (a2 >= 0): guide
                // North (+Dec) is NEGATIVE axis motion there (same sign rule
                // as the DeclinationRate offset, confirmed by ConformU 4.5
                // measured-rate tests) -- in the NORTHERN hemisphere. See the
                // XOR derivation in apply_dec_rate_offset_locked()'s comment;
                // this call site carried the identical bug, and an autoguider
                // below the equator would have pushed every Dec correction the
                // wrong way.
                if ((branch_from_axis_locked(cached_dec_axis_deg_) > 0) != hemisphere_south_locked()) {
                    dec_rate_deg_per_sec = -dec_rate_deg_per_sec;
                }
            } else {
                // East/West: adjust the RA tracking rate for the pulse window
                // (slow speed mode allows a live step-period change). East
                // slows apparent RA drive, west speeds it.
                axis = kAxisRa;
                // In the duty regime the RA axis is stopped between bursts -
                // a live step-period change would not move it. Use the direct
                // nudge path; the worker resumes bursting after the pulse.
                ra_rate_adjust = tracking_ && ra_duty_rate_deg_s_ == 0.0;
                // Sky sense (East = RA increasing = slower in the tracking
                // direction) mapped onto the hemisphere's axis sense, like
                // effective_ra_rate_locked().
                const double axis_sign = ra_axis_sign_locked();
                double adjust = axis_sign * (direction == 2 ? -guide_rate_.ra : guide_rate_.ra);
                ra_restore_rate_deg_per_sec = effective_ra_rate_locked();
                ra_pulse_rate_deg_per_sec = ra_restore_rate_deg_per_sec + adjust;
                // In-place ":I" cannot change direction. A pulse rate that is
                // not (comfortably) in the tracking direction -- guide rate
                // near 1x sidereal under Lunar/Solar drive -- needs a full
                // stop-and-reverse, and a full restart to resume.
                ra_pulse_restart = axis_sign * ra_pulse_rate_deg_per_sec <= kMinInPlacePulseRateDegPerSec;
            }

            // No read freeze during the pulse: ConformU 4.5 measures the
            // physical pulse displacement (Dec moved, RA unchanged), and the
            // dead-reckoned live reads report exactly that.

            const int ai = axis - 1;  // 0=RA, 1=Dec (open-astro#620)
            pulse_axis_active_[ai] = true;
            pulse_axis_in_motion_[ai] = true;
            my_seq = ++pulse_seq_[ai];
            pulse_guide_end_time_[ai] = now + std::chrono::milliseconds(duration) + kPulseGuideCompletionDelay;
            invalidate_position_cache_locked();
        }

        const int ai = axis - 1;  // 0=RA, 1=Dec (open-astro#620)
        // Stop-the-pulse timer thread — joinable member thread, never detached.
        // Reaps only THIS axis's task: a Dec pulse must not cancel a running
        // RA pulse's task (open-astro#620), unlike goto/park/home/abort/
        // MoveAxis/disconnect, which own and re-command both axes.
        reap_pulse_task(axis);
        // Join any task that raced in between the reap above and this lock,
        // WITHOUT task_mutex_ held: the task's task_wait_for() must acquire it
        // to observe the cancel and exit, so joining under the lock deadlocks.
        std::unique_lock<std::mutex> tlock(task_mutex_);
        while (pulse_task_thread_[ai].joinable()) {
            std::thread stale = std::move(pulse_task_thread_[ai]);
            tlock.unlock();
            pulse_task_cancel_[ai].store(true);
            task_cv_.notify_all();
            stale.join();
            pulse_task_cancel_[ai].store(false);
            tlock.lock();
        }
        const bool restore_tracking = ra_rate_adjust;
        const double dec_rate = dec_rate_deg_per_sec;
        const double ra_pulse_rate = ra_pulse_rate_deg_per_sec;
        const bool pulse_restart = ra_pulse_restart;
        pulse_task_thread_[ai] = std::thread([this, axis, ai, duration, restore_tracking, direction, dec_rate,
                                              ra_pulse_rate, ra_restore_rate_deg_per_sec, pulse_restart, my_seq]() {
            // open-astro#559: a task clears pulse state only while its pulse
            // is still the current one. A superseding pulse sets both flags
            // before it reaps this task, and this task's exit used to clear
            // them from under it.
            auto end_pulse_locked = [this, ai, my_seq]() {
                if (pulse_seq_[ai] == my_seq) {
                    pulse_axis_active_[ai] = false;
                    pulse_axis_in_motion_[ai] = false;
                }
            };
            // PulseGuide is an asynchronous initiator (ITelescopeV4): the axis
            // dispatch (which can stop-and-wait a ramping axis, plus UDP
            // retries) runs here so pulse_guide() returns inside the STANDARD
            // response target. IsPulseGuiding is already true.
            bool verify_dispatch_rate = false;
            // Set once the live ":I" at the pulse rate has gone out on a
            // tracking axis. From then on a dispatch failure (the ":J"
            // re-latch below throwing, say) leaves the axis running at the
            // pulse rate with nothing scheduled to bring it back: before the
            // ":J" kick a dispatch failure left the axis at its prior, safe
            // drive rate. Give the drive-rate restore the same retried care
            // as the end-of-pulse stop (#249 review).
            bool ra_live_write_sent = false;
            // Same re-derivation as stop_axis() below, for the same reason:
            // this runs after the pulse, so a SiteLatitude write during it
            // must be honoured rather than the dispatch-time snapshot.
            auto recover_ra_drive_rate = [this, &ra_live_write_sent]() {
                if (!ra_live_write_sent) {
                    return;
                }
                // Deliberately NOT given stop_axis()'s ra_reverses branch: it
                // writes ":I" + ":J" in place with no ":G", so a re-derived
                // rate whose SIGN differs from the running one changes the
                // period and leaves the axis turning the old way. Reached only
                // when the dispatch ":J" throws after the ":I" went out, and a
                // pure hemisphere flip preserves the magnitude, so the practical
                // exposure is a period that is already correct. Recorded so the
                // asymmetry with stop_axis() reads as a choice rather than an
                // oversight (round-4 review note).
                constexpr int kRestoreAttempts = 3;
                std::string last_error;
                for (int attempt = 0; attempt < kRestoreAttempts; ++attempt) {
                    try {
                        std::lock_guard<std::mutex> lock(mutex_);
                        const double restore_rate = effective_ra_rate_locked();
                        auto& proto = *protocol_;
                        proto.set_step_period(kAxisRa, tracking_step_period_for(restore_rate),
                                              /*with_readback=*/false);
                        proto.start_motion(kAxisRa);
                        cmd_axis_rate_deg_s_[0] = restore_rate;
                        return;
                    } catch (const std::exception& e) {
                        last_error = e.what();
                    } catch (...) {
                        last_error = "unknown error";
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                ALPACA_LOG_ERROR("SkyWatcher",
                                 "PulseGuide dispatch failed AFTER the RA pulse rate was written and "
                                 "the drive-rate restore failed " +
                                     std::to_string(kRestoreAttempts) +
                                     " times: RA may be running at the guide rate: " + last_error);
            };
            try {
                std::unique_lock<std::mutex> lock(mutex_);
                // An in-place RA pulse never goes through a stop-wait: reap a
                // pending RightAscensionRate check here so it cannot sample
                // the pulse rate and "restore" the tracking rate mid-pulse.
                // RA only: a Dec pulse never touches the RA axis, and reaping
                // here would cancel a pending check with nothing to replace
                // it -- a Dec correction landing inside the check's window
                // (routine while autoguiding) would silently drop the one
                // chance to catch a stalled ":I" (#258 review).
                if (axis == kAxisRa) {
                    reap_rate_verify_task();
                }
                auto& proto = *protocol_;
                if (axis == kAxisDec) {
                    start_speed_motion_locked(lock, kAxisDec, dec_rate);
                } else if (restore_tracking && !pulse_restart) {
                    // No ":i" readback here: the axis is already running at
                    // the pulse rate once ":I" is acknowledged, and the pulse
                    // timer only starts after this returns, so an extra
                    // round-trip would lengthen every pulse (#245 review).
                    ra_live_write_sent = true;
                    proto.set_step_period(kAxisRa, tracking_step_period_for(ra_pulse_rate), /*with_readback=*/false);
                    // A bare ":I" on an already-running axis is sometimes
                    // accepted (":i" readback matches) but never applied to
                    // the spinning motor -- ConformU: "PulseGuide East ...
                    // RA change 0.00, expected 2.51s", count-sampled at
                    // exactly sidereal through the whole pulse (2026-09-06).
                    // INDI's skywatcherAPI.cpp always follows an in-place
                    // SetClockTicksPerMicrostep with StartAxisMotion even
                    // when the axis never stopped; do the same, and verify
                    // below in case that alone is not sufficient.
                    proto.start_motion(kAxisRa);
                    cmd_axis_rate_deg_s_[0] = ra_pulse_rate;
                    // Only sample-verify when the pulse is long enough to
                    // absorb the sample window. A real autoguider sends
                    // 50-500 ms pulses; there the ~450 ms check would BE the
                    // pulse (the deduction below can only clamp at zero, not
                    // give the time back), so short pulses rely on the ":J"
                    // kick alone. ConformU's 5 s pulses are always verified.
                    verify_dispatch_rate = duration >= kMinPulseForRateVerifyMs;
                } else if (restore_tracking) {
                    // The pulse runs against the axis's own tracking sense
                    // (the guard is on axis_sign * rate, so this is a rate <= 0
                    // north of the equator and >= 0 south of it): a live ":I"
                    // write cannot reverse the axis — stop and restart in the
                    // pulse direction instead.
                    start_speed_motion_locked(lock, kAxisRa, ra_pulse_rate);
                } else {
                    // Not tracking: nudge the RA axis directly like DEC, in
                    // the hemisphere's axis sense for East/West.
                    double rate = ra_axis_sign_locked() * (direction == 2 ? -guide_rate_.ra : guide_rate_.ra);
                    start_speed_motion_locked(lock, kAxisRa, rate);
                }
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("SkyWatcher", std::string("PulseGuide dispatch failed: ") + e.what());
                recover_ra_drive_rate();
                std::lock_guard<std::mutex> lock(mutex_);
                end_pulse_locked();
                return;
            } catch (...) {
                ALPACA_LOG_WARN("SkyWatcher", "PulseGuide dispatch failed with unknown exception");
                recover_ra_drive_rate();
                std::lock_guard<std::mutex> lock(mutex_);
                end_pulse_locked();
                return;
            }
            // Time spent verifying counts as pulse time: on this path the axis
            // is ALREADY running at the pulse rate before the check starts, so
            // the hold below must be shortened by however long it took.
            std::chrono::steady_clock::duration verify_elapsed{};
            if (verify_dispatch_rate) {
                // Runs unlocked (samples position across a short window):
                // never hold mutex_ across a sleep -- see stop_axis_and_wait_locked.
                const auto verify_start = std::chrono::steady_clock::now();
                // Bounded by the pulse's own duration budget, not the full
                // kRateVerifyMaxWindow: this check samples the axis WHILE it
                // is already running at the pulse rate, and its cost below
                // can only shrink the remaining hold, never extend it -- so
                // a window bigger than the pulse itself would let the axis
                // overshoot its commanded on-time (round-1 review finding).
                const auto dispatch_max_window = std::chrono::milliseconds(duration) > kRateVerifySettle
                                                     ? std::chrono::milliseconds(duration) - kRateVerifySettle
                                                     : std::chrono::milliseconds(0);
                verify_live_rate_or_rekick(kAxisRa, ra_restore_rate_deg_per_sec, ra_pulse_rate, pulse_task_cancel_[ai],
                                           dispatch_max_window);
                verify_elapsed = std::chrono::steady_clock::now() - verify_start;
            }
            // What stop_axis() actually restored, for the post-stop verify
            // below. Seeded with the dispatch-time capture so a stop that
            // never ran (or a non-restoring pulse) behaves as before.
            double applied_ra_restore_rate = ra_restore_rate_deg_per_sec;
            auto stop_axis = [this, axis, restore_tracking, pulse_restart, &applied_ra_restore_rate]() {
                auto& proto = *protocol_;
                // Re-derived here, NOT the value captured at dispatch: since
                // the drive direction became hemisphere-dependent, a
                // SiteLatitude write that crosses the equator during the
                // pulse changes what "restore tracking" means. The setter
                // skips a busy RA axis precisely because this path recomputes;
                // writing the captured pre-write rate would restore the old
                // hemisphere's direction and leave RA running backwards until
                // something else re-applied the drive -- the 2x-trailing
                // failure the setter exists to prevent, reached through the
                // one path that was still using a stale snapshot.
                double ra_restore_rate_deg_per_sec = 0.0;
                bool ra_reverses = false;
                if (restore_tracking) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ra_restore_rate_deg_per_sec = effective_ra_rate_locked();
                    applied_ra_restore_rate = ra_restore_rate_deg_per_sec;
                    // An in-place ":I" changes the PERIOD only; direction is
                    // latched by the ":G" that start_speed_motion_locked()
                    // sends. So a restore whose sign no longer matches what
                    // the axis is running has to take the stop-and-restart
                    // branch, exactly as set_site_latitude() does for the
                    // idle case -- otherwise the axis keeps turning the old
                    // way at the new rate.
                    ra_reverses = (ra_restore_rate_deg_per_sec > 0.0) != (cmd_axis_rate_deg_s_[0] > 0.0);
                }
                if (restore_tracking && !pulse_restart && !ra_reverses) {
                    // RA pulse over a live tracking axis: restore the drive
                    // step period; the axis never stopped. Same ":J" kick as
                    // the dispatch above, for the same reason.
                    proto.set_step_period(kAxisRa, tracking_step_period_for(ra_restore_rate_deg_per_sec),
                                          /*with_readback=*/false);
                    proto.start_motion(kAxisRa);
                    std::lock_guard<std::mutex> lock(mutex_);
                    cmd_axis_rate_deg_s_[0] = ra_restore_rate_deg_per_sec;
                } else if (restore_tracking) {
                    // Reversed pulse, or a hemisphere change mid-pulse: full
                    // stop-and-restart back to the drive rate.
                    std::unique_lock<std::mutex> lock(mutex_);
                    start_speed_motion_locked(lock, kAxisRa, ra_restore_rate_deg_per_sec);
                } else {
                    proto.stop_motion(axis);
                    // Zero the dead-reckoning rate: this direct stop bypasses
                    // stop_axis_and_wait_locked, and a stale rate kept the
                    // reads drifting after the pulse ended (ConformU 4.5 pulse
                    // displacement tests read phantom motion).
                    std::unique_lock<std::mutex> lock(mutex_);
                    cmd_axis_rate_deg_s_[axis - 1] = 0.0;
                    invalidate_position_cache_locked();
                    // A Dec pulse pre-empted any DeclinationRate offset
                    // motion: re-apply it so guiding corrections don't
                    // silently cancel comet/satellite tracking.
                    if (axis == kAxisDec && tracking_ && dec_rate_arcsec_per_sec_ != 0.0) {
                        try {
                            apply_dec_rate_offset_locked(lock);
                        } catch (const std::exception& e) {
                            ALPACA_LOG_WARN("SkyWatcher",
                                            std::string("Pulse end: failed to restore Dec rate offset: ") + e.what());
                        }
                    }
                }
            };
            // Hold for the REMAINDER of the requested duration: the verify
            // window above already ran with the axis at the pulse rate.
            // Found on real hardware (EQM-35 Pro, 2026-09-07): counting it
            // twice overshot a 5 s ConformU pulse by ~9% (RA change 2.74s vs
            // 2.51s expected). Dispatch time on the OTHER paths is not
            // deducted -- there the axis only starts moving once dispatch
            // finishes, so the full duration still applies.
            const auto remaining = std::chrono::milliseconds(duration) > verify_elapsed
                                       ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::milliseconds(duration) - verify_elapsed)
                                       : std::chrono::milliseconds(0);
            if (!task_wait_for(remaining, pulse_task_cancel_[ai])) {
                // Cancelled by a reaper (a new pulse/slew/park/home/moveaxis/
                // abort/disconnect). DO NOT touch the hardware here: the
                // reaper stops or re-commands the axes itself, and a stop or
                // step-period restore landing mid-goto corrupted the next
                // operation (ConformU: "declination axis did not move" on the
                // first pulse after a slew).
                std::lock_guard<std::mutex> lock(mutex_);
                end_pulse_locked();
                return;
            }
            // The stop/restore MUST land or the axis runs away at guide rate.
            constexpr int kStopAttempts = 3;
            bool stopped = false;
            std::string last_error;
            for (int attempt = 0; attempt < kStopAttempts && !stopped; ++attempt) {
                try {
                    stop_axis();
                    stopped = true;
                } catch (const std::exception& e) {
                    last_error = e.what();
                } catch (...) {
                    last_error = "unknown error";
                }
                if (!stopped && !task_wait_for(std::chrono::milliseconds(100), pulse_task_cancel_[ai])) {
                    break;
                }
            }
            // Same duration guard as the dispatch check: this one runs after
            // the motion, so it costs no pulse distance, but it does hold the
            // pulse task ~450 ms (or more) longer, which the next command's
            // reap must join. Not worth that latency on short guide pulses.
            if (stopped && restore_tracking && !pulse_restart && duration >= kMinPulseForRateVerifyMs) {
                // What stop_axis() RE-DERIVED, not the dispatch-time capture.
                // The two differ whenever effective_ra_rate_locked() moved
                // during the pulse -- a RightAscensionRate write (deferred by
                // the RA-axis busy skip precisely so this restore applies it)
                // or a SiteLatitude crossing. Checking against the stale value
                // made live_rate_change_took() classify the correctly restored
                // axis as "did not take" and resend ":I" at the pre-write
                // period, silently dropping the client's offset and leaving
                // cmd_axis_rate_deg_s_[0] describing a rate the axis is not
                // running. Narrow (East pulses, offset > ~0.25 s/s) but it is
                // exactly the contract set_right_ascension_rate()'s skip rests
                // on (round-4 review note).
                verify_live_rate_or_rekick(kAxisRa, ra_pulse_rate, applied_ra_restore_rate, pulse_task_cancel_[ai]);
            }
            if (stopped) {
                // open-astro#559: the property and the axis are released
                // together, once the restore (and its verify) is done. If the
                // ownership flag outlived IsPulseGuiding, a client that waited
                // for false and then wrote DeclinationRate/RightAscensionRate
                // took the busy-axis deferral and waited on a restore this
                // task had already run: the write was stranded.
                std::lock_guard<std::mutex> lock(mutex_);
                end_pulse_locked();
            }
            if (!stopped) {
                ALPACA_LOG_ERROR("SkyWatcher", "PulseGuide STOP FAILED after " + std::to_string(kStopAttempts) +
                                                   " attempts on axis " + std::to_string(axis) +
                                                   " — axis may still be moving (mount runaway risk): " + last_error);
                std::lock_guard<std::mutex> lock(mutex_);
                end_pulse_locked();
                if (connected_) {
                    manual_axis_slewing_[axis - 1] = true;
                }
            }
        });
    }

    void set_park() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        refresh_position_cache_locked(true);
        park_ra_axis_deg_ = cached_ra_axis_deg_;
        park_dec_axis_deg_ = cached_dec_axis_deg_;
        park_position_set_ = true;
    }

    void slew_to_coordinates(double ra, double dec) override {
        reap_slew_task();  // also clears a leftover AbortSlew cancellation
        reap_pulse_task();
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("SlewToCoordinates");
        validate_ra_dec(ra, dec, "SlewToCoordinates");
        goto_in_progress_ = true;
        try {
            do_slew_to_ra_dec_locked(lock, ra, dec);
            wait_for_slew_complete(lock);
            refine_goto_landing(lock, ra, dec);
        } catch (...) {
            goto_in_progress_ = false;
            // An abandoned goto never reaches the landing that consumes this
            // stamp, and a Park/FindHome landing within the next 30 s would
            // otherwise fold the abandoned interval into goto_overhead_seconds_.
            last_goto_dispatch_time_ = std::chrono::steady_clock::time_point{};
            throw;
        }
        // Same order as the async task: Slewing stays true until tracking is
        // running again. restore_tracking_after_slew_locked() now releases
        // mutex_ for the post-slew rate check, so clearing the flag first let
        // a client polling Slewing see the slew finish and fire PulseGuide or
        // MoveAxis into exactly the restart window this check exists to
        // protect. Budget for a synchronous SlewToCoordinates: two sample
        // windows of kRateVerifySettle + kPostSlewRateWindow (~450 ms each),
        // and between them a recovery of kAxisStopTimeout (5 s worst case) +
        // kLandingSettleTimeout (2 s) -- about 7.5 s past the landing if
        // every bound is hit, against ~450 ms when the first sample agrees,
        // which is the ordinary case.
        goto_in_progress_ = false;
        restoring_tracking_ = true;
        try {
            restore_tracking_after_slew_locked(lock);
        } catch (...) {
            restoring_tracking_ = false;
            throw;
        }
        restoring_tracking_ = false;
    }

    void slew_to_coordinates_async(double ra, double dec) override {
        std::lock_guard<std::mutex> ilock(initiator_mutex_);  // see park()
        {
            // Gate BEFORE reaping: reap_slew_task() would cancel a park in
            // flight (clearing parking_) and let this slew clobber it.
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("SlewToCoordinatesAsync");
        }
        // Cancel + join any previous slew or pulse task first (without mutex_):
        // a stale pulse timer firing mid-goto corrupts the slew.
        reap_slew_task();
        reap_pulse_task();
        uint64_t slew_epoch = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("SlewToCoordinatesAsync");
            validate_ra_dec(ra, dec, "SlewToCoordinatesAsync");
            invalidate_position_cache_locked();
            slewing_cached_ = true;
            // open-astro#575: a fresh initiator is a clean start -- a client
            // that retries a rejected goto must not be told the OLD goto
            // failed.
            clear_last_slew_error_locked();
            slew_epoch = slew_error_epoch_;
            slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            target_ra_hours_ = ra;
            target_dec_degrees_ = dec;
            target_ra_set_ = true;
            target_dec_set_ = true;
            restore_tracking_after_slew_ = tracking_;
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            parked_ = false;
            at_home_ = false;
        }

        // Join any task that raced in between the reap above and this lock,
        // WITHOUT task_mutex_ held: the task's task_wait_for() must acquire it
        // to observe the cancel and exit, so joining under the lock deadlocks.
        std::unique_lock<std::mutex> tlock(task_mutex_);
        while (slew_task_thread_.joinable()) {
            std::thread stale = std::move(slew_task_thread_);
            tlock.unlock();
            slew_task_cancel_.store(true);
            task_cv_.notify_all();
            stale.join();
            slew_task_cancel_.store(false);
            tlock.lock();
        }
        slew_task_thread_ = std::thread([this, ra, dec, slew_epoch]() {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!connected_ || slew_task_cancel_.load()) {
                return;
            }
            goto_in_progress_ = true;
            try {
                dispatch_predicted_goto_locked(lock, ra, dec);
                // Poll for completion so tracking restarts after the goto —
                // releasing the mutex between polls so GETs stay responsive.
                wait_for_slew_complete(lock);
                refine_goto_landing(lock, ra, dec);
                // Slewing stays true until tracking is running again: a client
                // that fires motion into the restart window (ConformU's pulse
                // test polls Slewing at 500 ms) otherwise races the board.
                goto_in_progress_ = false;
                restoring_tracking_ = true;
                restore_tracking_after_slew_locked(lock);
                restoring_tracking_ = false;
            } catch (const std::exception& ex) {
                goto_in_progress_ = false;
                restoring_tracking_ = false;
                // See the sync path: a cancelled goto (AbortSlew throws
                // "Slew wait cancelled") must not leave its dispatch stamp
                // for the next landing's overhead EMA.
                last_goto_dispatch_time_ = std::chrono::steady_clock::time_point{};
                slewing_cached_ = false;
                slew_force_until_ = std::chrono::steady_clock::time_point::min();
                // open-astro#575: a cancellation (AbortSlew, or a reap by a
                // newer initiator) is not a failure -- the canceller already
                // owns clearing/replacing last_slew_error_. Recording it here
                // would poison the NEXT Slewing read with an artifact of the
                // cancel, not a real fault. The epoch covers a supersession
                // in the dispatch's unlock windows ("Slew superseded before
                // dispatch" after a MoveAxis/AbortSlew already cleared the
                // slot): any newer command that cleared it owns it now.
                if (!slew_task_cancel_.load() && slew_error_epoch_ == slew_epoch) {
                    last_slew_error_ = std::string("SlewToCoordinatesAsync failed: ") + ex.what();
                }
                stop_axes_if_cancelled_locked();
                ALPACA_LOG_WARN("SkyWatcher", std::string("Async slew failed: ") + ex.what());
            } catch (...) {
                goto_in_progress_ = false;
                restoring_tracking_ = false;
                last_goto_dispatch_time_ = std::chrono::steady_clock::time_point{};
                slewing_cached_ = false;
                slew_force_until_ = std::chrono::steady_clock::time_point::min();
                if (!slew_task_cancel_.load() && slew_error_epoch_ == slew_epoch) {
                    last_slew_error_ = "SlewToCoordinatesAsync failed with an unknown error";
                }
                stop_axes_if_cancelled_locked();
                ALPACA_LOG_WARN("SkyWatcher", "Async slew failed with unknown exception");
            }
        });
    }

    // Snapshot of the target pair, taken under mutex_ and released before the
    // motion call, which takes the same lock itself (open-astro#391).
    std::pair<double, double> target_or_throw() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!target_ra_set_ || !target_dec_set_) {
            throw AlpacaException("Target coordinates have not been set", AlpacaError::ValueNotSet);
        }
        return {target_ra_hours_, target_dec_degrees_};
    }

    void slew_to_target() override {
        const auto [ra, dec] = target_or_throw();
        slew_to_coordinates(ra, dec);
    }

    void slew_to_target_async() override {
        const auto [ra, dec] = target_or_throw();
        slew_to_coordinates_async(ra, dec);
    }

    void sync_to_coordinates(double ra, double dec) override {
        reap_pulse_task();
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("SyncToCoordinates");
        validate_ra_dec(ra, dec, "SyncToCoordinates");

        auto& protocol = *protocol_;

        // ":E" requires the motors fully stopped — pause tracking around the
        // position write, then resume. This is the mount's native sync (the
        // controller's own position register moves), never a driver offset.
        //
        // ORDER MATTERS: the axis frame must be computed AFTER the axis has
        // stopped, aimed at the moment tracking RESUMES — the sky keeps
        // moving while the counts are frozen, and a frame computed before the
        // stop is stale by the whole pause (stop ramp + writes + restart,
        // ~1-3 s = 15-45 arcsec of RA; seen by ConformU as a constant
        // ~79 arcsec return error together with the old post-sync freeze).
        // open-astro#404: published before the hardware write, like the two
        // slew paths, so a sync that fails mid-way still reports the pair the
        // client asked for.
        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_ra_set_ = true;
        target_dec_set_ = true;
        const bool was_tracking = tracking_;
        if (was_tracking) {
            const uint64_t gen = ++motion_generation_;
            if (!stop_axis_and_wait_locked(lock, kAxisRa, gen)) {
                throw AlpacaException("Sync superseded by a concurrent motion command");
            }
        }
        // Aim the frame at the expected restart moment (two ":E" writes plus
        // the tracking start sequence).
        constexpr double kSyncRestartLatencySeconds = 0.25;
        auto [axis1, axis2] = ra_dec_to_axis_degrees_locked(
            ra, dec, was_tracking ? kSyncRestartLatencySeconds * kLstHoursPerSecond : 0.0);
        protocol.set_position(kAxisRa, degrees_to_counts(axis1, axis_params_[0].counts_per_revolution));
        protocol.set_position(kAxisDec, degrees_to_counts(axis2, axis_params_[1].counts_per_revolution));
        // After the writes, for the same reason dispatch_goto_locked() records
        // after its motor commands (open-astro#459).
        remember_command_branch_locked(axis2);
        if (was_tracking) {
            set_tracking_locked(lock, true);
        }
        invalidate_position_cache_locked();
        // No post-sync read freeze: live reads land on the synced frame.
    }

    void sync_to_target() override {
        const auto [ra, dec] = target_or_throw();
        sync_to_coordinates(ra, dec);
    }

    void unpark() override {
        // Cancel an in-flight park first (without mutex_ -- the park task
        // takes it): Unpark during a park must win, not race the task's
        // parked_ = true assignment.
        bool was_parking = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            was_parking = parking_;
        }
        if (was_parking) {
            reap_slew_task();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (was_parking) {
            // The park slew may still be moving the axes; stop them.
            try {
                auto& protocol = *protocol_;
                protocol.stop_motion(kAxisRa);
                protocol.stop_motion(kAxisDec);
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                // Best effort; status polling still reports the true state.
            }
            slewing_cached_ = false;
            slew_force_until_ = std::chrono::steady_clock::time_point::min();
        }
        parking_ = false;
        parked_ = false;
    }

    bool get_can_move_axis(int axis) const override {
        if (axis != 0 && axis != 1 && axis != 2) {
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }
        return axis == 0 || axis == 1;
    }

    void move_axis(int axis, double rate) override {
        reap_pulse_task();
        // Join any previous stop-completion task for THIS axis first, WITHOUT
        // mutex_ held (the task takes mutex_). axis is not yet validated here
        // (the throw for an out-of-range value happens below, under the
        // lock) -- reap_stop_task() itself is a no-op for anything outside
        // {0, 1}, so this never indexes the per-axis slots out of bounds.
        reap_stop_task(axis);
        bool need_stop_task = false;
        uint64_t stop_task_generation = 0;
        int channel = kAxisRa;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("MoveAxis");
            if (axis != 0 && axis != 1) {
                throw AlpacaException("MoveAxis axis must be 0 or 1", AlpacaError::InvalidValue);
            }
            if (std::isnan(rate) || std::isinf(rate)) {
                throw AlpacaException("MoveAxis rate must be finite", AlpacaError::InvalidValue);
            }
            if (std::abs(rate) > kMaxMoveAxisRateDegPerSec) {
                throw AlpacaException("MoveAxis rate exceeds supported range", AlpacaError::InvalidValue);
            }

            channel = axis == 0 ? kAxisRa : kAxisDec;
            constexpr double kStopEpsilon = 1e-9;
            const bool moving = std::abs(rate) > kStopEpsilon;
            if (moving) {
                parked_ = false;
                at_home_ = false;
                // Command the motion BEFORE publishing the Slewing flag: if the
                // transport throws (seen as UDP timeouts over a flaky Wi-Fi
                // link), a pre-set flag is never cleared and Slewing wedges
                // true forever (ConformU Wi-Fi finding).
                start_speed_motion_locked(lock, channel, rate);
                manual_axis_slewing_[axis] = true;
            } else if (manual_axis_slewing_[axis]) {
                // MoveAxis(axis, 0) on a moving axis is an asynchronous
                // initiator: issue the stop and return inside the STANDARD
                // response target. A background task keeps Slewing true until
                // the axis reports fully stopped (the deceleration ramp can
                // exceed 1 s), then restores the previous tracking state per
                // ASCOM.
                protocol_->stop_motion(channel);
                cmd_axis_rate_deg_s_[axis] = 0.0;
                // The stop is a motion command: bump and capture the
                // generation so the background restore-tracking task can tell
                // whether a newer motion command took over while it polled
                // (PR #216 round-5 finding — the restore otherwise re-starts
                // tracking a concurrent SetTracking(false) just stopped).
                stop_task_generation = ++motion_generation_;
                need_stop_task = true;
            }
            // MoveAxis(axis, 0) on an axis with no manual motion is a no-op:
            // Slewing must read false immediately, and tracking (if running on
            // the RA axis) continues untouched per the ASCOM restore-tracking
            // semantics. Raising the flag here and clearing it via the status
            // poll failed ConformU over Wi-Fi, where the poll round-trip
            // outlasted the checker's window.
            invalidate_position_cache_locked();
            // open-astro#575: a fresh initiator is a clean start -- a client
            // that jogs an axis after a failed GOTO must not be told the OLD
            // goto failed.
            clear_last_slew_error_locked();
        }
        if (!need_stop_task) {
            return;
        }

        // Join any task that raced in between the reap above and this lock,
        // WITHOUT task_mutex_ held: the task's task_wait_for() must acquire it
        // to observe the cancel and exit, so joining under the lock deadlocks.
        // Indexed by axis: this must only ever race with (and join) a PRIOR
        // task for the SAME axis, never the other axis's in-flight stop.
        std::unique_lock<std::mutex> tlock(task_mutex_);
        while (stop_task_thread_[axis].joinable()) {
            std::thread stale = std::move(stop_task_thread_[axis]);
            tlock.unlock();
            stop_task_cancel_[axis].store(true);
            task_cv_.notify_all();
            stale.join();
            stop_task_cancel_[axis].store(false);
            tlock.lock();
        }
        stop_task_thread_[axis] = std::thread([this, channel, axis, stop_task_generation]() {
            auto& protocol = *protocol_;
            auto deadline = std::chrono::steady_clock::now() + kAxisStopTimeout;
            bool stopped = false;
            while (std::chrono::steady_clock::now() < deadline) {
                try {
                    if (!protocol.inquire_status(channel).running) {
                        stopped = true;
                        break;
                    }
                } catch (...) {  // NOLINT(bugprone-empty-catch)
                    // Transient poll failure; keep trying until the deadline.
                }
                if (!task_wait_for(std::chrono::milliseconds(50), stop_task_cancel_[axis])) {
                    return;  // cancelled by disconnect/destruction
                }
            }
            std::unique_lock<std::mutex> lock(mutex_);
            if (!connected_ || stop_task_cancel_[axis].load()) {
                return;
            }
            manual_axis_slewing_[axis] = false;
            if (!stopped) {
                ALPACA_LOG_WARN("SkyWatcher", "MoveAxis stop: axis " + std::to_string(channel) +
                                                  " still reported running at timeout");
            }
            // ASCOM: MoveAxis(axis, 0) restores the previous tracking state —
            // but only if no newer command took over THIS axis while the task
            // polled. motion_generation_ is bumped by every motion command on
            // EITHER axis (see its declaration), so a raw equality check here
            // was a false positive: stopping the OTHER axis bumped the shared
            // counter and silently skipped this restore, even though nothing
            // touched this axis at all (found via a loopback regression test
            // during EQM-35 Pro bring-up, 2026-09-06). `tracking_`/
            // `dec_rate_arcsec_per_sec_` below guard a COMPLETED
            // SetTracking(false) -- that path publishes tracking_ = false
            // under the SAME mutex_ this task also holds here. They do NOT
            // guard one still IN FLIGHT: set_tracking_locked(false) bumps
            // motion_generation_ first, then releases mutex_ inside
            // stop_axis_and_wait_locked()'s poll loop, and assigns tracking_
            // only after that wait returns -- so a restore tail waking inside
            // that window reads tracking_ == true, restores the drive, and the
            // caller throws "Tracking change superseded by a concurrent motion
            // command" with the mount left tracking. That is issue #535
            // (open); its regression case is quarantined [!mayfail] in
            // test_skywatcher_async.cpp, so nothing gates this path until #535
            // lands. What the generation check still needs to catch
            // is a goto/park/home/pulse-guide that took over THIS axis, none
            // of which necessarily touch tracking_/dec_rate_arcsec_per_sec_ --
            // hence the same same_axis_owner idiom already used by the duty-
            // cycle worker above (same rationale, same comment there: "the
            // global generation cannot tell a same-axis supersession from an
            // unrelated other-axis command"). manual_axis_slewing_[axis] is
            // NOT part of this check (unlike the duty-cycle worker's copy):
            // it was just unconditionally cleared under this same lock a few
            // lines above, so it can never be true here (PR #1 review).
            const bool same_axis_owner =
                goto_in_progress_ || parking_ || homing_ || slewing_cached_ || pulse_axis_active_[channel - 1];
            const bool generation_ok = motion_generation_ == stop_task_generation || !same_axis_owner;
            if (channel == kAxisRa && tracking_ && generation_ok) {
                try {
                    set_tracking_locked(lock, true);
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("SkyWatcher",
                                    std::string("MoveAxis stop: failed to restore tracking: ") + e.what());
                }
            } else if (channel == kAxisDec && tracking_ && dec_rate_arcsec_per_sec_ != 0.0 && generation_ok) {
                // Same restore contract for Dec: a manual nudge must not
                // silently cancel an active DeclinationRate offset.
                try {
                    apply_dec_rate_offset_locked(lock);
                } catch (const std::exception& e) {
                    ALPACA_LOG_WARN("SkyWatcher",
                                    std::string("MoveAxis stop: failed to restore Dec rate offset: ") + e.what());
                }
            }
        });
    }

    std::pair<double, double> get_axis_rate_range(int axis) const override {
        if (axis != 0 && axis != 1) {
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }
        return {0.0, kMaxMoveAxisRateDegPerSec};
    }

    std::vector<std::pair<double, double>> get_axis_rate_ranges(int axis) const override {
        if (axis == 2) {
            // Tertiary axis unsupported: empty range set per ASCOM semantics.
            return {};
        }
        if (axis != 0 && axis != 1) {
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }
        return {{0.0, kMaxMoveAxisRateDegPerSec}};
    }

    void abort_slew() override {
        reap_pulse_task();
        // Cancel an in-flight async slew/refinement (the task joins later via
        // reap; the flag makes its waits and the refine loop exit promptly —
        // without this, the refinement re-slews after the abort's stop).
        slew_task_cancel_.store(true);
        task_cv_.notify_all();
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        check_not_fully_parked_locked("AbortSlew");
        // AbortSlew is itself a motion command: bump the generation so any
        // stop-wait sleeping in an unlock window observes the supersession
        // and its dispatch aborts BEFORE sending new motor commands (the
        // cancel flag alone only catches it after the re-dispatch).
        ++motion_generation_;
        reap_rate_verify_task();  // AbortSlew stops RA too: no resend into it
        auto& protocol = *protocol_;
        // Instant stop (":L") rather than the ramped ":K": AbortSlew's contract
        // is to stop NOW, and the ramp-down from an 800x slew otherwise leaves
        // the axes reporting a GOTO in progress for over a second, which the
        // next ConformU test observes as Slewing stuck true.
        protocol.instant_stop(kAxisRa);
        protocol.instant_stop(kAxisDec);
        cmd_axis_rate_deg_s_[0] = 0.0;
        cmd_axis_rate_deg_s_[1] = 0.0;
        goto_in_progress_ = false;
        restoring_tracking_ = false;
        slewing_cached_ = false;
        // open-astro#575: AbortSlew is a valid clearing command for a stored
        // slew failure -- the client acted on the error, so the next Slewing
        // read must answer normally again.
        clear_last_slew_error_locked();
        slew_force_until_ = std::chrono::steady_clock::time_point::min();
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
        restore_tracking_after_slew_ = false;
        tracking_ = false;
        invalidate_position_cache_locked();
    }

    void slew_to_alt_az(double altitude, double azimuth) override {
        (void)altitude;
        (void)azimuth;
        throw AlpacaException("SlewToAltAz not supported", AlpacaError::MethodNotImplemented);
    }

    void slew_to_alt_az_async(double altitude, double azimuth) override {
        (void)altitude;
        (void)azimuth;
        throw AlpacaException("SlewToAltAzAsync not supported", AlpacaError::MethodNotImplemented);
    }

    void sync_to_alt_az(double altitude, double azimuth) override {
        (void)altitude;
        (void)azimuth;
        throw AlpacaException("SyncToAltAz not supported", AlpacaError::MethodNotImplemented);
    }

private:
    void check_connected() const {
        // open-astro#445: also refuse on a lost link, so cached position reads
        // cannot keep answering from a mount that is no longer there.
        if (!connected_ || !protocol_->link_alive()) {
            throw AlpacaException("Not connected to Sky-Watcher mount", AlpacaError::NotConnected);
        }
    }

    // A park in flight (parking_) gates the same members as a completed park
    // so a slew/MoveAxis/sync cannot silently clobber it; AbortSlew and
    // Unpark are allowed through and cancel the park.
    void check_not_parked_locked(const char* operation) const {
        if (parked_ || parking_) {
            throw AlpacaException(std::string(operation) + " is not allowed while " + (parked_ ? "parked" : "parking"),
                                  AlpacaError::InvalidWhileParked);
        }
    }

    void check_not_fully_parked_locked(const char* operation) const {
        if (parked_) {
            throw AlpacaException(std::string(operation) + " is not allowed while parked",
                                  AlpacaError::InvalidWhileParked);
        }
    }

    static void validate_ra_dec(double ra, double dec, const char* context) {
        if (!std::isfinite(ra) || ra < 0.0 || ra >= 24.0) {
            throw AlpacaException(std::string(context) + ": RA out of range", AlpacaError::InvalidValue);
        }
        if (!std::isfinite(dec) || dec < -90.0 || dec > 90.0) {
            throw AlpacaException(std::string(context) + ": Dec out of range", AlpacaError::InvalidValue);
        }
    }

    // open-astro#521: a Sky-Watcher axis keeps running until something tells it
    // to stop, so motion outlives the link that started it. On the way back in,
    // decide from the LENGTH of the outage rather than by a flat rule: a USB
    // re-enumeration or a briefly disturbed connector is a glitch and the slew
    // the client asked for is still wanted, while a long gap means nobody has
    // been in control of a moving mount.
    //
    // Confirmed on an EQM-35 Pro (2026-09-17): with RA turning at 0.5 deg/s the
    // cable was pulled and replaced, ":f1" then read "=111" (speed mode,
    // running, initialized) while the driver reported Slewing false, and no
    // ":K" was sent anywhere in the relink.
    void adopt_surviving_motion_locked(std::unique_lock<std::mutex>& lock, const AxisStatus (&entry_status)[2]) {
        (void)lock;  // held by the caller for the whole connect
        const auto lost_at = protocol_->consume_link_lost_at();
        const auto now = std::chrono::steady_clock::now();
        // A stop is only justified when we have POSITIVE evidence of a long,
        // unmonitored outage (a recorded link-loss timestamp old enough to
        // clear the preserve window). No stamp is NOT that evidence: it also
        // covers a driver instance that has never connected before (a
        // process restart while the mount kept tracking under its own
        // power), where "nothing recorded" means nothing happened, not that
        // something did. Treat the no-stamp case the same as a brief,
        // still-supervised outage: preserve rather than stop. Reviewed
        // 2026-09-18: sending ":K" on this branch previously halted a
        // perfectly healthy tracking mount on every fresh connect where the
        // axis was already running.
        //
        // Consequence worth knowing before relying on the stop branch: only
        // the SERIAL loss paths stamp link_lost_at_ (the device-node presence
        // check and lose_serial_link_locked, both serial-only). A network
        // (UDP) mount therefore never records an outage, so lost_at is always
        // empty for it and it ALWAYS takes the preserve branch, however long
        // it was gone. That follows the "no positive evidence => preserve"
        // rule rather than violating it, but it means the stop branch is in
        // practice serial-only today (review of #553).
        const bool long_unmonitored_outage =
            lost_at.has_value() && (now - *lost_at) >= detail::relink_motion_preserve_window();

        // ONE budget for the whole stop-and-confirm phase, not one per axis.
        // Both axes can take the stopping branch, and this all runs under the
        // connect's lock -- which get_connected() also takes -- so a per-axis
        // timeout made the worst case two full timeouts of blocking inside
        // set_connected(true), overrunning the client's Connect() budget and
        // stalling every Connected poll meanwhile (review of #553). Whichever
        // axis is confirmed second gets whatever remains; its ":K" is sent
        // regardless, since only the confirmation is bounded here.
        const auto stop_confirm_deadline = std::chrono::steady_clock::now() + kConnectStopConfirmBudget;

        for (int axis = 0; axis < 2; ++axis) {
            const AxisStatus& status = entry_status[axis];
            if (!status.running) {
                continue;
            }
            const int channel = axis == 0 ? kAxisRa : kAxisDec;
            std::string where = "axis " + std::to_string(channel) + " (";
            where += status.speed_mode ? "speed mode" : "GOTO mode";
            if (status.fast) {
                where += ", fast";
            }
            if (status.blocked) {
                where += ", blocked";
            }
            where += ")";
            std::string gap;
            if (lost_at) {
                gap = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now - *lost_at).count());
                gap += " s";
            } else {
                gap = "no recorded link loss";
            }

            if (!long_unmonitored_outage) {
                // Preserve the motion and leave every session flag clear.
                //
                // manual_axis_slewing_ means "a MoveAxis THIS driver issued
                // owns this axis": the only other writers set it having just
                // commanded the motion (move_axis) or having failed to stop an
                // axis they know is running away (the pulse-guide stop tail).
                // Here the provenance is unknown -- ":f" reports speed mode
                // and running for a MoveAxis and for ordinary sidereal
                // tracking alike, and tracking is by far the likelier of the
                // two to be found at connect. Setting the flag on that guess
                // wedges Slewing true for the rest of the session on a merely
                // tracking mount (nothing on the ordinary path clears it, so a
                // sequencer waiting for Slewing to drop hangs), contradicts
                // the board semantics this driver documents -- a tracking axis
                // is NOT slewing -- and, through axis_busy_locked(), strands
                // every RightAscensionRate / SiteLatitude write behind a
                // "the busy operation's restore will re-apply this" branch
                // whose restore is never scheduled (review of #553).
                //
                // Not setting it costs one bounded thing instead: a genuine
                // MoveAxis that outlived the link is no longer stoppable
                // through MoveAxis(axis, 0), which consults this flag.
                // AbortSlew still stops it, and Slewing meanwhile reports what
                // the board actually says. Guessing "tracking" degrades one
                // stop route in a rare case; guessing "MoveAxis" silently
                // breaks ordinary operation -- so the tie goes to leaving the
                // flag clear.
                //
                // cmd_axis_rate_deg_s_ stays zero for its own reason: the
                // board reports THAT an axis is running, not at what rate, and
                // inventing one would feed the dead-reckoning model a number
                // nothing measured. Reads re-anchor on hardware counts within
                // kPositionCacheTtl, so the position stays honest; only the
                // between-poll interpolation is flat.
                //
                // GOTO-mode motion needs no flag either:
                // get_hardware_slewing_locked() re-derives it from the board
                // on every read.
                std::string message = "Link restored after ";
                message += gap;
                message += " with ";
                message += where;
                message +=
                    " still running; motion preserved and left under client control"
                    " (AbortSlew stops it; MoveAxis(axis, 0) cannot, the session flag is clear)";
                ALPACA_LOG_WARN("SkyWatcher", message);
            } else {
                std::string message = "Link restored after ";
                message += gap;
                message += " with ";
                message += where;
                message += " still running and nobody in control; stopping it";
                ALPACA_LOG_WARN("SkyWatcher", message);
                stop_surviving_axis_locked(channel, stop_confirm_deadline);
            }
        }
    }

    // Stop and CONFIRM: a stop command that was accepted is not an axis at
    // rest, and the caller is about to report the mount connected and idle.
    // @p deadline is the caller's budget for the whole stop-confirm phase and
    // is SHARED with the other axis, so this must never extend it: the stop
    // itself always goes out, only the waiting for rest is bounded.
    void stop_surviving_axis_locked(int channel, std::chrono::steady_clock::time_point deadline) {
        auto& protocol = *protocol_;
        try {
            protocol.stop_motion(channel);
        } catch (const std::exception& e) {
            ALPACA_LOG_ERROR("SkyWatcher",
                             "Failed to stop surviving motion on axis " + std::to_string(channel) + ": " + e.what());
            return;
        }
        while (std::chrono::steady_clock::now() < deadline) {
            try {
                if (!protocol.inquire_status(channel).running) {
                    cmd_axis_rate_deg_s_[channel - 1] = 0.0;
                    return;
                }
            } catch (const std::exception& e) {
                // Transient poll failure; keep trying until the deadline.
                std::string message = "stop_surviving_axis_locked: transient poll failure: ";
                message += e.what();
                ALPACA_LOG_TRACE("SkyWatcher", message);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        // The stop was sent and accepted; we simply ran out of the shared
        // connect budget before seeing it come to rest. Say both halves, so
        // this is not read as "the axis refused to stop".
        ALPACA_LOG_ERROR("SkyWatcher", "Axis " + std::to_string(channel) +
                                           " was stopped at connect but had not reported at rest within the shared "
                                           "stop-confirm budget; it may still be decelerating");
    }

    // open-astro#575: every clear of the stored slew failure (new initiator,
    // AbortSlew, reconnect) starts a new epoch; see slew_error_epoch_.
    void clear_last_slew_error_locked() {
        last_slew_error_.clear();
        ++slew_error_epoch_;
    }

    void reset_runtime_state_locked() {
        target_ra_set_ = false;
        target_dec_set_ = false;
        client_disagreement_warned_ = false;  // open-astro#400: one WARN per connection
        // open-astro#414: the client UTCDate offset and the discipline flag
        // sampled with it are session state; a reconnect starts from the
        // host clock until the client writes UTCDate again.
        has_utc_offset_ = false;
        utc_offset_ = {};
        utc_offset_host_was_synchronized_ = false;
        parked_ = false;
        at_home_ = false;
        tracking_ = false;
        restore_tracking_after_slew_ = false;
        pulse_axis_active_[0] = false;
        pulse_axis_active_[1] = false;
        pulse_axis_in_motion_[0] = false;
        pulse_axis_in_motion_[1] = false;
        slewing_cached_ = false;
        clear_last_slew_error_locked();  // open-astro#575: a reconnect starts clean
        slew_force_until_ = std::chrono::steady_clock::time_point::min();
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
        parking_ = false;
        homing_ = false;
        goto_in_progress_ = false;
        restoring_tracking_ = false;
        tracking_rate_ = 0;
        ra_rate_sec_per_sidereal_sec_ = 0.0;
        dec_rate_arcsec_per_sec_ = 0.0;
        ra_duty_rate_deg_s_ = 0.0;
        dec_duty_rate_deg_s_ = 0.0;
        dec_offset_running_ = false;
        cmd_axis_rate_deg_s_[0] = 0.0;
        cmd_axis_rate_deg_s_[1] = 0.0;
        position_cache_valid_ = false;
        // open-astro#505: a reconnect re-runs the ":F" init, which is exactly
        // what clears the board-reset condition, so the fault must not survive
        // into the new session. Re-seed the epoch so the connect's own reads
        // are not mistaken for a recovery that needs validating.
        board_reset_fault_.clear();
        seen_recovery_epoch_ = protocol_->link_recovery_epoch();
        pointing_branch_ = 1;  // open-astro#459: the a2 >= 0 branch, the pre-#459 answer at home
        // open-astro#458: a board that will not answer ":e" is an unmeasured
        // one; never carry the previous connection's sense into this one.
        dec_axis_sense_ = 0;
        // Both are MEASURED off the mount that was connected, so they must not
        // survive into the next one: a driver instance reconnected to
        // different hardware would otherwise aim a goto ahead by the previous
        // mount's numbers (review note on #448).
        resume_latency_seconds_ = kTrackingResumeSeconds;
        goto_overhead_seconds_ = kGotoRampSeconds;
        last_goto_dispatch_time_ = std::chrono::steady_clock::time_point{};
        last_landing_time_ = std::chrono::steady_clock::time_point{};
    }

    // True once both coordinates have a provenance: either the device config
    // carried both, or a client wrote each through the ASCOM setters
    // (open-astro#274). A client that writes only one leaves the other at its
    // configured value, so the two are tracked separately.
    bool site_coordinates_known_locked() const { return site_latitude_set_ && site_longitude_set_; }

    bool hemisphere_south_locked() const { return site_latitude_ < 0.0; }

    // Direction the RA axis must turn for the SKY hour angle to increase
    // (tracking, RightAscensionRate, East/West guide pulses): increasing
    // counts north of the equator, decreasing south of it, because the mount
    // faces the opposite pole and ha = -ha_mech there (pointing-model
    // comment). Mechanical motion -- MoveAxis, goto deltas, AutoHome -- never
    // applies this: a signed axis rate means the same thing everywhere.
    double ra_axis_sign_locked() const { return hemisphere_south_locked() ? -1.0 : 1.0; }

    // open-astro#458: the sign `k` of the 6 h home term, `s * eps` for a board
    // whose dec-axis sense was measured, and +1 for any other board, which
    // keeps the #432 model it was validated on. Every reader of the dec-axis
    // branch that means a SIDE OF THE MERIDIAN (the home term, the goto's
    // branch choice, SideOfPier) goes through `k * branch`; the Dec rate and
    // guide signs do not, because dec = s * (90 - |a2|) holds on every board.
    double home_term_sign_locked() const {
        if (dec_axis_sense_ == 0) {
            return 1.0;
        }
        return ra_axis_sign_locked() * static_cast<double>(dec_axis_sense_);
    }

    // Drops a client offset the host clock has moved out from under. The
    // offset is a snapshot delta against the host clock at the time of the
    // UTCDate write; if the host clock is stepped afterwards (Sync Time, NTP,
    // a manual `date`) the delta no longer describes anything, so it goes
    // rather than being applied on top of the corrected clock (review of
    // #291). Returns true when an offset survives.
    bool client_offset_survives_locked(std::chrono::system_clock::time_point system_now) const {
        if (!has_utc_offset_) {
            return false;
        }
        if (detail::host_clock_stepped(system_now - utc_anchor_system_,
                                       std::chrono::steady_clock::now() - utc_anchor_steady_)) {
            has_utc_offset_ = false;
            utc_offset_ = {};
            ALPACA_LOG_INFO("SkyWatcher",
                            "Host clock was stepped after the client's UTCDate write; dropping the "
                            "client offset and using the host clock");
            return false;
        }
        return true;
    }

    // The clock the mount is AIMED by: LST, SiderealTime,
    // DestinationSideOfPier and every goto. Honours the client's offset only
    // when the host clock was undisciplined at the time of the write
    // (open-astro#301); see detail::pointing_uses_client_offset().
    // See docs/decisions/0001-skywatcher-pointing-clock.md for the rationale.
    std::chrono::system_clock::time_point utc_now_locked() const {
        const auto system_now = std::chrono::system_clock::now();
        const bool survives = client_offset_survives_locked(system_now);
        resample_host_discipline_locked(survives);
        if (!detail::pointing_uses_client_offset(survives, utc_offset_host_was_synchronized_)) {
            return system_now;
        }
        return system_now + utc_offset_;
    }

    // open-astro#405: a host that was undisciplined at the write and acquires
    // NTP discipline by slewing (no step, so client_offset_survives_locked()
    // never notices) would keep pointing by the client's offset for the whole
    // session. Re-sample the discipline probe at most once per interval, off
    // the write path, and stop applying the offset once the host is good.
    // The mirror direction (disciplined then lost) is left alone: ignoring
    // the offset is the safe side. Only ever moves the flag false -> true.
    void resample_host_discipline_locked(bool offset_survives) const {
        if (!offset_survives || utc_offset_host_was_synchronized_) {
            return;
        }
        const auto interval = detail::host_discipline_resample_interval();
        if (interval.count() <= 0) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < next_discipline_resample_) {
            return;
        }
        next_discipline_resample_ = now + interval;
        if (detail::host_synchronized_probe()) {
            utc_offset_host_was_synchronized_ = true;
            ALPACA_LOG_INFO("SkyWatcher",
                            "Host clock became NTP-disciplined after the client's UTCDate write; pointing by the "
                            "host clock from now on (the UTCDate readback still honours the client)");
        }
    }

    // The ASCOM UTCDate readback, which always honours a client's write:
    // UTCDate is the client's property to set, and ConformU reads back what
    // it wrote. Only a host clock step drops the offset here.
    std::chrono::system_clock::time_point client_utc_now_locked() const {
        const auto system_now = std::chrono::system_clock::now();
        if (!client_offset_survives_locked(system_now)) {
            return system_now;
        }
        return system_now + utc_offset_;
    }

    // ── Pointing model ──────────────────────────────────────────────────────
    // Home (counts == kHomeCounts on both axes): counterweight down, OTA at
    // the visible celestial pole. Axis angles are signed degrees from home,
    // positive in the board's increasing-count direction.
    //
    // MECHANICAL frame (the geometry of a German equatorial, written here
    // with the northern sense of the RA axis):
    //   Branch A (dec axis angle >= 0): dec_mech = 90 - a2, ha_mech = a1/15 + 6.
    //   Branch B (dec axis angle <  0): dec_mech = 90 + a2, ha_mech = a1/15 - 6.
    // The code carries the two terms separately: `ha_mech_hours` below holds
    // the a1 term alone, and the signed 6 h term is added after the
    // hemisphere sign, which is what the SKY frame paragraph describes. The
    // branch is read off the axis angle except at the pole, where both
    // branches sit on the same encoder count and the one the command path
    // last chose is remembered instead (branch_from_axis_locked, #459).
    // The 6 h term is the counterweight-down home: with the counterweight bar
    // vertical the dec axis lies IN the meridian plane, so a pure dec
    // rotation sweeps the OTA along the HA = +/-6 h great circle, and the
    // meridian is reached only with the bar horizontal (a1 = +/-90). Both
    // branches therefore keep a1 inside +/-90 for every reachable target,
    // which is the counterweight-never-above-horizontal rule every GEM
    // driver enforces. North of the equator this is exactly indi-eqmod's
    // EncoderToHours() (`range24(result + 6.0)`) once its DE zero is
    // re-expressed relative to its home (DEStepHome = DEStepInit + steps/4);
    // south of it the two differ by 12 h, see the SKY frame note below.
    //
    // SKY frame: the mount faces the visible pole, so south of the equator
    // the same RA-axis rotation runs the sky's hour angle the other way, and
    // so does the 6 h home term, because a mount facing the other pole is
    // the same mount turned half a turn about the vertical:
    //   HA = s * (a1/15) + k * (a2 >= 0 ? +6 : -6),  dec = s * (90 - |a2|),
    //   with s = +1 north, -1 south, and k = s * eps (home_term_sign_locked()).
    //   eps is the board's dec-axis count sense, which is wiring, not
    //   latitude: the board is never told the latitude. Tracking DEcreases a1
    //   south of the equator, which ra_axis_sign_locked() applies to the
    //   drive rate. With eps = +1, k = s is exactly indi-eqmod in both
    //   hemispheres.
    //
    // open-astro#458: eps is MEASURED for three boards only, -1 on the EQM-35
    // Pro (0x32) and +1 on the Wave 150i (0x45) and the EQ-AL55i Pro (0x09);
    // see measured_dec_axis_sense(). Every other board keeps k = +1, the model it
    // shipped with, which is right wherever s * eps = +1 and 12 h out in hour
    // angle wherever s * eps = -1; which of the two applies to an unmeasured
    // board takes one reading on that board (drive to a1 = 0, a2 = +90 and
    // see whether the tube ends level east or level west).
    //   The ASCOM pier side is (k * branch > 0) -> pierEast in both
    //   hemispheres: the goto chooses the branch from the sky hour angle, so
    //   the two agree by construction (open-astro#261).
    //
    // History: until open-astro#432 the model read HA = a1/15 (branch A) and
    // a1/15 - 12 (branch B). That is six hours out in the north and, away
    // from a1 = 45 deg, wrong in the south too, so gotos landed on the wrong
    // sky position while the driver reported the target back. It passed
    // ConformU because the driver reports the same model it commands.
    //
    // MEASURED ON HARDWARE, 2026-09-12, EQM-35 Pro at latitude -37.2 (rounded)
    // with the shipped 3.5.1 build, tube position read off the mount by hand.
    // Each row is an axis position the driver was commanded to, and where the
    // OTA physically ended up:
    //
    //   a1     a2     observed                     this model        shipped
    //   +1.6   -90    level, pointing east         HA -6.1 h, lvl    -52.8 deg
    //   +60.0  -90    down about 45 deg            HA -10.0 h, -44   -23.5 deg
    //   +45.1  -70    down, azimuth about 136      HA -9.0 h, -19    -18.8 deg
    //
    // The first row is the decisive one and needs no instrument: with the
    // counterweight straight down and the dec axis at 90 deg the OTA is
    // perpendicular to both the polar axis and the counterweight bar, which
    // both lie in one vertical plane, so the tube MUST come out level -- and
    // level, square to the meridian, is six hours of hour angle from it. The
    // shipped model puts that same position 53 deg below the horizon.
    //
    // The fourth data point is northern and comes from the Wave 150i report
    // that opened #432: commanded a1 = +62.0, a2 = +71.0 for a target at
    // HA +4.12 h, dec +19.05; this model puts those axes at HA +10.13 h,
    // altitude -20.7, and the reporter photographed the tube about 20 deg
    // below the horizon. The shipped model claims altitude +33.
    //
    // The fifth (open-astro#458), 2026-09-19, the same EQM-35 Pro with the
    // 4.0.0 build, still set up facing the south pole but with the driver's
    // latitude at +37.2, which is a northern mount turned half a turn about
    // the vertical. For HA -3 h, dec +30 the driver sent a1 = +45, a2 = -60
    // and reported alt 52, az 87; the saddle ended front-left and slightly
    // down (SE, about -8 deg), which is HA +9 h, alt -10.7: 12 h out, the k
    // this build now applies to 0x32 in the north.
    //
    // Note for anyone tempted to re-derive this from indi-eqmod: its
    // EncoderToHours() is written against its own encoder zero and step
    // direction, and transcribing it cost this fix a wrong sign that only
    // the rig caught. The table above is the reference.

    std::pair<double, double> compute_ra_dec_locked() const {
        // Dead-reckon between hardware reads: while an axis runs at a
        // commanded speed rate, extrapolate the cached angle by rate x
        // elapsed. Raw counts quantize at ~0.31 arcsec and the cache is up to
        // kPositionCacheTtl stale -- reporting the commanded model keeps RA
        // steady under tracking (no LST-vs-stale-HA sawtooth) and resolves
        // sub-count offset rates. Goto/stop paths zero the commanded rates,
        // so a slewing or idle axis reports the raw cached angle.
        double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - last_position_update_).count();
        // Offsets hold the model anchor for the whole offset session; without
        // offsets the cache re-anchors within seconds, so clamp tight.
        dt = std::clamp(dt, 0.0, rate_offsets_active_locked() && tracking_ ? 3600.0 : 5.0);
        double a1 = cached_ra_axis_deg_ + cmd_axis_rate_deg_s_[0] * dt;
        double a2 = cached_dec_axis_deg_ + cmd_axis_rate_deg_s_[1] * dt;
        // dec_mech = 90 - |a2| on both branches; the branch decides only the
        // sign of the 6 h home term, and at the pole it is the remembered one
        // (see branch_from_axis_locked, open-astro#459).
        const double dec_mech = 90.0 - std::abs(a2);
        const double ha_mech_hours = a1 / kHoursToDegrees;
        const double sky_sign = hemisphere_south_locked() ? -1.0 : 1.0;
        const double dec = sky_sign * dec_mech;
        const double ha_hours =
            wrap_hour_angle(sky_sign * ha_mech_hours +
                            home_term_sign_locked() * kHomeHourAngleOffsetHours * branch_from_axis_locked(a2));
        double lst = compute_local_sidereal_time_hours(utc_now_locked(), site_longitude_);
        double ra = wrap_hours(lst - ha_hours);
        return {ra, std::clamp(dec, -90.0, 90.0)};
    }

    std::pair<double, double> ra_dec_to_axis_degrees_locked(double ra, double dec,
                                                            double lst_advance_hours = 0.0) const {
        double lst = compute_local_sidereal_time_hours(utc_now_locked(), site_longitude_) + lst_advance_hours;
        double ha = wrap_hour_angle(lst - ra);
        const double sky_sign = hemisphere_south_locked() ? -1.0 : 1.0;
        const double dec_mech = sky_sign * dec;
        // The side is chosen from the SKY hour angle in both hemispheres:
        // HA >= 0 (target west of the meridian) puts the OTA on the east side
        // of the pier, which is the branch with k * branch > 0 (the a2 >= 0
        // branch on a board with k = +1, open-astro#458). get_side_of_pier()
        // reads the same rule back off the axis, and
        // get_destination_side_of_pier() states it directly, so all three
        // agree by construction.
        const double k = home_term_sign_locked();
        const double side = ha >= 0.0 ? 1.0 : -1.0;
        const double branch = k * side;
        const double a2 = branch * (90.0 - dec_mech);
        // HA = sky_sign * a1/15 + k * 6 * branch, inverted. k * branch is the
        // side, so a1 does not depend on k. |a1| <= 90 for every reachable
        // target, which is the counterweight-never-above-horizontal rule
        // falling out of the geometry rather than being enforced.
        const double a1 = sky_sign * (ha - kHomeHourAngleOffsetHours * side) * kHoursToDegrees;
        return {a1, a2};
    }

    // open-astro#459: which dec-axis branch the mount is on, read back from
    // the axis angle. Away from a2 = 0 the sign of the angle IS the branch.
    // At the exact pole both branches command the same count (a2 =
    // branch * (90 - 90) rounds to the home count either way), so nothing
    // read back from the encoder can tell them apart: the -0.0 the command
    // path produces on the negative branch never survives the int32 counts
    // it is stored as. Inside a deadband of two encoder counts (one count of
    // rounding on the way out, one on the way back) the answer is the branch
    // the command path last committed, pointing_branch_. Every site that
    // branches on the dec-axis sign (the 6 h home term, SideOfPier, the Dec
    // guide and rate signs) goes through here so they agree by construction.
    int branch_from_axis_locked(double a2) const {
        const auto cpr = axis_params_[1].counts_per_revolution;
        const double deadband = cpr > 0 ? 2.0 * 360.0 / static_cast<double>(cpr) : 0.0;
        if (std::abs(a2) <= deadband) {
            return pointing_branch_;
        }
        return a2 >= 0.0 ? 1 : -1;
    }

    // Record the branch a commanded dec-axis angle sits on. Called wherever
    // a dec-axis angle reaches the hardware (dispatch_goto_locked() for every
    // goto, sync after its position writes), never for a mere
    // DestinationSideOfPier computation. A goto to home (a2 = +0.0: the
    // no-indexer FindHome, the default Park) therefore lands on the positive
    // branch, the same answer AutoHome's re-anchor and connect give. The
    // command-path angle keeps its sign bit at the pole (-0.0 on the
    // negative branch), so std::signbit is the right reader here, and only
    // here.
    void remember_command_branch_locked(double commanded_a2) { pointing_branch_ = std::signbit(commanded_a2) ? -1 : 1; }

    std::pair<double, double> compute_alt_az_locked() const {
        auto [ra, dec] = compute_ra_dec_locked();
        double lst = compute_local_sidereal_time_hours(utc_now_locked(), site_longitude_);
        double ha_rad = wrap_hour_angle(lst - ra) * kHoursToDegrees * std::numbers::pi / 180.0;
        double dec_rad = dec * std::numbers::pi / 180.0;
        double lat_rad = site_latitude_ * std::numbers::pi / 180.0;
        double sin_alt =
            std::sin(dec_rad) * std::sin(lat_rad) + std::cos(dec_rad) * std::cos(lat_rad) * std::cos(ha_rad);
        sin_alt = std::clamp(sin_alt, -1.0, 1.0);
        double alt_rad = std::asin(sin_alt);
        double cos_az =
            (std::sin(dec_rad) - std::sin(alt_rad) * std::sin(lat_rad)) / (std::cos(alt_rad) * std::cos(lat_rad));
        cos_az = std::clamp(cos_az, -1.0, 1.0);
        double az_deg = std::acos(cos_az) * 180.0 / std::numbers::pi;
        if (std::sin(ha_rad) > 0.0) {
            az_deg = 360.0 - az_deg;
        }
        return {alt_rad * 180.0 / std::numbers::pi, wrap_degrees(az_deg)};
    }

    // ── Position cache ──────────────────────────────────────────────────────

    void invalidate_position_cache_locked() const { position_cache_valid_ = false; }

    // Re-anchor the dead-reckoning model at "now" WITHOUT a hardware read.
    // Rate setters need a fresh anchor time for the new rate, but a hardware
    // re-anchor on a moving axis shifts the reported position by up to one
    // encoder count (~0.31 arcsec) plus the axis start latency. ConformU
    // samples RA before the rate write and 10 s after it: at the 0.05
    // arcsec/s test rate that jump alone is a 25% "rate" error (measured on
    // the Wave 100i). Advancing the cached angle by the commanded rate keeps
    // the reported position continuous across the rate change; a cache that
    // is invalid or stale for the current regime falls back to hardware.
    void anchor_model_locked() const {
        auto now = std::chrono::steady_clock::now();
        auto age = now - last_position_update_;
        bool fresh = position_cache_valid_ &&
                     (age < kPositionCacheTtl || (rate_offsets_active_locked() && tracking_ && age < kOffsetModelHold));
        if (!fresh) {
            refresh_position_cache_locked(true);
            return;
        }
        double dt = std::chrono::duration<double>(age).count();
        cached_ra_axis_deg_ += cmd_axis_rate_deg_s_[0] * dt;
        cached_dec_axis_deg_ += cmd_axis_rate_deg_s_[1] * dt;
        last_position_update_ = now;
    }

    // True while a goto/park/home/pulse/manual motion owns THIS axis.
    // Ownership is per axis: a goto/park/home/slew takes both, a pulse or a
    // manual MoveAxis takes only its own -- the same idiom the duty worker
    // and the MoveAxis stop task already use ("the global generation cannot
    // tell a same-axis supersession from an unrelated other-axis command").
    bool axis_busy_locked(int channel) const {
        return goto_in_progress_ || parking_ || homing_ || slewing_cached_ || manual_axis_slewing_[channel - 1] ||
               pulse_axis_active_[channel - 1];
    }

    // True while a goto/park/home/pulse/manual motion owns EITHER axis: rate
    // setters must not issue motion then (they would hijack the axis and
    // make get_hardware_slewing_locked read "not slewing" mid-goto); the
    // stored rates are applied by the post-slew/pulse/MoveAxis restores.
    bool axes_busy_locked() const { return axis_busy_locked(kAxisRa) || axis_busy_locked(kAxisDec); }

    bool rate_offsets_active_locked() const {
        return ra_rate_sec_per_sidereal_sec_ != 0.0 || dec_rate_arcsec_per_sec_ != 0.0;
    }

    // open-astro#505: refuse to serve the cache — or the dead-reckoned model —
    // while the link fault is latched, and surface the board-reset case the
    // latch alone cannot catch. Callers are the read paths; a faulted link is
    // NOT a disconnection, so this is DriverException and Connected stays true.
    void throw_comms_compromised_locked(const std::string& reason) const {
        throw AlpacaException("Sky-Watcher mount communications compromised: " + reason, AlpacaError::DriverException);
    }

    // Terminal until reconnect, so it is checked BEFORE any hardware attempt —
    // unlike a link fault, which must not short-circuit the read that would
    // clear it.
    void throw_if_board_reset_locked() const {
        if (!board_reset_fault_.empty()) {
            throw_comms_compromised_locked(board_reset_fault_);
        }
    }

    // Run after a good reply cleared a latched fault. A board that merely went
    // quiet comes back with its session intact; one that power-cycled answers
    // just as well while reporting init_done false with its position registers
    // reset to the home count, and the driver only ever sends ":F" at connect.
    // Serving coordinates from those reset registers is silent mispointing, so
    // latch it and make the client reconnect (which re-initialises the board).
    // Confirmed on an EQM-35 Pro 2026-09-17: ":f1" read "=100" after a mains
    // power cycle mid-session, ":j2" read the bare home count.
    void check_board_survived_recovery_locked() const {
        const std::uint64_t epoch = protocol_->link_recovery_epoch();
        if (epoch == seen_recovery_epoch_) {
            return;
        }
        // The epoch is consumed only once BOTH probes have answered, below.
        // Consuming it up front looked equivalent but was not: the link has
        // just come back, so a single mis-paired straggler surviving the
        // dirty/settle machinery turns one ":f" into an exception, and nothing
        // mints a new epoch unless the link faults AND recovers all over
        // again. The check would then never run, the fault would stay unset,
        // and a power-cycled board's reset home-count registers would be
        // served as a position for the rest of the session -- the exact silent
        // mispointing this check exists to catch (review of #553).
        try {
            const AxisStatus ra = protocol_->inquire_status(kAxisRa);
            const AxisStatus dec = protocol_->inquire_status(kAxisDec);
            seen_recovery_epoch_ = epoch;  // both probes answered: this recovery is now checked
            if (ra.init_done && dec.init_done) {
                ALPACA_LOG_INFO("SkyWatcher", "Link recovered with the board's session intact");
                return;
            }
            board_reset_fault_ =
                "the motor controller restarted while the link was down (initialization cleared), so its "
                "position registers no longer describe where the mount is pointing; reconnect to re-initialise";
            position_cache_valid_ = false;
            ALPACA_LOG_ERROR("SkyWatcher", "Link recovered but the board had restarted: RA init_done=" +
                                               std::string(ra.init_done ? "true" : "false") +
                                               ", Dec init_done=" + std::string(dec.init_done ? "true" : "false"));
        } catch (const std::exception& e) {
            // Link trouble mid-check: leave the epoch UNCONSUMED so the next
            // read retries this recovery rather than skipping it forever.
            std::string message = "check_board_survived_recovery_locked: probe failed, will retry this recovery: ";
            message += e.what();
            ALPACA_LOG_TRACE("SkyWatcher", message);
        }
    }

    void refresh_position_cache_locked(bool force) const {
        auto now = std::chrono::steady_clock::now();
        throw_if_board_reset_locked();
        // A latched fault must not be short-circuited by a fresh-enough cache
        // or by the offset model: on a polled link these reads are the only
        // traffic that can clear the latch, so fall through to the hardware
        // attempt instead of serving a value the board has not confirmed.
        const bool faulted = protocol_->link_faulted();
        if (!faulted && !force && position_cache_valid_ && (now - last_position_update_) < kPositionCacheTtl) {
            return;
        }
        // While rate offsets run, serve the dead-reckoned model instead of
        // re-anchoring on hardware counts: duty-cycled Dec bursts and the
        // offset RA rate make raw reads jitter around the commanded average.
        // Offset entry points and motion commands force a fresh anchor, and
        // the hold is bounded: past kOffsetModelHold the model would pin at
        // the dt clamp and the reported position would silently freeze, so
        // fall through and take a fresh hardware anchor instead.
        // open-astro#505: the 30-minute hold is the longest-lived stale window
        // in this driver and the one the hardware run caught — a board powered
        // off mid-offset-session is not noticed for the whole of it. A latched
        // fault ends the hold.
        if (!faulted && !force && position_cache_valid_ && rate_offsets_active_locked() && tracking_ &&
            (now - last_position_update_) < kOffsetModelHold) {
            return;
        }
        auto& protocol = *protocol_;
        bool succeeded = false;
        try {
            uint32_t ra_counts = protocol.inquire_position(kAxisRa);
            uint32_t dec_counts = protocol.inquire_position(kAxisDec);
            cached_ra_axis_deg_ = counts_to_degrees(ra_counts, axis_params_[0].counts_per_revolution);
            cached_dec_axis_deg_ = counts_to_degrees(dec_counts, axis_params_[1].counts_per_revolution);
            position_cache_valid_ = true;
            last_position_update_ = now;
            succeeded = true;
        } catch (...) {
            // Once latched, the cache is not an answer at any age: the values
            // are as unreachable as the board. This supersedes the ad-hoc
            // stale window below, which on its own let a powered-off mount
            // keep reporting a plausible sidereal-advancing RA.
            const std::string fault = protocol_->link_fault();
            if (!fault.empty()) {
                position_cache_valid_ = false;
                throw_comms_compromised_locked(fault);
            }
            if (!position_cache_valid_) {
                throw;
            }
            // Serve last-known values only briefly: a sustained comms fault
            // must surface as an error, not as a frozen position.
            if ((now - last_position_update_) > kStaleCacheLimit) {
                position_cache_valid_ = false;
                throw;
            }
        }
        if (succeeded) {
            // Those reads just cleared a latched fault if anything did; find
            // out whether the board behind them is still the one we set up.
            check_board_survived_recovery_locked();
            throw_if_board_reset_locked();
        }
    }

    // ── Motion primitives ───────────────────────────────────────────────────

    uint32_t step_period_for_locked(int channel, double rate_deg_per_sec, bool fast) const {
        const AxisParameters& params = axis_params_[channel - 1];
        double counts_per_sec = std::abs(rate_deg_per_sec) * params.counts_per_revolution / 360.0;
        if (counts_per_sec <= 0.0) {
            return kCountsMask;
        }
        double preset = static_cast<double>(params.timer_frequency) / counts_per_sec;
        if (fast) {
            preset *= static_cast<double>(params.high_speed_ratio);
        }
        preset = std::clamp(preset, 1.0, static_cast<double>(kCountsMask));
        return static_cast<uint32_t>(std::lround(preset));
    }

    uint32_t tracking_step_period_for(double rate_deg_per_sec) const {
        // Callers hold mutex_ or run from the pulse task after setup under it;
        // axis_params_ is immutable after connect.
        const AxisParameters& params = axis_params_[0];
        double counts_per_sec = std::abs(rate_deg_per_sec) * params.counts_per_revolution / 360.0;
        double preset = static_cast<double>(params.timer_frequency) / counts_per_sec;
        preset = std::clamp(preset, 1.0, static_cast<double>(kCountsMask));
        return static_cast<uint32_t>(std::lround(preset));
    }

    // Confirm a live ":I" step-period change on a RUNNING axis actually took
    // effect, and re-kick with ":I" + ":J" if not. Found 2026-09-06: ConformU
    // "PulseGuide East ... RA change 0.00, expected 2.51s" with the ":i"
    // readback matching what was written (6b4988b's fix did not help) and
    // count-sampling showing the axis held exactly sidereal through the
    // whole pulse — the board stored the preset but never applied it to the
    // spinning motor. Not reproducible in isolation; a bare ":I" is the
    // common thread across the three live-rate-change call sites (PulseGuide
    // dispatch/restore, RightAscensionRate). Must run with mutex_ NOT held
    // (see stop_axis_and_wait_locked) — it sleeps across the sample window,
    // via task_wait_for on the OWNING task's cancel flag (pulse task or the
    // one-shot rate-verify task) so a reap aborts it promptly instead of
    // stalling teardown.
    // Did a live step-period change take? Classify the observed rate by which
    // of the two commanded rates it is closer to, so the check stays
    // discriminating however small the change: a guide rate of 0.1x sidereal
    // moves the RA rate by only 10%, well inside any fixed fractional
    // tolerance (fork PR #6 review). Strict "closer to the new rate" -- a tie
    // or a tiny delta falls on the "did not take" side, because a spurious
    // re-kick costs one redundant ":I"+":J" at the rate the axis is already
    // meant to run at, while a missed stall costs the whole pulse.
    static bool live_rate_change_took(double observed_cps, double previous_cps, double expected_cps) {
        const double o = std::abs(observed_cps);
        return std::abs(o - std::abs(expected_cps)) < std::abs(o - std::abs(previous_cps));
    }

    // max_window bounds how far the sample window may stretch to resolve a
    // small rate delta (see kRateVerifyMaxWindow above for why the pulse
    // dispatch call passes something tighter than the default).
    void verify_live_rate_or_rekick(int channel, double previous_rate_deg_per_sec, double expected_rate_deg_per_sec,
                                    std::atomic<bool>& cancel,
                                    std::chrono::milliseconds max_window = kRateVerifyMaxWindow) {
        if (previous_rate_deg_per_sec == expected_rate_deg_per_sec) {
            return;  // nothing changed, nothing to verify
        }
        auto& protocol = *protocol_;
        const AxisParameters& params = axis_params_[static_cast<std::size_t>(channel - 1)];
        const double expected_counts_per_sec =
            std::abs(expected_rate_deg_per_sec) * params.counts_per_revolution / 360.0;
        const double previous_counts_per_sec =
            std::abs(previous_rate_deg_per_sec) * params.counts_per_revolution / 360.0;
        // ":j" is whole counts, and each of the two reads truncates, so the
        // sampled delta carries up to ~2 counts of error. The window must be
        // long enough for the two candidate rates to sit at least
        // kMinResolvableDeltaCounts apart, or the nearest-rate verdict is a
        // coin flip: on the EQM-35 Pro (~107 counts/s sidereal) a 300 ms
        // window resolves a 0.5 s/s RA offset (16 counts) fine, but Lunar is
        // 3.5% off sidereal (1.1 counts) and Solar 0.27% (0.09) -- seen on
        // hardware 2026-09-10 as a spurious "did not take" + resend on a
        // TrackingRate=Lunar write. Stretch the window as far as needed, up
        // to max_window; past that the change is below what this check can
        // resolve within its budget, so leave it to the ":J" kick alone.
        const auto effective_max_window = std::min(max_window, kRateVerifyMaxWindow);
        constexpr double kMinResolvableDeltaCounts = 4.0;
        const double delta_counts_per_sec = std::abs(expected_counts_per_sec - previous_counts_per_sec);
        const double needed_s = delta_counts_per_sec > 0.0 ? kMinResolvableDeltaCounts / delta_counts_per_sec : 1e9;
        if (needed_s > std::chrono::duration<double>(effective_max_window).count()) {
            ALPACA_LOG_INFO("SkyWatcher", "Axis " + std::to_string(channel) + " rate change of " +
                                              std::to_string(delta_counts_per_sec) +
                                              " counts/s is below the rate-applied check's resolution; relying "
                                              "on the :J re-latch alone");
            return;
        }
        const auto window =
            std::clamp(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<double>(needed_s)),
                       kRateVerifyMinWindow, effective_max_window);
        uint32_t before = 0;
        uint32_t after = 0;
        try {
            if (!task_wait_for(kRateVerifySettle, cancel)) {
                return;  // cancelled by a reaper — it owns the axis now
            }
            before = protocol.inquire_position(channel);
            if (!task_wait_for(window, cancel)) {
                return;
            }
            after = protocol.inquire_position(channel);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("SkyWatcher", "Rate-applied check on axis " + std::to_string(channel) +
                                              " could not read position: " + e.what());
            return;
        }
        const double window_s = std::chrono::duration<double>(window).count();
        // ":j" is a 24-bit counter: take the delta modulo 2^24 and re-sign it
        // so a wrap inside the sample window reads as the few hundred counts
        // it was, not as +/-16 million (fork PR #6 review).
        int32_t delta = static_cast<int32_t>((after - before) & kCountsMask);
        if (delta > static_cast<int32_t>(kCountsMask >> 1)) {
            delta -= static_cast<int32_t>(kCountsMask) + 1;
        }
        const double observed_counts_per_sec = static_cast<double>(delta) / window_s;
        // Not a rate measurement: this only has to tell "changed speed" from
        // "still at the old rate". Nearest-of-the-two keeps that distinction
        // sharp at small guide rates, where a fixed fraction of the expected
        // rate would swallow the whole difference (see live_rate_change_took).
        if (live_rate_change_took(observed_counts_per_sec, previous_counts_per_sec, expected_counts_per_sec)) {
            return;
        }
        // Board state is diagnostics only: if ":f" itself fails, still emit the
        // warning (with the reason in place of the flags) and still re-kick --
        // the axis running at the wrong rate is the thing that matters.
        std::string board_state;
        try {
            const AxisStatus status = protocol.inquire_status(channel);
            board_state =
                "running=" + std::to_string(status.running) + ", speed_mode=" + std::to_string(status.speed_mode);
        } catch (const std::exception& e) {
            board_state = std::string("status read failed: ") + e.what();
        }
        ALPACA_LOG_WARN("SkyWatcher",
                        "Axis " + std::to_string(channel) + " step-period change did not take: observed " +
                            std::to_string(observed_counts_per_sec) + " counts/s, expected " +
                            std::to_string(expected_counts_per_sec) + " (was " +
                            std::to_string(previous_counts_per_sec) + "; " + board_state + "); resending :I and :J");
        try {
            // Same preset the dispatch computed (slow mode: a live change never
            // switches speed mode). step_period_for_locked only reads
            // axis_params_, immutable after connect, so it is safe unlocked.
            protocol.set_step_period(channel, step_period_for_locked(channel, expected_rate_deg_per_sec, false));
            protocol.start_motion(channel);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("SkyWatcher", "Resend after rate-applied check failed on axis " + std::to_string(channel) +
                                              ": " + e.what());
        }
    }

    // One-shot background rate-applied check for a live in-place RA
    // step-period change made from a PROPERTY setter (RightAscensionRate,
    // TrackingRate -> apply_ra_tracking_rate_locked). The pulse path runs the
    // check inside its own task; a setter has no task and must answer inside
    // the property response target, so it cannot sit in the ~450 ms sample
    // window itself. Unlike a pulse, a stall here has no natural end point: a
    // silently unapplied RightAscensionRate leaves RA tracking at the wrong
    // rate until the next rate change (open-astro/AlpacaBridge#248).
    //
    // Ownership follows the pulse task's discipline, with one difference:
    // the task NEVER takes mutex_, so it is reaped (cancel + join) WITH
    // mutex_ held by every path that takes the RA axis -- the setters
    // themselves, Tracking off, stop_axis_and_wait_locked (goto/park/home/
    // MoveAxis/duty bursts), the pulse dispatch and AbortSlew -- and by
    // disconnect. Reaping under the lock is what closes the race a lock-free
    // reap would leave: a setter spawning between an initiator's reap and
    // its lock, whose re-kick would then land mid-pulse or on a stopped axis.
    // Called with mutex_ held, after reap_rate_verify_task() on the same
    // lock hold, so the handle is never joinable here.
    void spawn_rate_verify_task_locked(double previous_rate_deg_per_sec, double expected_rate_deg_per_sec) {
        if (previous_rate_deg_per_sec == expected_rate_deg_per_sec) {
            return;
        }
        std::lock_guard<std::mutex> tlock(task_mutex_);
        rate_verify_thread_ = std::thread([this, previous_rate_deg_per_sec, expected_rate_deg_per_sec]() {
            try {
                verify_live_rate_or_rekick(kAxisRa, previous_rate_deg_per_sec, expected_rate_deg_per_sec,
                                           rate_verify_cancel_);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("SkyWatcher", std::string("RA rate-applied check failed: ") + e.what());
            } catch (...) {
                ALPACA_LOG_WARN("SkyWatcher", "RA rate-applied check failed with unknown exception");
            }
        });
    }

    // Direction char for ":G": '0' = increasing counts, '1' = decreasing.
    // Positive axis rates (increasing axis angle) map to increasing counts.
    static char direction_char(double signed_rate) { return signed_rate >= 0.0 ? '0' : '1'; }

    // Stop one axis and poll until the controller reports it stationary,
    // RELEASING the mutex around every sleep (issue #212): the ramp-down can
    // take over a second, and holding mutex_ through it blocked every
    // concurrent Alpaca GET. The motion generation guards the unlock windows:
    // if another command claims the axes while we slept, the wait is no
    // longer ours to finish.
    // Returns true when the axis stopped and the caller's motion command is
    // still the current one; false when the wait was superseded (a newer
    // motion command bumped the generation — AbortSlew included — or the
    // slew task was cancelled) — the caller MUST NOT issue further motor
    // commands for its now-stale operation (PR #216 review: an AbortSlew
    // landing in the unlock window otherwise saw its goto re-dispatched).
    [[nodiscard]] bool stop_axis_and_wait_locked(std::unique_lock<std::mutex>& lock, int channel, uint64_t gen) {
        // Never emit a stop for a STALE generation (PR #216 round-6): when a
        // dispatch's first axis wait was superseded, a newer command may have
        // legitimately started motion on the second axis — a stale stop here
        // would silently kill it while its bookkeeping says it is running.
        // The newer generation owns the axes now, whatever their state.
        if (motion_generation_ != gen) {
            return false;
        }
        if (channel == kAxisRa) {
            // Whoever stops RA owns it from here: a rate-applied check still
            // sampling the tracking rate must not resend it after the stop.
            reap_rate_verify_task();
        }
        auto& protocol = *protocol_;
        cmd_axis_rate_deg_s_[channel - 1] = 0.0;
        protocol.stop_motion(channel);
        auto deadline = std::chrono::steady_clock::now() + kAxisStopTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            // Generation only: slew_task_cancel_ stays stale-true between an
            // AbortSlew and the next reap, and must not poison unrelated
            // commands (ConformU: Tracking Write failed "Motion superseded").
            // AbortSlew bumps the generation, which is the supersession signal.
            if (motion_generation_ != gen) {
                return false;
            }
            AxisStatus status = protocol.inquire_status(channel);
            if (!status.running) {
                return true;
            }
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            lock.lock();
            check_connected();
        }
        throw AlpacaException("Timed out waiting for axis " + std::to_string(channel) + " to stop");
    }

    // Start speed-mode motion at a signed rate on one axis (assumes the caller
    // wants the axis re-commanded from stopped).
    void start_speed_motion_locked(std::unique_lock<std::mutex>& lock, int channel, double signed_rate_deg_per_sec) {
        auto& protocol = *protocol_;
        const uint64_t gen = ++motion_generation_;
        if (!stop_axis_and_wait_locked(lock, channel, gen)) {
            throw AlpacaException("Motion superseded before dispatch");
        }
        const bool fast = std::abs(signed_rate_deg_per_sec) > kFastModeThresholdDegPerSec;
        // Motion mode: '1' = speed slow, '3' = speed fast.
        // No hemisphere handling HERE: this is the mechanical layer, and a
        // signed axis rate means the same thing everywhere (MoveAxis and
        // AutoHome depend on that). The hemisphere's RA sense for tracking,
        // RightAscensionRate and East/West pulses is applied by the callers
        // through ra_axis_sign_locked() -- see effective_ra_rate_locked() and
        // the pointing-model comment. (#250 had removed a flip from this
        // function on the strength of the old model's reported RA; #432 put
        // the southern reversal back where the sky frame is built.)
        const double rate = signed_rate_deg_per_sec;
        protocol.set_motion_mode(channel, fast ? '3' : '1', direction_char(rate));
        protocol.set_step_period(channel, step_period_for_locked(channel, rate, fast));
        protocol.start_motion(channel);
        cmd_axis_rate_deg_s_[channel - 1] = rate;
    }

    // Base drive rate for the selected ASCOM DriveRate.
    double base_tracking_rate_locked() const {
        switch (tracking_rate_) {
            case 1:
                return kLunarDegPerSec;
            case 2:
                return kSolarDegPerSec;
            default:
                return kSiderealDegPerSec;
        }
    }

    // RA drive rate with the RightAscensionRate offset folded in, as a SIGNED
    // AXIS rate. Positive offset = RA increasing = the axis advancing SLOWER
    // in the tracking direction (RA = LST - HA) -> subtract, then apply the
    // hemisphere's axis sense so the sky hour angle increases (south of the
    // equator tracking runs the counts DOWN).
    double effective_ra_rate_locked() const {
        return ra_axis_sign_locked() *
               (base_tracking_rate_locked() - ra_rate_sec_per_sidereal_sec_ * kRaRateSecondsToDegPerSec);
    }

    // Slowest achievable slow-mode rate (":I" clamps at 0xFFFFFF): ~0.26
    // arcsec/s on the Wave 100i. Sub-floor Dec offsets are duty-cycled.
    double slow_mode_floor_rate_locked(int channel) const {
        const AxisParameters& params = axis_params_[channel - 1];
        return static_cast<double>(params.timer_frequency) * 360.0 /
               (static_cast<double>(params.counts_per_revolution) * static_cast<double>(kCountsMask));
    }

    // Start/stop/duty the Dec-axis offset motion for DeclinationRate. Sign:
    // dec = 90 - a2 on the east-pointing branch (a2 >= 0) -> +Dec is NEGATIVE
    // axis motion there (ConformU 4.5 measured-rate confirmed). That is the
    // NORTHERN-hemisphere formula: compute_ra_dec_locked() negates dec below
    // the equator (dec_sky = -(90 - a2) = a2 - 90 on the same branch), which
    // flips the sign of d(dec_sky)/d(a2) as well. The plain `a2 >= 0` rule
    // never consulted hemisphere_south_locked(), so south of the equator it
    // drove the axis backwards on BOTH branches -- the same class of bug
    // already found and fixed for RA tracking (48afe0d,
    // start_speed_motion_locked). Found by static review 2026-09-06 and
    // pinned by a loopback regression that asserts the driver's own reported
    // Dec rises under +DeclinationRate / a North pulse; not yet measured on
    // hardware below the equator (a guiding session or plate-solved drift
    // run would do it). XOR-ing the branch test with the hemisphere is the
    // full fix: MoveAxis, which applies no sign transform at all, is the
    // hardware-observed reference for which way a raw axis rate moves
    // reported Dec (.github/instructions/skywatcher.instructions.md, EQM-35 Pro at latitude -37.2).
    // The sign is evaluated once at (re)apply time and held: it is NOT
    // re-evaluated as this offset's own motion carries the axis across the
    // branch boundary (a2 through 0). This was tracked as a bug for a while
    // (open-astro#255, deferred from #214) and even implemented -- and the
    // "fix" made the axis reverse and oscillate right at the crossing,
    // breaking the "DeclinationRate drives Dec with the east-branch sign"
    // loopback test, which asserts a HELD sign for exactly this case.
    // a2 = 0 is the celestial pole in this driver's convention (dec = 90 at
    // a2 = 0 on both branches): continuing a raw axis motion through it
    // necessarily produces a real cusp in reported Dec (it rises to 90 then
    // falls), on any correctly-behaving mount -- confirmed on hardware during
    // the EQM-35 Pro bring-up ("at a2 = 0, reported Dec rises for EITHER
    // mechanical direction", see .github/instructions/skywatcher.instructions.md). Holding the sign is what
    // produces that correct cusp; dynamically flipping it to keep reported
    // Dec monotonic would be fighting the mount's own geometry. Closed as
    // not a bug: see open-astro#255 for the full derivation. Every real
    // re-apply trigger (goto completion, sync, pulse end, MoveAxis stop,
    // tracking toggle, a rate write) already re-reads the axis position
    // fresh, which is the only case that ever needed covering.
    void apply_dec_rate_offset_locked(std::unique_lock<std::mutex>& lock, bool defer_motion = false) {
        double rate = dec_rate_arcsec_per_sec_ / 3600.0;
        if (rate == 0.0) {
            dec_duty_rate_deg_s_ = 0.0;
            if (dec_offset_running_ && !defer_motion) {
                const uint64_t gen = ++motion_generation_;
                static_cast<void>(stop_axis_and_wait_locked(lock, kAxisDec, gen));
            }
            dec_offset_running_ = false;
            return;
        }
        refresh_position_cache_locked(false);
        if ((branch_from_axis_locked(cached_dec_axis_deg_) > 0) != hemisphere_south_locked()) {
            rate = -rate;
        }
        // Rates within kSlowModeFloorPad of the floor still duty-cycle: an
        // ":I" value pinned at 0xFFFFFF cannot resolve them continuously.
        double floor_rate = slow_mode_floor_rate_locked(kAxisDec) * kSlowModeFloorPad;
        if (std::abs(rate) >= floor_rate) {
            dec_duty_rate_deg_s_ = 0.0;
            if (defer_motion) {
                // Continuous motion starts when the busy operation's restore
                // path re-applies (restore_tracking_after_slew_/pulse end/
                // MoveAxis stop all funnel through here without defer).
                dec_offset_running_ = false;
                return;
            }
            start_speed_motion_locked(lock, kAxisDec, rate);
        } else {
            if (dec_offset_running_ && !defer_motion) {
                const uint64_t gen = ++motion_generation_;
                static_cast<void>(stop_axis_and_wait_locked(lock, kAxisDec, gen));
            }
            dec_duty_rate_deg_s_ = rate;
            if (defer_motion) {
                // The duty worker's go-gate idles until the axes are free;
                // do not touch cmd_axis_rate_deg_s_ mid-goto.
                dec_offset_running_ = false;
                return;
            }
            cmd_axis_rate_deg_s_[1] = rate;  // dead-reckon the requested average
        }
        dec_offset_running_ = true;
    }

    // Duty-cycle worker for sub-floor Dec rates: floor-rate bursts sized to
    // the requested average. Exits when the duty rate returns to zero; idles
    // while tracking is off or a slew/park/home owns the axes.
    // Duty-cycle worker for sub-floor offset rates on EITHER axis: floor-rate
    // bursts sized so the average matches the requested rate, one interleaved
    // state machine per axis on a 50 ms tick (the axes' bursts overlap freely
    // — a near-stationary satellite can need both at once). Exits when both
    // duty rates return to zero; idles an axis while tracking is off or a
    // slew/park/home/pulse/manual motion owns the axes.
    void duty_loop() {
        constexpr auto kDutyPeriod = std::chrono::milliseconds(3000);
        struct AxisDuty {
            bool bursting = false;
            uint64_t gen = 0;
            double rate = 0.0;  // the duty rate this burst was sized for
            std::chrono::steady_clock::time_point burst_end{};
            std::chrono::steady_clock::time_point next_start = std::chrono::steady_clock::time_point::min();
        };
        AxisDuty ax[2];
        while (!duty_cancel_.load()) {
            bool any_rate = false;
            try {
                std::unique_lock<std::mutex> lock(mutex_);
                for (int i = 0; i < 2; ++i) {
                    const int channel = i + 1;
                    double rate = duty_rate_locked(channel);
                    if (rate != 0.0) {
                        any_rate = true;
                    }
                    auto now = std::chrono::steady_clock::now();
                    if (ax[i].bursting) {
                        if (rate == ax[i].rate && now < ax[i].burst_end) {
                            continue;
                        }
                        // Ownership is judged PER AXIS: a goto/park/home owns
                        // both axes, a pulse or manual MoveAxis owns only its
                        // own. The global generation cannot tell a same-axis
                        // supersession from an unrelated other-axis command,
                        // so a stale generation with no same-axis owner means
                        // the burst still runs and must be stopped (with a
                        // fresh generation) rather than left creeping at the
                        // floor rate.
                        bool same_axis_owner = goto_in_progress_ || parking_ || homing_ || slewing_cached_ ||
                                               manual_axis_slewing_[i] || pulse_axis_active_[i];
                        if (rate == 0.0 || (motion_generation_ != ax[i].gen && same_axis_owner)) {
                            // Zeroed by its owner, or a same-axis command took
                            // the axis: nothing left for this burst to stop.
                            ax[i].bursting = false;
                        } else if (motion_generation_ == ax[i].gen && same_axis_owner) {
                            // Same-axis operation is dispatching this tick;
                            // it will supersede momentarily. Retry.
                        } else {
                            uint64_t stop_gen = ax[i].gen;
                            if (motion_generation_ != ax[i].gen) {
                                stop_gen = ++motion_generation_;  // cross-axis bump: still ours
                            }
                            static_cast<void>(stop_axis_and_wait_locked(lock, channel, stop_gen));
                            if (duty_rate_locked(channel) == ax[i].rate) {
                                cmd_axis_rate_deg_s_[i] = ax[i].rate;  // still the average rate
                            }
                            ax[i].bursting = false;
                        }
                    } else if (rate != 0.0 && now >= ax[i].next_start && connected_ && tracking_ &&
                               !axes_busy_locked() && !restoring_tracking_) {
                        // restoring_tracking_ is in this gate and NOT in
                        // axes_busy_locked() on purpose: a burst START here
                        // would bump motion_generation_ under the post-slew
                        // rate check's supersession guard and make it skip,
                        // while a rate SETTER in the same window must go
                        // through. The burst END above is not gated: a burst
                        // already running when the restore began still stops
                        // with a cross-axis bump, and the rate check then
                        // logs a skip that names a newer motion command. It
                        // is visible in the log, not closed.
                        // ~140 ms stop-landing overrun measured on hardware;
                        // shorten the wait so the physical on-duration matches
                        // the duty fraction. The RAW floor is correct here
                        // (the burst physically runs at it); rates inside the
                        // kSlowModeFloorPad margin just compute an on-time
                        // near/above the period and become continuous.
                        double floor_rate = slow_mode_floor_rate_locked(channel);
                        auto on_time = std::chrono::milliseconds(
                            std::clamp(static_cast<int>(3000.0 * std::abs(rate) / floor_rate) - 140, 50, 3000));
                        start_speed_motion_locked(lock, channel, rate > 0.0 ? floor_rate : -floor_rate);
                        ax[i].gen = motion_generation_;  // owned by THIS burst
                        ax[i].rate = rate;
                        ax[i].bursting = true;
                        ax[i].burst_end = std::chrono::steady_clock::now() + on_time;
                        ax[i].next_start = std::chrono::steady_clock::now() + kDutyPeriod;
                        cmd_axis_rate_deg_s_[i] = rate;
                    }
                }
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                // Superseded or transport hiccup; next tick re-evaluates.
            }
            if (!any_rate) {
                break;
            }
            if (!task_wait_for(std::chrono::milliseconds(50), duty_cancel_)) {
                break;
            }
        }
        // Never leave an axis creeping on exit. A zeroed duty rate means
        // whoever cleared it already stopped the axis; only a cancel with the
        // rate still set (disconnect mid-burst) needs the safety stop.
        try {
            std::unique_lock<std::mutex> lock(mutex_);
            for (int channel = 1; channel <= 2; ++channel) {
                if (duty_rate_locked(channel) != 0.0 && connected_) {
                    const uint64_t gen = ++motion_generation_;
                    static_cast<void>(stop_axis_and_wait_locked(lock, channel, gen));
                }
            }
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
    }

    double duty_rate_locked(int channel) const {
        return channel == kAxisRa ? ra_duty_rate_deg_s_ : dec_duty_rate_deg_s_;
    }

    void reap_duty_task() {
        std::lock_guard<std::mutex> lifecycle(duty_lifecycle_mutex_);
        reap_duty_locked_lifecycle();
    }

    // Requires duty_lifecycle_mutex_. Moves the thread slot out under
    // task_mutex_ and joins with only the lifecycle mutex held.
    void reap_duty_locked_lifecycle() {
        duty_cancel_.store(true);
        task_cv_.notify_all();
        std::thread prev;
        {
            std::lock_guard<std::mutex> tlock(task_mutex_);
            prev = std::move(duty_thread_);
        }
        if (prev.joinable()) {
            prev.join();
        }
        duty_cancel_.store(false);
    }

    // Re-command the RA axis after a rate change. In-place step-period writes
    // are only legal while the direction is unchanged; a sign flip (or a
    // stopped/reversed axis) needs a full stop-and-restart.
    // Drive the RA axis at the current effective rate, handling all three
    // regimes: continuous speed motion at/above the slow-mode floor, a
    // duty-cycled sub-floor rate (":I" clamps at 0xFFFFFF — issuing a
    // sub-floor rate directly would silently creep at the floor rate), and
    // an exact zero (offset cancels the drive: the axis must STOP, not
    // creep). The duty worker must be running when this sets a duty rate —
    // callers check duty rates after and start it outside the mutex.
    void apply_ra_drive_locked(std::unique_lock<std::mutex>& lock) {
        double eff = effective_ra_rate_locked();
        double floor_rate = slow_mode_floor_rate_locked(kAxisRa) * kSlowModeFloorPad;
        if (std::abs(eff) >= floor_rate) {
            ra_duty_rate_deg_s_ = 0.0;
            start_speed_motion_locked(lock, kAxisRa, eff);
            return;
        }
        const uint64_t gen = ++motion_generation_;
        static_cast<void>(stop_axis_and_wait_locked(lock, kAxisRa, gen));
        ra_duty_rate_deg_s_ = eff;  // 0.0 = stay stopped; else the worker bursts
        cmd_axis_rate_deg_s_[0] = eff;
    }

    void apply_ra_tracking_rate_locked(std::unique_lock<std::mutex>& lock, double previous_effective) {
        double eff = effective_ra_rate_locked();
        double floor_rate = slow_mode_floor_rate_locked(kAxisRa) * kSlowModeFloorPad;
        if (std::abs(eff) >= floor_rate && std::abs(previous_effective) >= floor_rate &&
            (eff > 0.0) == (previous_effective > 0.0)) {
            // Same direction, both continuous: change the step period in
            // place — the axis never stops. ":J" kick for the same reason
            // as the PulseGuide live-rate change (see
            // verify_live_rate_or_rekick): a bare ":I" here is not always
            // enough on this firmware. The sampled rate-applied check cannot
            // run here (synchronous under mutex_ from a property setter, and
            // it needs an unlocked ~450 ms window), so it runs as a one-shot
            // background task instead.
            if (eff == previous_effective) {
                // Nothing changed: no write, no blocking ":J" round-trip under
                // mutex_ (#249 review) -- and deliberately NO reap either: a
                // check still in flight for this very rate is still valid.
                // set_tracking_rate() has no idempotent-rewrite guard of its
                // own, so a client re-asserting the same TrackingRate lands
                // here mid-check; reaping it with nothing to replace it would
                // silently drop the one chance to catch a stalled ":I" (#258
                // review).
                return;
            }
            // A check still sampling an OLDER rate change would resend that
            // rate over this one; this write supersedes it (see
            // spawn_rate_verify_task_locked for why reaping under mutex_ is
            // safe).
            reap_rate_verify_task();
            auto& protocol = *protocol_;
            protocol.set_step_period(kAxisRa, tracking_step_period_for(eff));
            protocol.start_motion(kAxisRa);
            cmd_axis_rate_deg_s_[0] = eff;  // keep dead reckoning on the new rate
            spawn_rate_verify_task_locked(previous_effective, eff);
        } else {
            // Stop-and-restart path: the axis is about to stop, so any
            // pending check must go first (its resend would restart it).
            reap_rate_verify_task();
            apply_ra_drive_locked(lock);
        }
    }

    void set_tracking_locked(std::unique_lock<std::mutex>& lock, bool tracking) {
        // Tracking off stops the RA axis: a pending rate-applied check must
        // not resend its ":I"+":J" into the stopped axis and restart it.
        reap_rate_verify_task();
        if (tracking) {
            // Track: RA axis in the direction of increasing SKY hour angle
            // (increasing counts north of the equator, decreasing south of
            // it -- effective_ra_rate_locked() carries the sign) at the
            // selected drive rate plus any RightAscensionRate offset.
            apply_ra_drive_locked(lock);
            apply_dec_rate_offset_locked(lock);
        } else {
            const uint64_t gen = ++motion_generation_;
            if (!stop_axis_and_wait_locked(lock, kAxisRa, gen)) {
                // A newer motion command took the axes while the mutex was
                // released: it owns the tracking state now — do not stomp it.
                throw AlpacaException("Tracking change superseded by a concurrent motion command");
            }
            if (dec_offset_running_) {
                const uint64_t dgen = ++motion_generation_;
                static_cast<void>(stop_axis_and_wait_locked(lock, kAxisDec, dgen));
                dec_offset_running_ = false;
            }
            // Let the duty worker exit while tracking is off; re-enabling
            // tracking recomputes the duty rates and restarts it. The RA stop
            // above already halted the axis for a duty-regime RA offset.
            ra_duty_rate_deg_s_ = 0.0;
            dec_duty_rate_deg_s_ = 0.0;
        }
        tracking_ = tracking;
        invalidate_position_cache_locked();
    }

    void dispatch_goto_locked(std::unique_lock<std::mutex>& lock, double target_ra_axis_deg,
                              double target_dec_axis_deg) {
        auto& protocol = *protocol_;
        const uint64_t gen = ++motion_generation_;
        cmd_axis_rate_deg_s_[0] = 0.0;
        cmd_axis_rate_deg_s_[1] = 0.0;
        // A single generation spans BOTH stop-waits and the goto commands
        // below: if either wait is superseded (AbortSlew bumps the
        // generation too), the whole dispatch aborts before any new motor
        // command is sent.
        // Evaluate BOTH waits unconditionally (round-4 finding: a short-
        // circuit skipped the second axis entirely) — while the stale-
        // generation gate inside the wait ensures a superseded dispatch
        // never emits stops that could clobber the superseding command's
        // fresh motion (round-6 finding).
        const bool ra_stopped = stop_axis_and_wait_locked(lock, kAxisRa, gen);
        const bool dec_stopped = stop_axis_and_wait_locked(lock, kAxisDec, gen);
        if (!ra_stopped || !dec_stopped) {
            throw AlpacaException("Slew superseded before dispatch");
        }
        refresh_position_cache_locked(true);

        struct AxisGoto {
            int channel;
            double current_deg;
            double target_deg;
        };
        const AxisGoto plans[2] = {
            {kAxisRa, cached_ra_axis_deg_, target_ra_axis_deg},
            {kAxisDec, cached_dec_axis_deg_, target_dec_axis_deg},
        };
        for (const auto& plan : plans) {
            double delta = plan.target_deg - plan.current_deg;
            // Motion mode '0' = fast GOTO (the controller manages ramp and the
            // brake point); direction from the signed move.
            protocol.set_motion_mode(plan.channel, '0', direction_char(delta));
            protocol.set_goto_target(
                plan.channel,
                degrees_to_counts(plan.target_deg,
                                  axis_params_[static_cast<std::size_t>(plan.channel - 1)].counts_per_revolution));
        }
        protocol.start_motion(kAxisRa);
        protocol.start_motion(kAxisDec);
        // open-astro#459: every commanded dec-axis angle passes through here
        // (sky goto, the no-indexer FindHome and Park gotos to home, the
        // AutoHome hunt phases), so this is the one place the branch memory
        // is set for a goto. AFTER the motor commands: a dispatch that threw
        // above must not leave the memory claiming a branch the mount never
        // moved to (a mount sitting at the pole would flip its reported RA
        // by 12 h without moving).
        remember_command_branch_locked(target_dec_axis_deg);
    }

    // LST advances 24 sidereal hours per sidereal day of SI seconds.
    static constexpr double kLstHoursPerSecond = 24.0 / 86164.0905;
    // Fixed goto overhead beyond distance/max-rate (ramp up + down, the
    // pre-dispatch stop-wait, landing detection). INITIAL estimate only: an
    // EQM-35 Pro takes ~3.1 s for even a 350-count refinement goto and
    // ~3.7 s of overhead on a 15 deg one, so goto_overhead_seconds_ is
    // measured on every goto (see wait_for_slew_complete). With this fixed
    // at 2.5 s each refinement landed ~0.6 s late and the loop never
    // converged (ConformU SlewToCoordinates "10.8 arc seconds away").
    static constexpr double kGotoRampSeconds = 2.5;
    // After the goto lands, the RA axis sits stopped while tracking restarts;
    // aim that far ahead so the drift-back lands ON target. 0.7 s is the
    // Wave 100i over Wi-Fi and only the INITIAL estimate: the restart on an
    // EQM-35 Pro over USB takes ~0.2 s, and with 0.7 s assumed in BOTH the
    // aim and the landing deadband check a 20 deg goto whose duration
    // estimate ran 1.2 s long read as 6 arcsec off (inside the deadband,
    // no refinement) and then resumed tracking 1.03 s ahead of the sky --
    // ConformU SyncToCoordinates "15.4 arc seconds away from RA target"
    // (2026-09-12). resume_latency_seconds_ is measured landing-to-":J" on
    // every slew and replaces this constant after the first one.
    static constexpr double kTrackingResumeSeconds = 0.7;
    // Landing deadband per axis (~8 arcsec; ConformU checks RA to 10 arcsec).
    static constexpr double kLandingDeadbandDeg = 8.0 / 3600.0;

    // First goto aimed at the ARRIVAL-time sky position: estimate the slew
    // duration from the distance and advance the LST used for the axis
    // target. An uncompensated goto lands east by the slew duration
    // (~3 arcmin of RA for a long slew on the Wave 100i).
    void dispatch_predicted_goto_locked(std::unique_lock<std::mutex>& lock, double ra, double dec) {
        refresh_position_cache_locked(true);
        auto [p1, p2] = ra_dec_to_axis_degrees_locked(ra, dec);
        double dist = std::max(std::abs(p1 - cached_ra_axis_deg_), std::abs(p2 - cached_dec_axis_deg_));
        last_goto_dispatch_time_ = std::chrono::steady_clock::now();
        last_goto_dist_deg_ = dist;
        double est_seconds = dist / kMaxMoveAxisRateDegPerSec + goto_overhead_seconds_ + resume_latency_seconds_;
        auto [t1, t2] = ra_dec_to_axis_degrees_locked(ra, dec, est_seconds * kLstHoursPerSecond);
        dispatch_goto_locked(lock, t1, t2);  // records the branch of t2 once the goto is on its way
    }

    // After the first goto lands, close the residual (prediction error) with
    // short re-gotos until inside the deadband. Slewing is held true across
    // the inter-goto gaps via slew_force_until_.
    void refine_goto_landing(std::unique_lock<std::mutex>& lock, double ra, double dec) {
        for (int iter = 0; iter < 3; ++iter) {
            if (slew_task_cancel_.load()) {
                break;  // AbortSlew/unpark/disconnect cancelled the slew
            }
            slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            refresh_position_cache_locked(true);
            // Judge the landing against the target ADVANCED by the resume
            // window -- the mount deliberately lands ahead (see above).
            auto [n1, n2] = ra_dec_to_axis_degrees_locked(ra, dec, resume_latency_seconds_ * kLstHoursPerSecond);
            if (std::abs(n1 - cached_ra_axis_deg_) <= kLandingDeadbandDeg &&
                std::abs(n2 - cached_dec_axis_deg_) <= kLandingDeadbandDeg) {
                break;
            }
            slewing_cached_ = true;
            dispatch_predicted_goto_locked(lock, ra, dec);
            wait_for_slew_complete(lock);
        }
        slew_force_until_ = std::chrono::steady_clock::time_point::min();
        slewing_cached_ = false;
    }

    void do_slew_to_ra_dec_locked(std::unique_lock<std::mutex>& lock, double ra, double dec) {
        invalidate_position_cache_locked();
        slewing_cached_ = true;
        // open-astro#575: a fresh initiator is a clean start -- a client
        // that retries a rejected goto (even via the blocking
        // SlewToCoordinates) must not be told the OLD goto failed.
        clear_last_slew_error_locked();
        slew_force_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        restore_tracking_after_slew_ = tracking_;
        // open-astro#404: the target is what the client ASKED for, so it is
        // published before dispatch on every writer (this one, the async slew
        // and sync). A dispatch that fails does not un-ask it, and the three
        // paths now agree on what TargetRightAscension reads afterwards.
        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_ra_set_ = true;
        target_dec_set_ = true;
        try {
            dispatch_predicted_goto_locked(lock, ra, dec);
        } catch (...) {
            // Dispatch failed before the mount started moving: clear the
            // pre-published slew state so Slewing cannot wedge true.
            slewing_cached_ = false;
            slew_force_until_ = std::chrono::steady_clock::time_point::min();
            restore_tracking_after_slew_ = false;
            throw;
        }
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
        parked_ = false;
        at_home_ = false;
    }

    // Some controllers stop tracking during a GOTO and do not resume; always
    // re-issue tracking after a completed slew when it was on (project lesson).
    void restore_tracking_after_slew_locked(std::unique_lock<std::mutex>& lock) {
        if (restore_tracking_after_slew_) {
            restore_tracking_after_slew_ = false;
            try {
                set_tracking_locked(lock, true);
                // Landing -> ":J" latency of THIS restart, folded into the
                // aim-ahead/deadband estimate (see kTrackingResumeSeconds).
                if (last_landing_time_ != std::chrono::steady_clock::time_point{}) {
                    const double measured =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - last_landing_time_).count();
                    if (measured > 0.0 && measured < 5.0) {
                        resume_latency_seconds_ = std::clamp(0.5 * resume_latency_seconds_ + 0.5 * measured, 0.05, 2.0);
                    }
                }
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("SkyWatcher", std::string("Failed to restore tracking after slew: ") + e.what());
                return;
            }
            // Separate catch on purpose: tracking IS running by this point, so
            // logging "Failed to restore tracking" for a failed VERIFICATION
            // sends the reader looking for a restart that did happen.
            try {
                verify_post_slew_tracking_rate_locked(lock);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("SkyWatcher",
                                std::string("Post-slew tracking rate check skipped: tracking is running, but the check "
                                            "threw before it completed a measurement: ") +
                                    e.what());
            }
        }
    }

    // The RA axis restarted from a goto landing has been seen running at ~2x
    // the commanded period (see kLandingSettle). Sample the count rate over
    // kPostSlewRateWindow and, if it is not the commanded drive rate, stop the
    // axis, wait for it to settle and restart once. Continuous drive only: a
    // duty-cycled sub-floor rate has no steady rate to sample. Runs inside the
    // slew task with restoring_tracking_ set, so Slewing stays true while the
    // axes read free -- see that flag for why the two had to be separated.
    void verify_post_slew_tracking_rate_locked(std::unique_lock<std::mutex>& lock) {
        if (!tracking_ || ra_duty_rate_deg_s_ != 0.0) {
            ALPACA_LOG_INFO("SkyWatcher", std::string("Post-slew tracking rate check skipped: ") +
                                              (!tracking_ ? "tracking is off"
                                                          : "the RA drive is in the duty-cycled sub-floor regime"));
            return;
        }
        auto& protocol = *protocol_;
        const double expected_cps =
            std::abs(effective_ra_rate_locked()) * axis_params_[0].counts_per_revolution / 360.0;
        if (expected_cps <= 0.0) {
            ALPACA_LOG_INFO("SkyWatcher",
                            "Post-slew tracking rate check skipped: the effective RA rate is zero (the offset "
                            "cancels the drive), so the axis is meant to be stationary");
            return;
        }
        // ":j" is whole counts and both reads truncate, so a sample carries
        // up to ~2 counts of error -- the same limit verify_live_rate_or_rekick
        // reasons about above. If the window cannot accumulate enough counts
        // to tell the expected rate from its quantisation, every possible
        // reading lands outside the tolerance and the check condemns a
        // healthy axis: with RightAscensionRate near-cancelling sidereal
        // (the documented satellite/geostationary use), expected_cps can be
        // ~5, so a 300 ms window expects 1.48 counts and :j1 can only answer
        // 1 or 2 -- both outside 25%. That produced a stop, a restart, a
        // second failed sample and a "restart did not correct it" WARN on an
        // axis that was tracking correctly. Below the resolution floor there
        // is nothing to verify, so say so once and leave the axis alone.
        constexpr double kMinResolvableDeltaCounts = 4.0;
        const double expected_counts_in_window =
            expected_cps * std::chrono::duration<double>(kPostSlewRateWindow).count();
        if (expected_counts_in_window < kMinResolvableDeltaCounts) {
            ALPACA_LOG_INFO("SkyWatcher", "Post-slew tracking rate check skipped: expected " +
                                              std::to_string(expected_counts_in_window) +
                                              " counts in the sample window, below the " +
                                              std::to_string(kMinResolvableDeltaCounts) + " needed to resolve it");
            return;
        }
        // Everything above is a snapshot taken under the lock, and the sleeps
        // below drop it for kRateVerifySettle + kPostSlewRateWindow. Whatever
        // claims the RA axis in that window -- Tracking off, MoveAxis, a
        // RightAscensionRate write -- owns it afterwards, so re-validate on
        // every re-lock rather than acting on the stale snapshot. Without
        // this, a client writing Tracking = false during the settle sleep got
        // delta == 0 measured against the old expected_cps, a bogus "running
        // at 0 counts/s" WARN, and then apply_ra_drive_locked() driving the
        // axis at sidereal while the Tracking property reported False.
        // The generation is captured HERE, before the first unlock: taking it
        // after the window (as ++motion_generation_ did) cannot detect a
        // command that landed inside the window, which is the only thing it
        // needed to detect.
        //
        // NOT const, and re-seeded after the attempt-0 recovery below: that
        // recovery is itself a motion command (its own ++motion_generation_,
        // and start_speed_motion_locked() bumps it again), so leaving the
        // entry value in place made attempt 1's first sleep_unlocked() read
        // its OWN restart as somebody else's supersession and return. The
        // second sample was unreachable, "restart did not correct it" could
        // never be emitted, and a restart that also latched wrong ran at the
        // wrong rate for the rest of the session in silence -- the #432
        // symptom, from inside the check meant to catch it (round-1 review).
        uint64_t entry_generation = motion_generation_;
        auto sleep_unlocked = [&](std::chrono::milliseconds d) {
            lock.unlock();
            std::this_thread::sleep_for(d);
            lock.lock();
            check_connected();
            if (slew_task_cancel_.load()) {
                ALPACA_LOG_INFO("SkyWatcher", "Post-slew tracking rate check skipped: slew cancelled");
                return false;
            }
            // Superseded, or the client stopped tracking: not ours any more.
            if (motion_generation_ != entry_generation || !tracking_ || ra_duty_rate_deg_s_ != 0.0) {
                // .github/instructions/skywatcher.instructions.md tells the reader to grep for "rate check skipped"
                // when a slew was never verified (review note on #448). Every
                // exit that does NOT complete a measurement says so -- this
                // one, the entry guards, the zero-rate and zero-interval
                // guards, the three exits inside the attempt-0 recovery
                // (including the restart throwing), the catch around the
                // position reads, and the catch in the caller that wraps
                // the whole check. A
                // check that ran and found the rate correct is deliberately
                // silent: it is the ordinary case, once per goto, and the
                // grep is for slews that were never verified.
                ALPACA_LOG_INFO("SkyWatcher",
                                std::string("Post-slew tracking rate check skipped: ") +
                                    (motion_generation_ != entry_generation
                                         ? "a newer motion command took the RA axis"
                                         : (!tracking_ ? "tracking was turned off"
                                                       : "the RA drive moved to the duty-cycled regime")));
                return false;
            }
            return true;
        };
        for (int attempt = 0; attempt < 2; ++attempt) {
            uint32_t before = 0;
            uint32_t after = 0;
            // Measured, not nominal: the real gap between the two :j1 reads is
            // kPostSlewRateWindow plus the mutex_ re-acquisition plus a serial
            // round trip. Dividing by the nominal window biases observed_cps
            // low by that overhead, which under GET polling can approach the
            // 25% tolerance and stop-and-restart the RA axis after every slew.
            std::chrono::steady_clock::time_point sampled_at{};
            double elapsed = 0.0;
            try {
                if (!sleep_unlocked(kRateVerifySettle)) {
                    return;
                }
                before = protocol.inquire_position(kAxisRa);
                sampled_at = std::chrono::steady_clock::now();
                if (!sleep_unlocked(kPostSlewRateWindow)) {
                    return;
                }
                after = protocol.inquire_position(kAxisRa);
                elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - sampled_at).count();
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(
                    "SkyWatcher",
                    std::string("Post-slew tracking rate check skipped: could not read position: ") + e.what());
                return;
            }
            int32_t delta = static_cast<int32_t>((after - before) & kCountsMask);
            if (delta > static_cast<int32_t>(kCountsMask >> 1)) {
                delta -= static_cast<int32_t>(kCountsMask) + 1;
            }
            if (elapsed <= 0.0) {
                ALPACA_LOG_INFO("SkyWatcher",
                                "Post-slew tracking rate check skipped: the two position reads came back with no "
                                "measurable interval between them");
                return;
            }
            const double observed_cps = std::abs(static_cast<double>(delta)) / elapsed;
            if (std::abs(observed_cps - expected_cps) <= kPostSlewRateTolerance * expected_cps) {
                return;  // verified: the one exit that deliberately says nothing
            }
            ALPACA_LOG_WARN("SkyWatcher",
                            "Post-slew tracking restart: RA axis running at " + std::to_string(observed_cps) +
                                " counts/s, expected " + std::to_string(expected_cps) +
                                (attempt == 0 ? "; stopping and restarting tracking" : "; restart did not correct it"));
            if (attempt == 0) {
                // Re-checked immediately above by sleep_unlocked(); bump the
                // generation only now that we know the axis is still ours.
                const uint64_t gen = ++motion_generation_;
                if (!stop_axis_and_wait_locked(lock, kAxisRa, gen)) {
                    // A supersession exit, and it has to say so: the check has
                    // already stopped the RA axis by this point, and .github/instructions/skywatcher.instructions.md
                    // tells an operator to grep for "rate check skipped" when a
                    // slew was never verified (round-3 review).
                    ALPACA_LOG_INFO("SkyWatcher",
                                    "Post-slew tracking rate check skipped: a newer motion command took the RA "
                                    "axis during the restart, which now owns it");
                    return;
                }
                wait_axis_stationary_locked(lock, kAxisRa);
                if (!tracking_) {
                    // Do not restart the drive: the client turned tracking off
                    // while we waited for the axis to settle.
                    ALPACA_LOG_INFO("SkyWatcher",
                                    "Post-slew tracking rate check skipped: tracking was turned off while the axis "
                                    "settled, so the drive was not restarted");
                    return;
                }
                try {
                    apply_ra_drive_locked(lock);
                    // Our own restart is the baseline for attempt 1's
                    // supersession test, not the value captured on entry.
                    entry_generation = motion_generation_;
                } catch (const std::exception& e) {
                    // This path has already stopped the axis. Leaving
                    // tracking_ true would report Tracking on a mount whose
                    // RA axis is stationary, so report what is true and let
                    // the client decide to re-enable it.
                    tracking_ = false;
                    ALPACA_LOG_WARN("SkyWatcher",
                                    std::string("Post-slew tracking rate check skipped: the restart failed; RA axis "
                                                "left stopped and Tracking now reports false: ") +
                                        e.what());
                    return;
                }
            }
        }
    }

    bool get_slewing_locked() const {
        // A park in progress reports Slewing until AtPark flips in the same
        // locked step -- ConformU polls Slewing for park completion, and a
        // fast (localhost) poller caught the gap between the slew ending and
        // parked_ being set, declaring the park failed.
        if (parking_ || homing_ || goto_in_progress_ || restoring_tracking_) {
            return true;
        }
        return get_hardware_slewing_locked();
    }

    // Hardware/manual slewing state WITHOUT the parking_ override. The park
    // task's own wait_for_slew_complete must poll this variant: polling
    // get_slewing_locked() while parking_ is set can never see "stopped" and
    // times out at 180s (AtPark stays false -- ConformU Park failure).
    bool get_hardware_slewing_locked() const {
        if (manual_axis_slewing_[0] || manual_axis_slewing_[1]) {
            return true;
        }
        if (std::chrono::steady_clock::now() < slew_force_until_) {
            return true;
        }
        bool was_slewing = slewing_cached_;
        try {
            auto& protocol = *protocol_;
            AxisStatus ra = protocol.inquire_status(kAxisRa);
            AxisStatus dec = protocol.inquire_status(kAxisDec);
            // Trust the controller's status register: a GOTO is in progress
            // while either axis is running in GOTO mode. A tracking axis
            // (speed mode) is NOT slewing.
            slewing_cached_ = (ra.running && !ra.speed_mode) || (dec.running && !dec.speed_mode);
            last_slewing_poll_ = std::chrono::steady_clock::now();
        } catch (...) {
            // Keep last known state across a transient poll failure, but a
            // sustained fault must surface, not report frozen Slewing forever.
            if ((std::chrono::steady_clock::now() - last_slewing_poll_) > kStaleCacheLimit) {
                throw;
            }
        }
        if (was_slewing && !slewing_cached_) {
            invalidate_position_cache_locked();
        }
        return slewing_cached_;
    }

    // ── AutoHome (home index sensors) ───────────────────────────────────────
    // Port of the SynScan/EQMod AutoHome procedure (indi-eqmod eqmodbase.cpp).
    // The Wave's home index sensor latches the axis count when the axis sweeps
    // past the physical home mark. Reading the indexer (":q" data 0x000000)
    // returns 0 (armed, currently below the index), 0xFFFFFF (armed, above),
    // or the latched count; ":W" data 0x000008 re-arms it. The procedure hunts
    // the sensor edge on both axes, always makes the final approach from below
    // (consistent direction kills backlash), then re-stamps the position
    // registers to kHomeCounts at the sensed mark — re-anchoring the count
    // frame to the physical home regardless of where the mount was powered on.
    // TODO: Validate AutoHome direction conventions in the southern hemisphere.
    // (start_speed_motion_locked no longer flips the RA sign there -- see
    // 48afe0d -- so the hunt runs the same way in both hemispheres; the only
    // southern mount tested so far, the EQM-35 Pro, has no index sensors and
    // never reaches this code.)

    void autohome_sleep(std::unique_lock<std::mutex>& lock, std::chrono::milliseconds d) const {
        lock.unlock();
        std::this_thread::sleep_for(d);
        lock.lock();
        check_connected();
        if (slew_task_cancel_.load()) {
            throw AlpacaException("AutoHome cancelled");
        }
    }

    void autohome_wait_axes_stopped(std::unique_lock<std::mutex>& lock) const {
        auto& proto = *protocol_;
        const auto start = std::chrono::steady_clock::now();
        while (proto.inquire_status(kAxisRa).running || proto.inquire_status(kAxisDec).running) {
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(300)) {
                throw AlpacaException("AutoHome: axes did not stop");
            }
            autohome_sleep(lock, std::chrono::milliseconds(250));
        }
    }

    void run_autohome(std::unique_lock<std::mutex>& lock) {
        auto& proto = *protocol_;
        const int axes[2] = {kAxisRa, kAxisDec};
        auto read_idx = [&](int i) { return proto.get_feature(axes[i], kIndexerInquiry); };
        auto reset_idx = [&](int i) { proto.set_feature(axes[i], kIndexerReset); };
        auto axis_deg = [&](int i) { return i == 0 ? cached_ra_axis_deg_ : cached_dec_axis_deg_; };
        auto idx_to_deg = [&](int i, uint32_t counts) {
            return counts_to_degrees(counts, axis_params_[i].counts_per_revolution);
        };

        // Phase 1: stop everything, arm the indexers, pick directions from the
        // armed reading (0 = below the index -> move down first, away from it),
        // and step 5 degrees off so the edge is approached cleanly.
        ALPACA_LOG_INFO("SkyWatcher", "AutoHome phase 1: arming home indexers");
        {
            const uint64_t gen = ++motion_generation_;
            // Both waits evaluated unconditionally (see dispatch_goto_locked).
            const bool ra_stopped = stop_axis_and_wait_locked(lock, kAxisRa, gen);
            const bool dec_stopped = stop_axis_and_wait_locked(lock, kAxisDec, gen);
            if (!ra_stopped || !dec_stopped) {
                throw AlpacaException("AutoHome cancelled");
            }
        }
        tracking_ = false;
        bool up[2];
        for (int i = 0; i < 2; ++i) {
            reset_idx(i);
            up[i] = read_idx(i) == 0;
        }
        refresh_position_cache_locked(true);
        dispatch_goto_locked(lock, axis_deg(0) + (up[0] ? -5.0 : 5.0), axis_deg(1) + (up[1] ? -5.0 : 5.0));
        autohome_wait_axes_stopped(lock);

        // Phase 2: if the 5-degree step swept PAST the index (latched), move a
        // further 5 degrees in the same direction, then re-arm and re-read the
        // side (0 now means above -> hunt downward).
        bool swept[2];
        for (int i = 0; i < 2; ++i) {
            uint32_t v = read_idx(i);
            swept[i] = v != 0 && v != kIndexerAbove;
        }
        if (swept[0] || swept[1]) {
            ALPACA_LOG_INFO("SkyWatcher", "AutoHome phase 2: stepping past a latched index");
            refresh_position_cache_locked(true);
            dispatch_goto_locked(lock, axis_deg(0) + (swept[0] ? (up[0] ? -5.0 : 5.0) : 0.0),
                                 axis_deg(1) + (swept[1] ? (up[1] ? -5.0 : 5.0) : 0.0));
            autohome_wait_axes_stopped(lock);
            for (int i = 0; i < 2; ++i) {
                if (swept[i]) {
                    reset_idx(i);
                    up[i] = read_idx(i) != 0;
                }
            }
        }

        // Phase 3: any axis above the index hunts downward at the coarse rate
        // until the indexer reports the below side, runs 3 s further, stops,
        // and re-arms — every axis now sits below its index.
        if (!up[0] || !up[1]) {
            ALPACA_LOG_INFO("SkyWatcher", "AutoHome phase 3: coarse hunt below the index");
            bool hunting[2] = {!up[0], !up[1]};
            for (int i = 0; i < 2; ++i) {
                if (hunting[i]) {
                    start_speed_motion_locked(lock, axes[i], -kAutoHomeCoarseRateDegPerSec);
                }
            }
            std::chrono::steady_clock::time_point edge_at[2];
            bool edge[2] = {false, false};
            const auto start = std::chrono::steady_clock::now();
            while (hunting[0] || hunting[1]) {
                if (std::chrono::steady_clock::now() - start > std::chrono::seconds(300)) {
                    throw AlpacaException("AutoHome: coarse hunt timed out");
                }
                for (int i = 0; i < 2; ++i) {
                    if (!hunting[i]) continue;
                    if (!edge[i] && read_idx(i) != kIndexerAbove) {
                        edge[i] = true;
                        edge_at[i] = std::chrono::steady_clock::now();
                    }
                    if (edge[i] && std::chrono::steady_clock::now() - edge_at[i] > std::chrono::seconds(3)) {
                        {
                            const uint64_t gen = ++motion_generation_;
                            if (!stop_axis_and_wait_locked(lock, axes[i], gen)) {
                                throw AlpacaException("AutoHome cancelled");
                            }
                        }
                        reset_idx(i);
                        up[i] = true;
                        hunting[i] = false;
                    }
                }
                autohome_sleep(lock, std::chrono::milliseconds(200));
            }
        }

        // Phase 4: sweep upward at the detect rate; the indexer latches the
        // exact count of the home mark as each axis crosses it.
        ALPACA_LOG_INFO("SkyWatcher", "AutoHome phase 4: detecting the home index");
        uint32_t home_idx[2] = {0, 0};
        bool latched[2] = {false, false};
        start_speed_motion_locked(lock, kAxisRa, kAutoHomeDetectRateDegPerSec);
        start_speed_motion_locked(lock, kAxisDec, kAutoHomeDetectRateDegPerSec);
        const auto detect_start = std::chrono::steady_clock::now();
        while (!latched[0] || !latched[1]) {
            if (std::chrono::steady_clock::now() - detect_start > std::chrono::seconds(300)) {
                proto.stop_motion(kAxisRa);
                proto.stop_motion(kAxisDec);
                throw AlpacaException("AutoHome: index detect timed out");
            }
            for (int i = 0; i < 2; ++i) {
                if (latched[i]) continue;
                uint32_t v = read_idx(i);
                if (v != 0) {
                    home_idx[i] = v;
                    latched[i] = true;
                    {
                        const uint64_t gen = ++motion_generation_;
                        if (!stop_axis_and_wait_locked(lock, axes[i], gen)) {
                            throw AlpacaException("AutoHome cancelled");
                        }
                    }
                }
            }
            autohome_sleep(lock, std::chrono::milliseconds(150));
        }
        ALPACA_LOG_INFO("SkyWatcher", "AutoHome: index latched at RA=" + std::to_string(home_idx[0]) +
                                          " Dec=" + std::to_string(home_idx[1]));

        // Phase 5+6: back 10 degrees below the latched mark, then approach it
        // from below and stamp the position registers to the home offset.
        dispatch_goto_locked(lock, idx_to_deg(0, home_idx[0]) - 10.0, idx_to_deg(1, home_idx[1]) - 10.0);
        autohome_wait_axes_stopped(lock);
        dispatch_goto_locked(lock, idx_to_deg(0, home_idx[0]), idx_to_deg(1, home_idx[1]));
        autohome_wait_axes_stopped(lock);
        proto.set_position(kAxisRa, kHomeCounts);
        proto.set_position(kAxisDec, kHomeCounts);
        pointing_branch_ = 1;  // open-astro#459: re-anchored at home, no branch commanded yet
        invalidate_position_cache_locked();
        ALPACA_LOG_INFO("SkyWatcher", "AutoHome: complete, count frame re-anchored to home");
    }

    // Poll for slew completion, RELEASING the mutex around every sleep so a
    // sync slew/park doesn't block all GETs (project reference pattern).
    // The controller's stopped flag is not the end of a goto (see
    // kLandingSettle): poll until it reads stopped AND two position reads
    // kLandingSettle apart agree. Releases mutex_ around every sleep.
    void wait_axis_stationary_locked(std::unique_lock<std::mutex>& lock, int channel) const {
        auto& protocol = *protocol_;
        const auto deadline = std::chrono::steady_clock::now() + kLandingSettleTimeout;
        try {
            uint32_t last = protocol.inquire_position(channel);
            while (std::chrono::steady_clock::now() < deadline) {
                if (slew_task_cancel_.load()) {
                    return;
                }
                lock.unlock();
                std::this_thread::sleep_for(kLandingSettle);
                lock.lock();
                check_connected();
                const AxisStatus status = protocol.inquire_status(channel);
                const uint32_t now = protocol.inquire_position(channel);
                if (!status.running && now == last) {
                    return;
                }
                last = now;
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("SkyWatcher", "Axis " + std::to_string(channel) +
                                              " landing settle check could not read the board: " + e.what());
            return;
        }
        ALPACA_LOG_WARN(
            "SkyWatcher",
            "Axis " + std::to_string(channel) + " still moving " +
                std::to_string(std::chrono::duration_cast<std::chrono::seconds>(kLandingSettleTimeout).count()) +
                " s after the controller reported it stopped");
    }

    void wait_for_slew_complete(std::unique_lock<std::mutex>& lock) const {
        const auto timeout = std::chrono::seconds(180);
        auto start = std::chrono::steady_clock::now();
        const auto start_grace = std::chrono::seconds(2);
        bool saw_slewing = false;
        auto sleep_unlocked = [&](std::chrono::milliseconds d) {
            lock.unlock();
            std::this_thread::sleep_for(d);
            lock.lock();
            check_connected();
        };
        while (true) {
            if (slew_task_cancel_.load()) {
                // A reap (unpark cancelling an in-flight park, or a newer async
                // slew) wants this waiter gone; abandon the wait promptly so
                // the join is bounded.
                throw AlpacaException("Slew wait cancelled");
            }
            bool slewing = get_hardware_slewing_locked();
            if (slewing) {
                saw_slewing = true;
            }
            if (!slewing) {
                if (!saw_slewing && (std::chrono::steady_clock::now() - start) < start_grace) {
                    sleep_unlocked(std::chrono::milliseconds(200));
                    continue;
                }
                break;
            }
            if (std::chrono::steady_clock::now() - start > timeout) {
                throw AlpacaException("Slew timed out");
            }
            sleep_unlocked(std::chrono::milliseconds(250));
        }
        wait_axis_stationary_locked(lock, kAxisRa);
        wait_axis_stationary_locked(lock, kAxisDec);
        last_landing_time_ = std::chrono::steady_clock::now();
        if (last_goto_dispatch_time_ != std::chrono::steady_clock::time_point{}) {
            const double took = std::chrono::duration<double>(last_landing_time_ - last_goto_dispatch_time_).count();
            const double overhead = took - last_goto_dist_deg_ / kMaxMoveAxisRateDegPerSec;
            if (overhead > 0.0 && overhead < 30.0) {
                goto_overhead_seconds_ = std::clamp(0.5 * goto_overhead_seconds_ + 0.5 * overhead, 0.5, 10.0);
            }
            last_goto_dispatch_time_ = std::chrono::steady_clock::time_point{};
        }
        slewing_cached_ = false;
        slew_force_until_ = std::chrono::steady_clock::time_point::min();
        invalidate_position_cache_locked();
        // No post-slew position freeze: gotos are LST-compensated and refined
        // (see goto helpers), so live reads land on target -- freezing them
        // corrupted ConformU's rate-offset endpoint measurements instead.
        if (slew_settle_time_seconds_ > 0) {
            sleep_unlocked(std::chrono::seconds(slew_settle_time_seconds_));
        }
    }

    // ── Background task threads (async slew, pulse stop) ────────────────────

    bool task_wait_for(std::chrono::milliseconds d, std::atomic<bool>& cancel) const {
        std::unique_lock<std::mutex> tlock(task_mutex_);
        task_cv_.wait_for(tlock, d, [&] { return cancel.load(); });
        return !cancel.load();
    }

    // A slew/park/home task that dies CANCELLED may have launched its goto
    // before the cancel landed (abort in the pre-dispatch window): the goto
    // must not keep running. Best effort — an aborting reaper re-commands or
    // has already stopped the axes; running this before the reaper's join
    // returns keeps the two orderings consistent.
    void stop_axes_if_cancelled_locked() {
        if (!slew_task_cancel_.load() || !connected_) {
            return;
        }
        try {
            auto& protocol = *protocol_;
            protocol.instant_stop(kAxisRa);
            protocol.instant_stop(kAxisDec);
            cmd_axis_rate_deg_s_[0] = 0.0;
            cmd_axis_rate_deg_s_[1] = 0.0;
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // Best effort; the axes were already stopped by the aborter.
        }
    }

    void cancel_async_tasks() {
        slew_task_cancel_.store(true);
        pulse_task_cancel_[0].store(true);
        pulse_task_cancel_[1].store(true);
        stop_task_cancel_[0].store(true);
        stop_task_cancel_[1].store(true);
        rate_verify_cancel_.store(true);
        task_cv_.notify_all();
        std::thread slew_thread;
        std::thread pulse_thread_ra;
        std::thread pulse_thread_dec;
        std::thread stop_thread_ra;
        std::thread stop_thread_dec;
        std::thread rate_verify_thread;
        {
            std::lock_guard<std::mutex> tlock(task_mutex_);
            slew_thread = std::move(slew_task_thread_);
            pulse_thread_ra = std::move(pulse_task_thread_[0]);
            pulse_thread_dec = std::move(pulse_task_thread_[1]);
            stop_thread_ra = std::move(stop_task_thread_[0]);
            stop_thread_dec = std::move(stop_task_thread_[1]);
            rate_verify_thread = std::move(rate_verify_thread_);
        }
        if (slew_thread.joinable()) {
            slew_thread.join();
        }
        if (pulse_thread_ra.joinable()) {
            pulse_thread_ra.join();
        }
        if (pulse_thread_dec.joinable()) {
            pulse_thread_dec.join();
        }
        if (stop_thread_ra.joinable()) {
            stop_thread_ra.join();
        }
        if (stop_thread_dec.joinable()) {
            stop_thread_dec.join();
        }
        if (rate_verify_thread.joinable()) {
            rate_verify_thread.join();
        }
        // The duty worker goes through the lifecycle mutex like every other
        // reap+create path, so a disconnect racing a setter serializes with
        // it instead of joining a freshly-started worker out from under it.
        reap_duty_task();
    }

    // Per-axis: a stop-completion task on ONE axis must never cancel or join
    // the other axis's task. Before this was split, MoveAxis(0,0) followed
    // quickly by MoveAxis(1,0) (as CCDciel issues on button release, ~44ms
    // apart) cancelled the RA task before it reached its tail -- stranding
    // manual_axis_slewing_[0] set (Slewing true forever, tracking never
    // restored) since get_hardware_slewing_locked() ORs both axes' flags.
    void reap_stop_task(int axis) {
        if (axis != 0 && axis != 1) {
            return;  // MoveAxis will reject this axis under the lock shortly.
        }
        stop_task_cancel_[axis].store(true);
        task_cv_.notify_all();
        std::thread prev;
        {
            std::lock_guard<std::mutex> tlock(task_mutex_);
            prev = std::move(stop_task_thread_[axis]);
        }
        if (prev.joinable()) {
            prev.join();
        }
        stop_task_cancel_[axis].store(false);
    }

    void reap_slew_task() {
        slew_task_cancel_.store(true);
        task_cv_.notify_all();
        std::thread prev;
        {
            std::lock_guard<std::mutex> tlock(task_mutex_);
            prev = std::move(slew_task_thread_);
        }
        if (prev.joinable()) {
            prev.join();
        }
        slew_task_cancel_.store(false);
    }

    // Per-axis (open-astro#620): a pulse on one axis must never cancel or
    // join the other axis's pulse task -- ASCOM allows RA and Dec PulseGuide
    // to run concurrently. Mirrors reap_stop_task(int) above.
    void reap_pulse_task(int axis) {
        if (axis != kAxisRa && axis != kAxisDec) {
            return;  // pulse_guide() will reject this axis under the lock shortly.
        }
        const int ai = axis - 1;
        pulse_task_cancel_[ai].store(true);
        task_cv_.notify_all();
        std::thread prev;
        {
            std::lock_guard<std::mutex> tlock(task_mutex_);
            prev = std::move(pulse_task_thread_[ai]);
        }
        if (prev.joinable()) {
            prev.join();
        }
        pulse_task_cancel_[ai].store(false);
    }

    // Both-axes wrapper for every reaper OTHER than pulse_guide() itself,
    // same as before open-astro#620. goto/park/home/abort/disconnect
    // re-command or stop both axes afterwards; move_axis() and
    // sync_to_coordinates() also reap both tasks but re-command only their
    // own axis (MoveAxis) or RA (sync), so nothing re-commands the other
    // axis's reaped pulse -- pre-existing, not addressed by the per-axis
    // split (which fixes pulse-vs-pulse). Cancel BOTH
    // flags before joining either (mirrors cancel_async_tasks()): joining
    // RA first would let the still-uncancelled Dec task run to completion
    // (sending its own stop/offset-reapply I/O) while this reaper waits on
    // RA, instead of both tasks unwinding in parallel.
    void reap_pulse_task() {
        pulse_task_cancel_[kAxisRa - 1].store(true);
        pulse_task_cancel_[kAxisDec - 1].store(true);
        task_cv_.notify_all();
        reap_pulse_task(kAxisRa);
        reap_pulse_task(kAxisDec);
    }

    // Safe with mutex_ held: the rate-verify task never takes mutex_ (see
    // spawn_rate_verify_task_locked). A cancelled check just returns; it
    // never touches the hardware on the way out.
    void reap_rate_verify_task() {
        rate_verify_cancel_.store(true);
        task_cv_.notify_all();
        std::thread prev;
        {
            std::lock_guard<std::mutex> tlock(task_mutex_);
            prev = std::move(rate_verify_thread_);
        }
        if (prev.joinable()) {
            prev.join();
        }
        rate_verify_cancel_.store(false);
    }

    // ── State ───────────────────────────────────────────────────────────────

    int device_number_;
    ConnectionInfo connection_info_;
    std::unique_ptr<SkyWatcherProtocolWrapper> protocol_;
    mutable std::mutex mutex_;
    bool connected_ = false;

    AxisParameters axis_params_[2]{};

    double target_ra_hours_ = 0.0;
    double target_dec_degrees_ = 0.0;
    bool target_ra_set_ = false;
    bool target_dec_set_ = false;

    double aperture_diameter_m_ = 0.0;
    double aperture_area_m2_ = 0.0;
    double focal_length_m_ = 0.0;

    double site_latitude_;
    double site_longitude_;
    double site_elevation_m_;
    // open-astro#274: "was this coordinate ever explicitly set", tracked apart
    // from the value because 0.0 is a legitimate coordinate. Seeded from the
    // constructor's optionals and set by each ASCOM setter for its own axis.
    // Declared after site_elevation_m_ to match the initialiser order.
    bool site_latitude_set_;
    bool site_longitude_set_;
    // Client UTCDate offset and the anchors client_offset_survives_locked()
    // uses to notice a host clock step underneath it -- both clocks answer
    // that question, and both time paths ask it. Mutable: the drop happens on
    // a read.
    mutable bool has_utc_offset_ = false;
    // Was the host clock NTP/PTP-disciplined when the client wrote UTCDate?
    // Sampled at the write (open-astro#301) and, only from false to true,
    // re-sampled on the pointing path at most once per interval (#405),
    // which is why it is mutable.
    mutable bool utc_offset_host_was_synchronized_ = false;
    mutable std::chrono::steady_clock::time_point next_discipline_resample_{};  // open-astro#405
    bool client_disagreement_warned_ = false;                                   // open-astro#400
    mutable std::chrono::system_clock::duration utc_offset_{};
    mutable std::chrono::system_clock::time_point utc_anchor_system_{};
    mutable std::chrono::steady_clock::time_point utc_anchor_steady_{};

    mutable double cached_ra_axis_deg_ = 0.0;
    mutable double cached_dec_axis_deg_ = 0.0;
    // open-astro#459: the dec-axis branch (+1: a2 >= 0, -1: a2 < 0) the
    // command path last committed; the readback's answer inside the pole
    // deadband where the encoder cannot say. See branch_from_axis_locked().
    int pointing_branch_ = 1;
    // open-astro#458: measured_dec_axis_sense() of the connected board, 0 when
    // unmeasured or unknown. See home_term_sign_locked().
    int dec_axis_sense_ = 0;
    mutable bool position_cache_valid_ = false;
    // open-astro#505: set when a recovered link turned out to belong to a
    // board that had restarted (init_done cleared, position registers reset).
    // Terminal for the session — only a reconnect re-sends ":F" — and mutable
    // because it is latched from the read path. Empty means no such fault.
    mutable std::string board_reset_fault_;
    // Last protocol-side recovery epoch this driver has validated.
    mutable std::uint64_t seen_recovery_epoch_ = 0;
    mutable std::chrono::steady_clock::time_point last_position_update_{};

    bool tracking_ = false;
    bool restore_tracking_after_slew_ = false;
    mutable bool parked_ = false;
    mutable bool at_home_ = false;
    mutable bool slewing_cached_ = false;
    // open-astro#575: an async slew that fails AFTER slew_to_coordinates_async()
    // returned used to be logged and forgotten, leaving Slewing read FALSE --
    // indistinguishable from a landed goto. Set (under mutex_) by the slew
    // task's catch block on a REAL failure (never on the task's own
    // cancellation -- that is AbortSlew/a newer initiator reaping it, not a
    // failure), cleared by the next slew initiator and by AbortSlew. Consulted
    // by get_slewing() before the cached bool.
    mutable std::string last_slew_error_;
    // Bumped by every clear_last_slew_error_locked() (under mutex_): the slew
    // task records a failure only if no newer command cleared the slot since
    // its initiator did.
    uint64_t slew_error_epoch_ = 0;
    mutable std::chrono::steady_clock::time_point last_slewing_poll_ = std::chrono::steady_clock::now();
    mutable std::chrono::steady_clock::time_point slew_force_until_ = std::chrono::steady_clock::time_point::min();
    mutable bool manual_axis_slewing_[2] = {false, false};

    bool does_refraction_ = false;
    int slew_settle_time_seconds_ = 0;
    GuideRate guide_rate_{};
    // open-astro#620: one pulse task per axis (index = axis - 1, 0=RA,
    // 1=Dec, same idiom as stop_task_thread_[2]) -- ASCOM allows RA and Dec
    // PulseGuide to run concurrently, and a shared slot let an RA pulse reap
    // (and thereby strand) a running Dec pulse. pulse_axis_active_[i] is the
    // ownership flag (burst-stop / busy-axis gate, replaces the old
    // pulse_guiding_active_ + pulse_axis_ pair).
    mutable bool pulse_axis_active_[2] = {false, false};
    // open-astro#559: the ASCOM IsPulseGuiding value, true from dispatch
    // until that axis's pulse task ends (stop landed, post-stop verify
    // done). The task clears pulse_axis_active_[i] above at the same point;
    // the per-axis dispatch-time deadline remains only as a backstop.
    // IsPulseGuiding is the OR of both axes (open-astro#620). Both arrays
    // under mutex_.
    bool pulse_axis_in_motion_[2] = {false, false};
    // Incremented per pulse, per axis, under mutex_, so a task can tell
    // whether it has been superseded before clearing its axis's pulse state.
    std::uint64_t pulse_seq_[2] = {0, 0};
    mutable std::chrono::steady_clock::time_point pulse_guide_end_time_[2]{};

    mutable bool parking_ = false;
    mutable bool homing_ = false;
    // True from goto dispatch until the landing refinement finishes: Slewing
    // must not flicker false mid-refinement (the timed slew_force_until_
    // expired during a slow refine iteration, ConformU proceeded, and the
    // next refinement goto fought its pulse-guide test for the motors).
    mutable bool goto_in_progress_ = false;
    // Slewing must stay true across the post-slew tracking restore and its
    // rate check, which release mutex_ for ~450 ms (up to ~7.5 s if every
    // bound is hit), or a client polling Slewing sees the slew finish and
    // fires motion into exactly the restart window the check exists to
    // protect. But the axes are NOT owned in that window: the restore has
    // already re-applied the drive, so a rate write landing there must be
    // applied on the spot -- deferring it to "the busy operation's restore
    // path" defers it to something that has already run, and the rate is
    // stored and silently never driven. goto_in_progress_ answers both
    // questions at once and cannot express that, hence a second flag, which
    // get_slewing_locked() consults and axes_busy_locked() deliberately does
    // not (round-2 review).
    mutable bool restoring_tracking_ = false;
    // Measured goto-landing -> tracking-restart latency (EMA), see
    // kTrackingResumeSeconds; stamped by wait_for_slew_complete().
    mutable double resume_latency_seconds_ = kTrackingResumeSeconds;
    // Measured goto overhead (EMA), see kGotoRampSeconds; the dispatch stamp
    // is set by dispatch_predicted_goto_locked() and consumed at landing.
    mutable double goto_overhead_seconds_ = kGotoRampSeconds;
    mutable std::chrono::steady_clock::time_point last_goto_dispatch_time_{};
    mutable double last_goto_dist_deg_ = 0.0;
    mutable std::chrono::steady_clock::time_point last_landing_time_{};
    bool has_home_indexer_ = false;
    int tracking_rate_ = 0;                      // ASCOM DriveRate (0/1/2)
    double ra_rate_sec_per_sidereal_sec_ = 0.0;  // RightAscensionRate
    double dec_rate_arcsec_per_sec_ = 0.0;       // DeclinationRate
    double ra_duty_rate_deg_s_ = 0.0;            // sub-floor effective RA rate (duty-cycled)
    double dec_duty_rate_deg_s_ = 0.0;           // sub-floor Dec rate (duty-cycled)
    bool dec_offset_running_ = false;
    std::thread duty_thread_;
    std::atomic<bool> duty_cancel_{false};
    std::mutex duty_lifecycle_mutex_;                     // serializes duty-worker reap+create
    mutable double cmd_axis_rate_deg_s_[2] = {0.0, 0.0};  // dead-reckoning rates
    uint64_t motion_generation_ = 0;                      // bumped by every motion command; guards unlocked stop-waits
    bool park_position_set_ = false;
    double park_ra_axis_deg_ = 0.0;
    double park_dec_axis_deg_ = 0.0;

    // Web-UI firmware copy under its own narrow mutex (never mutex_).
    mutable std::mutex firmware_mutex_;
    std::string model_cache_;  // guarded by firmware_mutex_
    std::string firmware_cache_;

    // Background task threads; task_mutex_ only guards handles + cv, never
    // held across protocol I/O.
    // Serializes the async initiators (park, slew_to_coordinates_async,
    // pulse_guide -- open-astro#620) so their check -> reap -> spawn
    // sequences cannot interleave. Never held by the task threads and never
    // taken while mutex_ is held.
    std::mutex initiator_mutex_;
    mutable std::mutex task_mutex_;
    mutable std::condition_variable task_cv_;
    std::thread slew_task_thread_;
    std::thread pulse_task_thread_[2];  // indexed by axis (0=RA, 1=Dec) -- open-astro#620
    std::thread stop_task_thread_[2];   // indexed by axis (0=RA, 1=Dec)
    std::thread rate_verify_thread_;    // one-shot RightAscensionRate/TrackingRate rate-applied check
    mutable std::atomic<bool> slew_task_cancel_{false};
    mutable std::atomic<bool> pulse_task_cancel_[2]{false, false};  // indexed by axis
    mutable std::atomic<bool> stop_task_cancel_[2]{false, false};   // indexed by axis
    mutable std::atomic<bool> rate_verify_cancel_{false};
};

namespace detail {
namespace {
std::mutex& probe_mutex() {
    static std::mutex m;
    return m;
}
std::function<bool()>& probe_slot() {
    static std::function<bool()> probe = &alpacacore::util::HostClock::kernel_is_synchronized;
    return probe;
}
std::chrono::milliseconds& resample_slot() {
    static std::chrono::milliseconds interval{30000};
    return interval;
}
std::chrono::milliseconds& relink_window_slot() {
    static std::chrono::milliseconds window =
        std::chrono::duration_cast<std::chrono::milliseconds>(kRelinkMotionPreserveWindow);
    return window;
}
}  // namespace

void set_host_synchronized_probe(std::function<bool()> probe) {
    std::lock_guard<std::mutex> lock(probe_mutex());
    probe_slot() =
        probe ? std::move(probe) : std::function<bool()>(&alpacacore::util::HostClock::kernel_is_synchronized);
}

bool host_synchronized_probe() {
    std::function<bool()> probe;
    {
        std::lock_guard<std::mutex> lock(probe_mutex());
        probe = probe_slot();
    }
    return probe();
}

void set_host_discipline_resample_interval(std::chrono::milliseconds interval) {
    std::lock_guard<std::mutex> lock(probe_mutex());
    resample_slot() = interval;
}

std::chrono::milliseconds host_discipline_resample_interval() {
    std::lock_guard<std::mutex> lock(probe_mutex());
    return resample_slot();
}

void set_relink_motion_preserve_window(std::chrono::milliseconds window) {
    std::lock_guard<std::mutex> lock(probe_mutex());
    relink_window_slot() = window;
}

std::chrono::milliseconds relink_motion_preserve_window() {
    std::lock_guard<std::mutex> lock(probe_mutex());
    return relink_window_slot();
}

bool host_clock_stepped(std::chrono::system_clock::duration system_elapsed,
                        std::chrono::steady_clock::duration steady_elapsed, std::chrono::milliseconds tolerance) {
    const auto system_ms = std::chrono::duration_cast<std::chrono::milliseconds>(system_elapsed);
    const auto steady_ms = std::chrono::duration_cast<std::chrono::milliseconds>(steady_elapsed);
    const auto drift = system_ms - steady_ms;
    return drift > tolerance || drift < -tolerance;
}

bool pointing_uses_client_offset(bool offset_survives, bool host_was_synchronized) {
    if (!offset_survives) {
        return false;  // nothing to apply, or the delta no longer describes anything
    }
    // A disciplined host already has a better clock than the client's, and
    // the router refused to step it for exactly that reason.
    return !host_was_synchronized;
}
}  // namespace detail

std::unique_ptr<TelescopeDriver> create_skywatcher_telescope(int device_number, const ConnectionInfo& connection_info,
                                                             std::optional<double> site_latitude_deg,
                                                             std::optional<double> site_longitude_deg,
                                                             std::optional<double> site_elevation_m,
                                                             std::unique_ptr<SkyWatcherProtocolWrapper> protocol) {
    return std::make_unique<SkyWatcherTelescopeDriver>(device_number, connection_info, site_latitude_deg,
                                                       site_longitude_deg, site_elevation_m, std::move(protocol));
}

std::unique_ptr<TelescopeDriver> create_skywatcher_telescope_auto(int device_number, int mount_index,
                                                                  std::optional<double> site_latitude_deg,
                                                                  std::optional<double> site_longitude_deg,
                                                                  std::optional<double> site_elevation_m) {
    auto ports = enumerate_skywatcher_ports();
    if (!ports.empty()) {
        if (mount_index < 0 || mount_index >= static_cast<int>(ports.size())) {
            throw AlpacaException("Mount index " + std::to_string(mount_index) + " out of range (found " +
                                  std::to_string(ports.size()) + " mount(s))");
        }
        const auto& port = ports[static_cast<std::size_t>(mount_index)];
        ALPACA_LOG_INFO("SkyWatcher", "Auto-detected " + port.model_name + " on " + port.port_path + " (MC fw " +
                                          port.firmware_version + ", " + std::to_string(port.baud_rate) + " baud)");
        ConnectionInfo conn;
        conn.type = ConnectionType::Serial;
        conn.port_path = port.port_path;
        // The probe already proved which baud this board answers at; dropping
        // it here would reopen an EQM-35 Pro's 115200 port at the 9600 default.
        conn.baud_rate = port.baud_rate;
        return create_skywatcher_telescope(device_number, conn, site_latitude_deg, site_longitude_deg,
                                           site_elevation_m);
    }

    auto hosts = discover_skywatcher_hosts();
    if (hosts.empty()) {
        throw AlpacaException(
            "No Sky-Watcher motor controller found on any serial port or via "
            "Wi-Fi discovery (UDP 11880).");
    }
    if (mount_index < 0 || mount_index >= static_cast<int>(hosts.size())) {
        throw AlpacaException("Mount index " + std::to_string(mount_index) + " out of range (found " +
                              std::to_string(hosts.size()) + " mount(s))");
    }
    const auto& host = hosts[static_cast<std::size_t>(mount_index)];
    ALPACA_LOG_INFO("SkyWatcher", "Auto-detected mount at " + host.host + " (MC fw " + host.firmware_version + ")");
    ConnectionInfo conn;
    conn.type = ConnectionType::Network;
    conn.host = host.host;
    conn.udp_port = host.udp_port;
    return create_skywatcher_telescope(device_number, conn, site_latitude_deg, site_longitude_deg, site_elevation_m);
}

}  // namespace alpacacore::vendor::skywatcher
