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

// OnStep serial link loss (decision 0009, issue #1241): an unplugged or
// re-enumerated adapter reads as not connected, operations fail NotConnected
// at once, a Connected=true reconnects instead of short-circuiting, and the
// reconnect after a long outage stops the motion that ran unattended (#521).
// The existing OnStep tests use the TCP seam, which has no device node to
// remove, so these run the driver over a pty reached through a symlink.
// The write-side loss mapping (fail_write_locked) has no case of its own: on
// a severed pty the removed-node check before every write fires first, so a
// pty cannot make the write itself return EIO with the node still present.

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/motion_policy.h>
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>
#include <alpacacore/vendor/onstep/onstep_telescope_driver.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_pty_write.h"
#include "fake_task_clock.h"

namespace {

using alpacacore::vendor::onstep::ConnectionInfo;
using alpacacore::vendor::onstep::ConnectionType;
using alpacacore::vendor::onstep::OnStepProtocolWrapper;
using alpacacore::vendor::onstep::detail::LinkLossHookPoint;

// A minimal OnStepX behind a pseudo-terminal: answers the LX200 commands the
// driver's connect and position reads send, and records every command.
// Unterminated set commands (:S...) ack "1"; stops (:Q...) are silent, as on
// the firmware.
class FakeOnStepSerialMount {
public:
    FakeOnStepSerialMount() : pty_("FakeOnStepSerialMount") {
        worker_ = std::thread([this] { run(); });
    }

    ~FakeOnStepSerialMount() { stop_and_join(); }

    FakeOnStepSerialMount(const FakeOnStepSerialMount&) = delete;
    FakeOnStepSerialMount& operator=(const FakeOnStepSerialMount&) = delete;

    std::string slave_path() const { return pty_.slave_path(); }

    /// USB unplug: stop answering and close both pty ends, so the slave node
    /// is gone, writes fail with EIO and reads see the hang-up.
    void sever_link() {
        stop_and_join();
        pty_.sever();
    }

    /// Delay every reply by `ms`, so a test can act while an exchange holds
    /// the driver's link.
    void hold_replies(int ms) { hold_ms_.store(ms); }

    /// A mount that stops answering with the adapter still plugged in (mount
    /// unpowered, cable pulled at the mount end; #1414): the port stays open
    /// and healthy, commands are read and logged, nothing is answered.
    void mute(bool muted) { muted_.store(muted); }

    /// Goes silent when `command` arrives, without answering it.
    void mute_on(const std::string& command) {
        std::lock_guard<std::mutex> lock(mutex_);
        mute_on_ = command;
    }

    /// Answers `command` with `reply` instead of the default.
    void set_reply(const std::string& command, const std::string& reply) {
        std::lock_guard<std::mutex> lock(mutex_);
        replies_[command] = reply;
    }

    /// Unplug in the middle of an exchange: when `command` arrives the fake
    /// closes the pty instead of replying, so the driver is already reading.
    void sever_on(const std::string& command) {
        std::lock_guard<std::mutex> lock(mutex_);
        sever_on_ = command;
    }

    /// As sever_on(), but for the command that follows `command`.
    void sever_after(const std::string& command) {
        std::lock_guard<std::mutex> lock(mutex_);
        sever_after_ = command;
    }

    std::vector<std::string> commands() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return commands_;
    }

    bool received(const std::string& command) const {
        const auto all = commands();
        return std::find(all.begin(), all.end(), command) != all.end();
    }

    bool received_any_stop() const {
        for (const auto& c : commands()) {
            if (c.rfind(":Q", 0) == 0) {
                return true;
            }
        }
        return false;
    }

private:
    void stop_and_join() {
        stop_.store(true);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    static std::string reply_to(const std::string& cmd) {
        if (cmd == ":GVP#") return "On-Step#";
        if (cmd == ":GVN#") return "10.23a#";
        if (cmd == ":GU#") return "nNpHEo190#";
        if (cmd == ":GRa#") return "01:00:00.0000#";
        if (cmd == ":GDe#") return "+10*00:00.000#";
        if (cmd == ":GA#") return "+40*00:00#";
        if (cmd == ":GZ#") return "000*00:00#";
        if (cmd.rfind(":Q", 0) == 0) return "";
        if (cmd.rfind(":S", 0) == 0 || cmd == ":GXEE#") return "1";
        return "0#";
    }

    void run() {
        std::string pending;
        char buf[64];
        while (!stop_.load()) {
            struct pollfd pfd {};
            pfd.fd = pty_.master_fd();
            pfd.events = POLLIN;
            if (poll(&pfd, 1, 20) <= 0) {
                continue;
            }
            const ssize_t n = read(pty_.master_fd(), buf, sizeof(buf));
            if (n <= 0) {
                continue;
            }
            pending.append(buf, static_cast<std::size_t>(n));
            std::size_t end = 0;
            while ((end = pending.find('#')) != std::string::npos) {
                const std::string cmd = pending.substr(0, end + 1);
                pending.erase(0, end + 1);
                bool cut = false;
                std::string reply = reply_to(cmd);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    commands_.push_back(cmd);
                    cut = (!sever_on_.empty() && cmd == sever_on_) || sever_next_;
                    sever_next_ = !sever_after_.empty() && cmd == sever_after_;
                    if (!mute_on_.empty() && cmd == mute_on_) {
                        muted_.store(true);
                    }
                    if (const auto it = replies_.find(cmd); it != replies_.end()) {
                        reply = it->second;
                    }
                }
                if (cut) {
                    // Let the driver reach its read() first, so the hang-up
                    // lands inside the read (EIO) and not before it.
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    pty_.sever();
                    return;
                }
                if (muted_.load()) {
                    continue;
                }
                if (const int hold = hold_ms_.load(); hold > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(hold));
                }
                if (!reply.empty()) {
                    alpacacore::test::pty_write_bounded(pty_.master_fd(), reply.c_str(), reply.size(), stop_);
                }
            }
        }
    }

    alpacacore::test::PtyPair pty_;
    std::atomic<bool> stop_{false};
    std::atomic<int> hold_ms_{0};
    std::atomic<bool> muted_{false};
    mutable std::mutex mutex_;
    std::string mute_on_;                         // under mutex_
    std::map<std::string, std::string> replies_;  // under mutex_
    std::vector<std::string> commands_;
    std::string sever_on_;
    std::string sever_after_;  // under mutex_
    bool sever_next_ = false;  // under mutex_
    std::thread worker_;
};

// A by-id style symlink to a fake's pty. A removed link is never reissued,
// unlike a freed /dev/pts/N under ctest -j, so the driver connects through
// this and an unplug removes or re-points it (the QHY and Sky-Watcher shape).
struct PortLink {
    std::filesystem::path path;
    explicit PortLink(const std::string& target) {
        path = std::filesystem::temp_directory_path() /
               ("alpacacore-onstep1241-" + std::to_string(::getpid()) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_symlink(target, path);
    }
    void remove() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    void repoint(const std::string& target) {
        remove();
        std::filesystem::create_symlink(target, path);
    }
    ~PortLink() { remove(); }
    PortLink(const PortLink&) = delete;
    PortLink& operator=(const PortLink&) = delete;
};

ConnectionInfo serial_info(const PortLink& link) {
    ConnectionInfo info;
    info.type = ConnectionType::Serial;
    info.port_path = link.path.string();
    info.response_timeout_ms = 2000;
    return info;
}

void require_not_connected_error(const std::function<void()>& fn) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        INFO(ex.what());
        REQUIRE(ex.error_code() == alpacacore::AlpacaError::NotConnected);
    }
}

// The wrapper is a process-wide singleton: leave it disconnected and with no
// loss stamp, so the next case's first connect is a fresh one.
// Reset on entry too, in case an earlier OnStep test left it connected.
struct WrapperReset {
    WrapperReset() { reset(); }
    ~WrapperReset() { reset(); }
    WrapperReset(const WrapperReset&) = delete;
    WrapperReset& operator=(const WrapperReset&) = delete;
    static void reset() {
        auto& protocol = OnStepProtocolWrapper::instance();
        protocol.disconnect();
        (void)protocol.consume_link_lost_at();
        (void)protocol.consume_last_alive_before_loss();
    }
};

// Joins a thread on every way out of a scope, so a failed REQUIRE while it
// runs fails the case instead of terminating the test binary.
struct JoinOnExit {
    std::thread& thread;
    ~JoinOnExit() {
        if (thread.joinable()) {
            thread.join();
        }
    }
};

struct WindowReset {
    ~WindowReset() {
        alpacacore::vendor::onstep::detail::set_relink_motion_preserve_window(
            std::chrono::duration_cast<std::chrono::milliseconds>(alpacacore::util::kRelinkMotionPreserveWindow));
    }
};

// A short reply timeout, so each read of a muted mount fails quickly.
ConnectionInfo quick_serial_info(const PortLink& link) {
    ConnectionInfo info = serial_info(link);
    info.response_timeout_ms = 200;
    return info;
}

// Clears the link_alive() test hook on every way out of a case.
struct LossHookReset {
    ~LossHookReset() { alpacacore::vendor::onstep::detail::set_link_loss_hook(nullptr); }
};

}  // namespace

TEST_CASE("OnStep link loss - Connected drops when the adapter is pulled with no client I/O",
          "[onstep][serial][connected]") {
    WrapperReset reset;
    FakeOnStepSerialMount mount;
    PortLink link(mount.slave_path());
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, serial_info(link));
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE(driver->get_link_fault().empty());

    mount.sever_link();
    link.remove();

    // The management listing alone notices the unplug: nothing has read
    // Connected or run an operation yet.
    CHECK(driver->get_link_fault().find("lost") != std::string::npos);
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_device_state().empty());
    const auto started = std::chrono::steady_clock::now();
    require_not_connected_error([&] { (void)driver->get_right_ascension(); });
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500));

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().empty());
    // With no link at all the wrapper answers NotConnected too, not a
    // generic driver error.
    require_not_connected_error([] { (void)OnStepProtocolWrapper::instance().send_command(":GVP#"); });
}

TEST_CASE("OnStep link loss - a reply cut off by the unplug fails NotConnected before the timeout",
          "[onstep][serial][connected]") {
    WrapperReset reset;
    FakeOnStepSerialMount mount;
    PortLink link(mount.slave_path());
    auto& protocol = OnStepProtocolWrapper::instance();
    auto info = serial_info(link);
    info.response_timeout_ms = 5000;
    REQUIRE(protocol.connect(info));
    REQUIRE(protocol.link_alive());

    // The node is still there when the read starts; the read itself must
    // see the hang-up and give up, not wait out the 5 s response timeout.
    mount.sever_on(":GRa#");
    const auto started = std::chrono::steady_clock::now();
    require_not_connected_error([&] { (void)protocol.send_command(":GRa#"); });
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(1000));
    // The pty reports the hang-up as EIO on that read. (Were it missed, the
    // next read returns 0 and the removed-node check would still catch it, so
    // the reason is what shows the read's own errno was acted on.)
    CHECK(protocol.link_lost_reason().find("read failed") != std::string::npos);
    CHECK_FALSE(protocol.link_alive());
    CHECK(protocol.consume_link_lost_at().has_value());
}

// #1430: FindHome reads :GE# right after :hC#. A link lost in between must
// fail that read (NotConnected), not read as "no code": FindHome would then
// report success on a dead link.
TEST_CASE("OnStep link loss - a link lost before the :GE# read fails it instead of reading no code",
          "[onstep][serial][connected]") {
    WrapperReset reset;
    FakeOnStepSerialMount mount;
    PortLink link(mount.slave_path());
    auto& protocol = OnStepProtocolWrapper::instance();
    REQUIRE(protocol.connect(serial_info(link)));
    REQUIRE(protocol.link_alive());

    mount.sever_on(":GE#");
    protocol.find_home();
    require_not_connected_error([&] { (void)protocol.get_last_command_error(); });
    CHECK_FALSE(protocol.link_alive());
}

// #1430: on a link that is closed rather than lost, the :GE# read fails too,
// instead of reading as "no code".
TEST_CASE("OnStep link loss - the :GE# read fails NotConnected while disconnected", "[onstep][serial][connected]") {
    WrapperReset reset;
    auto& protocol = OnStepProtocolWrapper::instance();
    REQUIRE_FALSE(protocol.is_connected());
    require_not_connected_error([&] { (void)protocol.get_last_command_error(); });
}

TEST_CASE("OnStep link loss - Connected=true on a lost link reconnects instead of short-circuiting",
          "[onstep][serial][connected]") {
    WrapperReset reset;
    FakeOnStepSerialMount first;
    PortLink link(first.slave_path());
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, serial_info(link));
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // A redundant connect on a live link sends nothing.
    const auto before = first.commands().size();
    driver->set_connected(true);
    CHECK(first.commands().size() == before);

    // Build the replugged mount before cutting the first, so its pty cannot
    // take over the freed /dev/pts index.
    FakeOnStepSerialMount replugged;
    first.sever_link();
    link.repoint(replugged.slave_path());
    REQUIRE_FALSE(driver->get_connected());

    driver->set_connected(true);
    CHECK(driver->get_connected());
    CHECK(replugged.received(":GVP#"));
    CHECK(driver->get_link_fault().empty());
    CHECK_NOTHROW((void)driver->get_right_ascension());
    driver->set_connected(false);
}

// #910: Connected is read lock-free from a flag raised only at the end of a
// connect. The relink drops it first, so the lost session's Connected cannot
// show through the new link's warm-up while Connecting is still true.
TEST_CASE("OnStep link loss - Connected stays false through the relink's warm-up", "[onstep][serial][connected]") {
    WrapperReset reset;
    FakeOnStepSerialMount first;
    PortLink link(first.slave_path());
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, serial_info(link));
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    FakeOnStepSerialMount replugged;
    replugged.hold_replies(300);  // a slow warm-up on the new link
    first.sever_link();
    link.repoint(replugged.slave_path());

    driver->connect();
    // The firmware query (:GVP#) is the first step of the warm-up on the new
    // link, and every reply takes 300 ms, so the warm-up still has seconds to
    // run when it arrives: one sample there sees the relink mid-way.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!replugged.received(":GVP#") && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(replugged.received(":GVP#"));
    const bool connected_mid_relink = driver->get_connected();
    CHECK(driver->get_connecting());
    CHECK_FALSE(connected_mid_relink);
    const auto done_by = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (driver->get_connecting() && std::chrono::steady_clock::now() < done_by) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE_FALSE(driver->get_connecting());
    CHECK(driver->get_connected());
    driver->set_connected(false);
}

// #1289 item 3: link_alive() checks the node with no mutex_ held. A Disconnect
// that lands between the stat() and the record must not leave a loss behind,
// or the next Connect sends the #521 stops to a mount nobody lost.
TEST_CASE("OnStep link loss - a Disconnect during the node check records no loss", "[onstep][serial][connected]") {
    WrapperReset reset;
    LossHookReset hook_reset;
    auto& protocol = OnStepProtocolWrapper::instance();
    FakeOnStepSerialMount mount;
    PortLink link(mount.slave_path());
    REQUIRE(protocol.connect(serial_info(link)));
    REQUIRE(protocol.link_alive());

    link.remove();  // the node is gone, so the next check finds a loss
    bool hook_ran = false;
    alpacacore::vendor::onstep::detail::set_link_loss_hook([&](LinkLossHookPoint point) {
        if (point == LinkLossHookPoint::AliveBeforeRecord && !hook_ran) {
            hook_ran = true;
            protocol.disconnect();  // the client's deliberate Disconnect
        }
    });
    CHECK_FALSE(protocol.link_alive());
    REQUIRE(hook_ran);
    CHECK(protocol.link_lost_reason().empty());
    CHECK_FALSE(protocol.consume_link_lost_at().has_value());
    CHECK_FALSE(protocol.consume_last_alive_before_loss().has_value());
}

// The same race with the Disconnect still in progress: it has marked the link
// down but not yet closed the port when link_alive() goes to record. The
// snapshot link_alive() took before it must not match by then.
TEST_CASE("OnStep link loss - a Disconnect still closing the port records no loss", "[onstep][serial][connected]") {
    WrapperReset reset;
    LossHookReset hook_reset;
    auto& protocol = OnStepProtocolWrapper::instance();
    FakeOnStepSerialMount mount;
    PortLink link(mount.slave_path());
    REQUIRE(protocol.connect(serial_info(link)));
    REQUIRE(protocol.link_alive());
    link.remove();

    std::mutex m;
    std::condition_variable cv;
    bool disconnect_paused = false;
    bool release_disconnect = false;
    std::thread disconnector;
    JoinOnExit join{disconnector};
    alpacacore::vendor::onstep::detail::set_link_loss_hook([&](LinkLossHookPoint point) {
        if (point == LinkLossHookPoint::AliveBeforeRecord && !disconnector.joinable()) {
            // link_alive() has its snapshot; start a Disconnect and wait until
            // it is between marking the link down and closing the port.
            disconnector = std::thread([&] { protocol.disconnect(); });
            std::unique_lock<std::mutex> lock(m);
            cv.wait_for(lock, std::chrono::seconds(5), [&] { return disconnect_paused; });
        } else if (point == LinkLossHookPoint::DisconnectBeforeClose) {
            std::unique_lock<std::mutex> lock(m);
            disconnect_paused = true;
            cv.notify_all();
            cv.wait_for(lock, std::chrono::seconds(5), [&] { return release_disconnect; });
        }
    });
    CHECK_FALSE(protocol.link_alive());
    {
        std::lock_guard<std::mutex> lock(m);
        REQUIRE(disconnect_paused);
        release_disconnect = true;
    }
    cv.notify_all();
    disconnector.join();
    CHECK(protocol.link_lost_reason().empty());
    CHECK_FALSE(protocol.consume_link_lost_at().has_value());
    CHECK_FALSE(protocol.consume_last_alive_before_loss().has_value());
}

// #1289 item 3, the relink half: a reconnect to the replugged adapter that
// lands in the same window must not see its new link marked lost and closed.
TEST_CASE("OnStep link loss - a reconnect during the node check keeps the new link", "[onstep][serial][connected]") {
    WrapperReset reset;
    LossHookReset hook_reset;
    auto& protocol = OnStepProtocolWrapper::instance();
    FakeOnStepSerialMount first;
    PortLink link(first.slave_path());
    REQUIRE(protocol.connect(serial_info(link)));
    REQUIRE(protocol.link_alive());

    FakeOnStepSerialMount replugged;
    first.sever_link();
    link.repoint(replugged.slave_path());
    bool hook_ran = false;
    bool reconnected = false;
    alpacacore::vendor::onstep::detail::set_link_loss_hook([&](LinkLossHookPoint point) {
        if (point == LinkLossHookPoint::AliveBeforeRecord && !hook_ran) {
            hook_ran = true;
            protocol.disconnect();
            reconnected = protocol.connect(serial_info(link));
        }
    });
    // Checked the old node; the relink ran inside. The call reports the link
    // that is current now, which is the fresh one.
    CHECK(protocol.link_alive());
    REQUIRE(hook_ran);
    REQUIRE(reconnected);
    alpacacore::vendor::onstep::detail::set_link_loss_hook(nullptr);
    CHECK(protocol.link_lost_reason().empty());
    CHECK(protocol.is_connected());
    CHECK(protocol.link_alive());
    CHECK_NOTHROW((void)protocol.send_command(":GVP#"));
    CHECK(replugged.received(":GVP#"));
}

// The relink can also land after the loss is recorded and before link_alive()
// closes the port: the close must not take the fresh link down with it.
TEST_CASE("OnStep link loss - a reconnect after the loss is recorded keeps the new link",
          "[onstep][serial][connected]") {
    WrapperReset reset;
    LossHookReset hook_reset;
    auto& protocol = OnStepProtocolWrapper::instance();
    FakeOnStepSerialMount first;
    PortLink link(first.slave_path());
    REQUIRE(protocol.connect(serial_info(link)));
    REQUIRE(protocol.link_alive());

    FakeOnStepSerialMount replugged;
    first.sever_link();
    link.repoint(replugged.slave_path());
    bool hook_ran = false;
    bool reconnected = false;
    alpacacore::vendor::onstep::detail::set_link_loss_hook([&](LinkLossHookPoint point) {
        if (point == LinkLossHookPoint::AliveBeforeClose && !hook_ran) {
            hook_ran = true;
            protocol.disconnect();
            reconnected = protocol.connect(serial_info(link));
        }
    });
    // The old link was lost and recorded; the call reports the fresh link that
    // replaced it before the close.
    CHECK(protocol.link_alive());
    REQUIRE(hook_ran);
    REQUIRE(reconnected);
    alpacacore::vendor::onstep::detail::set_link_loss_hook(nullptr);
    CHECK(protocol.is_connected());
    CHECK(protocol.link_alive());
    CHECK_NOTHROW((void)protocol.send_command(":GVP#"));
    CHECK(replugged.received(":GVP#"));
}

TEST_CASE("OnStep link loss - the reconnect stops motion only after a long outage", "[onstep][serial][connected]") {
    WrapperReset reset;
    WindowReset window_reset;
    // The outage is timed on the driver's clock, which the wrapper's loss
    // stamps read too (decision 0005); declared before the driver.
    alpacacore::test::FakeTaskClock clock;
    FakeOnStepSerialMount first;
    PortLink link(first.slave_path());
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, serial_info(link), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    // A first connect has no recorded loss, so it stops nothing.
    CHECK_FALSE(first.received_any_stop());

    FakeOnStepSerialMount replugged;
    first.sever_link();
    link.repoint(replugged.slave_path());

    SECTION("outage past the window: AbortSlew's stops are sent") {
        REQUIRE_FALSE(driver->get_connected());  // the loss is seen, and stamped, here
        alpacacore::vendor::onstep::detail::set_relink_motion_preserve_window(std::chrono::milliseconds(0));
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        CHECK(replugged.received(":Q#"));
        CHECK(replugged.received(":Qn#"));
        CHECK(replugged.received(":Qs#"));
        CHECK(replugged.received(":Qe#"));
        CHECK(replugged.received(":Qw#"));
    }
    SECTION("outage inside the window: motion is left alone") {
        REQUIRE_FALSE(driver->get_connected());  // the loss is seen, and stamped, here
        alpacacore::vendor::onstep::detail::set_relink_motion_preserve_window(std::chrono::hours(1));
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        CHECK_FALSE(replugged.received_any_stop());
    }
    SECTION("the management listing alone notices the loss, and a watched outage keeps motion") {
        REQUIRE_FALSE(driver->get_link_fault().empty());  // only the listing has looked
        alpacacore::vendor::onstep::detail::set_relink_motion_preserve_window(std::chrono::hours(1));
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        CHECK_FALSE(replugged.received_any_stop());
    }
    SECTION("nobody looked for longer than the window: the reconnect stops, even after its own Connected read") {
        // Nothing reads Connected or runs an operation for longer than the
        // window, then Connected is read right before the reconnect, as the
        // router does before every Connect. That read is the first to notice
        // the loss, so timing from it would call the outage 0 s; the outage
        // is timed from the last moment the link was seen alive instead.
        alpacacore::vendor::onstep::detail::set_relink_motion_preserve_window(std::chrono::milliseconds(1500));
        clock.advance(std::chrono::milliseconds(2000));
        REQUIRE_FALSE(driver->get_connected());
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        CHECK(replugged.received(":Q#"));
        CHECK(replugged.received(":Qn#"));
    }
    driver->set_connected(false);
}

// #1460: the process-wide wrapper takes its loss stamps on the clock of the
// driver that connected last. A stamp taken on one driver's clock means
// nothing on the next driver's, so it goes with the clock instead of being
// timed as an outage there.
TEST_CASE("OnStep link loss - a loss stamped on one driver's clock is not timed on the next driver's",
          "[onstep][serial][connected]") {
    WrapperReset reset;
    WindowReset window_reset;
    alpacacore::test::FakeTaskClock first_clock;
    alpacacore::test::FakeTaskClock second_clock;
    FakeOnStepSerialMount first;
    FakeOnStepSerialMount replugged;
    PortLink link(first.slave_path());
    {
        auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, serial_info(link), first_clock);
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        first.sever_link();
        link.repoint(replugged.slave_path());
        REQUIRE_FALSE(driver->get_connected());  // the loss is seen, and stamped on first_clock, here
    }
    // Any recorded outage would stop motion now.
    alpacacore::vendor::onstep::detail::set_relink_motion_preserve_window(std::chrono::milliseconds(0));
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, serial_info(link), second_clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK_FALSE(replugged.received_any_stop());
    driver->set_connected(false);
}

// #1460: setting another clock, or releasing the one in use, forgets the
// loss and last-alive stamps taken on the old one.
TEST_CASE("OnStep link loss - a change of clock forgets the wrapper's loss stamps", "[onstep][serial][connected]") {
    WrapperReset reset;
    alpacacore::test::FakeTaskClock first_clock;
    alpacacore::test::FakeTaskClock second_clock;
    auto& protocol = OnStepProtocolWrapper::instance();
    struct ClockReset {
        alpacacore::test::FakeTaskClock& a;
        alpacacore::test::FakeTaskClock& b;
        ~ClockReset() {
            auto& wrapper = OnStepProtocolWrapper::instance();
            wrapper.release_task_clock(a);
            wrapper.release_task_clock(b);
        }
    } clock_reset{first_clock, second_clock};
    FakeOnStepSerialMount mount;
    PortLink link(mount.slave_path());
    protocol.set_task_clock(first_clock);
    REQUIRE(protocol.connect(serial_info(link)));
    mount.sever_link();
    REQUIRE_FALSE(protocol.link_alive());  // the loss is stamped on first_clock

    SECTION("another clock is set") { protocol.set_task_clock(second_clock); }
    SECTION("the clock in use is released") { protocol.release_task_clock(first_clock); }
    CHECK_FALSE(protocol.consume_link_lost_at().has_value());
    CHECK_FALSE(protocol.consume_last_alive_before_loss().has_value());
}

// #1460: only a change of the clock in use forgets the stamps. A driver that
// never connected, or one on the process clock, releases nothing, so the
// lost driver's own reconnect still times its outage.
TEST_CASE("OnStep link loss - a driver that does not own the clock leaves the loss stamps alone",
          "[onstep][serial][connected]") {
    WrapperReset reset;
    WindowReset window_reset;
    alpacacore::test::FakeTaskClock fake_clock;
    alpacacore::test::FakeTaskClock other_clock;
    FakeOnStepSerialMount first;
    FakeOnStepSerialMount replugged;
    PortLink link(first.slave_path());

    const auto lose_then_reconnect = [&](alpacacore::util::TaskClock& own, alpacacore::util::TaskClock& other) {
        auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, serial_info(link), own);
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        first.sever_link();
        link.repoint(replugged.slave_path());
        REQUIRE_FALSE(driver->get_connected());  // the loss is stamped here
        { auto bystander = alpacacore::vendor::onstep::create_onstep_telescope(1, serial_info(link), other); }
        alpacacore::vendor::onstep::detail::set_relink_motion_preserve_window(std::chrono::milliseconds(0));
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        CHECK(replugged.received(":Q#"));  // the outage was still timed
        driver->set_connected(false);
    };
    SECTION("a driver on another clock that never connected") { lose_then_reconnect(fake_clock, other_clock); }
    SECTION("two drivers on the process clock") {
        lose_then_reconnect(alpacacore::util::default_task_clock(), alpacacore::util::default_task_clock());
    }
}

TEST_CASE("OnStep link loss - a loss found during an exchange closes the fd once the exchange ends",
          "[onstep][serial][connected]") {
    WrapperReset reset;
    FakeOnStepSerialMount first;
    FakeOnStepSerialMount second;
    PortLink link(first.slave_path());
    auto& protocol = OnStepProtocolWrapper::instance();
    REQUIRE(protocol.connect(serial_info(link)));

    // An exchange holds the link while the path is re-pointed to another
    // adapter: link_alive() finds the loss but cannot close the busy fd.
    first.hold_replies(300);
    std::thread exchange([&] {
        try {
            (void)protocol.send_command(":GRa#");
        } catch (...) {  // NOLINT(bugprone-empty-catch)
        }
    });
    JoinOnExit join_exchange{exchange};
    // The fake has the command only once the driver wrote it under the
    // wrapper mutex, which the exchange keeps until the held reply arrives.
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!first.received(":GRa#") && std::chrono::steady_clock::now() < give_up) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(first.received(":GRa#"));
    link.repoint(second.slave_path());
    CHECK_FALSE(protocol.link_alive());
    CHECK(protocol.is_connected());  // the fd is still open: the exchange holds it
    exchange.join();

    SECTION("the next link_alive() closes it") {
        CHECK_FALSE(protocol.link_alive());
        CHECK_FALSE(protocol.is_connected());
    }
    SECTION("the next exchange closes it and fails NotConnected") {
        require_not_connected_error([&] { (void)protocol.send_command(":GVP#"); });
        CHECK_FALSE(protocol.is_connected());
    }
    SECTION("the next connect closes it instead of refusing a second mount") {
        CHECK(protocol.connect(serial_info(link)));
        CHECK(protocol.link_alive());
    }
}

TEST_CASE("OnStep link loss - a reconnect that auto-detects a new port still stops motion after a long outage",
          "[onstep][serial][connected]") {
    WrapperReset reset;
    WindowReset window_reset;
    FakeOnStepSerialMount first;
    PortLink old_port(first.slave_path());
    FakeOnStepSerialMount replugged;
    PortLink new_port(replugged.slave_path());
    // Auto-detect: the scan finds the old port first, and after the replug
    // the adapter answers under a new name (the held fd kept ttyUSB0 busy).
    int scans = 0;
    auto resolver = [&]() {
        auto info = serial_info(scans++ == 0 ? old_port : new_port);
        return info;
    };
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope_deferred(0, resolver);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    first.sever_link();
    old_port.remove();
    alpacacore::vendor::onstep::detail::set_relink_motion_preserve_window(std::chrono::milliseconds(0));
    REQUIRE_FALSE(driver->get_connected());  // the router's read before a Connect
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(scans == 2);
    CHECK(replugged.received(":Q#"));
    CHECK(replugged.received(":Qn#"));
    driver->set_connected(false);
}

TEST_CASE("OnStep link loss - a command after an unnoticed unplug fails NotConnected at once",
          "[onstep][serial][connected]") {
    WrapperReset reset;
    FakeOnStepSerialMount mount;
    PortLink link(mount.slave_path());
    auto& protocol = OnStepProtocolWrapper::instance();
    REQUIRE(protocol.connect(serial_info(link)));

    // Nothing has looked since the unplug; the path is still there (now
    // dangling). The check before the write finds the removed node.
    mount.sever_link();
    require_not_connected_error([&] { (void)protocol.send_command(":GVP#"); });
    CHECK(protocol.link_lost_reason().find("the adapter was removed") != std::string::npos);
    CHECK_FALSE(protocol.is_connected());
}

// #1414 (decision 0009): a mount that stops answering while its adapter stays
// plugged in was served from the caches for ever. Three failed reads in a row
// latch a fault: Connected stays true and reads throw DriverException with the
// fault text instead of serving the cache; the next good reply clears it.
TEST_CASE("OnStep link loss - a mount that stops answering latches a fault and a reply clears it",
          "[onstep][serial][connected]") {
    WrapperReset reset;
    alpacacore::test::FakeTaskClock clock;  // the caches and the fault run on it (decision 0005)
    FakeOnStepSerialMount mount;
    PortLink link(mount.slave_path());
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, quick_serial_info(link), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    driver->move_axis(0, 1.0);  // a MoveAxis runs when the mount goes silent
    const double ra = driver->get_right_ascension();
    clock.advance(std::chrono::milliseconds(2100));  // past the 2 s position cache
    const bool tracking = driver->get_tracking();    // a fresh status cache

    mount.mute(true);
    // Two failed reads: transient, the cache is still served.
    CHECK(driver->get_right_ascension() == ra);
    CHECK(driver->get_right_ascension() == ra);
    CHECK(driver->get_link_fault().empty());
    // The third latches the fault.
    try {
        (void)driver->get_right_ascension();
        FAIL("a third failed read served the cache");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("communications compromised") != std::string::npos);
    }
    CHECK(driver->get_connected());
    CHECK(driver->get_link_fault().find("not answering") != std::string::npos);
    // While faulted no cache is served, not even the ones read without a
    // refresh (SideOfPier) or inside their 2 s window (the status read above,
    // well under 2 s ago), and writes throw too (decision 0009 rule 1).
    const auto require_fault_error = [](const std::function<void()>& fn, const char* what) {
        try {
            fn();
            FAIL(what);
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        }
    };
    require_fault_error([&] { (void)driver->get_tracking(); }, "Tracking served the status cache during a fault");
    require_fault_error([&] { (void)driver->get_side_of_pier(); }, "SideOfPier served the cache during a fault");
    require_fault_error([&] { driver->set_tracking(!tracking); }, "a Tracking write succeeded during a fault");
    // Every motion and state command is refused; the blind ones would
    // otherwise report success. The prefix names the refusing command.
    const auto require_refused = [](const std::function<void()>& fn, const std::string& what) {
        try {
            fn();
            FAIL(what + " succeeded during a fault");
        } catch (const alpacacore::AlpacaException& ex) {
            INFO(ex.what());
            CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
            CHECK(std::string(ex.what()).rfind(what + ": ", 0) == 0);
        }
    };
    require_refused([&] { driver->pulse_guide(0, 100); }, "PulseGuide");
    require_refused([&] { driver->set_park(); }, "SetPark");
    require_refused([&] { driver->unpark(); }, "Unpark");
    require_refused([&] { driver->move_axis(1, 1.0); }, "MoveAxis");
    require_refused([&] { driver->sync_to_coordinates(2.0, 10.0); }, "SyncToCoordinates");
    // The Alt/Az slew reports under the coordinate slew's name (it becomes one).
    require_refused([&] { driver->slew_to_alt_az_async(45.0, 180.0); }, "SlewToCoordinatesAsync");
    require_refused([&] { driver->find_home(); }, "FindHome");
    require_refused([&] { driver->park(); }, "Park");

    SECTION("a MoveAxis stop is sent, then refused, and ends the move") {
        require_refused([&] { driver->move_axis(0, 0.0); }, "MoveAxis stop");
        CHECK(mount.received(":Qe#"));
        mount.mute(false);
        CHECK_FALSE(driver->get_slewing());  // the stopped axis no longer counts as moving
        CHECK(driver->get_link_fault().empty());
    }
    SECTION("an AbortSlew is sent, then refused") {
        require_fault_error([&] { driver->abort_slew(); }, "AbortSlew reported success during a fault");
        CHECK(mount.received(":Q#"));  // the stop was still sent
    }
    SECTION("a position reply clears it") {
        mount.mute(false);
        CHECK(driver->get_right_ascension() == ra);
        CHECK(driver->get_link_fault().empty());
    }
    SECTION("a status reply clears it") {
        mount.mute(false);
        (void)driver->get_tracking();
        CHECK(driver->get_link_fault().empty());
    }
    SECTION("an Alt/Az reply clears it") {
        mount.mute(false);
        (void)driver->get_altitude();
        CHECK(driver->get_link_fault().empty());
    }
    SECTION("a disconnect clears it") {
        driver->set_connected(false);
        CHECK(driver->get_link_fault().empty());
    }

    driver->set_connected(false);
}

// #1414: while faulted no cache is served, not even one still inside its 2 s
// window: the RA and Alt/Az caches read just before the fault latched (on the
// status reads) are not served.
TEST_CASE("OnStep link loss - a fault latched on status reads serves no fresh position cache",
          "[onstep][serial][connected]") {
    WrapperReset reset;
    alpacacore::test::FakeTaskClock clock;
    FakeOnStepSerialMount mount;
    PortLink link(mount.slave_path());
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, quick_serial_info(link), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    (void)driver->get_tracking();
    clock.advance(std::chrono::milliseconds(2100));  // the status cache is now stale
    (void)driver->get_right_ascension();             // fresh position caches
    (void)driver->get_altitude();

    mount.mute(true);
    (void)driver->get_tracking();  // two failed status reads: the stale cache is served
    (void)driver->get_tracking();
    CHECK_THROWS_AS(driver->get_tracking(), alpacacore::AlpacaException);  // the third latches
    REQUIRE_FALSE(driver->get_link_fault().empty());
    const auto require_driver_error = [](const std::function<void()>& fn) {
        try {
            fn();
            FAIL("a fresh cache was served during a fault");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        }
    };
    require_driver_error([&] { (void)driver->get_right_ascension(); });
    require_driver_error([&] { (void)driver->get_altitude(); });
    driver->set_connected(false);
}

// #1414: a read that finds the link gone (NotConnected) is a loss (#1241),
// not a silent mount: it does not count toward the fault.
TEST_CASE("OnStep link loss - a lost link during the failed reads is not latched as a fault",
          "[onstep][serial][connected]") {
    WrapperReset reset;
    alpacacore::test::FakeTaskClock clock;
    FakeOnStepSerialMount mount;
    PortLink link(mount.slave_path());
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, quick_serial_info(link), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    (void)driver->get_right_ascension();
    clock.advance(std::chrono::milliseconds(2100));

    mount.mute(true);
    (void)driver->get_right_ascension();  // two failed reads
    (void)driver->get_right_ascension();
    mount.sever_on(":GRa#");  // the third finds the adapter gone
    try {
        (void)driver->get_right_ascension();
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() != alpacacore::AlpacaError::DriverException);  // not a latched fault
    }
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().find("not answering") == std::string::npos);
}

// #1414: the wrapper records a loss only for a link it still holds.
TEST_CASE("OnStep link loss - declaring a lost link while disconnected records nothing",
          "[onstep][serial][connected]") {
    WrapperReset reset;
    auto& protocol = OnStepProtocolWrapper::instance();
    REQUIRE_FALSE(protocol.is_connected());
    protocol.declare_link_lost("test");
    CHECK(protocol.link_lost_reason().empty());
    CHECK_FALSE(protocol.consume_link_lost_at().has_value());
}

// #1414 (decision 0009): a fault that has stood for the staleness bound is a
// lost link: Connected false, NotConnected, the fault text kept; only the
// client's Connect relinks.
TEST_CASE("OnStep link loss - a fault standing past the staleness bound loses the link",
          "[onstep][serial][connected]") {
    WrapperReset reset;
    alpacacore::test::FakeTaskClock clock;  // the fault's age and the outage run on it (decision 0005)
    FakeOnStepSerialMount mount;
    PortLink link(mount.slave_path());
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, quick_serial_info(link), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    mount.mute(true);
    clock.advance(std::chrono::milliseconds(2100));  // past the 2 s position cache
    for (int i = 0; i < 3; ++i) {
        try {
            (void)driver->get_right_ascension();
        } catch (const alpacacore::AlpacaException&) {  // NOLINT(bugprone-empty-catch)
            // the third throws: the fault latched
        }
    }
    REQUIRE(driver->get_connected());  // faulted, not yet lost
    clock.advance(alpacacore::util::kLinkStalenessBound - std::chrono::seconds(1));
    try {
        (void)driver->get_right_ascension();
        FAIL("a read while faulted returned a value");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);  // still faulted, not lost
    }
    REQUIRE(driver->get_connected());
    clock.advance(std::chrono::seconds(2));  // the fault has now stood past the bound
    require_not_connected_error([&] { (void)driver->get_right_ascension(); });
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().find("lost") != std::string::npos);
    CHECK(driver->get_device_state().empty());
    require_not_connected_error([&] { (void)driver->get_tracking(); });

    mount.mute(false);
    REQUIRE_FALSE(mount.received_any_stop());
    // The outage is timed from the loss (the muted reads still found the fd
    // alive), so the relink must come past the #521 preserve window.
    clock.advance(alpacacore::util::kRelinkMotionPreserveWindow + std::chrono::seconds(1));
    driver->set_connected(true);  // only the client relinks
    CHECK(driver->get_connected());
    CHECK(driver->get_link_fault().empty());
    // The loss was recorded as for a pulled adapter, so the relink after a
    // long outage sends the #521 stops.
    CHECK(mount.received_any_stop());

    driver->set_connected(false);
}

// #1414: a latched fault during a MoveAxis is reported by Slewing too, as by
// every read (decision 0009 rule 1); one failed read alone still keeps
// reporting the move.
TEST_CASE("OnStep link loss - Slewing reports a latched fault during a MoveAxis", "[onstep][serial][connected]") {
    WrapperReset reset;
    alpacacore::test::FakeTaskClock clock;
    FakeOnStepSerialMount mount;
    PortLink link(mount.slave_path());
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, quick_serial_info(link), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE_NOTHROW(driver->move_axis(0, 1.0));

    mount.mute(true);
    clock.advance(std::chrono::milliseconds(2100));  // past the 2 s caches
    CHECK(driver->get_slewing());                    // one failed read: still the move
    for (int i = 0; i < 2; ++i) {
        try {
            (void)driver->get_right_ascension();
        } catch (const alpacacore::AlpacaException&) {  // NOLINT(bugprone-empty-catch)
            // the third failed read latches the fault
        }
    }
    try {
        (void)driver->get_slewing();
        FAIL("Slewing hid a latched fault");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
    }
    mount.mute(false);
    driver->set_connected(false);
}

// #1414 / #1230: an AbortSlew refused for a latched fault still gives up the
// tracking restore of the GOTO it aborted, so a mount that answers again
// idle is not sent the tracking start (:ST60.164275#) at the next slew end
// the driver sees.
TEST_CASE("OnStep link loss - a faulted AbortSlew does not restore tracking later", "[onstep][serial][connected]") {
    WrapperReset reset;
    alpacacore::test::FakeTaskClock clock;
    FakeOnStepSerialMount mount;
    mount.set_reply(":GU#", "pEo190#");  // tracking, GOTO running
    PortLink link(mount.slave_path());
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, quick_serial_info(link), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(2.0, 20.0));

    mount.mute(true);
    clock.advance(std::chrono::milliseconds(2100));
    for (int i = 0; i < 3; ++i) {
        try {
            (void)driver->get_right_ascension();
        } catch (const alpacacore::AlpacaException&) {  // NOLINT(bugprone-empty-catch)
            // the third throws: the fault latched
        }
    }
    try {
        driver->abort_slew();
        FAIL("AbortSlew reported success to a silent mount");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
    }

    const auto tracking_starts = [&mount] {
        const auto all = mount.commands();
        return std::count_if(all.begin(), all.end(), [](const std::string& c) { return c.rfind(":ST6", 0) == 0; });
    };
    const auto starts_before = tracking_starts();
    mount.set_reply(":GU#", "nNpEo190#");  // back, idle, not tracking
    mount.mute(false);
    clock.advance(std::chrono::milliseconds(2100));
    for (int i = 0; i < 3; ++i) {
        (void)driver->get_slewing();
    }
    CHECK(tracking_starts() == starts_before);
    driver->set_connected(false);
}

// #1414: a client that only writes after the fault latched still sees the
// link lost once the fault has stood past the bound, as a read would.
TEST_CASE("OnStep link loss - a write refused past the staleness bound loses the link", "[onstep][serial][connected]") {
    WrapperReset reset;
    alpacacore::test::FakeTaskClock clock;
    FakeOnStepSerialMount mount;
    PortLink link(mount.slave_path());
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, quick_serial_info(link), clock);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    mount.mute(true);
    clock.advance(std::chrono::milliseconds(2100));  // past the 2 s position cache
    for (int i = 0; i < 3; ++i) {
        try {
            (void)driver->get_right_ascension();
        } catch (const alpacacore::AlpacaException&) {  // NOLINT(bugprone-empty-catch)
            // the third throws: the fault latched
        }
    }
    REQUIRE(driver->get_connected());
    try {
        driver->set_tracking(true);
        FAIL("a write while faulted returned success");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);  // faulted, not yet stale
    }
    clock.advance(alpacacore::util::kLinkStalenessBound + std::chrono::seconds(1));  // no reads meanwhile
    require_not_connected_error([&] { driver->set_tracking(true); });
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_link_fault().find("lost") != std::string::npos);
}

// #1414 (Diego's decision 0009 note on #1494): a mount that goes silent after
// :hC# leaves the :GE# read with no code, which firmware without :GE# does
// too. A :GU# read tells them apart: a silent mount fails FindHome instead
// of reporting it accepted.
TEST_CASE("OnStep link loss - FindHome fails when the mount goes silent after :hC#", "[onstep][serial][connected]") {
    WrapperReset reset;
    FakeOnStepSerialMount mount;
    mount.set_reply(":GU#", "nNpEo190#");  // not at home
    PortLink link(mount.slave_path());
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, quick_serial_info(link));
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    SECTION("a silent mount") {
        mount.mute_on(":hC#");
        try {
            driver->find_home();
            FAIL("FindHome reported success to a mount that stopped answering");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
            CHECK(std::string(ex.what()).find("did not answer") != std::string::npos);
        }
        CHECK(mount.received(":GE#"));
        CHECK(driver->get_connected());  // one failed read: not yet a fault
    }
    SECTION("a link lost at the :GU# read fails NotConnected") {
        mount.set_reply(":hC#", "");
        mount.set_reply(":GE#", "");  // no code, so the :GU# read follows
        mount.sever_after(":GE#");    // the adapter goes as that :GU# is sent
        require_not_connected_error([&] { driver->find_home(); });
        CHECK_FALSE(driver->get_connected());
    }
    SECTION("firmware without :GE# still homes") {
        mount.set_reply(":hC#", "");
        mount.set_reply(":GE#", "");  // no reply: the read times out
        REQUIRE_NOTHROW(driver->find_home());
    }
    driver->set_connected(false);
}

// #1414: the same for Park. A :hP# answered 0 is followed by :GE#; a mount
// that goes silent there would leave no code and the park counted as
// retried in the background, so Park returned success to a silent mount.
TEST_CASE("OnStep link loss - Park fails when the mount goes silent after its :hP# reply",
          "[onstep][serial][connected]") {
    WrapperReset reset;
    FakeOnStepSerialMount mount;
    mount.set_reply(":GU#", "nNpEo190#");  // not parked
    mount.set_reply(":hP#", "0");
    mount.mute_on(":GE#");
    PortLink link(mount.slave_path());
    auto driver = alpacacore::vendor::onstep::create_onstep_telescope(0, quick_serial_info(link));
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    try {
        driver->park();
        FAIL("Park reported success to a mount that stopped answering");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("did not answer") != std::string::npos);
    }
    CHECK(driver->get_connected());  // one failed read: not yet a fault
    driver->set_connected(false);
}
