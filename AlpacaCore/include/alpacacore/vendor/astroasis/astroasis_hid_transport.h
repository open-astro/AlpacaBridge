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

#include <cstddef>
#include <cstdint>
#include <string>

namespace alpacacore::vendor::astroasis {

/**
 * @brief Abstract HID surface under AstroasisProtocolWrapper (open-astro#294).
 *
 * Production uses a hidapi-backed instance the wrapper creates itself; tests
 * inject a scripted fake through the factory overload in
 * astroasis_focuser_driver.h, so the driver runs and its HID transactions can
 * be counted without a focuser. Same pattern as ZWOCAASDK, with one
 * difference: that seam is a stateless singleton the driver references, while
 * a HID handle is per-device state, so each wrapper owns its transport.
 */
class AstroasisHidTransport {
public:
    virtual ~AstroasisHidTransport() = default;

    /// Open the device. Returns false when the node cannot be opened; throws
    /// AlpacaException when the HID library itself cannot initialize.
    virtual bool open(const std::string& hid_path) = 0;

    /// Close the handle. No-op when not open.
    virtual void close() = 0;

    /// Write one output report. Returns bytes written, or < 0 on failure.
    virtual int write(const std::uint8_t* data, std::size_t length) = 0;

    /// Read one input report, waiting up to timeout_ms (0 = non-blocking).
    /// Returns bytes read, 0 on timeout, < 0 on failure.
    virtual int read(std::uint8_t* data, std::size_t length, int timeout_ms) = 0;
};

}  // namespace alpacacore::vendor::astroasis
