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
    Unknown,
    Error
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

    /**
     * @brief Platform 7 DeviceState snapshot for CoverCalibrator devices.
     *
     * Reports the operational properties (Brightness, CalibratorState,
     * CoverState, CalibratorChanging, CoverMoving) plus a TimeStamp by calling
     * this device's own property getters — the same ones the GET endpoints use,
     * which is what guarantees the DeviceState↔GET consistency ConformU checks.
     * Each getter is wrapped so a property that throws (e.g. NotConnected) is
     * omitted rather than failing the whole call.
     *
     * Per AGENTS.md, DeviceState is intentionally NOT an atomic snapshot (each
     * getter locks separately) and vendors must NOT override this with a
     * single-lock per-vendor version: ASCOM doesn't require cross-property
     * atomicity and ConformU only checks per-property GET consistency.
     */
    std::vector<DeviceState> get_device_state() const override {
        std::vector<DeviceState> state;
        auto add = [&state](const char* name, auto getter) {
            try {
                state.push_back({name, DeviceStateValue{getter()}});
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
                // Not currently known -- or an unwrapped vendor error -- so omit per the DeviceState contract.
            }
        };
        add("Brightness", [this] { return static_cast<std::int32_t>(get_brightness()); });
        add("CalibratorState", [this] { return static_cast<std::int32_t>(get_calibrator_state()); });
        add("CoverState", [this] { return static_cast<std::int32_t>(get_cover_state()); });
        add("CalibratorChanging", [this] { return get_calibrator_changing(); });
        add("CoverMoving", [this] { return get_cover_moving(); });
        state.push_back({"TimeStamp", device_state_timestamp()});
        return state;
    }

    // CoverCalibrator-specific properties

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
     * @brief Get whether the calibrator is changing state.
     */
    virtual bool get_calibrator_changing() const = 0;

    /**
     * @brief Get the maximum brightness.
     */
    virtual int get_max_brightness() const = 0;

    /**
     * @brief Get the cover state.
     */
    virtual CoverState get_cover_state() const = 0;

    /**
     * @brief Get whether the cover is moving.
     */
    virtual bool get_cover_moving() const = 0;

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
