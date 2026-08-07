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

// Shared /dev/serial/by-id scan used by every vendor's serial auto-detect
// (Gemini, WandererAstro, iOptron, SynScan, Celestron, ZWO, ...). Originally
// each enumerate_*_ports() duplicated its own directory_iterator/is_symlink
// loop; issue #179 found the same throwing-overload TOCTOU hazard unfixed in
// nine of those copies (PR #177 had only fixed canonical() in Gemini's flat
// panel wrapper) and it was clearly a repo-wide pattern worth centralising
// instead of patching nine near-identical copies separately.
//
// Also home to read_raw_tty_usb_descriptor()/usb_tty_descriptor_matches(),
// generalised from the sysfs vendor-descriptor filter PR #177 added to
// Gemini's flat panel wrapper (originally file-local as
// raw_port_looks_like_flatpanel_candidate()) so every vendor's raw-node
// fallback scan can use the same udev-by-id-collision workaround: run the
// raw /dev/ttyUSBn scan unconditionally (not only when by-id is entirely
// missing) without indiscriminately opening -- and for CH340/CH341-class
// hardware, DTR-resetting -- every unrelated serial device on the box.

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace alpacacore::util {

/**
 * @brief Non-throwing std::filesystem::exists().
 *
 * The plain exists(path) overload throws filesystem_error on a traversal
 * error (e.g. EACCES on a parent directory), which in enumerate_*_ports()
 * would abort the whole auto-detect — including fallbacks the scan is
 * expected to fall through to (issue #181; ZWO's WiFi fallback was the
 * reported case, but every wrapper gates its scans on exists()). A path
 * that can't be checked is treated as absent.
 */
inline bool path_exists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

/// One symlink found directly inside a /dev/serial/by-id-style directory.
struct SerialByIdEntry {
    std::string name;            ///< Symlink filename, e.g. "usb-1a86_USB_Serial-if00-port0".
    std::filesystem::path path;  ///< Full path to the symlink (not yet canonicalized).
};

/**
 * @brief List the symlinks directly inside a /dev/serial/by-id-style directory.
 *
 * A device can be unplugged at any point during the scan — between the
 * directory being opened and an entry being read, or between two entries.
 * Every filesystem call here uses the std::error_code overload (iterator
 * construction, increment, and is_symlink()), so this function never throws
 * std::filesystem::filesystem_error: a mid-scan unplug just ends the scan
 * with whatever entries were already collected, instead of throwing out of
 * the caller's enumerate_*_ports() and discarding them (the caller's own
 * canonical() resolution on each returned path can still race a further
 * unplug and should keep using the error_code overload too).
 *
 * Non-symlink entries are skipped — /dev/serial/by-id only ever contains
 * symlinks to the real /dev/ttyUSBn or /dev/ttyACMn node, but callers used to
 * filter this themselves and a stray regular file/directory would otherwise
 * need the same check duplicated at every call site.
 *
 * Callers that need to distinguish "directory absent" from "directory empty"
 * (e.g. to decide whether to run a raw /dev/ttyUSBn / ttyACMn fallback scan)
 * should check std::filesystem::exists(dir) themselves first, as they
 * already do for their own by-id/raw-fallback branching — this function
 * treats both cases the same way and simply returns an empty list.
 */
inline std::vector<SerialByIdEntry> list_serial_by_id(const std::filesystem::path& dir) {
    std::vector<SerialByIdEntry> results;

    std::error_code dir_ec;
    for (auto it = std::filesystem::directory_iterator(dir, dir_ec);
         !dir_ec && it != std::filesystem::directory_iterator(); it.increment(dir_ec)) {
        const auto& entry = *it;
        std::error_code symlink_ec;
        if (!entry.is_symlink(symlink_ec) || symlink_ec) continue;
        results.push_back({entry.path().filename().string(), entry.path()});
    }
    return results;
}

/// USB vendor/manufacturer/product descriptor strings for a tty's physical device.
struct UsbTtyDescriptor {
    std::string vendor_id;  ///< e.g. "1a86" (lowercase hex, no "0x" prefix).
    std::string manufacturer;
    std::string product;
};

namespace detail {
inline std::string read_sysfs_line(const std::filesystem::path& file) {
    std::ifstream in(file);
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    return line;
}

// udev builds by-id names by replacing whitespace (and other unsafe
// characters) in the raw descriptor strings with '_', so "USB Serial"
// becomes "USB_Serial". Vendor pattern lists are written against those
// udev-mangled names; mangling the raw sysfs string the same way lets one
// pattern spelling match both.
inline std::string udev_mangle(std::string value) {
    for (char& ch : value) {
        if (ch == ' ' || ch == '\t' || ch == '/') ch = '_';
    }
    return value;
}
}  // namespace detail

/**
 * @brief Read the USB descriptor of the physical device behind a raw tty node.
 *
 * The raw /dev/ttyUSBn/ttyACMn fallback scan has no by-id symlink name to
 * filter on, so without this it would open (and for some hardware,
 * DTR-reset) EVERY serial device on the box when probing -- unrelated FTDI
 * dongles, GPS receivers, or a second mount controller sharing the same
 * ttyUSB/ttyACM namespace -- not just the vendor's own hardware. This walks
 * up from the tty's sysfs node to the physical USB device directory and
 * reads the same vendor/manufacturer/product descriptor fields udev uses to
 * build by-id names, so a raw-node fallback can be filtered exactly as
 * narrowly as its by-id counterpart. Returns std::nullopt -- never probe --
 * if the descriptor can't be read.
 */
inline std::optional<UsbTtyDescriptor> read_raw_tty_usb_descriptor(const std::string& port_path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = fs::canonical(fs::path("/sys/class/tty") / fs::path(port_path).filename() / "device", ec);
    if (ec) return std::nullopt;

    // The tty's immediate sysfs node is a USB interface directory; the
    // physical device carrying idVendor/manufacturer/product is a couple of
    // levels up. Bounded walk so a weird sysfs layout can't loop forever.
    for (int hop = 0; hop < 6; ++hop) {
        std::error_code exists_ec;
        if (fs::exists(dir / "idVendor", exists_ec) && !exists_ec) {
            return UsbTtyDescriptor{detail::read_sysfs_line(dir / "idVendor"),
                                    detail::read_sysfs_line(dir / "manufacturer"),
                                    detail::read_sysfs_line(dir / "product")};
        }
        fs::path parent = dir.parent_path();
        if (parent.empty() || parent == dir) break;
        dir = parent;
    }
    return std::nullopt;
}

/**
 * @brief Check a USB descriptor against a vendor's known hardware signatures.
 *
 * Each pattern is checked as an exact match against vendor_id and as a
 * substring match against manufacturer/product -- the same two-level check
 * every vendor's by-id name filter already does, just against sysfs fields
 * instead of the by-id symlink's filename.
 *
 * Raw sysfs strings keep their spaces ("USB Serial", "Silicon Labs") while
 * by-id names carry udev's underscore mangling ("USB_Serial"); pattern lists
 * are shared with the by-id name filters and mostly written in the udev
 * spelling, so manufacturer/product are matched in both the raw and the
 * udev-mangled form (issue #181 -- the underscore patterns could never
 * match otherwise).
 */
inline bool usb_tty_descriptor_matches(const UsbTtyDescriptor& descriptor,
                                       std::initializer_list<std::string> patterns) {
    const std::string mangled_manufacturer = detail::udev_mangle(descriptor.manufacturer);
    const std::string mangled_product = detail::udev_mangle(descriptor.product);
    for (const auto& pattern : patterns) {
        if (descriptor.vendor_id == pattern) return true;
        if (descriptor.manufacturer.find(pattern) != std::string::npos) return true;
        if (descriptor.product.find(pattern) != std::string::npos) return true;
        if (mangled_manufacturer.find(pattern) != std::string::npos) return true;
        if (mangled_product.find(pattern) != std::string::npos) return true;
    }
    return false;
}

}  // namespace alpacacore::util
