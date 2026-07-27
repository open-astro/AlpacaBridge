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

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace alpacacore::vendor::astroasis {

/**
 * @brief Information about a detected Astroasis Oasis focuser USB HID device.
 */
struct AstroasisPortInfo {
    std::string hid_path;  // hidapi device path (opaque, OS-specific)
    // No serial_number field: nothing consumes it today, and the naive
    // wchar_t->char narrowing it needed would mangle non-ASCII USB serial
    // strings. Re-add with proper UTF-8 conversion if/when it is wired into
    // unique_id or auto-detect disambiguation (issue #155).
};

/**
 * @brief Enumerate Astroasis Oasis focusers on the USB bus (VID:PID 338F:A0F0).
 */
std::vector<AstroasisPortInfo> enumerate_astroasis_focusers();

/**
 * @brief Protocol wrapper for the Astroasis Oasis Focuser.
 *
 * Reverse-engineered from the vendor's ASCOM driver (Astroasis_Oasis_Focuser_
 * ASCOM_2.0.2.1_Setup.exe -> OasisFocuser64.dll), which is not the serial
 * MyFocuserPro2 protocol the other "Gemini"-branded focusers in this repo
 * use -- it is a USB HID vendor protocol (VID:PID 338F:A0F0), talked to
 * directly via hidapi (hidraw backend) rather than any vendor SDK binary.
 *
 * Wire format (65-byte HID reports, report ID 0):
 *   Request:  [reportId=0][cmd][len][payload...]
 *   Response: [cmd echo][len][payload...]
 * Multi-byte integer payloads are big-endian (network byte order) except the
 * connect handshake's tick-count nonce, which is sent as-is (host byte order)
 * -- confirmed by disassembly, not a transcription error.
 *
 * TODO: the vendor SDK caches a protocol/firmware version from the connect
 * handshake and uses it to pick between a short (older firmware) and long
 * (newer firmware) GetStatus/GetConfig response layout. This wrapper does not
 * replicate that cache; it tries the older/shorter layout first and falls
 * back to the newer/longer one if the device reports a length mismatch.
 * Needs verification against real hardware.
 */
class AstroasisProtocolWrapper {
public:
    AstroasisProtocolWrapper();
    ~AstroasisProtocolWrapper();

    AstroasisProtocolWrapper(const AstroasisProtocolWrapper&) = delete;
    AstroasisProtocolWrapper& operator=(const AstroasisProtocolWrapper&) = delete;

    /**
     * @brief Open the HID device and run the connect handshake (cmd 0x11, 0x10).
     * @param hid_path hidapi device path, e.g. from enumerate_astroasis_focusers().
     */
    void connect(const std::string& hid_path);

    /**
     * @brief Close the HID device. No protocol command is sent (matches the
     * vendor SDK's AOFocuserClose, which only tears down the OS handle).
     */
    void disconnect();

    bool is_connected() const;

    struct Status {
        int position = 0;
        bool moving = false;
        double temperature_internal = 0.0;
        double temperature_external = 0.0;
        bool temperature_external_valid = false;
    };

    /** @brief Query position/moving/temperature. Command 0x32, no payload. */
    Status get_status();

    /** @brief Query the configured max step. Command 0x30 (or 0x3a), no payload. */
    int get_max_step();

    /** @brief Move to an absolute position. Command 0x36, 4-byte BE position. */
    void move_to(int position);

    /** @brief Stop any in-progress move. Command 0x37, no payload. */
    void stop_move();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alpacacore::vendor::astroasis
