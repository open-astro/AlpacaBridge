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
//
// util::port_answers_synscan_echo() is the guard every non-9600 serial probe
// runs before touching a Prolific-class port: a SynScan V4 handset stops
// answering serial entirely after bytes at the wrong rate and only a
// power-cycle recovers it (EQM-35 Pro rig, 2026-09). A pty stands in for the
// handset so the three outcomes - exact echo, silence, and a non-echo reply -
// are exercised hardware-free.
#ifndef _WIN32

#include <alpacacore/util/synscan_handset_probe.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "fake_pty_write.h"

namespace {

// Minimal pty "handset": every chunk read from the master goes through the
// responder, whose reply (if any) is written straight back.
class FakeSerialHandset {
public:
    using Responder = std::function<std::string(const std::string& chunk)>;

    explicit FakeSerialHandset(Responder responder) : responder_(std::move(responder)) {
        master_fd_ = posix_openpt(O_RDWR | O_NOCTTY);
        if (master_fd_ >= 0) {
            alpacacore::test::make_pty_nonblocking(master_fd_);
        }
        if (master_fd_ < 0 || grantpt(master_fd_) != 0 || unlockpt(master_fd_) != 0) {
            throw std::runtime_error("FakeSerialHandset: cannot open pty");
        }
        const char* name = ptsname(master_fd_);
        if (name == nullptr) {
            throw std::runtime_error("FakeSerialHandset: ptsname failed");
        }
        slave_path_ = name;
        // Keep a slave handle open so the master never sees EIO between the
        // probe's close and a later open (same trick as FakeGeminiFlatPanel).
        keepalive_fd_ = open(slave_path_.c_str(), O_RDWR | O_NOCTTY);
        struct termios tty {};
        if (keepalive_fd_ >= 0 && tcgetattr(keepalive_fd_, &tty) == 0) {
            cfmakeraw(&tty);
            tcsetattr(keepalive_fd_, TCSANOW, &tty);
        }
        reader_ = std::thread([this] { run(); });
    }

    ~FakeSerialHandset() {
        stop_.store(true);
        if (reader_.joinable()) {
            reader_.join();
        }
        if (keepalive_fd_ >= 0) {
            close(keepalive_fd_);
        }
        if (master_fd_ >= 0) {
            close(master_fd_);
        }
    }

    FakeSerialHandset(const FakeSerialHandset&) = delete;
    FakeSerialHandset& operator=(const FakeSerialHandset&) = delete;

    const std::string& slave_path() const { return slave_path_; }

    std::string received() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return received_;
    }

private:
    void run() {
        while (!stop_.load()) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(master_fd_, &fds);
            timeval tv{0, 50000};  // 50 ms poll so stop_ is honoured promptly
            if (select(master_fd_ + 1, &fds, nullptr, nullptr, &tv) <= 0) {
                continue;
            }
            char buf[64];
            const ssize_t n = read(master_fd_, buf, sizeof(buf));
            if (n <= 0) {
                continue;
            }
            const std::string chunk(buf, static_cast<std::size_t>(n));
            {
                std::lock_guard<std::mutex> lock(mutex_);
                received_ += chunk;
            }
            const std::string reply = responder_(chunk);
            if (!reply.empty()) {
                // Issue #424: bounded, on a non-blocking master. A short or
                // dropped write just means the probe sees less than the full
                // reply, which the silent / non-echo cases exercise on
                // purpose -- whereas a blocking write to a pty the probe has
                // stopped draining parks this thread and hangs the join.
                alpacacore::test::pty_write_bounded(master_fd_, reply, stop_);
            }
        }
    }

    Responder responder_;
    int master_fd_ = -1;
    int keepalive_fd_ = -1;
    std::string slave_path_;
    std::thread reader_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    std::string received_;
};

long elapsed_ms(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
}

}  // namespace

TEST_CASE("SynScan handset probe - a handset's exact echo is recognised", "[util][synscan_handset_probe]") {
    FakeSerialHandset handset([](const std::string& chunk) -> std::string {
        // Protocol echo: "K" + byte -> byte + "#"
        if (chunk.size() >= 2 && chunk[0] == 'K') {
            return std::string(1, chunk[1]) + "#";
        }
        return "";
    });
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(alpacacore::util::port_answers_synscan_echo(handset.slave_path(), 1000));
    CHECK(elapsed_ms(t0) < 500);        // answered at once, not at the deadline
    CHECK(handset.received() == "KB");  // exactly the echo went out, nothing at another baud
}

TEST_CASE("SynScan handset probe - a silent port is not a handset and returns at the deadline",
          "[util][synscan_handset_probe]") {
    FakeSerialHandset silent([](const std::string&) { return std::string(); });
    const auto t0 = std::chrono::steady_clock::now();
    CHECK_FALSE(alpacacore::util::port_answers_synscan_echo(silent.slave_path(), 300));
    const auto ms = elapsed_ms(t0);
    CHECK(ms >= 250);
    CHECK(ms < 1500);
}

TEST_CASE("SynScan handset probe - a non-echo reply is not mistaken for a handset", "[util][synscan_handset_probe]") {
    // A motor-controller board or another device answering *something* must
    // not make the scan skip the port: only the exact echo identifies a handset.
    FakeSerialHandset other([](const std::string&) { return std::string("0#"); });
    CHECK_FALSE(alpacacore::util::port_answers_synscan_echo(other.slave_path(), 500));
}

#endif  // !_WIN32
