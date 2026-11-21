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

#ifndef _WIN32

#include <alpacacore/vendor/celestron/celestron_cgx_protocol.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace alpacacore::test {

// KNOWN PARITY GAPS: a loopback stand-in for the SkyPortal WiFi adapter in front of the CGX motors (source 0x20,
// one TCP client at a time), modelled on the 2026-11-20 rig evidence and the scratch fake_aux_adapter.py. It answers
// at once. Its motor model is the smallest that tells a command that ran from one that did not (slice 3): a GOTO
// (0x02, 0x17) moves the axis linearly over set_goto_duration() (default 0: it lands at once), a home search (0x0b)
// reads searching for set_home_duration() (default 0: done at once) and moves nothing, a guide pulse (0x26) reads
// active for its payload's duration and moves nothing, a stop (0x24 {0}) freezes a GOTO, a home search and a drift
// where they are, a set position (0x04) relabels the axis, and a drift (set_drift()) moves an axis while SLEW_DONE
// reads done (a tracking axis, or a status that reads done too early). Real motors accelerate and decelerate; this one
// does not, except that set_stop_settle_reads() makes SLEW_DONE read busy for a number of reads after a stop. Rate
// commands (0x06, 0x07) are ACKed and change nothing. It ACKs every write except a client's ACK of a motor report
// (0x50, 0x51), which it does not answer. Motor reports are sent only when a case injects them. It does not simulate
// the bus, the switch boards' timing or the adapter's real loss mechanism; the faults are scripted per request. The
// Gen4 startup backlog is replayed as one scripted burst; the real one was 216 frames of an earlier session (October
// progress log).

/// Loopback fake of a SkyPortal WiFi adapter on the AUX bus.
class FakeCelestronAuxTcpAdapter {
public:
    enum class Fault {
        Drop,           ///< take the request and lose it: never executed, never answered (the adapter TCP-acks it)
        DropReply,      ///< execute the request, never answer it (the request reached the bus; the reply was lost)
        BadChecksum,    ///< answer with a corrupted checksum byte
        Debris,         ///< send the collision-debris byte 0xac just before the reply
        HoldUntilNext,  ///< hold the reply until the next request, then send both replies in one segment (late reply)
        Split,          ///< send the reply in two TCP segments 30 ms apart
        Close,          ///< close the connection (FIN) instead of answering
        Reset,          ///< reset the connection (RST) instead of answering
        Late,           ///< answer 250 ms late
    };

    FakeCelestronAuxTcpAdapter() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listen_fd_ < 0) return;
        const int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t size = sizeof(address);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), size) != 0 || ::listen(listen_fd_, 4) != 0 ||
            ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        port_ = ntohs(address.sin_port);
        worker_ = std::thread([this] { run(); });
    }

    ~FakeCelestronAuxTcpAdapter() {
        stop_.store(true);
        if (worker_.joinable()) worker_.join();
        if (listen_fd_ >= 0) ::close(listen_fd_);
    }

    FakeCelestronAuxTcpAdapter(const FakeCelestronAuxTcpAdapter&) = delete;
    FakeCelestronAuxTcpAdapter& operator=(const FakeCelestronAuxTcpAdapter&) = delete;

    bool ok() const { return port_ > 0; }
    int port() const { return port_; }

    void set_position(std::uint8_t axis, std::uint32_t position) {
        std::lock_guard<std::mutex> lock(mutex_);
        axes_[axis] = Axis{.position = position & kCountMask};
    }
    /// How long a GOTO takes from where the axis is to its target (default 0: it lands at once).
    void set_goto_duration(std::chrono::milliseconds duration) {
        std::lock_guard<std::mutex> lock(mutex_);
        goto_duration_ = duration;
    }
    /// How long a home search reads searching (default 0: done at once).
    void set_home_duration(std::chrono::milliseconds duration) {
        std::lock_guard<std::mutex> lock(mutex_);
        home_duration_ = duration;
    }
    /// A GOTO another client started: `axis` moves to `target` over `duration`.
    void start_goto(std::uint8_t axis, std::uint32_t target, std::chrono::milliseconds duration) {
        std::lock_guard<std::mutex> lock(mutex_);
        begin_goto_locked(axes_[axis], target, duration);
    }
    /// `axis` moves at `counts_per_second` while SLEW_DONE reads done, until a stop.
    void set_drift(std::uint8_t axis, double counts_per_second) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& state = axes_[axis];
        settle_locked(state);
        state.drift = counts_per_second;
        state.drift_since = std::chrono::steady_clock::now();
    }
    /// After a stop, SLEW_DONE on that axis reads busy for its next `reads` reads (a deceleration).
    void set_stop_settle_reads(int reads) {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_settle_reads_ = reads;
    }
    std::uint32_t position(std::uint8_t axis) {
        std::lock_guard<std::mutex> lock(mutex_);
        return position_locked(axes_[axis]);
    }
    /// Echo each request back before its reply, as the AMW006 adapter did.
    void set_echo(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        echo_ = enabled;
    }
    /// Before each reply, forward bus traffic addressed to others: a Dec switch-board frame (0x31 -> 0x11 0x09) and a
    /// reply of the same axis and command addressed to the hand control (0x0d), with a different payload.
    void set_foreign_traffic(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        foreign_ = enabled;
    }
    /// Frames sent `delay` after a connection is accepted, before any request is read (the Gen4 startup backlog).
    void set_startup_backlog(std::vector<std::uint8_t> bytes, std::chrono::milliseconds delay) {
        std::lock_guard<std::mutex> lock(mutex_);
        backlog_ = std::move(bytes);
        backlog_delay_ = delay;
    }
    /// While muted, every request is swallowed (Fault::Drop): taken, never executed, never answered.
    void set_muted(bool muted) {
        std::lock_guard<std::mutex> lock(mutex_);
        muted_ = muted;
    }
    /// Applies `fault` to the `nth` (1-based) request to (`destination`, `command`), counted over all connections.
    void script(std::uint8_t destination, std::uint8_t command, int nth, Fault fault) {
        std::lock_guard<std::mutex> lock(mutex_);
        faults_[{destination, command, nth}] = fault;
    }
    /// Sends `bytes` (an unsolicited motor report) within one 20 ms serve slice, between requests.
    void inject(std::vector<std::uint8_t> bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.insert(pending_.end(), bytes.begin(), bytes.end());
    }
    /// Sends `bytes` just before the reply to the `nth` (1-based) request to (`destination`, `command`).
    void inject_before_reply(std::uint8_t destination, std::uint8_t command, int nth, std::vector<std::uint8_t> bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        before_reply_[{destination, command, nth}] = std::move(bytes);
    }
    int count(std::uint8_t destination, std::uint8_t command) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = counts_.find({destination, command});
        return it == counts_.end() ? 0 : it->second;
    }
    std::vector<vendor::celestron::AuxFrame> received() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return received_;
    }
    int connections() const { return connections_.load(); }
    /// True while a client's connection is open on the fake's side (it sees the client's close at once).
    bool serving() const { return serving_.load(); }

    static std::vector<std::uint8_t> frame(std::uint8_t source, std::uint8_t destination, std::uint8_t command,
                                           std::vector<std::uint8_t> payload = {}) {
        return vendor::celestron::encode_aux_frame(vendor::celestron::AuxFrame{
            .source = source, .destination = destination, .command = command, .payload = std::move(payload)});
    }

private:
    static void send_bytes(int fd, const std::vector<std::uint8_t>& bytes) {
        if (!bytes.empty()) static_cast<void>(::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL));
    }

    static void abort_connection(int fd) {
        const linger abortive{1, 0};
        static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_LINGER, &abortive, sizeof(abortive)));
    }

    void run() {
        while (!stop_.load()) {
            pollfd descriptor{listen_fd_, POLLIN, 0};
            if (::poll(&descriptor, 1, 20) <= 0) continue;
            const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd < 0) continue;
            const int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            ++connections_;
            serving_.store(true);
            serve(fd);
            ::close(fd);
            serving_.store(false);
        }
    }

    void serve(int fd) {
        std::vector<std::uint8_t> backlog;
        std::chrono::milliseconds backlog_delay{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            backlog = backlog_;
            backlog_delay = backlog_delay_;
        }
        if (!backlog.empty()) {
            std::this_thread::sleep_for(backlog_delay);
            send_bytes(fd, backlog);
        }
        std::vector<std::uint8_t> buffer;
        std::vector<std::uint8_t> held;  // a HoldUntilNext reply, sent with the next one
        while (!stop_.load()) {
            {
                std::vector<std::uint8_t> pending;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    pending.swap(pending_);
                }
                send_bytes(fd, pending);
            }
            pollfd descriptor{fd, POLLIN, 0};
            if (::poll(&descriptor, 1, 20) <= 0) continue;  // 20 ms slices, so a stopping fake is noticed
            std::array<std::uint8_t, 256> incoming{};
            const ssize_t count = ::recv(fd, incoming.data(), incoming.size(), MSG_DONTWAIT);
            if (count == 0) return;
            if (count < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                return;
            }
            buffer.insert(buffer.end(), incoming.begin(), incoming.begin() + count);
            while (buffer.size() >= 2 && buffer.size() >= static_cast<std::size_t>(buffer[1]) + 3) {
                const std::size_t size = static_cast<std::size_t>(buffer[1]) + 3;
                const std::vector<std::uint8_t> raw(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size));
                buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size));
                if (!answer(fd, raw, held)) return;
            }
        }
    }

    // Answers one request; false when the connection was closed by a scripted fault.
    bool answer(int fd, const std::vector<std::uint8_t>& raw, std::vector<std::uint8_t>& held) {
        vendor::celestron::AuxFrame request;
        try {
            request = vendor::celestron::decode_aux_frame(raw);
        } catch (const std::invalid_argument&) {
            return true;  // a damaged request is not answered, as on the bus
        }
        std::optional<Fault> fault;
        bool echo = false;
        bool foreign = false;
        std::vector<std::uint8_t> payload;
        std::vector<std::uint8_t> before_bytes;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            received_.push_back(request);
            const int nth = ++counts_[{request.destination, request.command}];
            const auto it = faults_.find({request.destination, request.command, nth});
            if (it != faults_.end()) fault = it->second;
            if (muted_) fault = Fault::Drop;
            const auto before = before_reply_.find({request.destination, request.command, nth});
            if (before != before_reply_.end()) before_bytes = before->second;
            echo = echo_;
            foreign = foreign_;
            // A swallowed request never reaches the motor; a closed connection takes nothing either.
            if (fault != Fault::Drop && fault != Fault::Close && fault != Fault::Reset)
                payload = execute_locked(request);
        }
        std::vector<std::uint8_t> out = std::move(held);
        held.clear();
        if (echo) out.insert(out.end(), raw.begin(), raw.end());
        // Only the two motor controllers answer, and never the client's ACK of their own report (0x50, 0x51).
        if ((request.destination != 0x10 && request.destination != 0x11) || request.command == 0x50 ||
            request.command == 0x51) {
            send_bytes(fd, out);
            return true;
        }
        out.insert(out.end(), before_bytes.begin(), before_bytes.end());
        if (foreign) {
            const auto chatter = frame(0x31, 0x11, 0x09, {0x00, 0x00});
            out.insert(out.end(), chatter.begin(), chatter.end());
            std::vector<std::uint8_t> other = payload;
            for (auto& byte : other) byte = static_cast<std::uint8_t>(byte ^ 0x5a);
            const auto to_hand_control = frame(request.destination, 0x0d, request.command, other);
            out.insert(out.end(), to_hand_control.begin(), to_hand_control.end());
        }
        auto reply = frame(request.destination, request.source, request.command, payload);
        if (fault == Fault::Drop || fault == Fault::DropReply) {
            send_bytes(fd, out);
            return true;
        }
        if (fault == Fault::Close || fault == Fault::Reset) {
            if (fault == Fault::Reset) abort_connection(fd);
            return false;
        }
        if (fault == Fault::HoldUntilNext) {
            send_bytes(fd, out);
            held = std::move(reply);
            return true;
        }
        if (fault == Fault::Late) {
            send_bytes(fd, out);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            send_bytes(fd, reply);
            return true;
        }
        if (fault == Fault::BadChecksum) reply.back() = static_cast<std::uint8_t>(reply.back() ^ 0x55);
        if (fault == Fault::Debris) out.push_back(0xac);
        if (fault == Fault::Split) {
            out.insert(out.end(), reply.begin(), reply.begin() + 3);
            send_bytes(fd, out);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            send_bytes(fd, std::vector<std::uint8_t>(reply.begin() + 3, reply.end()));
            return true;
        }
        out.insert(out.end(), reply.begin(), reply.end());
        send_bytes(fd, out);
        return true;
    }

    static constexpr std::uint32_t kCountMask = 0xffffff;  // 24-bit encoder counts

    struct Axis {
        std::uint32_t position = 0;  // at rest, or where a GOTO or a drift started
        bool going = false;
        std::int64_t goto_delta = 0;
        std::chrono::steady_clock::time_point goto_started{};
        std::chrono::milliseconds goto_duration{0};
        std::chrono::steady_clock::time_point home_until{};
        std::chrono::steady_clock::time_point guide_until{};
        double drift = 0.0;
        std::chrono::steady_clock::time_point drift_since{};
        int settle_reads = 0;
    };

    static std::int64_t signed_delta(std::uint32_t to, std::uint32_t from) {
        std::int64_t delta = static_cast<std::int64_t>(to & kCountMask) - static_cast<std::int64_t>(from & kCountMask);
        if (delta >= 0x800000) delta -= 0x1000000;
        if (delta < -0x800000) delta += 0x1000000;
        return delta;
    }

    static std::uint32_t position_locked(const Axis& axis) {
        const auto now = std::chrono::steady_clock::now();
        std::int64_t moved = 0;
        if (axis.going) {
            const auto elapsed = now - axis.goto_started;
            moved = elapsed >= axis.goto_duration
                        ? axis.goto_delta
                        : static_cast<std::int64_t>(static_cast<double>(axis.goto_delta) *
                                                    std::chrono::duration<double>(elapsed).count() /
                                                    std::chrono::duration<double>(axis.goto_duration).count());
        } else if (axis.drift != 0.0) {
            moved =
                static_cast<std::int64_t>(axis.drift * std::chrono::duration<double>(now - axis.drift_since).count());
        }
        return static_cast<std::uint32_t>((static_cast<std::int64_t>(axis.position) + moved) & kCountMask);
    }

    // Folds a GOTO or a drift into a resting position where it is now.
    static void settle_locked(Axis& axis) {
        axis.position = position_locked(axis);
        axis.going = false;
        axis.drift = 0.0;
    }

    static void begin_goto_locked(Axis& axis, std::uint32_t target, std::chrono::milliseconds duration) {
        settle_locked(axis);
        axis.goto_delta = signed_delta(target, axis.position);
        axis.goto_started = std::chrono::steady_clock::now();
        axis.goto_duration = duration;
        axis.going = duration.count() > 0;
        if (!axis.going) axis.position = target & kCountMask;
    }

    static std::uint32_t u24(const std::vector<std::uint8_t>& payload) {
        return payload.size() == 3 ? (static_cast<std::uint32_t>(payload[0]) << 16) |
                                         (static_cast<std::uint32_t>(payload[1]) << 8) | payload[2]
                                   : 0;
    }

    // What the motor does with `request`, and the payload of its reply. Called under mutex_.
    std::vector<std::uint8_t> execute_locked(const vendor::celestron::AuxFrame& request) {
        if (request.destination != 0x10 && request.destination != 0x11) return {};
        auto& axis = axes_[request.destination];
        const auto now = std::chrono::steady_clock::now();
        if (axis.going && now - axis.goto_started >= axis.goto_duration) settle_locked(axis);
        switch (request.command) {
            case 0x01: {
                const auto position = position_locked(axis);
                return {static_cast<std::uint8_t>(position >> 16), static_cast<std::uint8_t>(position >> 8),
                        static_cast<std::uint8_t>(position)};
            }
            case 0x02:
            case 0x17:
                begin_goto_locked(axis, u24(request.payload), goto_duration_);
                return {};
            case 0x04:
                settle_locked(axis);
                axis.position = u24(request.payload);
                return {};
            case 0x05:
                return {0x17, 0x88};
            case 0x0b:
                axis.home_until = now + home_duration_;
                return {};
            case 0x12:
                return {static_cast<std::uint8_t>(now < axis.home_until ? 0x00 : 0xff)};
            case 0x13:
                if (axis.settle_reads > 0) {
                    --axis.settle_reads;
                    return {0x00};
                }
                return {static_cast<std::uint8_t>(axis.going ? 0x00 : 0xff)};
            case 0x24:
                if (request.payload == std::vector<std::uint8_t>{0x00}) {
                    settle_locked(axis);
                    axis.home_until = now;
                    axis.settle_reads = stop_settle_reads_;
                }
                return {};
            case 0x26:
                if (request.payload.size() == 2)
                    axis.guide_until = now + std::chrono::milliseconds(10 * request.payload[1]);
                return {};
            case 0x27:
                return {static_cast<std::uint8_t>(now < axis.guide_until ? 0x01 : 0x00)};
            case 0xfe:
                return {7, 18, 0x08, 0xa6};
            default:
                return {};  // every other write is ACKed with no payload
        }
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<int> connections_{0};
    std::atomic<bool> serving_{false};
    mutable std::mutex mutex_;
    std::map<std::uint8_t, Axis> axes_;
    std::chrono::milliseconds goto_duration_{0};
    std::chrono::milliseconds home_duration_{0};
    int stop_settle_reads_ = 0;
    bool echo_ = false;
    bool foreign_ = false;
    bool muted_ = false;
    std::vector<std::uint8_t> backlog_;
    std::chrono::milliseconds backlog_delay_{0};
    std::map<std::tuple<std::uint8_t, std::uint8_t, int>, Fault> faults_;
    std::map<std::tuple<std::uint8_t, std::uint8_t, int>, std::vector<std::uint8_t>> before_reply_;
    std::vector<std::uint8_t> pending_;
    std::map<std::pair<std::uint8_t, std::uint8_t>, int> counts_;
    std::vector<vendor::celestron::AuxFrame> received_;
    std::thread worker_;  // last: started after every member it reads
};

}  // namespace alpacacore::test

#endif  // !_WIN32
