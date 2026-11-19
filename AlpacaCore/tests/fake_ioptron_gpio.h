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
#include <alpacacore/vendor/ioptron/ioptron_powerbox_wrapper.h>

#include <cerrno>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "connect_hold_gate.h"

namespace alpacacore::test {

// Scripted iMate GPIO side: line levels in memory, no GPIO chip. A line's level
// outlives a claim, as on the OpenAstro kernels where a released output keeps
// its level until the next boot; a line never touched reads boot_level (the
// stock iMate drives DC1/DC2 high at boot). Every method runs under one mutex.
//
// KNOWN PARITY GAPS (the real backend is harsher here; prefer closing a gap):
// - request() fails only when the lines are already claimed or fail_request is
//   set; the real one fails on a missing chip, a busy line or a bad offset.
// - A PWM duty is a run of set_value() calls, never a waveform.
class FakeIoptronGpio final : public vendor::ioptron::IoptronGpioBackend {
public:
    // Level of a line nothing has driven yet (before the first claim of a boot).
    int boot_level = 1;
    // While true every request() throws (a connect that must fail); never cleared.
    bool fail_request = false;
    // request() sleeps this long before claiming (a connect held open).
    std::chrono::milliseconds request_delay{0};

    std::vector<int> request(const std::string& chip_path, const std::vector<unsigned int>& offsets) override {
        alpacacore::test::connect_hold(request_delay);  // connect_hold_gate.h
        std::lock_guard<std::mutex> lock(m_);
        if (fail_request) {
            throw AlpacaException("FakeIoptronGpio: scripted request failure", AlpacaError::NotConnected);
        }
        if (claimed_) {
            throw AlpacaException("FakeIoptronGpio: lines already claimed", AlpacaError::DriverException);
        }
        chip_path_ = chip_path;
        std::vector<int> read;
        read.reserve(offsets.size());
        for (const unsigned int offset : offsets) {
            read.push_back(level_locked(offset));
        }
        claimed_offsets_ = offsets;
        claimed_ = true;
        ++requests_;
        return read;
    }

    int set_value(unsigned int offset, int value) override {
        std::lock_guard<std::mutex> lock(m_);
        bool known = false;
        for (const unsigned int o : claimed_offsets_) known = known || o == offset;
        if (!claimed_ || !known) {
            errno = EBADF;
            return -1;
        }
        const int level = value != 0 ? 1 : 0;
        if (level_locked(offset) != level) ++level_changes_;
        levels_[offset] = level;
        ++writes_;
        return 0;
    }

    void release() override {
        std::lock_guard<std::mutex> lock(m_);
        if (claimed_) ++releases_;
        claimed_ = false;
        claimed_offsets_.clear();
    }

    // Sets a line's level as the boot or a previous session left it.
    void set_level(unsigned int offset, int level) {
        std::lock_guard<std::mutex> lock(m_);
        levels_[offset] = level != 0 ? 1 : 0;
    }
    int level(unsigned int offset) const {
        std::lock_guard<std::mutex> lock(m_);
        return level_locked(offset);
    }
    int requests() const {
        std::lock_guard<std::mutex> lock(m_);
        return requests_;
    }
    int releases() const {
        std::lock_guard<std::mutex> lock(m_);
        return releases_;
    }
    int writes() const {
        std::lock_guard<std::mutex> lock(m_);
        return writes_;
    }
    // Writes that changed a line's level; a PWM worker's first steady write
    // re-drives the level the line already has and does not count.
    int level_changes() const {
        std::lock_guard<std::mutex> lock(m_);
        return level_changes_;
    }
    bool claimed() const {
        std::lock_guard<std::mutex> lock(m_);
        return claimed_;
    }

private:
    int level_locked(unsigned int offset) const {
        const auto it = levels_.find(offset);
        return it == levels_.end() ? boot_level : it->second;
    }

    mutable std::mutex m_;
    std::string chip_path_;
    std::map<unsigned int, int> levels_;
    std::vector<unsigned int> claimed_offsets_;
    bool claimed_ = false;
    int requests_ = 0;
    int releases_ = 0;
    int writes_ = 0;
    int level_changes_ = 0;
};

}  // namespace alpacacore::test
