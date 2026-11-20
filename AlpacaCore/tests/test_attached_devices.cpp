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

// The attacheddevices listing (#2312) over a fake by-id/sysfs tree and stub
// descriptors. Fake-only: no hardware, no vendor SDK, no real tty.

#include <alpacacore/catalog/attached_devices.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/serial_port_registry.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "catch2_compat.h"
#include "fake_usb_serial_tree.h"

// Designated initializers of Field<T> leave most members defaulted on purpose.
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

using namespace alpacacore;
using namespace alpacacore::catalog;

namespace {

const Field<std::string> kConnectionType{.key = "connectionType", .default_value = "auto", .role = Role::Discriminator};
const Field<std::string> kSerialPort{.key = "portPath",
                                     .default_value = "",
                                     .role = Role::PortPath,
                                     .applies_when = AppliesWhen{"connectionType", "serial"}};
const Field<std::string> kWheelType{.key = "wheelType", .default_value = "integrated", .role = Role::Discriminator};
const Field<std::string> kHidPath{.key = "hidPath", .default_value = "", .role = Role::PortPath};
const Field<std::int64_t> kCameraIndex{.key = "cameraIndex", .default_value = 0, .role = Role::EnumerationIndex};
const Field<std::string> kSerialNumber{.key = "serialNumber", .default_value = ""};

const std::vector<FieldRef>& serial_fields() {
    static const std::vector<FieldRef> f{kConnectionType.ref(), kSerialPort.ref()};
    return f;
}
const std::vector<FieldRef>& wheel_fields() {
    static const std::vector<FieldRef> f{kWheelType.ref(), kConnectionType.ref(), kSerialPort.ref()};
    return f;
}
const std::vector<FieldRef>& hid_fields() {
    static const std::vector<FieldRef> f{kHidPath.ref()};
    return f;
}
const std::vector<FieldRef>& camera_fields() {
    static const std::vector<FieldRef> f{kCameraIndex.ref(), kSerialNumber.ref()};
    return f;
}

const DeviceKey kFocuser{"alpha", DeviceType::Focuser};
const DeviceKey kMount{"beta", DeviceType::Telescope};
const DeviceKey kWheel{"gamma", DeviceType::FilterWheel};
const DeviceKey kHid{"delta", DeviceType::Focuser};
const DeviceKey kCamera{"epsilon", DeviceType::Camera};

void add_schema(DeviceCatalog& catalog, const DeviceKey& key, const std::vector<FieldRef>& fields) {
    Schema schema;
    schema.key = key;
    schema.display_name = "Stub";
    schema.build_option = "ALPACACORE_ENABLE_STUB";
    schema.fields = fields;
    catalog.add(std::move(schema));
}

// Two CH340 entries (as Gemini and OnStep both list one) and a CP210x wheel.
DeviceCatalog make_catalog() {
    DeviceCatalog catalog;
    add_schema(catalog, kFocuser, serial_fields());
    add_schema(catalog, kMount, serial_fields());
    add_schema(catalog, kWheel, wheel_fields());
    add_schema(catalog, kHid, hid_fields());
    add_schema(catalog, kCamera, camera_fields());
    catalog.add_usb_serial_identity({kFocuser, {"1a86", "CH340", "CH341", "USB_Serial"}, {}});
    catalog.add_usb_serial_identity({kMount, {"Prolific", "1a86", "CH340"}, {}});
    catalog.add_usb_serial_identity({kWheel, {"10c4", "Silicon Labs", "CP210"}, {{"wheelType", "cfw3-usb"}}});
    return catalog;
}

std::string canonical(const std::string& path) { return std::filesystem::canonical(path).string(); }

const AttachedDevice* find_port(const AttachedDeviceList& list, const std::string& port) {
    for (const auto& device : list.devices) {
        if (device.port == port) return &device;
    }
    return nullptr;
}

std::string config_string(const DeviceConfig& config, const char* key) {
    const ConfigValue* value = config.find_value(key);
    const auto* text = value != nullptr ? std::get_if<std::string>(value) : nullptr;
    return text != nullptr ? *text : std::string("<unset>");
}

}  // namespace

TEST_CASE("Attached devices: a CH340 port is listed once with every catalog entry it could be", "[attached_devices]") {
    test::FakeUsbSerialTree tree;
    const auto ch340 = tree.add_port("ttyUSB0", "1a86", "", "USB Serial", "usb-1a86_USB_Serial-if00-port0");
    // A second CH340 whose by-id name collided: only its raw node and sysfs say what it is.
    const auto second = tree.add_port("ttyUSB1", "1a86", "", "USB Serial");
    tree.add_port("ttyUSB2", "0403", "FTDI", "FT232R USB UART");  // matches no entry: not listed
    const auto cp210x = tree.add_port("ttyACM0", "10c4", "Silicon Labs", "CP2102 USB to UART Bridge Controller");
    const DeviceCatalog catalog = make_catalog();

    const auto list = list_attached_devices(catalog, {}, tree.roots());

    REQUIRE(list.errors.empty());
    REQUIRE(list.devices.size() == 3);
    const std::string by_id = (tree.by_id() / "usb-1a86_USB_Serial-if00-port0").string();
    const AttachedDevice* first = find_port(list, by_id);
    REQUIRE(first != nullptr);
    CHECK(first->kind == AttachedKind::Serial);
    CHECK(first->node == canonical(ch340));
    CHECK(first->name == "usb-1a86_USB_Serial-if00-port0");
    REQUIRE(first->candidates.size() == 2);
    CHECK(first->candidates[0].key == kFocuser);
    CHECK(first->candidates[1].key == kMount);
    for (const auto& candidate : first->candidates) {
        CHECK(config_string(candidate.config, "connectionType") == "serial");
        CHECK(config_string(candidate.config, "portPath") == by_id);
    }
    CHECK(first->configured_as.empty());
    // The by-id port is not listed a second time under its raw node.
    CHECK(find_port(list, canonical(ch340)) == nullptr);

    const AttachedDevice* raw = find_port(list, canonical(second));
    REQUIRE(raw != nullptr);
    CHECK(raw->name == "USB Serial");
    CHECK(raw->candidates.size() == 2);

    const AttachedDevice* wheel = find_port(list, canonical(cp210x));
    REQUIRE(wheel != nullptr);
    REQUIRE(wheel->candidates.size() == 1);
    CHECK(wheel->candidates[0].key == kWheel);
    CHECK(config_string(wheel->candidates[0].config, "wheelType") == "cfw3-usb");
    CHECK(config_string(wheel->candidates[0].config, "connectionType") == "serial");
    CHECK(config_string(wheel->candidates[0].config, "portPath") == canonical(cp210x));
}

TEST_CASE("Attached devices: listing opens no port and takes no serial claim", "[attached_devices]") {
    test::FakeUsbSerialTree tree;
    const auto held = tree.add_port("ttyUSB0", "1a86", "", "USB Serial", "usb-1a86_USB_Serial-if00-port0");
    const auto probed = tree.add_port("ttyACM0", "10c4", "Silicon Labs", "CP2102");
    const auto free_port = tree.add_port("ttyUSB1", "1a86", "", "USB Serial");
    const DeviceCatalog catalog = make_catalog();

    // A connected device holds one port and another scan's probe holds a second:
    // a listing that claimed either would be refused or wait out the probe
    // (kSerialProbeClaimWait, 20 s).
    auto link = util::SerialPortClaim::acquire(held, util::SerialClaimKind::Link, "Gemini PDH");
    auto probe = util::SerialPortClaim::acquire(probed, util::SerialClaimKind::Probe, "QHY CFW3");
    REQUIRE(link.held());
    REQUIRE(probe.held());

    // Every open of a file in the fake /dev is an IN_OPEN event; listing the
    // directories is not (IN_ISDIR).
    const int watch_fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    REQUIRE(watch_fd >= 0);
    REQUIRE(::inotify_add_watch(watch_fd, tree.dev().c_str(), IN_OPEN) >= 0);

    const auto started = std::chrono::steady_clock::now();
    const auto list = list_attached_devices(catalog, {}, tree.roots());
    const auto elapsed = std::chrono::steady_clock::now() - started;

    int file_opens = 0;
    alignas(inotify_event) char buffer[4096];
    for (;;) {
        const ssize_t n = ::read(watch_fd, buffer, sizeof(buffer));
        if (n <= 0) break;
        for (ssize_t offset = 0; offset < n;) {
            const auto* event = reinterpret_cast<const inotify_event*>(buffer + offset);
            if ((event->mask & IN_ISDIR) == 0 && event->len > 0) ++file_opens;
            offset += static_cast<ssize_t>(sizeof(inotify_event) + event->len);
        }
    }
    ::close(watch_fd);

    CHECK(file_opens == 0);
    CHECK(elapsed < std::chrono::seconds(5));
    CHECK(list.devices.size() == 3);
    CHECK(find_port(list, (tree.by_id() / "usb-1a86_USB_Serial-if00-port0").string()) != nullptr);
    const auto holder = util::serial_port_holder(held);
    REQUIRE(holder.has_value());
    CHECK(holder->kind == util::SerialClaimKind::Link);
    CHECK(holder->owner == "Gemini PDH");
    CHECK(util::serial_port_holder(probed)->owner == "QHY CFW3");
    CHECK_FALSE(util::serial_port_holder(free_port).has_value());
}

TEST_CASE("Attached devices: a configured port, HID path or camera serial names its device", "[attached_devices]") {
    test::FakeUsbSerialTree tree;
    const auto ch340 = tree.add_port("ttyUSB0", "1a86", "", "USB Serial", "usb-1a86_USB_Serial-if00-port0");
    const auto cp210x = tree.add_port("ttyACM0", "10c4", "Silicon Labs", "CP2102");
    const auto other = tree.add_port("ttyUSB1", "1a86", "", "USB Serial");
    DeviceCatalog catalog = make_catalog();
    catalog.add_hid_lister(kHid,
                           [] { return std::vector<AttachedHidDevice>{{"1-1:1.0", "Oasis"}, {"1-2:1.0", "Oasis"}}; });
    catalog.add_camera_lister(
        "epsilon", [] { return std::vector<AttachedCamera>{{0, "ASI432MM", "abc123"}, {1, "ASI120MM", "def456"}}; });

    std::vector<ConfiguredDevice> configured;
    // Configured through a by-path link: neither the by-id port nor the node,
    // but it resolves to the node.
    const auto by_path = tree.dev() / "serial" / "by-path";
    std::filesystem::create_directories(by_path);
    std::filesystem::create_symlink(ch340, by_path / "platform-xhci-usb-0:1.1:1.0-port0");
    DeviceConfig focuser;
    focuser.set("connectionType", std::string("serial"));
    focuser.set("portPath", (by_path / "platform-xhci-usb-0:1.1:1.0-port0").string());
    configured.push_back({kFocuser, 3, focuser});
    // A port field that does not apply (connectionType auto) names no port.
    DeviceConfig mount;
    mount.set("connectionType", std::string("auto"));
    mount.set("portPath", cp210x);
    configured.push_back({kMount, 1, mount});
    DeviceConfig hid;
    hid.set("hidPath", std::string("1-2:1.0"));
    configured.push_back({kHid, 4, hid});
    DeviceConfig camera;
    camera.set("serialNumber", std::string("def456"));
    configured.push_back({kCamera, 2, camera});

    const auto list = list_attached_devices(catalog, configured, tree.roots());

    REQUIRE(list.errors.empty());
    const AttachedDevice* marked = find_port(list, (tree.by_id() / "usb-1a86_USB_Serial-if00-port0").string());
    REQUIRE(marked != nullptr);
    REQUIRE(marked->configured_as.size() == 1);
    CHECK(marked->configured_as[0].type == DeviceType::Focuser);
    CHECK(marked->configured_as[0].device_number == 3);
    const AttachedDevice* unmarked = find_port(list, canonical(cp210x));
    REQUIRE(unmarked != nullptr);
    CHECK(unmarked->configured_as.empty());
    const AttachedDevice* free_port = find_port(list, canonical(other));
    REQUIRE(free_port != nullptr);
    CHECK(free_port->configured_as.empty());

    const AttachedDevice* hid_free = find_port(list, "1-1:1.0");
    const AttachedDevice* hid_used = find_port(list, "1-2:1.0");
    REQUIRE(hid_free != nullptr);
    REQUIRE(hid_used != nullptr);
    CHECK(hid_free->kind == AttachedKind::Hid);
    CHECK(config_string(hid_free->candidates.at(0).config, "hidPath") == "1-1:1.0");
    CHECK(hid_free->configured_as.empty());
    REQUIRE(hid_used->configured_as.size() == 1);
    CHECK(hid_used->configured_as[0].device_number == 4);

    std::vector<const AttachedDevice*> cameras;
    for (const auto& device : list.devices) {
        if (device.kind == AttachedKind::Camera) cameras.push_back(&device);
    }
    REQUIRE(cameras.size() == 2);
    CHECK(cameras[0]->serial_number == "abc123");
    CHECK(cameras[0]->configured_as.empty());
    REQUIRE(cameras[0]->candidates.size() == 1);
    CHECK(cameras[0]->candidates[0].key == kCamera);
    CHECK(config_string(cameras[0]->candidates[0].config, "serialNumber") == "abc123");
    const ConfigValue* index = cameras[1]->candidates[0].config.find_value("cameraIndex");
    REQUIRE(index != nullptr);
    CHECK(std::get<std::int64_t>(*index) == 1);
    REQUIRE(cameras[1]->configured_as.size() == 1);
    CHECK(cameras[1]->configured_as[0].type == DeviceType::Camera);
    CHECK(cameras[1]->configured_as[0].device_number == 2);
}

TEST_CASE("Attached devices: a throwing lister leaves the other vendors' results", "[attached_devices]") {
    test::FakeUsbSerialTree tree;
    tree.add_port("ttyUSB0", "1a86", "", "USB Serial", "usb-1a86_USB_Serial-if00-port0");
    DeviceCatalog catalog = make_catalog();
    add_schema(catalog, DeviceKey{"zeta", DeviceType::Camera}, camera_fields());
    catalog.add_hid_lister(kHid,
                           []() -> std::vector<AttachedHidDevice> { throw std::runtime_error("hid_init failed"); });
    catalog.add_camera_lister("zeta", []() -> std::vector<AttachedCamera> {
        throw AlpacaException("SDK listing failed", AlpacaError::DriverException);
    });
    catalog.add_camera_lister("epsilon", [] { return std::vector<AttachedCamera>{{0, "ASI432MM", "abc123"}}; });

    const auto list = list_attached_devices(catalog, {}, tree.roots());

    REQUIRE(list.errors.size() == 2);
    CHECK(list.errors[0].source == "delta HID lister");
    CHECK(list.errors[0].message == "hid_init failed");
    CHECK(list.errors[1].source == "zeta camera lister");
    CHECK(list.errors[1].message.find("SDK listing failed") != std::string::npos);
    REQUIRE(list.devices.size() == 2);
    CHECK(list.devices[0].kind == AttachedKind::Serial);
    CHECK(list.devices[1].kind == AttachedKind::Camera);
    CHECK(list.devices[1].serial_number == "abc123");
}
