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

#pragma once

// A fake /dev, /dev/serial/by-id and /sys/class/tty tree for the
// attacheddevices listing (#2312), shared by test_attached_devices.cpp and
// AlpacaHTTP's test_routing.cpp. Each port is a regular file standing in for
// the tty node (never a real tty, so nothing can talk to it), with the sysfs
// layout a USB-serial adapter has: /sys/class/tty/<node>/device links to the
// port directory under the USB interface, and the USB device two levels up
// holds idVendor, manufacturer and product. No Catch2 here; the caller checks.

#include <alpacacore/catalog/attached_devices.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace alpacacore::test {

class FakeUsbSerialTree {
public:
    FakeUsbSerialTree() {
        static std::atomic<int> counter{0};
        root_ = std::filesystem::temp_directory_path() /
                ("alpacacore-usb-tree-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(by_id());
        std::filesystem::create_directories(root_ / "sys" / "class" / "tty");
    }
    ~FakeUsbSerialTree() {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }
    FakeUsbSerialTree(const FakeUsbSerialTree&) = delete;
    FakeUsbSerialTree& operator=(const FakeUsbSerialTree&) = delete;

    // Adds /dev/<node> with the given USB descriptor; a non-empty by_id_name
    // also adds /dev/serial/by-id/<by_id_name> linking to it. Returns the
    // node's path.
    std::string add_port(const std::string& node, const std::string& vendor_id, const std::string& manufacturer,
                         const std::string& product, const std::string& by_id_name = "") {
        const auto node_path = dev() / node;
        std::ofstream(node_path).put('\0');
        const auto usb_device = root_ / "sys" / "devices" / "usb1" / ("1-" + std::to_string(++ports_));
        const auto port_dir = usb_device / (usb_device.filename().string() + ":1.0") / node;
        std::filesystem::create_directories(port_dir);
        std::ofstream(usb_device / "idVendor") << vendor_id << "\n";
        if (!manufacturer.empty()) std::ofstream(usb_device / "manufacturer") << manufacturer << "\n";
        std::ofstream(usb_device / "product") << product << "\n";
        const auto class_dir = root_ / "sys" / "class" / "tty" / node;
        std::filesystem::create_directories(class_dir);
        std::filesystem::create_directory_symlink(port_dir, class_dir / "device");
        if (!by_id_name.empty()) std::filesystem::create_symlink(node_path, by_id() / by_id_name);
        return node_path.string();
    }

    std::filesystem::path dev() const { return root_ / "dev"; }
    std::filesystem::path by_id() const { return root_ / "dev" / "serial" / "by-id"; }

    catalog::AttachedDeviceRoots roots() const {
        catalog::AttachedDeviceRoots roots;
        roots.serial_by_id = by_id();
        roots.dev = dev();
        roots.tty_class = root_ / "sys" / "class" / "tty";
        return roots;
    }

private:
    std::filesystem::path root_;
    int ports_ = 0;
};

}  // namespace alpacacore::test
