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

// The shared serial-port ownership helpers (issue #1202): registry claims and
// SerialLink, tested directly over a pty pair.

#include <alpacacore/util/serial_link.h>
#include <alpacacore/util/serial_port_registry.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <mutex>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "fake_pty_write.h"

using alpacacore::util::is_serial_port_in_use;
using alpacacore::util::make_raw_8n1;
using alpacacore::util::SerialClaimKind;
using alpacacore::util::SerialLink;
using alpacacore::util::SerialPortClaim;
using alpacacore::util::SerialPortHolder;
using alpacacore::util::tty_read;
using alpacacore::util::tty_write_all;

// Test-binary-only interposer for tcsetattr (issue #1439): forwards to the real
// one, but parks a call on the latched fd until the test releases it, standing
// in for a wedged USB-serial adapter. Every other fd passes straight through.
namespace {
struct TcsetattrLatch {
    std::mutex m;
    std::condition_variable cv;
    int fd = -1;  // -1: disarmed
    bool entered = false;
    bool released = false;
};
TcsetattrLatch& tcsetattr_latch() {
    static auto* l = new TcsetattrLatch();
    return *l;
}
}  // namespace

extern "C" int tcsetattr(int fd, int optional_actions, const struct termios* tio) __THROW {
    using Fn = int (*)(int, int, const struct termios*);
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "tcsetattr"));
    auto& l = tcsetattr_latch();
    {
        std::unique_lock<std::mutex> lock(l.m);
        if (l.fd == fd) {
            l.entered = true;
            l.cv.notify_all();
            l.cv.wait(lock, [&] { return l.released; });
        }
    }
    return real(fd, optional_actions, tio);
}

namespace {

void raw_9600(struct termios& tty) { make_raw_8n1(tty, B9600); }
void raw_19200(struct termios& tty) { make_raw_8n1(tty, B19200); }

std::string unique_path(const char* tag) {
    static std::atomic<int> n{0};
    return std::string("/nonexistent/serial-link-test-") + tag + "-" + std::to_string(n++);
}

}  // namespace

TEST_CASE("Serial port claim - a connected link refuses a second link and a probe",
          "[serial][serial_link][issue1202]") {
    const std::string path = unique_path("link");
    auto first = SerialPortClaim::acquire(path, SerialClaimKind::Link, "Gemini PDH");
    REQUIRE(first);
    REQUIRE(is_serial_port_in_use(path));

    SerialPortHolder holder;
    const auto t0 = std::chrono::steady_clock::now();
    auto second =
        SerialPortClaim::acquire(path, SerialClaimKind::Link, "WandererRotator", std::chrono::seconds(5), &holder);
    auto probe = SerialPortClaim::acquire(path, SerialClaimKind::Probe, "Gemini flat panel", std::chrono::seconds(5));
    // A link-held port is refused at once, never waited for.
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
    CHECK_FALSE(second);
    CHECK_FALSE(probe);
    CHECK(holder.kind == SerialClaimKind::Link);
    CHECK(holder.owner == "Gemini PDH");
    CHECK(alpacacore::util::serial_port_held_message(path, holder).find("Gemini PDH") != std::string::npos);

    first.release();
    CHECK_FALSE(is_serial_port_in_use(path));
    CHECK(SerialPortClaim::acquire(path, SerialClaimKind::Link, "WandererRotator"));
}

TEST_CASE("Serial port claim - a probe waits for another probe to finish", "[serial][serial_link][issue1202]") {
    const std::string path = unique_path("probe");
    auto first = SerialPortClaim::acquire(path, SerialClaimKind::Probe, "iOptron mount");
    REQUIRE(first);

    auto waiter = std::async(std::launch::async, [&] {
        return SerialPortClaim::acquire(path, SerialClaimKind::Link, "OnStep", std::chrono::seconds(5)).held();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    first.release();
    REQUIRE(waiter.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    CHECK(waiter.get());
    CHECK_FALSE(is_serial_port_in_use(path));  // the waiter's claim died with its future's value
}

TEST_CASE("Serial port claim - waiting for a probe is bounded", "[serial][serial_link][issue1202]") {
    const std::string path = unique_path("bounded");
    auto first = SerialPortClaim::acquire(path, SerialClaimKind::Probe, "QHY CFW3");
    REQUIRE(first);
    SerialPortHolder holder;
    const auto t0 = std::chrono::steady_clock::now();
    auto second = SerialPortClaim::acquire(path, SerialClaimKind::Probe, "Gemini focuser",
                                           std::chrono::milliseconds(200), &holder);
    const auto waited = std::chrono::steady_clock::now() - t0;
    CHECK_FALSE(second);
    CHECK(waited >= std::chrono::milliseconds(150));
    CHECK(waited < std::chrono::seconds(2));
    CHECK(holder.kind == SerialClaimKind::Probe);
    CHECK(holder.owner == "QHY CFW3");
}

TEST_CASE("Serial port claim - moves transfer ownership and a symlink names the same port",
          "[serial][serial_link][issue1202]") {
    alpacacore::test::PtyPair pty("claim-move");
    const auto link_path =
        std::filesystem::temp_directory_path() / ("serial-link-test-" + std::to_string(::getpid()) + "-by-id");
    std::error_code ec;
    std::filesystem::remove(link_path, ec);
    std::filesystem::create_symlink(pty.slave_path(), link_path, ec);
    REQUIRE_FALSE(ec);

    SerialPortClaim outer;
    {
        auto inner = SerialPortClaim::acquire(link_path.string(), SerialClaimKind::Link, "SynScan");
        REQUIRE(inner);
        CHECK(inner.key() == pty.slave_path());
        outer = std::move(inner);
    }
    // The moved-from claim's destructor released nothing.
    CHECK(is_serial_port_in_use(pty.slave_path()));
    CHECK_FALSE(
        SerialPortClaim::acquire(pty.slave_path(), SerialClaimKind::Probe, "Celestron", std::chrono::milliseconds(0)));
    outer = SerialPortClaim();
    CHECK_FALSE(is_serial_port_in_use(pty.slave_path()));
    std::filesystem::remove(link_path, ec);
}

TEST_CASE("Serial link - open claims the port and refuses a second driver before touching it",
          "[serial][serial_link][issue1202]") {
    alpacacore::test::PtyPair pty("link-refuse");
    std::string error;
    auto rotator = SerialLink::open(pty.slave_path(), SerialClaimKind::Link, "WandererRotator", raw_19200, &error);
    REQUIRE(rotator);
    REQUIRE(error.empty());

    struct termios before {};
    REQUIRE(tcgetattr(pty.keepalive_fd(), &before) == 0);
    REQUIRE(cfgetospeed(&before) == B19200);

    auto focuser = SerialLink::open(pty.slave_path(), SerialClaimKind::Link, "Gemini focuser", raw_9600, &error);
    CHECK_FALSE(focuser);
    CHECK(error.find("WandererRotator") != std::string::npos);

    struct termios after {};
    REQUIRE(tcgetattr(pty.keepalive_fd(), &after) == 0);
    CHECK(cfgetospeed(&after) == B19200);  // the refused driver never retuned it

    rotator.close();
    CHECK_FALSE(is_serial_port_in_use(pty.slave_path()));
    CHECK(SerialLink::open(pty.slave_path(), SerialClaimKind::Link, "Gemini focuser", raw_9600));
}

TEST_CASE("Serial link - another program cannot open a held port, and can after close",
          "[serial][serial_link][issue1202]") {
    if (::geteuid() == 0) {
        WARN("running as root: TIOCEXCL does not apply to CAP_SYS_ADMIN, skipping");
        return;
    }
    alpacacore::test::PtyPair pty("link-excl");
    auto link = SerialLink::open(pty.slave_path(), SerialClaimKind::Link, "OnStep", raw_9600);
    REQUIRE(link);

    const int outsider = ::open(pty.slave_path().c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    const int outsider_errno = errno;
    if (outsider >= 0) ::close(outsider);
    CHECK(outsider < 0);
    CHECK(outsider_errno == EBUSY);

    // The keep-alive fd keeps the tty open, so the exclusive flag would outlive
    // the link unless close() clears it.
    link.close();
    const int after = ::open(pty.slave_path().c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    CHECK(after >= 0);
    if (after >= 0) ::close(after);
}

TEST_CASE("Serial link - a read returns on time after the port is retuned to VMIN=1, and the settings come back",
          "[serial][serial_link][issue1202]") {
    alpacacore::test::PtyPair pty("link-retune");
    auto link = SerialLink::open(pty.slave_path(), SerialClaimKind::Link, "OnStep", raw_19200);
    REQUIRE(link);

    // Another program (opened before us, so TIOCEXCL did not stop it) retunes
    // the line the way the #1202 GPS scanner did.
    struct termios tty {};
    REQUIRE(tcgetattr(pty.keepalive_fd(), &tty) == 0);
    cfsetispeed(&tty, B9600);
    cfsetospeed(&tty, B9600);
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;
    REQUIRE(tcsetattr(pty.keepalive_fd(), TCSANOW, &tty) == 0);

    char ch = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const ssize_t n = tty_read(link.fd(), &ch, 1, 200);
    const auto waited = std::chrono::steady_clock::now() - t0;
    CHECK(n == 0);
    CHECK(waited < std::chrono::seconds(2));

    // The timeout put the driver's settings back.
    struct termios now {};
    REQUIRE(tcgetattr(pty.keepalive_fd(), &now) == 0);
    CHECK(cfgetospeed(&now) == B19200);
}

TEST_CASE("Serial link - reads and writes go through the pty", "[serial][serial_link][issue1202]") {
    alpacacore::test::PtyPair pty("link-io");
    auto link = SerialLink::open(pty.slave_path(), SerialClaimKind::Link, "iOptron iEAF", raw_9600);
    REQUIRE(link);
    CHECK(link.node_present());

    REQUIRE(tty_write_all(link.fd(), std::string(":GP#")));
    char buf[16] = {};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::string got;
    while (got.size() < 4 && std::chrono::steady_clock::now() < deadline) {
        const ssize_t n = ::read(pty.master_fd(), buf, sizeof(buf));
        if (n > 0) got.append(buf, static_cast<std::size_t>(n));
    }
    CHECK(got == ":GP#");

    const char reply[] = "1234#";
    REQUIRE(::write(pty.master_fd(), reply, 5) == 5);
    std::string answer;
    while (answer.size() < 5) {
        const ssize_t n = tty_read(link.fd(), buf, sizeof(buf), 1000);
        if (n <= 0) break;
        answer.append(buf, static_cast<std::size_t>(n));
    }
    CHECK(answer == "1234#");
}

TEST_CASE("Serial link - a severed pty reads as an error, not as silence", "[serial][serial_link][issue1202]") {
    alpacacore::test::PtyPair pty("link-sever");
    auto link = SerialLink::open(pty.slave_path(), SerialClaimKind::Link, "Gemini PDH", raw_9600);
    REQUIRE(link);
    pty.sever();
    char ch = 0;
    CHECK(tty_read(link.fd(), &ch, 1, 500) < 0);
    CHECK_FALSE(link.node_present());
}

// Falsified by: serial_link.h tty_read() passing poll() the remaining wait rounded down (a wait under 1 ms polls 0 and
// returns at once, so read_until spins through its last millisecond)
TEST_CASE("Serial link - a 1 ms read on a silent line waits the whole millisecond",
          "[serial][serial_link][issue1696]") {
    alpacacore::test::PtyPair pty("link-wait");
    auto link = SerialLink::open(pty.slave_path(), SerialClaimKind::Link, "Gemini focuser", raw_9600);
    REQUIRE(link);
    for (int i = 0; i < 20; ++i) {
        char ch = 0;
        const auto start = std::chrono::steady_clock::now();
        const ssize_t n = tty_read(link.fd(), &ch, 1, 1);
        const auto took = std::chrono::steady_clock::now() - start;
        INFO("read " << i << " took " << std::chrono::duration_cast<std::chrono::microseconds>(took).count() << " us");
        CHECK(n == 0);
        CHECK(took >= std::chrono::milliseconds(1));
    }
}

TEST_CASE("Serial link - a probe skips a port a connected driver holds without opening it",
          "[serial][serial_link][issue1202]") {
    alpacacore::test::PtyPair pty("link-probe");
    auto link = SerialLink::open(pty.slave_path(), SerialClaimKind::Link, "Gemini PDH", raw_9600);
    REQUIRE(link);
    std::string error;
    const auto t0 = std::chrono::steady_clock::now();
    auto probe = SerialLink::open(pty.slave_path(), SerialClaimKind::Probe, "Gemini flat panel", raw_19200, &error);
    CHECK_FALSE(probe);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
    struct termios now {};
    REQUIRE(tcgetattr(pty.keepalive_fd(), &now) == 0);
    CHECK(cfgetospeed(&now) == B9600);
}

TEST_CASE("Serial port claim - a scan counts a sibling's port and skips another kind's",
          "[serial][serial_link][issue1202]") {
    using alpacacore::util::serial_candidate_use;
    using alpacacore::util::SerialCandidateUse;
    const std::string path = unique_path("candidate");
    CHECK(serial_candidate_use(path, "Gemini focuser") == SerialCandidateUse::Free);
    {
        auto probe = SerialPortClaim::acquire(path, SerialClaimKind::Probe, "iOptron mount");
        REQUIRE(probe);
        // Another scan's probe is waited out by the probe's own claim, not skipped.
        CHECK(serial_candidate_use(path, "Gemini focuser") == SerialCandidateUse::Free);
    }
    auto link = SerialPortClaim::acquire(path, SerialClaimKind::Link, "Gemini focuser");
    REQUIRE(link);
    CHECK(serial_candidate_use(path, "Gemini focuser") == SerialCandidateUse::HeldBySibling);
    CHECK(serial_candidate_use(path, "Gemini flat panel") == SerialCandidateUse::HeldByOther);
}

TEST_CASE("Serial link - a retune of VMIN/VTIME alone is put back too", "[serial][serial_link][issue1202]") {
    alpacacore::test::PtyPair pty("link-vmin");
    auto link = SerialLink::open(pty.slave_path(), SerialClaimKind::Link, "WandererRotator", raw_19200);
    REQUIRE(link);
    struct termios tty {};
    REQUIRE(tcgetattr(pty.keepalive_fd(), &tty) == 0);
    tty.c_cc[VMIN] = 1;  // the #1202 retune, baud unchanged
    tty.c_cc[VTIME] = 0;
    REQUIRE(tcsetattr(pty.keepalive_fd(), TCSANOW, &tty) == 0);

    CHECK(alpacacore::util::restore_serial_line_settings(link.fd()));
    struct termios now {};
    REQUIRE(tcgetattr(pty.keepalive_fd(), &now) == 0);
    CHECK(now.c_cc[VMIN] == 0);
    CHECK(cfgetospeed(&now) == B19200);
    CHECK_FALSE(alpacacore::util::restore_serial_line_settings(link.fd()));  // nothing left to restore
}

TEST_CASE("Serial link - a stalled line restore on one port does not block close or open on another",
          "[serial][serial_link][issue1439]") {
    alpacacore::test::PtyPair pty_a("link-stall-a");
    alpacacore::test::PtyPair pty_b("link-stall-b");
    alpacacore::test::PtyPair pty_c("link-stall-c");
    auto link_a = SerialLink::open(pty_a.slave_path(), SerialClaimKind::Link, "OnStep", raw_19200);
    auto link_b = SerialLink::open(pty_b.slave_path(), SerialClaimKind::Link, "WandererRotator", raw_19200);
    REQUIRE(link_a);
    REQUIRE(link_b);

    // Another program retunes A, so the restore on A reaches tcsetattr().
    struct termios tty {};
    REQUIRE(tcgetattr(pty_a.keepalive_fd(), &tty) == 0);
    cfsetispeed(&tty, B9600);
    cfsetospeed(&tty, B9600);
    REQUIRE(tcsetattr(pty_a.keepalive_fd(), TCSANOW, &tty) == 0);

    auto& latch = tcsetattr_latch();
    {
        std::lock_guard<std::mutex> lock(latch.m);
        latch.fd = link_a.fd();
        latch.entered = false;
        latch.released = false;
    }
    auto release = [&] {
        std::lock_guard<std::mutex> lock(latch.m);
        latch.released = true;
        latch.fd = -1;
        latch.cv.notify_all();
    };

    const int fd_a = link_a.fd();
    auto restoring =
        std::async(std::launch::async, [fd_a] { return alpacacore::util::restore_serial_line_settings(fd_a); });
    {
        std::unique_lock<std::mutex> lock(latch.m);
        const bool in = latch.cv.wait_for(lock, std::chrono::seconds(5), [&] { return latch.entered; });
        if (!in) {
            lock.unlock();
            release();
            restoring.wait();
            FAIL("restore on A never reached tcsetattr");
        }
    }

    const auto bound = std::chrono::milliseconds(500);
    auto closing = std::async(std::launch::async, [&] { link_b.close(); });
    const bool close_done = closing.wait_for(bound) == std::future_status::ready;

    auto opening = std::async(std::launch::async, [&] {
        return SerialLink::open(pty_c.slave_path(), SerialClaimKind::Link, "Gemini PDH", raw_19200);
    });
    const bool open_done = opening.wait_for(bound) == std::future_status::ready;

    release();  // never leave the red path hanging
    CHECK(restoring.get());
    closing.wait();
    auto link_c = opening.get();

    CHECK(close_done);  // B.close() must not wait for A's tcsetattr
    CHECK(open_done);   // open + apply on C must not wait for it either
    CHECK(link_c);

    struct termios now {};
    REQUIRE(tcgetattr(pty_a.keepalive_fd(), &now) == 0);
    CHECK(cfgetospeed(&now) == B19200);
}
