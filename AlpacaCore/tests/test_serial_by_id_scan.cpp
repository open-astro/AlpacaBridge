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

#include <alpacacore/util/serial_by_id_scan.h>

#include "catch2_compat.h"

using alpacacore::util::path_exists;
using alpacacore::util::usb_tty_descriptor_matches;
using alpacacore::util::UsbTtyDescriptor;

TEST_CASE("usb_tty_descriptor_matches: vendor id exact, manufacturer/product substring", "[serial_by_id_scan]") {
    const UsbTtyDescriptor ch340{"1a86", "wch.cn", "USB Serial"};
    REQUIRE(usb_tty_descriptor_matches(ch340, {"1a86"}));
    REQUIRE_FALSE(usb_tty_descriptor_matches(ch340, {"a86"}));  // vendor_id is exact-match only
    REQUIRE(usb_tty_descriptor_matches(ch340, {"wch"}));
    REQUIRE(usb_tty_descriptor_matches(ch340, {"USB Serial"}));
    REQUIRE_FALSE(usb_tty_descriptor_matches(ch340, {"FTDI", "CP210", "Prolific"}));
}

TEST_CASE("usb_tty_descriptor_matches: udev-mangled patterns match raw sysfs strings (issue #181)",
          "[serial_by_id_scan]") {
    // udev by-id names replace spaces with underscores ("USB_Serial",
    // "Silicon_Labs"); pattern lists shared with by-id name filters use that
    // spelling, but the raw sysfs strings keep their spaces. Both spellings
    // must match.
    const UsbTtyDescriptor generic{"dead", "USB Serial", "USB2.0-Serial"};
    REQUIRE(usb_tty_descriptor_matches(generic, {"USB_Serial"}));
    REQUIRE(usb_tty_descriptor_matches(generic, {"USB Serial"}));

    const UsbTtyDescriptor cp210x{"10c4", "Silicon Labs", "CP2102 USB to UART Bridge Controller"};
    REQUIRE(usb_tty_descriptor_matches(cp210x, {"Silicon_Labs"}));
    REQUIRE(usb_tty_descriptor_matches(cp210x, {"Silicon Labs"}));
    REQUIRE(usb_tty_descriptor_matches(cp210x, {"CP210"}));
    REQUIRE(usb_tty_descriptor_matches(cp210x, {"USB_to_UART"}));
}

TEST_CASE("usb_tty_descriptor_matches: empty pattern list matches nothing", "[serial_by_id_scan]") {
    const UsbTtyDescriptor descriptor{"1a86", "wch.cn", "USB Serial"};
    REQUIRE_FALSE(usb_tty_descriptor_matches(descriptor, {}));
}

TEST_CASE("path_exists never throws and treats unreachable paths as absent", "[serial_by_id_scan]") {
    REQUIRE(path_exists("/"));
    REQUIRE_FALSE(path_exists("/nonexistent-alpacabridge-test-path"));
    // Traversing through a regular file as if it were a directory is the
    // error case that makes the throwing exists() overload abort a scan.
    REQUIRE_FALSE(path_exists("/etc/hostname/not-a-directory"));
}
