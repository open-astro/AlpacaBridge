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
// Loopback UDP Sky-Watcher motor controller simulator (POSIX-only, like the
// other test socket helpers): the UDP socket and serve loop over the transport-free
// FakeSkyWatcherBoard (fake_skywatcher_board.h), which holds the continuous axis
// model, the profile, the knobs and the ":" command handler, so the SkyWatcher
// driver's async state machines (slew dispatch + landing refinement, Park/FindHome
// tasks, pulse-guide timers, MoveAxis stop tasks) run end-to-end through the REAL
// protocol wrapper and UDP transport with no hardware and no production-code seams
// (issue #213). The reply-hold sleep (hold_next_reply) stays on wall time: it models
// transport latency the driver waits for in real time.

#ifndef _WIN32

#include <alpacacore/util/task_clock.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>

#include "fake_skywatcher_board.h"

namespace alpacacore::test {

class FakeSkyWatcherMount : public FakeSkyWatcherBoard {
public:
    /// @param clock the clock the axis model integrates motion against.
    /// Real by default; a FakeTaskClock makes motion virtual (see
    /// fake_skywatcher_board.h). The clock must outlive the mount.
    /// @param bind_port 0 for an ephemeral port; a number to come back on the
    /// port of a mount that went away (link-loss recovery cases).
    explicit FakeSkyWatcherMount(FakeMountProfile profile = FakeMountProfile::wave_100i(),
                                 alpacacore::util::TaskClock& clock = alpacacore::util::default_task_clock(),
                                 int bind_port = 0)
        : FakeSkyWatcherBoard(std::move(profile), clock) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) {
            return;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<uint16_t>(bind_port));
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd_);
            fd_ = -1;
            return;
        }
        socklen_t len = sizeof(addr);
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 50 * 1000;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        thread_ = std::thread([this] { serve(); });
    }

    ~FakeSkyWatcherMount() {
        stop_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    const FakeMountProfile& profile() const { return profile_; }

    bool ok() const { return fd_ >= 0; }
    int port() const { return port_; }

    /// One shot, then spent: the reply to the next frame received, whichever command it is
    /// is held for @p delay. A connect waiting on that reply stays open that long; used by the contract sweep to
    /// make Connecting observable.
    void hold_next_reply(std::chrono::milliseconds delay) { hold_ms_.store(static_cast<int>(delay.count())); }

    /// Steady state, until set back to zero: every reply is held for @p latency
    /// before it is sent, on wall time like hold_next_reply. Models a link with
    /// a fixed per-transaction cost; transactions_served() counts the replies
    /// that paid it. A reply is counted before the fake sends it (or drops it
    /// under set_silent), so a client that holds its reply sees it counted; the
    /// silent decision is taken before the count becomes visible.
    void set_reply_latency(std::chrono::milliseconds latency) { latency_ms_.store(static_cast<int>(latency.count())); }

    /// A reply latency long enough that a slew still runs after the driver's
    /// first poll of it: the named constant a case uses to keep a slew alive.
    static constexpr std::chrono::milliseconds kSlewPastFirstPollLatency{30};

    /// While @p on, the board hears every frame (frames_seen() still counts
    /// it and the axes still act on it) but never answers: the driver's
    /// exchange times out with no datagram seen. Until cleared.
    void set_silent(bool on) { silent_.store(on); }

    /// Transactions the fake has finished, answered or dropped by set_silent().
    int transactions_served() const { return served_.load(); }

private:
    void serve() {
        char buf[64];
        while (!stop_.load()) {
            sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            ssize_t n = ::recvfrom(fd_, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&peer), &plen);
            if (n <= 0) {
                continue;
            }
            std::string frame(buf, static_cast<size_t>(n));
            while (!frame.empty() && (frame.back() == '\r' || frame.back() == '\n')) {
                frame.pop_back();
            }
            std::string reply = handle(frame) + "\r";
            if (const int hold_ms = hold_ms_.exchange(0); hold_ms > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
            if (const int latency_ms = latency_ms_.load(); latency_ms > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(latency_ms));
            // Decide silence before the count is visible: a case that waits for the count and then
            // calls set_silent() must not see its late reply dropped. Count before the send, so a
            // client that has its reply already sees it counted.
            const bool drop = silent_.load();
            served_.fetch_add(1);
            if (drop) continue;
            ::sendto(fd_, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr*>(&peer), plen);
        }
    }

    int fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::atomic<int> hold_ms_{0};
    std::atomic<int> latency_ms_{0};
    std::atomic<bool> silent_{false};
    std::atomic<int> served_{0};
};

}  // namespace alpacacore::test

#endif  // _WIN32
