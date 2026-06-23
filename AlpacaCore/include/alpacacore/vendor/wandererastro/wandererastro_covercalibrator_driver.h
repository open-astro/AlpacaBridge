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

#include <alpacacore/covercalibrator_driver.h>
#include <memory>
#include <string>

namespace alpacacore::vendor::wandererastro {

/**
 * @brief Create a WandererCover V4 CoverCalibrator driver via serial port.
 *
 * The WandererCover V4 (Pro / EC / EC-IR) is a motorized dust cover combined
 * with an EL flat panel, controlled over a CH340 USB-serial link at 19200 baud.
 *
 * @param device_number Alpaca device number
 * @param serial_port Serial port path (e.g., "/dev/ttyUSB0")
 * @param baud_rate Serial baud rate (default 19200)
 * @return Unique pointer to CoverCalibrator driver
 */
std::unique_ptr<CoverCalibratorDriver> create_wandererastro_covercalibrator(
    int device_number,
    const std::string& serial_port,
    int baud_rate = 19200);

/**
 * @brief Create a WandererCover V4 driver by auto-detecting the serial port.
 *
 * Scans CH340/CH341 USB-serial adapters and listens for the WandererCover
 * status stream. The cover_index selects which detected cover to use (0-based).
 *
 * @param device_number Alpaca device number
 * @param cover_index 0-based index into the list of detected covers
 * @return Unique pointer to CoverCalibrator driver
 */
std::unique_ptr<CoverCalibratorDriver> create_wandererastro_covercalibrator_by_index(
    int device_number,
    int cover_index = 0);

} // namespace alpacacore::vendor::wandererastro
