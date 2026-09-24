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

#pragma once

#include <alpacacore/alpacadriver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/motion_policy.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace alpacacore {

/**
 * @brief Telescope alignment mode.
 */
enum class AlignmentMode {
    AltAz,
    Polar,
    GermanPolar
};

/**
 * @brief Equatorial coordinate system.
 */
enum class EquatorialSystem {
    Other = 0,
    Topocentric = 1,
    J2000 = 2,
    J2050 = 3,
    B1950 = 4
};

/**
 * @brief Telescope guide rate.
 */
struct GuideRate {
    double ra{};
    double dec{};
};

/**
 * @brief Pure virtual interface for Alpaca Telescope drivers.
 *
 * Follows ASCOM Alpaca Telescope API specification.
 * All telescope drivers must implement this interface.
 */
class TelescopeDriver : public AlpacaDriver {
public:
    virtual ~TelescopeDriver() = default;

    // Platform 7 operational state (ITelescopeV4): built from the individual
    // property getters (omit-on-throw keeps DeviceState consistent with the GET
    // endpoints) plus a TimeStamp. A disconnected driver returns the empty list, with no TimeStamp.
    // Inline so the vtable stays weak and the per-vendor static libraries link
    // without a base-library ordering dependency. UTCDate is intentionally omitted to avoid format drift versus
    // the /utcdate endpoint; it is optional ("if known").
    std::vector<DeviceState> get_device_state() const override final {
        if (!get_connected()) {
            return {};
        }
        std::vector<DeviceState> state;
        auto add = [&state](const char* name, auto getter) {
            try {
                state.push_back({name, DeviceStateValue{getter()}});
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
                // Not currently known -- or an unwrapped vendor error -- so omit per the DeviceState contract.
            }
        };
        add("Altitude", [this] { return get_altitude(); });
        add("AtHome", [this] { return get_at_home(); });
        add("AtPark", [this] { return get_at_park(); });
        add("Azimuth", [this] { return get_azimuth(); });
        add("Declination", [this] { return get_declination(); });
        add("IsPulseGuiding", [this] { return get_is_pulse_guiding(); });
        add("RightAscension", [this] { return get_right_ascension(); });
        add("SideOfPier", [this] { return static_cast<std::int32_t>(get_side_of_pier()); });
        add("SiderealTime", [this] { return get_sidereal_time(); });
        add("Slewing", [this] { return get_slewing(); });
        add("Tracking", [this] { return get_tracking(); });
        state.push_back({"TimeStamp", device_state_timestamp()});
        return state;
    }

    // open-astro#547: client-silence motion watchdog. Stops motion the
    // driver started but nobody is watching any more, without ever
    // dropping Connected. Inline for the same link-topology reason as
    // get_device_state() above (AlpacaCore/CMakeLists.txt links every
    // vendor library PRIVATE into the base, never the other way, so an
    // out-of-line base symbol would not resolve for a vendor-only test
    // binary).
    //
    // Arming predicate: Slewing IS the arming predicate, by ASCOM contract,
    // for every vendor -- no per-driver plumbing needed. get_slewing() is
    // true for a goto/park/home/MoveAxis-at-a-rate in progress and false
    // for ordinary sidereal tracking, so a quietly tracking mount is never
    // armed. PulseGuide does not set Slewing either (ITelescopeV4:
    // IsPulseGuiding is the separate property for that), so a guide pulse
    // never arms this watchdog.
    //
    // Client activity: the caller (the HTTP router's single device-dispatch
    // choke point) calls note_client_activity() on every request addressed
    // to this device, including the client's own Slewing polls -- there is
    // no per-endpoint list to keep in sync.
    //
    // The pending flag means a quietly tracking mount costs one atomic
    // compare per check and no mount I/O: get_slewing() (and, if armed,
    // abort_slew()/move_axis()) is only ever called after the interval has
    // already elapsed, once per silence episode.

    /**
     * @brief Record that a client just addressed this device (issue #547).
     *
     * Called from the HTTP router's device-dispatch choke point for every
     * request routed to this telescope, so no per-endpoint plumbing is
     * needed to keep the client-silence watchdog fed.
     */
    void note_client_activity(std::chrono::steady_clock::time_point now) noexcept {
        last_client_activity_.store(now.time_since_epoch().count());
        silence_check_pending_.store(true);
        probe_failure_logged_.store(false);
    }

    /**
     * @brief Mark that a request addressed to this device is being handled
     * right now, for as long as it takes (issue #547 review finding).
     *
     * note_client_activity() only stamps once, at intake, but a synchronous
     * call the router dispatches after it -- SlewToCoordinates chief among
     * them -- can block the HTTP worker for the length of a whole goto,
     * well past the watchdog interval, while the client is actively
     * waiting on its own response. Pair with end_client_request() around
     * the dispatch (RAII at the call site), so stop_motion_if_client_silent()
     * can tell "nobody has addressed this device" apart from "a request to
     * this device is still in flight".
     */
    void begin_client_request() noexcept { in_flight_requests_.fetch_add(1); }

    /**
     * @brief Pairs with begin_client_request(); see its comment.
     */
    void end_client_request() noexcept { in_flight_requests_.fetch_sub(1); }

    /**
     * @brief The last time recorded by note_client_activity(), if any.
     *
     * Test-only observability seam; production code drives the watchdog
     * through stop_motion_if_client_silent() instead of reading this back.
     */
    std::optional<std::chrono::steady_clock::time_point> last_client_activity() const {
        if (!silence_check_pending_.load() && last_client_activity_.load() == 0) {
            // Never stamped: time_since_epoch() == 0 is indistinguishable
            // from a real stamp at the epoch, but steady_clock's epoch is
            // unspecified and not wall-clock time, so a genuine stamp at
            // exactly zero ticks since process start cannot happen in
            // practice. Treat it as "never stamped".
            return std::nullopt;
        }
        return std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(last_client_activity_.load()));
    }

    /**
     * @brief Stop this telescope's motion if it has been slewing with no
     * client activity for at least @p interval (issue #547).
     *
     * Called periodically (roughly once a second) by the server's existing
     * low-frequency timer thread -- see open-astro#314's rtc_probe_thread_,
     * which this rides rather than spawning a thread per device. Checks
     * cheaply (a couple of atomic loads) on every call and only does mount
     * I/O (get_slewing(), and on expiry abort_slew()/move_axis()) once per
     * silence episode, guarded by silence_check_pending_. A request for
     * this device still in flight (begin_client_request()/
     * end_client_request()) counts as activity for its whole duration, not
     * just at intake, so a slow synchronous call is never treated as
     * silence while the client is actively waiting on its own response.
     *
     * @param now Caller-supplied clock reading (issue #105's injectable-
     *            clock precedent), so tests drive this with synthetic time
     *            points and need no wall-clock sleeps.
     * @param interval The configured silence limit. <= 0 disables the
     *            watchdog entirely (no check, no atomic even touched).
     * @return true if this call found the mount slewing with no client
     *         activity for at least @p interval and at least one stop call
     *         succeeded. false covers every other outcome, including a
     *         failed probe or a stop where every call threw; both re-arm
     *         and retry on the next tick.
     */
    bool stop_motion_if_client_silent(std::chrono::steady_clock::time_point now,
                                      std::chrono::milliseconds interval) noexcept {
        if (interval <= std::chrono::milliseconds::zero()) {
            return false;
        }
        if (!silence_check_pending_.load()) {
            return false;
        }
        if (in_flight_requests_.load() > 0) {
            // A request for this device (e.g. a synchronous
            // SlewToCoordinates) is being handled right now -- that IS
            // client activity for as long as it runs, however long the call
            // takes. note_client_activity() only stamped once, at intake,
            // so without this the interval can elapse while the client is
            // actively on the wire waiting on its own response (review
            // finding for #547: a long synchronous goto was getting
            // aborted out from under the very client that issued it).
            // Refresh the timestamp and leave the pending flag set so the
            // next tick re-checks once the request completes.
            last_client_activity_.store(now.time_since_epoch().count());
            return false;
        }
        const auto last =
            std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(last_client_activity_.load()));
        if (now - last < interval) {
            return false;
        }
        // One probe per silence episode: clear the flag before doing any
        // mount I/O, so a burst of ticks while the check itself is running
        // does not re-enter.
        silence_check_pending_.store(false);
        // A client that resumed talking (or a request that went in-flight)
        // between the elapsed check above and the clear just now would have
        // its re-arm overwritten by the store above -- re-read both signals
        // and put the flag back if either shows fresher activity than what
        // we based the elapsed-time decision on.
        if (in_flight_requests_.load() > 0 ||
            now - std::chrono::steady_clock::time_point(
                      std::chrono::steady_clock::duration(last_client_activity_.load())) <
                interval) {
            silence_check_pending_.store(true);
            return false;
        }
        bool slewing = false;
        try {
            slewing = get_connected() && get_slewing();
        } catch (const std::exception& ex) {
            // The probe itself failed (e.g. a transient link fault -- see
            // open-astro#521's relink window) rather than finding anything
            // to stop. Re-arm so the NEXT tick retries this device instead
            // of leaving it unwatched until another client request happens
            // to land (which, mid-runaway, may never come).
            silence_check_pending_.store(true);
            // Retry every tick, but log once per silence episode: a
            // connected-but-faulted mount throws on every probe, and one
            // ERROR line per second would flood the log (review of #628).
            if (!probe_failure_logged_.exchange(true)) {
                ALPACA_LOG_ERROR("telescope", "Client-silence motion watchdog probe for " + get_name() + " #" +
                                                  std::to_string(get_device_number()) +
                                                  " failed, will retry: " + std::string(ex.what()));
            }
            return false;
        } catch (...) {
            silence_check_pending_.store(true);
            if (!probe_failure_logged_.exchange(true)) {
                ALPACA_LOG_ERROR("telescope", "Client-silence motion watchdog probe for " + get_name() + " #" +
                                                  std::to_string(get_device_number()) +
                                                  " failed with a non-standard exception, will retry.");
            }
            return false;
        }
        probe_failure_logged_.store(false);
        if (!slewing) {
            return false;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last);
        const auto limit_s = std::chrono::duration_cast<std::chrono::seconds>(interval);
        ALPACA_LOG_ERROR("telescope", "Client-silence motion watchdog: no request reached " + get_name() + " #" +
                                          std::to_string(get_device_number()) + " for " +
                                          std::to_string(elapsed.count()) + " s (limit " +
                                          std::to_string(limit_s.count()) +
                                          " s) while it was slewing; stopping motion, Connected left true.");
        // Counts stop calls that returned normally. If none did, nothing was
        // stopped and the watchdog re-arms to retry on the next tick, for
        // the same reason a throwing probe does (red-team finding on #547).
        bool any_stop_succeeded = false;
        try {
            abort_slew();
            any_stop_succeeded = true;
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
            // The per-axis stop below is the actual backstop (issue #521
            // review: a MoveAxis(axis, 0) alone can be a silent no-op on
            // some vendors, which is why BOTH run) -- swallow so a throwing
            // abort_slew() does not skip it.
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
        for (int axis = 0; axis < 2; ++axis) {
            bool can_move = false;
            try {
                can_move = get_can_move_axis(axis);
            } catch (const std::exception&) {
                continue;
            } catch (...) {
                continue;
            }
            if (!can_move) {
                continue;
            }
            try {
                move_axis(axis, 0.0);
                any_stop_succeeded = true;
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
                // Swallow rather than let it escape from a background timer
                // thread and terminate the process (AGENTS.md concurrency
                // checklist: an async tail that can throw must be caught
                // inline).
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }
        }
        if (!any_stop_succeeded) {
            silence_check_pending_.store(true);
            ALPACA_LOG_ERROR("telescope", "Client-silence motion watchdog could not stop " + get_name() + " #" +
                                              std::to_string(get_device_number()) +
                                              ": every stop call failed, will retry.");
            return false;
        }
        return true;
    }

    // Telescope-specific properties

    /**
     * @brief Get the telescope's alignment mode.
     */
    virtual AlignmentMode get_alignment_mode() const = 0;

    /**
     * @brief Get the telescope's altitude in degrees.
     */
    virtual double get_altitude() const = 0;

    /**
     * @brief Get the telescope's aperture diameter in meters.
     */
    virtual double get_aperture_diameter() const = 0;
    
    /**
     * @brief Set the telescope's aperture diameter in meters.
     */
    virtual void set_aperture_diameter(double meters) = 0;

    /**
     * @brief Get the telescope's aperture area in square meters.
     */
    virtual double get_aperture_area() const = 0;

    /**
     * @brief Get whether the telescope is at home.
     */
    virtual bool get_at_home() const = 0;

    /**
     * @brief Get whether the telescope is parked.
     */
    virtual bool get_at_park() const = 0;

    /**
     * @brief Get the telescope's azimuth in degrees.
     */
    virtual double get_azimuth() const = 0;

    /**
     * @brief Get whether the telescope can find home.
     */
    virtual bool get_can_find_home() const = 0;

    /**
     * @brief Get whether the telescope can park.
     */
    virtual bool get_can_park() const = 0;

    /**
     * @brief Get whether the telescope can pulse guide.
     */
    virtual bool get_can_pulse_guide() const = 0;

    /**
     * @brief Get whether a pulse guide command is currently active.
     */
    virtual bool get_is_pulse_guiding() const = 0;

    /**
     * @brief Get whether the telescope can set declination rate.
     */
    virtual bool get_can_set_declination_rate() const = 0;

    /**
     * @brief Get whether the telescope can set guide rates.
     */
    virtual bool get_can_set_guide_rates() const = 0;

    /**
     * @brief Get whether the telescope can set park position.
     */
    virtual bool get_can_set_park() const = 0;

    /**
     * @brief Get whether the telescope can set pier side.
     */
    virtual bool get_can_set_pier_side() const = 0;

    /**
     * @brief Get whether the telescope can set right ascension rate.
     */
    virtual bool get_can_set_right_ascension_rate() const = 0;

    /**
     * @brief Get whether the telescope can set tracking.
     */
    virtual bool get_can_set_tracking() const = 0;

    /**
     * @brief Get whether the telescope can slew to Alt/Az coordinates.
     */
    virtual bool get_can_slew_alt_az() const = 0;

    /**
     * @brief Get whether the telescope can slew to Alt/Az coordinates asynchronously.
     */
    virtual bool get_can_slew_alt_az_async() const = 0;

    /**
     * @brief Get whether the telescope can sync to Alt/Az coordinates.
     */
    virtual bool get_can_sync_alt_az() const = 0;

    /**
     * @brief Get whether the telescope can slew.
     */
    virtual bool get_can_slew() const = 0;

    /**
     * @brief Get whether the telescope can slew asynchronously.
     */
    virtual bool get_can_slew_async() const = 0;

    /**
     * @brief Get whether the telescope can sync.
     */
    virtual bool get_can_sync() const = 0;

    /**
     * @brief Get whether the telescope can unpark.
     */
    virtual bool get_can_unpark() const = 0;

    /**
     * @brief Get the telescope's declination in degrees.
     */
    virtual double get_declination() const = 0;

    /**
     * @brief Get the telescope's declination rate in arcseconds per second.
     */
    virtual double get_declination_rate() const = 0;

    /**
     * @brief Set the telescope's declination rate in arcseconds per second.
     */
    virtual void set_declination_rate(double rate) = 0;

    /**
     * @brief Get whether the telescope is tracking.
     */
    virtual bool get_tracking() const = 0;

    /**
     * @brief Set whether the telescope is tracking.
     */
    virtual void set_tracking(bool tracking) = 0;

    /**
     * @brief Get the telescope's focal length in meters.
     */
    virtual double get_focal_length() const = 0;
    
    /**
     * @brief Set the telescope's focal length in meters.
     */
    virtual void set_focal_length(double meters) = 0;

    /**
     * @brief Get the telescope's guide rate.
     */
    virtual GuideRate get_guide_rate() const = 0;

    /**
     * @brief Set the telescope's guide rate.
     */
    virtual void set_guide_rate(const GuideRate& rate) = 0;

    /**
     * @brief Get the telescope's right ascension in hours.
     */
    virtual double get_right_ascension() const = 0;

    /**
     * @brief Get the telescope's right ascension rate offset in seconds of RA per
     *        sidereal second (NOT arcseconds per second; 1 s of RA = 15 arcsec).
     */
    virtual double get_right_ascension_rate() const = 0;

    /**
     * @brief Set the telescope's right ascension rate offset in seconds of RA per
     *        sidereal second (NOT arcseconds per second; 1 s of RA = 15 arcsec).
     */
    virtual void set_right_ascension_rate(double rate) = 0;

    /**
     * @brief Get the telescope's side of pier.
     */
    virtual int get_side_of_pier() const = 0;

    /**
     * @brief Set the telescope's side of pier.
     */
    virtual void set_side_of_pier(int side) = 0;

    /**
     * @brief Get the destination side of pier for the specified target coordinates.
     */
    virtual int get_destination_side_of_pier(double ra, double dec) const = 0;

    /**
     * @brief Get the equatorial coordinate system.
     */
    virtual EquatorialSystem get_equatorial_system() const = 0;

    /**
     * @brief Get whether atmospheric refraction is applied.
     */
    virtual bool get_does_refraction() const = 0;

    /**
     * @brief Set whether atmospheric refraction is applied.
     */
    virtual void set_does_refraction(bool does_refraction) = 0;

    /**
     * @brief Get the slew settle time in seconds.
     */
    virtual int get_slew_settle_time() const = 0;

    /**
     * @brief Set the slew settle time in seconds.
     */
    virtual void set_slew_settle_time(int seconds) = 0;

    /**
     * @brief Get the telescope's sidereal time in hours.
     */
    virtual double get_sidereal_time() const = 0;

    /**
     * @brief Get the telescope's site elevation in meters.
     */
    virtual double get_site_elevation() const = 0;

    /**
     * @brief Set the telescope's site elevation in meters.
     */
    virtual void set_site_elevation(double elevation) = 0;

    /**
     * @brief Get the telescope's site latitude in degrees.
     */
    virtual double get_site_latitude() const = 0;

    /**
     * @brief Set the telescope's site latitude in degrees.
     */
    virtual void set_site_latitude(double latitude) = 0;

    /**
     * @brief Get the telescope's site longitude in degrees.
     */
    virtual double get_site_longitude() const = 0;

    /**
     * @brief Set the telescope's site longitude in degrees.
     */
    virtual void set_site_longitude(double longitude) = 0;

    /**
     * @brief Get the telescope's slewing state.
     */
    virtual bool get_slewing() const = 0;

    /**
     * @brief Get the telescope's target declination in degrees.
     */
    virtual double get_target_declination() const = 0;

    /**
     * @brief Set the telescope's target declination in degrees.
     */
    virtual void set_target_declination(double dec) = 0;

    /**
     * @brief Get the telescope's target right ascension in hours.
     */
    virtual double get_target_right_ascension() const = 0;

    /**
     * @brief Set the telescope's target right ascension in hours.
     */
    virtual void set_target_right_ascension(double ra) = 0;

    /**
     * @brief Get the telescope's tracking rate.
     */
    virtual int get_tracking_rate() const = 0;

    /**
     * @brief Set the telescope's tracking rate.
     */
    virtual void set_tracking_rate(int rate) = 0;

    /**
     * @brief Get the collection of supported tracking rates (DriveRates).
     * 
     * Returns a vector of integers representing supported DriveRates:
     * 0 = driveSidereal, 1 = driveLunar, 2 = driveSolar, 3 = driveKing
     */
    virtual std::vector<int> get_tracking_rates() const = 0;

    /**
     * @brief Get the telescope's UTC date/time.
     */
    virtual std::chrono::system_clock::time_point get_utc_date() const = 0;

    /**
     * @brief Set the telescope's UTC date/time.
     */
    virtual void set_utc_date(std::chrono::system_clock::time_point utc) = 0;

    // Telescope-specific methods

    /**
     * @brief Move the telescope to the home position.
     */
    virtual void find_home() = 0;

    /**
     * @brief Move the telescope to the park position.
     */
    virtual void park() = 0;

    /**
     * @brief Pulse guide the telescope.
     *
     * @param direction Direction (0=North, 1=South, 2=East, 3=West)
     * @param duration Duration in milliseconds
     */
    virtual void pulse_guide(int direction, int duration) = 0;

    /**
     * @brief Set the telescope's park position.
     */
    virtual void set_park() = 0;

    /**
     * @brief Slew the telescope to the target coordinates.
     */
    virtual void slew_to_coordinates(double ra, double dec) = 0;

    /**
     * @brief Slew the telescope to the target coordinates asynchronously.
     */
    virtual void slew_to_coordinates_async(double ra, double dec) = 0;

    /**
     * @brief Slew the telescope to the target.
     */
    virtual void slew_to_target() = 0;

    /**
     * @brief Slew the telescope to the target asynchronously.
     */
    virtual void slew_to_target_async() = 0;

    /**
     * @brief Sync the telescope to the target coordinates.
     */
    virtual void sync_to_coordinates(double ra, double dec) = 0;

    /**
     * @brief Sync the telescope to the target.
     */
    virtual void sync_to_target() = 0;

    /**
     * @brief Unpark the telescope.
     */
    virtual void unpark() = 0;

    /**
     * @brief Check if the telescope can move the specified axis.
     *
     * @param axis Axis number (0=Primary, 1=Secondary, 2=Tertiary)
     * @return true if the axis can be moved, false otherwise
     */
    virtual bool get_can_move_axis(int axis) const = 0;

    /**
     * @brief Move the specified axis at the given rate.
     *
     * @param axis Axis number (0=Primary, 1=Secondary, 2=Tertiary)
     * @param rate Rate in degrees per second (positive or negative)
     */
    virtual void move_axis(int axis, double rate) = 0;

    /**
     * @brief Get the allowed axis rate range.
     *
     * @param axis Axis number (0=Primary, 1=Secondary, 2=Tertiary)
     * @return Pair of (min_rate, max_rate) in degrees per second.
     */
    virtual std::pair<double, double> get_axis_rate_range(int axis) const = 0;
    /**
     * @brief Get the allowed axis rate ranges.
     *
     * Use multiple ranges to represent discrete speeds.
     *
     * @param axis Axis number (0=Primary, 1=Secondary, 2=Tertiary)
     * @return Vector of (min_rate, max_rate) ranges in degrees per second.
     */
    virtual std::vector<std::pair<double, double>> get_axis_rate_ranges(int axis) const {
        return {get_axis_rate_range(axis)};
    }

    /**
     * @brief Abort any current slew operation.
     */
    virtual void abort_slew() = 0;

    /**
     * @brief Slew the telescope to the given Alt/Az coordinates.
     *
     * @param altitude Altitude in degrees
     * @param azimuth Azimuth in degrees
     */
    virtual void slew_to_alt_az(double altitude, double azimuth) = 0;

    /**
     * @brief Slew the telescope to the given Alt/Az coordinates asynchronously.
     *
     * @param altitude Altitude in degrees
     * @param azimuth Azimuth in degrees
     */
    virtual void slew_to_alt_az_async(double altitude, double azimuth) = 0;

    /**
     * @brief Sync the telescope to the given Alt/Az coordinates.
     *
     * @param altitude Altitude in degrees
     * @param azimuth Azimuth in degrees
     */
    virtual void sync_to_alt_az(double altitude, double azimuth) = 0;

private:
    // open-astro#547. Atomics, not a mutex: note_client_activity() is
    // called from HTTP worker threads on the hot path, and
    // stop_motion_if_client_silent() from the server's timer thread; a new
    // lock here would sit outside the documented driver-mutex_ order
    // (AGENTS.md concurrency checklist). The three watchdog atomics use the
    // default sequentially consistent ordering, not relaxed: the timer
    // thread's clear-then-recheck of silence_check_pending_ relies on
    // seeing a request thread's timestamp store no later than its flag
    // store, and relaxed ordering does not promise that on aarch64.
    // 0 means "never stamped" (see last_client_activity()'s comment).
    std::atomic<std::chrono::steady_clock::rep> last_client_activity_{0};
    // Set true by note_client_activity(), cleared by
    // stop_motion_if_client_silent() before it does any mount I/O -- this
    // is what limits a live silence episode to one get_slewing()/
    // abort_slew() probe rather than one per timer tick.
    std::atomic<bool> silence_check_pending_{false};
    // True once this silence episode's probe failure has been logged; reset
    // by note_client_activity() and by any probe that returns normally.
    std::atomic<bool> probe_failure_logged_{false};
    // Count of requests for THIS device currently inside dispatch (issue
    // #547 review finding). Incremented/decremented by the router's
    // begin_client_request()/end_client_request() RAII guard around
    // dispatch_device_method(), so a synchronous call that blocks past the
    // watchdog interval (e.g. SlewToCoordinates) is never mistaken for
    // silence while the client is actively waiting on it.
    std::atomic<int> in_flight_requests_{0};
};

} // namespace alpacacore
