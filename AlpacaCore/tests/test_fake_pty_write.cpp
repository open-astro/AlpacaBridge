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
//
// The PtyPair cases below pin the ownership contract from issue #387: the
// pair owns both descriptors, nothing outlives it, sever() closes both ends
// and forgets the path, and a setup failure after the master is open throws
// with the master closed. That last one is the leak the issue reported, and
// it is forced from inside the process: with the soft RLIMIT_NOFILE lowered
// and every slot but one filled, posix_openpt() takes the last descriptor and
// the keep-alive open() fails with EMFILE. Against the hand-rolled block the
// pair replaced, that construction SUCCEEDED with a dead keep-alive, so the
// case is red without the fix.

#ifndef _WIN32

// Set when this translation unit is built under a sanitizer, by either gcc's
// predefined macro or __has_feature. Used by the EMFILE case below, which
// cannot run under a sanitizer that needs a descriptor of its own.
//
// UBSan is the one that actually kills that case (its vptr check on Catch2's
// expression decomposer -- see the case's own note), and it is the one with
// no predefined macro: gcc has __SANITIZE_ADDRESS__/__SANITIZE_THREAD__ but
// no __SANITIZE_UNDEFINED__. It is detectable all the same --
// __has_feature(undefined_behavior_sanitizer) answers 1 under gcc 14's
// -fsanitize=undefined (measured on Debian 13, gcc 14.2) -- so the
// __has_feature branch covers it. The predefined-macro branch above stays
// first for gcc < 14, which has no __has_feature at all; a UBSan-only build
// on such a compiler is still undetectable, but every build this repo runs
// pairs address with undefined and so trips the first branch anyway.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define ALPACACORE_TESTS_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#define ALPACACORE_TESTS_SANITIZED 1
#endif
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>  // posix_openpt/grantpt/unlockpt/ptsname: POSIX, not the <cstdlib> subset
#include <sys/resource.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "fake_pty_write.h"
#include "fake_serial_streamer.h"

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
    // The drained case below reads from the slave; the undrained cases simply
    // never touch it, which is what leaves the buffer full.
    int slave() const { return slave_fd_; }

private:
    int master_fd_ = -1;
    int slave_fd_ = -1;
};

long elapsed_ms(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
}

// Descriptors this process holds open (count) and the highest number among
// them (max_fd), from /proc/self/fd; count is -1 where that directory does
// not exist (a non-Linux host). The directory handle used to read it is
// excluded, so the walk does not count itself.
struct FdTable {
    int count = -1;
    int max_fd = -1;
};

FdTable fd_table() {
    FdTable t;
    DIR* dir = opendir("/proc/self/fd");
    if (dir == nullptr) {
        return t;
    }
    const int self = dirfd(dir);
    t.count = 0;
    while (dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        const int fd = std::stoi(entry->d_name);
        if (fd == self) continue;
        ++t.count;
        if (fd > t.max_fd) t.max_fd = fd;
    }
    closedir(dir);
    return t;
}

int open_fd_count() { return fd_table().count; }

// Lowers the soft RLIMIT_NOFILE for the duration of a case and puts it back
// on every exit, including a failed REQUIRE.
class ScopedFdLimit {
public:
    explicit ScopedFdLimit(rlim_t soft) {
        ok_ = getrlimit(RLIMIT_NOFILE, &saved_) == 0;
        if (ok_) {
            rlimit tight = saved_;
            tight.rlim_cur = soft;
            ok_ = setrlimit(RLIMIT_NOFILE, &tight) == 0;
        }
    }
    ~ScopedFdLimit() {
        if (ok_) setrlimit(RLIMIT_NOFILE, &saved_);
    }
    bool ok() const { return ok_; }

private:
    rlimit saved_{};
    bool ok_ = false;
};

// Closes every descriptor it holds on scope exit, whichever way the case
// leaves.
struct FdHoard {
    std::vector<int> fds;
    ~FdHoard() {
        for (int fd : fds) ::close(fd);
    }
};

}  // namespace

TEST_CASE("PtyPair - owns both descriptors: a construct/destroy loop leaves the fd table unchanged",
          "[fakes][pty][unit]") {
    // The baseline is taken immediately before the loop and compared
    // immediately after it, with nothing else in between, so only what the
    // loop itself opened can move the count.
    const int before = open_fd_count();
    if (before < 0) {
        WARN("/proc/self/fd is not available on this host; the ownership check is skipped");
        return;
    }
    for (int i = 0; i < 32; ++i) {
        PtyPair pty("PtyPair test");
        REQUIRE(pty.master_fd() >= 0);
        REQUIRE(pty.keepalive_fd() >= 0);
        REQUIRE_FALSE(pty.slave_path().empty());
        // The master is non-blocking from construction: the #424 property,
        // now set inside the pair rather than by each fake.
        CHECK((fcntl(pty.master_fd(), F_GETFL, 0) & O_NONBLOCK) != 0);
    }
    // The leak check: the pair must give back exactly what it took.
    CHECK(open_fd_count() == before);
}

TEST_CASE("PtyPair - a keep-alive open that fails throws, with the master closed", "[fakes][pty][unit]") {
    // The #387 leak, forced in-process. Lower the soft descriptor limit to
    // just above the highest descriptor already open, fill every free slot
    // below it with dup(), then free exactly one: posix_openpt() takes that
    // slot and the keep-alive open() of the slave fails with EMFILE. The
    // pair must throw and must not leave the master behind. The hand-rolled
    // block this replaced constructed successfully here, with the keep-alive
    // at -1 and a confusing EIO waiting on the first read; against it the
    // REQUIRE_THROWS_AS below is the red line.
    //
    // The limit is process-wide while this block runs, so anything the
    // runtime lazily opens inside it fails too: a sanitizer's symbolizer, or
    // Catch2's own report if a REQUIRE here goes red. The window is a few
    // syscalls wide and the limit is restored on every exit path; if it ever
    // flakes under the ASan/TSan jobs, gate this case out there rather than
    // loosening the check.
    //
    // Taking that instruction (issue #586, which made the `sanitizers` job
    // actually run this suite): under -fsanitize=address,undefined this case
    // fails DETERMINISTICALLY, not flakily. UBSan's vptr check on Catch2's
    // expression decomposer has to reach the runtime the first time a
    // REQUIRE is decomposed inside the EMFILE window, and cannot, so the
    // case dies on `member access within address ... does not point to an
    // object of type 'BinaryExpr'` with no stack trace -- the symbolizer
    // needs a descriptor too. That is the sanitizer being unable to observe
    // the case, not the case finding a defect. It still runs for real in
    // build-test and build-vendors, which is where its coverage lives.
#if defined(ALPACACORE_TESTS_SANITIZED)
    WARN("built with sanitizers; the EMFILE setup-failure check is skipped (see the note above)");
    // Keep the case from ending with zero assertions: WARN is not one, so a
    // runner started with -w NoAssertions would fail a case we skipped on
    // purpose.
    SUCCEED("skipped under sanitizers");
    return;
#endif
    const FdTable start = fd_table();
    if (start.count < 0) {
        WARN("/proc/self/fd is not available on this host; the setup-failure check is skipped");
        return;
    }
    ScopedFdLimit limit(static_cast<rlim_t>(start.max_fd + 4));
    REQUIRE(limit.ok());
    FdHoard hoard;
    for (;;) {
        const int fd = ::dup(STDERR_FILENO);
        if (fd < 0) {
            REQUIRE(errno == EMFILE);
            break;
        }
        hoard.fds.push_back(fd);
    }
    REQUIRE_FALSE(hoard.fds.empty());
    ::close(hoard.fds.back());  // exactly one free slot, for the master
    hoard.fds.pop_back();
    const int filled = open_fd_count();  // uses the free slot briefly, then gives it back
    REQUIRE(filled >= 0);

    REQUIRE_THROWS_AS(PtyPair("PtyPair test"), std::runtime_error);
    // Nothing left open: the master the constructor took has been closed.
    CHECK(open_fd_count() == filled);
}

TEST_CASE("PtyPair - sever closes both ends, forgets the slave path, and is idempotent", "[fakes][pty][unit]") {
    PtyPair pty("PtyPair test");
    // What a driver does with the path: open the slave.
    const int driver_fd = ::open(pty.slave_path().c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    REQUIRE(driver_fd >= 0);
    // Baseline taken immediately before the call under test: the two
    // descriptors the pair owns plus the driver's one.
    const int before = open_fd_count();

    pty.sever();
    CHECK(pty.master_fd() == -1);
    CHECK(pty.keepalive_fd() == -1);
    // The path named a pty that no longer exists; handing it out again would
    // let a test reopen a recycled /dev/pts/N.
    CHECK(pty.slave_path().empty());
    if (before >= 0) {
        CHECK(open_fd_count() == before - 2);  // only the driver's own fd remains
    }
    // The driver's side sees the unplug (issue #237): a write to the orphaned
    // slave fails with EIO, and a read returns at once with no data (0 or
    // EIO, depending on whether the input queue had drained; either is the
    // hangup, neither is a byte). errno is captured right after the syscall,
    // before any assertion machinery can run between the two.
    const ssize_t wrote = ::write(driver_fd, "x", 1);
    const int write_err = errno;
    CHECK(wrote < 0);
    CHECK(write_err == EIO);
    char buf[8];
    CHECK(::read(driver_fd, buf, sizeof(buf)) <= 0);

    pty.sever();  // a second sever, and then the destructor, must be no-ops
    CHECK(pty.master_fd() == -1);
    ::close(driver_fd);
}

TEST_CASE("fake pty write - make_pty_nonblocking actually sets O_NONBLOCK", "[fakes][pty][unit]") {
    UndrainedPty pty;
    REQUIRE(pty.ok());

    // The pre-condition is libc/kernel behaviour, not something this helper
    // owns -- it is asserted only to show the next line is doing real work,
    // and a platform whose posix_openpt() ever returned a non-blocking master
    // would make this line, not the helper, the thing that went red.
    REQUIRE((fcntl(pty.master(), F_GETFL, 0) & O_NONBLOCK) == 0);
    // The property the whole fix rests on: without it, a write to a full pty
    // parks forever.
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
    // Uses the same RAII helper as the cases above: opening the pair inline
    // leaked both descriptors whenever one of these REQUIREs fired.
    UndrainedPty pty;
    REQUIRE(pty.ok());
    const int master = pty.master();
    const int slave = pty.slave();
    REQUIRE(make_pty_nonblocking(master));

    const std::atomic<bool> stop{false};
    const std::string reply = "*G0000U000000A1T1D1M1D1M1B50C50S20H15V50P10D12C1B12#";
    // Issue #513: this was a CHECK, so a short write fell through into the
    // unbounded read loop below instead of stopping the case here -- in a
    // file whose entire purpose is proving that fake-pty writes no longer
    // hang. REQUIRE so a short write fails fast.
    REQUIRE(pty_write_bounded(master, reply, stop) == reply.size());

    // The slave was opened blocking (UndrainedPty), so a read that never gets
    // its remaining bytes -- exactly what a short write above would cause --
    // would otherwise block forever. Bound each read with poll() against a
    // deadline, consistent with how pty_write_bounded itself is bounded, so
    // that failure mode is a clean CHECK rather than a hung test process.
    std::string received;
    received.resize(reply.size());
    std::size_t got = 0;
    // Declared here, not inside the `if (ready <= 0)` block below, so the
    // message survives the `break` that follows it (issue #560: a bare
    // INFO(...) there is an unnamed Catch2 ScopedMessage, destroyed at the
    // closing brace of that `if` before the CHECK below ever sees it).
    std::string poll_err;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (got < reply.size()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            break;
        }
        const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        pollfd pfd{};  // no `struct` tag: elaborated-type-specifier + braced-init is the one
                       // construct clang-format 19 (trixie) and CI's unpinned apt clang-format
                       // disagree on spacing for; a plain declaration formats identically on both.
        pfd.fd = slave;
        pfd.events = POLLIN;
        const int ready = ::poll(&pfd, 1, static_cast<int>(remaining_ms));
        if (ready < 0 && errno == EINTR) {
            continue;  // interrupted (profiler/debugger/SIGCHLD) -- re-poll against the same deadline
        }
        if (ready <= 0) {
            // Only a negative return is a poll() failure; ready == 0 is a plain
            // timeout, which leaves errno untouched, so reading it there would
            // report whatever stale value happened to be sitting in it.
            if (ready < 0) poll_err = std::strerror(errno);
            break;
        }
        const ssize_t n = read(slave, received.data() + got, reply.size() - got);
        if (n <= 0) break;
        got += static_cast<std::size_t>(n);
    }
    CAPTURE(poll_err);
    CHECK(got == reply.size());
    CHECK(received == reply);
}

// The end-to-end shape #424 actually reported: a real fake, nothing draining
// its pty, destroyed. The helper cases above pin the mechanism; this one pins
// that a fake built on it cannot hang its own destructor.
//
// FakeSerialStreamer is the one that needs no driver to reach the state --
// it writes its frame on an interval whether or not anyone reads, so the pty
// buffer fills on its own within a few hundred milliseconds.
TEST_CASE("fake pty write - a fake whose pty is never drained still destructs", "[fakes][pty][unit]") {
    using namespace std::chrono_literals;

    // Big frame, short interval: fill the buffer well before the destruction.
    auto* streamer = new alpacacore::test::FakeSerialStreamer(std::string(4096, 'x'), 1ms);
    std::this_thread::sleep_for(400ms);

    // The destruction runs on its own thread so a hang is a FAILED assertion
    // rather than a hung test process -- which is the whole problem with this
    // bug: the old code gave no assertion to fail, it just stopped.
    // The promise is owned by a shared_ptr captured BY VALUE, and the
    // streamer pointer is captured by value too: on the failure path this
    // thread is detached and can outlive the TEST_CASE, so anything it
    // touches after that point must not live on this stack. Capturing the
    // promise by reference would turn a clean FAILED into a write to a
    // destroyed std::promise -- a crash, or a corrupted later case, in the
    // run we are trying to report on.
    auto destroyed = std::make_shared<std::promise<void>>();
    auto done = destroyed->get_future();
    std::thread destroyer([streamer, destroyed] {
        delete streamer;
        destroyed->set_value();
    });

    const bool finished = done.wait_for(10s) == std::future_status::ready;
    if (finished) {
        destroyer.join();
    } else {
        // Deliberately leaked: the thread is parked inside the destructor and
        // joining it would hang the run we are trying to report on. It keeps
        // the promise alive through the shared_ptr it holds.
        destroyer.detach();
    }
    CHECK(finished);
}

// The first-frame hold promises "no frame is sent until that long after the worker started". The worker starts in the
// constructor and streams at once, so the hold has to be a constructor argument: a setter called after the
// constructor returns races the first frames (it let 24 bytes through at 50 ms). Nothing may reach the slave.
TEST_CASE("fake serial streamer - the first-frame hold holds every frame", "[fakes][pty][unit]") {
    using namespace std::chrono_literals;

    alpacacore::test::FakeSerialStreamer streamer("FRAME\n", 10ms, 300ms);
    std::this_thread::sleep_for(50ms);

    const int fd = ::open(streamer.slave_path().c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK);
    REQUIRE(fd >= 0);
    termios tio{};
    REQUIRE(::tcgetattr(fd, &tio) == 0);
    ::cfmakeraw(&tio);
    REQUIRE(::tcsetattr(fd, TCSANOW, &tio) == 0);

    std::size_t received = 0;
    const auto deadline = std::chrono::steady_clock::now() + 100ms;
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, 10) > 0) {
            char buf[64];
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n > 0) received += static_cast<std::size_t>(n);
        }
    }
    ::close(fd);
    CHECK(received == 0);
}

#endif  // _WIN32
