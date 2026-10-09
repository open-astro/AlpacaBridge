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

// Serial-transport tests for the Sky-Watcher protocol wrapper, over a
// pty-backed fake motor controller (fake_skywatcher_serial_board.h). These
// drive exchange_serial() itself: the late-reply settle after a timeout, the
// mis-paired-reply shape check and resend, and the ":i" readback's failure
// classification. The UDP loopback tests in test_skywatcher_async.cpp cannot
// reach any of this.

#ifndef _WIN32

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/vendor/skywatcher/skywatcher_protocol_wrapper.h>
#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

#include "catch2_compat.h"
#include "fake_skywatcher_serial_board.h"

using alpacacore::test::FakeSkyWatcherSerialBoard;
namespace sw = alpacacore::vendor::skywatcher;

namespace {

struct SerialLink {
    FakeSkyWatcherSerialBoard board;
    sw::SkyWatcherProtocolWrapper& proto = sw::SkyWatcherProtocolWrapper::instance();

    explicit SerialLink(int timeout_ms = 300) {
        sw::ConnectionInfo info;
        info.type = sw::ConnectionType::Serial;
        info.port_path = board.slave_path();
        info.baud_rate = 9600;
        info.response_timeout_ms = timeout_ms;
        REQUIRE(proto.connect(info));
        REQUIRE_FALSE(proto.get_motor_board_version().empty());  // ":e1" answered
    }
    ~SerialLink() { proto.disconnect(); }
};

}  // namespace

TEST_CASE("SkyWatcher serial - a reply that arrives after the timeout is not read as the next command's answer",
          "[skywatcher][serial]") {
    // A plain tcflush before the next write only discards bytes that have
    // ALREADY arrived. If the previous reply is still in flight it lands
    // after the flush and is consumed as this command's reply: here a ":j1"
    // answered with the STALE counts. exchange_serial marks the link dirty
    // after a timeout and settles (waits for the line to go quiet) before the
    // next write instead.
    SerialLink link(300);
    link.board.set_counts(1, 0x800000);
    // Lands 100 ms after the wrapper gives up, inside the 200 ms settle
    // window. A reply later than that window is only caught by the shape
    // check, which cannot help when it is the SAME command's shape.
    link.board.delay_next_reply(400);
    REQUIRE_THROWS_AS(link.proto.inquire_position(1), alpacacore::AlpacaException);

    link.board.set_counts(1, 0x812345);
    // The late "=…800000" reply is now on the line (or about to be). The next
    // inquiry must return the NEW counts, i.e. its own reply.
    REQUIRE(link.proto.inquire_position(1) == 0x812345);
}

TEST_CASE("SkyWatcher serial - a mis-paired OK reply is rejected and the command resent once", "[skywatcher][serial]") {
    // An OK reply whose data length does not match the command is a reply to
    // something else. Before the shape check, ":j1" answered with "=00" would
    // have been decoded as counts 0 (a 3-hex-char nothing) and reported as
    // success.
    SerialLink link(300);
    link.board.set_counts(1, 0x8000FF);
    link.board.mispair_next();
    REQUIRE(link.proto.inquire_position(1) == 0x8000FF);
    REQUIRE(link.board.count_frames('j') == 2);  // rejected once, resent once
}

TEST_CASE("SkyWatcher serial - a malformed (byte-dropped) reply is resent and recovers", "[skywatcher][serial]") {
    // A reply with a dropped byte ("25278", neither "=" nor "!") is not a valid
    // answer. Before this change it threw immediately and failed the whole
    // operation. Electrical noise on a serial link produces these, so -- like a
    // mis-paired reply -- the wrapper now settles and resends once, recovering
    // the clean reply.
    SerialLink link(300);
    link.board.set_counts(1, 0x8000FF);
    link.board.malform_next();
    REQUIRE(link.proto.inquire_position(1) == 0x8000FF);
    REQUIRE(link.board.count_frames('j') == 2);  // rejected once, resent once
}

TEST_CASE("SkyWatcher serial - giving up on a second mis-pair still settles the line", "[skywatcher][serial]") {
    // send_command settles the link before its one resend. It must also
    // settle before it gives up on a second mis-pair: the stale frame behind
    // the mis-paired reply is still in flight, and a caller that catches the
    // exception and carries on (the driver's dispatch threads do) would have
    // its NEXT command answered by it. When that straggler has the same
    // shape as the next reply the shape check cannot help, so only the
    // settle window catches it (PR #245 review).
    SerialLink link(300);
    link.board.set_counts(1, 0x800000);
    // Two mis-paired ":j1" replies, then a stale ":j" reply (the old counts)
    // landing 50 ms after the second one -- inside the settle window.
    link.board.mispair_next(2, "=800000", 50);
    REQUIRE_THROWS_AS(link.proto.inquire_position(1), alpacacore::AlpacaException);
    REQUIRE(link.board.count_frames('j') == 2);  // one resend, then gave up

    link.board.set_counts(1, 0x812345);
    // The next inquiry must get its OWN reply, not the straggler.
    REQUIRE(link.proto.inquire_position(1) == 0x812345);
}

TEST_CASE("SkyWatcher serial - a quiet board times out within the command budget, not the broken-read block",
          "[skywatcher][serial]") {
    // Regression test for the poll()-bounded reads. Some USB CDC-ACM virtual COM
    // ports do NOT honour VMIN=0/VTIME=1 as a read() timeout, so a bare read() on a
    // board that has gone quiet parks forever in n_tty_read, holding io_mutex_ (and
    // the driver mutex above it) and wedging the whole server until it is killed.
    // That broken read is modelled through the read seam: it returns at once when
    // data is present but otherwise blocks (here up to 2 s) rather than timing out.
    // With the board MUTED (quiet, healthy fd) the exchange read never gets data:
    //   - WITH the fix: poll(response_timeout) returns 0, the broken read is never
    //     entered, and the exchange times out at ~response_timeout.
    //   - WITHOUT it (bare read in exchange_serial): the seam blocks the full 2 s
    //     before the exchange deadline is even re-checked.
    // Asserting the call fails well under 2 s fails on the base exchange_serial and
    // passes on the poll-bounded one -- i.e. it catches a revert of the exchange
    // poll(), which the old mis-pair-based version did not (it recovered in ms
    // whether or not the poll was there).
    FakeSkyWatcherSerialBoard board;
    auto vtime_ignoring_read = [](int fd, char* buf, std::size_t n) -> std::ptrdiff_t {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        for (;;) {
            const ssize_t r = ::read(fd, buf, n);
            if (r > 0) {
                return r;  // data present: a real tty returns at once here too
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return 0;  // bounded so a regression cannot hang the suite
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    sw::SkyWatcherProtocolWrapper proto{vtime_ignoring_read};
    sw::ConnectionInfo info;
    info.type = sw::ConnectionType::Serial;
    info.port_path = board.slave_path();
    info.baud_rate = 9600;
    info.response_timeout_ms = 300;
    REQUIRE(proto.connect(info));

    board.set_muted(true);  // board goes quiet (healthy fd): reads get no data
    const auto start = std::chrono::steady_clock::now();
    REQUIRE_THROWS_AS(proto.inquire_position(1), alpacacore::AlpacaException);  // times out, does not hang
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    // WITH the fix: ~response_timeout (300 ms). WITHOUT it: >= the 2 s broken-read
    // block. The 1500 ms bound fails on the base code and passes on the fix.
    CHECK(elapsed < std::chrono::milliseconds(1500));

    // The first timeout set serial_dirty_, so a second call (still muted) runs
    // settle_serial(200) BEFORE its write -- exercising the broken read on the
    // SETTLE path too, not just exchange. It must also stay bounded; without the
    // settle poll() gate it would block the seam's full 2 s inside settle_serial.
    const auto start2 = std::chrono::steady_clock::now();
    REQUIRE_THROWS_AS(proto.inquire_position(1), alpacacore::AlpacaException);
    const auto elapsed2 =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start2);
    CHECK(elapsed2 < std::chrono::milliseconds(1500));
    proto.disconnect();
}

TEST_CASE("SkyWatcher serial - a stalled TX buffer (write EAGAIN) is waited out, not failed fast",
          "[skywatcher][serial]") {
    // The fd is kept non-blocking for the read fix, so write() can return EAGAIN
    // before the first byte when the TX buffer is full (a board that stopped
    // draining). util::write_all only retries EAGAIN AFTER a partial write, so it
    // would fail such a frame at once ("Serial write failed: Resource temporarily
    // unavailable"); write_all_bounded instead waits for POLLOUT within the command
    // budget. Model a write that reports EAGAIN a few times before it succeeds (the
    // seam is armed only after connect so the handshake writes go through normally).
    // Without write_all_bounded this throws on the first EAGAIN.
    FakeSkyWatcherSerialBoard board;
    bool armed = false;
    int eagain_left = 0;
    auto flaky_write = [&](int fd, const char* buf, std::size_t n) -> std::ptrdiff_t {
        if (armed && eagain_left > 0) {
            --eagain_left;
            errno = EAGAIN;
            return -1;  // TX buffer "full": not writable yet
        }
        return ::write(fd, buf, n);
    };
    sw::SkyWatcherProtocolWrapper proto{{}, flaky_write};  // default read seam, flaky write seam
    sw::ConnectionInfo info;
    info.type = sw::ConnectionType::Serial;
    info.port_path = board.slave_path();
    info.baud_rate = 9600;
    info.response_timeout_ms = 300;
    REQUIRE(proto.connect(info));

    board.set_counts(1, 0x8000FF);
    armed = true;
    eagain_left = 3;                                 // first three writes of the ":j1" frame report a full TX buffer
    REQUIRE(proto.inquire_position(1) == 0x8000FF);  // waited out EAGAIN; reply read normally
    CHECK(eagain_left == 0);                         // all three EAGAINs consumed (not failed-fast on the first)
    proto.disconnect();
}

TEST_CASE("SkyWatcher serial - a transient failure of the ':i' readback does not disable the diagnostic",
          "[skywatcher][serial]") {
    // Only an explicit "!0" (Unknown command) means the board has no ":i". A
    // timeout or a mis-pair on the readback itself is a transport blip, and a
    // busy link is exactly when the diagnostic matters, so it must stay on.
    SerialLink link(300);

    // 1. The ":i" reply times out: the write is done, the readback is skipped,
    //    nothing throws, and the NEXT write still reads back.
    link.board.delay_next_reply(400, 'i');  // 100 ms past the timeout, inside the settle window
    REQUIRE_NOTHROW(link.proto.set_step_period(1, 5000));
    REQUIRE(link.board.count_frames('I') == 1);
    REQUIRE(link.board.count_frames('i') == 1);
    REQUIRE_NOTHROW(link.proto.set_step_period(1, 5004));
    REQUIRE(link.board.count_frames('I') == 2);
    REQUIRE(link.board.count_frames('i') == 2);  // still enabled

    // 2. A refusal for another reason ("!2" Motor not stopped) is skipped, not
    //    treated as unsupported.
    link.board.reject_next_readback("2");
    REQUIRE_NOTHROW(link.proto.set_step_period(1, 5008));
    REQUIRE_NOTHROW(link.proto.set_step_period(1, 5012));
    REQUIRE(link.board.count_frames('i') == 4);  // still enabled

    // 3. "!0" Unknown command: off until the next connect.
    link.board.set_no_readback(true);
    REQUIRE_NOTHROW(link.proto.set_step_period(1, 5016));
    REQUIRE(link.board.count_frames('i') == 5);
    REQUIRE_NOTHROW(link.proto.set_step_period(1, 5020));
    REQUIRE(link.board.count_frames('i') == 5);  // no further ":i"
}

TEST_CASE("SkyWatcher serial - connect claims the port in the cross-vendor registry and releases it",
          "[skywatcher][serial][registry]") {
    // Issue #230: EQDIR cables share their USB-serial chips with other
    // vendors' hardware, so the registry is what keeps one driver's connect
    // off a port another driver holds. connect_serial() must refuse a held
    // port, claim a free one before opening, and release it on disconnect
    // and on every failure path.
    FakeSkyWatcherSerialBoard board;
    auto& proto = sw::SkyWatcherProtocolWrapper::instance();
    const std::string key = std::filesystem::canonical(board.slave_path()).string();
    REQUIRE_FALSE(alpacacore::util::is_serial_port_in_use(key));

    sw::ConnectionInfo info;
    info.type = sw::ConnectionType::Serial;
    info.port_path = board.slave_path();
    info.baud_rate = 9600;
    info.response_timeout_ms = 300;

    // Held by "another vendor": refused, nothing on the wire, still held.
    alpacacore::util::mark_serial_port_open(key);
    REQUIRE_FALSE(proto.connect(info));
    REQUIRE(board.frames().empty());
    REQUIRE(alpacacore::util::is_serial_port_in_use(key));
    alpacacore::util::mark_serial_port_closed(key);

    // Free: claimed for the life of the connection, released on disconnect.
    REQUIRE(proto.connect(info));
    REQUIRE(alpacacore::util::is_serial_port_in_use(key));
    REQUIRE_FALSE(proto.get_motor_board_version().empty());
    proto.disconnect();
    REQUIRE_FALSE(alpacacore::util::is_serial_port_in_use(key));

    // A failed open must not leave a stale claim behind.
    info.port_path = "/dev/alpacacore-no-such-port";
    REQUIRE_FALSE(proto.connect(info));
    REQUIRE_FALSE(alpacacore::util::is_serial_port_in_use(info.port_path));
}

TEST_CASE("SkyWatcher serial - the dual-baud probe finds a Synta EQ board at 115200 and carries the baud",
          "[skywatcher][serial][probe]") {
    // Issue #230: a Synta EQ board over its own USB port speaks 115200 and
    // never answered the 9600-only scan. probe_skywatcher_port_any_baud()
    // tries 9600 first (a Wave answers there) and then 115200.
    FakeSkyWatcherSerialBoard board;
    sw::MotorBoardInfo info;
    int baud = 0;

    SECTION("a Wave answers the 9600 attempt") {
        REQUIRE(sw::probe_skywatcher_port_any_baud(board.slave_path(), info, baud));
        CHECK(baud == 9600);
        CHECK(info.firmware_version == "3.58");
        CHECK(board.count_frames('e') == 1);  // no second attempt
    }

    SECTION("an EQM-35 board only decodes at 115200") {
        board.set_version_reply("032732");  // MC 3.39, mount code 0x32
        board.answer_only_at_baud(115200);
        REQUIRE(sw::probe_skywatcher_port_any_baud(board.slave_path(), info, baud));
        CHECK(baud == 115200);
        CHECK(info.firmware_version == "3.39");
        CHECK(info.mount_code == 0x32);
        CHECK(info.model_name == "EQM-35 Pro");
    }

    SECTION("a port another device holds is not probed at all") {
        // The re-check after open() (issue #230) and the echo guard's own
        // registry look: nothing is written to a held port at either rate.
        const std::string key = std::filesystem::canonical(board.slave_path()).string();
        alpacacore::util::mark_serial_port_open(key);
        REQUIRE_FALSE(sw::probe_skywatcher_port_any_baud(key, info, baud));
        REQUIRE(board.frames().empty());
        alpacacore::util::mark_serial_port_closed(key);
    }
}

TEST_CASE("SkyWatcher serial - the probe gives up within its budget when reads ignore VTIME",
          "[skywatcher][serial][probe]") {
    // #836 bounded the connected link's reads with poll() because some USB
    // CDC-ACM ports ignore VMIN/VTIME and a bare read() then parks forever.
    // The ":e1" probe must hold its 1500 ms budget on such a port too: a
    // silent candidate must not hang the auto-detect scan.
    FakeSkyWatcherSerialBoard board;
    board.set_muted(true);
    board.set_reads_ignore_vtime(true);

    std::promise<std::string> result;
    auto done = result.get_future();
    const auto start = std::chrono::steady_clock::now();
    std::thread prober([&] { result.set_value(sw::probe_skywatcher_port(board.slave_path(), 9600)); });
    // Budget 1500 ms plus margin; a parked read never gets there.
    const bool finished = done.wait_for(std::chrono::milliseconds(3000)) == std::future_status::ready;
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (!finished) {
        board.release_blocked_reader();  // unpark the read so the thread can be joined
    }
    prober.join();
    INFO("probe elapsed " << elapsed << " ms");
    REQUIRE(finished);
    CHECK(done.get().empty());
    CHECK(board.count_frames('e') == 1);  // the probe was sent and went unanswered
}

TEST_CASE("SkyWatcher serial - the dual-baud scan gives up within its budget when reads ignore VTIME",
          "[skywatcher][serial][probe]") {
    // The scan always reaches 115200 after a silent 9600 attempt, and that
    // attempt first runs the SynScan echo guard (util/synscan_handset_probe.h)
    // on its own fd: its read must be poll()-bounded too, or the scan parks
    // there on the same silent port.
    FakeSkyWatcherSerialBoard board;
    board.set_muted(true);
    board.set_reads_ignore_vtime(true);

    std::promise<bool> result;
    auto done = result.get_future();
    const auto start = std::chrono::steady_clock::now();
    std::thread prober([&] {
        sw::MotorBoardInfo info;
        int baud = 0;
        result.set_value(sw::probe_skywatcher_port_any_baud(board.slave_path(), info, baud));
    });
    // Budget 1500 (9600 :e1) + 300 (echo guard) + 1500 (115200 :e1) ms plus margin.
    const bool finished = done.wait_for(std::chrono::milliseconds(5000)) == std::future_status::ready;
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (!finished) {
        board.release_blocked_reader();  // unpark the read so the thread can be joined
    }
    prober.join();
    INFO("scan elapsed " << elapsed << " ms");
    REQUIRE(finished);
    CHECK_FALSE(done.get());
    CHECK(board.count_frames('e') == 2);  // :e1 went out at both bauds, unanswered
}

// ── open-astro#445: Connected follows the serial link ──────────────────────

namespace {

sw::ConnectionInfo serial_info(const std::string& path) {
    sw::ConnectionInfo info;
    info.type = sw::ConnectionType::Serial;
    info.port_path = path;
    info.baud_rate = 9600;
    info.response_timeout_ms = 300;
    return info;
}

std::unique_ptr<alpacacore::TelescopeDriver> serial_driver(const std::string& path) {
    return sw::create_skywatcher_telescope(0, serial_info(path), -37.0, 175.0, 50.0);
}

void require_not_connected_error(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::NotConnected);
        return;
    }
    FAIL("expected AlpacaException(NotConnected)");
}

// A by-id style symlink to a fake board, repointable to a "replugged" one.
struct PortLink {
    std::filesystem::path path;
    explicit PortLink(const std::string& target) {
        path = std::filesystem::temp_directory_path() /
               ("alpacacore-sw445-" + std::to_string(::getpid()) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_symlink(target, path);
    }
    void repoint(const std::string& target) const {
        std::filesystem::remove(path);
        std::filesystem::create_symlink(target, path);
    }
    ~PortLink() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

}  // namespace

TEST_CASE("SkyWatcher serial - Connected drops when the adapter is pulled, with no client I/O",
          "[skywatcher][serial][connected]") {
    // The #445 rig trace: `connected` read True on every sample for 80 s with
    // the device node gone. Nothing is asked of the mount between the unplug
    // and the read, so the answer must come from the link, not from an error.
    FakeSkyWatcherSerialBoard board;
    const std::string key = std::filesystem::canonical(board.slave_path()).string();
    auto driver = serial_driver(board.slave_path());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    board.sever_link();
    CHECK_FALSE(driver->get_connected());
    // A link lost under a connected client is a fault, not an idle device: the
    // Devices dot must not read like a mount nobody opened.
    CHECK(driver->get_link_fault().find("lost") != std::string::npos);
    // The stale descriptor is what kept the kernel from handing the returning
    // adapter its old name (/dev/ttyUSB0 -> ttyUSB1); noticing the loss
    // releases it, and the port's registry claim with it.
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(key));
    require_not_connected_error([&] { driver->get_right_ascension(); });

    // An explicit disconnect of the dead link still completes cleanly, and
    // the client's own disconnect settles the fault.
    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());
}

TEST_CASE("SkyWatcher serial - an operation that hits a dead link fails NotConnected and drops Connected",
          "[skywatcher][serial][connected]") {
    // The other half of the trace: "Serial write failed: Input/output error"
    // from every read, as a generic driver error, with Connected still True.
    FakeSkyWatcherSerialBoard board;
    auto& proto = sw::SkyWatcherProtocolWrapper::instance();
    REQUIRE(proto.connect(serial_info(board.slave_path())));
    REQUIRE_FALSE(proto.get_motor_board_version().empty());

    board.sever_link();
    require_not_connected_error([&] { proto.inquire_position(sw::kAxisRa); });
    CHECK_FALSE(proto.is_connected());
    proto.disconnect();
}

TEST_CASE("SkyWatcher serial - Connected=true on a stale link reconnects instead of short-circuiting",
          "[skywatcher][serial][connected]") {
    // The #445 recovery path: the adapter comes back (here under the same
    // by-id style symlink, repointed as udev does) and the client re-sends
    // Connected=true without a disconnect first. That used to return success
    // with no driver activity at all.
    auto board = std::make_unique<FakeSkyWatcherSerialBoard>();
    PortLink link(board->slave_path());
    auto driver = serial_driver(link.path.string());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // A redundant connect on a LIVE link is still a no-op: nothing on the wire.
    const std::size_t frames_before = board->frames().size();
    driver->set_connected(true);
    CHECK(board->frames().size() == frames_before);

    // Captured BEFORE sever_link(): PtyPair::sever() clears slave_path_, so
    // reading it off `board` after severing would always be empty and the
    // guard below would compare against "" -- true for any pty and unable
    // to catch the reorder it exists to catch.
    const std::string old_path = board->slave_path();
    board->sever_link();
    // devpts assigns st_ino = index + 3, so link_alive() would call a
    // recycled index the same node -- this only works because the driver
    // still holds `board`'s old slave fd here, which keeps devpts from
    // reusing its index before `replugged` is constructed below. Do not
    // reorder sever_link() / construction / repoint().
    FakeSkyWatcherSerialBoard replugged;
    // Self-checking: if a future edit moves construction earlier or drops the
    // driver's fd first, devpts could recycle the index and hand back the
    // severed board's own path, which would let this case pass vacuously.
    REQUIRE(replugged.slave_path() != old_path);
    link.repoint(replugged.slave_path());
    REQUIRE(replugged.frames().empty());

    driver->set_connected(true);
    CHECK(driver->get_connected());
    CHECK(replugged.count_frames('e') >= 1);  // the connect sequence really ran
    CHECK_NOTHROW(driver->get_right_ascension());
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher serial - a connect whose board never answers leaves Connected false",
          "[skywatcher][serial][connected]") {
    // open() and tcsetattr() succeeding proves only that a port exists. The
    // ":e" identity query is allowed to fail, but the axis-parameter reads
    // after it are not; when they throw, the connect must not stay latched.
    FakeSkyWatcherSerialBoard board;
    board.answer_only_at_baud(115200);  // the driver speaks 9600: nothing decodes
    const std::string key = std::filesystem::canonical(board.slave_path()).string();
    auto driver = serial_driver(board.slave_path());

    CHECK_THROWS(driver->set_connected(true));
    CHECK_FALSE(driver->get_connected());
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(key));
}

TEST_CASE("SkyWatcher serial - separate drivers keep separate links", "[skywatcher][serial][connected]") {
    FakeSkyWatcherSerialBoard first;
    FakeSkyWatcherSerialBoard second;
    first.set_version_reply("032732");
    auto a = serial_driver(first.slave_path());
    auto b = sw::create_skywatcher_telescope(1, serial_info(second.slave_path()), -37.0, 175.0, 50.0);
    a->set_connected(true);
    b->set_connected(true);
    CHECK(a->command_string("e1", false) == "=032732");
    CHECK(b->command_string("e1", false) == "=033A44");
    CHECK(a->get_connected());
    // Disconnecting B must not close A's descriptor or change A's Connected.
    b->set_connected(false);
    CHECK(a->get_connected());
    CHECK_NOTHROW(a->get_right_ascension());
    b->set_connected(true);
    first.sever_link();
    CHECK_FALSE(a->get_connected());
    CHECK(b->get_connected());
    CHECK_NOTHROW(b->get_right_ascension());
    a->set_connected(false);
    CHECK(b->get_connected());
    b->set_connected(false);
}

// Uses the real transport, but makes a link disappear immediately after one
// successful health probe. The next probe observes loss deterministically.
class LateLossProtocol final : public sw::SkyWatcherProtocolWrapper {
public:
    void arm(std::function<void()> callback) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        owner_ = std::this_thread::get_id();
        after_probe_ = std::move(callback);
    }
    bool link_alive() override {
        const bool result = sw::SkyWatcherProtocolWrapper::link_alive();
        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            if (owner_ == std::this_thread::get_id()) {
                callback = std::move(after_probe_);
                after_probe_ = {};
            }
        }
        if (callback) callback();
        return result;
    }

private:
    std::mutex callback_mutex_;
    std::thread::id owner_;
    std::function<void()> after_probe_;
};

TEST_CASE("SkyWatcher serial - late loss joins an old pulse before reconnecting",
          "[skywatcher][serial][connected][late-loss]") {
    FakeSkyWatcherSerialBoard board;
    FakeSkyWatcherSerialBoard replugged;
    PortLink port(board.slave_path());
    auto protocol = std::make_unique<LateLossProtocol>();
    auto* probe = protocol.get();
    auto driver =
        sw::create_skywatcher_telescope(0, serial_info(port.path.string()), -37.0, 175.0, 50.0, std::move(protocol));
    driver->set_connected(true);
    // Only the fake board moves. Wait for dispatch before introducing the loss.
    driver->pulse_guide(0, 400);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (board.count_frames('J') == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    REQUIRE(board.count_frames('J') > 0);
    probe->arm([&] {
        board.sever_link();
        port.repoint(replugged.slave_path());
    });
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE(replugged.count_frames('e') > 0);
    // Without the late-loss join, the old pulse timer sends its stop to the
    // NEW board after reconnect. Observe beyond the original pulse deadline.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(replugged.count_frames('K') == 0);
    CHECK_FALSE(driver->get_is_pulse_guiding());
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher serial - immediate no-progress reads do not spin at the timeout",
          "[skywatcher][serial][read-stall]") {
    const int retry = GENERATE(0, EAGAIN, EINTR);
    FakeSkyWatcherSerialBoard board;
    bool no_progress = false;
    int reads = 0;
    sw::SkyWatcherProtocolWrapper protocol([&](int fd, char* data, std::size_t size) -> std::ptrdiff_t {
        if (no_progress) {
            ++reads;
            errno = retry;
            return retry == 0 ? 0 : -1;
        }
        return ::read(fd, data, size);
    });
    REQUIRE(protocol.connect(serial_info(board.slave_path())));
    REQUIRE_FALSE(protocol.get_motor_board_version().empty());
    no_progress = true;
    CHECK_THROWS(protocol.send_command('j', 1, "", 60));
    CHECK(reads > 0);
    CHECK(reads <= 100);
    // A still-present node with no answer retains the existing timeout policy.
    CHECK(protocol.link_alive());
}

TEST_CASE("SkyWatcher serial - read errors on a present node lose the link", "[skywatcher][serial][connected]") {
    const int failure = GENERATE(EIO, ENXIO, ENODEV, EBADF);
    FakeSkyWatcherSerialBoard board;
    bool fail_read = false;
    sw::SkyWatcherProtocolWrapper protocol([&](int fd, char* data, std::size_t size) -> std::ptrdiff_t {
        if (fail_read) {
            errno = failure;
            return -1;
        }
        return ::read(fd, data, size);
    });
    const std::string key = std::filesystem::canonical(board.slave_path()).string();
    REQUIRE(protocol.connect(serial_info(board.slave_path())));
    REQUIRE_FALSE(protocol.get_motor_board_version().empty());
    fail_read = true;
    REQUIRE(std::filesystem::exists(board.slave_path()));
    require_not_connected_error([&] { protocol.inquire_position(1); });
    CHECK_FALSE(protocol.link_alive());
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(key));
}

// ── open-astro#505: a board that stops answering on a healthy link ──────────
//
// Distinct from #445 above: there the node is REMOVED, Connected drops and
// operations throw NotConnected. Here the mount is powered off with the
// adapter still plugged in, so the fd is healthy, the node still resolves,
// link_alive() stays true, and only the exchange timeouts reveal anything.
// Reproduced on an EQM-35 Pro 2026-09-17: `connected` read true for every one
// of 62 samples, and the reported right ascension kept ADVANCING at sidereal
// rate off the cached hour angle while the mount was unpowered — stale data
// that ticks is far harder for a client to notice than stale data that sits.

namespace {

void require_driver_error(const std::function<void()>& fn, const std::string& needle) {
    try {
        fn();
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(e.what()).find(needle) != std::string::npos);
        return;
    }
    FAIL("expected AlpacaException(DriverException) containing '" << needle << "'");
}

}  // namespace

TEST_CASE("SkyWatcher serial - a silent board latches a link fault after three exchanges, Connected stays true",
          "[skywatcher][serial][linkhealth]") {
    FakeSkyWatcherSerialBoard board;
    sw::SkyWatcherProtocolWrapper protocol;
    REQUIRE(protocol.connect(serial_info(board.slave_path())));
    REQUIRE_FALSE(protocol.get_motor_board_version().empty());
    CHECK_FALSE(protocol.link_faulted());

    board.set_muted(true);
    // One transient timeout must not brick a session: a single mis-timed reply
    // during a slew is ordinary.
    CHECK_THROWS_AS(protocol.inquire_position(sw::kAxisRa), alpacacore::AlpacaException);
    CHECK_FALSE(protocol.link_faulted());
    CHECK_THROWS_AS(protocol.inquire_position(sw::kAxisRa), alpacacore::AlpacaException);
    CHECK_FALSE(protocol.link_faulted());
    CHECK_THROWS_AS(protocol.inquire_position(sw::kAxisRa), alpacacore::AlpacaException);
    CHECK(protocol.link_faulted());
    CHECK(protocol.link_fault().find("consecutive failures") != std::string::npos);

    // The node is still there and the board may come back on the same fd, so
    // the client — not the driver — decides whether to reconnect (#237).
    CHECK(protocol.is_connected());
    CHECK(protocol.link_alive());

    // The driver must keep talking while faulted: on a polled link these reads
    // are the only traffic that can ever clear the latch.
    const int frames_while_faulted = board.count_frames('j');
    CHECK_THROWS_AS(protocol.inquire_position(sw::kAxisRa), alpacacore::AlpacaException);
    CHECK(board.count_frames('j') > frames_while_faulted);

    const std::uint64_t epoch_before = protocol.link_recovery_epoch();
    board.set_muted(false);
    CHECK(protocol.inquire_position(sw::kAxisRa) == 0x800000);
    CHECK_FALSE(protocol.link_faulted());
    CHECK(protocol.link_recovery_epoch() == epoch_before + 1);
    CHECK(protocol.is_connected());  // never dropped, so no reconnect was needed
    protocol.disconnect();
}

TEST_CASE("SkyWatcher serial - the connected port is claimed exclusively and released on disconnect",
          "[skywatcher][serial][exclusive]") {
    // open-astro#912: an outside open reprograms the line and makes a reconnect
    // fail EBUSY. TIOCEXCL refuses a non-root open while we hold the port.
    if (geteuid() == 0) {
        SUCCEED("root bypasses TIOCEXCL");
        return;
    }
    FakeSkyWatcherSerialBoard board;
    sw::SkyWatcherProtocolWrapper protocol;
    REQUIRE(protocol.connect(serial_info(board.slave_path())));

    errno = 0;
    int fd = open(board.slave_path().c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    const int err = errno;
    if (fd >= 0) close(fd);
    CHECK(fd < 0);
    CHECK(err == EBUSY);

    protocol.disconnect();
    fd = open(board.slave_path().c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    CHECK(fd >= 0);
    if (fd >= 0) close(fd);
}

TEST_CASE("SkyWatcher serial - a latched fault from a reprogrammed line recovers without a reconnect",
          "[skywatcher][serial][linkhealth]") {
    // open-astro#912: line settings belong to the tty. After an outside open
    // leaves it at the wrong rate the board is unreachable until the driver
    // re-applies its own settings.
    FakeSkyWatcherSerialBoard board;
    board.answer_only_at_baud(9600);
    sw::SkyWatcherProtocolWrapper protocol;
    REQUIRE(protocol.connect(serial_info(board.slave_path())));
    REQUIRE(protocol.inquire_position(sw::kAxisRa) == 0x800000);

    board.set_muted(true);
    for (int i = 0; i < 3; ++i) {
        CHECK_THROWS_AS(protocol.inquire_position(sw::kAxisRa), alpacacore::AlpacaException);
    }
    CHECK(protocol.link_faulted());

    // An outside open leaves the shared line at the wrong rate, then the board
    // answers again. The first exchange may still time out; the driver
    // re-applies its settings, so a later one succeeds without a reconnect.
    board.set_line_baud(115200);
    board.set_muted(false);
    bool recovered = false;
    for (int i = 0; i < 3 && !recovered; ++i) {
        try {
            recovered = protocol.inquire_position(sw::kAxisRa) == 0x800000;
        } catch (const alpacacore::AlpacaException&) {
        }
    }
    CHECK(recovered);
    CHECK_FALSE(protocol.link_faulted());
    CHECK(protocol.is_connected());
    protocol.disconnect();
}

TEST_CASE("SkyWatcher serial - a faulted link refuses the position cache instead of serving it",
          "[skywatcher][serial][linkhealth]") {
    FakeSkyWatcherSerialBoard board;
    auto driver = serial_driver(board.slave_path());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    const double ra_before = driver->get_right_ascension();

    board.set_muted(true);
    // Wait out the position cache's TTL so the next read actually asks the
    // board; inside the TTL a read is legitimately served from cache.
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));

    // The first attempts fail but are still within the pre-existing stale
    // window, so they answer from cache — that window is what the hardware run
    // measured at about nine seconds of plausible-looking data.
    for (int i = 0; i < 2; ++i) {
        try {
            driver->get_right_ascension();
        } catch (const alpacacore::AlpacaException&) {
        }
    }
    // By the threshold the fault latches, and from then on the cache is not an
    // answer at any age.
    require_driver_error([&] { driver->get_right_ascension(); }, "communications compromised");
    require_driver_error([&] { driver->get_declination(); }, "communications compromised");
    CHECK(driver->get_connected());  // DriverException, not NotConnected
    CHECK_FALSE(driver->get_link_fault().empty());  // surfaced to the management listing

    board.set_muted(false);
    // Recovery is automatic: no reconnect, no client action.
    const double ra_after = driver->get_right_ascension();
    CHECK(std::abs(ra_after - ra_before) < 0.01);
    CHECK(driver->get_connected());
    CHECK(driver->get_link_fault().empty());
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher serial - a board that restarted while the link was down is not silently resumed",
          "[skywatcher][serial][linkhealth]") {
    // The hardware run's sharpest finding (EQM-35 Pro, 2026-09-17): after a
    // mains power cycle the board answered every frame normally while ":f1"
    // read "=100" — initialization cleared — and ":j2" read the bare home
    // count. The driver only sends ":F" at connect, so it would have gone on
    // serving reset registers as a position for the rest of the session, and
    // the board would have refused every motion command.
    FakeSkyWatcherSerialBoard board;
    auto driver = serial_driver(board.slave_path());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE_NOTHROW(driver->get_right_ascension());

    board.set_muted(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    for (int i = 0; i < 4; ++i) {
        try {
            driver->get_right_ascension();
        } catch (const alpacacore::AlpacaException&) {
        }
    }

    // The board comes back — but it is not the board we set up.
    board.set_init_done(false);
    board.set_muted(false);
    require_driver_error([&] { driver->get_right_ascension(); }, "restarted");
    // Terminal for the session: the latch cleared on that good reply, so
    // nothing else would ever raise this again.
    require_driver_error([&] { driver->get_right_ascension(); }, "restarted");
    CHECK(driver->get_connected());
    // The link latch cleared on the good reply, but every read still fails:
    // the management listing must keep reporting a fault.
    CHECK(driver->get_link_fault().find("restarted") != std::string::npos);

    // A reconnect re-runs the ":F" init, which is what actually fixes it.
    driver->set_connected(false);
    driver->set_connected(true);
    CHECK_NOTHROW(driver->get_right_ascension());
    CHECK(driver->get_link_fault().empty());
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher serial - a board that merely went quiet resumes with its session intact",
          "[skywatcher][serial][linkhealth]") {
    // The other half of the decision: forcing a client to re-establish tracking
    // after a brief hiccup would be disruptive and needs a human, so a board
    // that comes back initialized resumes with no client action at all.
    FakeSkyWatcherSerialBoard board;
    auto driver = serial_driver(board.slave_path());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    board.set_muted(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    for (int i = 0; i < 4; ++i) {
        try {
            driver->get_right_ascension();
        } catch (const alpacacore::AlpacaException&) {
        }
    }
    board.set_muted(false);  // init_done left true: same session, still aligned
    CHECK_NOTHROW(driver->get_right_ascension());
    CHECK(driver->get_connected());
    driver->set_connected(false);
}

// ── open-astro#521: motion that outlived the link ───────────────────────────
//
// A Sky-Watcher axis keeps running until something tells it to stop, so a
// cable pulled mid-slew leaves the mount moving with nothing driving it. On an
// EQM-35 Pro (2026-09-17, bare mount): RA at 0.5 deg/s, cable pulled and
// replaced, and after Connected=true the board reported ":f1" = "=111" (speed
// mode, running, initialized) while the driver reported Slewing false and sent
// no ":K" anywhere in the relink. Worse, MoveAxis(axis, 0) — the documented
// way to stop a MoveAxis — returned success without putting a stop on the
// wire at all, because the runtime-state reset had cleared the flag that call
// consults. A client doing exactly the right thing could not stop its mount.

namespace {

// Relink under the same by-id style symlink, as udev does: sever the board,
// stand up a replacement, repoint. Returns the replacement, which the caller
// keeps alive. Ordering matters for devpts index reuse — see the #445 case.
std::unique_ptr<FakeSkyWatcherSerialBoard> relink_board(std::unique_ptr<FakeSkyWatcherSerialBoard>& board,
                                                        const PortLink& link, alpacacore::TelescopeDriver& driver) {
    const std::string old_path = board->slave_path();
    board->sever_link();
    // Stand the replacement up BEFORE anything observes the loss: noticing it
    // closes the driver's fd, and that fd is the only thing keeping devpts
    // from recycling the severed board's index and handing back its own path
    // (the #445 case documents the same ordering hazard). Do not reorder.
    auto replugged = std::make_unique<FakeSkyWatcherSerialBoard>();
    REQUIRE(replugged->slave_path() != old_path);
    // Observing the loss is what stamps it, and the stamp is what the relink
    // decision measures its outage from.
    CHECK_FALSE(driver.get_connected());
    link.repoint(replugged->slave_path());
    return replugged;
}

struct RelinkWindowGuard {
    std::chrono::milliseconds previous_ = sw::detail::relink_motion_preserve_window();
    explicit RelinkWindowGuard(std::chrono::milliseconds window) {
        sw::detail::set_relink_motion_preserve_window(window);
    }
    // Restore whatever was configured before this guard, rather than a
    // hardcoded default that would silently drift from
    // kRelinkMotionPreserveWindow if that constant ever changes (PR review,
    // 2026-09-18).
    ~RelinkWindowGuard() { sw::detail::set_relink_motion_preserve_window(previous_); }
};

}  // namespace

TEST_CASE("SkyWatcher serial - a brief outage preserves surviving motion and reports Slewing honestly",
          "[skywatcher][serial][relink]") {
    // The default 5 s window would also cover this relink on an idle box, but a
    // loaded arm64 runner under ASan/TSan can stretch sever -> new pty ->
    // connect -> six probes past it, and then the case silently flips to the
    // stop branch and fails on count_frames('K'). Pin the branch under test
    // the way the sibling pins the other one (review of #553).
    RelinkWindowGuard window(std::chrono::hours(1));  // every outage is "brief"
    auto board = std::make_unique<FakeSkyWatcherSerialBoard>();
    PortLink link(board->slave_path());
    auto driver = serial_driver(link.path.string());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    auto replugged = relink_board(board, link, *driver);
    // The board kept turning across the outage, in SPEED mode. ":f" cannot say
    // whether that is a MoveAxis or an ordinary tracking drive — they are the
    // same bits on the wire — so the driver must not guess.
    replugged->set_axis_running(sw::kAxisRa, true, /*speed_mode=*/true);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // Inside the window the motion is still wanted: aborting a long slew
    // because a connector was jostled for a second is worse than the bug.
    CHECK(replugged->axis_running(sw::kAxisRa));
    CHECK(replugged->count_frames('K') == 0);
    // Slewing reports what the board actually says, and a speed-mode axis is
    // NOT slewing. Claiming otherwise (by reconstructing the MoveAxis flag on
    // a guess) wedges Slewing true for the whole session on a mount that is
    // merely tracking, and strands every rate/site write behind
    // axis_busy_locked(). Review of #553.
    CHECK_FALSE(driver->get_slewing());

    // The accepted cost of not guessing, pinned so it cannot regress silently:
    // MoveAxis(axis, 0) consults that flag, so it is a no-op here.
    driver->move_axis(0, 0.0);
    CHECK(replugged->count_frames('K') == 0);
    CHECK(replugged->axis_running(sw::kAxisRa));

    // AbortSlew is the route that still works: it stops both axes
    // unconditionally (":L"), without consulting the flag.
    driver->abort_slew();
    CHECK_FALSE(replugged->axis_running(sw::kAxisRa));
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher serial - an outage past the window stops a surviving axis and confirms it at rest",
          "[skywatcher][serial][relink]") {
    RelinkWindowGuard window(std::chrono::milliseconds(0));  // every outage is "long"
    auto board = std::make_unique<FakeSkyWatcherSerialBoard>();
    PortLink link(board->slave_path());
    auto driver = serial_driver(link.path.string());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    auto replugged = relink_board(board, link, *driver);
    replugged->set_axis_running(sw::kAxisRa, true, /*speed_mode=*/true);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // Nobody has been in control of a moving mount: stop it, and prove it
    // came to rest rather than that the command was merely accepted.
    CHECK(replugged->count_frames('K') >= 1);
    CHECK_FALSE(replugged->axis_running(sw::kAxisRa));
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher serial - a GOTO-mode axis that survived the outage is reported without a flag",
          "[skywatcher][serial][relink]") {
    auto board = std::make_unique<FakeSkyWatcherSerialBoard>();
    PortLink link(board->slave_path());
    auto driver = serial_driver(link.path.string());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    auto replugged = relink_board(board, link, *driver);
    replugged->set_axis_running(sw::kAxisDec, true, /*speed_mode=*/false);

    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(replugged->axis_running(sw::kAxisDec));
    // GOTO-mode motion needs no reconstructed flag: Slewing is re-derived from
    // the board on every read, which is why this half was already visible on
    // hardware while the speed-mode half was not.
    CHECK(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher serial - a relink onto a board at rest sends no stop", "[skywatcher][serial][relink]") {
    // The negative control, with the window forced to zero so the stopping
    // branch is the one under test: an idle board must not be commanded.
    RelinkWindowGuard window(std::chrono::milliseconds(0));
    auto board = std::make_unique<FakeSkyWatcherSerialBoard>();
    PortLink link(board->slave_path());
    auto driver = serial_driver(link.path.string());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    auto replugged = relink_board(board, link, *driver);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(replugged->count_frames('K') == 0);
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// PR review, 2026-09-18: a driver instance that has NEVER connected before
// has no recorded link-loss stamp at all -- not because the outage was
// brief, but because there was no outage to record. That must not be
// mistaken for "no stamp means a clean disconnect, so anything running is
// unexplained": a process restart while the mount kept tracking under its
// own power looks identical at the protocol level, and the old code sent
// ":K" to a perfectly healthy, already-running axis on first connect.
TEST_CASE("SkyWatcher serial - a fresh connect finds an already-running axis and does not stop it",
          "[skywatcher][serial][relink]") {
    FakeSkyWatcherSerialBoard board;
    board.set_axis_running(sw::kAxisRa, true, /*speed_mode=*/true);
    auto driver = serial_driver(board.slave_path());

    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // No stamp exists for a driver that has never connected before, so the
    // destructive branch must never fire here.
    CHECK(board.count_frames('K') == 0);
    CHECK(board.axis_running(sw::kAxisRa));
    // And the axis is left honestly reported: this is the service-restart
    // case, where the running speed-mode axis is almost certainly tracking.
    // Reporting Slewing true here would hang any sequencer that waits for it
    // to drop, and strand rate/site writes behind axis_busy_locked() for the
    // rest of the session (review of #553).
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

// Review of #553: the fake decoded ":G"'s mode digit as "1 or 2 = speed
// mode", but the board tests bit 0, and the driver sends '3' for a FAST speed
// move. The fake therefore recorded a fast MoveAxis as GOTO motion, which is
// what get_hardware_slewing_locked() reads as Slewing after a relink. Pin the
// wire semantics: both MoveAxis rates select speed mode on the board.
TEST_CASE("SkyWatcher serial - a fast MoveAxis puts the fake's axis in speed mode, like a slow one",
          "[skywatcher][serial][fake]") {
    FakeSkyWatcherSerialBoard board;
    auto driver = serial_driver(board.slave_path());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // 1 deg/s is above kFastModeThresholdDegPerSec (128x sidereal), so the
    // driver sends ":G3x" (fast, speed mode) rather than ":G1x".
    driver->move_axis(0, 1.0);
    CHECK(board.axis_running(sw::kAxisRa));
    CHECK(board.axis_speed_mode(sw::kAxisRa));
    driver->move_axis(0, 0.0);
    CHECK_FALSE(board.axis_running(sw::kAxisRa));

    // Sidereal-class rate: ":G1x", also speed mode.
    driver->move_axis(0, 0.01);
    CHECK(board.axis_running(sw::kAxisRa));
    CHECK(board.axis_speed_mode(sw::kAxisRa));
    driver->move_axis(0, 0.0);
    CHECK_FALSE(board.axis_running(sw::kAxisRa));
    driver->set_connected(false);
}

// ── open-astro#559: a lost ":K" on serial is retransmitted, not waited out ──
//
// The serial transport has no retransmit: exchange_serial() waits the whole
// reply timeout (1000 ms by default) and throws, and only the driver's outer
// stop loop resends, 100 ms later, after a 200 ms settle. A ":K" frame that
// the board never decoded (a corrupted byte on an EQDIR link) therefore
// leaves the axis running at guide rate for ~1.3 s past the pulse end -- the
// +9.4 arcsec overshoot on a 37.6 arcsec pulse the issue measured. The UDP
// transport already retransmits (kUdpRetries); the serial one must too, and
// a stop -- idempotent by nature -- must not wait the full data-command
// timeout before it does.

namespace {

// Production-shaped link: the 1000 ms default reply timeout the issue names,
// on the pty-backed board. Reuses the SerialLink fixture's shape rather than
// its 300 ms default so the bound below is the real one.
constexpr int kIssue559ReplyTimeoutMs = 1000;

}  // namespace

TEST_CASE("SkyWatcher serial - a lost ':K' is retransmitted and the stop lands well inside the reply timeout (#559)",
          "[skywatcher][serial][pulseguide]") {
    SerialLink link(kIssue559ReplyTimeoutMs);
    link.board.set_axis_running(sw::kAxisRa, true);
    link.board.drop_next_frames('K');  // the board never sees the first ":K1"

    const auto t0 = std::chrono::steady_clock::now();
    // Before the fix: throws "Timeout waiting for motor controller reply"
    // after the full 1000 ms with no second ":K1" on the wire.
    REQUIRE_NOTHROW(link.proto.stop_motion(sw::kAxisRa));
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    CHECK(link.board.count_frames('K') == 2);  // lost once, resent once
    CHECK_FALSE(link.board.axis_running(sw::kAxisRa));
    // The retransmit must fire on a stop-sized wait, not the data-command
    // timeout: one short attempt + the 200 ms dirty-link settle + the resend
    // is ~450 ms. 700 ms leaves scheduling room and still rules out a stop
    // that only lands because the full 1000 ms elapsed.
    CHECK(elapsed_ms < 700);
}

TEST_CASE("SkyWatcher serial - a raised response timeout also governs ':K' on a slow link (#559)",
          "[skywatcher][serial]") {
    // A user who raises responseTimeoutMs above the 1000 ms default is
    // declaring a link that answers slowly. The 250 ms stop cap must not
    // override that: a ":K" whose reply takes 400 ms on such a link would
    // otherwise time out, and on a link that slow every attempt would, ending
    // in "STOP FAILED" for a board that did stop.
    SerialLink link(2000);
    link.board.set_axis_running(sw::kAxisRa, true);
    link.board.delay_next_reply(400, 'K');

    REQUIRE_NOTHROW(link.proto.stop_motion(sw::kAxisRa));
    CHECK(link.board.count_frames('K') == 1);  // answered inside the user's timeout: no resend
    CHECK_FALSE(link.board.axis_running(sw::kAxisRa));
}

TEST_CASE("SkyWatcher serial - a state-latching command is NOT retransmitted after a lost frame (#559 guard)",
          "[skywatcher][serial]") {
    // The retransmit is for idempotent frames only. ":I" latches a step
    // period the board may or may not have applied; blindly resending it
    // (or ":E", ":S", ":G", ":J", ":F", ":W") after silence could double-apply
    // a state change behind a late reply. Those keep the single attempt and
    // the throw. GREEN before the fix as well: it pins the boundary the fix
    // must not cross, it does not reproduce the issue.
    SerialLink link(300);
    link.board.drop_next_frames('I');
    REQUIRE_THROWS_AS(link.proto.set_step_period(sw::kAxisRa, 0x001234, /*with_readback=*/false),
                      alpacacore::AlpacaException);
    CHECK(link.board.count_frames('I') == 1);
}

TEST_CASE("SkyWatcher serial - a Dec pulse whose ':K' is lost stops before the full reply timeout would expire (#559)",
          "[skywatcher][serial][pulseguide]") {
    // Driver-level shape of the same defect: the pulse task's stop goes out
    // through proto.stop_motion(); when the board never decodes it, the axis
    // stays running until the wrapper gives up (1000 ms), the outer stop
    // loop waits 100 ms, settles 200 ms and resends. Measured from the pulse
    // start, the axis then stops at ~duration + 1300 ms. With the serial
    // retransmit it stops at ~duration + 450 ms. The bound is the reply
    // timeout itself: a stop that only lands AFTER the full timeout is the
    // overshoot the issue reports.
    FakeSkyWatcherSerialBoard board;
    sw::ConnectionInfo info = serial_info(board.slave_path());
    info.response_timeout_ms = kIssue559ReplyTimeoutMs;
    auto driver = sw::create_skywatcher_telescope(0, info, -37.0, 175.0, 50.0);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE_FALSE(board.axis_running(sw::kAxisDec));

    constexpr int kPulseMs = 500;

    const auto t0 = std::chrono::steady_clock::now();
    driver->pulse_guide(0, kPulseMs);  // North: Dec axis starts, ":K2" at the end
    REQUIRE(driver->get_is_pulse_guiding());
    // The axis must be running before we time its stop, or a pulse that
    // never started would pass trivially. Dispatch sends its own ":K2"
    // before ":J2", so the drop is armed only once the axis is running:
    // the next ":K2" on the wire is then the pulse-ending one.
    {
        const auto start_deadline = t0 + std::chrono::milliseconds(kPulseMs);
        while (!board.axis_running(sw::kAxisDec) && std::chrono::steady_clock::now() < start_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(board.axis_running(sw::kAxisDec));
    }
    const int stops_before = board.count_frames('K');
    board.drop_next_frames('K');  // the pulse-ending ":K2" is lost on the wire
    // Poll the BOARD, not IsPulseGuiding: the driver's flag follows the
    // deadline, not the axis (a separate defect noted on the issue).
    const auto give_up = t0 + std::chrono::seconds(5);
    while (board.axis_running(sw::kAxisDec) && std::chrono::steady_clock::now() < give_up) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto stopped_after_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    REQUIRE_FALSE(board.axis_running(sw::kAxisDec));
    CHECK(board.count_frames('K') - stops_before >= 2);  // the lost stop was resent
    CHECK(stopped_after_ms < kPulseMs + kIssue559ReplyTimeoutMs);

    driver->set_connected(false);
}

#endif  // _WIN32

TEST_CASE("FakeSkyWatcherSerialBoard - a steady reply latency is paid by every transaction and counted",
          "[skywatcher][serial]") {
    FakeSkyWatcherSerialBoard board;
    sw::SkyWatcherProtocolWrapper protocol;
    REQUIRE(protocol.connect(serial_info(board.slave_path())));

    const auto timed_reads = [&](int n) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) (void)protocol.inquire_position(sw::kAxisRa);
        return std::chrono::steady_clock::now() - start;
    };
    const int before = board.transactions_served();
    board.set_reply_latency(40);
    const auto slow = timed_reads(5);
    CHECK(board.transactions_served() - before == 5);
    CHECK(slow >= std::chrono::milliseconds(5 * 40));

    board.set_reply_latency(0);
    const auto fast = timed_reads(5);
    CHECK(board.transactions_served() - before == 10);
    CHECK(fast < std::chrono::milliseconds(5 * 40));
    protocol.disconnect();
}
