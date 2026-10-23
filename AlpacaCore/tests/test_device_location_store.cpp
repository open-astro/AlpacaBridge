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

#include <alpacacore/util/device_location_store.h>
#include <alpacacore/util/serial_port_registry.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "catch2_compat.h"
#include "device_location_test_dir.h"

namespace {

using alpacacore::util::DeviceLocationStore;
namespace fs = std::filesystem;
using alpacacore::test::TempLocationDir;

}  // namespace

TEST_CASE("Device location store - round trip", "[device-location][unit]") {
    TempLocationDir dir;
    CHECK_FALSE(DeviceLocationStore::load("A").has_value());
    REQUIRE(DeviceLocationStore::save("A", "/dev/ttyUSB3"));
    CHECK(DeviceLocationStore::load("A") == "/dev/ttyUSB3");
    REQUIRE(DeviceLocationStore::save("A", "192.0.2.10:8899"));
    CHECK(DeviceLocationStore::load("A") == "192.0.2.10:8899");
}

TEST_CASE("Device location store - other keys are kept", "[device-location][unit]") {
    TempLocationDir dir;
    REQUIRE(DeviceLocationStore::save("A", "/dev/ttyUSB0"));
    REQUIRE(DeviceLocationStore::save("B", "192.0.2.10:4030"));
    REQUIRE(DeviceLocationStore::save("A", "/dev/ttyUSB1"));
    CHECK(DeviceLocationStore::load("A") == "/dev/ttyUSB1");
    CHECK(DeviceLocationStore::load("B") == "192.0.2.10:4030");
    CHECK_FALSE(DeviceLocationStore::load("C").has_value());
}

TEST_CASE("Device location store - a value with quotes and backslashes survives", "[device-location][unit]") {
    TempLocationDir dir;
    REQUIRE(DeviceLocationStore::save("A", "/dev/serial/by-id/a\"b\\c"));
    CHECK(DeviceLocationStore::load("A") == "/dev/serial/by-id/a\"b\\c");
}

TEST_CASE("Device location store - an unchanged location is not rewritten", "[device-location][unit]") {
    TempLocationDir dir;
    REQUIRE(DeviceLocationStore::save("A", "/dev/ttyUSB0"));
    const auto first = fs::last_write_time(dir.file());
    // Make a rewrite visible: back-date the file, then save the same value.
    fs::last_write_time(dir.file(), first - std::chrono::hours(1));
    const auto backdated = fs::last_write_time(dir.file());
    REQUIRE(DeviceLocationStore::save("A", "/dev/ttyUSB0"));
    CHECK(fs::last_write_time(dir.file()) == backdated);
}

TEST_CASE("Device location store - a missing file means no remembered location", "[device-location][unit]") {
    TempLocationDir dir;
    CHECK_FALSE(fs::exists(dir.file()));
    CHECK_FALSE(DeviceLocationStore::load("A").has_value());
}

TEST_CASE("Device location store - a corrupt file means no remembered location and a save recovers",
          "[device-location][unit]") {
    TempLocationDir dir;
    fs::create_directories(dir.file().parent_path());
    for (const char* bad : {"", "not json", "{\"A\": ", "{\"A\": 3}", "[\"x\"]", "{\"A\" \"x\"}"}) {
        {
            std::ofstream(dir.file(), std::ios::trunc) << bad;
        }
        CHECK_FALSE(DeviceLocationStore::load("A").has_value());
    }
    REQUIRE(DeviceLocationStore::save("A", "/dev/ttyUSB0"));
    CHECK(DeviceLocationStore::load("A") == "/dev/ttyUSB0");
}

TEST_CASE("Device location store - a failed save returns false and does not throw", "[device-location][unit]") {
    TempLocationDir dir;
    // The state directory cannot be created: its parent is a regular file.
    std::ofstream(dir.dir() / "state") << "in the way";
    CHECK_FALSE(DeviceLocationStore::save("A", "/dev/ttyUSB0"));
    CHECK_FALSE(DeviceLocationStore::load("A").has_value());
}

TEST_CASE("Device location store - an empty key or location is refused", "[device-location][unit]") {
    TempLocationDir dir;
    CHECK_FALSE(DeviceLocationStore::save("", "/dev/ttyUSB0"));
    CHECK_FALSE(DeviceLocationStore::save("A", ""));
    CHECK_FALSE(fs::exists(dir.file()));
}

TEST_CASE("Device location store - serial_location_for prefers the by-id path", "[device-location][unit]") {
    TempLocationDir dir;
    const fs::path by_id = dir.dir() / "by-id";
    fs::create_directories(by_id);
    const fs::path raw = dir.dir() / "ttyUSB7";
    const fs::path other = dir.dir() / "ttyUSB8";
    std::ofstream(raw) << "";
    std::ofstream(other) << "";
    fs::create_symlink(raw, by_id / "usb-Example_Adapter_SN0001-if00-port0");

    using alpacacore::util::serial_location_for;
    CHECK(serial_location_for(raw.string(), by_id.string()) ==
          (by_id / "usb-Example_Adapter_SN0001-if00-port0").string());
    // A port with no by-id link, and a missing by-id directory, keep the raw node.
    CHECK(serial_location_for(other.string(), by_id.string()) == other.string());
    CHECK(serial_location_for(raw.string(), (dir.dir() / "absent").string()) == raw.string());
}

TEST_CASE("Device location store - network locations parse and format", "[device-location][unit]") {
    using alpacacore::util::network_location_for;
    using alpacacore::util::parse_network_location;
    CHECK(network_location_for("192.0.2.10", 8899) == "192.0.2.10:8899");
    std::string host;
    int port = 0;
    REQUIRE(parse_network_location("192.0.2.10:4030", host, port));
    CHECK(host == "192.0.2.10");
    CHECK(port == 4030);
    for (const char* bad : {"", "/dev/ttyUSB0", "/dev/serial/by-id/x:1", "192.0.2.10", ":8899",
                            "192.0.2.10:", "192.0.2.10:0", "192.0.2.10:70000", "192.0.2.10:80x"}) {
        INFO(bad);
        CHECK_FALSE(parse_network_location(bad, host, port));
    }
}

TEST_CASE("Device location store - recall_serial_location runs the handshake once and only on a free port",
          "[device-location][unit]") {
    using alpacacore::util::recall_serial_location;
    using alpacacore::util::SerialClaimKind;
    using alpacacore::util::SerialPortClaim;
    TempLocationDir dir;
    int calls = 0;
    auto accept = [&](const std::string&) {
        ++calls;
        return true;
    };

    CHECK_FALSE(recall_serial_location("A", accept).has_value());  // nothing remembered
    CHECK(calls == 0);

    const std::string port = (dir.dir() / "ttyUSB5").string();
    REQUIRE(DeviceLocationStore::save("A", port));
    CHECK(recall_serial_location("A", accept) == port);
    CHECK(calls == 1);

    auto reject = [&](const std::string&) {
        ++calls;
        return false;
    };
    CHECK_FALSE(recall_serial_location("A", reject).has_value());
    CHECK(calls == 2);

    // A connected driver holds the port: it is never probed.
    auto held = SerialPortClaim::acquire(port, SerialClaimKind::Link, "Other device");
    REQUIRE(held);
    CHECK_FALSE(recall_serial_location("A", accept).has_value());
    CHECK(calls == 2);

    // A network location is not a serial path.
    REQUIRE(DeviceLocationStore::save("N", "192.0.2.10:8899"));
    CHECK_FALSE(recall_serial_location("N", accept).has_value());
    CHECK(calls == 2);
}

TEST_CASE("Device location store - location_key separates the enumeration indexes of one id",
          "[device-location][issue1452]") {
    using alpacacore::util::location_key;
    CHECK(location_key("A", 0) == "A#0");
    CHECK(location_key("A", 0) != location_key("A", 1));
}
