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

#include <alpacacore/util/serial_io.h>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>

namespace alpacacore::util {

/// How long a latched link fault may stand before the link counts as LOST
/// (decision 0009). One default for every driver; a driver may set a longer
/// bound with a reason in its code, never one shorter than its own latch
/// threshold.
inline constexpr std::chrono::seconds kLinkStalenessBound{30};

/**
 * @brief Link-health latch for drivers that serve reads from a cache a
 *        background reader fills from a streamed status frame.
 *
 * Issue #237: when the serial link dies (USB re-enumeration, unplugged
 * cable, port stolen by another process) a cache-backed driver otherwise
 * keeps serving the last good frame forever, indistinguishable from live
 * telemetry, while only writes surface the fault as EIO. This helper ties
 * cache validity to the one thing a streaming device guarantees: frames
 * keep arriving.
 *
 * Usage (caller holds whatever mutex guards its cached status):
 *   - reset(now) at connect (the silence clock starts at connect, so a
 *     device that has not sent yet is not faulted on the spot);
 *   - on_frame(now) each time a frame commits; returns true when that frame
 *     cleared a latched fault (log "restored");
 *   - note_read_error(errno) on a persistent read() failure so the fault
 *     reason names it;
 *   - check_silence(now, limit) from the reader loop; returns the reason on
 *     the call that latches the fault (log it, invalidate the cache);
 *   - fault() from the driver's read/write paths: non-empty means refuse to
 *     serve the cache (DriverException "communications compromised").
 *
 * The fault clears on its own when a frame arrives again. Connected stays
 * true while the fault stands; a driver that adopts decision 0009 drops it
 * once the fault has stood past kLinkStalenessBound (see PolledLinkHealth).
 */
class StreamLinkHealth {
public:
    using clock = std::chrono::steady_clock;

    void reset(clock::time_point now) {
        last_frame_ = now;
        fault_.clear();
        read_error_.clear();
    }

    bool on_frame(clock::time_point now) {
        last_frame_ = now;
        read_error_.clear();
        const bool restored = !fault_.empty();
        fault_.clear();
        return restored;
    }

    void note_read_error(int err) { read_error_ = errno_string(err); }

    std::optional<std::string> check_silence(clock::time_point now, std::chrono::milliseconds limit) {
        if (!fault_.empty() || (now - last_frame_) <= limit) {
            return std::nullopt;
        }
        const auto silent_s = std::chrono::duration_cast<std::chrono::seconds>(now - last_frame_).count();
        fault_ = "no status frame for " + std::to_string(silent_s) + " s";
        if (!read_error_.empty()) {
            fault_ += " (last read error: " + read_error_ + ")";
        }
        return fault_;
    }

    bool faulted() const { return !fault_.empty(); }
    const std::string& fault() const { return fault_; }

private:
    clock::time_point last_frame_{};
    std::string fault_;       // non-empty while latched
    std::string read_error_;  // errno_string() of the last persistent read failure, if any
};

/**
 * @brief Link-health latch for REQUEST/RESPONSE drivers, whose cache is filled
 *        by the reads themselves rather than by an unsolicited frame stream.
 *
 * Issue #505 (Sky-Watcher direct driver): StreamLinkHealth measures elapsed
 * silence, which only means anything when the device sends on its own. A polled
 * board is silent exactly as often as it is asked, so the signal there is
 * CONSECUTIVE FAILED EXCHANGES with no good reply in between — the iOptron
 * `device_faulted_` shape, promoted here so a third driver does not grow a
 * fourth private copy of it.
 *
 * The contract is StreamLinkHealth's, so the two read the same way from a
 * driver: latched until a good reply, recovery clears it with no reconnect,
 * `Connected` is untouched throughout (the node is still there and the board
 * may come back on the same fd), and `fault()` non-empty means refuse to serve
 * the cache with `DriverException` "<device> communications compromised".
 * Decision 0009 adds the second stage: a fault that has stood past
 * kLinkStalenessBound (`fault_stale()`) is a LOST link, and the driver drops
 * `Connected` and closes the transport; `Connected` is untouched only while
 * the fault is younger than the bound.
 *
 * Usage (caller holds whatever mutex guards its exchanges):
 *   - reset() at connect;
 *   - on_reply() after every good exchange; returns true when that reply
 *     cleared a latched fault (log "restored" at INFO);
 *   - note_failure(reason, threshold) after every failed exchange; returns the
 *     fault reason on the call that LATCHES it (log that at ERROR), and
 *     nullopt on the transient calls before it (log those at WARN with
 *     consecutive_failures(), so a flapping link is visible before it latches);
 *   - faulted()/fault() from the driver's read and write paths.
 *
 * The threshold is a count of exchanges, not a duration: a polled link only
 * accumulates evidence when something asks, so the wall-clock time to latch is
 * the caller's poll cadence times the threshold, not the response timeout
 * times it.
 *
 * NOTE: a driver gating its reads on fault() must still ATTEMPT the hardware
 * exchange while latched, because on a polled link those reads are the only
 * traffic that can clear the fault. Refuse to serve the CACHE, not to talk.
 */
class PolledLinkHealth {
public:
    void reset() {
        consecutive_failures_ = 0;
        fault_.clear();
        fault_since_.reset();
    }

    bool on_reply() {
        consecutive_failures_ = 0;
        fault_since_.reset();
        const bool restored = !fault_.empty();
        fault_.clear();
        return restored;
    }

    std::optional<std::string> note_failure(const std::string& reason, int threshold) {
        ++consecutive_failures_;
        if (!fault_.empty() || consecutive_failures_ < threshold) {
            return std::nullopt;
        }
        fault_ = reason + " (" + std::to_string(consecutive_failures_) + " consecutive failures)";
        return fault_;
    }

    /// As above, and stamps `now` (task clock) on the call that latches, so
    /// fault_stale() can tell how long the fault has stood.
    std::optional<std::string> note_failure(const std::string& reason, int threshold,
                                            std::chrono::steady_clock::time_point now) {
        auto latched = note_failure(reason, threshold);
        if (latched) {
            fault_since_ = now;
        }
        return latched;
    }

    /// True when a latched fault has stood, without a good reply, for at least
    /// `bound` on the task clock. False when not latched or latched without a
    /// stamp.
    bool fault_stale(std::chrono::steady_clock::time_point now,
                     std::chrono::nanoseconds bound = kLinkStalenessBound) const {
        return !fault_.empty() && fault_since_ && (now - *fault_since_) >= bound;
    }

    bool faulted() const { return !fault_.empty(); }
    const std::string& fault() const { return fault_; }
    int consecutive_failures() const { return consecutive_failures_; }

private:
    int consecutive_failures_ = 0;
    std::string fault_;  // non-empty while latched
    std::optional<std::chrono::steady_clock::time_point> fault_since_;
};

/**
 * @brief When a link was LOST, as opposed to cleanly disconnected, on the task
 *        clock (decision 0009, point 3).
 *
 * Stamped once per outage by whichever loss path sees it first, for every
 * transport (serial, UDP/TCP, HID, SDK). consume() reads and clears, so the
 * stamp answers exactly one question per connect: how long was the outage a
 * relink is recovering from. A client's own disconnect never stamps, so a
 * fresh connect finds nothing and takes the safe branch.
 */
class LinkLostStamp {
public:
    using time_point = std::chrono::steady_clock::time_point;

    /// First detection wins: later polls must not push the stamp forward.
    void note(time_point now) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!at_) {
            at_ = now;
        }
    }

    std::optional<time_point> consume() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto at = at_;
        at_.reset();
        return at;
    }

private:
    std::mutex mutex_;
    std::optional<time_point> at_;
};

}  // namespace alpacacore::util
