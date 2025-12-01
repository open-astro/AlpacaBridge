// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, version 1 (SSPL v1).
// https://www.mongodb.com/licensing/server-side-public-license
//
// If you use this library to provide a network-accessible service, you must comply
// with the SSPL v1 requirements.

#pragma once

#include <alpacacore/alpacadriver.h>
#include <alpacacore/util/error_handling.h>

namespace alpacacore {

/**
 * @brief Cover state.
 */
enum class CoverState {
    NotPresent,
    Closed,
    Moving,
    Open,
    Unknown
};

/**
 * @brief Calibrator state.
 */
enum class CalibratorState {
    NotPresent,
    Off,
    NotReady,
    Ready,
    Unknown,
    Error
};

/**
 * @brief Pure virtual interface for Alpaca CoverCalibrator drivers.
 *
 * Follows ASCOM Alpaca CoverCalibrator API specification.
 * All cover calibrator drivers must implement this interface.
 */
class CoverCalibratorDriver : public AlpacaDriver {
public:
    virtual ~CoverCalibratorDriver() = default;

    // CoverCalibrator-specific properties

    /**
     * @brief Get whether the cover is present.
     */
    virtual bool get_calibrator_available() const = 0;

    /**
     * @brief Get the calibrator brightness.
     */
    virtual int get_brightness() const = 0;

    /**
     * @brief Set the calibrator brightness.
     */
    virtual void set_brightness(int brightness) = 0;

    /**
     * @brief Get the calibrator state.
     */
    virtual CalibratorState get_calibrator_state() const = 0;

    /**
     * @brief Get the maximum brightness.
     */
    virtual int get_max_brightness() const = 0;

    /**
     * @brief Get whether the cover is present.
     */
    virtual bool get_cover_state_available() const = 0;

    /**
     * @brief Get the cover state.
     */
    virtual CoverState get_cover_state() const = 0;

    // CoverCalibrator-specific methods

    /**
     * @brief Close the cover.
     */
    virtual void close_cover() = 0;

    /**
     * @brief Halt the cover movement.
     */
    virtual void halt_cover() = 0;

    /**
     * @brief Open the cover.
     */
    virtual void open_cover() = 0;

    /**
     * @brief Turn the calibrator off.
     */
    virtual void calibrator_off() = 0;

    /**
     * @brief Turn the calibrator on.
     */
    virtual void calibrator_on(int brightness) = 0;
};

} // namespace alpacacore

