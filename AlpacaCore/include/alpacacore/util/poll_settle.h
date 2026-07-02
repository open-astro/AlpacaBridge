// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

#pragma once

#include <cstdint>

namespace alpacacore::util {

/**
 * Pure decision logic for "poll a device until it settles or times out"
 * loops (issue #105).
 *
 * The driver keeps its sleep-and-read loop; this object owns the DECISION —
 * which is where the bugs live (premature settle on a deceleration bounce,
 * missed timeout, off-by-one on the final poll). Being a pure function of the
 * fed readings, it is unit-testable with scripted sequences: no sleeps, no
 * hardware, no clock (the poll count IS the elapsed time, one feed per poll
 * period — see test_poll_settle.cpp for the bounce/fast-homer/timeout
 * batteries).
 *
 * Semantics (mirrors the ToupTek AFW wait_for_home contract exactly):
 * - A reading either "looks settled" (e.g. a real slot number) or not (the
 *   firmware's in-motion sentinel). The caller applies its own predicate and
 *   feeds the boolean.
 * - Settled requires `stable_reads_required` CONSECUTIVE settled readings —
 *   a single real-slot read mid-move (deceleration bounce) must not settle;
 *   any unsettled reading resets the run.
 * - TimedOut after `max_polls` feeds without settling. A run that completes
 *   on the final feed settles (settle is checked before the poll budget).
 *
 * Use this for every new poll-until-settled loop whose shape matches
 * (stability run + poll budget); loops that complete on a single edge (e.g.
 * "poll until IsSlewing flips false") don't need it.
 */
class ConsecutiveSettle {
public:
    enum class State : std::uint8_t { Pending, Settled, TimedOut };

    ConsecutiveSettle(int stable_reads_required, int max_polls)
        : stable_required_(stable_reads_required), max_polls_(max_polls) {}

    /// Feed the result of one poll. Returns the decision as of this reading.
    /// Terminal states are sticky: further feeds keep returning them.
    State feed(bool reading_looks_settled) {
        if (state_ != State::Pending) {
            return state_;
        }
        ++polls_;
        if (reading_looks_settled) {
            if (++stable_run_ >= stable_required_) {
                state_ = State::Settled;
                return state_;
            }
        } else {
            stable_run_ = 0;
        }
        if (polls_ >= max_polls_) {
            state_ = State::TimedOut;
        }
        return state_;
    }

    State state() const { return state_; }

private:
    int stable_required_;
    int max_polls_;
    int stable_run_ = 0;
    int polls_ = 0;
    State state_ = State::Pending;
};

}  // namespace alpacacore::util
