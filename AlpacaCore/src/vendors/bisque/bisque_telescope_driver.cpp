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
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/hold_window.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/motion_owner.h>
#include <alpacacore/util/pointing.h>
#include <alpacacore/util/state_snapshot.h>
#include <alpacacore/util/telescope_command_gate.h>
#include <alpacacore/util/utc_offset.h>
#include <alpacacore/vendor/bisque/bisque_protocol_wrapper.h>
#include <alpacacore/vendor/bisque/bisque_telescope_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <utility>

namespace alpacacore::vendor::bisque {

namespace {

// Decision 0010, Bisque adoption. A poll body samples TheSkyX every kStatusPollPeriod (start to start; ticks missed
// behind a slow link are skipped, never made up) and publishes one frame; the getters and DeviceState serve it from
// memory and never touch the socket. kSnapshotMaxAge is the FRESHNESS bound: an older frame reads `stale` and is
// still served. It is not the bound at which a getter refuses: that is the link-loss policy of decision 0009
// (kPositionLinkFailureThreshold failed ticks latch a fault, util::kLinkStalenessBound of fault makes the link lost).
constexpr auto kStatusPollPeriod = std::chrono::milliseconds(1000);
constexpr auto kSnapshotMaxAge = std::chrono::milliseconds(3000);
static_assert(kSnapshotMaxAge >= 2 * kStatusPollPeriod, "two poll periods, or a healthy link reads stale");
static_assert(kSnapshotMaxAge < util::kLinkStalenessBound, "freshness must undercut the link-loss bound");
constexpr int kPositionLinkFailureThreshold = 3;

// One polled frame of operational state. Small and copyable: the snapshot copies it under its mutex. A field the
// last read could not fill keeps its previous value; an explicit unavailable UTC mode clears its anchor.
struct PolledFrame {
    double ra_hours = 0.0;
    double dec_degrees = 0.0;
    bool equatorial_known = false;
    double alt_degrees = 0.0;
    double az_degrees = 0.0;
    bool altaz_known = false;
    int side_of_pier = 0;  // ASCOM value
    bool pier_known = false;
    bool tracking = false;
    bool tracking_known = false;
    util::UtcOffset utc;
    util::TaskClock::clock::time_point utc_sampled_at{};
    std::string utc_error;
    int utc_error_code = AlpacaError::InvalidOperation;
};

// Sidereal rate in arcseconds per second (INDI TRACKRATE_SIDEREAL).
constexpr double kSiderealRateArcsecPerSec = 15.0411;

// Slew speed presets (multiples of sidereal rate).
constexpr int kNumSlewSpeeds = 9;
constexpr double kSlewSpeeds[kNumSlewSpeeds] = {
    1.0, 2.0, 4.0, 8.0, 32.0, 64.0, 128.0, 256.0, 512.0
};

// Maximum move axis rate in degrees per second (512x sidereal).
constexpr double kMaxMoveAxisRateDegPerSec =
    kSlewSpeeds[kNumSlewSpeeds - 1] * kSiderealRateArcsecPerSec / 3600.0;

// Default guide rate: 50% of sidereal, in degrees per second.
constexpr double kDefaultGuideRateDegPerSec = 0.5 * kSiderealRateArcsecPerSec / 3600.0;

// Bisque's limits and orders for the telescope command gate (#2258). PulseGuide is capped at 30 s (see pulse_guide());
// MoveAxis moves axes 0 and 1 at any finite rate (the nearest of the nine presets); the guide rate is 0 to 1x sidereal
// on both axes (open-astro#775). Every family checks the link fault before the parked check (check_link_locked()).
constexpr util::TelescopeCommandLimits kCommandLimits = [] {
    util::TelescopeCommandLimits limits;
    limits.pulse_guide_max_ms = 30000;
    limits.pulse_guide_order = util::ParkedFaultOrder::FaultFirst;
    limits.move_axis_axes = 2;
    limits.move_axis_order = util::ParkedFaultOrder::FaultFirst;
    limits.slew_order = util::ParkedFaultOrder::FaultFirst;
    limits.guide_rate_ra_min = 0.0;
    limits.guide_rate_ra_max = kSiderealRateArcsecPerSec / 3600.0;
    limits.guide_rate_dec_min = 0.0;
    limits.guide_rate_dec_max = kSiderealRateArcsecPerSec / 3600.0;
    limits.texts.pulse_direction = "PulseGuide direction or duration is invalid";
    limits.texts.pulse_duration = "PulseGuide duration must be at most 30000 ms";
    limits.texts.guide_rate_range = "Guide rate must be between 0 and 1x sidereal";
    limits.tracking_rate_max = 0;  // TODO: support solar/lunar/custom rates via SetTracking
    limits.texts.tracking_rate = "Only sidereal tracking rate is currently supported";
    return limits;
}();

// Find the closest slewspeed index for a given rate in degrees/sec.
int rate_to_speed_index(double rate_deg_per_sec) {
    double abs_rate = std::abs(rate_deg_per_sec);
    double rate_sidereal = abs_rate / (kSiderealRateArcsecPerSec / 3600.0);

    int best = 0;
    double best_diff = std::abs(rate_sidereal - kSlewSpeeds[0]);
    for (int i = 1; i < kNumSlewSpeeds; ++i) {
        double diff = std::abs(rate_sidereal - kSlewSpeeds[i]);
        if (diff < best_diff) {
            best = i;
            best_diff = diff;
        }
    }
    return best;
}

} // namespace

namespace detail {

void validate_move_axis_rate(double rate) {
    // A NaN rate fails the |rate| < 1e-9 stop test and would reach rate_to_speed_index().
    util::check_move_axis_arguments(kCommandLimits, /*axis=*/0, rate);
}

}  // namespace detail

class BisqueTelescopeDriver : public TelescopeDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    BisqueTelescopeDriver(int device_number, const ConnectionInfo& connection_info,
                          std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
                          std::optional<double> site_elevation_m, util::TaskClock& clock)
        : AsyncConnectable("Bisque"),
          clock_(clock),
          device_number_(device_number),
          connection_info_(connection_info),
          site_latitude_(site_latitude_deg.value_or(0.0)),
          site_longitude_(site_longitude_deg.value_or(0.0)),
          site_elevation_m_(site_elevation_m),
          site_info_valid_(site_latitude_deg.has_value() && site_longitude_deg.has_value()) {
        guide_rate_.ra = kDefaultGuideRateDegPerSec;
        guide_rate_.dec = kDefaultGuideRateDegPerSec;
    }

    ~BisqueTelescopeDriver() override {
        // Blocks new connection tasks, then joins the in-flight one — MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        if (connected_) {
            try {
                set_connected(false);
            } catch (...) {
            }
        }
        status_poll_.cancel_all_and_join();
        pulse_guide_op_.cancel_all_and_join();
    }

    // ── AlpacaDriver base methods ──

    int get_device_number() const override {
        return device_number_;
    }

    std::string get_name() const override { return "Bisque Paramount"; }

    DeviceType get_device_type() const override {
        return DeviceType::Telescope;
    }

    std::string fallback_unique_id() const override { return "Bisque_" + std::to_string(device_number_); }

    std::string get_description() const override {
        return "Bisque Paramount / TheSkyX Mount Driver";
    }

    std::string get_driver_info() const override {
        return "AlpacaCore Bisque TheSkyX Driver v0.1";
    }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 4; }

    // A lost link (decision 0009) reads as not connected until the client's Connected=true relinks.
    // Reads published_connected_: raised only at the end of a connect, dropped when the session ends or
    // the link is declared lost, so it takes no lock set_connected() holds and stays false while
    // Connecting is true (#910). The blocking-driver list lives in async_connectable.h.
    bool get_connected() const override { return published_connected_.load(); }

    std::string get_link_fault() const override {
        std::lock_guard<std::mutex> lock(link_fault_mutex_);
        return link_fault_text_;
    }

    ALPACA_ASYNC_CONNECT_FORWARDERS()

    void set_connected(bool connected) override {
        std::lock_guard<std::mutex> transition_lock(transition_mutex_);
        std::unique_lock<std::mutex> lock(mutex_);
        // Base gates BEFORE the idempotency check: a sync disconnect during an
        // in-flight connect looks idempotent (both sides see disconnected) and
        // would be silently dropped without the record; a connect must honor a
        // newer pending disconnect by staying down.
        const bool effectively_connected = connected_ && !link_lost_;
        if (!connected && record_disconnect_if_connect_in_flight(effectively_connected)) {
            return;
        }
        if (connected && consume_pending_disconnect(effectively_connected)) {
            return;
        }
        if (connected == effectively_connected) {
            if (!connected && connected_) {
                // A disconnect of a lost link: the transport is closed already; drop the rest of the session.
                published_connected_ = false;
                connected_ = false;
                link_lost_ = false;
                // Cancels a Park poll still running, which then leaves parking_ alone: clear it here, as the other
                // cancel paths do, or the next session refuses every slew as parked (open-astro#2062).
                parking_ = false;
                motion_owner_.cancel(util::MotionOwner::kBothAxes);
                ++pulse_request_;
                pulse_generation_.bump();
                pulse_guiding_ = false;
                slewing_cached_ = false;
                slew_hold_.clear();  // decision 0022 rule 2
                reset_snapshot_state_locked();
                lock.unlock();
                status_poll_.cancel_all_and_join();
                pulse_guide_op_.cancel_all_and_join();
            }
            return;
        }

        auto& protocol = BisqueProtocolWrapper::instance();
        if (connected && connected_) {
            // Relink after a lost link: retire the dead session, then connect as a fresh one. Only the client's
            // Connected=true gets here; the driver never reopens on its own (decision 0009 point 4).
            published_connected_ = false;
            connected_ = false;
            link_lost_ = false;
            parking_ = false;  // cancels a Park poll still running, as the lost-link disconnect above does
            motion_owner_.cancel(util::MotionOwner::kBothAxes);  // (open-astro#2062)
            ++pulse_request_;
            pulse_generation_.bump();
            pulse_guiding_ = false;
            lock.unlock();
            status_poll_.cancel_all_and_join();
            pulse_guide_op_.cancel_all_and_join();
            lock.lock();
        }
        if (connected) {
            // The handshake and the first sample run under transition_mutex_ alone: every exchange takes the
            // wrapper's per-link lock, and no getter waits behind the wire (decision 0016).
            lock.unlock();
            if (!protocol.connect(connection_info_)) {
                throw AlpacaException("Failed to connect to TheSkyX at " +
                                      connection_info_.host + ":" +
                                      std::to_string(connection_info_.tcp_port));
            }
            // A handshake that throws (timeout, send failure, closed socket) must
            // release the shared wrapper too, or every later connect is refused.
            bool handshake_ok = false;
            try {
                handshake_ok = protocol.handshake();
            } catch (...) {
                protocol.disconnect();
                throw;
            }
            if (!handshake_ok) {
                protocol.disconnect();
                throw AlpacaException("TheSkyX handshake failed — is the mount connected in TheSkyX?");
            }
            // Check initial park/tracking state.
            bool parked_now = false;
            try {
                parked_now = protocol.is_parked();
            } catch (...) {  // NOLINT(bugprone-empty-catch): no answer reads as not parked
            }
            // The first frame, so the first property reads are served from memory. A TheSkyX that does not
            // answer leaves a frame with nothing known; the poll fills it in.
            PolledFrame seed;
            try {
                seed = sample_frame(seed);
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): the poll fills it in
            }

            lock.lock();
            connected_ = true;
            target_ra_set_ = false;
            target_dec_set_ = false;
            parked_ = parked_now;
            at_home_ = false;
            slewing_cached_ = false;
            slew_hold_.clear();
            slew_poll_error_.clear();
            reset_snapshot_state_locked();
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            pulse_guiding_ = false;
            pulse_guide_error_.clear();
            ++pulse_request_;
            pulse_generation_.bump();
            seed_snapshot_locked(seed);

            ALPACA_LOG_INFO("Bisque", "Connected to TheSkyX, parked=" +
                            std::string(parked_ ? "true" : "false"));
            // Raised under mutex_, so a link-loss declaration (also under mutex_) cannot be
            // overwritten by this store (decision 0009 rule 3).
            published_connected_ = true;  // the connect is complete (#910)
            lock.unlock();  // the poll slot starts without mutex_ held (AsyncOperation rule 10)
            start_status_poller();
        } else {
            ++pulse_request_;
            pulse_generation_.bump();
            published_connected_ = false;
            connected_ = false;
            target_ra_set_ = false;
            target_dec_set_ = false;
            parked_ = false;
            parking_ = false;
            // A Park poll or a FindHome still running must not set parked_ or at_home_ in a later session (#2062).
            motion_owner_.cancel(util::MotionOwner::kBothAxes);
            at_home_ = false;
            slewing_cached_ = false;
            slew_hold_.clear();  // decision 0022 rule 2
            link_lost_ = false;
            reset_snapshot_state_locked();
            manual_axis_slewing_[0] = false;
            manual_axis_slewing_[1] = false;
            pulse_guiding_ = false;
            lock.unlock();
            // State is cleared first; the close then takes the wrapper's lock with mutex_ free, so an exchange in
            // flight finishes before the socket closes and no getter waits for it.
            try {
                protocol.disconnect();
            } catch (...) {  // NOLINT(bugprone-empty-catch): state is already cleared
            }
            status_poll_.cancel_all_and_join();
            pulse_guide_op_.cancel_all_and_join();
        }
    }

    std::string command_blind(std::string_view command, bool /*raw*/) override {
        throw AlpacaException("CommandBlind not supported: " + std::string(command), AlpacaError::MethodNotImplemented);
    }

    bool command_bool(std::string_view command, bool /*raw*/) override {
        throw AlpacaException("CommandBool not supported: " + std::string(command), AlpacaError::MethodNotImplemented);
    }

    std::string command_string(std::string_view command, bool /*raw*/) override {
        throw AlpacaException("CommandString not supported: " + std::string(command),
                              AlpacaError::MethodNotImplemented);
    }

    // ── Telescope properties ──

    AlignmentMode get_alignment_mode() const override {
        return AlignmentMode::GermanPolar;
    }

    double get_altitude() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return served_altaz_locked().first;
    }

    // The optics and SlewSettleTime are the client's values, not the mount's, so a latched fault does not refuse
    // them; while disconnected they are NotConnected like every other member (ASCOM contract precedence). The optics
    // setters are not ASCOM members: the catalog and updatedevice (#2057) inject the configured values with them,
    // connected or not.
    double get_aperture_diameter() const override {
        check_connected_now();
        return aperture_diameter_m_;
    }

    void set_aperture_diameter(double meters) override { aperture_diameter_m_ = meters; }

    double get_aperture_area() const override {
        check_connected_now();
        // Derived from the atomic diameter so a live updatedevice cannot leave a stale area (#2057).
        const double d = aperture_diameter_m_;
        return d > 0.0 ? std::numbers::pi * (d / 2.0) * (d / 2.0) : 0.0;
    }

    bool get_at_home() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_link_locked();
        return at_home_;
    }

    bool get_at_park() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_link_locked();
        return parked_;
    }

    double get_azimuth() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return served_altaz_locked().second;
    }

    bool get_can_find_home() const override { return true; }
    bool get_can_park() const override { return true; }
    bool get_can_pulse_guide() const override { return true; }

    bool get_is_pulse_guiding() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_link_locked();
        if (!pulse_guide_error_.empty()) {
            throw AlpacaException(pulse_guide_error_, AlpacaError::DriverException);
        }
        return pulse_guiding_;
    }

    bool get_can_set_declination_rate() const override { return false; }
    bool get_can_set_guide_rates() const override { return true; }
    bool get_can_set_park() const override { return true; }
    bool get_can_set_pier_side() const override { return false; }
    bool get_can_set_right_ascension_rate() const override { return false; }
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
        return std::clamp(served_equatorial_locked().second, -90.0, 90.0);
    }

    double get_declination_rate() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_link_locked();
        return 0.0;
    }

    void set_declination_rate(double /*rate*/) override {
        check_connected_now();
        throw AlpacaException("Declination rate not supported", AlpacaError::PropertyNotImplemented);
    }

    bool get_tracking() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto frame = served_frame_locked();
        if (!frame.value.tracking_known) {
            throw AlpacaException("Bisque tracking state is not available: the last read failed",
                                  AlpacaError::DriverException);
        }
        return frame.value.tracking;
    }

    void set_tracking(bool tracking) override {
        check_link_now();
        auto& protocol = BisqueProtocolWrapper::instance();
        // ignore_rates=true uses sidereal. The exchange runs without mutex_.
        protocol.set_tracking(tracking, true, 0.0, 0.0);
        // Written through, so the next read sees it before the next poll runs.
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.write([tracking](PolledFrame& frame) {
            frame.tracking = tracking;
            frame.tracking_known = true;
        });
    }

    double get_focal_length() const override {
        check_connected_now();
        return focal_length_m_;
    }

    void set_focal_length(double meters) override {
        focal_length_m_ = meters;
    }

    GuideRate get_guide_rate() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_link_locked();
        return guide_rate_;
    }

    void set_guide_rate(const GuideRate& rate) override {
        util::check_guide_rate_arguments(kCommandLimits, util::GuideRateAxis::RightAscension, rate.ra);
        util::check_guide_rate_arguments(kCommandLimits, util::GuideRateAxis::Declination, rate.dec);
        std::lock_guard<std::mutex> lock(mutex_);
        util::check_guide_rate_state(kCommandLimits, GateChecks{*this, "GuideRate"});
        guide_rate_ = rate;
    }

    // One axis under one lock: the base default reads the pair through the connection-checking getter first, so an
    // out-of-range write on a disconnected mount answered NotConnected (open-astro#1786).
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
        return served_equatorial_locked().first;
    }

    double get_right_ascension_rate() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_link_locked();
        return 0.0;
    }

    void set_right_ascension_rate(double /*rate*/) override {
        check_connected_now();
        throw AlpacaException("RightAscension rate not supported", AlpacaError::PropertyNotImplemented);
    }

    int get_side_of_pier() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto frame = served_frame_locked();
        if (!frame.value.pier_known) {
            throw AlpacaException("Bisque pier side is not available: the last read failed",
                                  AlpacaError::DriverException);
        }
        return frame.value.side_of_pier;
    }

    void set_side_of_pier(int /*side*/) override {
        check_connected_now();
        throw AlpacaException("SetSideOfPier not supported", AlpacaError::PropertyNotImplemented);
    }

    int get_destination_side_of_pier(double ra, double dec) const override {
        detail::validate_ra_dec(ra, dec, "DestinationSideOfPier");
        std::lock_guard<std::mutex> lock(mutex_);
        double lst_hours = util::local_sidereal_hours(remote_utc_locked(), site_longitude_);
        double hour_angle = util::shortest_ra_delta_hours(lst_hours, ra);
        // Positive HA → west of meridian → pier east (0).
        return util::pier_side_for_hour_angle(hour_angle);
    }

    EquatorialSystem get_equatorial_system() const override {
        return EquatorialSystem::Topocentric;
    }

    // The driver applies no refraction correction, so it neither knows nor computes the flag (ITelescopeV4).
    bool get_does_refraction() const override {
        check_connected_now();
        throw AlpacaException("DoesRefraction is not implemented", AlpacaError::PropertyNotImplemented);
    }

    void set_does_refraction(bool) override {
        check_connected_now();
        throw AlpacaException("DoesRefraction is not implemented", AlpacaError::PropertyNotImplemented);
    }

    int get_slew_settle_time() const override {
        check_connected_now();
        return slew_settle_time_seconds_;
    }

    void set_slew_settle_time(int seconds) override {
        if (seconds < 0) {
            throw AlpacaException("Slew settle time must be >= 0 seconds", AlpacaError::InvalidValue);
        }
        check_connected_now();  // an ASCOM member: InvalidValue first, then NotConnected
        slew_settle_time_seconds_ = seconds;
    }

    double get_sidereal_time() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return util::local_sidereal_hours(remote_utc_locked(), site_longitude_);
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
        return site_latitude_;
    }

    void set_site_latitude(double latitude) override {
        util::validate_latitude(latitude);
        site_latitude_ = latitude;
        site_info_valid_ = true;
    }

    double get_site_longitude() const override {
        return site_longitude_;
    }

    void set_site_longitude(double longitude) override {
        util::validate_longitude(longitude);
        site_longitude_ = longitude;
        site_info_valid_ = true;
    }

    bool get_slewing() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_link_locked();
        // Not served from the frame but from the control path's own state (like AtPark): a slew start, an abort and
        // a completion decide it under mutex_. It never touches the socket.
        return get_slewing_locked();
    }

    double get_target_declination() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_link_locked();
        if (!target_dec_set_) {
            throw AlpacaException("Target declination has not been set", AlpacaError::ValueNotSet);
        }
        return target_dec_degrees_;
    }

    void set_target_declination(double dec) override {
        util::validate_dec(dec, "TargetDeclination");
        std::lock_guard<std::mutex> lock(mutex_);
        check_link_locked();
        target_dec_degrees_ = dec;
        target_dec_set_ = true;
    }

    double get_target_right_ascension() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        check_link_locked();
        if (!target_ra_set_) {
            throw AlpacaException("Target right ascension has not been set", AlpacaError::ValueNotSet);
        }
        return target_ra_hours_;
    }

    void set_target_right_ascension(double ra) override {
        util::validate_ra(ra, "TargetRightAscension");
        std::lock_guard<std::mutex> lock(mutex_);
        check_link_locked();
        target_ra_hours_ = ra;
        target_ra_set_ = true;
    }

    int get_tracking_rate() const override {
        check_link_now();
        return 0; // Sidereal
    }

    void set_tracking_rate(int rate) override {
        util::check_tracking_rate_arguments(kCommandLimits, rate);
        std::lock_guard<std::mutex> lock(mutex_);
        // Sidereal is already the default; a write still needs a live link.
        util::check_tracking_rate_state(kCommandLimits, GateChecks{*this, "TrackingRate"});
    }

    std::vector<int> get_tracking_rates() const override {
        return {0}; // driveSidereal
    }

    std::chrono::system_clock::time_point get_utc_date() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return remote_utc_locked();
    }

    void set_utc_date(std::chrono::system_clock::time_point /*utc*/) override {
        std::lock_guard<std::mutex> lock(mutex_);
        (void)served_frame_locked();
        throw AlpacaException("UTCDate writes are not supported; set the clock on the TheSkyX computer",
                              AlpacaError::PropertyNotImplemented);
    }

    // ── Telescope methods ──

    void find_home() override {
        // Counted from the gate to every exit, a throw included: PulseGuide is refused while a FindHome runs.
        struct HomeGuard {
            BisqueTelescopeDriver& self;
            ~HomeGuard() {
                std::lock_guard<std::mutex> lock(self.mutex_);
                --self.homes_in_flight_;
            }
        };
        util::MotionClaim claim;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_link_locked();
            check_not_parked("FindHome");
            claim = motion_owner_.claim(util::MotionCommand::Slew);
            ++homes_in_flight_;
        }
        const HomeGuard guard{*this};
        auto& protocol = BisqueProtocolWrapper::instance();
        // FindHome blocks in TheSkyX (with internal sleep loop), up to 60 seconds; mutex_ is free meanwhile.
        protocol.find_home();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // A MoveAxis, slew or Park claimed the axes while TheSkyX homed, or a stop cancelled the home: the mount
            // is no longer this call's to report home (#2263).
            if (motion_owner_.stop_reason(claim) != util::StopReason::None) return;
            at_home_ = true;
        }
        publish_measurement();
    }

    void park() override {
        auto& protocol = BisqueProtocolWrapper::instance();
        // Cleared on every exit; the poll below runs without initiator_mutex_, so AbortSlew never waits for it.
        struct ParkingGuard {
            BisqueTelescopeDriver& self;
            bool armed = false;
            util::MotionClaim claim;
            ~ParkingGuard() {
                if (!armed) return;
                std::lock_guard<std::mutex> lock(self.mutex_);
                // A newer Park owns the flag now, or a stop cleared it already.
                if (self.motion_owner_.stop_reason(claim) == util::StopReason::None) self.parking_ = false;
            }
        };
        ParkingGuard guard{*this, false, {}};
        {
            // Initiator lock for the gate and the Park send only: a slew either went to the wire before the park
            // or sees parking_ after it (#1838).
            std::lock_guard<std::mutex> ilock(initiator_mutex_);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                check_link_locked();
                guard.claim = motion_owner_.claim(util::MotionCommand::Slew);  // before its flag (0006 rule 3)
                guard.armed = true;
                parking_ = true;
            }
            protocol.park();
        }
        // A newer Park supersedes this one; AbortSlew, Unpark and a disconnect cancel it: either way it no longer owns
        // the outcome. A retired Park returns quietly at its next poll boundary and writes neither parked_ nor
        // parking_ (open-astro#2062), as AbortSlew already returned the Park caller promptly. A slew, a FindHome or a
        // MoveAxis cannot claim meanwhile: parking_ refuses each of them.
        const auto retired = [&] {
            std::lock_guard<std::mutex> lock(mutex_);
            return motion_owner_.stop_reason(guard.claim) != util::StopReason::None;
        };
        // Poll until parked.
        auto start = clock_.now();
        while (true) {
            if (retired()) return;
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(clock_.now() - start).count();
            if (elapsed > 120) {
                throw AlpacaException("Park timed out after 120 seconds");
            }
            try {
                if (protocol.is_parked()) break;
            } catch (...) {
            }
            clock_.sleep_for(std::chrono::seconds(1));
            if (retired()) return;
            // The mount may have been disconnected meanwhile.
            check_connected_now();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Retired while its last IsParked was on the wire.
            if (motion_owner_.stop_reason(guard.claim) != util::StopReason::None) return;
            parked_ = true;
            parking_ = false;
        }
        publish_measurement();
    }

    void pulse_guide(int direction, int duration) override {
        if (duration < 0) {  // the gate's duration text is the 30 s cap's; a negative one keeps the shared text
            throw AlpacaException(kCommandLimits.texts.pulse_direction, AlpacaError::InvalidValue);
        }
        // Cap (kCommandLimits.pulse_guide_max_ms): DirectGuide is a synchronous MoveTelescope that holds the
        // protocol wrapper (and so the socket) until TheSkyX replies, and
        // AbortSlew cannot interrupt it without interleaving replies on the
        // same socket. Real guiding pulses are milliseconds to a few seconds;
        // 30 s is well above any guide client's longest pulse, and bounds how
        // long an AbortSlew or any other command can wait behind a guide.
        util::check_pulse_guide_arguments(kCommandLimits, direction, duration);
        std::unique_lock<std::mutex> lock(mutex_);
        util::check_pulse_guide_state(kCommandLimits, GateChecks{*this, "PulseGuide"});
        if (duration == 0) {
            return;
        }
        // #2263: a slew or a home owns both axes. A Park was refused above (InvalidWhileParked). The slews, parks and
        // homes run in TheSkyX, not in a body here, so the refusal reads their own state, not slew_running().
        if (slewing_cached_ || homes_in_flight_ > 0) {
            throw AlpacaException("PulseGuide is not allowed while a slew, park or home is running",
                                  AlpacaError::InvalidOperation);
        }
        if (pulse_guiding_ && pulse_guide_error_.empty()) {
            throw AlpacaException("TheSkyX DirectGuide is already guiding; wait for IsPulseGuiding=false",
                                  AlpacaError::InvalidOperation);
        }

        // Convert direction + duration to arcsecond displacement.
        // Formula from INDI reference: displacement = guide_rate * TRACKRATE_SIDEREAL * ms / 1000.
        double ra_arcsec = 0.0;
        double dec_arcsec = 0.0;

        // Guide rates stored in degrees/sec. Convert to fraction of sidereal.
        double ra_rate_fraction = guide_rate_.ra / (kSiderealRateArcsecPerSec / 3600.0);
        double dec_rate_fraction = guide_rate_.dec / (kSiderealRateArcsecPerSec / 3600.0);

        switch (direction) {
            case 0: // North
                dec_arcsec = dec_rate_fraction * kSiderealRateArcsecPerSec * duration / 1000.0;
                break;
            case 1: // South
                dec_arcsec = -dec_rate_fraction * kSiderealRateArcsecPerSec * duration / 1000.0;
                break;
            case 2: // East
                ra_arcsec = ra_rate_fraction * kSiderealRateArcsecPerSec * duration / 1000.0;
                break;
            case 3: // West
                ra_arcsec = -ra_rate_fraction * kSiderealRateArcsecPerSec * duration / 1000.0;
                break;
            default:
                return;  // validated above
        }

        pulse_guiding_ = true;
        pulse_guide_error_.clear();
        // The request id is taken under mutex_ with the state it guards; abort,
        // connect and disconnect bump it, so the body compares it under mutex_
        // and a guide they retired neither sends nor records anything.
        const uint64_t request = ++pulse_request_;
        // MoveTelescope runs at TheSkyX's fixed speed, so its run time only
        // roughly tracks `duration`; never wait less than the configured bound.
        constexpr int kResponseMarginMs = 1000;
        const int duration_bound_ms = duration + kResponseMarginMs;  // no overflow: duration <= pulse_guide_max_ms
        const int guide_timeout_ms = std::max(connection_info_.response_timeout_ms, duration_bound_ms);
        // The slot's start() runs without the driver mutex (a stale body needs
        // it to return). A guide retired meanwhile (AbortSlew, disconnect) is
        // told apart by `request`, not by the slot's generation, which start()
        // bumps again after the retiring call.
        lock.unlock();
        try {
            pulse_guide_op_.start(
                [this, request, ra_arcsec, dec_arcsec, guide_timeout_ms](util::OperationContext& ctx) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (!connected_ || ctx.stop_reason() != util::StopReason::None || request != pulse_request_) {
                            // Two start() calls can run in the opposite order
                            // to their requests, so a body the slot stopped
                            // while its request is current retires it here.
                            if (request == pulse_request_) {
                                pulse_guiding_ = false;
                            }
                            return;
                        }
                    }
                    std::string error;
                    try {
                        BisqueProtocolWrapper::instance().guide(ra_arcsec, dec_arcsec, guide_timeout_ms);
                    } catch (const std::exception& e) {
                        error = std::string("PulseGuide failed: ") + e.what();
                    } catch (...) {
                        error = "PulseGuide failed with an unknown TheSkyX error";
                    }
                    // The request check and the state write share one mutex_ hold.
                    std::unique_lock<std::mutex> lock(mutex_);
                    if (ctx.stop_reason() != util::StopReason::None || request != pulse_request_) {
                        if (request == pulse_request_) {
                            pulse_guiding_ = false;
                        }
                        return;
                    }
                    if (!error.empty()) {
                        // pulse_guiding_ stays true; IsPulseGuiding reports the
                        // error until the next PulseGuide, abort or connect.
                        pulse_guide_error_ = std::move(error);
                        return;
                    }
                    // The pulse moved the mount: read where it ended up. The sample is four TheSkyX exchanges,
                    // so it runs without mutex_ (every getter takes it). A write that lands meanwhile (an
                    // abort, a slew) is newer than the token and drops the publish.
                    const auto token = snapshot_.begin_poll();
                    lock.unlock();
                    const auto measured = measure_frame();
                    lock.lock();
                    if (ctx.stop_reason() != util::StopReason::None || request != pulse_request_) {
                        if (request == pulse_request_) {
                            pulse_guiding_ = false;
                        }
                        return;
                    }
                    pulse_guiding_ = false;
                    publish_measured_locked(token, measured);
                });
        } catch (...) {
            std::lock_guard<std::mutex> relock(mutex_);
            if (request == pulse_request_) {
                pulse_guiding_ = false;
            }
            throw;
        }
    }

    void set_park() override {
        check_link_now();
        auto& protocol = BisqueProtocolWrapper::instance();
        protocol.set_park_position();
    }

    void slew_to_coordinates(double ra, double dec) override {
        // #627: argument validation precedes the connection check.
        util::check_equatorial_slew_arguments(ra, dec, "SlewToCoordinates");
        // Initiator lock: the gate, the command and the state write happen in the order of the commands on the
        // wire, so an AbortSlew cannot slip between them. Released before the wait loop.
        std::unique_lock<std::mutex> ilock(initiator_mutex_);
        std::unique_lock<std::mutex> lock(mutex_);
        util::check_equatorial_slew_state(kCommandLimits, GateChecks{*this, "SlewToCoordinates", &lock});
        // mutex_ was released for the tracking read: a disconnect or a park may have landed meanwhile (#1838).
        check_link_locked();
        check_not_parked("SlewToCoordinates");

        auto& protocol = BisqueProtocolWrapper::instance();
        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_ra_set_ = true;
        target_dec_set_ = true;

        // Decision 0022 rule 2: Slewing is true from before the command goes out.
        const SlewStart start = begin_slew_locked();
        lock.unlock();
        try {
            protocol.slew_to_ra_dec(ra, dec);
        } catch (...) {
            lock.lock();
            undo_failed_slew_locked(start);
            throw;
        }
        lock.lock();
        ilock.unlock();

        // Wait for completion.
        const SlewWaitGuard waiting(slew_waiters_);
        wait_for_slew_complete(lock, start.claim);
    }

    void slew_to_coordinates_async(double ra, double dec) override {
        // #627: argument validation precedes the connection check.
        util::check_equatorial_slew_arguments(ra, dec, "SlewToCoordinatesAsync");
        std::lock_guard<std::mutex> ilock(initiator_mutex_);  // see slew_to_coordinates()
        std::unique_lock<std::mutex> lock(mutex_);
        util::check_equatorial_slew_state(kCommandLimits, GateChecks{*this, "SlewToCoordinatesAsync", &lock});
        // See slew_to_coordinates().
        check_link_locked();
        check_not_parked("SlewToCoordinatesAsync");

        auto& protocol = BisqueProtocolWrapper::instance();
        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_ra_set_ = true;
        target_dec_set_ = true;

        const SlewStart start = begin_slew_locked();  // see slew_to_coordinates()
        lock.unlock();
        try {
            protocol.slew_to_ra_dec(ra, dec);
        } catch (...) {
            lock.lock();
            undo_failed_slew_locked(start);
            throw;
        }
    }

    void slew_to_target() override {
        double ra, dec;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_link_locked();  // before the unset target (#2075)
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
            check_link_locked();  // before the unset target (#2075)
            if (!target_ra_set_ || !target_dec_set_) {
                throw AlpacaException("Target coordinates have not been set", AlpacaError::ValueNotSet);
            }
            ra = target_ra_hours_;
            dec = target_dec_degrees_;
        }
        slew_to_coordinates_async(ra, dec);
    }

    void sync_to_coordinates(double ra, double dec) override {
        // #627: argument validation precedes the connection check.
        detail::validate_ra_dec(ra, dec, "SyncToCoordinates");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_link_locked();
            check_not_parked("SyncToCoordinates");
        }

        auto& protocol = BisqueProtocolWrapper::instance();
        protocol.sync_to_coordinates(ra, dec);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            target_ra_hours_ = ra;
            target_dec_degrees_ = dec;
            target_ra_set_ = true;
            target_dec_set_ = true;
        }
        publish_measurement();
    }

    void sync_to_target() override {
        double ra, dec;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            check_link_locked();  // before the unset target (#2075)
            if (!target_ra_set_ || !target_dec_set_) {
                throw AlpacaException("Target coordinates have not been set", AlpacaError::ValueNotSet);
            }
            ra = target_ra_hours_;
            dec = target_dec_degrees_;
        }
        sync_to_coordinates(ra, dec);
    }

    void unpark() override {
        check_link_now();
        auto& protocol = BisqueProtocolWrapper::instance();
        protocol.unpark();
        // Confirm unpark.
        if (protocol.is_parked()) {
            throw AlpacaException("Failed to unpark mount");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        parked_ = false;
        parking_ = false;  // a park poll still running no longer blocks slews
        // and no longer sets parked_ when the mount reports parked (open-astro#2062)
        motion_owner_.cancel(util::MotionOwner::kBothAxes);
    }

    bool get_can_move_axis(int axis) const override {
        if (axis < 0 || axis > 2) {
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }
        return axis == 0 || axis == 1;
    }

    void move_axis(int axis, double rate) override {
        // #627: argument validation precedes the connection check.
        if (axis < 0 || axis > 1) {  // the axis in the text, as get_axis_rate_range() words it
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }
        util::check_move_axis_arguments(kCommandLimits, axis, rate);
        const bool stop = std::abs(rate) < 1e-9;

        // Initiator lock: the exchange and the state write stay in wire order against AbortSlew.
        std::lock_guard<std::mutex> ilock(initiator_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            util::check_move_axis_state(kCommandLimits, stop, GateChecks{*this, "MoveAxis"});
            // #2263: a move claims its own axis, so a FindHome in flight loses it; a stop cancels its own axis. A
            // Park refuses both above, and a slew's Slewing stays the slew's own (slew_claim_) until TheSkyX reports
            // it complete. MoveAxis runs no body, so the claim is not kept.
            if (stop) {
                motion_owner_.cancel(util::MotionOwner::axes_of(util::MotionCommand::MoveAxis, axis));
            } else {
                static_cast<void>(motion_owner_.claim(util::MotionCommand::MoveAxis, axis));
            }
        }

        auto& protocol = BisqueProtocolWrapper::instance();

        if (stop) {
            // Stop motion.
            protocol.stop_open_loop_motion();
            std::lock_guard<std::mutex> lock(mutex_);
            manual_axis_slewing_[axis] = false;
            drop_poll_in_flight_locked();
            return;
        }

        int speed_index = rate_to_speed_index(rate);

        // Map axis + sign to direction.
        // Axis 0 (Primary/RA): positive=East(2), negative=West(3)
        // Axis 1 (Secondary/Dec): positive=North(0), negative=South(1)
        int direction;
        if (axis == 0) {
            direction = rate > 0 ? 2 : 3; // East or West
        } else {
            direction = rate > 0 ? 0 : 1; // North or South
        }

        protocol.start_open_loop_motion(direction, speed_index);
        std::lock_guard<std::mutex> lock(mutex_);
        manual_axis_slewing_[axis] = true;
        drop_poll_in_flight_locked();
    }

    std::pair<double, double> get_axis_rate_range(int axis) const override {
        if (axis < 0 || axis > 1) {
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }
        return {0.0, kMaxMoveAxisRateDegPerSec};
    }

    std::vector<std::pair<double, double>> get_axis_rate_ranges(int axis) const override {
        if (axis == 2) return {};
        return {get_axis_rate_range(axis)};
    }

    void abort_slew() override {
        // Initiator lock: the state clear and the command follow any initiator whose command is on the wire.
        std::lock_guard<std::mutex> ilock(initiator_mutex_);
        std::unique_lock<std::mutex> lock(mutex_);
        // Only a lost link refuses: a latched fault still lets the stop try, as OnStep and SynScan do (#2102).
        check_connected();
        // Cancels a Park poll (it no longer sets parked_ when the mount reports parked, open-astro#2062) and a
        // FindHome; and no SlewToCoordinates wait owns Slewing any more.
        motion_owner_.cancel(util::MotionOwner::kBothAxes);
        slew_claim_ = {};
        parking_ = false;  // a park poll still running no longer blocks slews
        // Bumping the request id retires a running guide body: under mutex_ it
        // sends and records nothing. It is not cancelled or joined here, because a PulseGuide
        // started while the abort waits on the wrapper owns the slot by then.
        ++pulse_request_;
        pulse_generation_.bump();
        pulse_guide_error_.clear();
        pulse_guiding_ = false;
        slewing_cached_ = false;
        slew_poll_error_.clear();
        slew_hold_.clear();
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
        drop_poll_in_flight_locked();
        // Release the driver mutex before the abort: protocol.abort() waits on
        // the wrapper mutex, which a pending pulse guide holds until TheSkyX
        // replies, and getters must not stall behind that wait. The abort is
        // sent only after the guide reply, never on the busy socket.
        lock.unlock();
        std::exception_ptr error;
        try {
            BisqueProtocolWrapper::instance().abort();
        } catch (...) {
            error = std::current_exception();
        }
        if (error) {
            std::rethrow_exception(error);
        }
    }

    void slew_to_alt_az(double /*altitude*/, double /*azimuth*/) override {
        check_connected_now();
        throw AlpacaException("SlewToAltAz not supported", AlpacaError::MethodNotImplemented);
    }

    void slew_to_alt_az_async(double /*altitude*/, double /*azimuth*/) override {
        check_connected_now();
        throw AlpacaException("SlewToAltAzAsync not supported", AlpacaError::MethodNotImplemented);
    }

    void sync_to_alt_az(double /*altitude*/, double /*azimuth*/) override {
        check_connected_now();
        throw AlpacaException("SyncToAltAz not supported", AlpacaError::MethodNotImplemented);
    }

    // Test seams of the status poll (decision 0010): stop or resume its ticks, and run one tick on the calling
    // thread. A paused poll joins its slot, so it leaves no waiter on the task clock either.
    void set_status_poll_paused_for_testing(bool paused) {
        status_poll_paused_for_testing_ = paused;
        if (paused) {
            status_poll_.cancel_all_and_join();
        } else {
            start_status_poller();
        }
    }

    // One full tick on the calling thread, paused or not. False when the connection is gone.
    bool poll_status_now_for_testing() {
        const bool alive = poll_status_once(nullptr);
        if (close_lost_transport_.exchange(false)) {
            BisqueProtocolWrapper::instance().disconnect();
        }
        return alive;
    }

    void set_slew_complete_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> lock(test_hook_mutex_);
        slew_complete_hook_ = std::move(hook);
    }

    bool slew_hold_active_for_testing() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return slew_hold_.active(clock_.now());
    }

    bool slew_hold_cleared_for_testing() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return slew_hold_.until() == util::HoldWindow::mono::min();
    }

private:
    void check_connected() const {
        if (!connected_ || link_lost_) {
            throw AlpacaException("Not connected to TheSkyX", AlpacaError::NotConnected);
        }
    }

    // A lost link (NotConnected) or a latched fault (DriverException, Connected stays true, decision 0009 point 1).
    void check_link_locked() const {
        check_connected();
        check_no_fault_locked();
    }

    void check_no_fault_locked() const {
        if (position_link_health_.faulted()) {
            throw AlpacaException("Bisque mount communications compromised: " + position_link_health_.fault(),
                                  AlpacaError::DriverException);
        }
    }

    // The command gate's state checks in Bisque's words (#2258): util/telescope_command_gate.h decides the order,
    // these answer each step with the driver's own texts and reads. `lock` is the held mutex_ a slew's Tracking check
    // releases for its TheSkyX read; the other families never call that check.
    struct GateChecks {
        BisqueTelescopeDriver& driver;
        const char* who = nullptr;
        std::unique_lock<std::mutex>* lock = nullptr;

        void check_connected() const { driver.check_connected(); }
        void check_not_parked(bool /*stop*/) const { driver.check_not_parked(who); }
        void check_no_fault() const { driver.check_no_fault_locked(); }
        void check_tracking() const { driver.check_tracking_for_slew(*lock, who); }
    };

    void check_link_now() const {
        std::lock_guard<std::mutex> lock(mutex_);
        check_link_locked();
    }

    void check_connected_now() const {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected();
    }

    void check_not_parked(const char* operation) const {
        if (parked_ || parking_) {
            throw AlpacaException(std::string(operation) + " is not allowed while parked",
                                  AlpacaError::InvalidWhileParked);
        }
    }

    // ── Snapshot-served state (decision 0010, Bisque adoption) ──────────────

    // The TheSkyX computer's UTC now, from the polled frame. UTCDate, SiderealTime and DestinationSideOfPier share it.
    std::chrono::system_clock::time_point remote_utc_locked() const {
        const auto frame = served_frame_locked();
        if (const auto anchor = frame.value.utc.current(frame.value.utc_sampled_at)) {
            const auto elapsed = clock_.now() - frame.value.utc_sampled_at;
            // Remote input can be near system_clock's limit; UtcOffset assumes ordinary dates.
            if (anchor->time_since_epoch().count() > 0 &&
                elapsed > std::chrono::system_clock::time_point::max() - *anchor) {
                throw AlpacaException("TheSkyX UTCDate exceeds the supported time range", AlpacaError::DriverException);
            }
            return *anchor + std::chrono::duration_cast<std::chrono::system_clock::duration>(elapsed);
        }
        throw AlpacaException(
            frame.value.utc_error.empty() ? "TheSkyX UTCDate has not been established" : frame.value.utc_error,
            frame.value.utc_error_code);
    }

    // The frame a getter serves, or the refusal that applies: a lost link (NotConnected), a latched fault
    // (DriverException "communications compromised", Connected stays true, decision 0009 point 1), no frame yet.
    // A frame past kSnapshotMaxAge is served as it is: stale is not lost.
    util::StateSnapshot<PolledFrame>::Reading served_frame_locked() const {
        check_link_locked();
        auto reading = snapshot_.read();
        if (!reading) {
            throw AlpacaException(
                "Bisque mount state is not available yet: the first status poll has not been answered",
                AlpacaError::DriverException);
        }
        return *reading;
    }

    // {RA hours, Dec degrees}
    std::pair<double, double> served_equatorial_locked() const {
        const auto frame = served_frame_locked();
        if (!frame.value.equatorial_known) {
            throw AlpacaException("Bisque mount position is not available: the last read failed",
                                  AlpacaError::DriverException);
        }
        return {frame.value.ra_hours, frame.value.dec_degrees};
    }

    // {altitude, azimuth}, degrees
    std::pair<double, double> served_altaz_locked() const {
        const auto frame = served_frame_locked();
        if (!frame.value.altaz_known) {
            throw AlpacaException("Bisque mount alt/az is not available: the last read failed",
                                  AlpacaError::DriverException);
        }
        return {frame.value.alt_degrees, frame.value.az_degrees};
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

    // A write that changed what the mount's position means (a slew start, a move): drops the poll in flight,
    // so its older sample cannot overwrite what the control path knows. The next poll publishes.
    void drop_poll_in_flight_locked() const {
        snapshot_.write([](PolledFrame&) {});
    }

    void reset_snapshot_state_locked() {
        snapshot_.reset();
        position_link_health_.reset();
        slew_poll_error_.clear();
        publish_link_fault_locked();
    }

    void note_slew_started_locked() {
        slewing_cached_ = true;
        slew_poll_error_.clear();
        slew_hold_.arm(clock_.now(), std::chrono::seconds(8));
        drop_poll_in_flight_locked();
    }

    // The slew state a SlewToRaDec replaces (decision 0022 rule 2). The window and Slewing are armed before the
    // command goes out; a command TheSkyX refused started nothing, so undo_failed_slew_locked() puts back what an
    // accepted earlier slew had: its window, its Slewing, its kept poll error and its slew_claim_, so a
    // SlewToCoordinates still waiting for that slew lowers Slewing when it lands. The owner's own claim cannot be
    // handed back (its generations only grow), which is why slew_claim_ is kept apart from it. The caller holds
    // initiator_mutex_ from the arm to the undo, as every slew_claim_ writer does, so no other slew start or abort
    // lands in between.
    struct SlewStart {
        util::HoldWindow hold;
        bool slewing = false;
        std::string poll_error;
        int poll_error_code = AlpacaError::DriverException;
        util::MotionClaim claim_before;
        util::MotionClaim claim;  // this slew's, once it is armed
    };

    // A slew claims both axes (#2263) before it sets its flags (decision 0006 rule 3).
    SlewStart begin_slew_locked() {
        SlewStart start{slew_hold_, slewing_cached_, slew_poll_error_, slew_poll_error_code_, slew_claim_, {}};
        slew_claim_ = motion_owner_.claim(util::MotionCommand::Slew);
        start.claim = slew_claim_;
        note_slew_started_locked();
        return start;
    }

    // True while `claim` is the slew whose Slewing flag slewing_cached_ is.
    bool owns_slewing_locked(const util::MotionClaim& claim) const {
        return slew_claim_.generation == claim.generation;
    }

    // Unless a disconnect, a connect or a landing has lowered Slewing since the arm: the state those wrote stands.
    void undo_failed_slew_locked(const SlewStart& start) {
        if (!owns_slewing_locked(start.claim) || !slewing_cached_) {
            return;
        }
        slew_hold_ = start.hold;
        slewing_cached_ = start.slewing;
        slew_poll_error_ = start.poll_error;
        slew_poll_error_code_ = start.poll_error_code;
        slew_claim_ = start.claim_before;
    }

    // One sample, taken without mutex_ (the protocol wrapper serialises each exchange). The position is
    // mandatory, so a failed read throws and the tick ends there; the other fields keep the last value when
    // their own read fails. An explicit unavailable UTC mode clears its anchor. `previous` is the frame refined.
    PolledFrame sample_frame(const PolledFrame& previous) const {
        auto& protocol = BisqueProtocolWrapper::instance();
        PolledFrame frame = previous;
        const auto pos = protocol.get_ra_dec();
        frame.ra_hours = pos.ra_hours;
        frame.dec_degrees = pos.dec_degrees;
        frame.equatorial_known = true;
        frame.utc_error.clear();
        try {
            const auto utc = protocol.get_utc_date();
            frame.utc_sampled_at = clock_.now();  // Stamp this reply, not the end of the remaining telemetry queries.
            frame.utc.set(utc, frame.utc_sampled_at);
        } catch (const AlpacaException& e) {
            if (e.error_code() == AlpacaError::InvalidOperation) {
                frame.utc.clear();
            }
            frame.utc_error = e.what();
            frame.utc_error_code = e.error_code();
        } catch (const std::exception& e) {
            frame.utc_error = e.what();
            frame.utc_error_code = AlpacaError::DriverException;
        }
        try {
            const auto altaz = protocol.get_alt_az();
            frame.alt_degrees = altaz.altitude_degrees;
            frame.az_degrees = altaz.azimuth_degrees;
            frame.altaz_known = true;
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): keeps the last value
        }
        try {
            frame.tracking = protocol.is_tracking();
            frame.tracking_known = true;
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): keeps the last value
        }
        try {
            // TheSkyX: 1 = west side of pier, else = east.
            // Alpaca: 0 = pierEast (OTA west of pier), 1 = pierWest (OTA east of pier).
            frame.side_of_pier = protocol.get_pier_side() == 1 ? 1 : 0;
            frame.pier_known = true;
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): keeps the last value
        }
        return frame;
    }

    // Publishes the first frame, sampled by the connect before it took mutex_, so the getters have an answer the
    // moment Connect returns.
    void seed_snapshot_locked(const PolledFrame& frame) { snapshot_.publish(snapshot_.begin_poll(), frame); }

    // Where the control path ends or lands a move (sync, slew complete, home, park): reads the mount now and
    // publishes the frame, so a read right after sees the new position, not one from before the move. A failed
    // read leaves the position unknown, which the getters refuse until the next poll answers.
    // Call without mutex_: the token is taken under it, the sample runs without it (every getter takes it), and the
    // publish is under it again. A write since the token (an abort, a slew) drops the publish; a disconnect since
    // drops it too.
    void publish_measurement() const {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto token = snapshot_.begin_poll();
        lock.unlock();
        const auto measured = measure_frame();
        lock.lock();
        if (!connected_) {
            return;
        }
        publish_measured_locked(token, measured);
    }

    // The sample half of publish_measurement(), callable without mutex_. nullopt: the position read failed.
    std::optional<PolledFrame> measure_frame() const {
        try {
            const auto previous = snapshot_.read();
            return sample_frame(previous ? previous->value : PolledFrame{});
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    // The publish half: `token` was taken before the sample, so a write since drops it.
    void publish_measured_locked(const util::StateSnapshot<PolledFrame>::Token& token,
                                 const std::optional<PolledFrame>& measured) const {
        if (measured) {
            snapshot_.publish(token, *measured);
            drop_poll_in_flight_locked();  // a poll that sampled before this read must not replace it
        } else {
            snapshot_.write([](PolledFrame& frame) {
                frame.equatorial_known = false;
                frame.altaz_known = false;
            });
        }
    }

    // True when the poll should ask TheSkyX whether an async slew is complete: one is in flight, its force window
    // is over, and no control-path wait (SlewToCoordinates) is polling it already.
    bool slew_poll_due_locked() const {
        return slewing_cached_ && !slew_hold_.active(clock_.now()) && slew_waiters_ == 0 &&
               !(manual_axis_slewing_[0] || manual_axis_slewing_[1]);
    }

    struct SlewWaitGuard {
        explicit SlewWaitGuard(std::atomic<int>& count) : count_(count) { ++count_; }
        ~SlewWaitGuard() { --count_; }
        SlewWaitGuard(const SlewWaitGuard&) = delete;
        SlewWaitGuard& operator=(const SlewWaitGuard&) = delete;

    private:
        std::atomic<int>& count_;
    };

    void publish_link_fault_locked() const {
        std::lock_guard<std::mutex> lock(link_fault_mutex_);
        link_fault_text_ = position_link_health_.fault();
    }

    // One poll tick. The exchanges run WITHOUT mutex_, so a getter never waits behind a slow link and the snapshot
    // mutex is never held across I/O; the result is published under mutex_. Returns false when the connection is
    // gone or lost and the poll body should end. `ctx` is null for the test seam's direct tick, which ignores the
    // pause.
    bool poll_status_once(util::OperationContext* ctx) {
        const auto stopped = [ctx] { return ctx != nullptr && ctx->stop_reason() != util::StopReason::None; };
        if (ctx != nullptr && status_poll_paused_for_testing_.load()) {
            return true;
        }
        bool ask_slew = false;
        util::StateSnapshot<PolledFrame>::Token token;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_ || link_lost_ || stopped()) {
                return false;
            }
            ask_slew = slew_poll_due_locked();
            // Taken under the same mutex_ hold that decided ask_slew: a write (a retarget, an abort) that lands
            // after this point is newer than the token and drops the publish, and one before it is already in
            // the state slew_poll_due_locked() read.
            token = snapshot_.begin_poll();
        }
        // The completion flag is read BEFORE the position, so a frame that follows "complete" carries a position
        // measured after the stop.
        bool slew_complete = false;
        std::string slew_error;
        int slew_error_code = AlpacaError::DriverException;
        if (ask_slew) {
            try {
                slew_complete = BisqueProtocolWrapper::instance().is_slew_complete();
            } catch (const AlpacaException& ex) {
                slew_error = ex.what();
                slew_error_code = ex.error_code();
            } catch (const std::exception& ex) {
                slew_error = ex.what();
            }
        }
        const auto held = snapshot_.read();
        PolledFrame frame;
        try {
            frame = sample_frame(held ? held->value : PolledFrame{});
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected_ || link_lost_ || stopped()) {
                return false;
            }
            note_position_failure_locked(e);
            return !declare_link_lost_if_stale_locked();
        }
        std::unique_lock<std::mutex> lock(mutex_);
        if (!connected_ || link_lost_ || stopped()) {
            return false;
        }
        // A reply, even one the snapshot drops because a write landed since the sample, proves the link.
        if (position_link_health_.on_reply()) {
            ALPACA_LOG_INFO("Bisque", "Position link recovered; mount readback is available again");
        }
        publish_link_fault_locked();
        // The slew outcome applies only with its frame: a write since the sample (a new slew, an abort) makes
        // the publish fail, and then the answer is about a slew that is no longer the current one.
        if (snapshot_.publish(token, frame) && ask_slew && slewing_cached_) {
            if (!slew_error.empty()) {
                slew_poll_error_ = slew_error;
                slew_poll_error_code_ = slew_error_code;
            } else {
                slew_poll_error_.clear();
                if (slew_complete) {
                    slewing_cached_ = false;
                }
            }
        }
        return true;
    }

    void note_position_failure_locked(const std::exception& e) {
        if (auto fault = position_link_health_.note_failure(e.what(), kPositionLinkFailureThreshold, clock_.now())) {
            ALPACA_LOG_ERROR("Bisque", "Position link faulted: " + *fault);
        } else {
            ALPACA_LOG_WARN("Bisque", "Position read failed (" +
                                          std::to_string(position_link_health_.consecutive_failures()) +
                                          " consecutive failures): " + e.what());
        }
        publish_link_fault_locked();
    }

    // Decision 0009 point 2(b): a fault that has stood past the bound is a LOST link. Connected reads false, the
    // getters and setters throw NotConnected, DeviceState is empty, the transport is closed; only the client's
    // Connected=true relinks. Caller holds mutex_ (the transport close waits on the wrapper, which a pending guide
    // may hold, so it happens in the poll body after the lock is released). True when the link was just declared
    // lost.
    bool declare_link_lost_if_stale_locked() {
        if (!position_link_health_.fault_stale(clock_.now())) {
            return false;
        }
        ALPACA_LOG_ERROR(
            "Bisque",
            "Link to TheSkyX lost after " +
                std::to_string(std::chrono::duration_cast<std::chrono::seconds>(util::kLinkStalenessBound).count()) +
                " s without a reply; reconnect to resume (" + position_link_health_.fault() + ")");
        link_lost_ = true;
        published_connected_ = false;
        close_lost_transport_ = true;
        return true;
    }

    void run_status_poller(util::OperationContext& ctx) {
        // The first frame was seeded at connect: wait one period before the first tick.
        if (!ctx.wait_for(kStatusPollPeriod)) {
            return;
        }
        for (;;) {
            const auto started = clock_.now();
            const bool alive = poll_status_once(&ctx);
            if (close_lost_transport_.exchange(false)) {
                try {
                    BisqueProtocolWrapper::instance().disconnect();
                } catch (...) {  // NOLINT(bugprone-empty-catch): best effort, the link is lost already
                }
            }
            if (!alive) {
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

    // Call without mutex_ held (AsyncOperation rule 10), after a connect.
    void start_status_poller() {
        if (status_poll_paused_for_testing_.load()) {
            return;
        }
        try {
            status_poll_.start([this](util::OperationContext& ctx) { run_status_poller(ctx); });
        } catch (const std::exception& e) {
            ALPACA_LOG_ERROR("Bisque", std::string("Status poll did not start: ") + e.what());
        }
    }

    bool get_slewing_locked() const {
        // Within the slew-force window, report as slewing without querying.
        if (slewing_cached_ && slew_hold_.active(clock_.now())) {
            return true;
        }
        // Check manual axis motion.
        if (manual_axis_slewing_[0] || manual_axis_slewing_[1]) {
            return true;
        }
        if (!slewing_cached_) {
            return false;
        }
        // Past the window the poll asks TheSkyX and clears slewing_cached_ when the slew is complete. A failed
        // completion poll is not evidence that the slew completed: keep reporting in progress and surface the
        // protocol error until a reply clears it.
        if (!slew_poll_error_.empty()) {
            throw AlpacaException(slew_poll_error_, slew_poll_error_code_);
        }
        return true;
    }

    // ITelescopeV4: an equatorial slew with Tracking false is InvalidOperation
    // (open-astro#1274). The gate asks TheSkyX itself (a failed query falls back to the frame's value) and writes the
    // answer through to the snapshot, so the Tracking getter agrees with it at once; it does not read the frame, which
    // can be a poll period old and would refuse a slew right after the client switched Tracking on. A retarget of this
    // driver's own slew still in flight is not refused: the TheSkyX docs do not say what IsTracking reads during a
    // GOTO. Called with `lock` held; it releases it across each exchange and returns with it held again.
    void check_tracking_for_slew(std::unique_lock<std::mutex>& lock, const char* who) {
        if (own_slew_in_flight(lock)) {
            return;
        }
        if (!get_tracking_live(lock)) {
            throw AlpacaException(std::string(who) + " requires Tracking to be true", AlpacaError::InvalidOperation);
        }
    }

    // slewing_cached_ alone is not enough: it stays set until a poll tick or a wait loop sees the slew complete,
    // so ask TheSkyX past the force window. A failed
    // poll proves nothing either way; the IsTracking read then decides. Lock handling as check_tracking_for_slew().
    bool own_slew_in_flight(std::unique_lock<std::mutex>& lock) {
        if (!slewing_cached_) {
            return false;
        }
        if (slew_hold_.active(clock_.now())) {
            return true;
        }
        bool complete = false;
        lock.unlock();
        try {
            complete = BisqueProtocolWrapper::instance().is_slew_complete();
        } catch (const std::exception&) {
            lock.lock();
            return false;  // keep slewing_cached_: a failed poll is not completion
        }
        lock.lock();
        check_connected();
        if (!complete) {
            return true;
        }
        // No slew start or abort wrote meanwhile: both callers hold initiator_mutex_, as every slew_claim_ writer does.
        // No measurement here: an initiator is timed against the 1 s target, and a retarget starts a new slew at
        // once. Drop the tick in flight; the next poll publishes the position.
        drop_poll_in_flight_locked();
        slewing_cached_ = false;
        slew_poll_error_.clear();
        return false;
    }

    // A fresh read for the slew gate (a control-path call may talk to TheSkyX, and a Tracking the client just
    // switched on must not be refused on a frame up to a poll period old). Written through, so the getter agrees.
    // Lock handling as check_tracking_for_slew().
    bool get_tracking_live(std::unique_lock<std::mutex>& lock) {
        lock.unlock();
        try {
            const bool tracking = BisqueProtocolWrapper::instance().is_tracking();
            lock.lock();
            snapshot_.write([tracking](PolledFrame& frame) {
                frame.tracking = tracking;
                frame.tracking_known = true;
            });
            return tracking;
        } catch (...) {
            if (!lock.owns_lock()) {
                lock.lock();
            }
            // A failed read keeps what the Tracking getter would answer: the frame's last value.
            const auto held = snapshot_.read();
            return held && held->value.tracking_known && held->value.tracking;
        }
    }

    // Called with `lock` held; each exchange runs with it released, so a getter never waits for a poll of TheSkyX.
    // `claim` is this slew's: a newer slew start or an abort since keeps the flag it wrote.
    void wait_for_slew_complete(std::unique_lock<std::mutex>& lock, const util::MotionClaim& claim) {
        // One failed poll is tolerated (a transient TheSkyX hiccup); this many in a
        // row is a persistent fault and is reported as such, not as a 120 s timeout.
        constexpr int kMaxConsecutivePollFailures = 3;
        int consecutive_failures = 0;
        auto start = clock_.now();
        while (true) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(clock_.now() - start).count();
            if (elapsed > 120) {
                throw AlpacaException("Slew timed out after 120 seconds");
            }
            int failure_code = AlpacaError::DriverException;
            std::string failure_what;
            bool complete = false;
            lock.unlock();
            try {
                complete = BisqueProtocolWrapper::instance().is_slew_complete();
                consecutive_failures = 0;
            } catch (const AlpacaException& ex) {
                // A racing disconnect stays NotConnected.
                if (ex.error_code() == AlpacaError::NotConnected) {
                    throw;
                }
                failure_code = ex.error_code();
                failure_what = ex.what();
                ++consecutive_failures;
            } catch (const std::exception& ex) {
                failure_what = ex.what();
                ++consecutive_failures;
            } catch (...) {
                failure_what = "unknown error";
                ++consecutive_failures;
            }
            if (complete) {
                // The position read after the stop is published first; Slewing clears in the next mutex_ hold.
                publish_measurement();
                run_slew_complete_hook();  // test seam: a retarget lands before the outcome is applied
                lock.lock();
                if (owns_slewing_locked(claim)) {  // a retarget or abort since this slew started owns the flag
                    slewing_cached_ = false;
                    slew_hold_.clear();  // landed: decision 0022 rule 2
                }
                if (slew_settle_time_seconds_ > 0) {
                    lock.unlock();
                    clock_.sleep_for(std::chrono::seconds(slew_settle_time_seconds_));
                    lock.lock();
                    check_connected();
                }
                return;
            }
            if (consecutive_failures >= kMaxConsecutivePollFailures) {
                // slewing_cached_ stays set: a failed poll is not evidence of arrival.
                throw AlpacaException("Slew completion poll failed " + std::to_string(consecutive_failures) +
                                          " times in a row: " + failure_what,
                                      failure_code);
            }
            clock_.sleep_for(std::chrono::seconds(1));
            lock.lock();
            // The mount may have been disconnected while the lock was released.
            check_connected();
        }
    }

    void run_slew_complete_hook() {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(test_hook_mutex_);
            hook = slew_complete_hook_;
        }
        if (hook) {
            hook();
        }
    }

    util::TaskClock& clock_;
    int device_number_;
    ConnectionInfo connection_info_;
    std::mutex transition_mutex_;
    // Serialises the motion initiators (MoveAxis, the slew forms) with AbortSlew across gate, exchange and state
    // write. Order: initiator_mutex_, the wrapper's per-link lock, mutex_ (leaf).
    std::mutex initiator_mutex_;
    mutable std::mutex mutex_;
    bool connected_ = false;
    // What get_connected() reads: true only once a connect has fully finished and the link is not
    // lost. Written under mutex_ or transition_mutex_, read with no lock (#910).
    std::atomic<bool> published_connected_{false};

    double target_ra_hours_ = 0.0;
    double target_dec_degrees_ = 0.0;
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

    std::atomic<double> aperture_diameter_m_ = 0.0;
    std::atomic<double> focal_length_m_ = 0.0;

    // Snapshot-served state (decision 0010). The poll slot's body takes mutex_ only to publish.
    mutable util::StateSnapshot<PolledFrame> snapshot_{clock_, kSnapshotMaxAge};
    mutable util::PolledLinkHealth position_link_health_;  // under mutex_
    bool link_lost_ = false;                               // decision 0009, under mutex_
    std::atomic<bool> close_lost_transport_{false};        // set with link_lost_, consumed by the poll body
    mutable std::mutex link_fault_mutex_;                  // narrow: get_link_fault() never waits on mutex_
    mutable std::string link_fault_text_;
    mutable std::string slew_poll_error_;  // last failed IsSlewComplete poll, under mutex_
    int slew_poll_error_code_ = AlpacaError::DriverException;
    std::atomic<int> slew_waiters_{0};  // SlewToCoordinates waits polling by themselves; atomic: a wait that throws
                                        // leaves its guard without mutex_
    std::atomic<bool> status_poll_paused_for_testing_{false};
    std::mutex test_hook_mutex_;                // leaf; never held while the hook runs
    std::function<void()> slew_complete_hook_;  // guarded by test_hook_mutex_

    double site_latitude_ = 0.0;
    double site_longitude_ = 0.0;
    std::optional<double> site_elevation_m_;  // nullopt until set or configured
    bool site_info_valid_ = false;

    mutable bool parked_ = false;
    // Under mutex_ (#2263). A slew, Park or FindHome claims both axes, a MoveAxis its own; AbortSlew, Unpark and a
    // disconnect cancel both, MoveAxis(axis, 0) its own. A Park or FindHome that is no longer None is retired: it
    // writes neither parked_, parking_ nor at_home_ (open-astro#2062). The driver never asks slew_running() and never
    // calls finished(): TheSkyX runs the slews, parks and homes, and the PulseGuide refusal reads slewing_cached_ and
    // homes_in_flight_. Pulses are not claimed: one DirectGuide holds the socket and nothing can end it early.
    util::MotionOwner motion_owner_;
    bool parking_ = false;     // under mutex_; a Park is sent and its IsParked poll runs; refuses slews like parked_
    mutable bool at_home_ = false;
    int homes_in_flight_ = 0;  // under mutex_; FindHome calls between their gate and their exit
    mutable bool slewing_cached_ = false;
    // Under mutex_; the claim of the slew whose flag slewing_cached_ is. Set by a slew start, put back by a refused
    // SlewToRaDec, emptied by AbortSlew; a SlewToCoordinates wait lowers Slewing only while it is its own.
    util::MotionClaim slew_claim_;
    util::HoldWindow slew_hold_;  // under mutex_; Slewing's 8 s hold after a slew command (decision 0022)
    mutable bool manual_axis_slewing_[2] = {false, false};
    mutable bool pulse_guiding_ = false;
    std::string pulse_guide_error_;
    uint64_t pulse_request_ = 0;
    util::OperationGeneration pulse_generation_;
    util::AsyncOperation pulse_guide_op_{"Bisque pulse guide", pulse_generation_, clock_};
    util::OperationGeneration poll_generation_;
    util::AsyncOperation status_poll_{"Bisque status poll", poll_generation_, clock_};

    int slew_settle_time_seconds_ = 0;

    GuideRate guide_rate_{};
};

std::unique_ptr<TelescopeDriver> create_bisque_telescope(int device_number, const ConnectionInfo& connection_info,
                                                         util::TaskClock& clock) {
    return create_bisque_telescope_with_site(device_number, connection_info, std::nullopt, std::nullopt, std::nullopt,
                                             clock);
}

std::unique_ptr<TelescopeDriver> create_bisque_telescope_with_site(
    int device_number, const ConnectionInfo& connection_info, std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg, std::optional<double> site_elevation_m, util::TaskClock& clock) {
    return std::make_unique<BisqueTelescopeDriver>(device_number, connection_info, site_latitude_deg,
                                                   site_longitude_deg, site_elevation_m, clock);
}

void set_status_poll_paused_for_testing(TelescopeDriver& driver, bool paused) {
    if (auto* bisque = dynamic_cast<BisqueTelescopeDriver*>(&driver)) {
        bisque->set_status_poll_paused_for_testing(paused);
    }
}

bool poll_status_now_for_testing(TelescopeDriver& driver) {
    if (auto* bisque = dynamic_cast<BisqueTelescopeDriver*>(&driver)) {
        return bisque->poll_status_now_for_testing();
    }
    return false;
}

void set_slew_complete_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* bisque = dynamic_cast<BisqueTelescopeDriver*>(&driver)) {
        bisque->set_slew_complete_hook_for_testing(std::move(hook));
    }
}

bool slew_hold_active_for_testing(TelescopeDriver& driver) {
    if (auto* bisque = dynamic_cast<BisqueTelescopeDriver*>(&driver)) {
        return bisque->slew_hold_active_for_testing();
    }
    return false;
}

bool slew_hold_cleared_for_testing(TelescopeDriver& driver) {
    if (auto* bisque = dynamic_cast<BisqueTelescopeDriver*>(&driver)) {
        return bisque->slew_hold_cleared_for_testing();
    }
    return false;
}

} // namespace alpacacore::vendor::bisque
