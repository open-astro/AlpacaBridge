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

// Remembered device location for the iOptron mount (issue #1452): an
// auto-detected mount is remembered once it answered, and the next connect (a
// fresh driver, as after a service restart) tries that address with one
// handshake before the full sweep. A configured address is used as given and
// never stored.

#ifndef _WIN32

#include <alpacacore/util/device_location_store.h>
#include <alpacacore/vendor/ioptron/ioptron_protocol_wrapper.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "catch2_compat.h"
#include "device_location_test_dir.h"
#include "fake_ioptron_mount.h"

namespace {

namespace ioptron = alpacacore::vendor::ioptron;
using alpacacore::util::DeviceLocationStore;

constexpr const char* kUnitId = "7e41c0b2-5d3a-4f68-a1b9-0c2d8e6f3a47";
const std::string kKey = alpacacore::util::location_key(kUnitId, 0);

struct Counts {
    std::atomic<int> probes{0};
    std::atomic<int> sweeps{0};
};

// Stubs the sweep to find @p port on loopback and counts probes and sweeps; clears them on exit.
class ScanStub {
public:
    ScanStub(Counts& counts, int port, int second_port = 0) {
        ioptron::iOptronScanHooks hooks;
        hooks.on_probe = [&counts](const std::string&) { ++counts.probes; };
        hooks.enumerate_hosts = [&counts, port, second_port] {
            ++counts.sweeps;
            std::vector<ioptron::iOptronNetworkHostInfo> found{{"127.0.0.1", port, "0012"}};
            if (second_port != 0) found.push_back({"127.0.0.1", second_port, "0012"});
            return found;
        };
        ioptron::set_ioptron_scan_hooks_for_testing(std::move(hooks));
    }
    ~ScanStub() { ioptron::set_ioptron_scan_hooks_for_testing({}); }
    ScanStub(const ScanStub&) = delete;
    ScanStub& operator=(const ScanStub&) = delete;
};

std::unique_ptr<alpacacore::TelescopeDriver> make_auto(const std::string& id, int index = 0) {
    auto driver = ioptron::create_ioptron_telescope_auto_network(0, index, -33.856784, 151.215297, 42.0);
    driver->assign_unique_id(id);
    return driver;
}

}  // namespace

TEST_CASE("iOptron network auto-detect - a fresh driver probes the remembered address once and skips the sweep",
          "[ioptron][telescope][location][issue1452]") {
    alpacacore::test::TempLocationDir store;
    alpacacore::test::FakeIoptronMount fake("0012", 0.0);
    REQUIRE(fake.ok());
    Counts counts;
    ScanStub stub(counts, fake.port());
    const std::string where = "127.0.0.1:" + std::to_string(fake.port());

    {
        auto first = make_auto(kUnitId);
        first->set_connected(true);
        REQUIRE(first->get_connected());
        CHECK(counts.sweeps == 1);
        CHECK(counts.probes == 0);  // the stub sweep stands in for the probes of a real sweep
        CHECK(DeviceLocationStore::load(kKey) == where);
        first->set_connected(false);
    }

    auto second = make_auto(kUnitId);
    second->set_connected(true);
    REQUIRE(second->get_connected());
    CHECK(counts.probes == 1);
    CHECK(counts.sweeps == 1);  // no second sweep
    CHECK(DeviceLocationStore::load(kKey) == where);
    second->set_connected(false);
}

TEST_CASE("iOptron network auto-detect - a remembered address that no longer answers falls back to the sweep",
          "[ioptron][telescope][location][issue1452]") {
    alpacacore::test::TempLocationDir store;
    alpacacore::test::FakeIoptronMount fake("0012", 0.0);
    REQUIRE(fake.ok());
    Counts counts;
    ScanStub stub(counts, fake.port());
    REQUIRE(DeviceLocationStore::save(kKey, "127.0.0.1:1"));  // nothing listens on port 1

    auto driver = make_auto(kUnitId);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(counts.probes == 1);
    CHECK(counts.sweeps == 1);
    CHECK(DeviceLocationStore::load(kKey) == "127.0.0.1:" + std::to_string(fake.port()));
    driver->set_connected(false);
}

TEST_CASE("iOptron network auto-detect - a location the store holds for a serial mount is not probed",
          "[ioptron][telescope][location][issue1452]") {
    alpacacore::test::TempLocationDir store;
    alpacacore::test::FakeIoptronMount fake("0012", 0.0);
    REQUIRE(fake.ok());
    Counts counts;
    ScanStub stub(counts, fake.port());
    REQUIRE(DeviceLocationStore::save(kKey, "/dev/serial/by-id/usb-Example_Adapter-if00-port0"));

    auto driver = make_auto(kUnitId);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(counts.probes == 0);
    CHECK(counts.sweeps == 1);
    CHECK(DeviceLocationStore::load(kKey) == "127.0.0.1:" + std::to_string(fake.port()));
    driver->set_connected(false);
}

TEST_CASE("iOptron network auto-detect - a changed mount index ignores the address remembered for the old index",
          "[ioptron][telescope][location][issue1452]") {
    alpacacore::test::TempLocationDir store;
    alpacacore::test::FakeIoptronMount first("0012", 0.0);
    alpacacore::test::FakeIoptronMount second("0012", 0.0);
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    Counts counts;
    ScanStub stub(counts, first.port(), second.port());
    REQUIRE(DeviceLocationStore::save(kKey, "127.0.0.1:" + std::to_string(first.port())));

    auto driver = make_auto(kUnitId, 1);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(counts.probes == 0);
    CHECK(counts.sweeps == 1);
    CHECK(DeviceLocationStore::load(alpacacore::util::location_key(kUnitId, 1)) ==
          "127.0.0.1:" + std::to_string(second.port()));
    driver->set_connected(false);
}

TEST_CASE("iOptron - a configured address never reads or writes the location store",
          "[ioptron][telescope][location][issue1452]") {
    alpacacore::test::TempLocationDir store;
    alpacacore::test::FakeIoptronMount fake("0012", 0.0);
    REQUIRE(fake.ok());
    Counts counts;
    ScanStub stub(counts, 1);
    REQUIRE(DeviceLocationStore::save(kKey, "127.0.0.1:1"));

    ioptron::ConnectionInfo info;
    info.type = ioptron::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = fake.port();
    auto driver = ioptron::create_ioptron_telescope_with_site(0, info, -33.856784, 151.215297, 42.0, std::nullopt);
    driver->assign_unique_id(kUnitId);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(counts.probes == 0);
    CHECK(counts.sweeps == 0);
    CHECK(DeviceLocationStore::load(kKey) == "127.0.0.1:1");  // unchanged
    driver->set_connected(false);
}

#endif  // _WIN32
