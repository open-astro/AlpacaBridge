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
#include <alpacacore/util/gpio_line_backend.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "fake_task_clock.h"

namespace alpacacore::test {

/**
 * In-memory util::GpioLineBackend: every open, write and close is recorded with the virtual time of a shared
 * FakeTaskClock, so a test checks the edges of a soft-PWM duty cycle exactly. A helper fake, not a driver: it is
 * listed in HELPER_FAKES in scripts/check_docs_drift.py.
 *
 * open() returns the levels set by set_level() (every line high by default, the boot's level), or nothing after
 * set_unreadable() (the RK3568). fail_open() and fail_write(line) make the next open, or every write to a line,
 * fail the way a kernel refusal does. No method blocks unless a case sets the before_write hook. Thread-safe: the
 * engine's workers write while the test thread reads the record.
 */
class FakeGpioLineBackend final : public util::GpioLineBackend {
public:
    struct Event {
        enum class Kind { Open, Write, Close };
        Kind kind;
        std::size_t line_index = 0;  // Write
        bool high = false;           // Write
        bool ok = true;              // Write: false when fail_write() refused it
        FakeTaskClock::clock::time_point at;
    };

    explicit FakeGpioLineBackend(FakeTaskClock& clock) : clock_(clock) {}

    std::vector<int> open(const std::string& device_path, const std::vector<unsigned int>& lines) override {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back({Event::Kind::Open, 0, false, true, clock_.now()});
        device_path_ = device_path;
        lines_ = lines;
        if (fail_open_) {
            throw AlpacaException("fake GPIO device cannot be opened", AlpacaError::NotConnected);
        }
        is_open_ = true;
        if (!readable_) {
            return {};
        }
        std::vector<int> levels(lines.size(), 1);
        for (const auto& [index, level] : levels_) {
            if (index < levels.size()) {
                levels[index] = level;
            }
        }
        return levels;
    }

    bool write(std::size_t line_index, bool high) override {
        // The hook runs before the write is recorded and without the fake's lock, so it may block the writer.
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hook = before_write_;
        }
        if (hook) {
            hook();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const bool ok = failing_.count(line_index) == 0;
        events_.push_back({Event::Kind::Write, line_index, high, ok, clock_.now()});
        cv_.notify_all();
        return ok;
    }

    void close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back({Event::Kind::Close, 0, false, true, clock_.now()});
        is_open_ = false;
    }

    // Level the line at @p line_index (a position in the opened vector) holds at the next open().
    void set_level(std::size_t line_index, int level) {
        std::lock_guard<std::mutex> lock(mutex_);
        levels_[line_index] = level;
    }
    void set_unreadable() {
        std::lock_guard<std::mutex> lock(mutex_);
        readable_ = false;
    }
    void fail_open(bool fail = true) {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_open_ = fail;
    }
    void fail_write(std::size_t line_index, bool fail = true) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fail) {
            failing_.insert(line_index);
        } else {
            failing_.erase(line_index);
        }
    }

    /// Test-only hook, null by default (the shape of FakeQHYSDK::before_call): called inside write() before it is
    /// recorded, so a case can hold one write in flight. The only fake method that may block, and only through it.
    void set_before_write(std::function<void()> hook) {
        std::lock_guard<std::mutex> lock(mutex_);
        before_write_ = std::move(hook);
    }

    std::vector<Event> events() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return events_;
    }
    // The writes to @p line_index, in order.
    std::vector<Event> writes_to(std::size_t line_index) const {
        std::vector<Event> out;
        for (const auto& e : events()) {
            if (e.kind == Event::Kind::Write && e.line_index == line_index) {
                out.push_back(e);
            }
        }
        return out;
    }
    std::size_t write_count() const {
        std::size_t n = 0;
        for (const auto& e : events()) {
            n += e.kind == Event::Kind::Write ? 1 : 0;
        }
        return n;
    }
    std::vector<unsigned int> opened_lines() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_;
    }
    std::string opened_path() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return device_path_;
    }
    bool is_open() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return is_open_;
    }

    /// Blocks the test thread until @p n writes have been recorded, or @p real_timeout of real time passes.
    bool wait_for_writes(std::size_t n, std::chrono::milliseconds real_timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, real_timeout, [this, n] {
            std::size_t count = 0;
            for (const auto& e : events_) {
                count += e.kind == Event::Kind::Write ? 1 : 0;
            }
            return count >= n;
        });
    }

private:
    FakeTaskClock& clock_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Event> events_;
    std::vector<unsigned int> lines_;
    std::string device_path_;
    std::map<std::size_t, int> levels_;
    std::set<std::size_t> failing_;
    std::function<void()> before_write_;
    bool readable_ = true;
    bool fail_open_ = false;
    bool is_open_ = false;
};

}  // namespace alpacacore::test
