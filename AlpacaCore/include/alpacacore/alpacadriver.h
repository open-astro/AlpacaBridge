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

#include <alpacacore/alpaca_defs.h>
#include <alpacacore/util/error_handling.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alpacacore {

/**
 * @brief Base interface for all Alpaca device drivers.
 *
 * All device drivers must implement this interface.
 * This provides the common Alpaca device API.
 */
class AlpacaDriver {
public:
    virtual ~AlpacaDriver() = default;

    // Common Alpaca device properties and methods

    /**
     * @brief Get the device number.
     */
    virtual int get_device_number() const = 0;

    /**
     * @brief Get the device name.
     */
    virtual std::string get_name() const = 0;

    /**
     * @brief Get the device type.
     */
    virtual DeviceType get_device_type() const = 0;

    /**
     * @brief Get the device unique ID.
     *
     * This is a unique identifier for the device instance.
     */
    virtual std::string get_unique_id() const = 0;

    /**
     * @brief Get the device description.
     */
    virtual std::string get_description() const = 0;

    /**
     * @brief Get the driver info.
     */
    virtual std::string get_driver_info() const = 0;

    /**
     * @brief Get the driver version.
     */
    virtual std::string get_driver_version() const = 0;

    /**
     * @brief Get the device's reported hardware firmware revision, if available.
     *
     * Optional hook surfaced **only** in the AlpacaBridge web UI (the management
     * configureddevices response), never in the ASCOM @c DriverInfo string, so
     * @c DriverInfo stays clean for NINA and other Alpaca clients. This is the
     * **device's own firmware** (e.g. a mount handset version or a camera's
     * on-board firmware) — NOT the vendor SDK/library version, which has its own
     * hook below. Drivers that can read real firmware override this; the default
     * returns @c std::nullopt so nothing is shown. Implementations must be cheap
     * and non-blocking (return a value cached at connect, not a fresh device
     * round-trip) and should return @c std::nullopt when the device is not
     * connected and the value is unknown.
     */
    virtual std::optional<std::string> get_device_firmware() const { return std::nullopt; }

    /**
     * @brief Get the vendor SDK / library version backing this driver, if any.
     *
     * Same web-UI-only contract as get_device_firmware() (never in @c DriverInfo).
     * This is the version of the vendor SDK the driver links against (e.g. the
     * ZWO ASI SDK), reported separately from device firmware because it is a host
     * software version, not a property of the connected hardware. SDK-based
     * drivers (ZWO, QHY, Player One, ...) override this; the default returns
     * @c std::nullopt. Must be cheap and non-blocking.
     */
    virtual std::optional<std::string> get_device_sdk_version() const { return std::nullopt; }

    /**
     * @brief Get the ASCOM interface version.
     */
    virtual int get_interface_version() const = 0;

    /**
     * @brief Get whether the device is connected.
     */
    virtual bool get_connected() const = 0;

    /**
     * @brief Set whether the device is connected.
     */
    virtual void set_connected(bool connected) = 0;

    /**
     * @brief Start an asynchronous connect to the device.
     */
    virtual void connect() { set_connected(true); }

    /**
     * @brief Start an asynchronous disconnect from the device.
     */
    virtual void disconnect() { set_connected(false); }

    /**
     * @brief Get whether the device is currently connecting or disconnecting.
     */
    virtual bool get_connecting() const { return false; }

    /**
     * @brief Get the device state snapshot.
     */
    virtual std::vector<DeviceState> get_device_state() const { return {}; }

    /**
     * @brief Get the supported actions.
     */
    virtual std::vector<std::string> get_supported_actions() const = 0;

    /**
     * @brief Invoke an action.
     *
     * @param action_name Action name
     * @param action_parameters Action parameters (JSON string)
     * @return Action result (JSON string)
     */
    virtual std::string action(std::string_view action_name,
                               std::string_view action_parameters) = 0;

    /**
     * @brief Get whether an action is supported.
     */
    virtual bool can_action(std::string_view action_name) const = 0;

    /**
     * @brief Get the command blind result.
     *
     * @param command Command string
     * @param raw Whether to return raw response
     * @return Command result
     */
    virtual std::string command_blind(std::string_view command, bool raw = false) = 0;

    /**
     * @brief Get the command bool result.
     *
     * @param command Command string
     * @param raw Whether to return raw response
     * @return Command result
     */
    virtual bool command_bool(std::string_view command, bool raw = false) = 0;

    /**
     * @brief Get the command string result.
     *
     * @param command Command string
     * @param raw Whether to return raw response
     * @return Command result
     */
    virtual std::string command_string(std::string_view command, bool raw = false) = 0;
};

/**
 * @brief Current UTC time formatted for a DeviceState "TimeStamp" entry.
 *
 * Returns an ISO 8601 / round-trippable string (YYYY-MM-DDTHH:MM:SS.mmmZ).
 * Every Platform 7 DeviceState response includes a TimeStamp recording when the
 * state snapshot was taken; this is the shared helper the device base classes
 * use to produce it. Defined inline so it is available in every translation
 * unit (including the per-vendor static libraries) without a link-order
 * dependency on the core library.
 */
inline std::string device_state_timestamp() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const std::time_t secs = clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;

    std::tm utc_tm{};
#if defined(_WIN32)
    gmtime_s(&utc_tm, &secs);
#else
    gmtime_r(&secs, &utc_tm);
#endif

    char date_buf[24];
    std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%dT%H:%M:%S", &utc_tm);

    char out[40];
    std::snprintf(out, sizeof(out), "%s.%03lldZ", date_buf, static_cast<long long>(ms));
    return out;
}

} // namespace alpacacore
