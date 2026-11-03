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

#include <alpacacore/vendor/zwo/shared_open_table.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace alpacacore::vendor::zwo {

struct ZWOEFWInfo {
    int wheel_id{};
    std::string name;
    int slot_count{};
    // SDK enumeration slot, not the device ID or a filter position; unknown for ID-only reads.
    int slot_index{-1};
};

/**
 * @brief Abstract ZWO EFW SDK surface used by the filter wheel driver.
 *
 * Production passes the ZWOEFWSDKWrapper singleton; tests inject a scripted
 * fake through the factory overloads in zwo_filterwheel_driver.h, so the driver
 * runs without the vendor SDK. Same shape as ZWOEAFSDK and ZWOCAASDK: the
 * destructor is protected and non-virtual, the driver holds a reference.
 *
 * Opens are shared through SharedOpenTable (issue #1668, as the EAF since issue #1500): an open
 * returns a link, and every per-device method below runs only inside with_link() for that id and
 * link. Enumeration, info and the SDK version need no link.
 */
class ZWOEFWSDK {
public:
    virtual std::vector<ZWOEFWInfo> enumerate_wheels() = 0;
    virtual bool get_wheel_info_by_id(int wheel_id, ZWOEFWInfo& info) = 0;
    virtual bool get_wheel_info_by_index(int wheel_index, ZWOEFWInfo& info) = 0;

    /// Opens the wheel, or shares a held open of the same unit; returns the link and the serial read at the open.
    virtual SharedOpenTable::Opened open_wheel_link(int wheel_id) = 0;
    /// As open_wheel_link(), but nullopt for a held unit that reports no serial (it is not shared).
    virtual std::optional<SharedOpenTable::Opened> open_wheel_candidate(int wheel_id) = 0;
    /// Drops this holder's open; a retired link is a no-op.
    virtual void close_wheel_link(int wheel_id, std::uint64_t link) = 0;
    /// Runs @p fn with @p link as this thread's link for @p wheel_id; the wrapper takes no lock across @p fn.
    virtual void with_link(int wheel_id, std::uint64_t link, const std::function<void()>& fn) = 0;

    virtual int get_position(int wheel_id) = 0;
    virtual void set_position(int wheel_id, int position) = 0;

    virtual void set_unidirectional(int wheel_id, bool enabled) = 0;
    virtual bool get_unidirectional(int wheel_id) = 0;
    virtual void calibrate(int wheel_id) = 0;

    virtual int get_hardware_error_code(int wheel_id) = 0;
    virtual std::string get_serial_number(int wheel_id) = 0;
    virtual std::string get_firmware_version(int wheel_id) = 0;
    virtual std::string get_sdk_version() = 0;

protected:
    ~ZWOEFWSDK() = default;
};

}  // namespace alpacacore::vendor::zwo
