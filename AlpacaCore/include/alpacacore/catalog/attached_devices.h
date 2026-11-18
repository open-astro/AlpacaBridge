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

// What is plugged in, for a setup flow that adds devices (#2312): every serial
// port, HID device and camera the built vendors could drive, each with the
// catalog entries it could be. Rules:
//   1. No serial port is opened, no SerialLink or registry claim is taken and no
//      probe runs. A serial port is read from the by-id directory, the raw
//      ttyUSBn/ttyACMn names and the USB descriptor in sysfs, so a port a
//      connected device holds is listed and its claim is untouched.
//   2. A port lists every catalog entry whose USB identity it matches (one CH340
//      matches several vendors); the client chooses. A port that matches none
//      is not listed.
//   3. HID devices come from the registered HID listers, which enumerate
//      without opening; cameras come from the registered camera listers, which
//      may open a camera through the vendor's shared open table, as
//      attachedcameras does.
//   4. A lister that throws is reported in `errors` beside the results the
//      other listers found.
//   5. A result whose port, HID path or camera serial a configured device
//      names lists that device in `configured_as`.

#include <alpacacore/alpaca_defs.h>
#include <alpacacore/catalog/device_catalog.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace alpacacore::catalog {

// Where the serial listing looks. Tests point all three at a fake tree.
struct AttachedDeviceRoots {
    std::filesystem::path serial_by_id = "/dev/serial/by-id";
    std::filesystem::path dev = "/dev";                  // raw ttyUSBn / ttyACMn nodes
    std::filesystem::path tty_class = "/sys/class/tty";  // USB descriptors
};

// The listing stops adding devices at this count, so one response is bounded
// whatever a box enumerates (decision 0007 rule 10).
inline constexpr std::size_t kMaxAttachedDevices = 256;

enum class AttachedKind : std::uint8_t { Serial, Hid, Camera };

struct AttachedCandidate {
    DeviceKey key;
    // What a configuredevice call needs beyond vendor and deviceType: the port
    // (or HID path, or camera index and serial) under the descriptor's own
    // field name, the discriminator that field applies under, and the
    // identity's fixed values.
    DeviceConfig config;
};

struct ConfiguredDeviceRef {
    DeviceType type;
    int device_number;
};

struct AttachedDevice {
    AttachedKind kind = AttachedKind::Serial;
    std::string port;           // serial: the by-id path, else the raw node; HID: the hidapi path
    std::string node;           // serial: the tty node the port resolves to
    std::string serial_number;  // camera: the serial its lister read, may be empty
    int camera_index = -1;      // camera: the lister's index
    std::string name;           // display name
    std::vector<AttachedCandidate> candidates;
    std::vector<ConfiguredDeviceRef> configured_as;
};

struct AttachedListError {
    std::string source;  // e.g. "zwo camera lister"
    std::string message;
};

struct AttachedDeviceList {
    std::vector<AttachedDevice> devices;
    std::vector<AttachedListError> errors;
};

// A configured device, as the caller's persisted list holds it.
struct ConfiguredDevice {
    DeviceKey key;
    int device_number = 0;
    DeviceConfig config;
};

// Lists the attached devices per the rules above. Never throws for a lister
// failure; a filesystem error ends that part of the scan with what it found.
AttachedDeviceList list_attached_devices(const DeviceCatalog& catalog, const std::vector<ConfiguredDevice>& configured,
                                         const AttachedDeviceRoots& roots = {});

}  // namespace alpacacore::catalog
