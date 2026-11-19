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

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/hold_window.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/motion_policy.h>
#include <alpacacore/util/pointing.h>
#include <alpacacore/util/task_clock.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// The shared telescope motion guards (#2405): protocol-neutral checks that make motion fail closed, the same on every
// mount. Every threshold is in util/motion_policy.h. A guard is a clock-free decision object (the caller passes the
// time, as util::ConsecutiveSettle and util::HoldWindow take theirs) or a free function over callbacks; none is a
// base class (decision 0013 rule 5). No guard takes a driver lock or does I/O itself (decision 0016): the driver's
// callbacks do the I/O and take the locks they need, with the driver mutex_ released around the wire.

namespace alpacacore::util {

using MotionTime = TaskClock::clock::time_point;

struct MountPosition {
    double ra_hours;
    double dec_degrees;
};

/// A position read and the task-clock time the device was read. A cached value carries the time it was measured.
struct PositionReading {
    MountPosition position;
    MotionTime measured_at;
};

/// The device's own motion flag and the task-clock time it was read.
struct MotionStatusReading {
    bool moving;
    MotionTime measured_at;
};

/// The larger per-axis difference in arcsec: RA as the shortest wrapped hour difference x 15, unprojected (no
/// cos(Dec)), and Dec (#2396 AB-7).
inline double axis_miss_arcsec(const MountPosition& a, const MountPosition& b) {
    const double ra = std::abs(shortest_ra_delta_hours(a.ra_hours, b.ra_hours)) * kPointingHoursToDegrees * 3600.0;
    const double dec = std::abs(a.dec_degrees - b.dec_degrees) * 3600.0;
    return std::max(ra, dec);
}

/**
 * "The mount is still": two position reads at least kStillSampleSpacing apart that agree within
 * kStillToleranceArcsec. Keeps the earlier read of the pair and its time; a read that moved beyond the tolerance
 * becomes the new earlier read. A failed read (nullopt), or one measured before `not_before`, proves nothing. Not
 * thread-safe: the caller owns it.
 */
class StillnessCheck {
public:
    explicit StillnessCheck(MotionTime not_before) : not_before_(not_before) {}

    /// True when this read and the kept earlier one show the mount still.
    bool feed(const std::optional<PositionReading>& reading) {
        if (!reading || reading->measured_at < not_before_) {
            return false;
        }
        if (!have_anchor_ || axis_miss_arcsec(anchor_.position, reading->position) > kStillToleranceArcsec) {
            anchor_ = *reading;
            have_anchor_ = true;
            return false;
        }
        return reading->measured_at - anchor_.measured_at >= kStillSampleSpacing;
    }

    /// Forgets the kept read; reads measured before `not_before` prove nothing from now on.
    void restart(MotionTime not_before) {
        not_before_ = not_before;
        have_anchor_ = false;
    }

private:
    MotionTime not_before_;
    // A plain value and a flag, not std::optional: GCC 14 at -O1 and above reports the optional's payload as maybe
    // uninitialized here (-Werror=maybe-uninitialized in the sanitizer and release builds).
    PositionReading anchor_{};
    bool have_anchor_ = false;
};

// ── Verified stop ────────────────────────────────────────────────────────────────────────────────────────────────

/// What the stop runner does after a poll.
enum class StopStep : std::uint8_t { Resend, Wait, Verified, Failed };

/// The error a stop that did not verify leaves for Slewing (#2405), on every mount.
inline constexpr const char* kStopNotVerifiedText = "AbortSlew: the mount still reports motion after 3 stops";

/**
 * The decision of a verified stop, clock-free. Verified when the status read shows no motion and the positions show
 * the mount still (StillnessCheck), all read after the latest stop was sent; a failed, unknown or stale read is never
 * verified. Each round lasts kStopVerifyBudget / kStopResendRounds; a round that ends unverified asks for a resend,
 * and the round after the kStopResendRounds-th stop fails.
 */
class StopVerifier {
public:
    /// `first_stop_sent`: when the first stop went out.
    explicit StopVerifier(MotionTime first_stop_sent) : round_started_(first_stop_sent), still_(first_stop_sent) {}

    StopStep feed(MotionTime now, const std::optional<MotionStatusReading>& status,
                  const std::optional<PositionReading>& position) {
        const bool still = still_.feed(position);
        const bool status_stopped = status && !status->moving && status->measured_at >= round_started_;
        if (still && status_stopped) {
            return StopStep::Verified;
        }
        // In milliseconds: seconds divided by a round count would truncate (4 s / 3 is 1 s in whole seconds).
        if (now - round_started_ < std::chrono::milliseconds(kStopVerifyBudget) / kStopResendRounds) {
            return StopStep::Wait;
        }
        return stops_sent_ >= kStopResendRounds ? StopStep::Failed : StopStep::Resend;
    }

    /// The stop went out again at `now`: a new round, and reads from before it prove nothing.
    void stop_resent(MotionTime now) {
        ++stops_sent_;
        round_started_ = now;
        still_.restart(now);
    }

    int stops_sent() const { return stops_sent_; }

private:
    MotionTime round_started_;
    StillnessCheck still_;
    int stops_sent_ = 1;
};

/// The driver's side of a verified stop. Each callback takes the locks it needs (the runner holds none) and may
/// throw; a throw counts as a stop that may not have gone out, or a read that failed. A read returns nullopt when
/// the device's answer is unknown.
struct StopVerifyHooks {
    std::function<void()> send_stop;
    std::function<std::optional<MotionStatusReading>()> read_status;
    std::function<std::optional<PositionReading>()> read_position;
};

namespace detail {
template <typename Read>
auto read_or_unknown(const Read& read) -> decltype(read()) {
    try {
        return read();
    } catch (const std::exception& ex) {
        ALPACA_LOG_DEBUG("MotionGuards", std::string("stop verify: read failed: ") + ex.what());
    } catch (...) {
        ALPACA_LOG_DEBUG("MotionGuards", "stop verify: read failed: unknown exception");
    }
    return std::nullopt;
}

inline void send_stop_logged(const StopVerifyHooks& hooks) {
    try {
        hooks.send_stop();
    } catch (const std::exception& ex) {
        ALPACA_LOG_WARN("MotionGuards", std::string("stop verify: the stop failed: ") + ex.what());
    } catch (...) {
        ALPACA_LOG_WARN("MotionGuards", "stop verify: the stop failed: unknown exception");
    }
}
}  // namespace detail

/**
 * Runs a verified stop inside a slot body: sends the stop, polls every kStopVerifyPollInterval through `ctx`, and
 * resends as StopVerifier asks. `clock` is the slot's own TaskClock (the one `ctx` waits on). Returns true when the
 * stop is verified. Returns false, sending nothing more, as soon as the body is cancelled or superseded: the canceller
 * (AbortSlew, Disconnect) or the newer owner owns the mount from then on (decision 0006 rule 6). Throws
 * AlpacaException(DriverException, kStopNotVerifiedText) when the stop did not verify; the driver keeps it for Slewing.
 */
inline bool run_verified_stop(OperationContext& ctx, TaskClock& clock, const StopVerifyHooks& hooks) {
    if (ctx.stop_reason() != StopReason::None) {
        return false;
    }
    detail::send_stop_logged(hooks);
    StopVerifier verifier(clock.now());
    while (true) {
        if (!ctx.wait_for(kStopVerifyPollInterval)) {
            return false;
        }
        const auto status = detail::read_or_unknown(hooks.read_status);
        const auto position = detail::read_or_unknown(hooks.read_position);
        if (ctx.stop_reason() != StopReason::None) {
            return false;
        }
        switch (verifier.feed(clock.now(), status, position)) {
            case StopStep::Verified:
                return true;
            case StopStep::Failed:
                throw AlpacaException(kStopNotVerifiedText, AlpacaError::DriverException);
            case StopStep::Resend:
                detail::send_stop_logged(hooks);
                verifier.stop_resent(clock.now());
                break;
            case StopStep::Wait:
                break;
        }
    }
}

// ── Slew deadline and MoveAxis cap ───────────────────────────────────────────────────────────────────────────────

/// The time a slew may take: kSlewDeadlineFactor x distance / rate + kSlewDeadlineMargin, at most kMaxSlewDuration.
/// An unknown, zero, negative or non-finite rate, or a distance that is negative or not finite, gives the cap.
inline std::chrono::nanoseconds slew_time_budget(double distance_degrees,
                                                 std::optional<double> rate_degrees_per_second) {
    const std::chrono::nanoseconds cap = kMaxSlewDuration;
    if (!rate_degrees_per_second || !std::isfinite(*rate_degrees_per_second) || *rate_degrees_per_second <= 0.0 ||
        !std::isfinite(distance_degrees) || distance_degrees < 0.0) {
        return cap;
    }
    const double seconds = kSlewDeadlineFactor * distance_degrees / *rate_degrees_per_second +
                           std::chrono::duration<double>(kSlewDeadlineMargin).count();
    if (!(seconds < std::chrono::duration<double>(cap).count())) {
        return cap;
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(seconds));
}

/// A configured MoveAxis time limit in seconds: kMoveAxisDurationMin to kMoveAxisDurationMax, else InvalidValue
/// (never clamped). The default is kMaxMoveAxisDuration.
inline std::chrono::nanoseconds validate_move_axis_time_limit(double seconds) {
    if (!std::isfinite(seconds) || seconds < std::chrono::duration<double>(kMoveAxisDurationMin).count() ||
        seconds > std::chrono::duration<double>(kMoveAxisDurationMax).count()) {
        throw AlpacaException("MoveAxis time limit must be from " + std::to_string(kMoveAxisDurationMin.count()) +
                                  " to " + std::to_string(kMoveAxisDurationMax.count()) + " seconds",
                              AlpacaError::InvalidValue);
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(seconds));
}

/**
 * One poll of a timed motion (a slew with its slew_time_budget(), a MoveAxis with its limit), from a slot body.
 * `deadline` is the body's copy, armed when the motion was sent (decision 0022). Returns true while the deadline has
 * not passed. Once it has, runs the verified stop and throws AlpacaException(DriverException, expired_text) for the
 * driver to keep for Slewing; a stop that does not verify throws kStopNotVerifiedText instead. Returns false when the
 * body was cancelled or superseded during the stop (run_verified_stop()).
 */
inline bool enforce_motion_deadline(OperationContext& ctx, TaskClock& clock, HoldWindow deadline,
                                    const StopVerifyHooks& hooks, const std::string& expired_text) {
    if (deadline.active(clock.now())) {
        return true;
    }
    if (!run_verified_stop(ctx, clock, hooks)) {
        return false;
    }
    throw AlpacaException(expired_text, AlpacaError::DriverException);
}

// ── Landing gate ─────────────────────────────────────────────────────────────────────────────────────────────────

enum class Landing : std::uint8_t { Settling, Landed, FarMiss };

/**
 * Decides when a slew has landed, fed the position reads that follow the device's own slew-end (which may come early:
 * #2396). Settling until the mount is still (StillnessCheck); then Landed, or FarMiss when the still position is
 * more than kLandingFarMissArcsec from `target` on either axis (axis_miss_arcsec). A moving mount is never a far
 * miss, however far it still is. Holds the earlier read and its time, never waits, takes no lock; the driver keeps
 * Slewing true while it is Settling.
 */
class LandingGate {
public:
    LandingGate(MountPosition target, MotionTime not_before) : target_(target), still_(not_before) {}

    Landing feed(const std::optional<PositionReading>& reading) {
        if (!reading || !still_.feed(reading)) {
            return Landing::Settling;
        }
        return axis_miss_arcsec(reading->position, target_) > kLandingFarMissArcsec ? Landing::FarMiss
                                                                                    : Landing::Landed;
    }

private:
    MountPosition target_;
    StillnessCheck still_;
};

// ── Sync bound ───────────────────────────────────────────────────────────────────────────────────────────────────

/// A configured sync offset limit in degrees: kSyncOffsetMinDegrees to kSyncOffsetMaxDegrees, else InvalidValue
/// (never clamped). The default is kMaxSyncOffsetDegrees.
inline double validate_max_sync_offset_degrees(double degrees) {
    if (!std::isfinite(degrees) || degrees < kSyncOffsetMinDegrees || degrees > kSyncOffsetMaxDegrees) {
        throw AlpacaException("The sync offset limit must be from " +
                                  std::to_string(static_cast<int>(kSyncOffsetMinDegrees)) + " to " +
                                  std::to_string(static_cast<int>(kSyncOffsetMaxDegrees)) + " degrees",
                              AlpacaError::InvalidValue);
    }
    return degrees;
}

/// The sync RA rule (#2405): the short-way RA change in minutes of time, true when it is above
/// kMaxSyncRaChangeMinutes.
inline bool sync_ra_change_exceeds_bound(double target_ra_hours, double current_ra_hours) {
    return !(std::abs(shortest_ra_delta_hours(target_ra_hours, current_ra_hours)) * 60.0 <= kMaxSyncRaChangeMinutes);
}

namespace detail {
inline std::string two_decimals(double value) {
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%.2f", value);
    return text.data();
}
}  // namespace detail

/**
 * Refuses a sync with InvalidValue when it would move the pointing from `current` to `target` by more than
 * `max_offset_degrees` on the sky, change RA by more than kMaxSyncRaChangeMinutes (sync_ra_change_exceeds_bound),
 * or, for a driver that keeps the sync as an offset, leave `resulting_dec_degrees` beyond +/-90. Each limit itself is
 * accepted; a value that is not a number is refused. `label` names the member ("SyncToCoordinates"). Reads nothing; the
 * driver passes the position it holds.
 */
inline void check_sync_bound(std::string_view label, const MountPosition& current, const MountPosition& target,
                             double max_offset_degrees, std::optional<double> resulting_dec_degrees = std::nullopt) {
    const double separation_degrees =
        angular_separation_arcsec(current.ra_hours * kPointingHoursToDegrees, current.dec_degrees,
                                  target.ra_hours * kPointingHoursToDegrees, target.dec_degrees) /
        3600.0;
    if (!(separation_degrees <= max_offset_degrees)) {
        throw AlpacaException(std::string(label) + ": the sync would move the pointing by " +
                                  detail::two_decimals(separation_degrees) + " degrees, more than the " +
                                  detail::two_decimals(max_offset_degrees) + " degree limit",
                              AlpacaError::InvalidValue);
    }
    if (sync_ra_change_exceeds_bound(target.ra_hours, current.ra_hours)) {
        throw AlpacaException(
            std::string(label) + ": the sync would change RightAscension by " +
                detail::two_decimals(std::abs(shortest_ra_delta_hours(target.ra_hours, current.ra_hours)) * 60.0) +
                " minutes, more than " + std::to_string(static_cast<int>(kMaxSyncRaChangeMinutes)) + " minutes",
            AlpacaError::InvalidValue);
    }
    if (resulting_dec_degrees && !(std::abs(*resulting_dec_degrees) <= 90.0)) {
        throw AlpacaException(std::string(label) + ": the sync would put Declination at " +
                                  detail::two_decimals(*resulting_dec_degrees) + " degrees, beyond +/-90",
                              AlpacaError::InvalidValue);
    }
}

// ── Relink stop ──────────────────────────────────────────────────────────────────────────────────────────────────

/**
 * The stop a relink owes (#2405, #2396 AB-9): a link lost while a GoTo or a MoveAxis was running leaves the mount
 * moving with nobody in control, so the next Connect sends the stop before any other command. The driver notes each
 * loss with whether it owned such a motion then, asks stop_owed() at Connect and calls stop_sent() once the stop went
 * out. Only a sent stop clears it: a Disconnect leaves it owed (decision 0009, alternative E), and a failed stop is
 * tried again at the next Connect. Not thread-safe: the driver guards it with its state lock.
 */
class RelinkMotionStop {
public:
    void note_link_lost(bool goto_or_move_axis_active) { owed_ = owed_ || goto_or_move_axis_active; }
    bool stop_owed() const { return owed_; }
    void stop_sent() { owed_ = false; }

private:
    bool owed_ = false;
};

}  // namespace alpacacore::util
