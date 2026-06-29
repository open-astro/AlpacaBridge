// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply
// with all SSPL v1 requirements.

#pragma once

#include <string>
#include <string_view>

namespace alpacacore::util {

/**
 * @brief Build the error message for a failed serial auto-detect.
 *
 * Serial port enumeration is POSIX-only; on platforms without it (Windows) the
 * enumerate_*_ports() helpers always return an empty list, so an empty result
 * there means "auto-detect is unavailable", not "no hardware found". Surfacing
 * the same "no device detected" message on both platforms misleads a Windows
 * user into thinking their hardware is missing when they simply need to select
 * an explicit serial port. This centralises the platform-aware wording so every
 * auto-detect driver (WandererCover, Gemini, SynScan, Celestron, iOptron, ...)
 * reports it consistently.
 *
 * @param device_label Human-readable device name, e.g. "iOptron mount".
 */
inline std::string serial_auto_detect_failed_message(std::string_view device_label) {
#ifdef _WIN32
    return "Auto-detect is not supported on this platform — configure an explicit serial "
           "port for the " +
           std::string(device_label) + ".";
#else
    return "No " + std::string(device_label) + " found on any serial port.";
#endif
}

}  // namespace alpacacore::util
