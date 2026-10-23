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

#ifndef _WIN32

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/vendor/zwo/zwo_mount_identity.h>
#include <alpacacore/vendor/zwo/zwo_telescope_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"
#include "pty_responder.h"

namespace {
using namespace std::chrono_literals;
using namespace alpacacore;
using namespace alpacacore::vendor::zwo;

bool eventually(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        if (predicate()) return true;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}

int error_code(const std::function<void()>& call) {
    try {
        call();
        return 0;
    } catch (const AlpacaException& error) {
        return error.error_code();
    }
}

struct ProbeGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;

    void block() {
        std::unique_lock lock(mutex);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return released; });
    }
    bool wait() {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, 5s, [&] { return entered; });
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
};

// Declare after a future: assertion unwinding releases the responder before joining its caller.
struct ReleaseProbe {
    ProbeGate& gate;
    ~ReleaseProbe() { gate.release(); }
};

struct IdentityRig {
    test::FakeTaskClock clock;
    ProbeGate probe;
    std::atomic<bool> hold_probe{false}, hold_status{false};
    std::atomic<bool> bad_status{false}, bad_ra{false}, bad_dec{false};
    std::mutex mutex;
    std::string identity = "AM5N#";
    std::string serial_number = "012345ABCDEF#";
    std::string pending;
    std::vector<std::string> commands;
    test::FakeMountServer server{[this](const std::string& chunk) { return respond(chunk); }};
    std::unique_ptr<TelescopeDriver> driver;

    std::string respond(const std::string& chunk) {
        std::vector<std::string> frames;
        {
            std::lock_guard lock(mutex);
            pending += chunk;
            for (auto end = pending.find('#'); end != std::string::npos; end = pending.find('#')) {
                frames.push_back(pending.substr(0, end + 1));
                commands.push_back(frames.back());
                pending.erase(0, end + 1);
            }
        }
        std::string reply;
        for (const auto& command : frames) {
            if (command == ":GVP#") {
                if (hold_probe.load()) probe.block();
                std::lock_guard lock(mutex);
                reply += identity;
            } else if (command == ":GMA#") {
                std::lock_guard lock(mutex);
                reply += serial_number;
            } else if (command == ":GU#") {
                if (hold_status.load()) probe.block();
                reply += bad_status.load() ? "e1#" : "NG#";
            } else if (command == ":GR#")
                reply += bad_ra.load() ? "bad#" : "12:00:00#";
            else if (command == ":GD#")
                reply += bad_dec.load() ? "bad#" : "+45*00:00#";
            else if (command == ":GA#")
                reply += "+45*00:00#";
            else if (command == ":GZ#")
                reply += "180*00:00#";
            else if (command == ":Ggr#")
                reply += "0.5#";
            else if (command == ":GMGE#")
                reply += "+30*00:00&+100*00:00#";
            else if (command == ":GMTI#" || command == ":GL#" || command == ":GC#")
                reply += "bad#";
            else if (command == ":Gps#")
                reply += "e1#";
            else if (command.starts_with(":S"))
                reply += "1#";
            else
                reply += "0#";
        }
        return reply;
    }

    ConnectionInfo info() const {
        ConnectionInfo info;
        info.type = ConnectionType::Network;
        info.host = "127.0.0.1";
        info.tcp_port = server.port();
        info.response_timeout_ms = 500;
        return info;
    }
    void make_driver(const ConnectionInfo& info, bool sync_time = false) {
        driver = create_zwo_telescope_with_site(0, info, 30.0, -100.0, 20.0, sync_time, clock);
    }
    void set_identity(std::string reply) {
        std::lock_guard lock(mutex);
        identity = std::move(reply);
    }
    void set_serial(std::string reply) {
        std::lock_guard lock(mutex);
        serial_number = std::move(reply);
    }
    std::vector<std::string> received() {
        std::lock_guard lock(mutex);
        return commands;
    }
    int probes() {
        const auto copy = received();
        return static_cast<int>(std::count(copy.begin(), copy.end(), ":GVP#"));
    }
    void tick() {
        REQUIRE(clock.wait_for_waiters(1, 5s));
        clock.advance(250ms);
        REQUIRE(clock.wait_for_woken_settled(5s));
    }
    ~IdentityRig() {
        probe.release();
        driver.reset();
    }
};
}  // namespace

TEST_CASE("ZWO mount identity accepts only supported models on TCP and serial",
          "[zwo][telescope][identity][issue1523]") {
    IdentityRig rig;
    REQUIRE(rig.server.ok());
    const bool serial = GENERATE(false, true);
    test::PtyResponder tty([&](const std::string& command) { return rig.respond(command); });
    auto info = rig.info();
    if (serial) {
        info.type = ConnectionType::Serial;
        info.port_path = tty.slave_path();
    }
    for (const auto& model : {"AM3", "AM5", "AM5N", "AM7"}) {
        for (const bool whitespace : {false, true}) {
            CAPTURE(serial, model, whitespace);
            rig.set_identity(whitespace ? " \t\r\n" + std::string(model) + " \t\r\n#" : std::string(model) + "#");
            ZWOMountProtocolWrapper protocol;
            REQUIRE(protocol.connect(info));
            CHECK(protocol.get_mount_info() == model);
            protocol.disconnect();
            if (serial) CHECK_FALSE(util::is_serial_port_in_use(tty.slave_path()));
            auto driver = create_zwo_telescope_with_site(0, info, 30.0, -100.0, 20.0, false, rig.clock);
            driver->set_connected(true);
            CHECK(driver->get_connected());
            CHECK(driver->get_right_ascension() == 12.0);
            driver.reset();
            if (serial) CHECK_FALSE(util::is_serial_port_in_use(tty.slave_path()));
        }
    }
}

TEST_CASE("ZWO mount identity rejects malformed foreign and incomplete replies",
          "[zwo][telescope][identity][issue1523]") {
    IdentityRig rig;
    REQUIRE(rig.server.ok());
    for (const auto& reply : {"#", " \t\r\n#", "0#", "1#", "e1#", "ERROR#", "OnStep#", "iOptron#", "NOTAM5#",
                              "AM5garbage#", "AM#", "AM4#", "AM50#", "AM5 N#", "AM\r5#", "AM\n5#", "am5#", "AM5", ""}) {
        CAPTURE(reply);
        rig.set_identity(reply);
        ZWOMountProtocolWrapper protocol;
        REQUIRE(protocol.connect(rig.info()));
        CHECK_THROWS_AS(protocol.get_mount_info(), AlpacaException);
        protocol.disconnect();
    }
}

TEST_CASE("ZWO refused identity quotes the reply with control bytes replaced",
          "[zwo][telescope][identity][issue1523]") {
    IdentityRig rig;
    REQUIRE(rig.server.ok());
    rig.set_identity("OnStep\x01\x1b[31mX#");
    rig.make_driver(rig.info());
    try {
        rig.driver->set_connected(true);
        FAIL("Expected AlpacaException");
    } catch (const AlpacaException& error) {
        const std::string what = error.what();
        CHECK(what.find("'OnStep??[31mX'") != std::string::npos);
        CHECK(what.find('\x1b') == std::string::npos);
        CHECK(what.find("check the configured serial port or host") != std::string::npos);
    }
    CHECK_FALSE(rig.driver->get_connected());
}

TEST_CASE("ZWO refused identity leaves no writes and permits corrected endpoint retry",
          "[zwo][telescope][identity][issue1523]") {
    const bool serial = GENERATE(false, true);
    const bool asynchronous = GENERATE(false, true);
    IdentityRig rig;
    REQUIRE(rig.server.ok());
    test::PtyResponder tty([&](const std::string& command) { return rig.respond(command); });
    auto info = rig.info();
    if (serial) {
        info.type = ConnectionType::Serial;
        info.port_path = tty.slave_path();
    }
    auto driver = create_zwo_telescope_with_site(0, info, 30.0, -100.0, 20.0, true, rig.clock);
    rig.set_identity("OnStep#");
    if (asynchronous) {
        driver->connect();
        REQUIRE(eventually([&] { return !driver->get_connecting(); }));
        CHECK_FALSE(driver->get_last_connect_error().empty());
    } else {
        CHECK_THROWS_AS(driver->set_connected(true), AlpacaException);
    }
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_device_state().empty());
    const auto commands = rig.received();
    REQUIRE(commands.size() == 1);
    CHECK(commands.front() == ":GVP#");
    if (serial) CHECK_FALSE(util::is_serial_port_in_use(tty.slave_path()));

    rig.set_identity("AM5N#");
    if (asynchronous) {
        driver->connect();
        REQUIRE(eventually([&] { return !driver->get_connecting(); }));
        CHECK(driver->get_last_connect_error().empty());
    } else
        driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(rig.probes() == 2);
}

TEST_CASE("ZWO blocked identity keeps operational calls offline and configuration off the wire",
          "[zwo][telescope][identity][issue1523]") {
    IdentityRig rig;
    REQUIRE(rig.server.ok());
    auto info = rig.info();
    info.response_timeout_ms = 5000;
    rig.make_driver(info, true);
    rig.set_identity("OnStep#");
    rig.hold_probe.store(true);
    rig.driver->connect();
    ReleaseProbe release{rig.probe};
    REQUIRE(rig.probe.wait());
    auto calls = std::async(std::launch::async, [&] {
        std::vector<int> errors;
        errors.push_back(error_code([&] { (void)rig.driver->get_right_ascension(); }));
        errors.push_back(error_code([&] { (void)rig.driver->get_tracking(); }));
        errors.push_back(error_code([&] { rig.driver->command_blind(":Q", false); }));
        errors.push_back(error_code([&] { rig.driver->command_bool(":RAWBOOL", false); }));
        errors.push_back(error_code([&] { rig.driver->command_string(":RAWSTRING", false); }));
        rig.driver->set_site_latitude(31.0);
        rig.driver->set_site_longitude(-101.0);
        rig.driver->set_site_elevation(42.0);
        rig.driver->set_utc_date(std::chrono::system_clock::now());
        rig.driver->set_target_right_ascension(6.0);
        rig.driver->set_target_declination(20.0);
        return errors;
    });
    ReleaseProbe release_calls{rig.probe};
    const bool fast = calls.wait_for(250ms) == std::future_status::ready;
    if (!fast) rig.probe.release();
    REQUIRE(fast);
    for (const auto code : calls.get()) CHECK(code == AlpacaError::NotConnected);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.driver->get_device_state().empty());
    CHECK(rig.received() == std::vector<std::string>{":GVP#"});
    rig.probe.release();
    REQUIRE(eventually([&] { return !rig.driver->get_connecting(); }));
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.received() == std::vector<std::string>{":GVP#"});
}

TEST_CASE("ZWO disconnect racing a delayed identity cannot leave a connected telescope",
          "[zwo][telescope][identity][issue1523]") {
    const bool async_connect = GENERATE(false, true);
    const bool async_disconnect = GENERATE(false, true);
    IdentityRig rig;
    REQUIRE(rig.server.ok());
    auto info = rig.info();
    info.response_timeout_ms = 5000;
    rig.make_driver(info);
    rig.hold_probe.store(true);
    auto connect = std::async(std::launch::async, [&] {
        return error_code([&] {
            if (async_connect)
                rig.driver->connect();
            else
                rig.driver->set_connected(true);
        });
    });
    ReleaseProbe release_connect{rig.probe};
    REQUIRE(rig.probe.wait());
    std::promise<void> disconnect_started;
    auto started = disconnect_started.get_future();
    auto disconnect = std::async(std::launch::async, [&] {
        disconnect_started.set_value();
        if (async_disconnect)
            rig.driver->disconnect();
        else
            rig.driver->set_connected(false);
    });
    ReleaseProbe release_disconnect{rig.probe};
    REQUIRE(started.wait_for(1s) == std::future_status::ready);
    if (async_connect || async_disconnect) {
        REQUIRE(disconnect.wait_for(1s) == std::future_status::ready);
        disconnect.get();
    } else {
        CHECK(disconnect.wait_for(50ms) == std::future_status::timeout);
    }
    rig.probe.release();
    REQUIRE(connect.wait_for(5s) == std::future_status::ready);
    (void)connect.get();  // A cancelled handshake may fail, but it must never resurrect the link.
    if (!async_connect && !async_disconnect) {
        REQUIRE(disconnect.wait_for(5s) == std::future_status::ready);
        disconnect.get();
    }
    REQUIRE(eventually([&] { return !rig.driver->get_connecting(); }));
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.driver->get_device_state().empty());
}

TEST_CASE("ZWO lost link reprobes identity but redundant Connected true does not",
          "[zwo][telescope][identity][issue1523]") {
    IdentityRig rig;
    REQUIRE(rig.server.ok());
    rig.make_driver(rig.info());
    rig.driver->set_connected(true);
    REQUIRE(rig.driver->get_connected());
    REQUIRE(rig.probes() == 1);
    rig.set_identity("OnStep#");
    rig.driver->set_connected(true);
    CHECK(rig.driver->get_connected());
    CHECK(rig.probes() == 1);

    REQUIRE(rig.clock.wait_for_waiters(1, 5s));
    REQUIRE(rig.server.drop_connections());
    rig.tick();
    REQUIRE(eventually([&] { return !rig.driver->get_connected(); }));
    const auto before = rig.received().size();
    CHECK_THROWS_AS(rig.driver->set_connected(true), AlpacaException);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.probes() == 2);
    const auto after = rig.received();
    REQUIRE(after.size() == before + 1);
    CHECK(after.back() == ":GVP#");
}

TEST_CASE("ZWO verified identity survives unavailable initial telemetry and recovers",
          "[zwo][telescope][identity][issue1523]") {
    const int failed_read = GENERATE(0, 1, 2);
    IdentityRig rig;
    REQUIRE(rig.server.ok());
    rig.make_driver(rig.info());
    rig.bad_status.store(failed_read == 0);
    rig.bad_ra.store(failed_read == 1);
    rig.bad_dec.store(failed_read == 2);
    rig.driver->set_connected(true);
    REQUIRE(rig.driver->get_connected());
    CHECK(error_code([&] { (void)rig.driver->get_right_ascension(); }) == AlpacaError::DriverException);
    CHECK(error_code([&] { (void)rig.driver->get_declination(); }) == AlpacaError::DriverException);
    rig.bad_status.store(false);
    rig.bad_ra.store(false);
    rig.bad_dec.store(false);
    rig.tick();
    CHECK(rig.driver->get_connected());
    CHECK(rig.driver->get_right_ascension() == 12.0);
    CHECK(rig.driver->get_declination() == 45.0);
    CHECK(rig.probes() == 1);
}

TEST_CASE("ZWO transport loss during identity fails the fresh connection", "[zwo][telescope][identity][issue1523]") {
    const bool after_identity = GENERATE(false, true);
    IdentityRig rig;
    REQUIRE(rig.server.ok());
    auto info = rig.info();
    info.response_timeout_ms = 5000;
    rig.make_driver(info);
    rig.hold_probe.store(!after_identity);
    rig.hold_status.store(after_identity);
    auto connect = std::async(std::launch::async, [&] { return error_code([&] { rig.driver->set_connected(true); }); });
    ReleaseProbe release{rig.probe};
    REQUIRE(rig.probe.wait());
    // Zero wait still shuts down the socket; its blocked handler is released below.
    (void)rig.server.drop_connections(0ms);
    rig.probe.release();
    REQUIRE(connect.wait_for(5s) == std::future_status::ready);
    CHECK(connect.get() != 0);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.driver->get_device_state().empty());
    MountIdentityClaim retry;
    CHECK_NOTHROW(retry.claim("012345ABCDEF", true, 9));
    CHECK(rig.probes() == 1);
    if (!after_identity) CHECK(rig.received() == std::vector<std::string>{":GVP#"});
}

TEST_CASE("ZWO mount serial identity requires a fresh normalized twelve-hex reply",
          "[zwo][telescope][identity][issue1523]") {
    IdentityRig rig;
    REQUIRE(rig.server.ok());
    ZWOMountProtocolWrapper protocol;
    REQUIRE(protocol.connect(rig.info()));
    for (const auto* reply : {"012345ABCDEF#", "012345abcdef#", " \t012345aBcDeF \r\n#"}) {
        rig.set_serial(reply);
        CHECK(protocol.get_mount_serial_number() == "012345ABCDEF");
    }
    for (const auto* reply : {"#", "0#", "e1#", "AM5#", "012345ABCDE#", "012345ABCDEFG#", "012345ABCDEG#",
                              "01234 5ABCDE#", "012345\rABCDEF#", "012345\nABCDEF#", "012345ABCDEF", ""}) {
        CAPTURE(reply);
        rig.set_serial(reply);
        CHECK_THROWS_AS(protocol.get_mount_serial_number(), AlpacaException);
    }
}

TEST_CASE("ZWO unavailable serial identity refuses before writes and permits retry",
          "[zwo][telescope][identity][issue1523]") {
    const bool asynchronous = GENERATE(false, true);
    IdentityRig rig;
    REQUIRE(rig.server.ok());
    rig.make_driver(rig.info(), true);
    rig.set_serial("e1#");
    if (asynchronous) {
        rig.driver->connect();
        REQUIRE(eventually([&] { return !rig.driver->get_connecting(); }));
        CHECK_FALSE(rig.driver->get_last_connect_error().empty());
    } else {
        CHECK_THROWS_AS(rig.driver->set_connected(true), AlpacaException);
    }
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.driver->get_device_state().empty());
    CHECK(rig.received() == std::vector<std::string>{":GVP#", ":GMA#"});
    rig.set_serial("012345ABCDEF#");
    rig.driver->set_connected(true);
    CHECK(rig.driver->get_connected());
}

TEST_CASE("ZWO duplicate mount identity refuses before configuration and permits retry",
          "[zwo][telescope][identity][issue1523]") {
    IdentityRig rig;
    REQUIRE(rig.server.ok());
    MountIdentityClaim automatic;
    automatic.claim("012345ABCDEF", true, 7);
    rig.make_driver(rig.info(), true);
    rig.set_serial("012345abcdef#");
    CHECK_THROWS_AS(rig.driver->set_connected(true), AlpacaException);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.received() == std::vector<std::string>{":GVP#", ":GMA#"});
    automatic.reset();
    rig.driver->set_connected(true);
    CHECK(rig.driver->get_connected());
}

TEST_CASE("ZWO mount identity claims release on disconnect loss and destruction",
          "[zwo][telescope][identity][issue1523]") {
    IdentityRig rig;
    REQUIRE(rig.server.ok());
    rig.make_driver(rig.info());
    rig.driver->set_connected(true);
    MountIdentityClaim automatic;
    CHECK_THROWS_AS(automatic.claim("012345ABCDEF", true, 9), AlpacaException);
    SECTION("disconnect") { rig.driver->set_connected(false); }
    SECTION("transport loss") {
        REQUIRE(rig.server.drop_connections());
        rig.tick();
        REQUIRE(eventually([&] { return !rig.driver->get_connected(); }));
    }
    SECTION("destruction") { rig.driver.reset(); }
    CHECK_NOTHROW(automatic.claim("012345ABCDEF", true, 9));
}

TEST_CASE("ZWO identity I/O fence rejects an older admitted call on an unverified replacement link",
          "[zwo][telescope][identity][issue1523]") {
    IdentityRig rig;
    REQUIRE(rig.server.ok());
    ZWOMountProtocolWrapper protocol(true);
    REQUIRE(protocol.connect(rig.info()));
    for (int session = 0; session < 2; ++session) {
        CAPTURE(session);
        const auto before = rig.received();
        CHECK_THROWS_AS(protocol.send_command(":Sguard"), AlpacaException);
        CHECK_THROWS_AS(protocol.send_command(":GVP#:Sguard#"), AlpacaException);
        CHECK_THROWS_AS(protocol.send_command_blind(":Q"), AlpacaException);
        CHECK(rig.received() == before);
        CHECK(protocol.get_mount_info() == "AM5N");
        CHECK(protocol.get_mount_serial_number() == "012345ABCDEF");
        protocol.finish_identity_check();
        CHECK(protocol.send_command(":Sguard") == "1");

        std::promise<void> release;
        auto admitted = release.get_future();
        auto old_call = std::async(std::launch::async, [&] {
            if (admitted.wait_for(5s) != std::future_status::ready) return -1;
            return error_code([&] { protocol.send_command_blind(":Sguard"); });
        });
        protocol.disconnect();
        const bool reconnected = protocol.connect(rig.info());
        const auto before_old_call = rig.received();
        release.set_value();
        REQUIRE(reconnected);
        REQUIRE(old_call.wait_for(5s) == std::future_status::ready);
        CHECK(old_call.get() == AlpacaError::NotConnected);
        CHECK(rig.received() == before_old_call);
    }
}

#endif  // !_WIN32
