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

#include <alpacacore/vendor/astroasis/astroasis_hid_transport.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace alpacacore::test {

// Scripted HID device (open-astro#294). Answers the wire protocol the wrapper
// speaks and counts transactions per command. The state is shared so a test
// keeps a handle after handing the transport to the driver.
struct FakeHidState {
    std::mutex mutex;
    int status_reads = 0;  // cmd 0x32
    int config_reads = 0;  // cmd 0x30 / 0x3a
    int moves = 0;         // cmd 0x36
    int halts = 0;         // cmd 0x37
    int position = 1234;
    bool moving = false;
    bool dead = false;         // reads time out, as a pulled cable does
    bool fail_config = false;  // MaxStep (0x30) goes unanswered
    bool fail_move = false;    // the device moves (0x36) but the reply is lost
    bool fail_halt = false;    // the device stops (0x37) but the reply is lost
    int closes = 0;
    int opens = 0;                    // open() calls that returned true
    bool move_reports_moving = true;  // the device reports "moving" after a Move (0x36)
    std::uint8_t gate_cmd = 0;        // hold this command's exchange open until released
    bool gate_entered = false;        // the gated exchange reached the gate
    std::condition_variable cv;
    std::vector<std::uint8_t> pending;

    int transactions() {
        std::lock_guard<std::mutex> lock(mutex);
        return status_reads + config_reads + moves + halts;
    }
};

class FakeHidTransport final : public alpacacore::vendor::astroasis::AstroasisHidTransport {
public:
    explicit FakeHidTransport(std::shared_ptr<FakeHidState> state) : state_(std::move(state)) {}

    bool open(const std::string&) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->opens;
        return true;
    }
    void close() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->closes;
    }

    int write(const std::uint8_t* data, std::size_t length) override {
        std::unique_lock<std::mutex> lock(state_->mutex);
        const std::uint8_t cmd = data[1];
        std::vector<std::uint8_t> resp;
        auto be32 = [&resp](std::uint32_t v) {
            for (int shift = 24; shift >= 0; shift -= 8) resp.push_back(static_cast<std::uint8_t>(v >> shift));
        };
        resp.push_back(cmd);
        switch (cmd) {
            case 0x11:
                resp.insert(resp.end(), {1, 0});
                break;
            case 0x10:
                resp.push_back(4);
                be32(0);
                break;
            case 0x32:
                ++state_->status_reads;
                resp.push_back(14);
                be32(2048);        // internal ADC
                be32(0x80000000);  // no external probe
                resp.push_back(0);
                resp.push_back(state_->moving ? 1 : 0);
                be32(static_cast<std::uint32_t>(state_->position));
                break;
            case 0x30:
                ++state_->config_reads;
                if (state_->fail_config) break;
                resp.push_back(18);
                be32(0xFFFFFFFF);
                be32(50000);
                resp.insert(resp.end(), 10, 0);
                break;
            case 0x36:
                ++state_->moves;
                state_->moving = state_->move_reports_moving;
                hold_gate(lock, cmd);
                if (state_->fail_move) break;
                resp.insert(resp.end(), {1, 0});
                break;
            case 0x37:
                ++state_->halts;
                state_->moving = false;
                hold_gate(lock, cmd);
                if (state_->fail_halt) break;
                resp.insert(resp.end(), {1, 0});
                break;
            default:
                break;
        }
        state_->pending = std::move(resp);
        return static_cast<int>(length);
    }

    int read(std::uint8_t* data, std::size_t length, int) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->dead || state_->pending.empty()) {
            state_->pending.clear();
            return 0;
        }
        std::memset(data, 0, length);
        std::memcpy(data, state_->pending.data(), state_->pending.size());
        state_->pending.clear();
        return static_cast<int>(length);
    }

private:
    // Blocks the exchange of the gated command until the test clears gate_cmd.
    void hold_gate(std::unique_lock<std::mutex>& lock, std::uint8_t cmd) {
        if (state_->gate_cmd != cmd) return;
        state_->gate_entered = true;
        state_->cv.notify_all();
        state_->cv.wait(lock, [this] { return state_->gate_cmd == 0; });
    }

    std::shared_ptr<FakeHidState> state_;
};

}  // namespace alpacacore::test
