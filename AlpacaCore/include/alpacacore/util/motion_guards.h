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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <string>

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
        if (!anchor_ || axis_miss_arcsec(anchor_->position, reading->position) > kStillToleranceArcsec) {
            anchor_ = reading;
            return false;
        }
        return reading->measured_at - anchor_->measured_at >= kStillSampleSpacing;
    }

    /// Forgets the kept read; reads measured before `not_before` prove nothing from now on.
    void restart(MotionTime not_before) {
        not_before_ = not_before;
        anchor_.reset();
    }

private:
    MotionTime not_before_;
    std::optional<PositionReading> anchor_;
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
        if (now - round_started_ < kStopVerifyBudget / kStopResendRounds) {
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

}  // namespace alpacacore::util
