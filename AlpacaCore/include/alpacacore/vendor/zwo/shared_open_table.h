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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_epoch.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace alpacacore::vendor::zwo {

/**
 * @brief Open bookkeeping for a ZWO SDK whose ids several drivers may hold (issue #1500).
 *
 * SDK-free: the wrapper passes the real open, close and serial read as Ops, a test fake passes
 * raw units. One mutex serialises every SDK entry point: per-device calls run in call(),
 * enumeration and info in unchecked().
 *
 * Each real open gets a link from a counter (never handed out twice) and records the serial
 * read at that open ("" for a unit that reports none). A later open of a held id reads the
 * serial again: the same serial shares the open; another serial, or a loss code, retires the
 * entry (the old handle is closed best effort at once, decision 0009 rule 3) and opens afresh;
 * any other read failure fails that open and keeps the entry. A per-device call runs only under
 * the caller's link for that id (with_link() on the calling thread): no link is a
 * DriverException, a retired link is NotConnected, both refused before the SDK is touched, and
 * a loss code from the call retires the entry.
 *
 * Residual: a held unit with no serial cannot be proven after a replug. open() shares it;
 * open_candidate() skips it.
 */
class SharedOpenTable {
public:
    struct Ops {
        std::function<void(int)> real_open;
        std::function<void(int)> real_close;
        std::function<std::string(int)> read_serial;
    };
    struct Opened {
        std::uint64_t link{};
        std::string serial;
    };

    explicit SharedOpenTable(Ops ops) : ops_(std::move(ops)) {}

    /// Opens @p id, or shares a held open of the same unit; returns the link and the serial read.
    Opened open(int id) {
        std::lock_guard<std::mutex> lock(mutex_);
        return open_locked(id);
    }

    /// As open(), but never shares a held open whose unit reported no serial (nullopt then).
    std::optional<Opened> open_candidate(int id) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(id);
        if (it != entries_.end() && it->second.serial_at_open.empty()) {
            return std::nullopt;
        }
        return open_locked(id);
    }

    /// Drops one holder of @p link; the last one really closes (its error propagates). A retired or
    /// unknown link is a no-op.
    void close(int id, std::uint64_t link) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(id);
        if (it == entries_.end() || it->second.link != link) {
            return;
        }
        if (--it->second.open_count > 0) {
            return;
        }
        entries_.erase(it);
        ops_.real_close(id);
    }

    /// A per-device SDK call, checked against the caller's link for @p id in the same hold.
    template <class Fn>
    decltype(auto) call(int id, Fn&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto expected = expected_link(id);
        if (!expected.has_value()) {
            throw AlpacaException("no link for id " + std::to_string(id) + " on this thread",
                                  AlpacaError::DriverException);
        }
        const auto it = entries_.find(id);
        if (it == entries_.end() || it->second.link != *expected) {
            throw AlpacaException("stale link for id " + std::to_string(id), AlpacaError::NotConnected);
        }
        try {
            return std::forward<Fn>(fn)();
        } catch (const std::exception& e) {
            if (util::is_link_loss(e)) {
                retire(id);
            }
            throw;
        }
    }

    /// Enumeration and info lookups: serialised, no link needed. Never a per-device call: that goes through call().
    [[nodiscard]] std::unique_lock<std::mutex> lock_unchecked() { return std::unique_lock<std::mutex>(mutex_); }

    template <class Fn>
    decltype(auto) unchecked(Fn&& fn) {
        const auto lock = lock_unchecked();
        return std::forward<Fn>(fn)();
    }

    /// Runs @p fn with @p link as this thread's link for @p id (the innermost wins); holds no lock.
    template <class Fn>
    decltype(auto) with_link(int id, std::uint64_t link, Fn&& fn) {
        const LinkScope scope(this, id, link);
        return std::forward<Fn>(fn)();
    }

    std::size_t open_count(int id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(id);
        return it == entries_.end() ? 0 : it->second.open_count;
    }

private:
    struct Entry {
        std::size_t open_count{};
        std::uint64_t link{};
        std::string serial_at_open;
    };
    struct Expected {
        const SharedOpenTable* table;
        int id;
        std::uint64_t link;
    };

    // Pushes this thread's expected link and pops it on any exit.
    class LinkScope {
    public:
        LinkScope(const SharedOpenTable* table, int id, std::uint64_t link) {
            expected_stack().push_back({table, id, link});
        }
        ~LinkScope() { expected_stack().pop_back(); }
        LinkScope(const LinkScope&) = delete;
        LinkScope& operator=(const LinkScope&) = delete;
    };

    static std::vector<Expected>& expected_stack() {
        thread_local std::vector<Expected> stack;
        return stack;
    }

    std::optional<std::uint64_t> expected_link(int id) const {
        const auto& stack = expected_stack();
        for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
            if (it->table == this && it->id == id) {
                return it->link;
            }
        }
        return std::nullopt;
    }

    // Caller holds mutex_.
    Opened open_locked(int id) {
        const auto it = entries_.find(id);
        if (it != entries_.end()) {
            Entry& held = it->second;
            if (held.serial_at_open.empty()) {
                ++held.open_count;
                return Opened{held.link, {}};
            }
            std::optional<std::string> serial;
            try {
                serial = ops_.read_serial(id);
            } catch (const std::exception& e) {
                if (!util::is_link_loss(e)) {
                    throw;  // nothing proves which unit answers: fail this open, keep the held one
                }
            }
            if (serial == held.serial_at_open) {
                ++held.open_count;
                return Opened{held.link, *serial};
            }
            retire(id);
        }
        ops_.real_open(id);
        std::string serial;
        try {
            serial = ops_.read_serial(id);
        } catch (const std::exception& e) {
            if (util::is_link_loss(e)) {
                close_best_effort(id);
                throw;
            }
            // Any other failure: a unit that reports no serial.
        }
        const std::uint64_t link = next_link_++;
        entries_.emplace(id, Entry{1, link, serial});
        return Opened{link, serial};
    }

    // Caller holds mutex_, so nothing can have reopened the id: EAFClose(id) closes the dead handle.
    void retire(int id) {
        entries_.erase(id);
        close_best_effort(id);
    }

    void close_best_effort(int id) {
        try {
            ops_.real_close(id);
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): the unit is gone
            // The SDK may refuse to close what it already dropped.
        }
    }

    Ops ops_;
    mutable std::mutex mutex_;
    std::map<int, Entry> entries_;
    std::uint64_t next_link_{1};
};

}  // namespace alpacacore::vendor::zwo
