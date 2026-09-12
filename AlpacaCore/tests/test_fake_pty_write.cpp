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

// Issue #424: the pty-backed fakes wrote replies with a bare blocking write()
// on a master opened without O_NONBLOCK. Once the driver side stopped draining
// -- exactly what happens as a concurrency test winds down -- that write parked
// the fake's worker thread, the destructor set stop_ and joined, and the join
// never returned, because the thread was asleep inside write() rather than at
// the top of its loop where stop_ is read. The process hung instead of failing.
//
// These cases pin the two properties that make that impossible, on the shared
// helper all six pty fakes now use. They cannot be written as "assert the old
// code fails", because the old code's failure mode IS a hang.

#ifndef _WIN32

#include <fcntl.h>
#include <stdlib.h>  // posix_openpt/grantpt/unlockpt/ptsname: POSIX, not the <cstdlib> subset
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>

#include "catch2_compat.h"
#include "fake_pty_write.h"

namespace {

using namespace alpacacore::test;

// A pty whose slave is opened but never read from, so the buffer fills and
// stays full: the state that used to wedge a fake's worker thread.
class UndrainedPty {
public:
    UndrainedPty() {
        master_fd_ = posix_openpt(O_RDWR | O_NOCTTY);
        if (master_fd_ < 0 || grantpt(master_fd_) != 0 || unlockpt(master_fd_) != 0) {
            return;
        }
        const char* name = ptsname(master_fd_);
        if (name == nullptr) {
            return;
        }
        slave_fd_ = open(name, O_RDWR | O_NOCTTY);
        struct termios tty {};
        if (slave_fd_ >= 0 && tcgetattr(slave_fd_, &tty) == 0) {
            cfmakeraw(&tty);
            tcsetattr(slave_fd_, TCSANOW, &tty);
        }
    }

    ~UndrainedPty() {
        if (slave_fd_ >= 0) close(slave_fd_);
        if (master_fd_ >= 0) close(master_fd_);
    }

    bool ok() const { return master_fd_ >= 0 && slave_fd_ >= 0; }
    int master() const { return master_fd_; }

private:
    int master_fd_ = -1;
    int slave_fd_ = -1;
};

long elapsed_ms(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
}

}  // namespace

TEST_CASE("fake pty write - make_pty_nonblocking actually sets O_NONBLOCK", "[fakes][pty][unit]") {
    UndrainedPty pty;
    REQUIRE(pty.ok());

    // The property the whole fix rests on: without it, the write below is the
    // one that parks forever.
    REQUIRE((fcntl(pty.master(), F_GETFL, 0) & O_NONBLOCK) == 0);
    REQUIRE(make_pty_nonblocking(pty.master()));
    CHECK((fcntl(pty.master(), F_GETFL, 0) & O_NONBLOCK) != 0);
}

TEST_CASE("fake pty write - a write to an undrained pty returns within its budget", "[fakes][pty][unit]") {
    UndrainedPty pty;
    REQUIRE(pty.ok());
    REQUIRE(make_pty_nonblocking(pty.master()));

    const std::atomic<bool> stop{false};
    const std::string payload(8192, 'x');

    // Fill the buffer. A bounded write returns short once there is no room,
    // rather than waiting for a reader that is never coming.
    std::size_t last_written = payload.size();
    int iterations = 0;
    const auto started = std::chrono::steady_clock::now();
    while (last_written == payload.size() && iterations < 200) {
        last_written = pty_write_bounded(pty.master(), payload, stop, std::chrono::milliseconds(50));
        ++iterations;
    }
    CHECK(last_written < payload.size());  // the buffer really did fill

    // Every one of those calls was bounded, so the whole loop is bounded too.
    // On the old blocking write the first full buffer would have hung here.
    CHECK(elapsed_ms(started) < 200 * 50 + 2000);

    // With the buffer still full, one more call returns inside its own budget.
    const auto one_call = std::chrono::steady_clock::now();
    const std::size_t written = pty_write_bounded(pty.master(), payload, stop, std::chrono::milliseconds(100));
    CHECK(written < payload.size());
    CHECK(elapsed_ms(one_call) < 1000);
}

TEST_CASE("fake pty write - a set stop flag short-circuits the wait", "[fakes][pty][unit]") {
    UndrainedPty pty;
    REQUIRE(pty.ok());
    REQUIRE(make_pty_nonblocking(pty.master()));

    const std::atomic<bool> never_stop{false};
    const std::string payload(8192, 'x');
    std::size_t last = payload.size();
    for (int i = 0; i < 200 && last == payload.size(); ++i) {
        last = pty_write_bounded(pty.master(), payload, never_stop, std::chrono::milliseconds(50));
    }
    REQUIRE(last < payload.size());

    // This is what makes a destructor's join safe: the worker notices the
    // teardown flag instead of sitting out the full budget.
    const std::atomic<bool> stop{true};
    const auto started = std::chrono::steady_clock::now();
    CHECK(pty_write_bounded(pty.master(), payload, stop, std::chrono::seconds(30)) == 0);
    CHECK(elapsed_ms(started) < 500);
}

TEST_CASE("fake pty write - a drained pty still receives the whole reply", "[fakes][pty][unit]") {
    // The fix must not cost the fakes their normal behaviour: when the driver
    // side is reading, every byte still arrives.
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    REQUIRE(master >= 0);
    REQUIRE(grantpt(master) == 0);
    REQUIRE(unlockpt(master) == 0);
    const char* name = ptsname(master);
    REQUIRE(name != nullptr);
    const int slave = open(name, O_RDWR | O_NOCTTY);
    REQUIRE(slave >= 0);
    struct termios tty {};
    if (tcgetattr(slave, &tty) == 0) {
        cfmakeraw(&tty);
        tcsetattr(slave, TCSANOW, &tty);
    }
    REQUIRE(make_pty_nonblocking(master));

    const std::atomic<bool> stop{false};
    const std::string reply = "*G0000U000000A1T1D1M1D1M1B50C50S20H15V50P10D12C1B12#";
    CHECK(pty_write_bounded(master, reply, stop) == reply.size());

    std::string received;
    received.resize(reply.size());
    std::size_t got = 0;
    while (got < reply.size()) {
        const ssize_t n = read(slave, received.data() + got, reply.size() - got);
        if (n <= 0) break;
        got += static_cast<std::size_t>(n);
    }
    CHECK(got == reply.size());
    CHECK(received == reply);

    close(slave);
    close(master);
}

#endif  // _WIN32
