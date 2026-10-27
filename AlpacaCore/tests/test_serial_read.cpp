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

// util::read_until / read_until_socket (issue #1604): the read loop above
// tty_read(), tested over a pty pair and a socketpair.

#include <alpacacore/util/serial_link.h>
#include <alpacacore/util/serial_read.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "fake_pty_write.h"

using namespace std::chrono_literals;
using alpacacore::test::PtyPair;
using alpacacore::util::make_raw_8n1;
using alpacacore::util::read_until;
using alpacacore::util::read_until_socket;
using alpacacore::util::ReadOutcome;
using alpacacore::util::ReadSpec;
using alpacacore::util::SerialClaimKind;
using alpacacore::util::SerialLink;

namespace {

using Clock = std::chrono::steady_clock;

SerialLink open_slave(const PtyPair& pty) {
    std::string error;
    auto link = SerialLink::open(
        pty.slave_path(), SerialClaimKind::Link, "serial_read test",
        [](struct termios& tty) { make_raw_8n1(tty, B9600); }, &error);
    INFO(error);
    REQUIRE(link);
    return link;
}

void feed(int fd, const std::string& s) { REQUIRE(::write(fd, s.data(), s.size()) == static_cast<ssize_t>(s.size())); }

struct SocketPair {
    int a = -1;
    int b = -1;
    SocketPair() {
        int fds[2];
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        a = fds[0];
        b = fds[1];
    }
    ~SocketPair() {
        if (a >= 0) ::close(a);
        if (b >= 0) ::close(b);
    }
    void close_b() {
        ::close(b);
        b = -1;
    }
};

void noop_handler(int) {}

// Interrupt the reading thread with a signal that has no SA_RESTART, then
// deliver the reply after the interrupt.
template <typename Read, typename Write>
ReadOutcome read_through_signal(Read read, Write write, std::string* bytes) {
    struct sigaction sa {};
    sa.sa_handler = noop_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    struct sigaction old {};
    REQUIRE(sigaction(SIGUSR1, &sa, &old) == 0);
    const pthread_t reader = pthread_self();
    std::thread helper([&] {
        std::this_thread::sleep_for(40ms);
        pthread_kill(reader, SIGUSR1);
        std::this_thread::sleep_for(40ms);
        write();
    });
    ReadSpec spec;
    const auto r = read(spec, Clock::now() + 3s);
    helper.join();
    sigaction(SIGUSR1, &old, nullptr);
    *bytes = r.bytes;
    return r.outcome;
}

}  // namespace

// Falsified by: serial_read.h read_until_impl() not dropping bytes found in spec.skip
TEST_CASE("read_until - a terminator read keeps the terminator and drops CR/LF", "[serial][serial_read][issue1604]") {
    PtyPair pty("serial_read");
    auto link = open_slave(pty);
    feed(pty.master_fd(), "\r\n12\n3#");
    const auto r = read_until(link.fd(), ReadSpec{}, Clock::now() + 1s);
    CHECK(r.outcome == ReadOutcome::Terminator);
    CHECK(r.bytes == "123#");
}

// Falsified by: serial_read.h read_until_impl() testing spec.terminators for only the first character
TEST_CASE("read_until - any byte of the terminator set ends the read", "[serial][serial_read][issue1604]") {
    PtyPair pty("serial_read");
    auto link = open_slave(pty);
    feed(pty.master_fd(), "ab;cd");
    ReadSpec spec;
    spec.terminators = "#;";
    const auto r = read_until(link.fd(), spec, Clock::now() + 1s);
    CHECK(r.outcome == ReadOutcome::Terminator);
    CHECK(r.bytes == "ab;");
}

// Falsified by: serial_read.h read_until_impl() ending a Length read one byte early or late
TEST_CASE("read_until - a Length read returns exactly that many bytes", "[serial][serial_read][issue1604]") {
    PtyPair pty("serial_read");
    auto link = open_slave(pty);
    feed(pty.master_fd(), std::string("\x01\r\x03\x04\x05", 5));
    ReadSpec spec;
    spec.stop = ReadSpec::Stop::Length;
    spec.length = 3;
    spec.skip.clear();
    const auto r = read_until(link.fd(), spec, Clock::now() + 1s);
    CHECK(r.outcome == ReadOutcome::Length);
    CHECK(r.bytes == std::string("\x01\r\x03", 3));
}

// Falsified by: serial_read.h read_until_impl() applying spec.skip only outside a Length read
TEST_CASE("read_until - a Length(1) ack read skips a stray CR/LF first", "[serial][serial_read][issue1604]") {
    PtyPair pty("serial_read");
    auto link = open_slave(pty);
    feed(pty.master_fd(), "\r\n1");
    ReadSpec spec;
    spec.stop = ReadSpec::Stop::Length;
    spec.length = 1;
    const auto r = read_until(link.fd(), spec, Clock::now() + 1s);
    CHECK(r.outcome == ReadOutcome::Length);
    CHECK(r.bytes == "1");
}

// Falsified by: serial_read.h read_until_impl() not shortening the wait to last_kept + spec.idle_gap
TEST_CASE("read_until - an IdleGap read ends after the gap that follows the first byte",
          "[serial][serial_read][issue1604]") {
    PtyPair pty("serial_read");
    auto link = open_slave(pty);
    feed(pty.master_fd(), "abc");
    ReadSpec spec;
    spec.stop = ReadSpec::Stop::IdleGap;
    spec.idle_gap = 60ms;
    const auto start = Clock::now();
    const auto r = read_until(link.fd(), spec, start + 5s);
    CHECK(r.outcome == ReadOutcome::Idle);
    CHECK(r.bytes == "abc");
    CHECK(Clock::now() - start < 2s);
}

// Falsified by: serial_read.h read_until_impl() not ending a TerminatorOrIdleGap read on a byte of spec.terminators
TEST_CASE("read_until - a TerminatorOrIdleGap read ends at the terminator and leaves later bytes in the fd",
          "[serial][serial_read][issue1644]") {
    PtyPair pty("serial_read");
    auto link = open_slave(pty);
    feed(pty.master_fd(), "ab#late");
    ReadSpec spec;
    spec.stop = ReadSpec::Stop::TerminatorOrIdleGap;
    spec.idle_gap = 2s;
    const auto start = Clock::now();
    const auto r = read_until(link.fd(), spec, start + 5s);
    CHECK(r.outcome == ReadOutcome::Terminator);
    CHECK(r.bytes == "ab#");
    CHECK(Clock::now() - start < 1s);

    ReadSpec rest;
    rest.stop = ReadSpec::Stop::Length;
    rest.length = 4;
    const auto tail = read_until(link.fd(), rest, Clock::now() + 1s);
    CHECK(tail.outcome == ReadOutcome::Length);
    CHECK(tail.bytes == "late");
}

// Falsified by: serial_read.h read_until_impl() not shortening the wait to last_kept + spec.idle_gap in
// TerminatorOrIdleGap mode
TEST_CASE("read_until - a TerminatorOrIdleGap read with no terminator ends after the idle gap",
          "[serial][serial_read][issue1644]") {
    PtyPair pty("serial_read");
    auto link = open_slave(pty);
    feed(pty.master_fd(), "abc");
    ReadSpec spec;
    spec.stop = ReadSpec::Stop::TerminatorOrIdleGap;
    spec.idle_gap = 60ms;
    const auto start = Clock::now();
    const auto r = read_until(link.fd(), spec, start + 5s);
    CHECK(r.outcome == ReadOutcome::Idle);
    CHECK(r.bytes == "abc");
    CHECK(Clock::now() - start < 2s);
}

// Falsified by: serial_read.h read_until_impl() not ending a TerminatorOrIdleGap read on a byte of spec.terminators
TEST_CASE("read_until_socket - a TerminatorOrIdleGap read ends at the terminator or after the idle gap",
          "[serial][serial_read][issue1644]") {
    SocketPair sp;
    feed(sp.b, "\r1#xy");
    ReadSpec spec;
    spec.stop = ReadSpec::Stop::TerminatorOrIdleGap;
    spec.idle_gap = 60ms;
    auto start = Clock::now();
    const auto r = read_until_socket(sp.a, spec, start + 5s);
    CHECK(r.outcome == ReadOutcome::Terminator);
    CHECK(r.bytes == "1#");

    start = Clock::now();
    const auto idle = read_until_socket(sp.a, spec, start + 5s);
    CHECK(idle.outcome == ReadOutcome::Idle);
    CHECK(idle.bytes == "xy");
    CHECK(Clock::now() - start < 2s);
}

// Falsified by: serial_read.h read_until_impl() not applying first_limit while no byte has arrived
TEST_CASE("read_until - first_byte_timeout ends a read that hears nothing", "[serial][serial_read][issue1604]") {
    PtyPair pty("serial_read");
    auto link = open_slave(pty);
    ReadSpec spec;
    spec.first_byte_timeout = 50ms;
    const auto start = Clock::now();
    const auto r = read_until(link.fd(), spec, start + 5s);
    CHECK(r.outcome == ReadOutcome::Timeout);
    CHECK(r.bytes.empty());
    CHECK(Clock::now() - start < 2s);
}

// Falsified by: serial_read.h read_until_impl() dropping res.bytes when the deadline passes
TEST_CASE("read_until - the deadline returns Timeout with the bytes that arrived", "[serial][serial_read][issue1604]") {
    PtyPair pty("serial_read");
    auto link = open_slave(pty);
    feed(pty.master_fd(), "ab");
    const auto start = Clock::now();
    const auto r = read_until(link.fd(), ReadSpec{}, start + 100ms);
    CHECK(r.outcome == ReadOutcome::Timeout);
    CHECK(r.bytes == "ab");
    CHECK(r.err == 0);
    CHECK(Clock::now() - start >= 100ms);
}

// Falsified by: serial_read.h read_until_impl() comparing bytes.size() to spec.max_len with > instead of >=
TEST_CASE("read_until - reaching max_len without a stop is Overflow", "[serial][serial_read][issue1604]") {
    PtyPair pty("serial_read");
    auto link = open_slave(pty);
    feed(pty.master_fd(), "0123456789#");
    ReadSpec spec;
    spec.max_len = 4;
    const auto r = read_until(link.fd(), spec, Clock::now() + 1s);
    CHECK(r.outcome == ReadOutcome::Overflow);
    CHECK(r.bytes == "0123");

    // A terminator on the max_len-th byte is a hit, not an overflow.
    PtyPair pty2("serial_read");
    auto link2 = open_slave(pty2);
    feed(pty2.master_fd(), "012#");
    const auto ok = read_until(link2.fd(), spec, Clock::now() + 1s);
    CHECK(ok.outcome == ReadOutcome::Terminator);
    CHECK(ok.bytes == "012#");
}

// Falsified by: serial_read.h read_until_impl() returning Timeout once two bytes are kept (bytes.size() == 2)
TEST_CASE("read_until - a reply split across two writes is assembled", "[serial][serial_read][issue1604]") {
    PtyPair pty("serial_read");
    auto link = open_slave(pty);
    std::thread writer([&] {
        feed(pty.master_fd(), "12");
        std::this_thread::sleep_for(80ms);
        feed(pty.master_fd(), "34#");
    });
    const auto r = read_until(link.fd(), ReadSpec{}, Clock::now() + 3s);
    writer.join();
    CHECK(r.outcome == ReadOutcome::Terminator);
    CHECK(r.bytes == "1234#");
}

// Falsified by: serial_read.h read_until_impl() reading more than one byte per call
TEST_CASE("read_until - bytes after the terminator stay in the fd", "[serial][serial_read][issue1604]") {
    PtyPair pty("serial_read");
    auto link = open_slave(pty);
    feed(pty.master_fd(), "ab#late#");
    const auto first = read_until(link.fd(), ReadSpec{}, Clock::now() + 1s);
    CHECK(first.bytes == "ab#");
    const auto second = read_until(link.fd(), ReadSpec{}, Clock::now() + 1s);
    CHECK(second.outcome == ReadOutcome::Terminator);
    CHECK(second.bytes == "late#");
}

// Falsified by: serial_read.h read_until_impl() mapping a negative tty_read() to Timeout instead of LinkDead
TEST_CASE("read_until - a severed pty is LinkDead with EIO", "[serial][serial_read][issue1604]") {
    PtyPair pty("serial_read");
    auto link = open_slave(pty);
    feed(pty.master_fd(), "ab");
    pty.sever();
    const auto start = Clock::now();
    const auto r = read_until(link.fd(), ReadSpec{}, start + 5s);
    CHECK(r.outcome == ReadOutcome::LinkDead);
    CHECK(r.err == EIO);
    CHECK(Clock::now() - start < 2s);
}

// Falsified by: serial_read.h socket_read_byte() returning 0 instead of -1/ECONNRESET when recv() reads 0
TEST_CASE("read_until_socket - a closed peer is LinkDead with ECONNRESET", "[serial][serial_read][issue1604]") {
    SocketPair sp;
    sp.close_b();
    const auto start = Clock::now();
    const auto r = read_until_socket(sp.a, ReadSpec{}, start + 5s);
    CHECK(r.outcome == ReadOutcome::LinkDead);
    CHECK(r.err == ECONNRESET);
    CHECK(Clock::now() - start < 2s);
}

// Falsified by: serial_read.h socket_hangup_errno() omitting ENOTSOCK
TEST_CASE("read_until_socket - a non-socket fd is LinkDead with ENOTSOCK", "[serial][serial_read][issue1604]") {
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    REQUIRE(::write(fds[1], "x", 1) == 1);
    const auto r = read_until_socket(fds[0], ReadSpec{}, Clock::now() + 1s);
    ::close(fds[0]);
    ::close(fds[1]);
    CHECK(r.outcome == ReadOutcome::LinkDead);
    CHECK(r.err == ENOTSOCK);
}

// Falsified by: serial_read.h socket_read_byte() turning a poll() timeout (pr == 0) into -1/ETIMEDOUT instead of 0
TEST_CASE("read_until_socket - terminator, skip, late bytes and deadline", "[serial][serial_read][issue1604]") {
    SocketPair sp;
    feed(sp.b, "\r1\n2#xy");
    const auto r = read_until_socket(sp.a, ReadSpec{}, Clock::now() + 1s);
    CHECK(r.outcome == ReadOutcome::Terminator);
    CHECK(r.bytes == "12#");

    ReadSpec spec;
    spec.stop = ReadSpec::Stop::Length;
    spec.length = 2;
    const auto tail = read_until_socket(sp.a, spec, Clock::now() + 1s);
    CHECK(tail.outcome == ReadOutcome::Length);
    CHECK(tail.bytes == "xy");

    const auto t = read_until_socket(sp.a, ReadSpec{}, Clock::now() + 60ms);
    CHECK(t.outcome == ReadOutcome::Timeout);
    CHECK(t.err == 0);
}

// Falsified by: tty_read() (serial_link.h:161) and read_until_impl() (serial_read.h:143) both not retrying EINTR
TEST_CASE("read_until - a signal during the wait is retried on a tty", "[serial][serial_read][issue1604]") {
    PtyPair pty("serial_read");
    auto link = open_slave(pty);
    std::string bytes;
    const auto outcome =
        read_through_signal([&](const ReadSpec& s, Clock::time_point d) { return read_until(link.fd(), s, d); },
                            [&] { feed(pty.master_fd(), "ok#"); }, &bytes);
    CHECK(outcome == ReadOutcome::Terminator);
    CHECK(bytes == "ok#");
}

// Falsified by: serial_read.h read_until_impl() treating EINTR as an error instead of retrying
TEST_CASE("read_until_socket - a signal during the wait is retried", "[serial][serial_read][issue1604]") {
    SocketPair sp;
    std::string bytes;
    const auto outcome =
        read_through_signal([&](const ReadSpec& s, Clock::time_point d) { return read_until_socket(sp.a, s, d); },
                            [&] { feed(sp.b, "ok#"); }, &bytes);
    CHECK(outcome == ReadOutcome::Terminator);
    CHECK(bytes == "ok#");
}

// ReadSpec::late_byte_grace (issue #1686): the byte in flight when the timeout runs out settles the read. Each case
// gives the read a 100 ms deadline and a 200 ms grace, with "12" waiting in the line and the rest written late.
namespace {

constexpr auto kLateDeadline = 100ms;
constexpr auto kLateGrace = 200ms;

// Writes each (delay from start, bytes) step to @p fd from a helper thread while @p read runs.
template <typename Read>
alpacacore::util::ReadResult read_with_late_writes(int fd, std::vector<std::pair<Clock::duration, std::string>> steps,
                                                   Read read, Clock::duration* took = nullptr) {
    const auto start = Clock::now();
    std::atomic<bool> wrote_all{true};
    std::thread writer([fd, start, steps, &wrote_all] {
        for (const auto& [at, bytes] : steps) {
            std::this_thread::sleep_until(start + at);
            if (::write(fd, bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size())) wrote_all.store(false);
        }
    });
    const auto r = read(start);
    if (took != nullptr) *took = Clock::now() - start;
    writer.join();
    REQUIRE(wrote_all.load());
    return r;
}

ReadSpec late_spec() {
    ReadSpec spec;
    spec.late_byte_grace = kLateGrace;
    return spec;
}

}  // namespace

// Falsified by: serial_read.h read_late_byte() not checking spec.terminators for the late byte (the '#' is kept as a
// timeout)
TEST_CASE("read_until - late_byte_grace: a '#' in flight when the timeout runs out ends the read",
          "[serial][serial_read][issue1686]") {
    PtyPair pty("serial_read");
    auto link = open_slave(pty);
    feed(pty.master_fd(), "12");
    const auto r = read_with_late_writes(pty.master_fd(), {{150ms, "#"}}, [&](Clock::time_point start) {
        return read_until(link.fd(), late_spec(), start + kLateDeadline);
    });
    CHECK(r.outcome == ReadOutcome::Terminator);
    CHECK(r.bytes == "12#");
    CHECK(r.err == 0);

    // Without the grace the same read times out and leaves the '#' for the next one.
    feed(pty.master_fd(), "12");
    const auto strict = read_with_late_writes(pty.master_fd(), {{150ms, "#"}}, [&](Clock::time_point start) {
        return read_until(link.fd(), ReadSpec{}, start + kLateDeadline);
    });
    CHECK(strict.outcome == ReadOutcome::Timeout);
    CHECK(strict.bytes == "12");
    const auto next = read_until(link.fd(), ReadSpec{}, Clock::now() + 1s);
    CHECK(next.bytes == "#");
}

// Falsified by: serial_read.h read_late_byte() reading on after a non-terminator byte in a Terminator read (deleting
// the "spec.stop != ReadSpec::Stop::Length" return ends the read on the length check instead)
TEST_CASE("read_until - late_byte_grace: a late byte that is not a terminator is kept and the read times out",
          "[serial][serial_read][issue1686]") {
    PtyPair pty("serial_read");
    auto link = open_slave(pty);
    feed(pty.master_fd(), "12");
    const auto r = read_with_late_writes(pty.master_fd(), {{150ms, "\r34#"}}, [&](Clock::time_point start) {
        return read_until(link.fd(), late_spec(), start + kLateDeadline);
    });
    CHECK(r.outcome == ReadOutcome::Timeout);
    CHECK(r.bytes == "123");  // the CR is skipped, the '3' is the late byte
    CHECK(r.err == 0);
    const auto next = read_until(link.fd(), ReadSpec{}, Clock::now() + 1s);
    CHECK(next.outcome == ReadOutcome::Terminator);
    CHECK(next.bytes == "4#");
}

// Falsified by: serial_read.h read_late_byte() waiting three times late_byte_grace (the reply that starts 150 ms after
// the grace is taken)
TEST_CASE("read_until - late_byte_grace: silence times out after the grace and a later reply is not taken",
          "[serial][serial_read][issue1686]") {
    PtyPair pty("serial_read");
    auto link = open_slave(pty);
    feed(pty.master_fd(), "12");
    Clock::duration took{};
    const auto r = read_with_late_writes(
        pty.master_fd(), {{kLateDeadline + kLateGrace + 150ms, "ab#"}},
        [&](Clock::time_point start) { return read_until(link.fd(), late_spec(), start + kLateDeadline); }, &took);
    CHECK(r.outcome == ReadOutcome::Timeout);
    CHECK(r.bytes == "12");
    CHECK(r.err == 0);
    CHECK(took >= kLateDeadline + kLateGrace);
    const auto next = read_until(link.fd(), ReadSpec{}, Clock::now() + 1s);
    CHECK(next.outcome == ReadOutcome::Terminator);
    CHECK(next.bytes == "ab#");
}

// Falsified by: serial_read.h read_late_byte() not restarting the grace after a kept byte of a Length read
TEST_CASE("read_until - late_byte_grace: a Length payload in flight is read whole",
          "[serial][serial_read][issue1686]") {
    PtyPair pty("serial_read");
    auto link = open_slave(pty);
    feed(pty.master_fd(), std::string("\x01", 1));
    ReadSpec spec = late_spec();
    spec.stop = ReadSpec::Stop::Length;
    spec.length = 4;
    spec.skip.clear();
    // Each payload byte 120 ms after the last: the last one comes 160 ms after the grace that began at the deadline.
    const auto r = read_with_late_writes(
        pty.master_fd(), {{150ms, std::string("\x02", 1)}, {270ms, std::string("\x0a", 1)}, {390ms, "#"}},
        [&](Clock::time_point start) { return read_until(link.fd(), spec, start + kLateDeadline); });
    CHECK(r.outcome == ReadOutcome::Length);
    CHECK(r.bytes == std::string("\x01\x02\x0a#", 4));
}

// Falsified by: serial_read.h read_late_byte() not checking spec.terminators for the late byte
TEST_CASE("read_until_socket - late_byte_grace: a '#' in flight when the timeout runs out ends the read",
          "[serial][serial_read][issue1686]") {
    SocketPair sp;
    feed(sp.b, "12");
    const auto r = read_with_late_writes(sp.b, {{150ms, "#"}}, [&](Clock::time_point start) {
        return read_until_socket(sp.a, late_spec(), start + kLateDeadline);
    });
    CHECK(r.outcome == ReadOutcome::Terminator);
    CHECK(r.bytes == "12#");
}

// Falsified by: serial_read.h read_late_byte() not reporting a hangup errno as LinkDead (the severed line reads as a
// Timeout)
TEST_CASE("read_until - late_byte_grace: a hangup during the grace is LinkDead", "[serial][serial_read][issue1686]") {
    PtyPair pty("serial_read");
    auto link = open_slave(pty);
    feed(pty.master_fd(), "12");
    const auto start = Clock::now();
    std::thread severer([&pty, start] {
        std::this_thread::sleep_until(start + 150ms);
        pty.sever();
    });
    const auto r = read_until(link.fd(), late_spec(), start + kLateDeadline);
    severer.join();
    CHECK(r.outcome == ReadOutcome::LinkDead);
    CHECK(r.bytes == "12");
    CHECK(r.err == EIO);
}

// Falsified by: serial_read.h read_late_byte() not reporting a hangup errno as LinkDead (the closed peer reads as a
// Timeout)
TEST_CASE("read_until_socket - late_byte_grace: a peer that closes during the grace is LinkDead",
          "[serial][serial_read][issue1686]") {
    SocketPair sp;
    feed(sp.b, "12");
    const auto start = Clock::now();
    std::thread closer([&sp, start] {
        std::this_thread::sleep_until(start + 150ms);
        sp.close_b();
    });
    const auto r = read_until_socket(sp.a, late_spec(), start + kLateDeadline);
    closer.join();
    CHECK(r.outcome == ReadOutcome::LinkDead);
    CHECK(r.bytes == "12");
    CHECK(r.err == ECONNRESET);
}
