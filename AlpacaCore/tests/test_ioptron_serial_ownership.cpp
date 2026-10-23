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

// Serial-port ownership for the iOptron serial wrappers (issue #1202): the
// mount (serial path), the iEAF / iAFS focuser and the iEFW filter wheel open
// their ports through util::SerialLink. A port another connected device holds
// is refused with its name, a connected wrapper holds its port until it
// disconnects, a port retuned to a blocking read cannot hang a command or
// Disconnect, and the three auto-detect probes (all matching the same
// Prolific chip class) wait for each other instead of sharing a port.

#include <alpacacore/util/device_location_store.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/vendor/ioptron/ioptron_filterwheel_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_ieaf_focuser_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_ieaf_protocol_wrapper.h>
#include <alpacacore/vendor/ioptron/ioptron_iefw_protocol_wrapper.h>
#include <alpacacore/vendor/ioptron/ioptron_protocol_wrapper.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "device_location_test_dir.h"
#include "fake_ioptron_ieaf.h"
#include "fake_pty_write.h"

namespace {

namespace ioptron = alpacacore::vendor::ioptron;
using alpacacore::util::is_serial_port_in_use;
using alpacacore::util::SerialClaimKind;
using alpacacore::util::SerialPortClaim;

// One iOptron serial device behind a pty: answers :MountInfo# as a mount when
// given a model code, :DeviceInfo# as an iEFW / iEAF when given a model code,
// and the iEFW's :WP# / :FW1# / :WF / :WM. A mount answers any other query
// with "0#"; a wheel or focuser stays silent. set_silent() stops every reply,
// which is what leaves a reader waiting on a retuned port.
class FakeIoptronSerial {
public:
    FakeIoptronSerial(std::string mount_code, int device_model)
        : pty_("FakeIoptronSerial"), mount_code_(std::move(mount_code)), device_model_(device_model) {
        th_ = std::thread([this] { run(); });
    }
    ~FakeIoptronSerial() {
        stop_.store(true);
        th_.join();
    }
    FakeIoptronSerial(const FakeIoptronSerial&) = delete;
    FakeIoptronSerial& operator=(const FakeIoptronSerial&) = delete;

    std::string slave_path() const { return pty_.slave_path(); }
    int master_fd() const { return pty_.master_fd(); }
    int keepalive_fd() const { return pty_.keepalive_fd(); }
    void set_silent(bool silent) { silent_.store(silent); }

private:
    void run() {
        std::string pending;
        char buf[64];
        while (!stop_.load()) {
            struct pollfd pfd {};
            pfd.fd = pty_.master_fd();
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, 20) <= 0) continue;
            const ssize_t n = ::read(pty_.master_fd(), buf, sizeof(buf));
            for (ssize_t i = 0; i < n; ++i) {
                if (pending.empty() && buf[i] != ':') continue;
                pending += buf[i];
                if (buf[i] == '#') {
                    handle(pending);
                    pending.clear();
                }
            }
        }
    }

    void handle(const std::string& cmd) {
        if (silent_.load()) return;
        std::string reply;
        char b[40] = {};
        if (cmd == ":MountInfo#") {
            reply = mount_code_;  // 4 digits, no '#', as the firmware sends
        } else if (cmd == ":DeviceInfo#" && device_model_ != 0) {
            std::snprintf(b, sizeof(b), "%+06d%02d%04d#", 0, device_model_, 100);
            reply = b;
        } else if (device_model_ != 0 && cmd == ":WP#") {
            reply = "00#";
        } else if (device_model_ != 0 && cmd == ":FW1#") {
            reply = "210101210101#";
        } else if (device_model_ != 0 && cmd.rfind(":WF", 0) == 0) {
            reply = "+00000#";
        } else if (device_model_ != 0 && cmd.rfind(":WM", 0) == 0) {
            reply = "1";
        } else if (!mount_code_.empty()) {
            reply = "0#";
        }
        if (!reply.empty()) alpacacore::test::pty_write_bounded(pty_.master_fd(), reply, stop_);
    }

    alpacacore::test::PtyPair pty_;
    std::string mount_code_;
    int device_model_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> silent_{false};
};

speed_t line_speed(int fd) {
    struct termios tty {};
    REQUIRE(tcgetattr(fd, &tty) == 0);
    return cfgetospeed(&tty);
}

void set_line_speed(int fd, speed_t speed) {
    struct termios tty {};
    REQUIRE(tcgetattr(fd, &tty) == 0);
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);
    REQUIRE(tcsetattr(fd, TCSANOW, &tty) == 0);
}

// What the #1202 GPS scanner did to a live port: another baud, and a read
// that blocks until a byte arrives.
void retune_to_blocking_read(int fd) {
    struct termios tty {};
    REQUIRE(tcgetattr(fd, &tty) == 0);
    cfsetispeed(&tty, B9600);
    cfsetospeed(&tty, B9600);
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;
    REQUIRE(tcsetattr(fd, TCSANOW, &tty) == 0);
}

// The exception text of fn(), or "" when it did not throw.
std::string thrown_message(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

// Runs fn on a thread and reports whether it finished within budget. On a
// miss, feeds the pty bytes until it does, so a parked read() (the bug this
// file guards against) cannot hang the test binary.
bool finishes_within(const std::function<void()>& fn, std::chrono::milliseconds budget, int master_fd) {
    std::promise<void> done;
    auto finished = done.get_future();
    std::thread worker([&] {
        try {
            fn();
        } catch (...) {
        }
        done.set_value();
    });
    const bool on_time = finished.wait_for(budget) == std::future_status::ready;
    while (finished.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
        const char release[] = "0#";
        (void)!::write(master_fd, release, 2);
    }
    worker.join();
    return on_time;
}

ioptron::ConnectionInfo mount_serial(const std::string& path) {
    ioptron::ConnectionInfo info;
    info.type = ioptron::ConnectionType::Serial;
    info.port_path = path;
    info.baud_rate = 115200;
    return info;
}

}  // namespace

// --- Mount (serial path of the shared protocol wrapper) ---

TEST_CASE("iOptron mount serial - a port another connected device holds is refused with its name",
          "[ioptron][serial][issue1202]") {
    alpacacore::test::PtyPair pty("ioptron-mount-refuse");
    set_line_speed(pty.keepalive_fd(), B9600);
    auto other = SerialPortClaim::acquire(pty.slave_path(), SerialClaimKind::Link, "Other device");
    REQUIRE(other);

    auto driver = ioptron::create_ioptron_telescope(0, mount_serial(pty.slave_path()));
    const std::string error = thrown_message([&] { driver->set_connected(true); });
    INFO(error);
    CHECK(error.find("Other device") != std::string::npos);
    CHECK_FALSE(driver->get_connected());
    CHECK(line_speed(pty.keepalive_fd()) == B9600);  // never opened, never retuned
}

TEST_CASE("iOptron mount serial - holds its port while connected and releases it on disconnect",
          "[ioptron][serial][issue1202]") {
    alpacacore::test::PtyPair pty("ioptron-mount-hold");
    auto& wrapper = ioptron::iOptronProtocolWrapper::instance();
    REQUIRE(wrapper.connect(mount_serial(pty.slave_path())));
    CHECK(is_serial_port_in_use(pty.slave_path()));
    wrapper.disconnect();
    CHECK_FALSE(is_serial_port_in_use(pty.slave_path()));
    // TIOCNXCL went with the close: the port opens again.
    CHECK(wrapper.connect(mount_serial(pty.slave_path())));
    wrapper.disconnect();
}

TEST_CASE("iOptron mount serial - a command on a port retuned to VMIN=1 times out and the settings come back",
          "[ioptron][serial][issue1202]") {
    alpacacore::test::PtyPair pty("ioptron-mount-retune");  // a mount that never answers
    auto& wrapper = ioptron::iOptronProtocolWrapper::instance();
    REQUIRE(wrapper.connect(mount_serial(pty.slave_path())));
    retune_to_blocking_read(pty.keepalive_fd());

    std::string error;
    CHECK(finishes_within([&] { error = thrown_message([&] { (void)wrapper.send_command(":GLS", true, 500); }); },
                          std::chrono::seconds(3), pty.master_fd()));
    CHECK_FALSE(error.empty());
    CHECK(line_speed(pty.keepalive_fd()) == B115200);  // the timed-out read put the line back
    CHECK(finishes_within([&] { wrapper.disconnect(); }, std::chrono::seconds(3), pty.master_fd()));
    CHECK_FALSE(is_serial_port_in_use(pty.slave_path()));
}

// --- iEAF / iAFS focuser ---

TEST_CASE("iOptron iEAF serial - a port another connected device holds is refused with its name",
          "[ioptron][serial][issue1202]") {
    alpacacore::test::PtyPair pty("ioptron-ieaf-refuse");
    set_line_speed(pty.keepalive_fd(), B9600);
    auto other = SerialPortClaim::acquire(pty.slave_path(), SerialClaimKind::Link, "Other device");
    REQUIRE(other);

    auto driver = ioptron::create_ieaf_focuser(0, pty.slave_path());
    const std::string error = thrown_message([&] { driver->set_connected(true); });
    INFO(error);
    CHECK(error.find("Other device") != std::string::npos);
    CHECK_FALSE(driver->get_connected());
    CHECK(line_speed(pty.keepalive_fd()) == B9600);
}

TEST_CASE("iOptron iEAF serial - holds its port while connected and releases it on disconnect",
          "[ioptron][serial][issue1202]") {
    alpacacore::test::FakeIoptronIeaf fake;
    auto driver = ioptron::create_ieaf_focuser(0, fake.slave_path());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(is_serial_port_in_use(fake.slave_path()));
    driver->set_connected(false);
    CHECK_FALSE(is_serial_port_in_use(fake.slave_path()));
    driver->set_connected(true);
    CHECK(driver->get_connected());
    driver->set_connected(false);
}

TEST_CASE("iOptron iEAF serial - a status read on a port retuned to VMIN=1 times out and the settings come back",
          "[ioptron][serial][issue1202]") {
    FakeIoptronSerial fake("", 2);
    ioptron::IeafProtocolWrapper wrapper;
    ioptron::IeafConnectionConfig cfg;
    cfg.serial_port = fake.slave_path();
    cfg.serial_timeout_s = 1;
    REQUIRE(wrapper.connect(cfg).model == 2);
    fake.set_silent(true);
    retune_to_blocking_read(fake.keepalive_fd());

    std::string error;
    CHECK(finishes_within([&] { error = thrown_message([&] { (void)wrapper.get_status(); }); }, std::chrono::seconds(4),
                          fake.master_fd()));
    CHECK_FALSE(error.empty());
    CHECK(line_speed(fake.keepalive_fd()) == B115200);
    CHECK(finishes_within([&] { wrapper.disconnect(); }, std::chrono::seconds(3), fake.master_fd()));
    CHECK_FALSE(is_serial_port_in_use(fake.slave_path()));
}

// --- iEFW filter wheel ---

TEST_CASE("iOptron iEFW serial - a port another connected device holds is refused with its name",
          "[ioptron][serial][issue1202]") {
    alpacacore::test::PtyPair pty("ioptron-iefw-refuse");
    set_line_speed(pty.keepalive_fd(), B9600);
    auto other = SerialPortClaim::acquire(pty.slave_path(), SerialClaimKind::Link, "Other device");
    REQUIRE(other);

    auto driver = ioptron::create_iefw_filterwheel(0, pty.slave_path());
    const std::string error = thrown_message([&] { driver->set_connected(true); });
    INFO(error);
    CHECK(error.find("Other device") != std::string::npos);
    CHECK_FALSE(driver->get_connected());
    CHECK(line_speed(pty.keepalive_fd()) == B9600);
}

TEST_CASE("iOptron iEFW serial - holds its port while connected and releases it on disconnect",
          "[ioptron][serial][issue1202]") {
    FakeIoptronSerial fake("", 99);
    auto driver = ioptron::create_iefw_filterwheel(0, fake.slave_path());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(is_serial_port_in_use(fake.slave_path()));
    driver->set_connected(false);
    CHECK_FALSE(is_serial_port_in_use(fake.slave_path()));
    driver->set_connected(true);
    CHECK(driver->get_connected());
    driver->set_connected(false);
}

TEST_CASE("iOptron iEFW serial - a position read on a port retuned to VMIN=1 times out and the settings come back",
          "[ioptron][serial][issue1202]") {
    FakeIoptronSerial fake("", 99);
    ioptron::IefwProtocolWrapper wrapper;
    ioptron::IefwConnectionConfig cfg;
    cfg.serial_port = fake.slave_path();
    cfg.serial_timeout_s = 1;
    REQUIRE(wrapper.connect(cfg).model == 99);
    fake.set_silent(true);
    retune_to_blocking_read(fake.keepalive_fd());

    std::string error;
    CHECK(finishes_within([&] { error = thrown_message([&] { (void)wrapper.get_position(); }); },
                          std::chrono::seconds(4), fake.master_fd()));
    CHECK_FALSE(error.empty());
    CHECK(line_speed(fake.keepalive_fd()) == B115200);
    CHECK(finishes_within([&] { wrapper.disconnect(); }, std::chrono::seconds(3), fake.master_fd()));
    CHECK_FALSE(is_serial_port_in_use(fake.slave_path()));
}

// --- The three probes share the Prolific chip class ---

TEST_CASE("iOptron serial probes - a mount scan and an iEFW scan probing the same ports at once each find their own",
          "[ioptron][serial][issue1202]") {
    FakeIoptronSerial mount("0036", 0);
    FakeIoptronSerial wheel("", 99);

    // The mount probe takes the wheel's port first and waits ~4 s for a
    // :MountInfo# reply that never comes; the iEFW probe of that port starts
    // while it does and must wait its turn, not skip the port.
    using clock = std::chrono::steady_clock;
    clock::time_point mount_on_wheel_done;
    clock::time_point wheel_on_wheel_done;
    std::string mount_on_wheel = "unset";
    auto a = std::async(std::launch::async, [&] {
        mount_on_wheel = ioptron::probe_ioptron_serial_port(wheel.slave_path());
        mount_on_wheel_done = clock::now();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(is_serial_port_in_use(wheel.slave_path()));

    ioptron::IefwDeviceInfo wheel_info;
    bool wheel_found = false;
    auto b = std::async(std::launch::async, [&] {
        wheel_found = ioptron::probe_iefw_port(wheel.slave_path(), wheel_info);
        wheel_on_wheel_done = clock::now();
    });
    std::string mount_on_mount;
    auto c = std::async(std::launch::async,
                        [&] { mount_on_mount = ioptron::probe_ioptron_serial_port(mount.slave_path()); });
    ioptron::IefwDeviceInfo stray;
    bool wheel_on_mount = true;
    auto d =
        std::async(std::launch::async, [&] { wheel_on_mount = ioptron::probe_iefw_port(mount.slave_path(), stray); });
    a.get();
    b.get();
    c.get();
    d.get();

    CHECK(mount_on_wheel.empty());
    CHECK(wheel_found);
    CHECK(wheel_info.model == 99);
    CHECK(wheel_on_wheel_done >= mount_on_wheel_done);  // it waited for the mount probe
    CHECK(mount_on_mount == "0036");
    CHECK_FALSE(wheel_on_mount);
    CHECK_FALSE(is_serial_port_in_use(wheel.slave_path()));
    CHECK_FALSE(is_serial_port_in_use(mount.slave_path()));
}

// --- Remembered port (issue #1452): the auto path tries the port that worked before it scans ---

namespace {

struct SerialScanCounts {
    std::atomic<int> probes{0};
    std::atomic<int> scans{0};
};

// Stubs the serial scan to find @p port and counts probes and scans; clears the stubs on exit.
class SerialScanStub {
public:
    SerialScanStub(SerialScanCounts& counts, std::string port, std::string second = {}) {
        ioptron::iOptronScanHooks hooks;
        hooks.on_probe = [&counts](const std::string&) { ++counts.probes; };
        hooks.enumerate_ports = [&counts, port = std::move(port), second = std::move(second)] {
            ++counts.scans;
            std::vector<ioptron::iOptronPortInfo> found{{port, "", "0036"}};
            if (!second.empty()) found.push_back({second, "", "0036"});
            return found;
        };
        ioptron::set_ioptron_scan_hooks_for_testing(std::move(hooks));
    }
    ~SerialScanStub() { ioptron::set_ioptron_scan_hooks_for_testing({}); }
    SerialScanStub(const SerialScanStub&) = delete;
    SerialScanStub& operator=(const SerialScanStub&) = delete;
};

const std::string kMountId = alpacacore::util::location_key("3a9d6f10-82c4-4b57-b0e3-61f7c5a2d948", 0);

}  // namespace

TEST_CASE("iOptron serial auto-detect - a remembered port that answers costs one probe and no scan",
          "[ioptron][serial][location][issue1452]") {
    alpacacore::test::TempLocationDir store;
    FakeIoptronSerial mount("0036", 0);
    SerialScanCounts counts;
    SerialScanStub stub(counts, "/dev/should-not-be-scanned");
    REQUIRE(alpacacore::util::DeviceLocationStore::save(kMountId, mount.slave_path()));

    const auto info = ioptron::resolve_ioptron_serial_auto(0, kMountId);
    CHECK(info.port_path == mount.slave_path());
    CHECK(info.baud_rate == 115200);
    CHECK(counts.probes == 1);
    CHECK(counts.scans == 0);
}

TEST_CASE("iOptron serial auto-detect - a remembered port that fails the handshake falls back to the scan",
          "[ioptron][serial][location][issue1452]") {
    alpacacore::test::TempLocationDir store;
    FakeIoptronSerial wheel("", 99);  // a wheel now sits on the remembered port: it never answers :MountInfo#
    FakeIoptronSerial mount("0036", 0);
    SerialScanCounts counts;
    SerialScanStub stub(counts, mount.slave_path());
    REQUIRE(alpacacore::util::DeviceLocationStore::save(kMountId, wheel.slave_path()));

    const auto info = ioptron::resolve_ioptron_serial_auto(0, kMountId);
    CHECK(info.port_path == mount.slave_path());
    CHECK(counts.probes == 1);
    CHECK(counts.scans == 1);
    CHECK_FALSE(is_serial_port_in_use(wheel.slave_path()));
}

TEST_CASE("iOptron serial auto-detect - a remembered port another connected driver holds is never probed",
          "[ioptron][serial][location][issue1452]") {
    alpacacore::test::TempLocationDir store;
    FakeIoptronSerial held("0036", 0);
    FakeIoptronSerial mount("0036", 0);
    SerialScanCounts counts;
    SerialScanStub stub(counts, mount.slave_path());
    auto other = SerialPortClaim::acquire(held.slave_path(), SerialClaimKind::Link, "Other device");
    REQUIRE(other);
    REQUIRE(alpacacore::util::DeviceLocationStore::save(kMountId, held.slave_path()));

    const auto info = ioptron::resolve_ioptron_serial_auto(0, kMountId);
    CHECK(info.port_path == mount.slave_path());
    CHECK(counts.probes == 0);
    CHECK(counts.scans == 1);
}

TEST_CASE("iOptron serial auto-detect - without a location key the scan runs and the store is not read",
          "[ioptron][serial][location][issue1452]") {
    alpacacore::test::TempLocationDir store;
    FakeIoptronSerial mount("0036", 0);
    SerialScanCounts counts;
    SerialScanStub stub(counts, mount.slave_path());
    REQUIRE(alpacacore::util::DeviceLocationStore::save(kMountId, "/dev/nonexistent-remembered-port"));

    const auto info = ioptron::resolve_ioptron_serial_auto(0);
    CHECK(info.port_path == mount.slave_path());
    CHECK(counts.probes == 0);
    CHECK(counts.scans == 1);
}

TEST_CASE("iOptron serial auto-detect - a changed mount index ignores the port remembered for the old index",
          "[ioptron][serial][location][issue1452]") {
    alpacacore::test::TempLocationDir store;
    FakeIoptronSerial old_mount("0036", 0);
    FakeIoptronSerial mount("0036", 0);
    SerialScanCounts counts;
    SerialScanStub stub(counts, old_mount.slave_path(), mount.slave_path());
    REQUIRE(alpacacore::util::DeviceLocationStore::save(kMountId, old_mount.slave_path()));

    // Through the factory, so the key the driver builds from its mount index is what is tested.
    auto driver = ioptron::create_ioptron_telescope_auto(0, 1, -33.856784, 151.215297, 42.0);
    driver->assign_unique_id("3a9d6f10-82c4-4b57-b0e3-61f7c5a2d948");
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(counts.probes == 0);
    CHECK(counts.scans == 1);
    CHECK(alpacacore::util::DeviceLocationStore::load(
              alpacacore::util::location_key("3a9d6f10-82c4-4b57-b0e3-61f7c5a2d948", 1)) == mount.slave_path());
    driver->set_connected(false);

    // A fresh driver at index 1 now finds its own remembered port.
    auto again = ioptron::create_ioptron_telescope_auto(0, 1, -33.856784, 151.215297, 42.0);
    again->assign_unique_id("3a9d6f10-82c4-4b57-b0e3-61f7c5a2d948");
    again->set_connected(true);
    REQUIRE(again->get_connected());
    CHECK(counts.probes == 1);
    CHECK(counts.scans == 1);
    again->set_connected(false);
}
