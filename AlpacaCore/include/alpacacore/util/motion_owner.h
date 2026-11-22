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

#include <array>
#include <cstddef>
#include <cstdint>

// The per-axis motion owner (#2263): one policy, on every mount, for what a new motion command does to the motion
// already running. A command owns only the axes it moves (CONTEXT.md, "Cancelled vs superseded"):
//
//   - a slew, Park or FindHome (MotionCommand::Slew) claims both axes and supersedes everything on them;
//   - a PulseGuide claims its own axis and supersedes a pulse or MoveAxis there; the other axis keeps running;
//   - a PulseGuide while a slew, park or home runs is refused with InvalidOperation and claims nothing;
//   - a non-zero MoveAxis claims its own axis: it supersedes a pulse there, keeps one on the other axis, and
//     supersedes a slew, park or home, which own both axes;
//   - a stop (MoveAxis(axis, 0), AbortSlew, Halt, disconnect) claims nothing and cancels the bodies on its axes.
//
// Each axis has a util::OperationGeneration. A claim bumps the generation of every axis it takes and records it; a
// stop bumps the axis's cancel count. A body asks stop_reason() with its claim: Superseded once any of its axes has a
// newer claim, otherwise Cancelled once a stop reached one of its axes, otherwise None. Superseded wins (decision
// 0006 rule 6) and both reasons are sticky, because both counts only grow.
//
// The owner runs no body and keeps no thread: a driver still runs its bodies in util::AsyncOperation slots and
// cancels or joins them as decision 0006 says. claim(), cancel() and finished() are called under the driver mutex_,
// before the command sets its flags (decision 0006 rule 3), which serialises them. stop_reason() reads atomics only,
// so a body may call it with or without that mutex; it re-checks under mutex_ before it touches hardware. No I/O,
// no lock of its own, no base class (decision 0013 rule 5).

namespace alpacacore::util {

/// The motion command families the policy knows. Slew stands for every command that moves both axes: the
/// equatorial and Alt/Az slews, Park and FindHome.
enum class MotionCommand : std::uint8_t { Slew, PulseGuide, MoveAxis };

/// What a claim took: its axes and, per axis, the generation and cancel count it saw. Handed back to the owner.
struct MotionClaim {
    std::uint8_t axes = 0;
    std::array<std::uint64_t, 2> generation{};
    std::array<std::uint64_t, 2> cancels{};
};

class MotionOwner {
public:
    /// Axis sets: bit 0 is the primary axis (RA or azimuth, MoveAxis axis 0), bit 1 the secondary (Dec or altitude).
    static constexpr std::uint8_t kPrimaryAxis = 1;
    static constexpr std::uint8_t kSecondaryAxis = 2;
    static constexpr std::uint8_t kBothAxes = kPrimaryAxis | kSecondaryAxis;

    /// The axes `command` claims. `axis` is the MoveAxis axis, or 0 (RA) / 1 (Dec) for a PulseGuide; a slew
    /// ignores it. Any other axis (ITelescopeV4's tertiary axis 2) claims none of the two, so it supersedes nothing.
    static constexpr std::uint8_t axes_of(MotionCommand command, int axis) {
        if (command == MotionCommand::Slew) {
            return kBothAxes;
        }
        if (axis == 0) {
            return kPrimaryAxis;
        }
        return axis == 1 ? kSecondaryAxis : std::uint8_t{0};
    }

    MotionOwner() = default;
    MotionOwner(const MotionOwner&) = delete;
    MotionOwner& operator=(const MotionOwner&) = delete;

    /// Claims the axes of `command`, superseding every body on them. A PulseGuide while a slew, park or home runs is
    /// refused with InvalidOperation in `refused_text` and changes nothing. Call under the driver mutex_.
    MotionClaim claim(MotionCommand command, int axis = 0,
                      const char* refused_text = "PulseGuide is not allowed while a slew, park or home is running") {
        if (command == MotionCommand::PulseGuide && slew_running()) {
            throw AlpacaException(refused_text, AlpacaError::InvalidOperation);
        }
        MotionClaim claim;
        claim.axes = axes_of(command, axis);
        for (std::size_t i = 0; i < 2; ++i) {
            claim.generation[i] = has(claim.axes, i) ? generation_[i].bump() : generation_[i].current();
            claim.cancels[i] = cancels_[i].current();
        }
        if (command == MotionCommand::Slew) {
            slew_ = claim;
            slew_active_ = true;
        }
        return claim;
    }

    /// The stop path: cancels the bodies on `axes` (MoveAxis(axis, 0) its own axis; AbortSlew, Halt and
    /// disconnect kBothAxes). Claims nothing. Call under the driver mutex_.
    void cancel(std::uint8_t axes) {
        for (std::size_t i = 0; i < 2; ++i) {
            if (has(axes, i)) {
                cancels_[i].bump();
            }
        }
    }

    /// Why the body of `claim` should stop, or None while it still owns its axes and no stop reached them.
    StopReason stop_reason(const MotionClaim& claim) const {
        for (std::size_t i = 0; i < 2; ++i) {
            if (has(claim.axes, i) && generation_[i].current() != claim.generation[i]) {
                return StopReason::Superseded;
            }
        }
        for (std::size_t i = 0; i < 2; ++i) {
            if (has(claim.axes, i) && cancels_[i].current() != claim.cancels[i]) {
                return StopReason::Cancelled;
            }
        }
        return StopReason::None;
    }

    /// The current generation and cancel count of `axes`, claiming nothing: stop_reason() of it stays None until a
    /// newer claim or a stop reaches one of those axes. For a watcher that runs no motion of its own (the Sky-Watcher
    /// AbortSlew stop check, open-astro#2407). Call under the driver mutex_.
    MotionClaim observe(std::uint8_t axes) const {
        MotionClaim claim;
        claim.axes = axes;
        for (std::size_t i = 0; i < 2; ++i) {
            claim.generation[i] = generation_[i].current();
            claim.cancels[i] = cancels_[i].current();
        }
        return claim;
    }

    /// A slew, park or home body calls this when it ends, cancelled or not, so a PulseGuide is accepted again.
    /// A body that is no longer the newest slew changes nothing. Call under the driver mutex_.
    void finished(const MotionClaim& claim) {
        if (slew_active_ && claim.generation == slew_.generation) {
            slew_active_ = false;
        }
    }

    /// True while the newest slew, park or home has neither finished nor lost an axis to a newer claim. A cancelled
    /// one still runs until its body finishes. Call under the driver mutex_.
    bool slew_running() const { return slew_active_ && stop_reason(slew_) != StopReason::Superseded; }

private:
    static constexpr bool has(std::uint8_t axes, std::size_t i) { return (axes & (1U << i)) != 0; }

    std::array<OperationGeneration, 2> generation_;
    std::array<OperationGeneration, 2> cancels_;
    MotionClaim slew_;
    bool slew_active_ = false;
};

}  // namespace alpacacore::util
