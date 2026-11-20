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
// at once and never moves an axis: positions change only through set_position(). It reports every axis idle and
// home-done, every guide inactive, and ACKs every write. It does not simulate the bus, the switch boards' timing or
// the adapter's real loss mechanism; the faults are scripted per request. The Gen4 startup backlog is replayed as one
// scripted burst; the real one was 216 frames of an earlier session (October progress log).

/// Loopback fake of a SkyPortal WiFi adapter on the AUX bus.
class FakeCelestronAuxTcpAdapter {
public:
    enum class Fault {
        Drop,           ///< take the request, never answer it (the adapter TCP-acks and loses it)
        BadChecksum,    ///< answer with a corrupted checksum byte
        Debris,         ///< send the collision-debris byte 0xac just before the reply
        HoldUntilNext,  ///< hold the reply until the next request, then send both replies in one segment (late reply)
        Split,          ///< send the reply in two TCP segments 30 ms apart
        Close,          ///< close the connection (FIN) instead of answering
        Reset,          ///< reset the connection (RST) instead of answering
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
        positions_[axis] = position;
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
    /// Applies `fault` to the `nth` (1-based) request to (`destination`, `command`), counted over all connections.
    void script(std::uint8_t destination, std::uint8_t command, int nth, Fault fault) {
        std::lock_guard<std::mutex> lock(mutex_);
        faults_[{destination, command, nth}] = fault;
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
            serve(fd);
            ::close(fd);
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
        std::uint32_t position = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            received_.push_back(request);
            const int nth = ++counts_[{request.destination, request.command}];
            const auto it = faults_.find({request.destination, request.command, nth});
            if (it != faults_.end()) fault = it->second;
            echo = echo_;
            foreign = foreign_;
            position = positions_[request.destination];
        }
        std::vector<std::uint8_t> out = std::move(held);
        held.clear();
        if (echo) out.insert(out.end(), raw.begin(), raw.end());
        if (request.destination != 0x10 && request.destination != 0x11) {
            send_bytes(fd, out);
            return true;  // only the two motor controllers answer
        }
        std::vector<std::uint8_t> payload;
        switch (request.command) {
            case 0x01:
                payload = {static_cast<std::uint8_t>(position >> 16), static_cast<std::uint8_t>(position >> 8),
                           static_cast<std::uint8_t>(position)};
                break;
            case 0x05:
                payload = {0x17, 0x88};
                break;
            case 0x12:
            case 0x13:
                payload = {0xff};
                break;
            case 0x27:
                payload = {0x00};
                break;
            case 0xfe:
                payload = {7, 18, 0x08, 0xa6};
                break;
            default:
                break;  // every write and motion command is ACKed with no payload
        }
        if (foreign) {
            const auto chatter = frame(0x31, 0x11, 0x09, {0x00, 0x00});
            out.insert(out.end(), chatter.begin(), chatter.end());
            std::vector<std::uint8_t> other = payload;
            for (auto& byte : other) byte = static_cast<std::uint8_t>(byte ^ 0x5a);
            const auto to_hand_control = frame(request.destination, 0x0d, request.command, other);
            out.insert(out.end(), to_hand_control.begin(), to_hand_control.end());
        }
        auto reply = frame(request.destination, request.source, request.command, payload);
        if (fault == Fault::Drop) {
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

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<int> connections_{0};
    mutable std::mutex mutex_;
    std::map<std::uint8_t, std::uint32_t> positions_;
    bool echo_ = false;
    bool foreign_ = false;
    std::vector<std::uint8_t> backlog_;
    std::chrono::milliseconds backlog_delay_{0};
    std::map<std::tuple<std::uint8_t, std::uint8_t, int>, Fault> faults_;
    std::map<std::pair<std::uint8_t, std::uint8_t>, int> counts_;
    std::vector<vendor::celestron::AuxFrame> received_;
    std::thread worker_;  // last: started after every member it reads
};

}  // namespace alpacacore::test

#endif  // !_WIN32
