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
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/auto_detect.h>
#include <alpacacore/util/client_utc_warning.h>
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/hold_window.h>
#include <alpacacore/util/host_clock_trust.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/motion_owner.h>
#include <alpacacore/util/park_slew.h>
#include <alpacacore/util/pointing.h>
#include <alpacacore/util/polled_state.h>
#include <alpacacore/util/state_snapshot.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/util/telescope_command_gate.h>
#include <alpacacore/util/utc_offset.h>
#include <alpacacore/vendor/synscan/synscan_protocol_wrapper.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <ctime>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <numbers>
#include <optional>
#include <sstream>
#include <thread>

namespace alpacacore::vendor::synscan {

namespace {

constexpr double kHoursToDegrees = 15.0;
// Decision 0010, SynScan adoption. A poll body samples the handset every kStatusPollPeriod (start to start; ticks
// missed behind a slow link are skipped, never made up): the slewing flag (L) and the equatorial pair (e) on every
// tick, the slower-moving fields (alt/az, pier side, tracking mode) on every kSlowPollEvery-th. Sky-Watcher's 100 ms
// period is unsafe on this link: a hand-controller round trip measured ~140 ms (open-astro#617: 0.56 s for four
// reads), so a frame costs 2-5 of them and 100 ms would keep the serial line busy and starve the control path.
// Alt/az, pier side and tracking mode move only with a motion the driver commands (and the control path publishes a
// full frame when that motion ends) or with a keypad press, so a slow cadence loses nothing a client can see.
// kSnapshotMaxAge is the FRESHNESS bound: a frame older than it reads `stale` and is still served. It is not the
// bound at which a getter refuses: that is the link-loss policy of decision 0009 (util::kLinkStalenessBound).
constexpr auto kStatusPollPeriod = std::chrono::milliseconds(1000);
constexpr int kSlowPollEvery = 4;
constexpr auto kSnapshotMaxAge = std::chrono::milliseconds(3000);
static_assert(kSnapshotMaxAge >= 2 * kStatusPollPeriod, "two poll periods, or a healthy link reads stale");
static_assert(kSnapshotMaxAge < util::kLinkStalenessBound, "freshness must undercut the link-loss bound");

// One polled frame of operational state (decision 0010). Small and copyable: the snapshot copies it under its mutex.
struct PolledFrame {
    double ra_hours = 0.0;
    double dec_degrees = 0.0;
    bool equatorial_known = false;
    double az_degrees = 0.0;
    double alt_degrees = 0.0;
    bool altaz_known = false;
    bool slewing = false;
    int side_of_pier = -1;    // ASCOM value, -1 (pierUnknown) when the handset's state maps to neither side
    bool pier_known = false;  // the handset has answered 'p' at least once
    int tracking_mode = -1;   // handset 'T' mode, -1 unknown
};
constexpr int kPositionLinkFailureThreshold = 3;
// How long the landing hunt waits for a status tick already sampling (open-astro#1247): a healthy tick is 2-5
// exchanges, about 0.3-0.7 s at the ~140 ms round trip of open-astro#617.
constexpr auto kPollSampleWaitBound = std::chrono::milliseconds(1500);
// Consecutive failed Park slew-status queries tolerated before the Park fails (open-astro#1122).
constexpr int kParkPollFailureThreshold = 3;
constexpr auto kSiteInfoRetryDelay = std::chrono::seconds(2);
constexpr double kMaxMoveAxisRateDegPerSec = 4.0;
constexpr double kDefaultGuideRateDegPerSec = 7.5 / 3600.0;
constexpr double kSiderealDegPerSec = 15.0411 / 3600.0;
// SynScan's limits and orders for the telescope command gate (#2258). PulseGuide has no maximum the handset protocol
// sets; MoveAxis moves axes 0 and 1 up to kMaxMoveAxisRateDegPerSec and a rate at or under 1e-6 is a stop; the guide
// rate is 0 to 1x sidereal on both axes (open-astro#775). The motion commands run no latched-fault check: a faulted
// handset refuses them through the wire. The guide-rate write, which sends nothing, checks the fault.
constexpr util::TelescopeCommandLimits kCommandLimits = [] {
    util::TelescopeCommandLimits limits;
    limits.pulse_guide_order = util::ParkedFaultOrder::NoFaultCheck;
    limits.pulse_guide_max_ms = kDefaultMaxPulseGuideMs;  // #2292: the handset sets no bound of its own
    limits.move_axis_axes = 2;
    limits.move_axis_max_rate = kMaxMoveAxisRateDegPerSec;
    limits.move_axis_stop_epsilon = 1e-6;
    limits.move_axis_order = util::ParkedFaultOrder::NoFaultCheck;
    limits.slew_order = util::ParkedFaultOrder::NoFaultCheck;
    limits.guide_rate_ra_min = 0.0;
    limits.guide_rate_ra_max = kSiderealDegPerSec;
    limits.guide_rate_dec_min = 0.0;
    limits.guide_rate_dec_max = kSiderealDegPerSec;
    limits.texts.pulse_duration = "PulseGuide duration must be 0 to 60000 ms";
    limits.texts.move_axis_axis = "MoveAxis axis must be 0 or 1";
    limits.texts.guide_rate_range = "Guide rate must be between 0 and 1x sidereal";
    return limits;
}();
// open-astro#880: a GOTO can land tens of arcseconds off (EQM-35 Pro: 29.4" in RA) and the driver reports the
// handset's own position afterwards. Refinement re-issues the GOTO until the RA and Dec readbacks are inside this
// tolerance, well inside ConformU's +/-10", for at most this many extra passes. A residual still above the
// tolerance after the last pass is logged and the slew completes: a handset's own GOTO scatter (EQM-35 Pro on
// HC 06.03.00: about +/-10" in Dec per GOTO, up to 20", open-astro#1027) is not something another pass can
// remove, and failing the slew there turned a plate-solvable 15" landing into an error.
// open-astro#1030: that handset's precise RA readback also ramps down 15.04" (one second of sidereal motion) and
// jumps back once a second, so a single RA read can be off by up to one 15.04" step; a re-GOTO round that has RA
// inside this tolerance must not move the RA aim (see refine_goto_landing_locked), or Dec-only rounds drag RA along.
// Equatorial mounts finish the landing with timed moves after at most one re-GOTO; the rule matters most for mounts
// not known to be equatorial, whose every round is a re-GOTO.
constexpr double kLandingToleranceArcsec = 3.0;
// A readback this far from the target is not a landing error a fine re-GOTO corrects. The handset sometimes ends a
// GOTO ~49' east of the aim (EQM-35 Pro, HC 06.03.00, open-astro#1105) and a repeat GOTO to the same target lands, so
// the GOTO is repeated once unchanged; a second miss this large (a refused or limit-stopped GOTO, a mount that never
// moved) fails the slew.
constexpr double kLandingRefineLimitArcsec = 600.0;
// How long a GOTO may report itself in progress before the wait stops the mount and fails the slew (#2412).
constexpr auto kSlewTimeout = std::chrono::seconds(120);
// How long a refinement GOTO forces Slewing true while the handset starts moving (the first GOTO uses 8 s).
constexpr auto kRefineStartForce = std::chrono::seconds(2);
// open-astro#1030: the handset computes RA from a local sidereal time it advances once a second, so after a GOTO
// its RA readback is a sawtooth: exact right after the handset's tick, one second of RA (15") low just before the
// next (EQM-35 Pro on HC 06.03.00, raw capture 2026-10-13: 194 counts, 1.00 s period, steady for the 125 s
// observed). A single read is therefore 0-15" low, and a landing check that reads at one phase and a client that
// reads at another disagree by up to that. After every GOTO the driver hunts for the jump, anchors the tick, and
// corrects each RA read by the fraction of the second elapsed since it.
// The hunt gives up after this long without a first jump (a handset without the sawtooth), and once it has one it
// keeps reading for up to this many jumps: the reads fall at a different phase of each second (0.277 s apart
// against a 1.000 s period), so intersecting the intervals that bracket each jump pins the tick to about 30 ms.
constexpr auto kReadbackTickFirstJumpBudget = std::chrono::milliseconds(1300);
constexpr auto kReadbackTickHuntBudget = std::chrono::milliseconds(3400);
// How long a probe waits for the hunt to release the line before it gives up and fails (the hunt's own wait for a
// probe in flight plus its read budget; still far under util::kLinkStalenessBound).
constexpr auto kProbeHoldWaitBound = kPollSampleWaitBound + kReadbackTickHuntBudget;
constexpr int kReadbackTickHuntJumps = 3;
constexpr auto kReadbackTickHuntInterval = std::chrono::milliseconds(50);
// A rise of at least half a second of RA between consecutive reads is the tick; tracking only lowers RA between.
constexpr double kReadbackTickJumpHours = 0.5 / 3600.0;
constexpr auto kReadbackTickValidity = std::chrono::seconds(120);
// A corrected read can be one second of RA off when it falls inside the tick's uncertainty; the last known RA
// (landing, sync or pulse end) picks the side. Half a second of RA is the decision margin.
constexpr double kReadbackDisambiguationHours = 0.5 / 3600.0;
// The handset also re-rolls each GOTO landing by a whole second of RA (passes with an identical aim landed 194
// counts apart), so the landing is finished with timed guide-rate moves, which it executes to a fraction of an
// arcsecond in Dec and within the readback's resolution in RA, not with more GOTOs. A residual beyond what a move
// of kNudgeMaxSeconds covers gets one re-GOTO first.
constexpr double kNudgeMaxSeconds = 8.0;
constexpr int kMaxLandingRounds = 3;
constexpr auto kPulseGuideCompletionDelay = std::chrono::milliseconds(1000);
constexpr auto kPulseGuidePositionGrace = std::chrono::milliseconds(3000);

double decode_angle(uint32_t raw, int bits) {
    const double denom = bits == 24 ? 16777216.0 : 65536.0;
    double degrees = (static_cast<double>(raw) / denom) * 360.0;
    if (degrees > 180.0) {
        degrees -= 360.0;
    }
    return degrees;
}

uint32_t encode_angle(double degrees, int bits) {
    const double denom = bits == 24 ? 16777216.0 : 65536.0;
    double wrapped = util::wrap_degrees(degrees);
    double fraction = wrapped / 360.0;
    uint32_t raw = static_cast<uint32_t>(std::round(fraction * denom));
    if (raw >= static_cast<uint32_t>(denom)) {
        raw = 0;
    }
    return raw;
}

double decode_ra_hours(uint32_t raw, int bits) {
    const double denom = bits == 24 ? 16777216.0 : 65536.0;
    double hours = (static_cast<double>(raw) / denom) * 24.0;
    if (hours >= 24.0) {
        hours -= 24.0;
    }
    return hours;
}

uint32_t encode_ra_raw(double hours, int bits) { return encode_angle(util::wrap_hours(hours) * kHoursToDegrees, bits); }

struct LocalTimeInfo {
    int offset_minutes = 0;
    bool dst = false;
};

LocalTimeInfo compute_local_timezone_info(std::time_t base_time) {
    LocalTimeInfo info{};
    std::tm local_tm {};
    std::tm utc_tm {};
#ifdef _WIN32
    localtime_s(&local_tm, &base_time);
    gmtime_s(&utc_tm, &base_time);
#else
    // Reentrant forms: the re-sync body (open-astro#2419) runs this off the client's thread, and any other thread of
    // the process (another driver, the server) may be in localtime/gmtime, whose shared std::tm it would overwrite.
    localtime_r(&base_time, &local_tm);
    gmtime_r(&base_time, &utc_tm);
#endif
    std::time_t local_time = std::mktime(&local_tm);
    std::time_t utc_as_local = std::mktime(&utc_tm);
    double offset_seconds = std::difftime(local_time, utc_as_local);
    info.offset_minutes = static_cast<int>(std::round(offset_seconds / 60.0));
    info.dst = local_tm.tm_isdst > 0;
    return info;
}

// SynScan V3/V4 hand controller model IDs (from "m" command), or empty for an id not in the table.
// Only includes mounts that ship with the V3/V4 handset.
std::string synscan_model_id_to_name(int model_id) {
    switch (model_id) {
        case 0:  return "EQ6 Pro";
        case 1:  return "HEQ5 Pro";
        case 2:  return "EQ5";
        case 3:  return "EQ3";
        case 4:  return "EQ8";
        case 5:  return "AZ-EQ6";
        case 6:  return "AZ-EQ5";
        // 50 (0x32) is absent from the published V3/V4 table but is what an
        // EQM-35 Pro reports; cross-confirmed against the same mount's motor
        // controller, whose ":e" mount-code byte is also 0x32.
        case 50:
            return "EQM-35 Pro";
        case 56: return "HEQ5 Pro";
        case 160: return "AllView";
        default:
            if (model_id >= 128 && model_id <= 143) return "AZ GOTO";
            if (model_id >= 144 && model_id <= 159) return "DOB GOTO";
            return {};  // unknown: the name falls back to the ASCOM type (decision 0024)
    }
}

} // namespace

double park_ra_from_hour_angle(double lst_hours, double hour_angle_hours) {
    return util::wrap_hours(lst_hours - hour_angle_hours);
}

class SynScanTelescopeDriver : public TelescopeDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    SynScanTelescopeDriver(int device_number, const ConnectionInfo& connection_info, SynScanVersion version,
                           std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
                           std::optional<double> site_elevation_m, std::optional<bool> sync_time_on_connect,
                           SynScanAlignmentSetting alignment,
                           util::ConnectionResolver<ConnectionInfo> connection_resolver = {},
                           util::TaskClock& clock = util::default_task_clock())
        : AsyncConnectable("SynScan"),
          device_number_(device_number),
          connection_info_(connection_info),
          connection_resolver_(std::move(connection_resolver)),
          version_(version),
          connected_(false),
          target_ra_hours_(0.0),
          target_dec_degrees_(0.0),
          aperture_diameter_m_(0.0),
          focal_length_m_(0.0),
          site_latitude_cached_(0.0),
          site_longitude_cached_(0.0),
          site_info_valid_(false),
          site_elevation_m_(site_elevation_m),
          timezone_offset_minutes_(0),
          timezone_offset_valid_(false),
          dst_observed_(false),
          tracking_mode_cached_(0),
          tracking_mode_valid_(false),
          parked_(false),
          at_home_(false),
          mount_model_id_(-1),
          use_precise_commands_(version_ != SynScanVersion::V3),
          pending_site_latitude_(site_latitude_deg),
          pending_site_longitude_(site_longitude_deg),
          pending_site_elevation_(site_elevation_m),
          sync_time_on_connect_(sync_time_on_connect.value_or(false)),
          alignment_setting_(alignment),
          clock_(clock) {
        guide_rate_.ra = kDefaultGuideRateDegPerSec;
        guide_rate_.dec = kDefaultGuideRateDegPerSec;
    }

    ~SynScanTelescopeDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        // Cancel + join the status poll and the background slew/pulse task
        // threads before any member they touch is destroyed. Must run WITHOUT
        // mutex_ held (the task threads take mutex_). closing_ first: a probe waiting on the hunt's hold ends at once.
        // The wrapper interrupt ends a read in flight, so the joins never wait out a silent handset (#2070).
        set_closing(true);
        const bool interrupt = connected_.load();
        if (interrupt) SynScanProtocolWrapper::instance().set_interrupt(true);
        run_closing_hook_for_testing();
        cancel_async_tasks();
        {
            // Under transition_mutex_, as every other reaper and starter of the clock slot (open-astro#2419).
            std::lock_guard<std::mutex> transition(transition_mutex_);
            clock_resync_.cancel_all_and_join();
        }
        polled_.stop_and_join();
        if (connected_) {
            try {
                // Qualified: virtual dispatch is already gone in a destructor.
                SynScanTelescopeDriver::set_connected(false);
            } catch (...) {  // NOLINT(bugprone-empty-catch): nothing to do in a destructor
                // Destructor: nothing useful to do with a disconnect failure.
            }
        }
        if (interrupt) SynScanProtocolWrapper::instance().set_interrupt(false);
    }

    int get_device_number() const override {
        return device_number_;
    }

    // Brand + model only (decision 0024). A mount reachable over both its hand
    // controller and the EQMOD direct driver has the same name on both; the
    // Description names the path.
    std::string get_name() const override {
        const int model_id = mount_model_id_.load();
        const std::string model = model_id >= 0 ? synscan_model_id_to_name(model_id) : std::string();
        return model.empty() ? "Sky-Watcher Telescope" : "Sky-Watcher " + model;
    }

    DeviceType get_device_type() const override {
        return DeviceType::Telescope;
    }

    std::string fallback_unique_id() const override { return "SynScan_" + std::to_string(device_number_); }

    std::string get_description() const override {
        return "Sky-Watcher SynScan V3/V4 Mount Driver";
    }

    std::string get_driver_info() const override {
        return "AlpacaCore SynScan Driver v0.1";
    }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Handset firmware captured at connect; surfaced in the web UI only. Guarded
    // by its OWN narrow firmware_mutex_ (a leaf), so a configureddevices poll never takes mutex_.
    std::optional<std::string> get_device_firmware() const override {
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        if (firmware_cache_.empty()) {
            return std::nullopt;
        }
        return firmware_cache_;
    }

    int get_interface_version() const override { return 4; }

    // Lock-free on purpose: the router polls this getter from the PUT connected wait and from every GET connected in
    // between, so it takes no lock a connect or a getter could hold (issue #130: 25 s measured against a silent hand
    // controller when it took mutex_). The connect sets connected_ last, after the first frame is held, and a lost link
    // reads false through the poll's loss latch or the wrapper's (decision 0009, link_lost()). Same atomic-flag pattern
    // as every other AsyncConnectable driver except the telescopes and the wrapper-backed switches named in
    // async_connectable.h. Stated as the rule rather than as a count: a bare number in a comment has nothing tying it
    // to the code it describes, and every new driver invalidates it silently (open-astro#381).
    bool get_connected() const override { return connected_.load() && !link_lost(); }

    // The poll's latched fault text, read under the module's own leaf mutex; never mutex_. A serial link lost at once
    // (decision 0009 rule 5) reports the wrapper's loss reason, which the wrapper keeps until the next Connect; a
    // Disconnect hides it with connected_. Preferring it whenever connected_ is true is correct only because the
    // wrapper's connect() clears the text before connected_ can turn true, so a reason from an earlier session never
    // shows in this one.
    std::string get_link_fault() const override {
        if (connected_.load()) {
            std::string lost = SynScanProtocolWrapper::instance().link_fault();
            if (!lost.empty()) return lost;
        }
        return polled_.fault();
    }

    ALPACA_ASYNC_CONNECT_FORWARDERS()

    void set_connected(bool connected) override {
        // transition_mutex_ spans the whole call and no getter takes it: the handshake runs under it, with mutex_ held
        // only to publish each result, never across an exchange (decision 0016). Lock order: transition_mutex_,
        // initiator_mutex_, the wire turn, the wrapper's lock; mutex_ is a leaf. A sync disconnect that arrives during
        // a sync connect waits here for the rest of the handshake; the async disconnect() the router uses never does.
        // Declared before the transition lock, so it runs after the lock is released: a host clock change that
        // on_host_clock_changed() could not act on while this call held the lock is acted on here (open-astro#2419).
        struct DrainPendingResync {
            SynScanTelescopeDriver& driver;
            ~DrainPendingResync() { driver.drain_pending_resync(); }
        } drain_pending_resync_on_exit{*this};
        std::lock_guard<std::mutex> transition(transition_mutex_);
        std::unique_lock<std::mutex> ilock(initiator_mutex_, std::defer_lock);
        ClosingScope closing(*this);
        // Decision 0009 point 4: a lost link is relinked only by the client's Connected=true, which tears the dead
        // session down (slew bodies included) and connects afresh.
        const bool relink = connected && connected_.load() && link_lost();
        if (!connected || relink) {
            // closing_ before the initiator wait: the poll already stops at its next exchange boundary, and a probe
            // waiting on the hunt's hold wakes, while the disconnect waits for an initiator in flight. A session of
            // this driver also interrupts the wrapper, so the read in flight (the poll's, a body's or an initiator's)
            // ends at once instead of after the response timeout (#2070). Only this driver's session: the wrapper is
            // one per process, and a disconnect of a second, never-connected SynScan device must not cut the
            // connected one's exchanges.
            closing.begin(/*interrupt_wire=*/connected_.load());
            ilock.lock();
            {
                // Cancels a slew body and ends a synchronous slew's landing, nudge or hunt on the caller's thread.
                std::lock_guard<std::mutex> lock(mutex_);
                cancel_motion_locked(util::MotionOwner::kBothAxes);
            }
            // Cancel + join the background slew/pulse bodies and the status poll BEFORE the close: they take mutex_
            // and send on the shared wrapper, so joining under the lock would deadlock and leaving them running
            // across the protocol disconnect would race the wrapper.
            cancel_async_tasks();
            // The host clock re-sync body takes no lock above config_write_mutex_, so it is joined here as well.
            clock_resync_.cancel_all_and_join();
            polled_.stop_and_join();
            if (relink) {
                teardown_session();
                closing.end();
            }
        }
        // Base gates BEFORE the idempotency check: a sync disconnect during an
        // in-flight connect looks idempotent (both sides see disconnected) and
        // would be silently dropped without the record; a connect must honor a
        // newer pending disconnect by staying down.
        if (!connected && record_disconnect_if_connect_in_flight(connected_)) {
            return;
        }
        if (connected && consume_pending_disconnect(connected_)) {
            return;
        }
        if (connected == connected_) {
            return;
        }
        if (!connected) {
            teardown_session();
            return;
        }
        // A host clock change seen before this connect is covered by the connect's own trust check: a reconnect never
        // inherits a pending re-sync (open-astro#2419).
        notify_connect_start_for_testing();
        resync_pending_.store(false);
        auto& protocol = SynScanProtocolWrapper::instance();
        try {
            connect_session(protocol);
        } catch (...) {
            // A throw after the open closes it again and leaves nothing of the half-built session.
            polled_.stop_and_join();
            try {
                teardown_session();
            } catch (...) {  // NOLINT(bugprone-empty-catch): the connect's own error is the one to report
            }
            throw;
        }
    }

    // The raw pass-through commands check the connection under mutex_ and send with it released (the wrapper
    // serialises the wire).
    std::string command_blind(std::string_view command, bool raw) override {
        (void)raw;
        require_connected();
        SynScanProtocolWrapper::instance().send_command_blind(std::string(command));
        return "";
    }

    bool command_bool(std::string_view command, bool raw) override {
        require_connected();
        auto& protocol = SynScanProtocolWrapper::instance();
        if (raw) {
            std::string response = protocol.send_command(std::string(command));
            return response == "1";
        }
        protocol.send_command_blind(std::string(command));
        return true;
    }

    std::string command_string(std::string_view command, bool raw) override {
        (void)raw;
        require_connected();
        return SynScanProtocolWrapper::instance().send_command(std::string(command));
    }

    AlignmentMode get_alignment_mode() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        return alignment_mode_locked();
    }

    double get_altitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        return served_altaz_locked().second;
    }

    // The optics and SlewSettleTime are the client's values, not the mount's: lock-free, no fault refusal, but
    // NotConnected while disconnected or lost like every other member (ASCOM contract precedence). The optics setters
    // are not ASCOM members: the catalog and updatedevice (#2057) inject the configured values with them, connected
    // or not.
    double get_aperture_diameter() const override {
        check_connected();
        return aperture_diameter_m_;
    }

    void set_aperture_diameter(double meters) override {
        if (meters < 0.0) {
            throw AlpacaException("Aperture diameter must be non-negative", AlpacaError::InvalidValue);
        }
        aperture_diameter_m_ = meters;
    }

    double get_aperture_area() const override {
        check_connected();
        // Derived from the atomic diameter so a live updatedevice cannot leave a stale area (#2057).
        const double d = aperture_diameter_m_;
        return d > 0.0 ? std::numbers::pi * (d / 2.0) * (d / 2.0) : 0.0;
    }

    bool get_at_home() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        throw_if_link_faulted();
        return at_home_;
    }

    bool get_at_park() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        throw_if_link_faulted();
        return parked_;
    }

    double get_azimuth() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        return served_altaz_locked().first;
    }

    bool get_can_find_home() const override {
        return false;
    }

    bool get_can_park() const override {
        return true;
    }

    bool get_can_pulse_guide() const override {
        return true;
    }

    bool get_is_pulse_guiding() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        throw_if_link_faulted();
        const auto now = clock_.now();
        return pulse_hold_[0].active(now) || pulse_hold_[1].active(now);
    }

    bool get_can_set_declination_rate() const override {
        return false;
    }

    bool get_can_set_guide_rates() const override {
        return true;
    }

    bool get_can_set_park() const override {
        return true;
    }

    bool get_can_set_pier_side() const override {
        return false;
    }

    bool get_can_set_right_ascension_rate() const override {
        return false;
    }

    bool get_can_set_tracking() const override {
        return true;
    }

    bool get_can_slew_alt_az() const override {
        return false;
    }

    bool get_can_slew_alt_az_async() const override {
        return false;
    }

    bool get_can_sync_alt_az() const override {
        return false;
    }

    bool get_can_slew() const override {
        return true;
    }

    bool get_can_slew_async() const override {
        return true;
    }

    bool get_can_sync() const override {
        return true;
    }

    bool get_can_unpark() const override {
        return true;
    }

    double get_declination() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (polled_.fault().empty() && guide_position_valid_ && clock_.now() < position_override_until_ &&
            !get_slewing_locked()) {
            return std::clamp(guide_position_dec_degrees_, -90.0, 90.0);
        }
        return std::clamp(served_equatorial_locked().second, -90.0, 90.0);
    }

    double get_declination_rate() const override {
        check_connected();
        throw_if_link_faulted();
        return 0.0;
    }

    void set_declination_rate(double rate) override {
        (void)rate;
        require_connected();
        throw AlpacaException("Declination rate not supported", AlpacaError::PropertyNotImplemented);
    }

    bool get_tracking() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        throw_if_link_faulted();
        const int mode = served_frame_locked().value.tracking_mode;
        if (mode < 0) {
            throw AlpacaException("SynScan tracking state is not known yet: the handset has not answered a status poll",
                                  AlpacaError::DriverException);
        }
        return mode != 0 && !ra_stop_unconfirmed_;
    }

    // The `T` goes out in its wire turn (decision 0006, "Claim"), after the stop and tracking restore of any motion
    // owner queued before it, so the requested mode is the one the handset keeps. It claims nothing: a slew or a
    // landing in flight goes on. Its wait: none while idle; during a landing move, the rest of that move (at most
    // kNudgeMaxSeconds) plus its stop, up to three exchanges of response_timeout_ms each; otherwise the one command
    // in flight.
    void set_tracking(bool tracking) override {
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        if (tracking) {
            check_not_parked_locked("Tracking");
        }
        auto& protocol = SynScanProtocolWrapper::instance();
        int mode = 0;
        if (tracking) {
            const AlignmentMode alignment = alignment_mode_locked();
            if (alignment != AlignmentMode::AltAz && !site_info_valid_) {
                ensure_site_info_cached(lock);
            }
            if (alignment != AlignmentMode::AltAz && !site_info_valid_) {
                throw AlpacaException("Set SiteLatitude before enabling equatorial tracking", AlpacaError::ValueNotSet);
            }
            // SynScan "T" modes (protocol v3.3): 1 = Alt/Az, 2 = equatorial in either hemisphere, 3 = PEC. The
            // handset applies its own site; 3 is not "EQ south" as it is on a Celestron.
            mode = alignment == AlignmentMode::AltAz ? 1 : 2;
        }
        // The cache is written after the send: a motion owner ahead in the queue restores the mode it found.
        bool sent = false;
        try {
            sent = send_in_turn(
                lock, [this] { return connected_ && !link_lost(); },
                [&protocol, mode] { protocol.set_tracking_mode(mode); });
        } catch (...) {
            // While a restore is owed (ra_stop_unconfirmed_), the cache is the mode the next RA stop sends, and
            // refresh_tracking_mode_locked() keeps it. A write that failed is still the client's latest request: the
            // next stop sends this mode, so a failed Tracking=false cannot leave an older mode to turn tracking on.
            // The turn is already passed on here, but mutex_ has been held since without_lock() re-took it, so an
            // owner queued behind this write reads the new mode at its turn. Keep mutex_ held until this write.
            if (ra_stop_unconfirmed_) {
                set_tracking_mode_cached_locked(mode);
            }
            throw;
        }
        if (!sent) {
            check_connected();
            return;
        }
        set_tracking_mode_cached_locked(mode);
        tracking_mode_valid_ = true;
        ra_stop_unconfirmed_ = false;  // the write landed, so the mode is known again
        invalidate_readback_tick_locked();
    }

    double get_focal_length() const override {
        check_connected();
        return focal_length_m_;
    }

    void set_focal_length(double meters) override {
        if (meters < 0.0) {
            throw AlpacaException("Focal length must be non-negative", AlpacaError::InvalidValue);
        }
        focal_length_m_ = meters;
    }

    GuideRate get_guide_rate() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();  // a driver that never connected has no rate to answer; lost is NotConnected too
        throw_if_link_faulted();
        return guide_rate_;
    }

    // Each value is checked before the connection, so a bad one is InvalidValue even while disconnected
    // (open-astro#1786); decision 0009 rule 1: a faulted link refuses the write.
    void set_guide_rate(const GuideRate& rate) override {
        util::check_guide_rate_arguments(kCommandLimits, util::GuideRateAxis::RightAscension, rate.ra);
        util::check_guide_rate_arguments(kCommandLimits, util::GuideRateAxis::Declination, rate.dec);
        std::lock_guard<std::mutex> lock(mutex_);
        util::check_guide_rate_state(kCommandLimits, GateChecks{*this, "GuideRate"});
        guide_rate_ = rate;
    }

    // One axis under one lock: the base default reads the pair first; here the connection and fault gates run in the
    // same lock as the store.
    void set_guide_rate_right_ascension(double value) override {
        util::check_guide_rate_arguments(kCommandLimits, util::GuideRateAxis::RightAscension, value);
        std::lock_guard<std::mutex> lock(mutex_);
        util::check_guide_rate_state(kCommandLimits, GateChecks{*this, "GuideRateRightAscension"});
        guide_rate_.ra = value;
    }

    void set_guide_rate_declination(double value) override {
        util::check_guide_rate_arguments(kCommandLimits, util::GuideRateAxis::Declination, value);
        std::lock_guard<std::mutex> lock(mutex_);
        util::check_guide_rate_state(kCommandLimits, GateChecks{*this, "GuideRateDeclination"});
        guide_rate_.dec = value;
    }

    double get_right_ascension() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (polled_.fault().empty() && guide_position_valid_ && clock_.now() < position_override_until_ &&
            !get_slewing_locked()) {
            return guide_position_ra_hours_;
        }
        return served_equatorial_locked().first;
    }

    double get_right_ascension_rate() const override {
        check_connected();
        throw_if_link_faulted();
        return 0.0;
    }

    void set_right_ascension_rate(double rate) override {
        (void)rate;
        require_connected();
        throw AlpacaException("Right ascension rate not supported", AlpacaError::PropertyNotImplemented);
    }

    int get_side_of_pier() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        const auto frame = served_frame_locked().value;
        if (!frame.pier_known) {
            throw AlpacaException("SynScan pier side is not known yet: the handset has not reported its pointing state",
                                  AlpacaError::DriverException);
        }
        return frame.side_of_pier;
    }

    void set_side_of_pier(int side) override {
        (void)side;
        require_connected();
        throw AlpacaException("Pier side not supported", AlpacaError::PropertyNotImplemented);
    }

    int get_destination_side_of_pier(double ra, double dec) const override {
        util::validate_ra_dec(ra, dec, "DestinationSideOfPier");
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        throw_if_link_faulted();
        if (!site_info_valid_) {
            ensure_site_info_cached(lock);
        }
        if (!site_info_valid_) {
            return served_frame_locked().value.side_of_pier;  // the same link checks as every other frame read
        }
        double lst_hours = util::local_sidereal_hours(sidereal_wall_now(), site_longitude_cached_);
        double hour_angle = util::shortest_ra_delta_hours(lst_hours, ra);
        return util::pier_side_for_hour_angle(hour_angle);
    }

    EquatorialSystem get_equatorial_system() const override {
        return EquatorialSystem::J2000;
    }

    // The driver applies no refraction correction, so it neither knows nor computes the flag (ITelescopeV4).
    bool get_does_refraction() const override {
        require_connected();
        throw AlpacaException("DoesRefraction is not implemented", AlpacaError::PropertyNotImplemented);
    }

    void set_does_refraction(bool) override {
        require_connected();
        throw AlpacaException("DoesRefraction is not implemented", AlpacaError::PropertyNotImplemented);
    }

    int get_slew_settle_time() const override {
        check_connected();
        return slew_settle_time_seconds_;
    }

    void set_slew_settle_time(int seconds) override {
        if (seconds < 0) {
            throw AlpacaException("Slew settle time must be >= 0 seconds", AlpacaError::InvalidValue);
        }
        check_connected();  // an ASCOM member: InvalidValue first, then NotConnected
        slew_settle_time_seconds_ = seconds;
    }

    double get_sidereal_time() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        throw_if_link_lost_or_faulted();
        // No handset read here: the poll retries an unread site (decision 0010), so DeviceState costs no transaction.
        return util::local_sidereal_hours(sidereal_wall_now(), site_longitude_cached_);
    }

    double get_site_elevation() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!site_elevation_m_) {
            throw AlpacaException("SiteElevation has not been set", AlpacaError::InvalidOperation);
        }
        return *site_elevation_m_;
    }

    void set_site_elevation(double elevation) override {
        if (!std::isfinite(elevation) || elevation < -300.0 || elevation > 10000.0) {
            throw AlpacaException("SiteElevation must be in range -300 to 10000 meters",
                                  AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        site_elevation_m_ = elevation;
    }

    double get_site_latitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        throw_if_link_lost_or_faulted();
        require_site_known_locked();
        return site_latitude_cached_;
    }

    void set_site_latitude(double latitude) override {
        util::validate_latitude(latitude);
        std::lock_guard<std::mutex> config(config_write_mutex_);  // the other coordinate is read before the `W`
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        LocationInfo info = current_location(lock);
        info.latitude_degrees = latitude;
        without_lock(lock, [&info] { SynScanProtocolWrapper::instance().set_location(info); });
        check_connected();
        site_latitude_cached_ = latitude;
        site_longitude_cached_ = info.longitude_degrees;
        site_info_valid_ = true;
    }

    double get_site_longitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        throw_if_link_lost_or_faulted();
        require_site_known_locked();
        return site_longitude_cached_;
    }

    void set_site_longitude(double longitude) override {
        util::validate_longitude(longitude);
        std::lock_guard<std::mutex> config(config_write_mutex_);  // the other coordinate is read before the `W`
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        LocationInfo info = current_location(lock);
        info.longitude_degrees = longitude;
        without_lock(lock, [&info] { SynScanProtocolWrapper::instance().set_location(info); });
        check_connected();
        site_latitude_cached_ = info.latitude_degrees;
        site_longitude_cached_ = longitude;
        site_info_valid_ = true;
    }

    bool get_slewing() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        // open-astro#575: surface a stored async-slew failure as an error
        // instead of a silent false, until AbortSlew or a new slew initiator
        // clears it (see last_slew_error_). It names the failed operation, so it precedes the link fault.
        if (!last_slew_error_.empty()) {
            throw AlpacaException(last_slew_error_);
        }
        throw_if_link_faulted();
        return get_slewing_locked();
    }

    double get_target_declination() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();  // decision 0009 rules 1 and 3: commanded state, refused on a lost or faulted link
        throw_if_link_faulted();
        if (!target_dec_set_) {
            throw AlpacaException("Target declination has not been set", AlpacaError::ValueNotSet);
        }
        return target_dec_degrees_;
    }

    void set_target_declination(double dec) override {
        util::validate_dec(dec, "TargetDeclination");
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        throw_if_link_faulted();  // decision 0009 rule 1: a refused write keeps the value and its set flag
        target_dec_degrees_ = dec;
        target_dec_set_ = true;
    }

    double get_target_right_ascension() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();  // decision 0009 rules 1 and 3: commanded state, refused on a lost or faulted link
        throw_if_link_faulted();
        if (!target_ra_set_) {
            throw AlpacaException("Target right ascension has not been set", AlpacaError::ValueNotSet);
        }
        return target_ra_hours_;
    }

    void set_target_right_ascension(double ra) override {
        util::validate_ra(ra, "TargetRightAscension");
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        throw_if_link_faulted();  // decision 0009 rule 1: a refused write keeps the value and its set flag
        target_ra_hours_ = ra;
        target_ra_set_ = true;
    }

    int get_tracking_rate() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        throw_if_link_faulted();
        return 0;
    }

    void set_tracking_rate(int rate) override {
        (void)rate;
        require_connected();
        throw AlpacaException("Tracking rates not supported", AlpacaError::PropertyNotImplemented);
    }

    std::vector<int> get_tracking_rates() const override {
        return {0};
    }

    std::chrono::system_clock::time_point get_utc_date() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        throw_if_link_faulted();
        if (const auto utc = utc_offset_.current(clock_.now())) return *utc;
        throw AlpacaException("The mount clock could not be read; set UTCDate first", AlpacaError::InvalidOperation);
    }

    void set_utc_date(std::chrono::system_clock::time_point utc) override {
        std::lock_guard<std::mutex> config(config_write_mutex_);
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        apply_utc_date(utc, lock, true);
        ++utc_write_generation_;  // a host clock re-sync decided before this write does not write over it (#2419)
    }

private:
    // Body of set_utc_date. `lock` (mutex_) is held on entry and on return; the `H` write runs with it released. The
    // connect calls it before connected_ is set, so only set_utc_date asks for the connection check after the write:
    // a disconnect during the `H` makes the write report NotConnected, as the site writes do.
    void apply_utc_date(std::chrono::system_clock::time_point utc, std::unique_lock<std::mutex>& lock,
                        bool require_connected) {
        std::time_t utc_time_t = std::chrono::system_clock::to_time_t(utc);
        LocalTimeInfo tz_info = compute_local_timezone_info(utc_time_t);

        std::tm local_tm {};
#ifdef _WIN32
        localtime_s(&local_tm, &utc_time_t);
#else
        localtime_r(&utc_time_t, &local_tm);  // reentrant, as in compute_local_timezone_info()
#endif

        TimeInfo info;
        info.hour = local_tm.tm_hour;
        info.minute = local_tm.tm_min;
        info.second = local_tm.tm_sec;
        info.month = local_tm.tm_mon + 1;
        info.day = local_tm.tm_mday;
        info.year = (local_tm.tm_year + 1900) % 100;
        info.timezone_offset_minutes = tz_info.offset_minutes;
        info.dst_enabled = tz_info.dst;

        const auto client_minus_host = utc - std::chrono::system_clock::now();  // before the write (#409)
        without_lock(lock, [&info] { SynScanProtocolWrapper::instance().set_time(info); });
        if (require_connected) {
            check_connected();
        }
        invalidate_readback_tick_locked();
        timezone_offset_minutes_ = tz_info.offset_minutes;
        timezone_offset_valid_ = true;
        dst_observed_ = tz_info.dst;
        utc_offset_.set(utc, clock_.now());
        // The mount now runs on the client's clock and so does the cached
        // pointing time; on a disciplined host say so once (open-astro#409).
        alpacacore::util::ClientUtcWarning::warn_once("SynScan", client_minus_host, client_disagreement_warned_);
    }

public:
    void find_home() override {
        require_connected();
        throw AlpacaException("FindHome not supported", AlpacaError::MethodNotImplemented);
    }

    // Park is an asynchronous initiator (ITelescopeV4; ConformU 4.5 times it
    // against the 1 s STANDARD target): the park slew is dispatched in the
    // slew task thread and this call returns at once. Slewing reports true
    // until the mount reaches the park position and tracking is stopped, at
    // which point AtPark flips true in the same locked step (no window where
    // a poller sees Slewing false with AtPark false). Issue #208.
    void park() override {
        // Serialize against other initiators (SlewToCoordinatesAsync): the
        // check-then-claim-then-spawn sequence must not interleave with
        // another initiator's, or a park could be cancelled and restarted
        // (or a slew could clobber a park) in the gap.
        std::lock_guard<std::mutex> ilock(initiator_mutex_);
        {
            // Check BEFORE claiming: the claim ends a park in flight, so a second
            // Park would restart the slew instead of being the documented no-op.
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            if (parked_ || parking_) {
                return;  // calling Park twice (or while parking) is harmless
            }
        }
        bool park_altaz = false;
        double park_target_first = 0.0;
        double park_target_second = 0.0;
        bool displaced = false;
        bool prev_slewing = false;
        bool prev_at_home = false;
        bool prev_guide_valid = false;
        bool prev_manual[2] = {false, false};
        auto prev_override = util::TaskClock::clock::time_point::min();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            check_connected();
            if (parked_ || parking_) {
                return;  // ASCOM: Park on a parked (or parking) mount is harmless.
            }
            if (!park_position_set_) {
                store_park_position(lock);
            }
            park_altaz = park_alignment_mode_ == AlignmentMode::AltAz;
            if (park_altaz) {
                park_target_first = park_azimuth_degrees_;
                park_target_second = park_altitude_degrees_;
            } else if (!park_ra_uses_hour_angle_) {
                park_target_first = park_ra_hours_;
                park_target_second = park_dec_degrees_;
            } else {
                if (!site_info_valid_ && !slew_.running()) {  // no `w` queued behind a body's exchange
                    ensure_site_info_cached(lock);
                }
                // An hour angle is saved only while a site is known, so a
                // handset that has since stopped reporting one (a reconnect
                // without location) still leaves the last known longitude here.
                const double lst = util::local_sidereal_hours(sidereal_wall_now(), site_longitude_cached_);
                park_target_first = park_ra_from_hour_angle(lst, park_hour_angle_hours_);
                park_target_second = park_dec_degrees_;
                util::validate_ra_dec(park_target_first, park_target_second, "Park");
            }
            // Nothing below refuses: supersede and claim only now, so a refused Park leaves the body and the pulses
            // in flight alone.
            const bool pulse_stopped = supersede_pulses_for_slew_locked(lock);
            check_connected();
            displaced = claim_slew_slot_locked() || pulse_stopped;
            // Snapshot what the block below overwrites, for a refused start.
            prev_slewing = slewing_cached_;
            prev_at_home = at_home_;
            prev_guide_valid = guide_position_valid_;
            prev_manual[0] = manual_axis_slewing_[0];
            prev_manual[1] = manual_axis_slewing_[1];
            prev_override = position_override_until_;
            // Publish the slewing state before the task starts so a poller
            // never sees Slewing false between Park returning and dispatch.
            set_slewing_locked(true);
            park_poll_failures_ = 0;
            slew_hold_.arm(clock_.now(), std::chrono::seconds(8));
            position_override_until_ = util::TaskClock::clock::time_point::min();
            guide_position_valid_ = false;
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            ra_stop_unconfirmed_ = false;
            at_home_ = false;
            parking_ = true;
            claim_motion_locked(util::MotionCommand::Slew);
            clear_pulse_guiding_locked();
            // open-astro#575: a fresh initiator is a clean start -- a client
            // that calls Park after a failed GOTO must not be told the OLD
            // goto failed while it's parking.
            last_slew_error_.clear();
        }

        start_slew_body(
            [this, park_altaz, park_target_first, park_target_second](util::OperationContext& ctx) {
                std::unique_lock<std::mutex> lock(mutex_);
                util::ParkSlewHooks hooks;
                // Every wire exchange below runs with mutex_ released (the hooks take `lock` back before they
                // return), and a hook never touches a flag once a newer initiator has superseded this body.
                const auto superseded = [&ctx] { return ctx.stop_reason() == util::StopReason::Superseded; };
                hooks.still_parking = [this] { return connected_ && parking_; };
                hooks.superseded = superseded;
                hooks.abandon = [this, superseded] {
                    if (!superseded()) {
                        parking_ = false;
                    }
                };
                hooks.dispatch = [&] {
                    if (park_altaz) {
                        do_slew_to_altaz_locked(park_target_second, park_target_first, lock,
                                                [&ctx] { return !slot_stopped(&ctx); });
                    } else {
                        do_slew_to_coordinates_locked(
                            park_target_first, park_target_second, lock, [&ctx] { return !slot_stopped(&ctx); }, false);
                    }
                };
                hooks.poll_slewing = [&] {
                    return poll_hardware_slewing_locked(lock, true, false, [&ctx] { return slot_stopped(&ctx); });
                };
                hooks.stop_tracking = [&] {
                    if (!send_in_turn(
                            lock, [&] { return !superseded(); },
                            [] { SynScanProtocolWrapper::instance().set_tracking_mode(0); }) ||
                        superseded()) {
                        return;
                    }
                    set_tracking_mode_cached_locked(0);
                    tracking_mode_valid_ = true;
                    invalidate_readback_tick_locked();
                };
                hooks.fail = [&](const std::string& message) { fail_park_locked(message, ctx, lock); };
                // The park GOTO runs no landing refinement, so a miss gets the one repeat GOTO a slew gets (#1105).
                hooks.repeat_on_miss = true;
                hooks.verify_at_target = [&]() -> std::string {
                    const RawSample raw =
                        without_lock(lock, [this] { return sample_raw(use_precise_commands_, true); });
                    if (superseded()) {
                        return {};  // a newer operation owns the state; the flow abandons
                    }
                    const PolledFrame landed = decode_sample_locked(raw, use_precise_commands_, PolledFrame{});
                    if (park_altaz && !landed.altaz_known) {
                        return "Park: the mount did not report its altitude and azimuth";
                    }
                    const double miss_arcsec =
                        park_altaz
                            ? util::angular_separation_arcsec(landed.az_degrees, landed.alt_degrees, park_target_first,
                                                              park_target_second)
                            : util::angular_separation_arcsec(landed.ra_hours * kHoursToDegrees, landed.dec_degrees,
                                                              park_target_first * kHoursToDegrees, park_target_second);
                    if (miss_arcsec <= kLandingRefineLimitArcsec) {
                        return {};
                    }
                    const std::string message = "Park ended " + std::to_string(static_cast<long>(miss_arcsec)) +
                                                " arcsec from the park position (limit " +
                                                std::to_string(static_cast<long>(kLandingRefineLimitArcsec)) +
                                                " arcsec): the mount did not reach it";
                    ALPACA_LOG_WARN("SynScan", message);
                    return message;
                };
                hooks.complete = [&] {
                    if (superseded()) {
                        return;
                    }
                    // The measurement is read with mutex_ released and published together with AtPark and
                    // Slewing, in one locked step.
                    publish_measurement_released(lock, superseded);
                    if (superseded()) {
                        return;
                    }
                    set_slewing_locked(false);
                    slew_hold_.clear();
                    guide_position_valid_ = false;
                    parked_ = true;
                    parking_ = false;
                };
                util::run_park_slew(ctx, lock, clock_, slew_settle_time_seconds_, hooks);
            },
            displaced,
            [&] {
                parking_ = false;
                set_slewing_locked(prev_slewing);
                at_home_ = prev_at_home;
                manual_axis_slewing_[0] = prev_manual[0];
                manual_axis_slewing_[1] = prev_manual[1];
                position_override_until_ = prev_override;
                slew_hold_.clear();
                guide_position_valid_ = prev_guide_valid;
            });
    }

    // Stop a park slew: cancel the GOTO, then both axes to rate 0. Each stop
    // is tried on its own so one failure does not skip the others. Returns
    // the first failure's message, or an empty string when every stop was
    // answered (#742). mutex_ must be held.
    std::string stop_park_slew_locked() { return stop_park_slew_wire(); }
    static std::string stop_park_slew_wire() { return util::stop_goto_and_axes(SynScanProtocolWrapper::instance()); }

    // Park task failure path: stop the hardware, then drop the parking state so
    // the caller can retry. A successful stop publishes Slewing=false; a failed
    // stop leaves a persistent Slewing error because the mount may still move.
    // mutex_ must be held.
    void fail_park_locked(const std::string& message, util::OperationContext& ctx, std::unique_lock<std::mutex>& lock) {
        // A superseded body leaves the axes to the initiator that replaced it: its stop would end that motion.
        if (ctx.stop_reason() == util::StopReason::Superseded) {
            return;
        }
        std::string stop_error;
        if (!send_in_turn(
                lock, [&ctx] { return ctx.stop_reason() != util::StopReason::Superseded; },
                [&stop_error] { stop_error = stop_park_slew_wire(); })) {
            return;
        }
        if (!connected_ || ctx.stop_reason() != util::StopReason::None) {
            return;
        }
        parking_ = false;
        last_slew_error_ = message;
        if (stop_error.empty()) {
            set_slewing_locked(false);
        } else {
            last_slew_error_ += "; stopping after failure failed: " + stop_error + "; the mount may still be moving";
            ALPACA_LOG_ERROR("SynScan",
                             "stop after park failure failed: " + stop_error + "; the mount may still be moving");
        }
        slew_hold_.clear();
        position_override_until_ = util::TaskClock::clock::time_point::min();
        ALPACA_LOG_WARN("SynScan", message);
    }

    void pulse_guide(int direction, int duration) override {
        util::check_pulse_guide_arguments(kCommandLimits, direction, duration);
        // 0 North and 1 South move Dec (axis 1); 2 East and 3 West move RA (axis 0).
        const int axis = direction <= 1 ? 1 : 0;
        const double direction_sign = (direction == 0 || direction == 3) ? 1.0 : -1.0;

        std::lock_guard<std::mutex> ilock(initiator_mutex_);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            util::check_pulse_guide_state(kCommandLimits, GateChecks{*this, "PulseGuide"});
            // open-astro#775: ITelescopeV4 raises InvalidOperation for a
            // PulseGuide while a slew is in progress. The gate asks the handset, with mutex_ released.
            if (get_slewing_fresh(lock)) {
                throw AlpacaException("PulseGuide is not allowed while the mount is slewing",
                                      AlpacaError::InvalidOperation);
            }
        }
        // A pulse already running on this axis is not joined (decision 0006 rule 3, #2263): this pulse supersedes it
        // once its own rate command has landed (below), and a refusal before then leaves it running to its own end.
        {
            std::unique_lock<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("PulseGuide");
            auto& protocol = SynScanProtocolWrapper::instance();
            // One wire turn spans the mode read and the rate command, each sent with mutex_ released: a Tracking `T`
            // takes a ticket, so it cannot land between them, and the rate uses the mode just read. The turn wait
            // released mutex_, so the checks run again at the turn; initiator_mutex_ keeps a new slew out, and the
            // slewing gate needs no second `L`.
            const WireTurn turn(*this, lock);
            check_connected();
            check_not_parked_locked("PulseGuide");
            if (slewing_by_driver_state_locked() || slewing_cached_) {
                throw AlpacaException("PulseGuide is not allowed while the mount is slewing",
                                      AlpacaError::InvalidOperation);
            }
            if (axis == 0) {
                refresh_tracking_mode_locked(lock);  // the pulse end restores this mode
            }
            const double guide_rate = axis == 1 ? guide_rate_.dec : guide_rate_.ra;
            // RA: West (direction_sign +1) lowers RA, East raises it.
            double slew_rate_deg_per_sec =
                axis == 0 ? ra_motor_rate_locked(-direction_sign * guide_rate * 3600.0) : direction_sign * guide_rate;
            if (axis == 1) {
                char pier = without_lock(lock, [&protocol] { return protocol.get_pointing_state(); });
                check_connected();
                if (pier == 'W') {
                    slew_rate_deg_per_sec = -slew_rate_deg_per_sec;
                }
            }

            const double duration_sec = duration / 1000.0;
            const auto now = clock_.now();

            // Initialize the guide-position estimate from mount position when its override expires.
            double estimate_ra_hours = guide_position_ra_hours_;
            double estimate_dec_degrees = guide_position_dec_degrees_;
            if (!guide_position_valid_ || now >= position_override_until_) {
                const auto equatorial = served_equatorial_locked();
                estimate_ra_hours = equatorial.first;
                estimate_dec_degrees = equatorial.second;
            }

            // Accumulate expected pulse delta in the private guide-position estimate.
            if (direction == 0 || direction == 1) {
                double delta_deg = guide_rate_.dec * duration_sec;
                if (direction == 1) delta_deg = -delta_deg;
                estimate_dec_degrees = std::clamp(estimate_dec_degrees + delta_deg, -90.0, 90.0);
            } else {
                double delta_hours = (guide_rate_.ra * duration_sec) / 15.0;
                if (direction == 3) delta_hours = -delta_hours;
                estimate_ra_hours = std::fmod(estimate_ra_hours + delta_hours, 24.0);
                if (estimate_ra_hours < 0.0) estimate_ra_hours += 24.0;
            }

            // Decision 0022 rule 2: the window is armed before the rate command goes out, so IsPulseGuiding reads true
            // from the moment the axis can move. Armed after the mode and pointing-state reads above: an armed RA
            // window makes refresh_tracking_mode_locked() skip its read. A failed command puts back the window from
            // before the call: none, a finished pulse's completion delay, which stays, or the window of an older
            // pulse still running, which a failed command does not supersede.
            const auto axis_index = static_cast<std::size_t>(axis);
            const util::HoldWindow previous_window = pulse_hold_[axis_index];
            pulse_hold_[axis_index].arm(now, std::chrono::milliseconds(duration) + kPulseGuideCompletionDelay);
            try {
                without_lock(lock, [&] { protocol.move_axis_variable_rate(axis, slew_rate_deg_per_sec); });
            } catch (...) {
                pulse_hold_[axis_index] = previous_window;
                throw;
            }
            // The rate has landed and this turn is still held: an older pulse on this axis, parked in its wait or
            // queued behind this turn for its end, has sent nothing since this call began. From here on it sends this
            // axis nothing and touches no state; this pulse owns the axis and its tracking restore.
            supersede_pulse_locked(axis);

            // Published after the command: a getter never sees the estimate without the pulse, and a failed command
            // publishes nothing. Nothing else writes this state meanwhile (initiator_mutex_, the slewing gate).
            guide_position_ra_hours_ = estimate_ra_hours;
            guide_position_dec_degrees_ = estimate_dec_degrees;
            guide_position_valid_ = true;
            // Keep position override active so get_ra/get_dec return the estimate. The estimate is
            // shared by both axes, so a pulse never shortens a hold another axis still needs (#990).
            position_override_until_ =
                std::max(position_override_until_, now + std::chrono::milliseconds(duration) +
                                                       kPulseGuideCompletionDelay + kPulseGuidePositionGrace);
            drop_poll_in_flight_locked();
        }

        // Each axis has its own slot; the supersede above happened before the turn
        // was released, so the old timer cannot stop this replacement pulse.
        try {
            pulse_ops_[axis].start(
                [this, axis, duration](util::OperationContext& ctx) { run_pulse_stop_body(ctx, axis, duration); });
        } catch (...) {
            // No timer will stop the axis the rate command just started: stop it
            // and drop the pulse state before the refusal reaches the client.
            std::unique_lock<std::mutex> lock(mutex_);
            const auto axis_index = static_cast<std::size_t>(axis);
            pulse_hold_[axis_index].clear();
            position_override_until_ = util::TaskClock::clock::time_point::min();
            if (connected_) {
                try {
                    // Stopping RA ends sidereal tracking too: restore the mode read at the turn, as AbortSlew does.
                    int restore_mode = 0;
                    std::string restore_error;
                    send_in_turn(
                        lock,
                        [this, axis, &restore_mode] {
                            restore_mode = axis == 0 ? tracking_mode_cached_ : 0;
                            return static_cast<bool>(connected_);
                        },
                        [axis, &restore_mode, &restore_error] {
                            auto& protocol = SynScanProtocolWrapper::instance();
                            protocol.move_axis_variable_rate(axis, 0.0);
                            if (restore_mode > 0) {
                                try {
                                    protocol.set_tracking_mode(restore_mode);
                                } catch (const std::exception& e) {
                                    restore_error = e.what();  // the stop landed: do not report it as failed
                                }
                            }
                        });
                    if (!restore_error.empty()) {
                        ra_stop_unconfirmed_ = true;  // Tracking reads false until a restore lands
                        ALPACA_LOG_ERROR("SynScan", "PulseGuide tracking restore after a refused timer start failed: " +
                                                        restore_error);
                    } else if (restore_mode > 0) {
                        // Written through, so a poll sampled during the RA stop (tracking off) cannot publish over it.
                        set_tracking_mode_cached_locked(restore_mode);
                        ra_stop_unconfirmed_ = false;
                        invalidate_readback_top_locked();
                    }
                } catch (...) {
                    manual_axis_slewing_[axis] = true;  // the axis may still be moving
                    ALPACA_LOG_ERROR("SynScan", "PulseGuide stop after a refused timer start failed on axis " +
                                                    std::to_string(axis) + "; the axis may still be moving");
                }
            }
            throw;
        }
    }

    // Body of the per-axis pulse slot: waits out the pulse, then stops the axis.
    void run_pulse_stop_body(util::OperationContext& ctx, int axis, int duration) {
        {
            const auto axis_index = static_cast<std::size_t>(axis);
            if (!ctx.wait_for(std::chrono::milliseconds(duration))) {
                // Superseded by a replacement pulse or a moving MoveAxis on this axis, or by a slew or Park
                // (supersede_pulse_locked()): the newer command owns the axis, so this body sends nothing and touches
                // no state (decision 0006 rule 6, #2263). Its rate is already replaced on the wire, or the slew or Park
                // sends the stop it owed (supersede_pulses_for_slew_locked(), #2416).
                if (ctx.stop_reason() == util::StopReason::Superseded) {
                    return;
                }
                // Cancelled by MoveAxis(axis, 0), AbortSlew, disconnect or
                // destruction. Still make a best-effort attempt to stop the
                // axis before exiting — the mount would otherwise keep slewing.
                // AbortSlew restores tracking itself; teardown must not. A failed stop is logged and the axis
                // marked as possibly moving, as the refused-start path does (#1725); the caller's own stop or
                // teardown goes on and the error is not stored for the next command.
                // Sent with no WireTurn, unlike the refused-start stop in pulse_guide: every initiator that cancels
                // this slot joins it before it takes its own turn, so this stop goes out before any command of the
                // initiator that cancelled it. It can still fall inside the other axis's pulse-end turn, which sends
                // nothing for this axis.
                std::string stop_error;
                try {
                    SynScanProtocolWrapper::instance().move_axis_variable_rate(axis, 0.0);
                } catch (const std::exception& e) {
                    stop_error = e.what();
                } catch (...) {
                    stop_error = "unknown error";
                }
                if (!stop_error.empty()) {
                    ALPACA_LOG_ERROR("SynScan", "PulseGuide stop after a cancelled pulse failed on axis " +
                                                    std::to_string(axis) +
                                                    "; the axis may still be moving: " + stop_error);
                }
                std::lock_guard<std::mutex> lock(mutex_);
                if (connected_) {
                    // The window stays armed for the canceller, which clears it (release_cut_short_pulse_locked()).
                    pulse_cut_short_[axis_index] = true;
                    if (!stop_error.empty()) {
                        manual_axis_slewing_[axis] = true;
                    }
                }
                return;
            }
            // The stop command MUST land: silently swallowing a failure leaves
            // the axis slewing at guide rate indefinitely (mount runaway).
            // Retry a few times; on final failure log loudly and flag the axis
            // as still moving so Slewing reports the true state.
            constexpr int kStopAttempts = 3;
            bool stopped = false;
            std::string last_error;
            auto& proto = SynScanProtocolWrapper::instance();
            // An RA pulse ran the motor at an absolute rate in place of tracking; while tracking, the tracking
            // command alone ends it (see ra_motor_rate_locked). Without the equatorial model the pulse ran the guide
            // rate alone, so it stops and then restores tracking as before.
            int end_tracking_mode = 0;
            for (int attempt = 0; attempt < kStopAttempts && !stopped; ++attempt) {
                try {
                    // The stop and the restore go out in one wire turn, and the mode to restore is read at the turn:
                    // a Tracking write queued ahead of this body has landed by then and is the mode that stays, and
                    // none can land between the stop and the restore (#2069).
                    std::unique_lock<std::mutex> lock(mutex_);
                    const WireTurn turn(*this, lock);
                    // Superseded while it waited for the turn: the newer command's rate went out first and owns the
                    // axis, so a stop now would end that motion (#2263).
                    if (ctx.stop_reason() == util::StopReason::Superseded) {
                        return;
                    }
                    bool end_with_tracking = false;
                    if (axis == 0) {
                        end_tracking_mode = connected_ ? tracking_mode_cached_ : 0;
                        end_with_tracking = end_tracking_mode > 0 && equatorial_geometry_locked();
                    }
                    const int restore_mode = end_tracking_mode;
                    try {
                        without_lock(lock, [&] {
                            if (end_with_tracking) {
                                proto.set_tracking_mode(restore_mode);
                            } else {
                                proto.move_axis_variable_rate(axis, 0.0);
                                if (restore_mode > 0) {
                                    proto.set_tracking_mode(restore_mode);
                                }
                            }
                        });
                    } catch (...) {
                        // The restore may not have landed: keep the captured mode for AbortSlew/MoveAxis(0) to
                        // restore; Tracking reports false until one of them, a Park, a disconnect or a Tracking write
                        // clears this. Set under the turn, as MoveAxis does, so no later writer is overtaken.
                        if (connected_ && restore_mode > 0) {
                            ra_stop_unconfirmed_ = true;
                        }
                        throw;
                    }
                    // Written through, as AbortSlew's restore: a poll sampled mid-pulse read tracking off. Still in
                    // the turn and under mutex_, so a Tracking write queued behind this body cannot be overwritten.
                    if (axis == 0 && connected_ && restore_mode > 0) {
                        set_tracking_mode_cached_locked(restore_mode);
                        ra_stop_unconfirmed_ = false;
                        // A same-mode T leaves the handset's readback regime as it is (measured); the pulse moved RA
                        // by the estimate the override already reports.
                        if (guide_position_valid_) {
                            readback_top_hours_ = guide_position_ra_hours_;
                        } else {
                            invalidate_readback_top_locked();
                        }
                    }
                    stopped = true;
                } catch (const std::exception& e) {
                    last_error = e.what();
                } catch (...) {
                    last_error = "unknown error";
                }
                if (!stopped && !ctx.wait_for(std::chrono::milliseconds(100))) {
                    break;
                }
            }
            if (!stopped) {
                // In a wire turn, with the Superseded check made at the turn as each attempt makes it: a replacement
                // pulse or a moving MoveAxis arms its window and releases mutex_ for its rate command, and supersedes
                // this body only once that rate has landed. Under mutex_ alone this tail could run in that gap and
                // clear the newer window or flag the newer motion as a runaway (decision 0022 rule 2, #2263). A newer
                // command that took the axis owns the window and the axis, so the body touches neither.
                std::unique_lock<std::mutex> lock(mutex_);
                const WireTurn turn(*this, lock);
                if (ctx.stop_reason() == util::StopReason::Superseded) {
                    return;
                }
                ALPACA_LOG_ERROR("SynScan", "PulseGuide STOP FAILED after " + std::to_string(kStopAttempts) +
                                                " attempts on axis " + std::to_string(axis) +
                                                " — axis may still be moving (mount runaway risk): " + last_error);
                pulse_hold_[axis_index].clear();
                // Surface the error state: report the axis as slewing until an
                // AbortSlew/MoveAxis(0) or disconnect clears it.
                if (connected_) {
                    manual_axis_slewing_[axis] = true;
                }
            }
        }
    }

    void set_park() override {
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        store_park_position(lock);
    }

    void slew_to_coordinates(double ra, double dec) override {
        // InvalidValue precedes the connection check.
        util::check_equatorial_slew_arguments(ra, dec, "SlewToCoordinates");
        std::unique_lock<std::mutex> ilock(initiator_mutex_);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            util::check_equatorial_slew_state(kCommandLimits, GateChecks{*this, "SlewToCoordinates", &lock});
        }
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("SlewToCoordinates");
        supersede_pulses_for_slew_locked(lock);
        check_connected();
        clear_pulse_guiding_locked();
        claim_slew_slot_locked();
        parking_ = false;  // a park body in flight no longer clears it: the claim ended it
        const util::MotionClaim claim = claim_motion_locked(util::MotionCommand::Slew);
        do_slew_to_coordinates_locked(ra, dec, lock, [&] { return !claim_lost_locked(claim) && connected_; }, true);
        // Slewing stays true from the first GOTO to the end of the last refinement pass (open-astro#880).
        const RefineScope refine_scope(*this, claim, !use_precise_commands_);
        ilock.unlock();
        if (!wait_for_slew_complete(lock, claim)) {
            throw AlpacaException("Slew superseded by a concurrent motion command", AlpacaError::InvalidOperation);
        }
        if (!refine_goto_landing_locked(lock, ra, dec, claim, nullptr) || !settle_after_slew_locked(lock, claim)) {
            throw AlpacaException("Slew superseded by a concurrent motion command", AlpacaError::InvalidOperation);
        }
    }

    void slew_to_coordinates_async(double ra, double dec) override {
        // Argument validation precedes the connection and state checks (ASCOM contract).
        util::check_equatorial_slew_arguments(ra, dec, "SlewToCoordinatesAsync");
        std::lock_guard<std::mutex> ilock(initiator_mutex_);  // see park()
        {
            // Gate BEFORE claiming: the claim ends a park in flight (clearing
            // parking_), so a slew this gate refuses must not have claimed.
            std::unique_lock<std::mutex> lock(mutex_);
            util::check_equatorial_slew_state(kCommandLimits, GateChecks{*this, "SlewToCoordinatesAsync", &lock});
        }
        uint32_t ra_raw = 0;
        uint32_t dec_raw = 0;
        bool precise = false;
        util::MotionClaim claim;
        bool displaced = false;
        bool prev_slewing = false;
        bool prev_at_home = false;
        bool prev_guide_valid = false;
        bool prev_manual[2] = {false, false};
        std::optional<util::MotionClaim> prev_refining;
        auto prev_override = util::TaskClock::clock::time_point::min();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("SlewToCoordinatesAsync");
            const bool pulse_stopped = supersede_pulses_for_slew_locked(lock);
            check_connected();
            displaced = claim_slew_slot_locked() || pulse_stopped;
            parking_ = false;  // a park body in flight no longer clears it: the claim ended it
            claim = claim_motion_locked(util::MotionCommand::Slew);
            prev_refining = refining_claim_;
            if (use_precise_commands_) {
                refining_claim_ = claim;
            }

            int bits = use_precise_commands_ ? 24 : 16;
            ra_raw = encode_ra_raw(ra, bits);
            dec_raw = encode_angle(dec, bits);
            precise = use_precise_commands_;

            drop_poll_in_flight_locked();
            prev_slewing = slewing_cached_;
            prev_at_home = at_home_;
            prev_guide_valid = guide_position_valid_;
            prev_manual[0] = manual_axis_slewing_[0];
            prev_manual[1] = manual_axis_slewing_[1];
            prev_override = position_override_until_;
            set_slewing_locked(true);
            // open-astro#575: a fresh initiator is a clean start -- a client
            // that retries a rejected goto must not be told the OLD goto
            // failed.
            last_slew_error_.clear();
            slew_hold_.arm(clock_.now(), std::chrono::seconds(8));
            position_override_until_ = util::TaskClock::clock::time_point::min();
            guide_position_valid_ = false;
            target_ra_hours_ = ra;
            target_dec_degrees_ = dec;
            target_ra_set_ = true;
            target_dec_set_ = true;
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            parked_ = false;
            at_home_ = false;
            clear_pulse_guiding_locked();
        }

        start_slew_body(
            [this, ra, dec, ra_raw, dec_raw, precise, claim](util::OperationContext& ctx) {
                std::unique_lock<std::mutex> lock(mutex_);
                const RefineScope refine_scope(*this, claim,
                                               true);  // armed by the initiator; this only clears
                if (!connected_ || ctx.stop_reason() != util::StopReason::None) {
                    return;
                }
                try {
                    // Sent in its turn with mutex_ released: a newer initiator must not wait for this body, and the
                    // GOTO never goes out behind the commands of the initiator that replaced the body.
                    if (!send_in_turn(
                            lock, [&] { return !slot_stopped(&ctx); },
                            [&] { SynScanProtocolWrapper::instance().goto_ra_dec_raw(ra_raw, dec_raw, precise); }) ||
                        slot_stopped(&ctx)) {
                        return;  // superseded (or cancelled) before or during the exchange: the newer operation owns
                                 // the state
                    }
                    // Slewing stays true across the refinement passes: the first wait ends when the handset reports
                    // the GOTO done, then the landing is read back and corrected (open-astro#880).
                    if (precise && wait_for_slew_complete(lock, claim, &ctx)) {
                        refine_goto_landing_locked(lock, ra, dec, claim, &ctx);
                    }
                } catch (const SlewFailureKept&) {
                    // A timed-out wait stopped the mount and kept its error (#2412): Slewing and the error stand.
                    return;
                } catch (const std::exception& ex) {
                    if (ctx.stop_reason() == util::StopReason::Superseded) {
                        // The newer initiator owns Slewing, the force window and the error text.
                        ALPACA_LOG_WARN("SynScan", std::string("Superseded slew dispatch failed: ") + ex.what());
                        return;
                    }
                    set_slewing_locked(false);
                    slew_hold_.clear();
                    position_override_until_ = util::TaskClock::clock::time_point::min();
                    // open-astro#575: a stop by a newer initiator is not a failure -- that
                    // initiator already owns clearing/replacing last_slew_error_.
                    // Recording an error after cancellation would poison the NEXT Slewing
                    // read with an artifact of the cancel, not a real fault.
                    if (ctx.stop_reason() == util::StopReason::None) {
                        last_slew_error_ = std::string("SlewToCoordinatesAsync failed: ") + ex.what();
                    }
                    ALPACA_LOG_WARN("SynScan", std::string("Async slew dispatch failed: ") + ex.what());
                } catch (...) {
                    if (ctx.stop_reason() == util::StopReason::Superseded) {
                        ALPACA_LOG_WARN("SynScan", "Superseded slew dispatch failed with unknown exception");
                        return;
                    }
                    set_slewing_locked(false);
                    slew_hold_.clear();
                    position_override_until_ = util::TaskClock::clock::time_point::min();
                    if (ctx.stop_reason() == util::StopReason::None) {
                        last_slew_error_ = "SlewToCoordinatesAsync failed with an unknown error";
                    }
                    ALPACA_LOG_WARN("SynScan", "Async slew dispatch failed with unknown exception");
                }
            },
            displaced,
            [&] {
                set_slewing_locked(prev_slewing);
                at_home_ = prev_at_home;
                manual_axis_slewing_[0] = prev_manual[0];
                manual_axis_slewing_[1] = prev_manual[1];
                position_override_until_ = prev_override;
                slew_hold_.clear();
                guide_position_valid_ = prev_guide_valid;
                refining_claim_ = prev_refining;
            });
    }

    void slew_to_target() override {
        // Its async sibling below has always required the pair; this one did
        // not, so SlewToTarget with nothing set slewed to whatever was in the
        // members -- 0h/0deg on a fresh connect. Found while splitting the
        // flag for open-astro#346: with one flag the omission was invisible,
        // since any target write at all made the check pass.
        double ra, dec;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();  // before the unset target (#2075)
            if (!target_ra_set_ || !target_dec_set_) {
                throw AlpacaException("Target coordinates have not been set", AlpacaError::ValueNotSet);
            }
            ra = target_ra_hours_;
            dec = target_dec_degrees_;
        }
        slew_to_coordinates(ra, dec);
    }

    void slew_to_target_async() override {
        double ra, dec;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();  // before the unset target (#2075)
            if (!target_ra_set_ || !target_dec_set_) {
                throw AlpacaException("Target coordinates have not been set", AlpacaError::ValueNotSet);
            }
            ra = target_ra_hours_;
            dec = target_dec_degrees_;
        }
        slew_to_coordinates_async(ra, dec);
    }

    // An initiator: no Park, slew, MoveAxis, AbortSlew, PulseGuide or disconnect starts between its checks and its
    // write. The `s` goes out in its wire turn with mutex_ released, so it follows any motion command a body has
    // already committed to send, and the checks run again at the turn.
    void sync_to_coordinates(double ra, double dec) override {
        util::validate_ra_dec(ra, dec, "SyncToCoordinates");  // InvalidValue precedes the connection check
        std::lock_guard<std::mutex> ilock(initiator_mutex_);
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("SyncToCoordinates");
        auto& protocol = SynScanProtocolWrapper::instance();
        int bits = use_precise_commands_ ? 24 : 16;
        uint32_t ra_raw = encode_ra_raw(ra, bits);
        uint32_t dec_raw = encode_angle(dec, bits);
        send_in_turn(
            lock,
            [this] {
                check_connected();
                check_not_parked_locked("SyncToCoordinates");
                return true;
            },
            [&] { protocol.sync_ra_dec_raw(ra_raw, dec_raw, use_precise_commands_); });
        if (readback_top_valid_) {
            readback_top_hours_ = ra;
        }
        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_ra_set_ = true;
        target_dec_set_ = true;
        guide_position_ra_hours_ = ra;
        guide_position_dec_degrees_ = dec;
        guide_position_valid_ = true;
        position_override_until_ = clock_.now() + std::chrono::seconds(10);
        // Write-through: the poll in flight sampled before the sync and is dropped.
        polled_.write([ra, dec](PolledFrame& frame) {
            frame.ra_hours = ra;
            frame.dec_degrees = dec;
            frame.equatorial_known = true;
        });
    }

    void sync_to_target() override {
        double ra, dec;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();  // before the unset target (#2075)
            if (!target_ra_set_ || !target_dec_set_) {
                throw AlpacaException("Target coordinates have not been set", AlpacaError::ValueNotSet);
            }
            ra = target_ra_hours_;
            dec = target_dec_degrees_;
        }
        sync_to_coordinates(ra, dec);
    }

    void unpark() override {
        std::lock_guard<std::mutex> ilock(initiator_mutex_);
        bool was_parking = false;
        std::string stop_error;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            check_connected();
            parked_ = false;
            was_parking = parking_;
            if (was_parking) {
                // Unpark during a park wins the race: supersede the park body,
                // stop the park slew and drop the parking state. A failed stop leaves Slewing at its cached value,
                // since the park slew may still be running.
                parking_ = false;
                claim_slew_slot_locked();  // the park body is superseded; it is not joined
                cancel_motion_locked(util::MotionOwner::kBothAxes);  // a stop: it claims nothing (#2263)
                // The stop goes out after anything the park body is committed to send, and the body sends nothing
                // after the claim. initiator_mutex_ keeps every other initiator out until it has gone.
                if (!send_in_turn(
                        lock, [&] { return static_cast<bool>(connected_); },
                        [&stop_error] { stop_error = stop_park_slew_wire(); })) {
                    check_connected();
                    stop_error = "the stop was not sent";
                }
                if (stop_error.empty()) {
                    set_slewing_locked(false);
                }
                slew_hold_.clear();
                position_override_until_ = util::TaskClock::clock::time_point::min();
            }
            if (stop_error.empty()) {
                last_slew_error_.clear();
            } else {
                last_slew_error_ = "Unpark could not stop the park slew: " + stop_error;
            }
        }
        if (!stop_error.empty()) {
            throw AlpacaException("Unpark could not stop the park slew: " + stop_error, AlpacaError::DriverException);
        }
    }

    bool get_can_move_axis(int axis) const override {
        if (axis < 0 || axis > 2) {
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }
        return axis == 0 || axis == 1;
    }

    void move_axis(int axis, double rate) override {
        util::check_move_axis_arguments(kCommandLimits, axis, rate);
        const bool moving = !util::is_move_axis_stop(kCommandLimits, rate);
        std::lock_guard<std::mutex> ilock(initiator_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            util::check_move_axis_state(kCommandLimits, /*stop=*/!moving, GateChecks{*this, "MoveAxis"});
        }
        // A stop is a stop path: it cancels and joins the pulse on its axis, whose body takes mutex_. A moving
        // MoveAxis supersedes that pulse instead, without a join, once its own rate has landed (below; decision 0006
        // rule 3, #2263). A pulse on the other axis runs on either way.
        if (!moving) {
            pulse_ops_[axis].cancel_all_and_join();
        }
        std::unique_lock<std::mutex> lock(mutex_);
        // A refusal or a failed send below clears the window of a pulse the stop's join just cut short.
        const CutShortPulseRelease cut_short(*this, axis);
        check_connected();
        check_not_parked_locked("MoveAxis");
        claim_slew_slot_locked();  // supersedes a slew or park body in flight; it is not joined
        parking_ = false;          // a park body no longer clears it: the claim ended it
        // A moving MoveAxis claims its own axis, which also supersedes a slew or park (they own both); a stop claims
        // nothing and cancels its axis (#2263).
        std::optional<util::MotionClaim> claim;
        if (moving) {
            claim = claim_motion_locked(util::MotionCommand::MoveAxis, axis);
        } else {
            cancel_motion_locked(util::MotionOwner::axes_of(util::MotionCommand::MoveAxis, axis));
        }

        if (axis == 0) {
            // Before manual_axis_slewing_ changes; the stop restores this mode. A failed read keeps the cache on a
            // stop, which must still be sent; a start has moved nothing yet, so it can throw.
            if (moving) {
                refresh_tracking_mode_locked(lock);
            } else {
                try {
                    refresh_tracking_mode_locked(lock);
                } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): the stop is still sent
                }
            }
        }
        manual_axis_slewing_[axis] = moving;
        if (axis == 0) {
            ra_stop_unconfirmed_ = false;
        }
        if (moving) {
            parked_ = false;
            at_home_ = false;
        }
        // open-astro#575: a fresh initiator is a clean start -- a client
        // that jogs an axis after a failed GOTO must not be told the OLD
        // goto failed.
        if (axis == 0) {
            invalidate_readback_top_locked();  // the jog moves RA by an unknown amount
        }
        auto& protocol = SynScanProtocolWrapper::instance();
        // A variable-rate P slew replaces tracking on the handset, so the stop leaves the RA motor stopped; ASCOM
        // resumes the tracking rate when MoveAxis stops (see ra_motor_rate_locked). The mode is read at the turn, as
        // AbortSlew's: a Tracking write queued ahead has sent its `T` by then.
        int restore_mode = 0;
        try {
            if (!send_in_turn(
                    lock,
                    [&] {
                        restore_mode = !moving && axis == 0 ? tracking_mode_cached_ : 0;
                        return (!claim || !claim_lost_locked(*claim)) && connected_;
                    },
                    [&] {
                        protocol.move_axis_variable_rate(axis, moving ? rate : 0.0);
                        if (restore_mode > 0) {
                            protocol.set_tracking_mode(restore_mode);
                        }
                    })) {
                check_connected();
                return;  // overtaken before its turn (disconnect): the rate was never sent
            }
            if (moving) {
                // The rate has landed and mutex_ has been held since: the pulse body on this axis, queued behind this
                // turn or parked in its wait, has not run in between. It now sends this axis nothing; on a failed
                // send it was never superseded, so its own end still stops the axis.
                supersede_pulse_locked(axis);
            }
        } catch (...) {
            // A restore that did not land leaves the RA motor stopped under a cached mode (#1722): Tracking reads
            // false.
            if (restore_mode > 0) {
                ra_stop_unconfirmed_ = true;
            }
            throw;
        }
        if (restore_mode > 0) {
            set_tracking_mode_cached_locked(restore_mode);  // written through, as AbortSlew's restore
        }
        last_slew_error_.clear();
        set_slewing_locked(false);
        slew_hold_.clear();
        position_override_until_ = util::TaskClock::clock::time_point::min();
        pulse_hold_[static_cast<std::size_t>(axis)].clear();
        pulse_cut_short_[static_cast<std::size_t>(axis)] = false;  // this MoveAxis owns the axis (and its restore)
    }

    std::pair<double, double> get_axis_rate_range(int axis) const override {
        if (axis != 0 && axis != 1) {
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }
        return {0.0, kMaxMoveAxisRateDegPerSec};
    }

    std::vector<std::pair<double, double>> get_axis_rate_ranges(int axis) const override {
        if (axis == 2) {
            // Tertiary axis is not supported; return an empty range set per ASCOM semantics.
            return {};
        }
        if (axis != 0 && axis != 1) {
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }
        return {{0.0, kMaxMoveAxisRateDegPerSec}};
    }

    void abort_slew() override {
        std::lock_guard<std::mutex> ilock(initiator_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_fully_parked_locked("AbortSlew");  // AbortSlew may cancel a park in flight
            claim_slew_slot_locked();                    // supersedes the slew or park body; it is not joined
            // A stop claims nothing (#2263). The cancel also ends a synchronous slew's landing nudge at once, so the
            // stops below wait for its stop only.
            cancel_motion_locked(util::MotionOwner::kBothAxes);
        }
        join_pulse_bodies();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // A failed stop below leaves the slew state as it was, but clears the windows of pulses the join cut
            // short: those pulses are over either way.
            const CutShortPulseRelease cut_short(*this, -1);
            check_connected();
            check_not_fully_parked_locked("AbortSlew");
            // The wrapper serializes transactions, so a GOTO already in flight
            // must finish (or time out) before these stops can reach the mount.
            auto& protocol = SynScanProtocolWrapper::instance();
            // With nothing running, the cache may predate a keypad change: read the handset's mode first. A
            // failed read keeps the cache, since the stops below must still be sent.
            try {
                refresh_tracking_mode_locked(lock);
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): the stops are still sent
            }
            // Try every stop on its own, so one lost command cannot skip the others (#781).
            std::string stop_error;
            const auto try_stop = [&stop_error](const std::function<void()>& stop) {
                try {
                    stop();
                } catch (const std::exception& ex) {
                    if (stop_error.empty()) {
                        stop_error = ex.what();
                    }
                } catch (...) {
                    if (stop_error.empty()) {
                        stop_error = "unknown exception";
                    }
                }
            };
            // The stops and the tracking restore go out in one wire turn: after anything an older body is committed
            // to send, and before nothing, since a body sends nothing once the claim above is visible to it. The
            // restore (below) runs only when every stop was answered. Its mode is read at the turn: a Tracking
            // write queued ahead of this turn has sent its `T` by then, and the mode it asked for is the one kept.
            int restore_mode = 0;
            std::exception_ptr restore_error;
            if (!send_in_turn(
                    lock,
                    [&] {
                        restore_mode = tracking_mode_cached_;
                        return static_cast<bool>(connected_);  // initiator_mutex_ keeps every other initiator out
                    },
                    [&] {
                        try_stop([&protocol]() { protocol.cancel_goto(); });
                        try_stop([&protocol]() { protocol.move_axis_fixed_rate(0, 0); });
                        try_stop([&protocol]() { protocol.move_axis_fixed_rate(1, 0); });
                        if (stop_error.empty() && restore_mode > 0) {
                            try {
                                protocol.set_tracking_mode(restore_mode);
                            } catch (...) {
                                restore_error = std::current_exception();
                            }
                        }
                    })) {
                check_connected();
                stop_error = "the stops were not sent";
            }
            // An aborted park never reaches AtPark, even when a stop failed: the claim above superseded the park
            // body, whose abandon hook (park()) then leaves parking_ alone, so nothing else would end it.
            parking_ = false;
            if (!stop_error.empty()) {
                last_slew_error_ = "AbortSlew stop failed: " + stop_error;
                // The mount may still be moving: leave Slewing and the rest of the slew state as they were.
                throw AlpacaException("AbortSlew stop failed: " + stop_error, AlpacaError::DriverException);
            }
            cancel_motion_locked(util::MotionOwner::kBothAxes);
            set_slewing_locked(false);
            clear_pulse_guiding_locked();
            // open-astro#575: AbortSlew is a valid clearing command for a stored
            // slew failure -- the client acted on the error, so the next Slewing
            // read must answer normally again.
            last_slew_error_.clear();
            slew_hold_.clear();
            position_override_until_ = util::TaskClock::clock::time_point::min();
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            ra_stop_unconfirmed_ = false;
            if (restore_mode > 0) {
                // Stopping an RA guide pulse also stops sidereal tracking.
                // AbortSlew owns this stop, so restore the currently
                // requested mode only after both axes have stopped. The motion
                // state is already clear, so a throwing write cannot leave
                // Slewing true over stopped axes (#830).
                if (restore_error) {
                    ra_stop_unconfirmed_ = true;  // the RA motor may be stopped under the cached mode (#1722)
                    std::rethrow_exception(restore_error);
                }
                // Written through, so a poll sampled during the RA stop (tracking off) cannot publish over it.
                set_tracking_mode_cached_locked(restore_mode);
                invalidate_readback_top_locked();
            }
        }
    }

    void slew_to_alt_az(double altitude, double azimuth) override {
        (void)altitude;
        (void)azimuth;
        require_connected();
        throw AlpacaException("SlewToAltAz not supported", AlpacaError::MethodNotImplemented);
    }

    void slew_to_alt_az_async(double altitude, double azimuth) override {
        (void)altitude;
        (void)azimuth;
        require_connected();
        throw AlpacaException("SlewToAltAzAsync not supported", AlpacaError::MethodNotImplemented);
    }

    void sync_to_alt_az(double altitude, double azimuth) override {
        (void)altitude;
        (void)azimuth;
        require_connected();
        throw AlpacaException("SyncToAltAz not supported", AlpacaError::MethodNotImplemented);
    }

private:
    static int map_pointing_state_to_side(char side) {
        // SynScan 'W' = pointing west → OTA east of pier (HA > 0) → ASCOM pierEast (0).
        // SynScan 'E' = pointing east → OTA west of pier (HA < 0) → ASCOM pierWest (1).
        // TODO: Adjust mapping for southern hemisphere per SynScan pointing-state rules.
        if (side == 'W') {
            return 0;
        }
        if (side == 'E') {
            return 1;
        }
        return -1;
    }

    // Reads the handset clock once per session (the connect seeds it), so UTCDate in DeviceState costs no transaction.
    // `lock` (mutex_) is held on entry and on return; the `h` read runs with it released.
    void ensure_utc_cached(std::unique_lock<std::mutex>& lock) {
        if (!utc_offset_.valid()) {
            std::chrono::system_clock::time_point mount_utc;
            try {
                TimeInfo info = without_lock(lock, [] { return SynScanProtocolWrapper::instance().get_time(); });
                if (utc_offset_.valid()) {
                    return;  // a write landed during the read; it is newer than this reading
                }
                timezone_offset_minutes_ = info.timezone_offset_minutes;
                timezone_offset_valid_ = true;
                dst_observed_ = info.dst_enabled;

                using namespace std::chrono;
                int year = 2000 + info.year;
                sys_days date =
                    sys_days{std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(info.month)} /
                             std::chrono::day{static_cast<unsigned>(info.day)}};
                auto local_time = date + hours{info.hour} + minutes{info.minute} + seconds{info.second};
                int total_offset = info.timezone_offset_minutes + (info.dst_enabled ? 60 : 0);
                mount_utc = local_time - minutes{total_offset};
            } catch (...) {
                return;  // Leave time unset; getters never retry this connect-time sample.
            }
            utc_offset_.set(mount_utc, clock_.now());
        }
    }

    // The connect, under transition_mutex_ with mutex_ released across every exchange; each result is published in
    // its own short mutex_ step. connected_ stays false until the end, so a getter refuses with NotConnected while the
    // handshake runs; it is set last, after the first frame is held, so a getter that sees Connected sees identity,
    // site, UTC and a frame (decision 0016 points 3 and 4).
    void connect_session(SynScanProtocolWrapper& protocol) {
        // An auto-detected mount resolves its port here, not in the factory (#659).
        // The echo test lives INSIDE the retry lambda: connect() only opens the
        // port, and a stale auto-detected path that another adapter now owns
        // still opens, so an identity gate outside the lambda would count that
        // as "the resolved endpoint still answers" and never re-scan.
        util::connect_resolved(
            connection_info_, connection_resolved_, connection_resolver_,
            [&protocol](const ConnectionInfo& info) {
                if (!protocol.connect(info)) {
                    // The wrapper's reason (a port another device holds, #1202)
                    // goes to the operator.
                    const std::string reason = protocol.last_connect_error();
                    const std::string message = reason.empty() ? "Failed to connect to SynScan mount"
                                                               : "Failed to connect to SynScan mount: " + reason;
                    // Refused TCP connect, vanished serial node, or a port another connected
                    // device now holds (#1202): stale, re-scan.
                    if (info.type == alpacacore::vendor::synscan::ConnectionType::Network ||
                        util::serial_endpoint_stale(info.port_path)) {
                        throw util::StaleEndpoint(message);
                    }
                    throw AlpacaException(message);
                }
                if (!protocol.echo_test()) {
                    // Without this gate a link with nothing listening came up
                    // as Connected=true once every query below had burnt its
                    // full response timeout (all of them swallowed), and the
                    // client then saw each command time out in turn. Fail
                    // within one timeout, and say where to look. It is the
                    // identity gate: a port that opens but does not echo is
                    // not (or no longer) this handset, so it is stale.
                    protocol.disconnect();
                    const std::string where = info.type == alpacacore::vendor::synscan::ConnectionType::Serial
                                                  ? info.port_path
                                                  : info.host + ":" + std::to_string(info.tcp_port);
                    throw util::StaleEndpoint("SynScan hand controller did not answer the echo test on " + where +
                                              " - check that the cable is on the handset's PC port, the handset is "
                                              "powered and past its start-up prompts, and the baud rate is 9600");
                }
            },
            "SynScan");

        std::unique_lock<std::mutex> lock(mutex_);
        clear_session_state_locked();
        mount_firmware_version_ = "";
        mount_model_id_ = -1;
        probe_tick_ = 0;
        polled_.reset();

        try {
            mount_firmware_version_ =
                without_lock(lock, [&protocol] { return protocol.get_handset_firmware_version(); });
        } catch (...) {
            // TODO: Confirm SynScan firmware query reliability on all V3/V4 handsets.
            ALPACA_LOG_DEBUG("SynScan", "Handset firmware query failed at connect; firmware stays unknown");
        }
        // Publish to the web-UI getter under its OWN narrow mutex (a leaf).
        {
            std::lock_guard<std::mutex> fwlock(firmware_mutex_);
            firmware_cache_ = (mount_firmware_version_ != "0.0.0") ? mount_firmware_version_ : std::string();
        }
        try {
            mount_model_id_ = without_lock(lock, [&protocol] { return protocol.get_model_id(); });
        } catch (...) {
            // TODO: Confirm SynScan model query reliability on all V3/V4 handsets.
            ALPACA_LOG_DEBUG("SynScan", "Mount model query failed at connect; model stays unknown");
        }

        if (pending_site_latitude_.has_value() && pending_site_longitude_.has_value()) {
            LocationInfo loc;
            loc.latitude_degrees = pending_site_latitude_.value();
            loc.longitude_degrees = pending_site_longitude_.value();
            try {
                without_lock(lock, [&protocol, &loc] { protocol.set_location(loc); });
                site_latitude_cached_ = loc.latitude_degrees;
                site_longitude_cached_ = loc.longitude_degrees;
                site_info_valid_ = true;
            } catch (...) {
                // TODO: Confirm SynScan accepts location updates while aligned.
                ALPACA_LOG_WARN("SynScan", "Configured site was not written to the handset at connect");
            }
        }
        if (pending_site_elevation_.has_value()) {
            site_elevation_m_ = pending_site_elevation_;
        }
        if (sync_time_on_connect_) {
            // open-astro#2419: the host clock goes into the handset only while it is trusted (NTP-synchronised, or set
            // or confirmed in this process). A Pi with no RTC boots on the time of its last shutdown, and the handset
            // aims every GOTO by its clock. The site write above does not depend on it, and the mount clock is still
            // read below for UTCDate. on_host_clock_changed() writes it once the host clock is trusted.
            if (without_lock(lock, [] { return util::HostClockTrust::trusted(); })) {
                fail_if_link_released(protocol);  // set_time() on a released port would throw a plain DriverException
                sync_mount_time(lock);
            } else {
                ALPACA_LOG_WARN("SynScan",
                                "Host clock is not trusted (not NTP-synchronised, not set or confirmed by a client); "
                                "leaving the mount clock alone until the host clock is set or a client writes UTCDate");
            }
        }
        ensure_utc_cached(lock);
        try {
            LocationInfo info = without_lock(lock, [&protocol] { return protocol.get_location(); });
            site_latitude_cached_ = info.latitude_degrees;
            site_longitude_cached_ = info.longitude_degrees;
            site_info_valid_ = true;
        } catch (...) {
            ALPACA_LOG_DEBUG("SynScan", "Handset site read failed at connect; a later site read retries it");
        }
        // Stamped after the `w` returns, so a failed read is retried kSiteInfoRetryDelay later, not by the seed
        // probe in this same connect, however long the read took to time out.
        last_site_info_attempt_ = clock_.now();
        lock.unlock();  // the poll slot starts without mutex_ held (AsyncOperation rule 10)

        // The first probe is the seed: it publishes a frame even when the handset answers the echo but not `L`/`e`
        // (the getters then say "not known yet"). Only that first probe may seed; every later one throws and feeds
        // the fault latch.
        seeding_ = true;
        const bool started = polled_.start();
        seeding_ = false;
        fail_if_link_released(protocol);  // a pull the seed or an earlier query swallowed
        if (!started) {
            throw AlpacaException("SynScan status poll did not start", AlpacaError::DriverException);
        }
        connected_ = true;
    }

    // Decision 0009 rule 2a: a pulled cable during a connect exchange whose failure the connect swallows released the
    // port. The Connect fails with NotConnected; set_connected()'s catch joins the poll and clears the session.
    static void fail_if_link_released(const SynScanProtocolWrapper& protocol) {
        if (!protocol.link_alive()) {
            throw AlpacaException("Failed to connect to SynScan mount: " + protocol.link_fault(),
                                  AlpacaError::NotConnected);
        }
    }

    // Everything the session owns, back to its idle value. Caller holds mutex_. connected_ and the poll's frame and
    // latch are the caller's (polled_.reset()).
    void clear_session_state_locked() {
        site_info_valid_ = false;
        timezone_offset_valid_ = false;
        tracking_mode_valid_ = false;
        target_ra_set_ = false;
        target_dec_set_ = false;
        parked_ = false;
        parking_ = false;
        at_home_ = false;
        clear_pulse_guiding_locked();
        set_slewing_locked(false);
        refining_claim_.reset();
        last_slew_error_.clear();
        slew_hold_.clear();
        position_override_until_ = util::TaskClock::clock::time_point::min();
        invalidate_readback_tick_locked();
        guide_position_valid_ = false;
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
        ra_stop_unconfirmed_ = false;
        utc_offset_.clear();  // decision 0014: cleared on disconnect as well as connect
        client_disagreement_warned_ = false;
        last_site_info_attempt_ = util::TaskClock::clock::time_point::min();
        park_poll_failures_ = 0;
    }

    // The disconnect body, also the first half of a relink: the poll and the bodies are already joined. State is
    // cleared under mutex_ before the close (decision 0016 point 4), and the close runs with no driver mutex: the
    // wrapper's lock makes it wait for an exchange in flight, which set_connected()'s wrapper interrupt has already
    // ended (#2070).
    void teardown_session() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connected_ = false;
            clear_session_state_locked();
            polled_.reset();
        }
        {
            std::lock_guard<std::mutex> fwlock(firmware_mutex_);
            firmware_cache_.clear();
        }
        SynScanProtocolWrapper::instance().disconnect();
    }

    // closing_ tells the poll and the hunt to stop at their next exchange boundary. Set under hold_mutex_ so a probe
    // between its predicate check and its wait cannot miss it.
    void set_closing(bool closing) {
        {
            std::lock_guard<std::mutex> hold_lock(hold_mutex_);
            closing_ = closing;
        }
        hold_cv_.notify_all();
    }

    // Raises closing_ for the teardown paths of set_connected() and clears it on every exit, the early returns of the
    // pending-disconnect gates included. On a relink end() clears it before the new connect's first probe.
    class ClosingScope {
    public:
        explicit ClosingScope(SynScanTelescopeDriver& driver) : driver_(driver) {}
        ~ClosingScope() { end(); }
        ClosingScope(const ClosingScope&) = delete;
        ClosingScope& operator=(const ClosingScope&) = delete;
        void begin(bool interrupt_wire) {
            driver_.set_closing(true);
            active_ = true;
            if (interrupt_wire) {
                SynScanProtocolWrapper::instance().set_interrupt(true);
                interrupting_ = true;
            }
            driver_.run_closing_hook_for_testing();
        }
        void end() {
            if (active_) {
                driver_.set_closing(false);
                active_ = false;
            }
            if (interrupting_) {
                SynScanProtocolWrapper::instance().set_interrupt(false);
                interrupting_ = false;
            }
        }

    private:
        SynScanTelescopeDriver& driver_;
        bool active_ = false;
        bool interrupting_ = false;
    };

    void check_connected() const {
        if (!connected_ || link_lost()) {
            throw AlpacaException("Not connected to SynScan mount", AlpacaError::NotConnected);
        }
    }

    // For a getter with no connection check: a lost link is NotConnected (decision 0009 rule 3), ahead of the fault
    // text that outlives the loss (rule 5).
    void throw_if_link_lost_or_faulted() const {
        if (link_lost()) {
            throw AlpacaException("Not connected to SynScan mount", AlpacaError::NotConnected);
        }
        throw_if_link_faulted();
    }

    // The command gate's state checks in SynScan's words (#2258): util/telescope_command_gate.h decides the order,
    // these answer each step with the driver's own texts and reads. `lock` is the held mutex_ a slew's
    // Tracking check releases for its handset read; the other families never call that check.
    struct GateChecks {
        const SynScanTelescopeDriver& driver;
        const char* who = nullptr;
        std::unique_lock<std::mutex>* lock = nullptr;

        void check_connected() const { driver.check_connected(); }
        void check_not_parked(bool /*stop*/) const { driver.check_not_parked_locked(who); }
        void check_no_fault() const { driver.throw_if_link_faulted(); }
        // open-astro#775: ITelescopeV4 raises InvalidOperation when a slew is requested with Tracking false. The
        // handset is asked, with mutex_ released.
        void check_tracking() const {
            assert(lock != nullptr && "a slew's GateChecks carries its held mutex_");
            driver.refresh_tracking_mode_locked(*lock);
            if (!driver.get_tracking_locked(*lock)) {
                throw AlpacaException(std::string(who) + " requires Tracking to be true",
                                      AlpacaError::InvalidOperation);
            }
        }
    };

    // Decision 0009: lost when the poll latched it (a fault past the staleness bound, or the probe's lose_link()), or
    // at once on positive evidence: the wrapper saw the serial port gone and released it. Lock-free.
    bool link_lost() const { return polled_.link_lost() || !SynScanProtocolWrapper::instance().link_alive(); }

    // The connection check for a member that then sends with mutex_ released.
    void require_connected() const {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
    }

    // ── Background bodies (async slew / park, pulse-guide stop) ──
    // Each runs on a util::AsyncOperation slot (decision 0006): never
    // detached, cancelled and joined in the destructor and on disconnect so a
    // wakeup can never touch a destroyed or disconnected driver.

    // Cancel and join every motion slot. Must be called WITHOUT mutex_ held (the bodies take mutex_). Only the
    // lifecycle paths call it (disconnect, relink, the destructor): a motion initiator claims the slew slot instead
    // (claim_slew_slot_locked()) and never waits for the body it replaces (decision 0006, "Claim").
    // Cancels and joins the motion slots only. The status poll is not one of them: it runs for the life of the
    // connection, and a GOTO or Park that joined it would leave every read frozen at the slew-end frame, the
    // link-fault latch and the site retry stopped, and the landing hunt waiting for a tick that never comes. Only the
    // lifecycle paths (disconnect, relink, the destructor) stop it, through polled_.stop_and_join().
    void cancel_async_tasks() {
        slew_.cancel_all_and_join();
        join_pulse_bodies();
    }

    // The pulse stop bodies take mutex_ (after an RA stop, to restore tracking, and on failure), so AbortSlew and the
    // lifecycle paths end them by cancelling and joining with no driver mutex held (decision 0006, "Pulse and
    // rate-check slots"); MoveAxis(axis, 0) joins its own axis's. A replacement pulse, a moving MoveAxis, a slew and a
    // Park supersede instead (supersede_pulse_locked(), supersede_pulses_for_slew_locked()).
    void join_pulse_bodies() {
        pulse_ops_[0].cancel_all_and_join();
        pulse_ops_[1].cancel_all_and_join();
    }

    // Decision 0006 rule 3 (#2263): a replacement pulse or a moving MoveAxis supersedes the pulse on `axis` without a
    // join. The bump makes the body Superseded and the cancel() wakes it from its wait; it then sends the axis
    // nothing and touches no state (run_pulse_stop_body()). Called under mutex_ once the newer command's rate has
    // landed, before anything lets the old body run again, so a failed command leaves the old pulse to end itself.
    // A slew or Park, which goes on whatever its stop does, calls it before that stop
    // (supersede_pulses_for_slew_locked()).
    void supersede_pulse_locked(int axis) {
        // The bump comes before the cancel: a body that the cancel wakes must already read Superseded. Read as
        // Cancelled, it would send its blind stop with no turn, which only a canceller that joins it may allow.
        pulse_generation_[axis].bump();
        pulse_ops_[axis].cancel();
    }

    // Decision 0006 rule 3 (#2416): a slew or Park supersedes a running pulse and never joins it. The superseded
    // body sends nothing (run_pulse_stop_body()), so the slew owns the stop that pulse still owed and sends it here,
    // before its own GOTO. A body parked in its wait returns at once; one queued for its end turn finds itself
    // superseded at that turn and sends nothing. The stop takes no turn, as the cancelled body's blind stop did:
    // waiting for a turn the old body may hold or be queued for would be waiting for the body. initiator_mutex_
    // keeps every newer motion command behind this one, so the stop still lands before any of them. A body that sent
    // its end just before gets a second stop, which the GOTO follows, as with the cancel. A failed stop is logged; the
    // slew goes on and owns both axes either way. Returns whether a stop went out. Caller holds mutex_ (released for
    // the exchange) and initiator_mutex_, after every check that can refuse the command.
    bool supersede_pulses_for_slew_locked(std::unique_lock<std::mutex>& lock) {
        std::array<bool, 2> owed{};
        for (std::size_t axis = 0; axis < owed.size(); ++axis) {
            owed[axis] = pulse_ops_[axis].running();
            if (owed[axis]) {
                supersede_pulse_locked(static_cast<int>(axis));
            }
        }
        if ((!owed[0] && !owed[1]) || !connected_) {
            return false;
        }
        std::array<std::string, 2> errors;
        without_lock(lock, [&owed, &errors] {
            for (std::size_t axis = 0; axis < owed.size(); ++axis) {
                if (!owed[axis]) {
                    continue;
                }
                try {
                    SynScanProtocolWrapper::instance().move_axis_variable_rate(static_cast<int>(axis), 0.0);
                } catch (const std::exception& e) {
                    errors[axis] = e.what();
                } catch (...) {
                    errors[axis] = "unknown error";
                }
            }
        });
        for (std::size_t axis = 0; axis < errors.size(); ++axis) {
            if (!errors[axis].empty()) {
                ALPACA_LOG_ERROR("SynScan", "stop of a pulse superseded by a slew failed on axis " +
                                                std::to_string(axis) +
                                                "; the axis may still be moving: " + errors[axis]);
            }
        }
        return true;
    }

    // Caller holds mutex_, after every check that can refuse the initiator and before it writes its own flags. The
    // body in flight becomes Superseded: it re-checks that under mutex_ after each wire exchange and then touches
    // neither a flag nor the axes. A bump alone wakes no waiter, so the cancel() wakes a body parked in its slot
    // wait; Superseded still wins over Cancelled. The body is not joined (it may be inside a serial exchange).
    // Returns whether a body was in flight: only then is there a motion nobody will stop if the start is refused.
    bool claim_slew_slot_locked() {
        const bool displaced = slew_.running();
        slew_generation_.bump();
        slew_.cancel();
        return displaced;
    }

    // True once the body behind `ctx` was cancelled or replaced; false for a synchronous caller (null).
    static bool slot_stopped(const util::OperationContext* ctx) {
        return ctx != nullptr && ctx->stop_reason() != util::StopReason::None;
    }

    // Runs `fn`, a blocking call into the protocol wrapper, with mutex_ released and takes mutex_ again before it
    // returns or throws. The wrapper serialises its transactions itself, so a body that held mutex_ across the
    // exchange would make every getter and every newer initiator wait for the old body (decision 0006, "Claim").
    // A motion command goes through send_in_turn(), which orders it behind earlier senders and checks ownership at
    // its turn; a read may call this directly. After it returns the caller checks it still owns the slew state
    // before it touches any flag.
    template <typename Fn>
    static auto without_lock(std::unique_lock<std::mutex>& lock, Fn&& fn) -> decltype(fn()) {
        struct Relock {
            std::unique_lock<std::mutex>& lock;
            ~Relock() { lock.lock(); }
        } relock{lock};
        lock.unlock();
        return fn();
    }

    // The ordered wire turn (decision 0006, "Claim"). Every motion command, from a body or an initiator, takes a
    // ticket, waits for its turn, checks it still owns its operation and only then sends; its commands go out in
    // ticket order. A newer owner claims the slot before it takes its ticket, so its commands always follow
    // anything an older owner is committed to send, and an older owner that finds the claim at its turn sends
    // nothing. A check made before the wait and a send made after it would leave a gap in which std::mutex, which is
    // not FIFO, can put the old command behind the new owner's. The Tracking write takes a ticket too, without
    // claiming (set_tracking()): its `T` must not land between a stop and its tracking restore. Getters and the status
    // poll take no ticket.
    // The constructor runs with `lock` (mutex_) held: it takes the ticket, releases `lock` to wait (nobody waits for
    // a turn while holding mutex_, and the holder of a turn may need mutex_), and returns with the turn and `lock`
    // both held. The destructor passes the turn on, on every path. Lock order: transition_mutex_, initiator_mutex_,
    // config_write_mutex_, turn, mutex_ (a leaf, never held while a protocol call runs), the wrapper mutex.
    class WireTurn {
    public:
        WireTurn(SynScanTelescopeDriver& driver, std::unique_lock<std::mutex>& lock) : driver_(driver) {
            std::unique_lock<std::mutex> turn_lock(driver_.turn_mutex_);
            const uint64_t ticket = driver_.turn_next_++;
            lock.unlock();
            driver_.notify_wire_turn_wait_for_testing(turn_lock);
            driver_.turn_cv_.wait(turn_lock, [&] { return driver_.turn_serving_ == ticket; });
            turn_lock.unlock();
            lock.lock();
        }
        ~WireTurn() {
            {
                std::lock_guard<std::mutex> turn_lock(driver_.turn_mutex_);
                ++driver_.turn_serving_;
            }
            driver_.turn_cv_.notify_all();
        }
        WireTurn(const WireTurn&) = delete;
        WireTurn& operator=(const WireTurn&) = delete;

    private:
        SynScanTelescopeDriver& driver_;
    };

    void notify_wire_turn_wait_for_testing(std::unique_lock<std::mutex>& turn_lock) {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(turn_hook_mutex_);
            hook = turn_wait_hook_;
        }
        if (hook) {
            turn_lock.unlock();
            hook();
            turn_lock.lock();
        }
    }

    // One command in its turn: takes the ticket, waits, and sends `fn` with mutex_ released only while `owns()`
    // (evaluated at the turn, under mutex_) still holds. Returns whether it sent. `lock` is held on entry and on
    // return or throw.
    template <typename Owns, typename Fn>
    bool send_in_turn(std::unique_lock<std::mutex>& lock, Owns&& owns, Fn&& fn) {
        WireTurn turn(*this, lock);
        if (!owns()) {
            return false;
        }
        without_lock(lock, std::forward<Fn>(fn));
        return true;
    }

    // Call without mutex_ held. Starts `body` on the slew slot. The initiator
    // claimed the slot (claim_slew_slot_locked()) and published its flags
    // before this; when the slot refuses (no thread), `rollback_locked` undoes
    // the flags and the exception goes to the caller. `displaced` is the
    // claim's result: a body was in flight and is superseded, so it will not
    // stop its motion and nobody is left to stop it; the mount is stopped and
    // the AbortSlew state published (decision 0006, "Thread refusal"). A
    // refusal that replaced nothing cancels nothing.
    template <typename Rollback>
    void start_slew_body(std::function<void(util::OperationContext&)> body, bool displaced,
                         Rollback&& rollback_locked) {
        try {
            slew_.start(std::move(body));
        } catch (...) {
            std::unique_lock<std::mutex> lock(mutex_);
            rollback_locked();
            if (displaced && connected_) {
                stop_after_refused_start_locked(lock);
            }
            throw;
        }
    }

    // The stops of stop_park_slew_wire(), then the tracking restore when every stop was answered and `restore_mode`
    // is a tracking mode: the RA stop ended sidereal tracking. Returns the first stop failure (empty when every stop
    // was answered); a failed restore goes to `restore_error` and never hides the stop. Runs with mutex_ released.
    static std::string stop_and_restore_tracking_wire(int restore_mode, std::string& restore_error) {
        std::string stop_error = stop_park_slew_wire();
        if (stop_error.empty() && restore_mode > 0) {
            try {
                SynScanProtocolWrapper::instance().set_tracking_mode(restore_mode);
            } catch (const std::exception& e) {
                restore_error = e.what();
            } catch (...) {
                restore_error = "unknown exception";
            }
        }
        return stop_error;
    }

    // Publishes the outcome of stop_and_restore_tracking_wire(), as AbortSlew's restore: a restore that did not land
    // leaves Tracking false until one does; one that landed is written through, so a poll sampled during the RA stop
    // cannot publish over it. `what` names the stop in the log. mutex_ must be held.
    void note_tracking_restore_locked(const std::string& stop_error, const std::string& restore_error, int restore_mode,
                                      const std::string& what) {
        if (!restore_error.empty()) {
            ra_stop_unconfirmed_ = true;
            ALPACA_LOG_ERROR("SynScan", "tracking restore after " + what + " failed: " + restore_error);
        } else if (stop_error.empty() && restore_mode > 0) {
            set_tracking_mode_cached_locked(restore_mode);
            ra_stop_unconfirmed_ = false;
            invalidate_readback_top_locked();
        }
    }

    // A slew failure whose stop and kept error are already published (fail_slew_timeout_locked()): the async body
    // passes it on without overwriting the error or Slewing.
    class SlewFailureKept : public AlpacaException {
    public:
        using AlpacaException::AlpacaException;
    };

    // A GOTO whose wait timed out (#2412): the handset still reports it in progress, so the mount is stopped in its
    // wire turn and its tracking restored, as AbortSlew does, and the error is kept for Slewing until the next motion
    // command or a reconnect clears it. An answered stop clears Slewing; a failed one leaves Slewing as it was and
    // the error says the mount may still be moving. Throws SlewFailureKept with the kept text. A newer operation that
    // took the mount (or a disconnect) owns the state: nothing is recorded, and this returns for the wait to report
    // the supersession (a disconnect throws NotConnected). `lock` (mutex_) is held on entry and on return or throw.
    void fail_slew_timeout_locked(std::unique_lock<std::mutex>& lock, const util::MotionClaim& claim,
                                  const util::OperationContext* ctx) {
        const std::string message = "Slew timed out after " + std::to_string(kSlewTimeout.count()) + " s";
        const auto owns = [&] { return connected_ && !lost_ownership_locked(claim, ctx); };
        std::string stop_error;
        std::string restore_error;
        int restore_mode = 0;
        if (!send_in_turn(
                lock,
                [&] {
                    restore_mode = tracking_mode_cached_;  // read at the turn, as AbortSlew's restore
                    return owns();
                },
                [&] { stop_error = stop_and_restore_tracking_wire(restore_mode, restore_error); })) {
            check_connected();
            ALPACA_LOG_WARN("SynScan", "slew timeout dropped, the slew was taken over: " + message);
            return;
        }
        if (!owns()) {
            check_connected();
            return;
        }
        note_tracking_restore_locked(stop_error, restore_error, restore_mode, "slew timeout");
        last_slew_error_ = message;
        if (stop_error.empty()) {
            set_slewing_locked(false);
            last_slew_error_ += "; the mount was stopped";
        } else {
            last_slew_error_ += "; stopping after failure failed: " + stop_error + "; the mount may still be moving";
            ALPACA_LOG_ERROR("SynScan",
                             "stop after slew timeout failed: " + stop_error + "; the mount may still be moving");
        }
        slew_hold_.clear();
        position_override_until_ = util::TaskClock::clock::time_point::min();
        guide_position_valid_ = false;
        drop_poll_in_flight_locked();
        ALPACA_LOG_WARN("SynScan", last_slew_error_);
        throw SlewFailureKept(last_slew_error_);
    }

    // AbortSlew's stop and published state, for a refused start that cancelled
    // a body in flight. A failed stop leaves Slewing as it was: the mount may
    // still be moving.
    void stop_after_refused_start_locked(std::unique_lock<std::mutex>& lock) {
        std::string stop_error;
        std::string restore_error;
        int restore_mode = 0;
        if (!send_in_turn(
                lock,
                [this, &restore_mode] {
                    restore_mode = tracking_mode_cached_;  // read at the turn, as AbortSlew's restore
                    return static_cast<bool>(connected_);
                },
                [&stop_error, &restore_error, &restore_mode] {
                    stop_error = stop_and_restore_tracking_wire(restore_mode, restore_error);
                })) {
            return;
        }
        note_tracking_restore_locked(stop_error, restore_error, restore_mode, "refused slew start");
        if (stop_error.empty()) {
            set_slewing_locked(false);
        } else {
            ALPACA_LOG_ERROR("SynScan",
                             "stop after refused slew start failed: " + stop_error + "; the mount may still be moving");
        }
        parking_ = false;
        cancel_motion_locked(util::MotionOwner::kBothAxes);
        slew_hold_.clear();
        position_override_until_ = util::TaskClock::clock::time_point::min();
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
    }

public:
    void set_slew_spawn_for_testing(std::function<std::thread(std::function<void()>)> spawn) {
        slew_.set_spawn_for_testing(std::move(spawn));
    }

    // Reaches the pulse slots only: the slew seam above cannot refuse a pulse start.
    void set_pulse_spawn_for_testing(std::function<std::thread(std::function<void()>)> spawn) {
        pulse_ops_[0].set_spawn_for_testing(spawn);
        pulse_ops_[1].set_spawn_for_testing(std::move(spawn));
    }

    // Forward the module's seams (util::PolledState::pause_for_testing / poll_now_for_testing). A paused poll waits its
    // ticks on the task clock and probes nothing: a case that scripts the handset's replies or advances the clock
    // while it counts them pauses it so the poll does not consume them.
    void set_status_poll_paused_for_testing(bool paused) { polled_.pause_for_testing(paused); }

    // One full tick on the calling thread, paused or not: a case that fixes the handset's state and the clock reads
    // exactly one frame (alt/az, pier side and tracking mode included).
    void poll_status_now_for_testing() {
        probe_tick_ = 0;
        polled_.poll_now_for_testing();
    }

    // Called between a probe's last exchange and its decode, with no driver lock held and the probe still marked as
    // sampling: a reply gate on the handset cannot stand in for it, because a gated exchange holds the wrapper's lock
    // and every other command queues behind it.
    void set_status_poll_after_sample_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> lock(probe_hook_mutex_);
        probe_after_sample_hook_ = std::move(hook);
    }

    // Called by each sender right after it takes its wire-turn ticket and before it waits for its turn, with no
    // driver lock held (turn_mutex_ is released around the call): a case that holds the wire in one exchange learns
    // that a second sender is queued behind it without sleeping. An empty function (the default) does nothing.
    void set_wire_turn_wait_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> lock(turn_hook_mutex_);
        turn_wait_hook_ = std::move(hook);
    }

    // Called by a Disconnect and the destructor right after they decide whether to raise the wrapper interrupt, with no
    // driver lock held: a case can hold the teardown there to check whose exchanges it cuts. An empty function (the
    // default) does nothing.
    void set_closing_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> lock(turn_hook_mutex_);
        closing_hook_ = std::move(hook);
    }
    void run_closing_hook_for_testing() {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(turn_hook_mutex_);
            hook = closing_hook_;
        }
        if (hook) hook();
    }

    // Replaces the wall time every local-sidereal-time read uses; an empty function restores system_clock.
    void set_sidereal_wall_clock_for_testing(std::function<std::chrono::system_clock::time_point()> now) {
        std::lock_guard<std::mutex> lock(sidereal_wall_clock_mutex_);
        sidereal_wall_clock_ = std::move(now);
    }

private:
    void clear_pulse_guiding_locked() {
        for (auto& window : pulse_hold_) {
            window.clear();
        }
        pulse_cut_short_ = {};
    }

    // Decision 0022 rule 2 (a cancel clears the window): a pulse cut short by MoveAxis(axis, 0) or AbortSlew leaves
    // its window armed while that canceller runs, because on RA the window is also what keeps
    // refresh_tracking_mode_locked() from reading the `t` 0 the blind stop left. A canceller that goes on clears the
    // window itself (a new arm, its success clear); one that fails or is refused clears it here, on its way out, so
    // IsPulseGuiding does not stay true over a stopped axis. On RA the owed restore moves to ra_stop_unconfirmed_: the
    // stop turned handset tracking off, so Tracking reads false and the next RA stop, AbortSlew or Tracking write
    // restores the cached mode instead of reading the handset's 0. Caller holds mutex_.
    void release_cut_short_pulse_locked(std::size_t axis) {
        if (!pulse_cut_short_[axis]) {
            return;
        }
        pulse_cut_short_[axis] = false;
        pulse_hold_[axis].clear();
        if (axis == 0 && connected_ && tracking_mode_cached_ > 0) {
            ra_stop_unconfirmed_ = true;
        }
    }

    // Runs release_cut_short_pulse_locked() when a canceller's locked block ends, by a throw or a return. Declared
    // after the block's lock, so it runs with mutex_ held. `axis` -1: both axes (join_pulse_bodies()).
    class CutShortPulseRelease {
    public:
        CutShortPulseRelease(SynScanTelescopeDriver& driver, int axis) : driver_(driver), axis_(axis) {}
        ~CutShortPulseRelease() {
            for (std::size_t a = 0; a < 2; ++a) {
                if (axis_ < 0 || static_cast<std::size_t>(axis_) == a) {
                    driver_.release_cut_short_pulse_locked(a);
                }
            }
        }
        CutShortPulseRelease(const CutShortPulseRelease&) = delete;
        CutShortPulseRelease& operator=(const CutShortPulseRelease&) = delete;

    private:
        SynScanTelescopeDriver& driver_;
        int axis_;
    };

    // A park in flight (parking_) gates the same members as a completed park:
    // otherwise a slew/MoveAxis/sync issued right after the async Park would
    // silently clobber it (and MoveAxis would jog an axis mid-GOTO). Only
    // AbortSlew and Unpark are allowed through, and both cancel the park.
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

    void invalidate_readback_tick_locked() const {
        readback_tick_valid_until_ = util::TaskClock::clock::time_point::min();
        readback_top_valid_ = false;
        readback_tick_absent_ = false;  // the handset's regime may have changed: hunt again (open-astro#1107)
    }

    void invalidate_readback_top_locked() const { readback_top_valid_ = false; }

    bool readback_tick_valid_at(util::TaskClock::clock::time_point at) const {
        return at < readback_tick_valid_until_ && at >= readback_tick_;
    }

    // The precise RA read `raw_hours` taken at `at`, corrected for the handset's once-a-second readback
    // (open-astro#1030): a read is low by the fraction of the handset's second since its tick. A read inside the
    // tick's uncertainty can land on either side of it, one second of RA apart; the last known RA picks the side.
    double corrected_ra_hours_locked(double raw_hours, util::TaskClock::clock::time_point at) const {
        if (!readback_tick_valid_at(at)) {
            return raw_hours;
        }
        const double since = std::chrono::duration<double>(at - readback_tick_).count();
        double corrected = std::fmod(raw_hours + (since - std::floor(since)) / 3600.0 + 24.0, 24.0);
        if (readback_top_valid_ &&
            std::abs(util::shortest_ra_delta_hours(corrected, readback_top_hours_)) > kReadbackDisambiguationHours) {
            // An anchor a little late puts a post-tick read one second high; a little early, a pre-tick read one
            // second low. Whichever neighbour sits by the last known RA is the read.
            for (const double side : {-1.0, 1.0}) {
                const double alternative = std::fmod(corrected + side / 3600.0 + 24.0, 24.0);
                if (std::abs(util::shortest_ra_delta_hours(alternative, readback_top_hours_)) <=
                    kReadbackDisambiguationHours) {
                    return alternative;
                }
            }
        }
        return corrected;
    }

    // The hunt's hold on the line (decision 0010, open-astro#1247): while it lives no probe starts sampling. Taken
    // under mutex_ (hold_mutex_ is a leaf below it); released on every exit, which wakes a probe waiting on it.
    class HuntHold {
    public:
        explicit HuntHold(const SynScanTelescopeDriver& driver) : driver_(driver) {
            std::lock_guard<std::mutex> hold_lock(driver_.hold_mutex_);
            ++driver_.hunt_holds_;
        }
        ~HuntHold() {
            {
                std::lock_guard<std::mutex> hold_lock(driver_.hold_mutex_);
                --driver_.hunt_holds_;
            }
            driver_.hold_cv_.notify_all();
        }
        HuntHold(const HuntHold&) = delete;
        HuntHold& operator=(const HuntHold&) = delete;

    private:
        const SynScanTelescopeDriver& driver_;
    };

    // Set while a probe is inside its exchanges; cleared on every exit, a throw included.
    class ProbeSampling {
    public:
        explicit ProbeSampling(SynScanTelescopeDriver& driver) : driver_(driver) {}
        ~ProbeSampling() {
            std::lock_guard<std::mutex> hold_lock(driver_.hold_mutex_);
            driver_.probe_sampling_ = false;
        }
        ProbeSampling(const ProbeSampling&) = delete;
        ProbeSampling& operator=(const ProbeSampling&) = delete;

    private:
        SynScanTelescopeDriver& driver_;
    };

    bool probe_sampling() const {
        std::lock_guard<std::mutex> hold_lock(hold_mutex_);
        return probe_sampling_;
    }

    struct TickHunt {
        bool found = false;
        util::TaskClock::clock::time_point tick;
        util::TaskClock::clock::time_point read_at;
        std::pair<uint32_t, uint32_t> raw{};
        double top_hours = 0.0;  // the true RA when found: the post-tick read plus the fraction since the tick
    };

    // One precise RA/Dec read with mutex_ released, and the task-clock time it was taken at, stamped before mutex_ is
    // taken again so a contended relock cannot widen the hunt's brackets.
    std::pair<std::pair<uint32_t, uint32_t>, util::TaskClock::clock::time_point> read_equatorial_released(
        std::unique_lock<std::mutex>& lock) const {
        return without_lock(lock, [this] {
            const auto raw = SynScanProtocolWrapper::instance().get_ra_dec_raw(true);
            return std::make_pair(raw, clock_.now());
        });
    }

    // Reads the position until the RA readback has risen by at least half a second of RA (the handset's tick)
    // kReadbackTickHuntJumps times, or the budget runs out. Each jump brackets the tick between two reads; the
    // brackets, shifted by whole seconds, are intersected. Without any jump the last read comes back uncorrected.
    TickHunt hunt_readback_tick_locked(std::unique_lock<std::mutex>& lock, const util::MotionClaim& claim,
                                       util::OperationContext* ctx) const {
        using clock = util::TaskClock::clock;
        // The hunt reads at a 50 ms cadence tuned on the rig; no probe starts sampling while it runs (a probe waits on
        // the hold, bounded). The hold is taken here, under mutex_, so a probe either is sampling already (waited out
        // below) or sees the hold and waits.
        const HuntHold hold(*this);
        // A probe already inside its exchanges queues the hunt's reads behind it on the wrapper mutex and widens the
        // bracket around the tick: let it finish. mutex_ is released for every slice of the wait, so a getter and the
        // probe's own decode take it freely. The wait is bounded: a probe on a silent link can take several response
        // timeouts, and past the bound the hunt reads anyway, queued behind it. A superseding command, a stop or a
        // disconnect ends the wait at once.
        const auto wait_until = clock_.now() + kPollSampleWaitBound;
        while (probe_sampling() && clock_.now() < wait_until) {
            lock.unlock();
            if (ctx != nullptr) {
                (void)ctx->wait_for(std::chrono::milliseconds(2));  // a stop ends it at once; the check below reads it
            } else {
                clock_.sleep_for(std::chrono::milliseconds(2));
            }
            lock.lock();
            if (claim_lost_locked(claim) || closing_.load() ||
                (ctx != nullptr && ctx->stop_reason() != util::StopReason::None)) {
                return TickHunt{};
            }
        }
        const auto start = clock_.now();
        TickHunt hunt;
        if (readback_tick_absent_) {
            // A hunt on this connection already spent its first-jump budget with no jump: one plain read.
            check_connected();
            const auto sample = read_equatorial_released(lock);
            hunt.raw = sample.first;
            hunt.read_at = sample.second;
            return hunt;
        }
        bool have_previous = false;
        double previous_hours = 0.0;
        auto previous_at = start;
        int jumps = 0;
        clock::time_point low;
        clock::time_point high;
        while (true) {
            if (claim_lost_locked(claim) || closing_.load() ||
                (ctx != nullptr && ctx->stop_reason() != util::StopReason::None)) {
                return hunt;
            }
            check_connected();
            const auto sample = read_equatorial_released(lock);
            if (claim_lost_locked(claim) || closing_.load() ||
                (ctx != nullptr && ctx->stop_reason() != util::StopReason::None)) {
                return hunt;  // overtaken during the read: the caller sees that and drops the result
            }
            const auto raw = sample.first;
            const auto read_at = sample.second;
            const double hours = decode_ra_hours(raw.first, 24);
            if (have_previous && util::shortest_ra_delta_hours(hours, previous_hours) >= kReadbackTickJumpHours) {
                // Bracket (previous_at, read_at], expressed in the first jump's second.
                const auto shift = std::chrono::duration_cast<clock::duration>(std::chrono::seconds(jumps));
                const auto bracket_low = previous_at - shift;
                const auto bracket_high = read_at - shift;
                if (jumps == 0) {
                    low = bracket_low;
                    high = bracket_high;
                } else {
                    low = std::max(low, bracket_low);
                    high = std::min(high, bracket_high);
                    if (high < low) {  // the period drifted off one second; keep the newest bracket alone
                        low = bracket_low;
                        high = bracket_high;
                    }
                }
                ++jumps;
                hunt.found = true;
                hunt.raw = raw;
                hunt.read_at = read_at;
                hunt.tick = low + (high - low) / 2;
                if (jumps >= kReadbackTickHuntJumps) {
                    break;
                }
            }
            if (!hunt.found) {
                hunt.raw = raw;
                hunt.read_at = read_at;
                if (read_at - start >= kReadbackTickFirstJumpBudget) {
                    readback_tick_absent_ = true;
                    return hunt;
                }
            } else if (read_at - start >= kReadbackTickHuntBudget) {
                break;
            }
            have_previous = true;
            previous_hours = hours;
            previous_at = read_at;
            lock.unlock();
            if (ctx != nullptr) {
                ctx->wait_for(kReadbackTickHuntInterval);
            } else {
                clock_.sleep_for(kReadbackTickHuntInterval);
            }
            lock.lock();
        }
        // The last jump's tick, in its own second, is the anchor; the top is that read plus the fraction since.
        const auto shift = std::chrono::duration_cast<clock::duration>(std::chrono::seconds(jumps - 1));
        hunt.tick += shift;
        const double since = std::chrono::duration<double>(hunt.read_at - hunt.tick).count();
        hunt.top_hours = std::fmod(decode_ra_hours(hunt.raw.first, 24) + since / 3600.0 + 24.0, 24.0);
        return hunt;
    }

    // The signed variable rate (deg/s, positive = code 6) that moves RA east at `east_arcsec_per_s`. On the handset
    // a variable-rate P slew is the RA motor's own speed and replaces tracking (EQM-35 Pro on HC 06.03.00,
    // 2026-10-13: a 2 s "west" pulse sent as code 6 at 7.5"/s moved RA 50.6" east, the "east" one as code 7 19.3"
    // east). So the motor runs westward at the base rate (sidereal while tracking, else 0) less the eastward rate,
    // and the tracking command alone ends the move: sent mid-slew it resumes sidereal at once, with no stop
    // between (measured: a 15" move landed within the readback's resolution). The westward code is 7 at that
    // southern site; INDI's SynScan driver sends 6 for west at sidereal + guide rate, so 6 is taken as westward
    // north of the equator (not measured on a northern mount).
    double ra_motor_rate_locked(double east_arcsec_per_s) const {
        if (!equatorial_geometry_locked()) {
            // Axis 0 is azimuth on an Alt-Az mount: no sidereal base, and the move is the guide rate as before.
            return -east_arcsec_per_s / 3600.0;
        }
        const double base_arcsec_per_s = tracking_mode_cached_ > 0 ? kSiderealDegPerSec * 3600.0 : 0.0;
        const double westward_arcsec_per_s = base_arcsec_per_s - east_arcsec_per_s;
        const double westward_sign = site_latitude_cached_ < 0.0 ? -1.0 : 1.0;
        return westward_sign * westward_arcsec_per_s / 3600.0;
    }

    // True only when the mount is known to be equatorial. The absolute RA motor model and the timed landing moves
    // assume the motor axes are RA and Dec; an Alt-Az mount, or an AZ-EQ mount whose geometry is not configured,
    // keeps the relative pulse rate and the residual-aimed re-GOTO.
    bool equatorial_geometry_locked() const {
        try {
            return alignment_mode_locked() != AlignmentMode::AltAz;
        } catch (const AlpacaException&) {
            return false;
        }
    }

    // #2263: the motion owner (util::MotionOwner) decides cancelled versus superseded per axis. A slew or a Park
    // claims both axes, a moving MoveAxis its own; a stop (AbortSlew, MoveAxis(axis, 0), Unpark during a Park, a
    // disconnect, the refused-start stop) claims nothing and cancels its axes. Both wake a synchronous landing nudge
    // waiting on `motion_cv_`. Caller holds mutex_.
    util::MotionClaim claim_motion_locked(util::MotionCommand command, int axis = 0) {
        const util::MotionClaim claim = motion_owner_.claim(command, axis);
        ++motion_epoch_;
        motion_cv_.notify_all();
        return claim;
    }
    void cancel_motion_locked(std::uint8_t axes) {
        motion_owner_.cancel(axes);
        ++motion_epoch_;
        motion_cv_.notify_all();
    }
    // True once a newer command claimed one of the axes of `claim` or a stop cancelled one. Caller holds mutex_.
    bool claim_lost_locked(const util::MotionClaim& claim) const {
        return motion_owner_.stop_reason(claim) != util::StopReason::None;
    }

    // Moves one axis by `delta_arcsec` (RA: east positive; Dec: north positive) at the guide rate and stops,
    // restoring tracking after an RA move as a pulse does. Returns false when superseded or cancelled.
    bool nudge_axis_locked(std::unique_lock<std::mutex>& lock, int axis, double delta_arcsec,
                           const util::MotionClaim& claim, util::OperationContext* ctx) {
        auto& protocol = SynScanProtocolWrapper::instance();
        // Every exchange below runs with mutex_ released. The move start and its stop share one wire turn (below).
        const double rate_deg_per_sec = axis == 0 ? guide_rate_.ra : guide_rate_.dec;
        const double rate_arcsec_per_sec = rate_deg_per_sec * 3600.0;
        if (rate_arcsec_per_sec <= 0.0) {
            return true;
        }
        const double move_arcsec = delta_arcsec;
        // A residual just over the tolerance can call for a move shorter than the handset resolves; a 50 ms
        // minimum keeps it from being skipped round after round.
        const double seconds = std::clamp(std::abs(move_arcsec) / rate_arcsec_per_sec, 0.05, kNudgeMaxSeconds);
        // RA goes through the absolute motor rate (ra_motor_rate_locked); Dec north is positive unless the mount
        // is on the west side of the pier, where the Dec motor runs the other way.
        double signed_rate = axis == 0
                                 ? ra_motor_rate_locked(move_arcsec > 0.0 ? rate_arcsec_per_sec : -rate_arcsec_per_sec)
                                 : (move_arcsec > 0.0 ? rate_deg_per_sec : -rate_deg_per_sec);
        if (axis == 1) {
            const char pier = without_lock(lock, [&protocol] { return protocol.get_pointing_state(); });
            if (lost_ownership_locked(claim, ctx)) {
                return false;
            }
            if (pier == 'W') {
                signed_rate = -signed_rate;
            }
        }
        // The wire turn is kept from the start command to the stop: the move can then never end without its stop, and
        // a newer initiator's commands, queued behind it, follow the stop. A newer initiator claims before it takes
        // its ticket and so wakes the wait below at once (the slot's wait_for() or motion_cv_), which bounds the hold
        // to the wake-up plus the stop exchanges. The move is the one thing this body commands that it must undo
        // itself, so the stop is sent even when the body was overtaken meanwhile.
        WireTurn turn(*this, lock);
        if (lost_ownership_locked(claim, ctx)) {
            check_connected();
            return false;  // nothing was started: the newer operation commands the axes itself
        }
        without_lock(lock, [&] { protocol.move_axis_variable_rate(axis, signed_rate); });
        bool keep = true;
        const auto move_time = std::chrono::milliseconds(static_cast<int>(seconds * 1000.0));
        if (ctx != nullptr) {
            lock.unlock();
            keep = ctx->wait_for(move_time);
            lock.lock();
        } else {
            // The wait releases mutex_ while parked; every claim and every stop (claim_motion_locked(),
            // cancel_motion_locked()) signals motion_cv_, so a superseded synchronous nudge returns at once instead of
            // sleeping out move_time.
            clock_.wait_for(lock, motion_cv_, move_time, [&] { return claim_lost_locked(claim); });
        }
        std::string stop_error;
        bool stopped = false;
        for (int attempt = 0; attempt < 3 && !stopped; ++attempt) {
            const bool restore_tracking = axis == 0 && tracking_mode_cached_ > 0;
            const int mode = tracking_mode_cached_;
            try {
                without_lock(lock, [&] {
                    if (restore_tracking) {
                        protocol.set_tracking_mode(mode);  // ends the RA move, see ra_motor_rate_locked
                    } else {
                        protocol.move_axis_variable_rate(axis, 0.0);
                    }
                });
                stopped = true;
            } catch (const std::exception& e) {
                stop_error = e.what();
            }
        }
        if (!stopped) {
            throw AlpacaException("Landing move stop failed on axis " + std::to_string(axis) + ": " + stop_error);
        }
        check_connected();
        return keep && !lost_ownership_locked(claim, ctx);
    }

    // ── Snapshot-served state (decision 0010, SynScan adoption) ──────────────

    // One handset sample, taken without mutex_ (the protocol wrapper serialises each exchange). The slewing flag is
    // read BEFORE the position, so a frame that says "not slewing" carries a position measured after the stop. The
    // slow fields are optional: a handset that does not answer one keeps the last value.
    struct RawSample {
        bool slewing = false;
        std::pair<uint32_t, uint32_t> equatorial{};
        util::TaskClock::clock::time_point read_at{};
        std::optional<std::pair<uint32_t, uint32_t>> altaz;
        std::optional<char> pier;
        std::optional<int> tracking_mode;
    };

    // Throws what the protocol throws for the slewing flag or the equatorial pair.
    // closing_ is checked before every exchange, so a disconnect never joins more than the exchange in flight.
    RawSample sample_raw(bool precise, bool full) const {
        auto& protocol = SynScanProtocolWrapper::instance();
        const auto not_closing = [this] {
            if (closing_.load()) {
                throw AlpacaException("SynScan state sample abandoned: the link is closing", AlpacaError::NotConnected);
            }
        };
        RawSample raw;
        not_closing();
        raw.slewing = protocol.is_goto_in_progress();
        not_closing();
        raw.equatorial = protocol.get_ra_dec_raw(precise);
        raw.read_at = clock_.now();
        if (full) {
            not_closing();
            try {
                raw.altaz = protocol.get_alt_az_raw(precise);
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): keeps the last value
            }
            not_closing();
            try {
                raw.pier = protocol.get_pointing_state();
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): keeps the last value
            }
            not_closing();
            try {
                raw.tracking_mode = protocol.get_tracking_mode();
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): keeps the last value
            }
        }
        return raw;
    }

    // The frame a sample makes, over the previous one (a slow field the sample lacks keeps its value). Caller holds
    // mutex_: the RA correction reads the readback tick.
    PolledFrame decode_sample_locked(const RawSample& raw, bool precise, const PolledFrame& previous) const {
        PolledFrame frame = previous;
        const int bits = precise ? 24 : 16;
        frame.slewing = raw.slewing;
        double ra = decode_ra_hours(raw.equatorial.first, bits);
        if (precise) {
            ra = corrected_ra_hours_locked(ra, raw.read_at);
        }
        frame.ra_hours = ra;
        frame.dec_degrees = decode_angle(raw.equatorial.second, bits);
        frame.equatorial_known = true;
        if (raw.altaz) {
            frame.az_degrees = util::wrap_degrees(decode_angle(raw.altaz->first, bits));
            frame.alt_degrees = decode_angle(raw.altaz->second, bits);
            frame.altaz_known = true;
        }
        if (raw.pier) {
            frame.side_of_pier = map_pointing_state_to_side(*raw.pier);  // -1 (pierUnknown) is an answer too
            frame.pier_known = true;
        }
        if (raw.tracking_mode) {
            frame.tracking_mode = *raw.tracking_mode;
        }
        return frame;
    }

    // The frame a getter serves, or the refusal that applies: a lost link (NotConnected), a latched fault
    // (DriverException "communications compromised", Connected stays true, decision 0009 point 1). A frame past
    // kSnapshotMaxAge is served as it is: stale is not lost. The module does the work under its own leaf mutexes.
    util::PolledState<PolledFrame>::Reading served_frame_locked() const {
        check_connected();
        return polled_.served();
    }

    std::pair<double, double> served_equatorial_locked() const {
        const auto frame = served_frame_locked();
        if (!frame.value.equatorial_known) {
            throw AlpacaException("SynScan mount position is not available: the last read failed",
                                  AlpacaError::DriverException);
        }
        return {frame.value.ra_hours, frame.value.dec_degrees};
    }

    // {azimuth, altitude}
    std::pair<double, double> served_altaz_locked() const {
        const auto frame = served_frame_locked();
        if (!frame.value.altaz_known) {
            throw AlpacaException("SynScan mount alt/az is not available: the last read failed",
                                  AlpacaError::DriverException);
        }
        return {frame.value.az_degrees, frame.value.alt_degrees};
    }

    // The age of the state DeviceState serves: how long ago the poll measured it, on the task clock. Takes only the
    // module's own leaf mutex.
    std::optional<std::chrono::nanoseconds> device_state_age() const override { return polled_.age(); }

    // A write that changed what the handset's position means (a slew start, a landing round): drops the poll in
    // flight so its older sample cannot overwrite what the control path knows. The next poll publishes.
    void drop_poll_in_flight_locked() const {
        polled_.write([](PolledFrame&) {});
    }

    // The slewing flag the control path just decided (a slew started or ended, an abort), written through so the
    // next read sees it before the next poll runs.
    // The tracking mode the control path just commanded, written through as set_slewing_locked() is.
    void set_tracking_mode_cached_locked(int mode) const {
        tracking_mode_cached_ = mode;
        ++tracking_mode_generation_;
        polled_.write([mode](PolledFrame& frame) { frame.tracking_mode = mode; });
    }

    void set_slewing_locked(bool slewing) const {
        slewing_cached_ = slewing;
        polled_.write([slewing](PolledFrame& frame) { frame.slewing = slewing; });
    }

    // A re-read of the handset's slewing flag (the slew-complete and Park loops, every 250 ms). Writes only a change:
    // every write drops the poll tick in flight, so a same-value write on each re-read would drop nearly every tick
    // of a GOTO and freeze the position. A control-path decision (a start, an end, an abort) goes through
    // set_slewing_locked() and always drops the tick, which sampled before what the driver now knows. The caller
    // holds mutex_, which every control-path write holds.
    void note_slewing_read_locked(bool slewing) const {
        const auto held = polled_.read();
        if (held && held->value.slewing == slewing) {
            slewing_cached_ = slewing;
            return;
        }
        set_slewing_locked(slewing);
    }

    // Where the control path ends a motion (slew landed, park complete): the handset is read with mutex_ released and
    // the full frame is written through, so Slewing false is never served beside a mid-motion position, unless
    // `lost()` (evaluated with mutex_ held) says a newer operation owns the state meanwhile. The write keeps
    // `measured_at`; the next poll renews it. A failed read leaves the position unknown, which the getters refuse
    // until the next poll answers. It feeds no fault latch: only the poll does (decision 0009).
    template <typename Lost>
    void publish_measurement_released(std::unique_lock<std::mutex>& lock, const Lost& lost) const {
        std::optional<RawSample> raw;
        try {
            raw = without_lock(lock, [this] { return sample_raw(use_precise_commands_, true); });
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): an unset raw logs and clears below
        }
        if (lost()) {
            return;
        }
        if (raw) {
            const auto previous = polled_.read();
            const PolledFrame frame =
                decode_sample_locked(*raw, use_precise_commands_, previous ? previous->value : PolledFrame{});
            polled_.write([&frame](PolledFrame& held) { held = frame; });
            drop_poll_in_flight_locked();  // a poll that sampled before this read must not replace it
        } else {
            polled_.write([](PolledFrame& frame) {
                frame.equatorial_known = false;
                frame.altaz_known = false;
            });
            // Fixed text: the exchange error can carry handset reply bytes (CWE-117).
            ALPACA_LOG_WARN("SynScan",
                            "Position read failed at the end of a motion; position unknown until the next poll");
        }
    }

    // Control path: one direct read of the equatorial pair, for an operation that needs the mount's position now
    // (SetPark), with mutex_ released for the exchange. {RA hours, Dec degrees}. A failure while a poll fault is
    // latched throws the fault text.
    std::pair<double, double> measure_equatorial(std::unique_lock<std::mutex>& lock) const {
        int bits = use_precise_commands_ ? 24 : 16;
        try {
            const auto sample = without_lock(lock, [this] {
                const auto raw = SynScanProtocolWrapper::instance().get_ra_dec_raw(use_precise_commands_);
                return std::make_pair(raw, clock_.now());
            });
            double ra = decode_ra_hours(sample.first.first, bits);
            if (use_precise_commands_) {
                ra = corrected_ra_hours_locked(ra, sample.second);
            }
            return {ra, decode_angle(sample.first.second, bits)};
        } catch (const std::exception&) {
            throw_if_link_faulted();
            throw;
        }
    }

    // The position for an initiator that arrives while a body owns the slew slot: the polled frame, so the initiator
    // does not queue a read behind the body's exchange in flight (decision 0006, "Claim"). Without a frame it reads.
    std::pair<double, double> served_equatorial_or_measure(std::unique_lock<std::mutex>& lock) const {
        try {
            return served_equatorial_locked();
        } catch (const AlpacaException&) {
            return measure_equatorial(lock);
        }
    }

    std::pair<double, double> served_altaz_or_measure(std::unique_lock<std::mutex>& lock) const {
        try {
            return served_altaz_locked();
        } catch (const AlpacaException&) {
            return measure_altaz(lock);
        }
    }

    // {azimuth, altitude}, as measure_equatorial().
    std::pair<double, double> measure_altaz(std::unique_lock<std::mutex>& lock) const {
        int bits = use_precise_commands_ ? 24 : 16;
        try {
            const auto raw = without_lock(
                lock, [this] { return SynScanProtocolWrapper::instance().get_alt_az_raw(use_precise_commands_); });
            return {util::wrap_degrees(decode_angle(raw.first, bits)), decode_angle(raw.second, bits)};
        } catch (const std::exception&) {
            throw_if_link_faulted();
            throw;
        }
    }

    // The polled frame (decision 0010), on the poll thread (or the connect thread for the first call) with no driver
    // lock held. The module publishes it, counts a throw as a failed probe and latches the fault and the loss.
    PolledFrame probe() {
        try {
            PolledFrame frame = sample_frame();
            // The slow fields and the site read swallow their own failures, so a hangup there returns normally. The
            // wrapper has released the port: throw so the frame is not published and the catch below ends the poll in
            // this tick, not on the next one.
            if (!SynScanProtocolWrapper::instance().link_alive()) {
                throw AlpacaException("SynScan mount link lost during the poll", AlpacaError::NotConnected);
            }
            return frame;
        } catch (...) {
            // Positive evidence (decision 0009 rule 2a): the wrapper saw the serial port gone, in this probe or in an
            // earlier exchange. The poll's loss latch is set at once, so the poll ends after this tick.
            if (!SynScanProtocolWrapper::instance().link_alive()) polled_.lose_link();
            throw;
        }
    }

    PolledFrame sample_frame() {
        const bool seeding = seeding_.exchange(false);
        {
            // Waits out the readback-tick hunt, which owns the line while it runs. The hunt takes its hold under
            // mutex_ and this check and the set below are one critical section on hold_mutex_ (which the hunt's
            // increment also takes): either the hunt sees the probe sampling and waits for it, or the probe sees the
            // hold and waits; never both go ahead. Past the bound, or on a disconnect, the probe fails: the frame is
            // not renewed and no old frame is returned.
            std::unique_lock<std::mutex> hold_lock(hold_mutex_);
            clock_.wait_for(hold_lock, hold_cv_, kProbeHoldWaitBound,
                            [this] { return hunt_holds_ == 0 || closing_.load(); });
            if (closing_.load() || hunt_holds_ > 0) {
                throw AlpacaException(
                    "SynScan state poll skipped: the link is closing or the readback hunt holds the line",
                    AlpacaError::InvalidOperation);
            }
            probe_sampling_ = true;
        }
        const ProbeSampling sampling(*this);
        const int tick = probe_tick_++;
        const bool full = tick % kSlowPollEvery == 0;
        const bool precise = use_precise_commands_;
        bool read_site = false;
        {
            // The unread site (a connect that could not reach `w`) is retried here, not in a getter, and like the
            // sample below its exchange runs without mutex_, so a getter never waits on a handset that ignores `w`.
            std::lock_guard<std::mutex> lock(mutex_);
            read_site = !site_info_valid_ && full && site_read_due_locked();
        }
        if (read_site) {
            std::optional<LocationInfo> site;
            try {
                site = SynScanProtocolWrapper::instance().get_location();
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): retried after kSiteInfoRetryDelay
            }
            if (site) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!site_info_valid_) {  // a site write during the `w` is newer than it
                    apply_site_info_locked(*site);
                }
            }
        }
        RawSample raw;
        try {
            raw = sample_raw(precise, full);
        } catch (const std::exception&) {
            if (seeding) {
                return PolledFrame{};  // nothing known yet; the getters say so until a poll answers
            }
            throw;
        }
        run_after_sample_hook();
        std::lock_guard<std::mutex> lock(mutex_);  // the RA correction reads the readback tick
        const auto previous = polled_.read();
        return decode_sample_locked(raw, precise, previous ? previous->value : PolledFrame{});
    }

    void run_after_sample_hook() {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(probe_hook_mutex_);
            hook = probe_after_sample_hook_;
        }
        if (hook) {
            hook();
        }
    }

    // A fault the poll has latched (three failed probes): every getter that serves the link throws it. Takes only the
    // module's leaf mutex, so a rate getter never queues behind anything.
    void throw_if_link_faulted() const {
        const std::string fault = polled_.fault();
        if (!fault.empty()) {
            throw AlpacaException("SynScan mount communications compromised: " + fault, AlpacaError::DriverException);
        }
    }

    // The slewing the driver itself knows: a park in flight, a refinement, a manual axis, the start force window.
    bool slewing_by_driver_state_locked() const {
        // A park in flight reports Slewing until the park task flips AtPark
        // (same locked step) — never Slewing false with AtPark false.
        if (parking_) {
            return true;
        }
        if (refining_locked()) {
            return true;  // a GOTO whose landing is still being refined (open-astro#880)
        }
        if (manual_axis_slewing_[0] || manual_axis_slewing_[1]) {
            return true;
        }
        return slew_hold_.active(clock_.now());
    }

    bool get_slewing_locked() const { return slewing_by_driver_state_locked() || served_frame_locked().value.slewing; }

    // The PulseGuide slew gate: asks the handset for the end of the answer, not the polled frame. `lock` (mutex_) is
    // held on entry and on return; the `L` runs with it released.
    bool get_slewing_fresh(std::unique_lock<std::mutex>& lock) const {
        return slewing_by_driver_state_locked() || poll_hardware_slewing_locked(lock);
    }

    // Control path only: asks the handset now (the Park and landing loops are what detect a GOTO's end). The
    // getters serve the frame instead. ignore_dec_jog: a Dec-only MoveAxis does not touch the RA motor or
    // tracking, so ra_motion_active_locked() leaves it out.
    // The read runs with `lock` (mutex_) released. A body passes `lost`: nothing is applied, not even a failed read's
    // count, once `lost()` (evaluated with mutex_ held) says a newer operation owns the state (the caller checks that
    // itself).
    bool poll_hardware_slewing_locked(std::unique_lock<std::mutex>& lock, bool propagate_error = false,
                                      bool ignore_dec_jog = false, const std::function<bool()>& lost = {}) const {
        if (manual_axis_slewing_[0] || (manual_axis_slewing_[1] && !ignore_dec_jog)) {
            return true;
        }
        if (slew_hold_.active(clock_.now())) {
            return true;
        }
        bool was_slewing = slewing_cached_;
        try {
            const bool reply =
                without_lock(lock, [] { return SynScanProtocolWrapper::instance().is_goto_in_progress(); });
            if (lost && lost()) {
                return slewing_cached_;
            }
            note_slewing_read_locked(reply);
            if (propagate_error) {
                park_poll_failures_ = 0;
            }
        } catch (const std::exception& ex) {
            if (lost && lost()) {
                return slewing_cached_;  // the failure count belongs to the operation that replaced this body
            }
            if (propagate_error) {
                // One lost reply must not stop the mount mid-Park: only a run of failures fails it.
                const int failures = ++park_poll_failures_;
                if (failures >= kParkPollFailureThreshold) {
                    throw;
                }
                ALPACA_LOG_WARN("SynScan", "Park slew status poll failed (" + std::to_string(failures) + "/" +
                                               std::to_string(kParkPollFailureThreshold) + "): " + ex.what());
                return true;
            }
            // Keep last known state if polling times out.
        } catch (...) {
            if (lost && lost()) {
                return slewing_cached_;
            }
            if (propagate_error) {
                const int failures = ++park_poll_failures_;
                if (failures >= kParkPollFailureThreshold) {
                    throw;
                }
                ALPACA_LOG_WARN("SynScan", "Park slew status poll failed (" + std::to_string(failures) + "/" +
                                               std::to_string(kParkPollFailureThreshold) + ")");
                return true;
            }
            // Keep last known state if polling times out.
        }
        if (was_slewing && !slewing_cached_) {
            guide_position_valid_ =
                false;  // the position getters serve the frame; the control path publishes at the end
        }
        return slewing_cached_;
    }

    // True while a motion that owns the RA motor or the tracking state is running. The RA motor stop turns
    // handset tracking off, so the handset's mode is not read while one runs (open-astro#1259). A pulse counts
    // as running until its hold window ends, and the slew state is polled, never taken from the frame. A Dec-only jog
    // is not an RA motion.
    bool ra_motion_active_locked(std::unique_lock<std::mutex>& lock) const {
        const bool pulsing = pulse_hold_[0].active(clock_.now());
        if (pulsing || parking_ || refining_locked()) {
            return true;
        }
        return poll_hardware_slewing_locked(lock, false, true);
    }

    // Reads the handset's tracking mode into the cache before a command starts a motion or checks Tracking, so a
    // stop restores the mode the handset held at the start and not the one cached at connect: the keypad can change
    // it in between. A motion that starts while another RA motion runs keeps the mode the first one captured.
    void refresh_tracking_mode_locked(std::unique_lock<std::mutex>& lock) const {
        // A body that owns the slew slot is an RA motion in progress: its answer comes from the driver (before the
        // first read, the polled mode), never from a handset exchange queued behind the body's own (an initiator must
        // not wait for the old body).
        if (!tracking_mode_valid_ && slew_.running()) {
            seed_tracking_mode_from_frame_locked();
        }
        // After a restore that did not land, the cache is the mode still owed to the handset and the handset's own
        // answer is the driver's unfinished stop: the RA rate command replaced tracking and the `T` never arrived, so
        // it reads 0. Reading it here would make that 0 the mode to restore, and the next RA pulse, MoveAxis stop or
        // AbortSlew would leave the mount untracked. The flag clears with a landed restore, a landed Tracking write,
        // Park, an RA MoveAxis, AbortSlew or a reconnect; a Tracking write that fails puts its mode in the cache.
        if (tracking_mode_valid_ && ra_stop_unconfirmed_) {
            return;
        }
        if (tracking_mode_valid_ && (slew_.running() || ra_motion_active_locked(lock))) {
            return;
        }
        int mode = 0;
        const uint64_t generation = tracking_mode_generation_;
        try {
            mode = without_lock(lock, [] { return SynScanProtocolWrapper::instance().get_tracking_mode(); });
        } catch (...) {
            // A failed read keeps what the driver knew: before the first read that is the polled mode.
            if (!tracking_mode_valid_) {
                seed_tracking_mode_from_frame_locked();
            }
            throw;
        }
        // A mode the control path wrote during the read is newer than the reading, so it stays. Otherwise written
        // through to the frame Tracking serves only when it changed: every write drops the poll tick in flight, as for
        // the slewing flag (note_slewing_read_locked).
        if (tracking_mode_generation_ == generation) {
            const auto reading = polled_.read();
            if (reading && reading->value.tracking_mode != mode) {
                set_tracking_mode_cached_locked(mode);
            } else {
                tracking_mode_cached_ = mode;
            }
        }
        tracking_mode_valid_ = true;
    }

    void seed_tracking_mode_from_frame_locked() const {
        const auto reading = polled_.read();
        if (reading && reading->value.tracking_mode >= 0) {
            tracking_mode_cached_ = reading->value.tracking_mode;
            tracking_mode_valid_ = true;
        }
    }

    bool get_tracking_locked(std::unique_lock<std::mutex>& lock) const {
        if (!tracking_mode_valid_) {
            const uint64_t generation = tracking_mode_generation_;
            const int mode = without_lock(lock, [] { return SynScanProtocolWrapper::instance().get_tracking_mode(); });
            if (tracking_mode_generation_ == generation) {  // else a write during the read is newer than it
                tracking_mode_cached_ = mode;
            }
            tracking_mode_valid_ = true;
        }
        return tracking_mode_cached_ != 0 && !ra_stop_unconfirmed_;
    }

    AlignmentMode alignment_mode_locked() const {
        const int model_id = mount_model_id_.load();
        switch (model_id) {
            case 0:
            case 1:
            case 2:
            case 3:
            case 4:
            case 50:
            case 56:
                return AlignmentMode::GermanPolar;
            case 160:
                return AlignmentMode::AltAz;
            case 5:
            case 6:
                // The handset does not say which geometry an AZ-EQ mount is set
                // up in; only the device config's alignmentMode can (#860).
                if (alignment_setting_ == SynScanAlignmentSetting::AltAz) {
                    return AlignmentMode::AltAz;
                }
                if (alignment_setting_ == SynScanAlignmentSetting::Equatorial) {
                    return AlignmentMode::GermanPolar;
                }
                throw AlpacaException(
                    "SynScan cannot report whether this AZ-EQ mount is currently configured "
                    "for Alt-Az or equatorial alignment",
                    AlpacaError::DriverException);
            default:
                if ((model_id >= 128 && model_id <= 159)) {
                    return AlignmentMode::AltAz;
                }
                throw AlpacaException(
                    "SynScan cannot determine AlignmentMode for mount model ID " + std::to_string(model_id),
                    AlpacaError::DriverException);
        }
    }

    // No handset read: the status poll retries an unread site (decision 0010), so a getter never waits on a `w`.
    void require_site_known_locked() const {
        check_connected();
        if (!site_info_valid_) {
            throw AlpacaException(
                "SynScan site is not known yet: the handset has not answered a site read and none is "
                "configured",
                AlpacaError::ValueNotSet);
        }
    }

    // `lock` (mutex_) is held on entry and on return; the `w` read runs with it released.
    void ensure_site_info_cached(std::unique_lock<std::mutex>& lock) const {
        if (!site_read_due_locked()) {
            return;
        }
        try {
            const LocationInfo info =
                without_lock(lock, [] { return SynScanProtocolWrapper::instance().get_location(); });
            if (!site_info_valid_) {  // a SiteLatitude/SiteLongitude write during the `w` is newer than it
                apply_site_info_locked(info);
            }
        } catch (...) {  // NOLINT(bugprone-empty-catch): an unread site stays unread, one written meanwhile known
        }
    }

    // True when a `w` read should run now (an unread site past its retry delay); stamps the attempt.
    bool site_read_due_locked() const {
        auto now = clock_.now();
        if (!site_info_valid_ && last_site_info_attempt_ != util::TaskClock::clock::time_point::min() &&
            (now - last_site_info_attempt_) < kSiteInfoRetryDelay) {
            return false;
        }
        last_site_info_attempt_ = now;
        return true;
    }

    void apply_site_info_locked(const LocationInfo& info) const {
        site_latitude_cached_ = info.latitude_degrees;
        site_longitude_cached_ = info.longitude_degrees;
        site_info_valid_ = true;
    }

    // Saves the current RA/Dec as the park target, as main did for every mount; the polled one while a body runs.
    void store_legacy_park_position(std::unique_lock<std::mutex>& lock) {
        const auto equatorial = slew_.running() ? served_equatorial_or_measure(lock) : measure_equatorial(lock);
        park_alignment_mode_ = AlignmentMode::GermanPolar;
        park_ra_uses_hour_angle_ = false;
        park_ra_hours_ = equatorial.first;
        park_dec_degrees_ = equatorial.second;
        park_position_set_ = true;
    }

    void store_park_position(std::unique_lock<std::mutex>& lock) {
        try {
            park_alignment_mode_ = alignment_mode_locked();
        } catch (const AlpacaException&) {
            // Dual-mode and unidentified handsets cannot tell us whether the
            // saved RA/Dec needs sidereal conversion. Keep their prior usable
            // park behavior instead of making SetPark and Park unavailable.
            store_legacy_park_position(lock);
            return;
        }
        if (park_alignment_mode_ == AlignmentMode::AltAz) {
            const auto altaz = slew_.running() ? served_altaz_or_measure(lock) : measure_altaz(lock);
            park_azimuth_degrees_ = altaz.first;
            park_altitude_degrees_ = altaz.second;
            park_ra_uses_hour_angle_ = false;
        } else {
            if (!site_info_valid_ && !slew_.running()) {  // no `w` queued behind a body's exchange
                ensure_site_info_cached(lock);
            }
            if (!site_info_valid_) {
                // No longitude, so no LST for an hour angle: keep the prior
                // park behavior rather than making SetPark and Park unavailable.
                store_legacy_park_position(lock);
                return;
            }
            const auto equatorial = slew_.running() ? served_equatorial_or_measure(lock) : measure_equatorial(lock);
            const double lst = util::local_sidereal_hours(sidereal_wall_now(), site_longitude_cached_);
            park_hour_angle_hours_ = util::shortest_ra_delta_hours(lst, equatorial.first);
            park_ra_hours_ = equatorial.first;
            park_dec_degrees_ = equatorial.second;
            park_ra_uses_hour_angle_ = true;
        }
        park_position_set_ = true;
    }

    LocationInfo current_location(std::unique_lock<std::mutex>& lock) const {
        LocationInfo info;
        if (site_info_valid_) {
            info.latitude_degrees = site_latitude_cached_;
            info.longitude_degrees = site_longitude_cached_;
            return info;
        }
        return without_lock(lock, [] { return SynScanProtocolWrapper::instance().get_location(); });
    }

    // A GOTO sent in its turn after the caller armed slew_hold_ (decision 0022 rule 2: armed before the command goes
    // out). A send that throws while the caller still owns the slew is a failed dispatch: the window is cleared before
    // the error goes on, and with `ends_slewing` Slewing is published false too, so a client is not told the mount
    // slews for the length of the hold after a GOTO that never went out. Clear, not restore: the caller's claim ended
    // any earlier motion's ownership, and the poll's next `L` reports a mount still moving. A caller overtaken during
    // the exchange leaves both to the operation that replaced it. Park passes ends_slewing false: its failure path
    // (fail_park_locked) stops the axes and decides Slewing from that stop.
    template <typename Owns, typename Fn>
    bool send_goto_in_turn_locked(std::unique_lock<std::mutex>& lock, Owns&& owns, Fn&& fn, bool ends_slewing) {
        try {
            return send_in_turn(lock, owns, std::forward<Fn>(fn));
        } catch (...) {
            if (owns()) {
                slew_hold_.clear();
                if (ends_slewing) {
                    set_slewing_locked(false);
                }
            }
            throw;
        }
    }

    // The GOTO is sent in its wire turn with mutex_ released (`lock` is held on entry and on return). `owns` says the
    // caller still owns the slew, at the start and again at the turn: a caller superseded meanwhile sends nothing and
    // leaves the target and the flags to the operation that replaced it.
    // `ends_slewing`: see send_goto_in_turn_locked().
    template <typename Owns>
    void do_slew_to_coordinates_locked(double ra, double dec, std::unique_lock<std::mutex>& lock, Owns&& owns,
                                       bool ends_slewing) {
        util::validate_ra_dec(ra, dec, "SlewToCoordinates");
        if (!owns()) {
            return;
        }
        auto& protocol = SynScanProtocolWrapper::instance();
        int bits = use_precise_commands_ ? 24 : 16;
        uint32_t ra_raw = encode_ra_raw(ra, bits);
        uint32_t dec_raw = encode_angle(dec, bits);
        drop_poll_in_flight_locked();
        set_slewing_locked(true);
        // open-astro#575: a fresh initiator is a clean start -- a client
        // that retries a rejected goto (even via the blocking
        // SlewToCoordinates) must not be told the OLD goto failed.
        last_slew_error_.clear();
        slew_hold_.arm(clock_.now(), std::chrono::seconds(8));
        position_override_until_ = util::TaskClock::clock::time_point::min();
        guide_position_valid_ = false;
        if (!send_goto_in_turn_locked(
                lock, owns, [&] { protocol.goto_ra_dec_raw(ra_raw, dec_raw, use_precise_commands_); }, ends_slewing) ||
            !owns()) {
            return;
        }
        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_ra_set_ = true;
        target_dec_set_ = true;
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
        parked_ = false;
        at_home_ = false;
    }

    // Park's Alt-Az dispatch: a failed send clears the window only (see send_goto_in_turn_locked()).
    template <typename Owns>
    void do_slew_to_altaz_locked(double altitude, double azimuth, std::unique_lock<std::mutex>& lock, Owns&& owns) {
        if (!owns()) {
            return;
        }
        auto& protocol = SynScanProtocolWrapper::instance();
        int bits = use_precise_commands_ ? 24 : 16;
        uint32_t az_raw = encode_angle(azimuth, bits);
        uint32_t alt_raw = encode_angle(altitude, bits);
        drop_poll_in_flight_locked();
        set_slewing_locked(true);
        last_slew_error_.clear();
        slew_hold_.arm(clock_.now(), std::chrono::seconds(8));
        position_override_until_ = util::TaskClock::clock::time_point::min();
        guide_position_valid_ = false;
        if (!send_goto_in_turn_locked(
                lock, owns, [&] { protocol.goto_alt_az_raw(az_raw, alt_raw, use_precise_commands_); }, false) ||
            !owns()) {
            return;
        }
        parked_ = false;
        at_home_ = false;
    }

    // Poll for slew completion, RELEASING the driver mutex around every sleep
    // so a sync slew/park doesn't block all GETs and disconnect for up to
    // 120 s (Bisque's unlock/sleep/relock loop is the reference pattern).
    // `lock` must be held on entry; it is held again on return/throw. The
    // connection state is re-checked after each relock.
    // Returns false when a concurrent motion command took `claim` (claim_lost_locked()) while the lock was released
    // (the slew was superseded or stopped).
    // `ctx` (async slew body only) makes every sleep interruptible through the slot's wait_for(), the way the park
    // body waits, so a join by Disconnect, MoveAxis, a new slew or the destructor returns at once; the wait then
    // returns false.
    // True once a newer operation has taken the axes or a stop cancelled them (claim_lost_locked()) or the body behind
    // `ctx` was cancelled or replaced. Caller holds mutex_.
    bool lost_ownership_locked(const util::MotionClaim& claim, const util::OperationContext* ctx) const {
        return claim_lost_locked(claim) || slot_stopped(ctx);
    }

    bool wait_for_slew_complete(std::unique_lock<std::mutex>& lock, const util::MotionClaim& claim,
                                util::OperationContext* ctx = nullptr) {
        auto start = clock_.now();
        const auto start_grace = std::chrono::seconds(2);
        bool saw_slewing = false;
        auto sleep_unlocked = [&](std::chrono::milliseconds d) {
            lock.unlock();
            if (ctx != nullptr) {
                ctx->wait_for(d);
            } else {
                clock_.sleep_for(d);
            }
            lock.lock();
            // The mount may have been disconnected while the lock was released.
            check_connected();
        };
        while (true) {
            if (claim_lost_locked(claim) || (ctx != nullptr && ctx->stop_reason() != util::StopReason::None)) {
                return false;
            }
            // The hardware answer, not get_slewing_locked(): a refinement scope holds the public Slewing true. The
            // read runs with mutex_ released; a poll that was overtaken applies nothing.
            bool slewing =
                poll_hardware_slewing_locked(lock, false, false, [&] { return lost_ownership_locked(claim, ctx); });
            if (lost_ownership_locked(claim, ctx)) {
                return false;
            }
            check_connected();
            if (slewing) {
                saw_slewing = true;
            }
            if (!slewing) {
                if (!saw_slewing && (clock_.now() - start) < start_grace) {
                    sleep_unlocked(std::chrono::milliseconds(200));
                    continue;
                }
                break;
            }
            if (clock_.now() - start > kSlewTimeout) {
                fail_slew_timeout_locked(lock, claim, ctx);
                return false;  // a newer operation took the mount meanwhile: it owns the state
            }
            sleep_unlocked(std::chrono::milliseconds(250));
        }
        // The measurement is read with mutex_ released; a body overtaken meanwhile publishes and clears nothing.
        publish_measurement_released(lock, [&] { return lost_ownership_locked(claim, ctx); });
        if (lost_ownership_locked(claim, ctx)) {
            return false;
        }
        set_slewing_locked(false);
        slew_hold_.clear();
        guide_position_valid_ = false;
        position_override_until_ = util::TaskClock::clock::time_point::min();
        return true;
    }

    // The configured settle time, applied once after the LAST GOTO pass (open-astro#880). `lock` held on entry
    // and on return; false when a concurrent motion command superseded the slew meanwhile.
    bool settle_after_slew_locked(std::unique_lock<std::mutex>& lock, const util::MotionClaim& claim) const {
        if (slew_settle_time_seconds_ > 0) {
            lock.unlock();
            clock_.sleep_for(std::chrono::seconds(slew_settle_time_seconds_));
            lock.lock();
            check_connected();
        }
        return !claim_lost_locked(claim);
    }

    // Marks a GOTO whose landing will be refined: get_slewing_locked() reports true while the scope's claim holds,
    // so Slewing cannot dip between the passes. Cleared on every exit; a newer motion command (or a stop) takes the
    // claim, which ends it on its own. `skip` leaves it unset (no refinement).
    class RefineScope {
    public:
        RefineScope(const SynScanTelescopeDriver& driver, const util::MotionClaim& claim, bool skip)
            : driver_(driver), claim_(claim) {
            if (!skip) {
                driver_.refining_claim_ = claim;
            }
        }
        ~RefineScope() {
            if (driver_.refining_claim_ && driver_.refining_claim_->generation == claim_.generation) {
                driver_.refining_claim_.reset();
            }
        }
        RefineScope(const RefineScope&) = delete;
        RefineScope& operator=(const RefineScope&) = delete;

    private:
        const SynScanTelescopeDriver& driver_;
        util::MotionClaim claim_;
    };

    // A GOTO whose landing is being refined and whose claim still holds. Caller holds mutex_.
    bool refining_locked() const { return refining_claim_ && !claim_lost_locked(*refining_claim_); }

    // After a GOTO completes, read the handset's own position at its tick (open-astro#1030) and, while RA or Dec is
    // off by more than kLandingToleranceArcsec, finish the landing with timed guide-rate moves, at most
    // kMaxLandingRounds rounds (open-astro#880). The handset re-rolls every GOTO landing by a whole second of RA,
    // so a residual is never chased with another GOTO; only one beyond a timed move's reach gets a single re-GOTO
    // first. A landing beyond kLandingRefineLimitArcsec gets one repeat GOTO to the same target, and throws if it
    // misses that far again (open-astro#1105). 16-bit handsets (V3) step ~19.8" in RA, so they are not refined.
    // `lock` is held on entry and on return. Returns false when the slew was superseded, aborted or cancelled; throws
    // when a move fails. A residual left after the last round is logged as a WARN, not thrown (open-astro#1027).
    bool refine_goto_landing_locked(std::unique_lock<std::mutex>& lock, double ra, double dec,
                                    const util::MotionClaim& claim, util::OperationContext* ctx) {
        if (!use_precise_commands_) {
            return true;
        }
        // The rounds move the mount after the slew-end frame was published: publish where the landing ended, unless
        // a newer motion owns the axes (its own end publishes).
        bool finished = false;
        try {
            finished = refine_goto_landing_rounds_locked(lock, ra, dec, claim, ctx);
        } catch (...) {
            if (!claim_lost_locked(claim) && connected_) {
                publish_measurement_released(lock, [&] { return claim_lost_locked(claim); });
            }
            throw;
        }
        if (!claim_lost_locked(claim) && connected_) {
            publish_measurement_released(lock, [&] { return claim_lost_locked(claim); });
        }
        return finished;
    }

    bool refine_goto_landing_rounds_locked(std::unique_lock<std::mutex>& lock, double ra, double dec,
                                           const util::MotionClaim& claim, util::OperationContext* ctx) {
        auto& protocol = SynScanProtocolWrapper::instance();
        const double nudge_reach_arcsec = kNudgeMaxSeconds * std::min(guide_rate_.ra, guide_rate_.dec) * 3600.0;
        const bool timed_finish = equatorial_geometry_locked();
        bool regoto_done = false;
        bool far_miss_repeated = false;
        // The re-GOTO aim carries over between rounds (open-astro#1030): a residual cancels a systematic landing
        // offset instead of repeating it, and RA inside the tolerance keeps its aim.
        double aim_ra = ra;
        double aim_dec = dec;
        for (int round = 0;; ++round) {
            if (claim_lost_locked(claim) || (ctx != nullptr && ctx->stop_reason() != util::StopReason::None)) {
                return false;
            }
            check_connected();
            const TickHunt hunt = hunt_readback_tick_locked(lock, claim, ctx);
            if (claim_lost_locked(claim) || (ctx != nullptr && ctx->stop_reason() != util::StopReason::None)) {
                return false;
            }
            double landed_ra_hours = decode_ra_hours(hunt.raw.first, 24);
            if (hunt.found) {
                readback_tick_ = hunt.tick;
                readback_tick_valid_until_ = hunt.read_at + kReadbackTickValidity;
                readback_top_hours_ = hunt.top_hours;
                readback_top_valid_ = true;
                landed_ra_hours = hunt.top_hours;
            }
            double ra_error_hours = util::shortest_ra_delta_hours(ra, landed_ra_hours);
            const double dec_error_degrees = dec - decode_angle(hunt.raw.second, 24);
            const double ra_error_arcsec = std::abs(ra_error_hours) * kHoursToDegrees * 3600.0;
            const double dec_error_arcsec = std::abs(dec_error_degrees) * 3600.0;
            if (ra_error_arcsec <= kLandingToleranceArcsec && dec_error_arcsec <= kLandingToleranceArcsec) {
                return true;
            }
            if (ra_error_arcsec > kLandingRefineLimitArcsec || dec_error_arcsec > kLandingRefineLimitArcsec) {
                const std::string miss = "GOTO ended " + std::to_string(ra_error_arcsec) + " arcsec (RA) and " +
                                         std::to_string(dec_error_arcsec) + " arcsec (Dec) from the target";
                if (far_miss_repeated) {
                    throw AlpacaException(miss + " after a repeat GOTO", AlpacaError::DriverException);
                }
                ALPACA_LOG_WARN("SynScan", miss + "; repeating the GOTO once (open-astro#1105)");
                // The same target, not one aimed by the residual: the miss is the handset's, not an offset to
                // correct, and an aim moved by 49' would land 49' the other way. The carried aim returns to the
                // target too, so the next re-GOTO round is aimed by the repeat's residual alone (open-astro#1120).
                far_miss_repeated = true;
                aim_ra = ra;
                aim_dec = dec;
                drop_poll_in_flight_locked();
                guide_position_valid_ = false;
                set_slewing_locked(true);
                slew_hold_.arm(clock_.now(), kRefineStartForce);
                if (!send_goto_in_turn_locked(
                        lock, [&] { return !lost_ownership_locked(claim, ctx); },
                        [&] { protocol.goto_ra_dec_raw(encode_ra_raw(ra, 24), encode_angle(dec, 24), true); }, true)) {
                    return false;
                }
                if (!wait_for_slew_complete(lock, claim, ctx)) {
                    return false;
                }
                continue;
            }
            if (round >= kMaxLandingRounds) {
                ALPACA_LOG_WARN("SynScan", "SynScan GOTO landed " + std::to_string(ra_error_arcsec) +
                                               " arcsec (RA) and " + std::to_string(dec_error_arcsec) +
                                               " arcsec (Dec) off target after " + std::to_string(kMaxLandingRounds) +
                                               " refinement passes");
                return true;
            }
            drop_poll_in_flight_locked();
            guide_position_valid_ = false;
            if (!timed_finish ||
                (!regoto_done && (ra_error_arcsec > nudge_reach_arcsec || dec_error_arcsec > nudge_reach_arcsec))) {
                // Too far for a timed move: one re-GOTO, aimed by the residual, brings it within reach. Without the
                // timed finish (not known equatorial) every round is a re-GOTO.
                regoto_done = true;
                // RA inside the tolerance keeps its aim, so a Dec-driven round does not chase the handset's
                // one-second RA readback steps (open-astro#1030).
                if (ra_error_arcsec > kLandingToleranceArcsec) {
                    aim_ra = std::fmod(aim_ra + ra_error_hours + 24.0, 24.0);
                }
                aim_dec = std::clamp(aim_dec + dec_error_degrees, -90.0, 90.0);
                set_slewing_locked(true);
                slew_hold_.arm(clock_.now(), kRefineStartForce);
                if (!send_goto_in_turn_locked(
                        lock, [&] { return !lost_ownership_locked(claim, ctx); },
                        [&] { protocol.goto_ra_dec_raw(encode_ra_raw(aim_ra, 24), encode_angle(aim_dec, 24), true); },
                        true)) {
                    return false;
                }
                if (!wait_for_slew_complete(lock, claim, ctx)) {
                    return false;
                }
                continue;
            }
            if (ra_error_arcsec > kLandingToleranceArcsec &&
                !nudge_axis_locked(lock, 0, ra_error_hours * kHoursToDegrees * 3600.0, claim, ctx)) {
                return false;
            }
            if (dec_error_arcsec > kLandingToleranceArcsec &&
                !nudge_axis_locked(lock, 1, dec_error_degrees * 3600.0, claim, ctx)) {
                return false;
            }
        }
    }

    void sync_mount_time(std::unique_lock<std::mutex>& lock) {
        apply_utc_date(std::chrono::system_clock::now(), lock, false);
    }

public:
    // open-astro#2419: the server's 1 s timer saw the host clock step or become trusted. The timer thread calls this
    // with no lock held: it does no I/O, joins nothing and never waits for transition_mutex_. It records the change
    // and starts the re-sync body on the clock slot when it can take transition_mutex_ at once; otherwise the holder
    // (a connect or a disconnect) starts it as it lets go (drain_pending_resync()). AsyncOperation::start() itself can
    // wait up to kStaleReapTimeout when earlier bodies are stuck on a blocked link (#2432).
    void on_host_clock_changed(const util::HostClockEvent& event) noexcept override {
        if (!sync_time_on_connect_) return;  // fixed at construction
        resync_stepped_.store(event.kind == util::HostClockEvent::Kind::Stepped);
        resync_step_ms_.store(static_cast<int64_t>(event.step.count()));
        resync_pending_.store(true);
        drain_pending_resync();
    }

private:
    // Starts the re-sync a host clock change left pending, unless another thread holds transition_mutex_: every
    // holder calls this after letting go, so a change recorded while the lock was held is never lost. A change seen
    // while disconnected is dropped. Call with no driver lock held.
    void drain_pending_resync() noexcept {
        try {
            while (resync_pending_.load()) {
                std::unique_lock<std::mutex> transition(transition_mutex_, std::try_to_lock);
                if (!transition.owns_lock()) return;  // its holder drains as it lets go
                if (!resync_pending_.exchange(false)) return;
                start_clock_resync_transition_locked();
            }
        } catch (...) {  // NOLINT(bugprone-empty-catch): a timer-thread hook must not throw
        }
    }

    // Under transition_mutex_ and no other driver lock. A refused start() (a slot still draining, a refused thread,
    // earlier bodies stuck on a blocked link) is logged; the next host clock change tries again.
    void start_clock_resync_transition_locked() noexcept {
        try {
            if (!connected_.load() || link_lost()) return;
            const std::string why = resync_stepped_.load()
                                        ? "Host clock stepped by " + std::to_string(resync_step_ms_.load()) + " ms"
                                        : std::string("Host clock became trusted");
            // A client UTCDate write after this point is newer than the re-sync and keeps the mount clock.
            const uint64_t decided_generation = utc_write_generation_.load();
            ++clock_resync_starts_;
            clock_resync_.start([this, why, decided_generation](util::OperationContext& ctx) {
                run_clock_resync(ctx, why, decided_generation);
            });
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("SynScan", std::string("Could not start the mount clock re-sync: ") + e.what());
        } catch (...) {
            ALPACA_LOG_WARN("SynScan", "Could not start the mount clock re-sync");
        }
    }

    // Under mutex_: motion the driver started is under way: a GOTO, Park or landing body, a MoveAxis, or a pulse guide
    // (its stop body included, which sends with no wire turn). slewing_cached_ is not read here: on a V3 handset an
    // async GOTO body ends at the `R` and nothing clears the flag after the landing, so the handset is asked instead
    // (clock_resync_handset_slewing_locked()).
    bool clock_resync_motion_locked() const {
        if (slew_.running() || slewing_by_driver_state_locked()) return true;
        const auto now = clock_.now();
        for (std::size_t axis = 0; axis < pulse_hold_.size(); ++axis) {
            if (pulse_hold_[axis].active(now) || pulse_ops_[axis].running()) {
                return true;
            }
        }
        return false;
    }

    // Under the re-sync's wire turn and mutex_ (released for the `L`): the handset's own slewing answer, as the
    // PulseGuide gate asks it, so a landed V3 GOTO (or a keypad GOTO) is read as it is now. A motion initiator that
    // claims or stops the axes during the `L` bumps motion_epoch_ (claim_motion_locked(), cancel_motion_locked()),
    // and the answer is then not written over its slewing flag. A failed `L` keeps the last flag.
    bool clock_resync_handset_slewing_locked(std::unique_lock<std::mutex>& lock) const {
        const uint64_t epoch = motion_epoch_;
        return poll_hardware_slewing_locked(lock, false, false, [&] { return motion_epoch_ != epoch; });
    }

    // Under the re-sync's wire turn and mutex_ (released for the `J`): true only when the handset answers that it
    // holds no star alignment. An aligned handset aims by a pointing model built on its clock, so changing the clock
    // shifts the model by the size of the change: the re-sync leaves it alone (a client UTCDate write or a reconnect
    // still sets it). A failed `J` leaves it alone too, since an aligned handset cannot be ruled out.
    bool clock_resync_handset_unaligned_locked(std::unique_lock<std::mutex>& lock, const std::string& why) const {
        try {
            if (!without_lock(lock, [] { return SynScanProtocolWrapper::instance().is_aligned(); })) return true;
            ALPACA_LOG_INFO("SynScan", why +
                                           "; the hand controller is star aligned, so the re-sync leaves its clock "
                                           "alone; a client UTCDate write or a reconnect still sets it");
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("SynScan", why +
                                           "; the hand controller's alignment query failed, so the re-sync leaves "
                                           "the mount clock alone: " +
                                           e.what());
        }
        return false;
    }

    // Under config_write_mutex_ and mutex_: the re-sync may still write. False once disconnected, stopped, or a client
    // UTCDate write landed after the re-sync was decided (that write keeps the mount clock).
    bool clock_resync_may_write_locked(const util::OperationContext& ctx, uint64_t decided_generation) const {
        if (!connected_ || link_lost() || ctx.stop_reason() != util::StopReason::None) return false;
        if (utc_write_generation_.load() != decided_generation) {
            ALPACA_LOG_INFO("SynScan", "A client UTCDate write after the host clock change keeps the mount clock");
            return false;
        }
        return true;
    }

    // The re-sync body (open-astro#2419), on the clock slot: writes the host clock into the handset, as the connect
    // does with syncTimeOnConnect on, once it is trusted. It never writes under driver motion, since the handset aims
    // a GOTO by its clock: the motion check runs at the write's own wire turn, which every motion command takes
    // (WireTurn), and asks the handset (`L`) there, so no motion command can go out between the check and the `H`;
    // in the same turn it asks `J` and leaves a star-aligned handset's clock alone (or one whose `J` fails);
    // the pulse stop body takes no turn, and is checked as running. A re-sync that finds motion WAITS for it to end,
    // looking again every util::kHostClockResyncMotionPoll on the task clock and giving up (logged) after
    // util::kHostClockResyncMotionBound; a disconnect or a newer re-sync ends the wait. It holds no lock while it
    // waits. config_write_mutex_ is held across the check, the `H` and the publish, so
    // a client UTCDate or site write is either before it (and a UTCDate write ends it) or after it on the wire. A
    // failed `H` is logged and leaves the mount clock: it feeds no fault latch (only the poll does).
    void run_clock_resync(util::OperationContext& ctx, const std::string& why, uint64_t decided_generation) {
        bool wrote = false;
        struct Done {
            SynScanTelescopeDriver& driver;
            const bool& wrote;
            ~Done() { driver.notify_clock_resync_done_for_testing(wrote); }
        } done{*this, wrote};
        try {
            if (!util::HostClockTrust::trusted()) {
                ALPACA_LOG_DEBUG("SynScan", why + "; the host clock is not trusted yet, the mount clock is left alone");
                return;
            }
            const auto give_up_at = clock_.now() + util::kHostClockResyncMotionBound;
            bool logged_wait = false;
            while (true) {
                {
                    std::lock_guard<std::mutex> config(config_write_mutex_);
                    std::unique_lock<std::mutex> lock(mutex_);
                    if (!clock_resync_may_write_locked(ctx, decided_generation)) return;
                    if (!clock_resync_motion_locked()) {
                        WireTurn turn(*this, lock);
                        if (!clock_resync_may_write_locked(ctx, decided_generation)) return;
                        const bool moving = clock_resync_motion_locked() || clock_resync_handset_slewing_locked(lock);
                        // The `L` released mutex_, so everything is checked again after it.
                        // cppcheck-suppress identicalConditionAfterEarlyExit
                        if (!clock_resync_may_write_locked(ctx, decided_generation)) return;
                        if (!moving && !clock_resync_motion_locked()) {
                            if (!clock_resync_handset_unaligned_locked(lock, why)) return;
                            // The `J` released mutex_ too.
                            if (!clock_resync_may_write_locked(ctx, decided_generation)) return;
                            if (!clock_resync_motion_locked()) {
                                apply_utc_date(std::chrono::system_clock::now(), lock, true);
                                wrote = true;  // NOLINT(clang-analyzer-deadcode.DeadStores): `done` reads it
                                ALPACA_LOG_INFO("SynScan", why + "; the mount clock was set from the host clock");
                                return;
                            }
                        }
                    }
                }
                if (!logged_wait) {
                    ALPACA_LOG_INFO("SynScan", why +
                                                   "; the mount is moving, the mount clock re-sync waits for the "
                                                   "motion to end");
                    logged_wait = true;
                }
                if (clock_.now() >= give_up_at) {
                    ALPACA_LOG_WARN("SynScan", why + "; the mount was still moving after " +
                                                   std::to_string(util::kHostClockResyncMotionBound.count()) +
                                                   " minutes, so the re-sync gives up; the next host clock change or "
                                                   "a client UTCDate write sets the hand controller clock");
                    return;
                }
                if (!ctx.wait_for(util::kHostClockResyncMotionPoll)) return;
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("SynScan",
                            why + "; the mount clock re-sync failed, the mount clock is left as it was: " + e.what());
        } catch (...) {
            ALPACA_LOG_WARN("SynScan", why + "; the mount clock re-sync failed, the mount clock is left as it was");
        }
    }

    void notify_clock_resync_done_for_testing(bool wrote) noexcept {
        try {
            std::function<void(bool)> hook;
            {
                std::lock_guard<std::mutex> lock(clock_resync_hook_mutex_);
                hook = clock_resync_done_hook_;
            }
            if (hook) hook(wrote);
        } catch (...) {  // NOLINT(bugprone-empty-catch): a test hook must not escape the body
        }
    }

public:
    void set_clock_resync_done_hook_for_testing(std::function<void(bool)> hook) {
        std::lock_guard<std::mutex> lock(clock_resync_hook_mutex_);
        clock_resync_done_hook_ = std::move(hook);
    }

    int clock_resync_starts_for_testing() const { return clock_resync_starts_.load(); }

    void set_connect_start_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> lock(clock_resync_hook_mutex_);
        connect_start_hook_ = std::move(hook);
    }

private:
    // Runs the connect-start test hook, under transition_mutex_ and, on a relink, initiator_mutex_ too; never under
    // mutex_ or a wire turn (see set_connect_start_hook_for_testing()).
    void notify_connect_start_for_testing() {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(clock_resync_hook_mutex_);
            hook = connect_start_hook_;
        }
        if (hook) hook();
    }

    int device_number_;
    ConnectionInfo connection_info_;
    // Set by the auto-detect factory; empty for an explicit port or host.
    // connection_resolved_ is true once a connect has run the resolver, so a
    // later connect retries that endpoint before scanning again (#659).
    util::ConnectionResolver<ConnectionInfo> connection_resolver_;
    bool connection_resolved_ = false;
    SynScanVersion version_;
    mutable std::mutex mutex_;
    bool client_disagreement_warned_ = false;  // open-astro#409, re-armed on connect
    std::atomic<bool> connected_;              // set last by the connect, read lock-free by get_connected()

    double target_ra_hours_;
    double target_dec_degrees_;
    double guide_position_ra_hours_ = 0.0;
    double guide_position_dec_degrees_ = 0.0;
    mutable bool guide_position_valid_ = false;
    std::atomic<double> aperture_diameter_m_;
    std::atomic<double> focal_length_m_;
    // The polled frame the getters serve, with the fault latch and the loss latch (decision 0010, module
    // util::PolledState). One publisher (the poll, plus the control path's write-through), any number of readers.
    // The hunt/probe exclusion (see probe() and hunt_readback_tick_locked()). hold_mutex_ is a leaf: taken under
    // mutex_ or alone, never the other way round, and nothing waits on another lock or does I/O while holding it.
    mutable std::mutex hold_mutex_;
    mutable std::condition_variable hold_cv_;
    mutable int hunt_holds_ = 0;        // hunts that own the line; a probe waits for 0
    bool probe_sampling_ = false;       // a probe is inside its exchanges
    std::atomic<bool> closing_{false};  // a disconnect is under way: probes and the hunt stop at the next exchange
    std::atomic<bool> seeding_{false};  // the next probe is the connect's first and may return an unknown frame
    mutable std::mutex probe_hook_mutex_;
    std::function<void()> probe_after_sample_hook_;  // test seam, see set_status_poll_after_sample_hook_for_testing()
    std::atomic<int> probe_tick_{0};  // probe count this session; a multiple of kSlowPollEvery is a full sample
    // open-astro#1030: the handset's once-a-second RA readback tick, anchored after the last GOTO. Valid until the
    // deadline; a tracking-mode or time write and connect/disconnect invalidate it.
    mutable util::TaskClock::clock::time_point readback_tick_;
    mutable util::TaskClock::clock::time_point readback_tick_valid_until_ = util::TaskClock::clock::time_point::min();
    // The last RA the driver knows to be true (a landing read at the tick, a sync, a pulse end), used to pick the
    // side of the tick for a read inside its uncertainty. Dropped by motion of unknown size (AbortSlew, MoveAxis).
    mutable double readback_top_hours_ = 0.0;
    mutable bool readback_top_valid_ = false;
    // open-astro#1107: a hunt's first-jump budget expired with no jump, so this handset shows no sawtooth; later
    // landings on the connection read once instead of hunting. Dropped with the anchor (connect, tracking, time).
    mutable bool readback_tick_absent_ = false;

    mutable double site_latitude_cached_;
    mutable double site_longitude_cached_;
    mutable bool site_info_valid_;
    mutable util::TaskClock::clock::time_point last_site_info_attempt_;
    std::optional<double> site_elevation_m_;  // nullopt until set or configured
    mutable int timezone_offset_minutes_;
    mutable bool timezone_offset_valid_;
    mutable bool dst_observed_;
    mutable util::UtcOffset utc_offset_;
    mutable int tracking_mode_cached_;
    mutable uint64_t tracking_mode_generation_ = 0;  // bumped on every control-path write of tracking_mode_cached_
    mutable bool tracking_mode_valid_;
    // A pulse-guide end whose tracking restore failed: the RA motor may be off while the cache still holds the mode.
    // The public Tracking read reports false while it is set, and refresh_tracking_mode_locked() keeps the cached
    // mode instead of the handset's 0, so the next RA stop restores it. A GOTO is refused while it is set; a pulse end
    // that fails while a GOTO reaps it sets the flag after the Tracking gate, and it stays set across the GOTO because
    // that T never landed.
    mutable bool ra_stop_unconfirmed_ = false;
    // open-astro#346 (the shape #304 fixed on the Sky-Watcher driver): ASCOM
    // treats the two target properties as independent, so each must throw
    // ValueNotSet until that property itself has been written. One shared flag
    // let a write to either unlock both, and a client reading the one it did
    // not set got a default 0 instead of an error. The paths that legitimately
    // define both coordinates at once -- the slew and sync coordinate forms,
    // the position-override and arrival reads, and the connect/disconnect
    // resets -- still set or clear both.
    mutable bool target_ra_set_ = false;
    mutable bool target_dec_set_ = false;
    mutable bool parked_;
    mutable bool at_home_;
    mutable bool slewing_cached_ = false;  // the control path's own copy; the frame carries the served one
    mutable int park_poll_failures_ = 0;  // consecutive failed Park status polls, under mutex_
    // The claim of the GOTO whose landing refinement is under way; empty when none (open-astro#880).
    mutable std::optional<util::MotionClaim> refining_claim_;
    // open-astro#575: an async slew dispatch that fails AFTER
    // slew_to_coordinates_async() returned used to be logged and forgotten,
    // leaving Slewing read FALSE -- indistinguishable from a landed goto. Set
    // (under mutex_) by the slew task's catch block on a REAL failure (never
    // on the task's own cancellation), cleared by the next slew initiator and
    // by AbortSlew. Consulted by get_slewing() before the cached bool.
    mutable std::string last_slew_error_;
    // under mutex_; Slewing's hold from a GOTO's start: 8 s, or kRefineStartForce for a landing re-GOTO (decision 0022)
    mutable util::HoldWindow slew_hold_;
    mutable util::TaskClock::clock::time_point position_override_until_;
    mutable bool manual_axis_slewing_[2] = {false, false};
    std::string mount_firmware_version_;
    // Web-UI firmware copy, guarded by its own narrow mutex (not mutex_) so the
    // get_device_firmware() poll never blocks on the coarse connect lock.
    mutable std::mutex firmware_mutex_;
    std::string firmware_cache_;
    // Read by get_name() on the HTTP thread with no driver lock while the
    // async connect sequence writes it under mutex_; whole-value atomic so
    // that read is not a data race (PR #281 review).
    std::atomic<int> mount_model_id_;
    bool use_precise_commands_;
    int slew_settle_time_seconds_ = 0;

    std::optional<double> pending_site_latitude_;
    std::optional<double> pending_site_longitude_;
    std::optional<double> pending_site_elevation_;
    const bool sync_time_on_connect_;
    // open-astro#2419: the host clock re-sync. A change on_host_clock_changed() has not acted on yet, and what it was.
    std::atomic<bool> resync_pending_{false};
    std::atomic<bool> resync_stepped_{false};
    std::atomic<int64_t> resync_step_ms_{0};
    // Bumped by each successful client UTCDate write (under config_write_mutex_ and mutex_); a re-sync decided at an
    // older value writes nothing.
    std::atomic<uint64_t> utc_write_generation_{0};
    std::atomic<int> clock_resync_starts_{0};           // test seam, see clock_resync_starts_for_testing()
    std::mutex clock_resync_hook_mutex_;                // a leaf
    std::function<void(bool)> clock_resync_done_hook_;  // test seam, see set_clock_resync_done_hook_for_testing()
    std::function<void()> connect_start_hook_;          // test seam, see set_connect_start_hook_for_testing()
    const SynScanAlignmentSetting alignment_setting_;  // AZ-EQ geometry from the device config (#860)
    GuideRate guide_rate_{};
    // IsPulseGuiding per axis (0 RA, 1 Dec): the pulse plus the completion delay (decision 0022).
    std::array<util::HoldWindow, 2> pulse_hold_{};
    // Set by a pulse body that a canceller cut short (its blind stop ran, no tracking restore): that axis's window is
    // still armed, for the canceller only (see CutShortPulseRelease). Cleared with the window by every clear and by a
    // MoveAxis whose rate command went out. A superseded body never sets it (#2263).
    std::array<bool, 2> pulse_cut_short_{};

    bool park_position_set_ = false;
    mutable bool parking_ = false;  // park task in flight (Slewing true, AtPark false)
    AlignmentMode park_alignment_mode_ = AlignmentMode::GermanPolar;
    bool park_ra_uses_hour_angle_ = false;
    double park_hour_angle_hours_ = 0.0;
    // #2263: who owns each axis, under mutex_. Every slew and Park claims both axes and a moving MoveAxis its own
    // (claim_motion_locked()); every stop cancels its axes (cancel_motion_locked()); a sync slew, the landing and a
    // stop's wire turn check their claim (claim_lost_locked(), #832). The pulses are not claims: each axis's pulse
    // slot has a generation of its own (supersede_pulse_locked()). SynScan's PulseGuide gate asks the handset and the
    // driver's slewing state (open-astro#775) and never the owner, so the driver claims no MotionCommand::PulseGuide,
    // reads no slew_running() and calls no finished().
    util::MotionOwner motion_owner_;
    // Bumped by every claim and stop above, under mutex_: the host clock re-sync's `L` (open-astro#2419) applies
    // nothing once it moved (clock_resync_handset_slewing_locked()).
    uint64_t motion_epoch_ = 0;
    std::condition_variable motion_cv_;  // signalled by every claim and stop; ends a synchronous landing nudge
    // The ordered wire turn (see WireTurn). A leaf mutex: held only to move the two counters or to wait on the cv.
    std::mutex turn_mutex_;
    std::condition_variable turn_cv_;
    std::mutex turn_hook_mutex_;
    std::function<void()> turn_wait_hook_;  // test seam, see set_wire_turn_wait_hook_for_testing()
    std::function<void()> closing_hook_;    // test seam, see set_closing_hook_for_testing()
    uint64_t turn_next_ = 0;     // next ticket to hand out
    uint64_t turn_serving_ = 0;  // ticket whose sender may send now
    double park_ra_hours_ = 0.0;
    double park_dec_degrees_ = 0.0;
    double park_azimuth_degrees_ = 0.0;
    double park_altitude_degrees_ = 0.0;

    // Serializes motion handoffs: sync/async slews, park, PulseGuide, MoveAxis,
    // AbortSlew, Unpark, and disconnect. Never taken under mutex_; a blocking
    // slew releases it before waiting for motion completion.
    std::mutex initiator_mutex_;
    // Serialises set_connected() calls; no getter takes it. First in the lock order.
    std::mutex transition_mutex_;
    // Serialises SiteLatitude, SiteLongitude and UTCDate writes, each across its read of the other value, its wire
    // write and its publish, so two concurrent writes cannot lose one. After initiator_mutex_, before the turn: such a
    // write never waits for a motion initiator. The connect does not take it (a public setter refuses while
    // connected_ is false).
    std::mutex config_write_mutex_;
    // Wall time behind every local-sidereal-time read (park and SetPark target, DestinationSideOfPier,
    // SiderealTime). Only a test replaces it, so hardware keeps std::chrono::system_clock. Own mutex: the
    // reads run on client calls (some under mutex_), the test setter on another thread.
    std::chrono::system_clock::time_point sidereal_wall_now() const {
        std::function<std::chrono::system_clock::time_point()> now;
        {
            std::lock_guard<std::mutex> lock(sidereal_wall_clock_mutex_);
            now = sidereal_wall_clock_;
        }
        return now ? now() : std::chrono::system_clock::now();
    }
    mutable std::mutex sidereal_wall_clock_mutex_;
    std::function<std::chrono::system_clock::time_point()> sidereal_wall_clock_;
    // Declared last, initialised last; must outlive the driver (decision 0005).
    util::TaskClock& clock_;
    // Declared after clock_ (the slots take it) and last, so they are destroyed
    // (and join) before any member their bodies touch. The slew slot carries
    // slew, Park and the refinement passes; each axis has its own pulse slot and
    // generation, so a pulse on one axis never supersedes the other's. A
    // replacement pulse or a moving MoveAxis bumps its own axis's generation,
    // a slew or Park both (supersede_pulse_locked()).
    util::OperationGeneration slew_generation_;
    util::AsyncOperation slew_{"SynScan slew", slew_generation_, clock_};
    util::OperationGeneration poll_generation_;
    util::OperationGeneration pulse_generation_[2];
    util::AsyncOperation pulse_ops_[2] = {{"SynScan pulse guide (RA)", pulse_generation_[0], clock_},
                                          {"SynScan pulse guide (Dec)", pulse_generation_[1], clock_}};
    // Last: destroyed first, so its poll body joins while every member the probe touches is alive.
    mutable util::PolledState<PolledFrame> polled_{
        clock_,
        poll_generation_,
        {kStatusPollPeriod, kPositionLinkFailureThreshold, kSnapshotMaxAge, "SynScan mount"},
        [this] { return probe(); },
        [] { SynScanProtocolWrapper::instance().disconnect(); }};
    // The host clock re-sync body (open-astro#2419). Started under transition_mutex_ (on_host_clock_changed(),
    // set_connected()'s exit) and reaped under it (disconnect, relink, the destructor), so its reapers never overlap
    // (docs/failures/0011). Declared after polled_, so it joins first: its body reads the poll's loss latch.
    util::OperationGeneration clock_resync_generation_;
    util::AsyncOperation clock_resync_{"SynScan clock re-sync", clock_resync_generation_, clock_};
};

std::unique_ptr<TelescopeDriver> create_synscan_telescope(
    int device_number,
    const ConnectionInfo& connection_info,
    SynScanVersion version) {
    return create_synscan_telescope_with_site(device_number, connection_info, version, std::nullopt, std::nullopt,
                                              std::nullopt, std::nullopt);
}

std::unique_ptr<TelescopeDriver> create_synscan_telescope_with_site(
    int device_number, const ConnectionInfo& connection_info, SynScanVersion version,
    std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
    std::optional<double> site_elevation_m, std::optional<bool> sync_time_on_connect, SynScanAlignmentSetting alignment,
    util::TaskClock& clock) {
    return std::make_unique<SynScanTelescopeDriver>(device_number, connection_info, version, site_latitude_deg,
                                                    site_longitude_deg, site_elevation_m, sync_time_on_connect,
                                                    alignment, util::ConnectionResolver<ConnectionInfo>{}, clock);
}

void set_slew_spawn_for_testing(TelescopeDriver& driver, std::function<std::thread(std::function<void()>)> spawn) {
    if (auto* synscan = dynamic_cast<SynScanTelescopeDriver*>(&driver)) {
        synscan->set_slew_spawn_for_testing(std::move(spawn));
    }
}

void set_pulse_spawn_for_testing(TelescopeDriver& driver, std::function<std::thread(std::function<void()>)> spawn) {
    if (auto* synscan = dynamic_cast<SynScanTelescopeDriver*>(&driver)) {
        synscan->set_pulse_spawn_for_testing(std::move(spawn));
    }
}

void set_status_poll_paused_for_testing(TelescopeDriver& driver, bool paused) {
    if (auto* synscan = dynamic_cast<SynScanTelescopeDriver*>(&driver)) {
        synscan->set_status_poll_paused_for_testing(paused);
    }
}

void set_status_poll_after_sample_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* synscan = dynamic_cast<SynScanTelescopeDriver*>(&driver)) {
        synscan->set_status_poll_after_sample_hook_for_testing(std::move(hook));
    }
}

void set_wire_turn_wait_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* synscan = dynamic_cast<SynScanTelescopeDriver*>(&driver)) {
        synscan->set_wire_turn_wait_hook_for_testing(std::move(hook));
    }
}

void set_clock_resync_done_hook_for_testing(TelescopeDriver& driver, std::function<void(bool wrote)> hook) {
    if (auto* synscan = dynamic_cast<SynScanTelescopeDriver*>(&driver)) {
        synscan->set_clock_resync_done_hook_for_testing(std::move(hook));
    }
}

int clock_resync_starts_for_testing(const TelescopeDriver& driver) {
    if (const auto* synscan = dynamic_cast<const SynScanTelescopeDriver*>(&driver)) {
        return synscan->clock_resync_starts_for_testing();
    }
    return 0;
}

void set_connect_start_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* synscan = dynamic_cast<SynScanTelescopeDriver*>(&driver)) {
        synscan->set_connect_start_hook_for_testing(std::move(hook));
    }
}

void set_closing_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* synscan = dynamic_cast<SynScanTelescopeDriver*>(&driver)) {
        synscan->set_closing_hook_for_testing(std::move(hook));
    }
}

void poll_status_now_for_testing(TelescopeDriver& driver) {
    if (auto* synscan = dynamic_cast<SynScanTelescopeDriver*>(&driver)) {
        synscan->poll_status_now_for_testing();
    }
}

void set_sidereal_wall_clock_for_testing(TelescopeDriver& driver,
                                         std::function<std::chrono::system_clock::time_point()> now) {
    if (auto* synscan = dynamic_cast<SynScanTelescopeDriver*>(&driver)) {
        synscan->set_sidereal_wall_clock_for_testing(std::move(now));
    }
}

std::unique_ptr<TelescopeDriver> create_synscan_telescope_deferred(
    int device_number, util::ConnectionResolver<ConnectionInfo> connection_resolver, SynScanVersion version,
    std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
    std::optional<double> site_elevation_m, std::optional<bool> sync_time_on_connect, SynScanAlignmentSetting alignment,
    util::TaskClock& clock) {
    if (!connection_resolver) {
        throw AlpacaException("SynScan telescope: a connection resolver is required", AlpacaError::InvalidValue);
    }
    return std::make_unique<SynScanTelescopeDriver>(device_number, ConnectionInfo{}, version, site_latitude_deg,
                                                    site_longitude_deg, site_elevation_m, sync_time_on_connect,
                                                    alignment, std::move(connection_resolver), clock);
}

ConnectionInfo resolve_synscan_serial_auto(int mount_index) {
    auto ports = enumerate_synscan_ports();
    if (ports.empty()) {
        throw AlpacaException(util::serial_auto_detect_failed_message("SynScan mount"));
    }
    if (mount_index < 0 || mount_index >= static_cast<int>(ports.size())) {
        throw AlpacaException("Mount index " + std::to_string(mount_index) +
                              " out of range (found " + std::to_string(ports.size()) + " mount(s))");
    }

    const auto& port = ports[static_cast<std::size_t>(mount_index)];
    ALPACA_LOG_INFO("SynScan", "Auto-detected mount on " + port.port_path +
                    " (HC fw " + port.firmware_version + ")");

    ConnectionInfo conn;
    conn.type = ConnectionType::Serial;
    conn.port_path = port.port_path;
    return conn;
}

std::unique_ptr<TelescopeDriver> create_synscan_telescope_auto(
    int device_number, int mount_index, SynScanVersion version, std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg, std::optional<double> site_elevation_m,
    std::optional<bool> sync_time_on_connect, SynScanAlignmentSetting alignment, util::TaskClock& clock) {
    // The serial scan runs at connect time (#659), not here.
    return create_synscan_telescope_deferred(
        device_number, [mount_index] { return resolve_synscan_serial_auto(mount_index); }, version, site_latitude_deg,
        site_longitude_deg, site_elevation_m, sync_time_on_connect, alignment, clock);
}

} // namespace alpacacore::vendor::synscan
