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
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/park_slew.h>
#include <alpacacore/util/state_snapshot.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/synscan/synscan_protocol_wrapper.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <array>
#include <atomic>
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

double wrap_degrees(double deg) {
    double wrapped = std::fmod(deg, 360.0);
    if (wrapped < 0.0) {
        wrapped += 360.0;
    }
    return wrapped;
}

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
    double wrapped = wrap_degrees(degrees);
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

uint32_t encode_ra_raw(double hours, int bits) {
    double wrapped_hours = std::fmod(hours, 24.0);
    if (wrapped_hours < 0.0) {
        wrapped_hours += 24.0;
    }
    return encode_angle(wrapped_hours * kHoursToDegrees, bits);
}

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
    local_tm = *std::localtime(&base_time);
    utc_tm = *std::gmtime(&base_time);
#endif
    std::time_t local_time = std::mktime(&local_tm);
    std::time_t utc_as_local = std::mktime(&utc_tm);
    double offset_seconds = std::difftime(local_time, utc_as_local);
    info.offset_minutes = static_cast<int>(std::round(offset_seconds / 60.0));
    info.dst = local_tm.tm_isdst > 0;
    return info;
}

// SynScan V3/V4 hand controller model IDs (from "m" command).
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
            return "Mount (ID " + std::to_string(model_id) + ")";
    }
}

} // namespace

double park_ra_from_hour_angle(double lst_hours, double hour_angle_hours) {
    double ra = std::fmod(lst_hours - hour_angle_hours, 24.0);
    if (ra < 0.0) {
        ra += 24.0;
    }
    // A tiny negative remainder plus 24.0 rounds to exactly 24.0.
    if (ra >= 24.0) {
        ra -= 24.0;
    }
    return ra;
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
          aperture_area_m2_(0.0),
          focal_length_m_(0.0),
          site_latitude_cached_(0.0),
          site_longitude_cached_(0.0),
          site_info_valid_(false),
          site_elevation_m_(site_elevation_m),
          timezone_offset_minutes_(0),
          timezone_offset_valid_(false),
          dst_observed_(false),
          last_utc_set_{},
          // Pointing time stays on the host's steady clock, not the task clock (decision 0005).
          last_utc_set_monotonic_(std::chrono::steady_clock::now()),
          last_utc_valid_(false),
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
        // mutex_ held (the task threads take mutex_).
        stop_status_poller();
        cancel_async_tasks();
        if (connected_) {
            try {
                set_connected(false);
            } catch (...) {
            }
        }
    }

    int get_device_number() const override {
        return device_number_;
    }

    // The "(SynScan)" suffix disambiguates from the skywatcher direct
    // driver's get_name(), which resolves to the identical model string for a
    // mount reachable over both connections (see the sibling comment there).
    // "SynScan" / "EQMOD" are the terms both ecosystems already use for the
    // two paths (INDI labels: "SynScan", "EQMod Mount"; ASCOM: "SynScan App
    // Driver", "EQMOD"), so keep them rather than "hand controller"/"direct".
    std::string get_name() const override {
        const int model_id = mount_model_id_.load();
        if (model_id >= 0) {
            return "Sky-Watcher " + synscan_model_id_to_name(model_id) + " (SynScan)";
        }
        return "Sky-Watcher Mount (SynScan)";
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
    // by its OWN narrow firmware_mutex_, NOT the coarse mutex_ that set_connected()
    // holds across the multi-second connect, so a configureddevices poll never
    // blocks on the mount connection.
    std::optional<std::string> get_device_firmware() const override {
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        if (firmware_cache_.empty()) {
            return std::nullopt;
        }
        return firmware_cache_;
    }

    int get_interface_version() const override { return 4; }

    // Lock-free on purpose: set_connected(true) holds mutex_ for the whole
    // handset handshake (echo, firmware, model, site, time, position warm-up,
    // each a serial round trip with its own response timeout), and the router
    // polls this getter from the PUT connected wait and from every GET
    // connected in between. Taking mutex_ here made those calls block for the
    // entire connect (25 s measured against a silent hand controller: five
    // 5 s timeouts), so the router's connect wait limit never fired and clients
    // reported "Dynamic client timeout for method Connected" (issue #130).
    // Same atomic-flag pattern as every other AsyncConnectable driver except
    // the telescopes and the wrapper-backed switches named in
    // async_connectable.h. Stated as the rule rather than as a count: a bare
    // number in a comment has nothing tying it to the code it describes, and
    // every new driver invalidates it silently (open-astro#381).
    bool get_connected() const override { return connected_.load() && !link_lost_.load(); }

    // Reads only the published copy under its own narrow mutex, never the coarse
    // mutex_ that set_connected() holds across the connect, so a configureddevices
    // poll does not wait on the mount connection.
    std::string get_link_fault() const override {
        std::lock_guard<std::mutex> lock(link_fault_mutex_);
        return link_fault_text_;
    }

    void connect() override {
        start_connection_task(true);
    }

    void disconnect() override {
        start_connection_task(false);
    }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        std::unique_lock<std::mutex> ilock(initiator_mutex_, std::defer_lock);
        // Decision 0009 point 4: a lost link is relinked only by the client's Connected=true, which tears the dead
        // session down (slew bodies included) and connects afresh.
        const bool relink = connected && connected_.load() && link_lost_.load();
        if (!connected || relink) {
            ilock.lock();
            // Cancel + join the status poll and the background slew/pulse task
            // threads BEFORE taking mutex_: the task threads take mutex_, so
            // joining under the lock would deadlock, and leaving them running
            // across the protocol disconnect would race the shared wrapper.
            stop_status_poller();
            cancel_async_tasks();
        }
        std::unique_lock<std::mutex> lock(mutex_);
        if (relink) {
            teardown_session_locked();
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
        bump_motion_generation();

        auto& protocol = SynScanProtocolWrapper::instance();
        if (connected) {
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
            link_lost_ = false;
            connected_ = true;
            mount_firmware_version_ = "";
            mount_model_id_ = -1;
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
            refining_generation_ = 0;
            last_slew_error_.clear();
            slew_force_until_ = util::TaskClock::clock::time_point::min();
            position_override_until_ = util::TaskClock::clock::time_point::min();
            invalidate_readback_tick_locked();
            guide_position_valid_ = false;
            last_utc_valid_ = false;
            client_disagreement_warned_ = false;
            snapshot_.reset();
            position_link_health_.reset();
            publish_link_fault_locked();
            last_site_info_attempt_ = util::TaskClock::clock::time_point::min();

            try {
                mount_firmware_version_ = protocol.get_handset_firmware_version();
            } catch (...) {
                // TODO: Confirm SynScan firmware query reliability on all V3/V4 handsets.
            }
            // Publish to the web-UI getter under its OWN narrow mutex so a
            // configureddevices poll never blocks on the coarse mutex_ this
            // connect sequence holds for several seconds.
            {
                std::lock_guard<std::mutex> fwlock(firmware_mutex_);
                firmware_cache_ = (mount_firmware_version_ != "0.0.0") ? mount_firmware_version_ : std::string();
            }
            try {
                mount_model_id_ = protocol.get_model_id();
            } catch (...) {
                // TODO: Confirm SynScan model query reliability on all V3/V4 handsets.
            }

            if (pending_site_latitude_.has_value() && pending_site_longitude_.has_value()) {
                LocationInfo loc;
                loc.latitude_degrees = pending_site_latitude_.value();
                loc.longitude_degrees = pending_site_longitude_.value();
                try {
                    protocol.set_location(loc);
                    site_latitude_cached_ = loc.latitude_degrees;
                    site_longitude_cached_ = loc.longitude_degrees;
                    site_info_valid_ = true;
                } catch (...) {
                    // TODO: Confirm SynScan accepts location updates while aligned.
                }
            }
            if (pending_site_elevation_.has_value()) {
                site_elevation_m_ = pending_site_elevation_.value();
            }
            if (sync_time_on_connect_) {
                sync_mount_time_locked();
            }
            // Seed the snapshot so the first property reads are served from memory, status flags included.
            seed_snapshot_locked();
            ensure_utc_cached_locked();
            try {
                LocationInfo info = protocol.get_location();
                site_latitude_cached_ = info.latitude_degrees;
                site_longitude_cached_ = info.longitude_degrees;
                site_info_valid_ = true;
            } catch (...) {
            }
        } else {
            teardown_session_locked();
        }
        if (connected) {
            lock.unlock();  // the poll slot starts without mutex_ held (AsyncOperation rule 10)
            start_status_poller();
        }
    }

    std::vector<std::string> get_supported_actions() const override {
        return {};
    }

    std::string action(std::string_view action_name, std::string_view action_parameters) override {
        (void)action_parameters;
        throw AlpacaException("Action not supported: " + std::string(action_name), AlpacaError::ActionNotImplemented);
    }

    bool can_action(std::string_view action_name) const override {
        (void)action_name;
        return false;
    }

    std::string command_blind(std::string_view command, bool raw) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        auto& protocol = SynScanProtocolWrapper::instance();
        if (raw) {
            protocol.send_command_blind(std::string(command));
        } else {
            protocol.send_command_blind(std::string(command));
        }
        return "";
    }

    bool command_bool(std::string_view command, bool raw) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        auto& protocol = SynScanProtocolWrapper::instance();
        if (raw) {
            std::string response = protocol.send_command(std::string(command));
            return response == "1";
        }
        protocol.send_command_blind(std::string(command));
        return true;
    }

    std::string command_string(std::string_view command, bool raw) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        auto& protocol = SynScanProtocolWrapper::instance();
        if (raw) {
            return protocol.send_command(std::string(command));
        }
        return protocol.send_command(std::string(command));
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

    double get_aperture_diameter() const override {
        return aperture_diameter_m_;
    }

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

    double get_aperture_area() const override {
        return aperture_area_m2_;
    }

    bool get_at_home() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
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
        const auto now = clock_.now();
        for (std::size_t axis = 0; axis < pulse_guiding_active_.size(); ++axis) {
            if (pulse_guiding_active_[axis] && now >= pulse_guide_end_time_[axis]) {
                pulse_guiding_active_[axis] = false;
            }
        }
        return pulse_guiding_active_[0] || pulse_guiding_active_[1];
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
        if (!position_link_health_.faulted() && guide_position_valid_ && clock_.now() < position_override_until_ &&
            !get_slewing_locked()) {
            return std::clamp(guide_position_dec_degrees_, -90.0, 90.0);
        }
        return std::clamp(served_equatorial_locked().second, -90.0, 90.0);
    }

    double get_declination_rate() const override {
        check_connected();
        return 0.0;
    }

    void set_declination_rate(double rate) override {
        (void)rate;
        throw AlpacaException("Declination rate not supported", AlpacaError::PropertyNotImplemented);
    }

    bool get_tracking() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        const int mode = served_frame_locked().value.tracking_mode;
        if (mode < 0) {
            throw AlpacaException("SynScan tracking state is not known yet: the handset has not answered a status poll",
                                  AlpacaError::DriverException);
        }
        return mode != 0;
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
                ensure_site_info_cached_locked();
            }
            if (alignment != AlignmentMode::AltAz && !site_info_valid_) {
                throw AlpacaException("Set SiteLatitude before enabling equatorial tracking", AlpacaError::ValueNotSet);
            }
            // SynScan "T" modes (protocol v3.3): 1 = Alt/Az, 2 = equatorial in either hemisphere, 3 = PEC. The
            // handset applies its own site; 3 is not "EQ south" as it is on a Celestron.
            mode = alignment == AlignmentMode::AltAz ? 1 : 2;
        }
        // The cache is written after the send: a motion owner ahead in the queue restores the mode it found.
        if (!send_in_turn(
                lock, [this] { return connected_ && !link_lost_.load(); },
                [&protocol, mode] { protocol.set_tracking_mode(mode); })) {
            check_connected();
            return;
        }
        set_tracking_mode_cached_locked(mode);
        tracking_mode_valid_ = true;
        invalidate_readback_tick_locked();
    }

    double get_focal_length() const override {
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
        return guide_rate_;
    }

    void set_guide_rate(const GuideRate& rate) override {
        if (!std::isfinite(rate.ra) || !std::isfinite(rate.dec)) {
            throw AlpacaException("GuideRate must be a finite number", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        double ra_percent = (rate.ra / kSiderealDegPerSec) * 100.0;
        double dec_percent = (rate.dec / kSiderealDegPerSec) * 100.0;
        if (ra_percent < 0.0 || ra_percent > 100.0 || dec_percent < 0.0 || dec_percent > 100.0) {
            throw AlpacaException("Guide rate must be between 0 and 1x sidereal",
                                  AlpacaError::InvalidValue);
        }
        guide_rate_ = rate;
    }

    double get_right_ascension() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (!position_link_health_.faulted() && guide_position_valid_ && clock_.now() < position_override_until_ &&
            !get_slewing_locked()) {
            return guide_position_ra_hours_;
        }
        return served_equatorial_locked().first;
    }

    double get_right_ascension_rate() const override {
        check_connected();
        return 0.0;
    }

    void set_right_ascension_rate(double rate) override {
        (void)rate;
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
        throw AlpacaException("Pier side not supported", AlpacaError::PropertyNotImplemented);
    }

    int get_destination_side_of_pier(double ra, double dec) const override {
        validate_ra_dec(ra, dec, "DestinationSideOfPier");
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        if (!site_info_valid_) {
            ensure_site_info_cached_locked();
        }
        if (!site_info_valid_) {
            return served_frame_locked().value.side_of_pier;  // the same link checks as every other frame read
        }
        double lst_hours = compute_local_sidereal_time_hours(sidereal_wall_now(), site_longitude_cached_);
        double hour_angle = shortest_ra_delta_hours(lst_hours, ra);
        return hour_angle >= 0.0 ? 0 : 1;
    }

    EquatorialSystem get_equatorial_system() const override {
        return EquatorialSystem::J2000;
    }

    // The driver applies no refraction correction, so it neither knows nor computes the flag (ITelescopeV4).
    bool get_does_refraction() const override {
        throw AlpacaException("DoesRefraction is not implemented", AlpacaError::PropertyNotImplemented);
    }

    void set_does_refraction(bool) override {
        throw AlpacaException("DoesRefraction is not implemented", AlpacaError::PropertyNotImplemented);
    }

    int get_slew_settle_time() const override {
        return slew_settle_time_seconds_;
    }

    void set_slew_settle_time(int seconds) override {
        if (seconds < 0) {
            throw AlpacaException("Slew settle time must be >= 0 seconds", AlpacaError::InvalidValue);
        }
        slew_settle_time_seconds_ = seconds;
    }

    double get_sidereal_time() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        // No handset read here: the poll retries an unread site (decision 0010), so DeviceState costs no transaction.
        return compute_local_sidereal_time_hours(sidereal_wall_now(), site_longitude_cached_);
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
        require_site_known_locked();
        return site_latitude_cached_;
    }

    void set_site_latitude(double latitude) override {
        if (!std::isfinite(latitude) || latitude < -90.0 || latitude > 90.0) {
            throw AlpacaException("SiteLatitude must be in range -90 to 90 degrees",
                                  AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        LocationInfo info = current_location_locked();
        info.latitude_degrees = latitude;
        SynScanProtocolWrapper::instance().set_location(info);
        site_latitude_cached_ = latitude;
        site_longitude_cached_ = info.longitude_degrees;
        site_info_valid_ = true;
    }

    double get_site_longitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_site_known_locked();
        return site_longitude_cached_;
    }

    void set_site_longitude(double longitude) override {
        if (!std::isfinite(longitude) || longitude < -180.0 || longitude > 180.0) {
            throw AlpacaException("SiteLongitude must be in range -180 to 180 degrees",
                                  AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        LocationInfo info = current_location_locked();
        info.longitude_degrees = longitude;
        SynScanProtocolWrapper::instance().set_location(info);
        site_latitude_cached_ = info.latitude_degrees;
        site_longitude_cached_ = longitude;
        site_info_valid_ = true;
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

    double get_target_declination() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!target_dec_set_) {
            throw AlpacaException("Target declination has not been set", AlpacaError::ValueNotSet);
        }
        return target_dec_degrees_;
    }

    void set_target_declination(double dec) override {
        if (!std::isfinite(dec) || dec < -90.0 || dec > 90.0) {
            throw AlpacaException("TargetDeclination must be in range -90 to 90 degrees",
                                  AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        target_dec_degrees_ = dec;
        target_dec_set_ = true;
    }

    double get_target_right_ascension() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!target_ra_set_) {
            throw AlpacaException("Target right ascension has not been set", AlpacaError::ValueNotSet);
        }
        return target_ra_hours_;
    }

    void set_target_right_ascension(double ra) override {
        if (!std::isfinite(ra) || ra < 0.0 || ra >= 24.0) {
            throw AlpacaException("TargetRightAscension must be in range 0 to <24 hours",
                                  AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        target_ra_hours_ = ra;
        target_ra_set_ = true;
    }

    int get_tracking_rate() const override {
        return 0;
    }

    void set_tracking_rate(int rate) override {
        (void)rate;
        throw AlpacaException("Tracking rates not supported", AlpacaError::PropertyNotImplemented);
    }

    std::vector<int> get_tracking_rates() const override {
        return {0};
    }

    std::chrono::system_clock::time_point get_utc_date() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        ensure_utc_cached_locked();
        return current_utc_time_locked();
    }

    void set_utc_date(std::chrono::system_clock::time_point utc) override {
        std::lock_guard<std::mutex> lock(mutex_);
        set_utc_date_locked(utc);
    }

private:
    // Body of set_utc_date with mutex_ already held — called from the connect
    // path (sync_mount_time_locked), which holds mutex_; calling the public
    // locking method there would self-deadlock on the non-recursive mutex.
    void set_utc_date_locked(std::chrono::system_clock::time_point utc) {
        check_connected();
        std::time_t utc_time_t = std::chrono::system_clock::to_time_t(utc);
        LocalTimeInfo tz_info = compute_local_timezone_info(utc_time_t);

        std::tm local_tm {};
#ifdef _WIN32
        localtime_s(&local_tm, &utc_time_t);
#else
        local_tm = *std::localtime(&utc_time_t);
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
        SynScanProtocolWrapper::instance().set_time(info);
        invalidate_readback_tick_locked();
        timezone_offset_minutes_ = tz_info.offset_minutes;
        timezone_offset_valid_ = true;
        dst_observed_ = tz_info.dst;
        last_utc_set_ = utc;
        last_utc_set_monotonic_ = std::chrono::steady_clock::now();
        last_utc_valid_ = true;
        // The mount now runs on the client's clock and so does the cached
        // pointing time; on a disciplined host say so once (open-astro#409).
        alpacacore::util::ClientUtcWarning::warn_once("SynScan", client_minus_host, client_disagreement_warned_);
    }

public:
    void find_home() override {
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
        // A pulse stop body takes mutex_, so its join runs without it. The slew body is not joined: the claim
        // below supersedes it.
        join_pulse_bodies();
        bool displaced = false;
        bool prev_slewing = false;
        bool prev_at_home = false;
        bool prev_guide_valid = false;
        bool prev_manual[2] = {false, false};
        auto prev_override = util::TaskClock::clock::time_point::min();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            if (parked_ || parking_) {
                return;  // ASCOM: Park on a parked (or parking) mount is harmless.
            }
            if (!park_position_set_) {
                store_park_position_locked();
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
                    ensure_site_info_cached_locked();
                }
                // An hour angle is saved only while a site is known, so a
                // handset that has since stopped reporting one (a reconnect
                // without location) still leaves the last known longitude here.
                const double lst = compute_local_sidereal_time_hours(sidereal_wall_now(), site_longitude_cached_);
                park_target_first = park_ra_from_hour_angle(lst, park_hour_angle_hours_);
                park_target_second = park_dec_degrees_;
                validate_ra_dec(park_target_first, park_target_second, "Park");
            }
            // Nothing below throws: claim only now, so a refused Park leaves the body in flight alone.
            displaced = claim_slew_slot_locked();
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
            slew_force_until_ = clock_.now() + std::chrono::seconds(8);
            position_override_until_ = util::TaskClock::clock::time_point::min();
            guide_position_valid_ = false;
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            at_home_ = false;
            parking_ = true;
            bump_motion_generation();
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
                        do_slew_to_coordinates_locked(park_target_first, park_target_second, lock,
                                                      [&ctx] { return !slot_stopped(&ctx); });
                    }
                };
                hooks.poll_slewing = [&] {
                    return poll_hardware_slewing_locked(true, false, &lock, [&ctx] { return slot_stopped(&ctx); });
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
                    slew_force_until_ = util::TaskClock::clock::time_point::min();
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
                slew_force_until_ = util::TaskClock::clock::time_point::min();
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
        slew_force_until_ = util::TaskClock::clock::time_point::min();
        position_override_until_ = util::TaskClock::clock::time_point::min();
        ALPACA_LOG_WARN("SynScan", message);
    }

    void pulse_guide(int direction, int duration) override {
        if (duration < 0) {
            throw AlpacaException("PulseGuide duration must be >= 0", AlpacaError::InvalidValue);
        }
        int axis = -1;
        double direction_sign = 0.0;
        switch (direction) {
            case 0:
                axis = 1;
                direction_sign = 1.0;
                break;
            case 1:
                axis = 1;
                direction_sign = -1.0;
                break;
            case 2:
                axis = 0;
                direction_sign = -1.0;
                break;
            case 3:
                axis = 0;
                direction_sign = 1.0;
                break;
            default:
                throw AlpacaException("Invalid PulseGuide direction", AlpacaError::InvalidValue);
        }

        std::lock_guard<std::mutex> ilock(initiator_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("PulseGuide");
            // open-astro#775: ITelescopeV4 raises InvalidOperation for a
            // PulseGuide while a slew is in progress.
            if (get_slewing_locked(true)) {
                throw AlpacaException("PulseGuide is not allowed while the mount is slewing",
                                      AlpacaError::InvalidOperation);
            }
        }
        // The pulse body takes mutex_ (after an RA stop, to restore tracking, and
        // on failure), so the join must run without it.
        pulse_ops_[axis].cancel_all_and_join();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("PulseGuide");
            auto& protocol = SynScanProtocolWrapper::instance();
            if (axis == 0) {
                refresh_tracking_mode_locked();  // the pulse end restores this mode
            }
            const double guide_rate = axis == 1 ? guide_rate_.dec : guide_rate_.ra;
            // RA: West (direction_sign +1) lowers RA, East raises it.
            double slew_rate_deg_per_sec =
                axis == 0 ? ra_motor_rate_locked(-direction_sign * guide_rate * 3600.0) : direction_sign * guide_rate;
            if (axis == 1) {
                char pier = protocol.get_pointing_state();
                if (pier == 'W') {
                    slew_rate_deg_per_sec = -slew_rate_deg_per_sec;
                }
            }

            const double duration_sec = duration / 1000.0;
            const auto now = clock_.now();

            // Initialize the guide-position estimate from mount position when its override expires.
            if (!guide_position_valid_ || now >= position_override_until_) {
                const auto equatorial = served_equatorial_locked();
                guide_position_ra_hours_ = equatorial.first;
                guide_position_dec_degrees_ = equatorial.second;
                guide_position_valid_ = true;
            }

            // Accumulate expected pulse delta in the private guide-position estimate.
            if (direction == 0 || direction == 1) {
                double delta_deg = guide_rate_.dec * duration_sec;
                if (direction == 1) delta_deg = -delta_deg;
                guide_position_dec_degrees_ = std::clamp(guide_position_dec_degrees_ + delta_deg, -90.0, 90.0);
            } else {
                double delta_hours = (guide_rate_.ra * duration_sec) / 15.0;
                if (direction == 3) delta_hours = -delta_hours;
                guide_position_ra_hours_ = std::fmod(guide_position_ra_hours_ + delta_hours, 24.0);
                if (guide_position_ra_hours_ < 0.0) guide_position_ra_hours_ += 24.0;
            }

            // Keep position override active so get_ra/get_dec return the estimate. The estimate is
            // shared by both axes, so a pulse never shortens a hold another axis still needs (#990).
            position_override_until_ =
                std::max(position_override_until_, now + std::chrono::milliseconds(duration) +
                                                       kPulseGuideCompletionDelay + kPulseGuidePositionGrace);

            protocol.move_axis_variable_rate(axis, slew_rate_deg_per_sec);

            pulse_guiding_active_[static_cast<std::size_t>(axis)] = true;
            pulse_guide_end_time_[static_cast<std::size_t>(axis)] =
                now + std::chrono::milliseconds(duration) + kPulseGuideCompletionDelay;
            drop_poll_in_flight_locked();
        }

        // Each axis has its own slot; cancelling above happened before the new
        // rate command, so the old timer cannot stop this replacement pulse.
        try {
            pulse_ops_[axis].start(
                [this, axis, duration](util::OperationContext& ctx) { run_pulse_stop_body(ctx, axis, duration); });
        } catch (...) {
            // No timer will stop the axis the rate command just started: stop it
            // and drop the pulse state before the refusal reaches the client.
            std::lock_guard<std::mutex> lock(mutex_);
            const auto axis_index = static_cast<std::size_t>(axis);
            pulse_guiding_active_[axis_index] = false;
            pulse_guide_end_time_[axis_index] = util::TaskClock::clock::time_point::min();
            position_override_until_ = util::TaskClock::clock::time_point::min();
            if (connected_) {
                try {
                    SynScanProtocolWrapper::instance().move_axis_variable_rate(axis, 0.0);
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
                // Cancelled by replacement, AbortSlew, disconnect or
                // destruction. Still make a best-effort attempt to stop the
                // axis before exiting — the mount would otherwise keep slewing.
                // AbortSlew restores tracking itself; teardown must not.
                try {
                    SynScanProtocolWrapper::instance().move_axis_variable_rate(axis, 0.0);
                } catch (...) {  // NOLINT(bugprone-empty-catch)
                    // Cancellation path (disconnect/destruction) — the caller
                    // is tearing the connection down; nothing more to do.
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
            // command alone ends it (see ra_motor_rate_locked). Read now: tracking may have changed mid-pulse.
            // Without the equatorial model the pulse ran the guide rate alone, so it stops and then restores
            // tracking as before.
            int end_tracking_mode = 0;
            bool end_with_tracking = false;
            if (axis == 0) {
                std::lock_guard<std::mutex> lock(mutex_);
                end_tracking_mode = connected_ ? tracking_mode_cached_ : 0;
                end_with_tracking = end_tracking_mode > 0 && equatorial_geometry_locked();
            }
            for (int attempt = 0; attempt < kStopAttempts && !stopped; ++attempt) {
                try {
                    if (end_with_tracking) {
                        proto.set_tracking_mode(end_tracking_mode);
                    } else {
                        proto.move_axis_variable_rate(axis, 0.0);
                        if (end_tracking_mode > 0) {
                            proto.set_tracking_mode(end_tracking_mode);
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
            if (stopped) {
                if (axis == 0) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!connected_) {
                        return;
                    }
                    if (end_tracking_mode <= 0) {
                        return;
                    }
                    // Written through, as AbortSlew's restore: a poll sampled mid-pulse read tracking off.
                    set_tracking_mode_cached_locked(end_tracking_mode);
                    // A same-mode T leaves the handset's readback regime as it is (measured); the pulse moved RA
                    // by the estimate the override already reports.
                    if (guide_position_valid_) {
                        readback_top_hours_ = guide_position_ra_hours_;
                    } else {
                        invalidate_readback_top_locked();
                    }
                }
            } else {
                ALPACA_LOG_ERROR("SynScan", "PulseGuide STOP FAILED after " + std::to_string(kStopAttempts) +
                                                " attempts on axis " + std::to_string(axis) +
                                                " — axis may still be moving (mount runaway risk): " + last_error);
                std::lock_guard<std::mutex> lock(mutex_);
                pulse_guiding_active_[axis_index] = false;
                // Surface the error state: report the axis as slewing until an
                // AbortSlew/MoveAxis(0) or disconnect clears it.
                if (connected_) {
                    manual_axis_slewing_[axis] = true;
                }
            }
        }
    }

    void set_park() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        store_park_position_locked();
    }

    void slew_to_coordinates(double ra, double dec) override {
        std::unique_lock<std::mutex> ilock(initiator_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("SlewToCoordinates");
            validate_ra_dec(ra, dec, "SlewToCoordinates");
            // open-astro#775: same Tracking precondition as the async forms.
            refresh_tracking_mode_locked();
            if (!get_tracking_locked()) {
                throw AlpacaException("SlewToCoordinates requires Tracking to be true", AlpacaError::InvalidOperation);
            }
        }
        join_pulse_bodies();
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        clear_pulse_guiding_locked();
        check_not_parked_locked("SlewToCoordinates");
        claim_slew_slot_locked();
        parking_ = false;  // a park body in flight no longer clears it: the claim ended it
        const uint64_t owner_generation = bump_motion_generation();
        do_slew_to_coordinates_locked(ra, dec, lock,
                                      [&] { return motion_generation_ == owner_generation && connected_; });
        // Slewing stays true from the first GOTO to the end of the last refinement pass (open-astro#880).
        const RefineScope refine_scope(*this, owner_generation, !use_precise_commands_);
        ilock.unlock();
        if (!wait_for_slew_complete(lock, owner_generation)) {
            throw AlpacaException("Slew superseded by a concurrent motion command", AlpacaError::InvalidOperation);
        }
        if (!refine_goto_landing_locked(lock, ra, dec, owner_generation, nullptr) ||
            !settle_after_slew_locked(lock, owner_generation)) {
            throw AlpacaException("Slew superseded by a concurrent motion command", AlpacaError::InvalidOperation);
        }
    }

    void slew_to_coordinates_async(double ra, double dec) override {
        std::lock_guard<std::mutex> ilock(initiator_mutex_);  // see park()
        {
            // Gate BEFORE claiming: the claim ends a park in flight (clearing
            // parking_), so a slew this gate refuses must not have claimed.
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("SlewToCoordinatesAsync");
            // Argument validation precedes the state check (ASCOM contract).
            validate_ra_dec(ra, dec, "SlewToCoordinatesAsync");
            // open-astro#775: ITelescopeV4 raises InvalidOperation when a slew
            // is requested with Tracking false.
            refresh_tracking_mode_locked();
            if (!get_tracking_locked()) {
                throw AlpacaException("SlewToCoordinatesAsync requires Tracking to be true",
                                      AlpacaError::InvalidOperation);
            }
        }
        uint32_t ra_raw = 0;
        uint32_t dec_raw = 0;
        bool precise = false;
        uint64_t owner_generation = 0;
        // A pulse stop body takes mutex_, so its join runs without it. The slew body is not joined: the claim
        // below supersedes it.
        join_pulse_bodies();
        bool displaced = false;
        bool prev_slewing = false;
        bool prev_at_home = false;
        bool prev_guide_valid = false;
        bool prev_manual[2] = {false, false};
        uint64_t prev_refining = 0;
        auto prev_override = util::TaskClock::clock::time_point::min();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("SlewToCoordinatesAsync");
            validate_ra_dec(ra, dec, "SlewToCoordinatesAsync");
            displaced = claim_slew_slot_locked();
            parking_ = false;  // a park body in flight no longer clears it: the claim ended it
            owner_generation = bump_motion_generation();
            prev_refining = refining_generation_;
            if (use_precise_commands_) {
                refining_generation_ = owner_generation;
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
            slew_force_until_ = clock_.now() + std::chrono::seconds(8);
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
            [this, ra, dec, ra_raw, dec_raw, precise, owner_generation](util::OperationContext& ctx) {
                std::unique_lock<std::mutex> lock(mutex_);
                const RefineScope refine_scope(*this, owner_generation,
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
                    if (precise && wait_for_slew_complete(lock, owner_generation, &ctx)) {
                        refine_goto_landing_locked(lock, ra, dec, owner_generation, &ctx);
                    }
                } catch (const std::exception& ex) {
                    if (ctx.stop_reason() == util::StopReason::Superseded) {
                        // The newer initiator owns Slewing, the force window and the error text.
                        ALPACA_LOG_WARN("SynScan", std::string("Superseded slew dispatch failed: ") + ex.what());
                        return;
                    }
                    set_slewing_locked(false);
                    slew_force_until_ = util::TaskClock::clock::time_point::min();
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
                    slew_force_until_ = util::TaskClock::clock::time_point::min();
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
                slew_force_until_ = util::TaskClock::clock::time_point::min();
                guide_position_valid_ = prev_guide_valid;
                refining_generation_ = prev_refining;
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
            if (!target_ra_set_ || !target_dec_set_) {
                throw AlpacaException("Target coordinates have not been set", AlpacaError::ValueNotSet);
            }
            ra = target_ra_hours_;
            dec = target_dec_degrees_;
        }
        slew_to_coordinates_async(ra, dec);
    }

    void sync_to_coordinates(double ra, double dec) override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("SyncToCoordinates");
        validate_ra_dec(ra, dec, "SyncToCoordinates");
        auto& protocol = SynScanProtocolWrapper::instance();
        int bits = use_precise_commands_ ? 24 : 16;
        uint32_t ra_raw = encode_ra_raw(ra, bits);
        uint32_t dec_raw = encode_angle(dec, bits);
        protocol.sync_ra_dec_raw(ra_raw, dec_raw, use_precise_commands_);
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
        snapshot_.write([ra, dec](PolledFrame& frame) {
            frame.ra_hours = ra;
            frame.dec_degrees = dec;
            frame.equatorial_known = true;
        });
    }

    void sync_to_target() override {
        double ra, dec;
        {
            std::lock_guard<std::mutex> lock(mutex_);
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
                const uint64_t owner_generation = bump_motion_generation();
                // The stop goes out after anything the park body is committed to send, and the body sends nothing
                // after the claim.
                if (!send_in_turn(
                        lock, [&] { return motion_generation_ == owner_generation && connected_; },
                        [&stop_error] { stop_error = stop_park_slew_wire(); })) {
                    check_connected();
                    stop_error = "the stop was not sent";
                }
                if (stop_error.empty()) {
                    set_slewing_locked(false);
                }
                slew_force_until_ = util::TaskClock::clock::time_point::min();
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
        if (axis != 0 && axis != 1) {
            throw AlpacaException("MoveAxis axis must be 0 or 1", AlpacaError::InvalidValue);
        }
        if (!std::isfinite(rate)) {
            throw AlpacaException("MoveAxis rate must be finite", AlpacaError::InvalidValue);
        }
        if (std::abs(rate) > kMaxMoveAxisRateDegPerSec) {
            throw AlpacaException("MoveAxis rate exceeds supported range", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> ilock(initiator_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_parked_locked("MoveAxis");
        }
        pulse_ops_[axis].cancel_all_and_join();
        std::unique_lock<std::mutex> lock(mutex_);
        check_connected();
        check_not_parked_locked("MoveAxis");
        claim_slew_slot_locked();  // supersedes a slew or park body in flight; it is not joined
        parking_ = false;          // a park body no longer clears it: the claim ended it
        const uint64_t owner_generation = bump_motion_generation();

        constexpr double kStopEpsilon = 1e-6;
        const bool moving = std::abs(rate) > kStopEpsilon;
        if (axis == 0) {
            // Before manual_axis_slewing_ changes; the stop restores this mode. A failed read keeps the cache on a
            // stop, which must still be sent; a start has moved nothing yet, so it can throw.
            if (moving) {
                refresh_tracking_mode_locked();
            } else {
                try {
                    refresh_tracking_mode_locked();
                } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
                }
            }
        }
        manual_axis_slewing_[axis] = moving;
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
        if (!send_in_turn(
                lock,
                [&] {
                    restore_mode = !moving && axis == 0 ? tracking_mode_cached_ : 0;
                    return motion_generation_ == owner_generation && connected_;
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
        if (restore_mode > 0) {
            set_tracking_mode_cached_locked(restore_mode);  // written through, as AbortSlew's restore
        }
        last_slew_error_.clear();
        set_slewing_locked(false);
        slew_force_until_ = util::TaskClock::clock::time_point::min();
        position_override_until_ = util::TaskClock::clock::time_point::min();
        pulse_guiding_active_[static_cast<std::size_t>(axis)] = false;
        pulse_guide_end_time_[static_cast<std::size_t>(axis)] = util::TaskClock::clock::time_point::min();
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
        uint64_t owner_generation = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_connected();
            check_not_fully_parked_locked("AbortSlew");  // AbortSlew may cancel a park in flight
            claim_slew_slot_locked();                    // supersedes the slew or park body; it is not joined
            // Also ends a synchronous slew's landing nudge at once, so the stops below wait for its stop only.
            owner_generation = bump_motion_generation();
        }
        join_pulse_bodies();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            check_connected();
            check_not_fully_parked_locked("AbortSlew");
            // The wrapper serializes transactions, so a GOTO already in flight
            // must finish (or time out) before these stops can reach the mount.
            auto& protocol = SynScanProtocolWrapper::instance();
            // With nothing running, the cache may predate a keypad change: read the handset's mode first. A
            // failed read keeps the cache, since the stops below must still be sent.
            try {
                refresh_tracking_mode_locked();
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
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
                        return motion_generation_ == owner_generation && connected_;
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
            bump_motion_generation();
            set_slewing_locked(false);
            clear_pulse_guiding_locked();
            // open-astro#575: AbortSlew is a valid clearing command for a stored
            // slew failure -- the client acted on the error, so the next Slewing
            // read must answer normally again.
            last_slew_error_.clear();
            slew_force_until_ = util::TaskClock::clock::time_point::min();
            position_override_until_ = util::TaskClock::clock::time_point::min();
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            if (restore_mode > 0) {
                // Stopping an RA guide pulse also stops sidereal tracking.
                // AbortSlew owns this stop, so restore the currently
                // requested mode only after both axes have stopped. The motion
                // state is already clear, so a throwing write cannot leave
                // Slewing true over stopped axes (#830).
                if (restore_error) {
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
    void ensure_utc_cached_locked() const {
        if (!last_utc_valid_) {
            try {
                TimeInfo info = SynScanProtocolWrapper::instance().get_time();
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
                last_utc_set_ = local_time - minutes{total_offset};
            } catch (...) {
                last_utc_set_ = std::chrono::system_clock::now();
            }
            last_utc_set_monotonic_ = std::chrono::steady_clock::now();
            last_utc_valid_ = true;
        }
    }

    // The disconnect body, also the first half of a relink. Caller holds mutex_.
    void teardown_session_locked() {
        auto& protocol = SynScanProtocolWrapper::instance();
        protocol.disconnect();
        connected_ = false;
        {
            std::lock_guard<std::mutex> fwlock(firmware_mutex_);
            firmware_cache_.clear();
        }
        target_ra_set_ = false;
        target_dec_set_ = false;
        parked_ = false;
        parking_ = false;
        at_home_ = false;
        clear_pulse_guiding_locked();
        set_slewing_locked(false);
        refining_generation_ = 0;
        last_slew_error_.clear();
        slew_force_until_ = util::TaskClock::clock::time_point::min();
        position_override_until_ = util::TaskClock::clock::time_point::min();
        invalidate_readback_tick_locked();
        guide_position_valid_ = false;
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
        position_link_health_.reset();
        publish_link_fault_locked();
        snapshot_.reset();
        link_lost_ = false;
    }

    void check_connected() const {
        if (!connected_ || link_lost_.load()) {
            throw AlpacaException("Not connected to SynScan mount", AlpacaError::NotConnected);
        }
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
    // lifecycle paths (disconnect, relink, the destructor) stop it, through stop_status_poller().
    void cancel_async_tasks() {
        slew_.cancel_all_and_join();
        join_pulse_bodies();
    }

    // The pulse stop bodies take mutex_ (after an RA stop, to restore tracking, and on failure), so a motion
    // initiator ends them by cancelling and joining with no driver mutex held (decision 0006, "Pulse and rate-check
    // slots"). Only the slew slot is never joined by an initiator.
    void join_pulse_bodies() {
        pulse_ops_[0].cancel_all_and_join();
        pulse_ops_[1].cancel_all_and_join();
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
    // both held. The destructor passes the turn on, on every path. Lock order: turn, mutex_, wrapper mutex.
    class WireTurn {
    public:
        WireTurn(SynScanTelescopeDriver& driver, std::unique_lock<std::mutex>& lock) : driver_(driver) {
            std::unique_lock<std::mutex> turn_lock(driver_.turn_mutex_);
            const uint64_t ticket = driver_.turn_next_++;
            lock.unlock();
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

    // AbortSlew's stop and published state, for a refused start that cancelled
    // a body in flight. A failed stop leaves Slewing as it was: the mount may
    // still be moving.
    void stop_after_refused_start_locked(std::unique_lock<std::mutex>& lock) {
        std::string stop_error;
        if (!send_in_turn(
                lock, [this] { return static_cast<bool>(connected_); },
                [&stop_error] { stop_error = stop_park_slew_wire(); })) {
            return;
        }
        if (stop_error.empty()) {
            set_slewing_locked(false);
        } else {
            ALPACA_LOG_ERROR("SynScan",
                             "stop after refused slew start failed: " + stop_error + "; the mount may still be moving");
        }
        parking_ = false;
        bump_motion_generation();
        slew_force_until_ = util::TaskClock::clock::time_point::min();
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

    // Stops (true: the poll body is joined, so it parks no waiter on a fake task clock) or resumes (false) the status
    // poll. A case that scripts the handset's replies or counts virtual seconds pauses it so the poll does not
    // consume them.
    void set_status_poll_paused_for_testing(bool paused) {
        status_poll_paused_for_testing_ = paused;
        if (paused) {
            status_poll_.cancel_all_and_join();
        } else {
            start_status_poller();
        }
    }

    // One full tick on the calling thread, paused or not: a case that fixes the handset's state and the clock
    // reads exactly one frame. False when the connection is gone.
    bool poll_status_now_for_testing() { return poll_status_once(nullptr, 0); }

    // Stands in for the readback-tick hunt: takes (true) or drops (false) the hold under mutex_, as the hunt does.
    void hold_status_poll_for_testing(bool hold) {
        std::lock_guard<std::mutex> lock(mutex_);
        poll_hold_ += hold ? 1 : -1;
    }

    // Called after a poll's first hold check, before its locked second one, with no lock held.
    void set_status_poll_before_sample_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> lock(status_poll_hook_mutex_);
        status_poll_before_sample_hook_ = std::move(hook);
    }

    // Replaces the wall time every local-sidereal-time read uses; an empty function restores system_clock.
    void set_sidereal_wall_clock_for_testing(std::function<std::chrono::system_clock::time_point()> now) {
        std::lock_guard<std::mutex> lock(sidereal_wall_clock_mutex_);
        sidereal_wall_clock_ = std::move(now);
    }

    // Called between a poll's sample and its publish, with no lock held.
    void set_status_poll_after_sample_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> lock(status_poll_hook_mutex_);
        status_poll_after_sample_hook_ = std::move(hook);
    }

private:
    void clear_pulse_guiding_locked() {
        pulse_guiding_active_.fill(false);
        pulse_guide_end_time_.fill(util::TaskClock::clock::time_point::min());
    }

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
            std::abs(shortest_ra_delta_hours(corrected, readback_top_hours_)) > kReadbackDisambiguationHours) {
            // An anchor a little late puts a post-tick read one second high; a little early, a pre-tick read one
            // second low. Whichever neighbour sits by the last known RA is the read.
            for (const double side : {-1.0, 1.0}) {
                const double alternative = std::fmod(corrected + side / 3600.0 + 24.0, 24.0);
                if (std::abs(shortest_ra_delta_hours(alternative, readback_top_hours_)) <=
                    kReadbackDisambiguationHours) {
                    return alternative;
                }
            }
        }
        return corrected;
    }

    struct PollHold {
        explicit PollHold(std::atomic<int>& count) : count_(count) { ++count_; }
        ~PollHold() { --count_; }
        PollHold(const PollHold&) = delete;
        PollHold& operator=(const PollHold&) = delete;

    private:
        std::atomic<int>& count_;
    };

    struct SamplingFlag {
        explicit SamplingFlag(std::atomic<bool>& flag) : flag_(flag) {}
        ~SamplingFlag() { flag_ = false; }
        SamplingFlag(const SamplingFlag&) = delete;
        SamplingFlag& operator=(const SamplingFlag&) = delete;

    private:
        std::atomic<bool>& flag_;
    };

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
    TickHunt hunt_readback_tick_locked(std::unique_lock<std::mutex>& lock, uint64_t owner_generation,
                                       util::OperationContext* ctx) const {
        using clock = util::TaskClock::clock;
        // The hunt reads at a 50 ms cadence tuned on the rig; the poll skips its ticks while it runs (bounded).
        const PollHold hold(poll_hold_);
        // A tick already inside its exchanges queues the hunt's reads behind it on the wrapper mutex and widens the
        // bracket around the tick: let it finish (mutex_ is dropped so its publish can run). No new tick starts. The
        // wait is bounded: a tick on a silent link can take several response timeouts, and past the bound the hunt
        // reads anyway, queued behind it. A superseding command or a stop ends the wait at once.
        const auto wait_until = clock_.now() + kPollSampleWaitBound;
        while (poll_sampling_.load() && clock_.now() < wait_until) {
            lock.unlock();
            if (ctx != nullptr) {
                (void)ctx->wait_for(std::chrono::milliseconds(2));  // a stop ends it at once; the check below reads it
            } else {
                clock_.sleep_for(std::chrono::milliseconds(2));
            }
            lock.lock();
            if (motion_generation_ != owner_generation ||
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
            if (motion_generation_ != owner_generation ||
                (ctx != nullptr && ctx->stop_reason() != util::StopReason::None)) {
                return hunt;
            }
            check_connected();
            const auto sample = read_equatorial_released(lock);
            if (motion_generation_ != owner_generation ||
                (ctx != nullptr && ctx->stop_reason() != util::StopReason::None)) {
                return hunt;  // overtaken during the read: the caller sees that and drops the result
            }
            const auto raw = sample.first;
            const auto read_at = sample.second;
            const double hours = decode_ra_hours(raw.first, 24);
            if (have_previous && shortest_ra_delta_hours(hours, previous_hours) >= kReadbackTickJumpHours) {
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

    // Supersedes every in-flight motion and wakes a synchronous landing nudge waiting on `motion_cv_`.
    uint64_t bump_motion_generation() {
        const uint64_t generation = ++motion_generation_;
        motion_cv_.notify_all();
        return generation;
    }

    // Moves one axis by `delta_arcsec` (RA: east positive; Dec: north positive) at the guide rate and stops,
    // restoring tracking after an RA move as a pulse does. Returns false when superseded or cancelled.
    bool nudge_axis_locked(std::unique_lock<std::mutex>& lock, int axis, double delta_arcsec, uint64_t owner_generation,
                           util::OperationContext* ctx) {
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
            if (lost_ownership_locked(owner_generation, ctx)) {
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
        if (lost_ownership_locked(owner_generation, ctx)) {
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
            // The wait releases mutex_ while parked; every initiator that bumps motion_generation_ signals
            // motion_cv_, so a superseded synchronous nudge returns at once instead of sleeping out move_time.
            clock_.wait_for(lock, motion_cv_, move_time, [&] { return motion_generation_ != owner_generation; });
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
        return keep && !lost_ownership_locked(owner_generation, ctx);
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
    RawSample sample_raw(bool precise, bool full) const {
        auto& protocol = SynScanProtocolWrapper::instance();
        RawSample raw;
        raw.slewing = protocol.is_goto_in_progress();
        raw.equatorial = protocol.get_ra_dec_raw(precise);
        raw.read_at = clock_.now();
        if (full) {
            try {
                raw.altaz = protocol.get_alt_az_raw(precise);
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): keeps the last value
            }
            try {
                raw.pier = protocol.get_pointing_state();
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): keeps the last value
            }
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
            frame.az_degrees = wrap_degrees(decode_angle(raw.altaz->first, bits));
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
    // (DriverException "communications compromised", Connected stays true, decision 0009 point 1), no frame yet.
    // A frame past kSnapshotMaxAge is served as it is: stale is not lost.
    util::StateSnapshot<PolledFrame>::Reading served_frame_locked() const {
        check_connected();
        if (position_link_health_.faulted()) {
            throw_position_link_fault_locked();
        }
        auto reading = snapshot_.read();
        if (!reading) {
            throw AlpacaException(
                "SynScan mount state is not available yet: the first status poll has not been answered",
                AlpacaError::DriverException);
        }
        return *reading;
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
    // snapshot's own mutex.
    std::optional<std::chrono::nanoseconds> device_state_age() const override {
        const auto reading = snapshot_.read();
        if (!reading) {
            return std::nullopt;
        }
        return clock_.now() - reading->measured_at;
    }

    // A write that changed what the handset's position means (a slew start, a landing round): drops the poll in
    // flight so its older sample cannot overwrite what the control path knows. The next poll publishes.
    void drop_poll_in_flight_locked() const {
        snapshot_.write([](PolledFrame&) {});
    }

    // The slewing flag the control path just decided (a slew started or ended, an abort), written through so the
    // next read sees it before the next poll runs.
    // The tracking mode the control path just commanded, written through as set_slewing_locked() is.
    void set_tracking_mode_cached_locked(int mode) const {
        tracking_mode_cached_ = mode;
        snapshot_.write([mode](PolledFrame& frame) { frame.tracking_mode = mode; });
    }

    void set_slewing_locked(bool slewing) const {
        slewing_cached_ = slewing;
        snapshot_.write([slewing](PolledFrame& frame) { frame.slewing = slewing; });
    }

    // A re-read of the handset's slewing flag (the slew-complete and Park loops, every 250 ms). Writes only a change:
    // every write drops the poll tick in flight, so a same-value write on each re-read would drop nearly every tick
    // of a GOTO and freeze the position. A control-path decision (a start, an end, an abort) goes through
    // set_slewing_locked() and always drops the tick, which sampled before what the driver now knows. The poll
    // publishes under mutex_, which the caller holds, so the held value cannot move between the read and the write.
    void note_slewing_read_locked(bool slewing) const {
        const auto held = snapshot_.read();
        if (held && held->value.slewing == slewing) {
            slewing_cached_ = slewing;
            return;
        }
        set_slewing_locked(slewing);
    }

    // Where the control path ends a motion (slew landed, park complete): reads the handset now and publishes a full
    // frame, so Slewing false is never served beside a mid-motion position. A failed read leaves the position
    // unknown, which the getters refuse until the next poll answers.
    void publish_measurement_locked() const {
        const auto token = snapshot_.begin_poll();
        try {
            const RawSample raw = sample_raw(use_precise_commands_, true);
            const auto previous = snapshot_.read();
            snapshot_.publish(
                token, decode_sample_locked(raw, use_precise_commands_, previous ? previous->value : PolledFrame{}));
            drop_poll_in_flight_locked();  // a poll that sampled before this read must not replace it
            note_position_reply_locked();
        } catch (const std::exception& e) {
            snapshot_.write([](PolledFrame& frame) {
                frame.equatorial_known = false;
                frame.altaz_known = false;
            });
            note_position_failure_locked(e);
        }
    }

    // publish_measurement_locked() for a body: the handset is read with mutex_ released, then the frame is published
    // unless `lost()` (evaluated with mutex_ held) says a newer operation owns the state meanwhile.
    template <typename Lost>
    void publish_measurement_released(std::unique_lock<std::mutex>& lock, const Lost& lost) const {
        const auto token = snapshot_.begin_poll();
        std::optional<RawSample> raw;
        std::string failure;
        try {
            raw = without_lock(lock, [this] { return sample_raw(use_precise_commands_, true); });
        } catch (const std::exception& e) {
            failure = e.what();
        }
        if (lost()) {
            return;
        }
        if (raw) {
            const auto previous = snapshot_.read();
            snapshot_.publish(
                token, decode_sample_locked(*raw, use_precise_commands_, previous ? previous->value : PolledFrame{}));
            drop_poll_in_flight_locked();  // a poll that sampled before this read must not replace it
            note_position_reply_locked();
        } else {
            snapshot_.write([](PolledFrame& frame) {
                frame.equatorial_known = false;
                frame.altaz_known = false;
            });
            note_position_failure_locked(std::runtime_error(failure));
        }
    }

    // The first frame, at the end of a connect with mutex_ held, so the getters have an answer, status flags
    // included, the moment Connect returns (ConformU's first reads are held to its fast-response target). A handset
    // that does not answer leaves a frame with nothing known.
    void seed_snapshot_locked() {
        const auto token = snapshot_.begin_poll();
        PolledFrame frame;
        try {
            frame = decode_sample_locked(sample_raw(use_precise_commands_, true), use_precise_commands_, frame);
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): the poll fills it in
        }
        snapshot_.publish(token, frame);
    }

    // Control path: one direct read of the equatorial pair, for an operation that needs the mount's position now
    // (SetPark). {RA hours, Dec degrees}.
    std::pair<double, double> measure_equatorial_locked() const {
        auto& protocol = SynScanProtocolWrapper::instance();
        int bits = use_precise_commands_ ? 24 : 16;
        try {
            auto raw = protocol.get_ra_dec_raw(use_precise_commands_);
            const auto read_at = clock_.now();
            double ra = decode_ra_hours(raw.first, bits);
            if (use_precise_commands_) {
                ra = corrected_ra_hours_locked(ra, read_at);
            }
            note_position_reply_locked();
            return {ra, decode_angle(raw.second, bits)};
        } catch (const std::exception& e) {
            note_position_failure_locked(e);
            if (position_link_health_.faulted()) {
                throw_position_link_fault_locked();
            }
            throw;
        }
    }

    // The position for an initiator that arrives while a body owns the slew slot: the polled frame, so the initiator
    // does not queue a read behind the body's exchange in flight (decision 0006, "Claim"). Without a frame it reads.
    std::pair<double, double> served_equatorial_or_measure_locked() const {
        try {
            return served_equatorial_locked();
        } catch (const AlpacaException&) {
            return measure_equatorial_locked();
        }
    }

    std::pair<double, double> served_altaz_or_measure_locked() const {
        try {
            return served_altaz_locked();
        } catch (const AlpacaException&) {
            return measure_altaz_locked();
        }
    }

    // {azimuth, altitude}, as measure_equatorial_locked().
    std::pair<double, double> measure_altaz_locked() const {
        auto& protocol = SynScanProtocolWrapper::instance();
        int bits = use_precise_commands_ ? 24 : 16;
        try {
            auto raw = protocol.get_alt_az_raw(use_precise_commands_);
            note_position_reply_locked();
            return {wrap_degrees(decode_angle(raw.first, bits)), decode_angle(raw.second, bits)};
        } catch (const std::exception& e) {
            note_position_failure_locked(e);
            if (position_link_health_.faulted()) {
                throw_position_link_fault_locked();
            }
            throw;
        }
    }

    // Test seam: called between a poll's sample and its publish, with no lock held, so a case can land a client
    // write in that window.
    void run_status_poll_after_sample_hook() {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(status_poll_hook_mutex_);
            hook = status_poll_after_sample_hook_;
        }
        if (hook) {
            hook();
        }
    }

    // Test seam: called after a poll's first hold check, before it takes mutex_, with no lock held, so a case can
    // start the hunt's hold in the window the second check closes.
    void run_status_poll_before_sample_hook() {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(status_poll_hook_mutex_);
            hook = status_poll_before_sample_hook_;
        }
        if (hook) {
            hook();
        }
    }

    // One poll tick. The exchanges run WITHOUT mutex_, so a getter never waits behind a slow link and the snapshot
    // mutex is never held across I/O; the frame is decoded and published under mutex_. Returns false when the
    // connection is gone or lost and the poll body should end.
    // `ctx` is null for the test seam's direct tick, which also ignores the pause.
    bool poll_status_once(util::OperationContext* ctx, int tick) {
        const auto stopped = [ctx] { return ctx != nullptr && ctx->stop_reason() != util::StopReason::None; };
        if ((ctx != nullptr && status_poll_paused_for_testing_.load()) || poll_hold_.load() > 0) {
            return true;  // paused, or the readback-tick hunt owns the line
        }
        run_status_poll_before_sample_hook();
        bool precise = false;
        bool read_site = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_ || link_lost_.load() || stopped()) {
                return false;
            }
            // The hunt takes its hold under mutex_, so this check and poll_sampling_ below cannot straddle it: either
            // the hunt sees the sample in flight and waits for it, or this tick sees the hold and skips.
            if (poll_hold_.load() > 0) {
                return true;
            }
            precise = use_precise_commands_;
            // The unread site (a connect that could not reach `w`) is retried here, not in a getter, and like the
            // sample below its exchange runs without mutex_, so a getter never waits on a handset that ignores `w`.
            read_site = !site_info_valid_ && tick % kSlowPollEvery == 0 && site_read_due_locked();
            poll_sampling_ = true;
        }
        const SamplingFlag sampling(poll_sampling_);  // cleared on every exit, a throw included
        if (read_site) {
            std::optional<LocationInfo> site;
            try {
                site = SynScanProtocolWrapper::instance().get_location();
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): retried after kSiteInfoRetryDelay
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_ || stopped()) {
                return false;
            }
            if (site) {
                apply_site_info_locked(*site);
            }
        }
        const auto token = snapshot_.begin_poll();
        RawSample raw;
        try {
            raw = sample_raw(precise, tick % kSlowPollEvery == 0);
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_ || stopped()) {
                return false;
            }
            note_position_failure_locked(e);
            return !declare_link_lost_if_stale_locked();
        }
        run_status_poll_after_sample_hook();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_ || stopped()) {
            return false;
        }
        const auto previous = snapshot_.read();
        snapshot_.publish(token, decode_sample_locked(raw, precise, previous ? previous->value : PolledFrame{}));
        note_position_reply_locked();  // a dropped frame (a write landed since the sample) was still a reply
        return true;
    }

    // Decision 0009 point 2(b): a fault that has stood past the bound is a LOST link. Connected reads false, the
    // getters and setters throw NotConnected, DeviceState is empty, the transport is closed; only the client's
    // Connected=true relinks. Caller holds mutex_. True when the link was just declared lost.
    bool declare_link_lost_if_stale_locked() {
        if (!position_link_health_.fault_stale(clock_.now())) {
            return false;
        }
        ALPACA_LOG_ERROR(
            "SynScan",
            "Link to the hand controller lost after " +
                std::to_string(std::chrono::duration_cast<std::chrono::seconds>(util::kLinkStalenessBound).count()) +
                " s without a reply; reconnect to resume (" + position_link_health_.fault() + ")");
        link_lost_ = true;
        SynScanProtocolWrapper::instance().disconnect();
        return true;
    }

    void run_status_poller(util::OperationContext& ctx) {
        for (int tick = 0;; ++tick) {
            const auto started = clock_.now();
            if (!poll_status_once(&ctx, tick)) {
                return;
            }
            // Start to start: wait for the next tick, skipping any a slow poll ran over (no catch-up).
            const auto spent = clock_.now() - started;
            const auto ticks = spent / kStatusPollPeriod + 1;
            if (!ctx.wait_for(ticks * kStatusPollPeriod - spent)) {
                return;
            }
        }
    }

    // Call without mutex_ held: the poll body takes mutex_. The lifecycle paths' half of the rule on
    // cancel_async_tasks().
    void stop_status_poller() { status_poll_.cancel_all_and_join(); }

    // Call without mutex_ held (AsyncOperation rule 10), after a connect.
    void start_status_poller() {
        try {
            status_poll_.start([this](util::OperationContext& ctx) { run_status_poller(ctx); });
        } catch (const std::exception& e) {
            ALPACA_LOG_ERROR("SynScan", std::string("Status poll did not start: ") + e.what());
        }
    }

    void note_position_reply_locked() const {
        if (position_link_health_.on_reply()) {
            ALPACA_LOG_INFO("SynScan", "Position link recovered; mount readback is available again");
        }
        publish_link_fault_locked();
    }

    // Copies the latched fault text to the narrow-mutex copy get_link_fault() reads.
    // Call under mutex_ after every change to position_link_health_.
    void publish_link_fault_locked() const {
        std::lock_guard<std::mutex> lock(link_fault_mutex_);
        link_fault_text_ = position_link_health_.fault();
    }

    void note_position_failure_locked(const std::exception& e) const {
        if (auto fault = position_link_health_.note_failure(e.what(), kPositionLinkFailureThreshold, clock_.now())) {
            ALPACA_LOG_ERROR("SynScan", "Position link faulted: " + *fault);
        } else {
            ALPACA_LOG_WARN("SynScan", "Position read failed (" +
                                           std::to_string(position_link_health_.consecutive_failures()) +
                                           " consecutive failures): " + e.what());
        }
        publish_link_fault_locked();
    }

    [[noreturn]] void throw_position_link_fault_locked() const {
        throw AlpacaException("SynScan mount communications compromised: " + position_link_health_.fault(),
                              AlpacaError::DriverException);
    }

    // `fresh` asks the handset for the end of the answer (the PulseGuide gate), not the polled frame.
    bool get_slewing_locked(bool fresh = false) const {
        // A park in flight reports Slewing until the park task flips AtPark
        // (same locked step) — never Slewing false with AtPark false.
        if (parking_) {
            return true;
        }
        if (refining_generation_ != 0 && refining_generation_ == motion_generation_) {
            return true;  // a GOTO whose landing is still being refined (open-astro#880)
        }
        if (manual_axis_slewing_[0] || manual_axis_slewing_[1]) {
            return true;
        }
        if (clock_.now() < slew_force_until_) {
            return true;
        }
        return fresh ? poll_hardware_slewing_locked() : served_frame_locked().value.slewing;
    }

    // Control path only: asks the handset now (the Park and landing loops are what detect a GOTO's end). The
    // getters serve the frame instead. ignore_dec_jog: a Dec-only MoveAxis does not touch the RA motor or
    // tracking, so ra_motion_active_locked() leaves it out.
    // A body passes `release` (its lock) and `lost`: the read runs with mutex_ released, and nothing is applied, not
    // even a failed read's count, once `lost()` says a newer operation owns the state (the caller checks that itself).
    bool poll_hardware_slewing_locked(bool propagate_error = false, bool ignore_dec_jog = false,
                                      std::unique_lock<std::mutex>* release = nullptr,
                                      const std::function<bool()>& lost = {}) const {
        if (manual_axis_slewing_[0] || (manual_axis_slewing_[1] && !ignore_dec_jog)) {
            return true;
        }
        if (clock_.now() < slew_force_until_) {
            return true;
        }
        bool was_slewing = slewing_cached_;
        try {
            bool reply = false;
            if (release != nullptr) {
                reply = without_lock(*release, [] { return SynScanProtocolWrapper::instance().is_goto_in_progress(); });
                if (lost && lost()) {
                    return slewing_cached_;
                }
            } else {
                reply = SynScanProtocolWrapper::instance().is_goto_in_progress();
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
    // handset tracking off, so the handset's mode is not read while one runs (open-astro#1259). The pulse flag
    // clears only when IsPulseGuiding is read, so a pulse counts as running until its end time, and the slew state
    // is polled, never taken from the frame. A Dec-only jog is not an RA motion.
    bool ra_motion_active_locked() const {
        const bool pulsing = pulse_guiding_active_[0] && clock_.now() < pulse_guide_end_time_[0];
        if (pulsing || parking_ || (refining_generation_ != 0 && refining_generation_ == motion_generation_)) {
            return true;
        }
        return poll_hardware_slewing_locked(false, true);
    }

    // Reads the handset's tracking mode into the cache before a command starts a motion or checks Tracking, so a
    // stop restores the mode the handset held at the start and not the one cached at connect: the keypad can change
    // it in between. A motion that starts while another RA motion runs keeps the mode the first one captured.
    void refresh_tracking_mode_locked() const {
        // A body that owns the slew slot is an RA motion in progress: its answer comes from the driver (before the
        // first read, the polled mode), never from a handset exchange queued behind the body's own (an initiator must
        // not wait for the old body).
        if (!tracking_mode_valid_ && slew_.running()) {
            seed_tracking_mode_from_frame_locked();
        }
        if (tracking_mode_valid_ && (slew_.running() || ra_motion_active_locked())) {
            return;
        }
        int mode = 0;
        try {
            mode = SynScanProtocolWrapper::instance().get_tracking_mode();
        } catch (...) {
            // A failed read keeps what the driver knew: before the first read that is the polled mode.
            if (!tracking_mode_valid_) {
                seed_tracking_mode_from_frame_locked();
            }
            throw;
        }
        // Written through to the frame Tracking serves only when it changed: every write drops the poll tick in
        // flight, as for the slewing flag (note_slewing_read_locked).
        const auto reading = snapshot_.read();
        if (reading && reading->value.tracking_mode != mode) {
            set_tracking_mode_cached_locked(mode);
        } else {
            tracking_mode_cached_ = mode;
        }
        tracking_mode_valid_ = true;
    }

    void seed_tracking_mode_from_frame_locked() const {
        const auto reading = snapshot_.read();
        if (reading && reading->value.tracking_mode >= 0) {
            tracking_mode_cached_ = reading->value.tracking_mode;
            tracking_mode_valid_ = true;
        }
    }

    bool get_tracking_locked() const {
        if (!tracking_mode_valid_) {
            tracking_mode_cached_ = SynScanProtocolWrapper::instance().get_tracking_mode();
            tracking_mode_valid_ = true;
        }
        return tracking_mode_cached_ != 0;
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

    void ensure_site_info_cached_locked() const {
        if (!site_read_due_locked()) {
            return;
        }
        try {
            apply_site_info_locked(SynScanProtocolWrapper::instance().get_location());
        } catch (...) {
            site_info_valid_ = false;
        }
    }

    // True when a `w` read should run now (connected, and an unread site past its retry delay); stamps the attempt.
    bool site_read_due_locked() const {
        if (!connected_) {
            return false;
        }
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
    void store_legacy_park_position_locked() {
        const auto equatorial = slew_.running() ? served_equatorial_or_measure_locked() : measure_equatorial_locked();
        park_alignment_mode_ = AlignmentMode::GermanPolar;
        park_ra_uses_hour_angle_ = false;
        park_ra_hours_ = equatorial.first;
        park_dec_degrees_ = equatorial.second;
        park_position_set_ = true;
    }

    void store_park_position_locked() {
        try {
            park_alignment_mode_ = alignment_mode_locked();
        } catch (const AlpacaException&) {
            // Dual-mode and unidentified handsets cannot tell us whether the
            // saved RA/Dec needs sidereal conversion. Keep their prior usable
            // park behavior instead of making SetPark and Park unavailable.
            store_legacy_park_position_locked();
            return;
        }
        if (park_alignment_mode_ == AlignmentMode::AltAz) {
            const auto altaz = slew_.running() ? served_altaz_or_measure_locked() : measure_altaz_locked();
            park_azimuth_degrees_ = altaz.first;
            park_altitude_degrees_ = altaz.second;
            park_ra_uses_hour_angle_ = false;
        } else {
            if (!site_info_valid_ && !slew_.running()) {  // no `w` queued behind a body's exchange
                ensure_site_info_cached_locked();
            }
            if (!site_info_valid_) {
                // No longitude, so no LST for an hour angle: keep the prior
                // park behavior rather than making SetPark and Park unavailable.
                store_legacy_park_position_locked();
                return;
            }
            const auto equatorial =
                slew_.running() ? served_equatorial_or_measure_locked() : measure_equatorial_locked();
            const double lst = compute_local_sidereal_time_hours(sidereal_wall_now(), site_longitude_cached_);
            park_hour_angle_hours_ = shortest_ra_delta_hours(lst, equatorial.first);
            park_ra_hours_ = equatorial.first;
            park_dec_degrees_ = equatorial.second;
            park_ra_uses_hour_angle_ = true;
        }
        park_position_set_ = true;
    }

    LocationInfo current_location_locked() const {
        LocationInfo info;
        if (site_info_valid_) {
            info.latitude_degrees = site_latitude_cached_;
            info.longitude_degrees = site_longitude_cached_;
            return info;
        }
        return SynScanProtocolWrapper::instance().get_location();
    }

    // The GOTO is sent in its wire turn with mutex_ released (`lock` is held on entry and on return). `owns` says the
    // caller still owns the slew, at the start and again at the turn: a caller superseded meanwhile sends nothing and
    // leaves the target and the flags to the operation that replaced it.
    template <typename Owns>
    void do_slew_to_coordinates_locked(double ra, double dec, std::unique_lock<std::mutex>& lock, Owns&& owns) {
        validate_ra_dec(ra, dec, "SlewToCoordinates");
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
        slew_force_until_ = clock_.now() + std::chrono::seconds(8);
        position_override_until_ = util::TaskClock::clock::time_point::min();
        guide_position_valid_ = false;
        if (!send_in_turn(lock, owns, [&] { protocol.goto_ra_dec_raw(ra_raw, dec_raw, use_precise_commands_); }) ||
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
        slew_force_until_ = clock_.now() + std::chrono::seconds(8);
        position_override_until_ = util::TaskClock::clock::time_point::min();
        guide_position_valid_ = false;
        if (!send_in_turn(lock, owns, [&] { protocol.goto_alt_az_raw(az_raw, alt_raw, use_precise_commands_); }) ||
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
    // Returns false when a concurrent motion command bumped motion_generation_
    // past `owner_generation` while the lock was released (the slew was superseded).
    // `ctx` (async slew body only) makes every sleep interruptible through the slot's wait_for(), the way the park
    // body waits, so a join by Disconnect, MoveAxis, a new slew or the destructor returns at once; the wait then
    // returns false.
    // True once a newer operation has taken the axes (a motion initiator bumps motion_generation_) or the body behind
    // `ctx` was cancelled or replaced. Caller holds mutex_.
    bool lost_ownership_locked(uint64_t owner_generation, const util::OperationContext* ctx) const {
        return motion_generation_ != owner_generation || slot_stopped(ctx);
    }

    bool wait_for_slew_complete(std::unique_lock<std::mutex>& lock, uint64_t owner_generation,
                                util::OperationContext* ctx = nullptr) const {
        const auto timeout = std::chrono::seconds(120);
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
            if (motion_generation_ != owner_generation ||
                (ctx != nullptr && ctx->stop_reason() != util::StopReason::None)) {
                return false;
            }
            // The hardware answer, not get_slewing_locked(): a refinement scope holds the public Slewing true. The
            // read runs with mutex_ released; a poll that was overtaken applies nothing.
            bool slewing = poll_hardware_slewing_locked(false, false, &lock,
                                                        [&] { return lost_ownership_locked(owner_generation, ctx); });
            if (lost_ownership_locked(owner_generation, ctx)) {
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
            if (clock_.now() - start > timeout) {
                throw AlpacaException("Slew timed out");
            }
            sleep_unlocked(std::chrono::milliseconds(250));
        }
        // The measurement is read with mutex_ released; a body overtaken meanwhile publishes and clears nothing.
        publish_measurement_released(lock, [&] { return lost_ownership_locked(owner_generation, ctx); });
        if (lost_ownership_locked(owner_generation, ctx)) {
            return false;
        }
        set_slewing_locked(false);
        slew_force_until_ = util::TaskClock::clock::time_point::min();
        guide_position_valid_ = false;
        position_override_until_ = util::TaskClock::clock::time_point::min();
        return true;
    }

    // The configured settle time, applied once after the LAST GOTO pass (open-astro#880). `lock` held on entry
    // and on return; false when a concurrent motion command superseded the slew meanwhile.
    bool settle_after_slew_locked(std::unique_lock<std::mutex>& lock, uint64_t owner_generation) const {
        if (slew_settle_time_seconds_ > 0) {
            lock.unlock();
            clock_.sleep_for(std::chrono::seconds(slew_settle_time_seconds_));
            lock.lock();
            check_connected();
        }
        return motion_generation_ == owner_generation;
    }

    // Marks a GOTO whose landing will be refined: get_slewing_locked() reports true while the scope's generation
    // is current, so Slewing cannot dip between the passes. Cleared on every exit; a newer motion command (or
    // AbortSlew) bumps motion_generation_, which ends it on its own. `skip` leaves it unset (no refinement).
    class RefineScope {
    public:
        RefineScope(const SynScanTelescopeDriver& driver, uint64_t generation, bool skip)
            : driver_(driver), generation_(generation) {
            if (!skip) {
                driver_.refining_generation_ = generation;
            }
        }
        ~RefineScope() {
            if (driver_.refining_generation_ == generation_) {
                driver_.refining_generation_ = 0;
            }
        }
        RefineScope(const RefineScope&) = delete;
        RefineScope& operator=(const RefineScope&) = delete;

    private:
        const SynScanTelescopeDriver& driver_;
        uint64_t generation_;
    };

    // After a GOTO completes, read the handset's own position at its tick (open-astro#1030) and, while RA or Dec is
    // off by more than kLandingToleranceArcsec, finish the landing with timed guide-rate moves, at most
    // kMaxLandingRounds rounds (open-astro#880). The handset re-rolls every GOTO landing by a whole second of RA,
    // so a residual is never chased with another GOTO; only one beyond a timed move's reach gets a single re-GOTO
    // first. A landing beyond kLandingRefineLimitArcsec gets one repeat GOTO to the same target, and throws if it
    // misses that far again (open-astro#1105). 16-bit handsets (V3) step ~19.8" in RA, so they are not refined.
    // `lock` is held on entry and on return. Returns false when the slew was superseded, aborted or cancelled; throws
    // when a move fails. A residual left after the last round is logged as a WARN, not thrown (open-astro#1027).
    bool refine_goto_landing_locked(std::unique_lock<std::mutex>& lock, double ra, double dec,
                                    uint64_t owner_generation, util::OperationContext* ctx) {
        if (!use_precise_commands_) {
            return true;
        }
        // The rounds move the mount after the slew-end frame was published: publish where the landing ended, unless
        // a newer motion owns the axes (its own end publishes).
        bool finished = false;
        try {
            finished = refine_goto_landing_rounds_locked(lock, ra, dec, owner_generation, ctx);
        } catch (...) {
            if (motion_generation_ == owner_generation && connected_) {
                publish_measurement_released(lock, [&] { return motion_generation_ != owner_generation; });
            }
            throw;
        }
        if (motion_generation_ == owner_generation && connected_) {
            publish_measurement_released(lock, [&] { return motion_generation_ != owner_generation; });
        }
        return finished;
    }

    bool refine_goto_landing_rounds_locked(std::unique_lock<std::mutex>& lock, double ra, double dec,
                                           uint64_t owner_generation, util::OperationContext* ctx) {
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
            if (motion_generation_ != owner_generation ||
                (ctx != nullptr && ctx->stop_reason() != util::StopReason::None)) {
                return false;
            }
            check_connected();
            const TickHunt hunt = hunt_readback_tick_locked(lock, owner_generation, ctx);
            if (motion_generation_ != owner_generation ||
                (ctx != nullptr && ctx->stop_reason() != util::StopReason::None)) {
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
            double ra_error_hours = shortest_ra_delta_hours(ra, landed_ra_hours);
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
                slew_force_until_ = clock_.now() + kRefineStartForce;
                if (!send_in_turn(
                        lock, [&] { return !lost_ownership_locked(owner_generation, ctx); },
                        [&] { protocol.goto_ra_dec_raw(encode_ra_raw(ra, 24), encode_angle(dec, 24), true); })) {
                    return false;
                }
                if (!wait_for_slew_complete(lock, owner_generation, ctx)) {
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
                slew_force_until_ = clock_.now() + kRefineStartForce;
                if (!send_in_turn(
                        lock, [&] { return !lost_ownership_locked(owner_generation, ctx); },
                        [&] {
                            protocol.goto_ra_dec_raw(encode_ra_raw(aim_ra, 24), encode_angle(aim_dec, 24), true);
                        })) {
                    return false;
                }
                if (!wait_for_slew_complete(lock, owner_generation, ctx)) {
                    return false;
                }
                continue;
            }
            if (ra_error_arcsec > kLandingToleranceArcsec &&
                !nudge_axis_locked(lock, 0, ra_error_hours * kHoursToDegrees * 3600.0, owner_generation, ctx)) {
                return false;
            }
            if (dec_error_arcsec > kLandingToleranceArcsec &&
                !nudge_axis_locked(lock, 1, dec_error_degrees * 3600.0, owner_generation, ctx)) {
                return false;
            }
        }
    }

    void sync_mount_time_locked() {
        auto now_utc = std::chrono::system_clock::now();
        // mutex_ is already held here — call the _locked body, not the public
        // locking method (non-recursive mutex would self-deadlock).
        set_utc_date_locked(now_utc);
    }

    std::chrono::system_clock::time_point current_utc_time_locked() const {
        if (!last_utc_valid_) {
            last_utc_set_ = std::chrono::system_clock::now();
            last_utc_set_monotonic_ = std::chrono::steady_clock::now();
            last_utc_valid_ = true;
        }
        // Pointing time (UTC extrapolation), not a task wait: real steady clock on purpose.
        auto elapsed = std::chrono::steady_clock::now() - last_utc_set_monotonic_;
        return last_utc_set_ + std::chrono::duration_cast<std::chrono::system_clock::duration>(elapsed);
    }

    static double compute_local_sidereal_time_hours(std::chrono::system_clock::time_point utc_time,
                                                    double longitude_degrees) {
        using namespace std::chrono;
        double days_since_epoch = duration<double>(utc_time.time_since_epoch()).count() / 86400.0;
        double jd = 2440587.5 + days_since_epoch;
        double t = (jd - 2451545.0) / 36525.0;
        double gmst = 280.46061837 + 360.98564736629 * (jd - 2451545.0)
                      + 0.000387933 * t * t - (t * t * t) / 38710000.0;
        double lst = gmst + longitude_degrees;
        lst = std::fmod(lst, 360.0);
        if (lst < 0.0) {
            lst += 360.0;
        }
        return lst / kHoursToDegrees;
    }

    static double shortest_ra_delta_hours(double a, double b) {
        double delta = a - b;
        while (delta > 12.0) {
            delta -= 24.0;
        }
        while (delta < -12.0) {
            delta += 24.0;
        }
        return delta;
    }

    static void validate_ra_dec(double ra, double dec, const char* context) {
        if (!std::isfinite(ra) || ra < 0.0 || ra >= 24.0) {
            throw AlpacaException(std::string(context) + ": RA out of range", AlpacaError::InvalidValue);
        }
        if (!std::isfinite(dec) || dec < -90.0 || dec > 90.0) {
            throw AlpacaException(std::string(context) + ": Dec out of range", AlpacaError::InvalidValue);
        }
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
    std::atomic<bool> connected_;  // written under mutex_, read lock-free by get_connected()

    double target_ra_hours_;
    double target_dec_degrees_;
    double guide_position_ra_hours_ = 0.0;
    double guide_position_dec_degrees_ = 0.0;
    mutable bool guide_position_valid_ = false;
    double aperture_diameter_m_;
    double aperture_area_m2_;
    double focal_length_m_;
    mutable util::PolledLinkHealth position_link_health_;
    mutable std::mutex link_fault_mutex_;
    mutable std::string link_fault_text_;
    // The polled frame the getters serve (decision 0010). Declared after clock_ below; one publisher (the poll
    // body, plus the control path at the end of a motion), any number of readers.
    std::atomic<bool> link_lost_{false};     // decision 0009: set by the poll body, read lock-free by get_connected()
    mutable std::atomic<int> poll_hold_{0};  // >0 while the readback-tick hunt owns the line: the poll skips its ticks
    std::atomic<bool> status_poll_paused_for_testing_{false};
    mutable std::mutex status_poll_hook_mutex_;
    std::function<void()> status_poll_after_sample_hook_;
    std::function<void()> status_poll_before_sample_hook_;
    // A poll tick is inside its exchanges (set under mutex_ with the hold check).
    std::atomic<bool> poll_sampling_{false};
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
    mutable std::chrono::system_clock::time_point last_utc_set_;
    mutable std::chrono::steady_clock::time_point last_utc_set_monotonic_;
    mutable bool last_utc_valid_;
    mutable int tracking_mode_cached_;
    mutable bool tracking_mode_valid_;
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
    // motion_generation_ of the GOTO whose landing refinement is under way; 0 when none (open-astro#880).
    mutable uint64_t refining_generation_ = 0;
    // open-astro#575: an async slew dispatch that fails AFTER
    // slew_to_coordinates_async() returned used to be logged and forgotten,
    // leaving Slewing read FALSE -- indistinguishable from a landed goto. Set
    // (under mutex_) by the slew task's catch block on a REAL failure (never
    // on the task's own cancellation), cleared by the next slew initiator and
    // by AbortSlew. Consulted by get_slewing() before the cached bool.
    mutable std::string last_slew_error_;
    mutable util::TaskClock::clock::time_point slew_force_until_;
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
    bool sync_time_on_connect_;
    const SynScanAlignmentSetting alignment_setting_;  // AZ-EQ geometry from the device config (#860)
    GuideRate guide_rate_{};
    mutable std::array<bool, 2> pulse_guiding_active_{};
    mutable std::array<util::TaskClock::clock::time_point, 2> pulse_guide_end_time_{};

    bool park_position_set_ = false;
    mutable bool parking_ = false;  // park task in flight (Slewing true, AtPark false)
    AlignmentMode park_alignment_mode_ = AlignmentMode::GermanPolar;
    bool park_ra_uses_hour_angle_ = false;
    double park_hour_angle_hours_ = 0.0;
    // Bumped under mutex_ by every motion initiator; a sync slew waiting with the lock
    // released compares it to learn it was superseded (Sky-Watcher shape, #832).
    uint64_t motion_generation_ = 0;
    std::condition_variable motion_cv_;  // signalled by bump_motion_generation(); ends a synchronous landing nudge
    // The ordered wire turn (see WireTurn). A leaf mutex: held only to move the two counters or to wait on the cv.
    std::mutex turn_mutex_;
    std::condition_variable turn_cv_;
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
    // generation, so a pulse on one axis never supersedes the other's.
    util::OperationGeneration slew_generation_;
    util::AsyncOperation slew_{"SynScan slew", slew_generation_, clock_};
    mutable util::StateSnapshot<PolledFrame> snapshot_{clock_, kSnapshotMaxAge};
    util::OperationGeneration poll_generation_;
    util::AsyncOperation status_poll_{"SynScan status poll", poll_generation_, clock_};
    util::OperationGeneration pulse_generation_[2];
    util::AsyncOperation pulse_ops_[2] = {{"SynScan pulse guide (RA)", pulse_generation_[0], clock_},
                                          {"SynScan pulse guide (Dec)", pulse_generation_[1], clock_}};
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

bool poll_status_now_for_testing(TelescopeDriver& driver) {
    if (auto* synscan = dynamic_cast<SynScanTelescopeDriver*>(&driver)) {
        return synscan->poll_status_now_for_testing();
    }
    return false;
}

void set_status_poll_after_sample_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* synscan = dynamic_cast<SynScanTelescopeDriver*>(&driver)) {
        synscan->set_status_poll_after_sample_hook_for_testing(std::move(hook));
    }
}

void set_sidereal_wall_clock_for_testing(TelescopeDriver& driver,
                                         std::function<std::chrono::system_clock::time_point()> now) {
    if (auto* synscan = dynamic_cast<SynScanTelescopeDriver*>(&driver)) {
        synscan->set_sidereal_wall_clock_for_testing(std::move(now));
    }
}

void hold_status_poll_for_testing(TelescopeDriver& driver, bool hold) {
    if (auto* synscan = dynamic_cast<SynScanTelescopeDriver*>(&driver)) {
        synscan->hold_status_poll_for_testing(hold);
    }
}

void set_status_poll_before_sample_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* synscan = dynamic_cast<SynScanTelescopeDriver*>(&driver)) {
        synscan->set_status_poll_before_sample_hook_for_testing(std::move(hook));
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
