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

    /// Unplug in the middle of an exchange: when `command` arrives the fake
    /// closes the pty instead of replying, so the driver is already reading.
    void sever_on(const std::string& command) {
        std::lock_guard<std::mutex> lock(mutex_);
        sever_on_ = command;
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
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    commands_.push_back(cmd);
                    cut = !sever_on_.empty() && cmd == sever_on_;
                }
                if (cut) {
                    // Let the driver reach its read() first, so the hang-up
                    // lands inside the read (EIO) and not before it.
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    pty_.sever();
                    return;
                }
                const std::string reply = reply_to(cmd);
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
    mutable std::mutex mutex_;
    std::vector<std::string> commands_;
    std::string sever_on_;
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
