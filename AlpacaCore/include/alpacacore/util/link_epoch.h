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
#include <alpacacore/util/error_handling.h>

#include <atomic>
#include <cstdint>
#include <exception>

namespace alpacacore::util {

/**
 * @brief Positive evidence that a device link is gone (decision 0009 rule 2a).
 *
 * True for an AlpacaException with code NotConnected, which an SDK wrapper throws for a
 * removed, closed or unknown handle; false for everything else (a timeout, a generic or
 * scripted error, InvalidOperation, NotImplemented). Only an error from a device call is
 * evidence: a NotConnected a driver throws for its own closed state is not.
 */
inline bool is_link_loss(const std::exception& e) {
    const auto* alpaca = dynamic_cast<const AlpacaException*>(&e);
    return alpaca != nullptr && alpaca->error_code() == AlpacaError::NotConnected;
}

/**
 * @brief A driver's Connected state as one atomic word: epoch << 1 | open bit (issue #1500).
 *
 * Every Connect and Disconnect calls begin_transition() first (a new epoch, closed), and a
 * Connect calls publish_open() last. An operation loads the word once before its device call;
 * on loss evidence it calls mark_lost() with that word, which closes the link only when the
 * word is still the open word of the same epoch. So a failure captured before a relink, or
 * while closed, never drops the new link, and a loss is applied (and logged) once.
 */
class LinkEpoch {
public:
    using Word = std::uint64_t;

    Word load() const { return word_.load(); }
    bool is_open() const { return is_open(load()); }
    static bool is_open(Word word) { return (word & 1U) != 0; }
    static Word epoch_of(Word word) { return word >> 1U; }

    /// Starts the next epoch, closed. Caller serialises transitions (the driver's transition_mutex_).
    void begin_transition() { word_.store(((epoch_of(load()) + 1U) << 1U)); }

    /// Opens the current epoch and returns the open word.
    Word publish_open() { return word_.fetch_or(1U) | 1U; }

    /// Closes the link if @p captured is still the current open word; true when this call closed it.
    bool mark_lost(Word captured) {
        if (!is_open(captured)) {
            return false;
        }
        return word_.compare_exchange_strong(captured, captured & ~Word{1U});
    }

private:
    std::atomic<Word> word_{0};
};

}  // namespace alpacacore::util
