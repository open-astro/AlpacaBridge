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
#include <alpacacore/device_capabilities.h>
#include <vector>
#include <string>

namespace alpacacore {

/**
 * @brief Pure virtual interface for Alpaca Management driver.
 *
 * Follows ASCOM Alpaca Management API specification.
 * Provides device discovery and management capabilities.
 */
class ManagementDriver : public AlpacaDriver {
public:
    virtual ~ManagementDriver() = default;

    // Management-specific properties

    /**
     * @brief Get the server name.
     *
     * Note: This shadows AlpacaDriver::get_name() for Management devices.
     */
    virtual std::string get_name() const = 0;

    /**
     * @brief Get the server version.
     */
    virtual std::string get_version() const = 0;

    /**
     * @brief Get the server description.
     */
    virtual std::string get_description() const = 0;

    /**
     * @brief Get the server manufacturer.
     */
    virtual std::string get_manufacturer() const = 0;

    /**
     * @brief Get the server manufacturer version.
     */
    virtual std::string get_manufacturer_version() const = 0;

    /**
     * @brief Get the server location.
     */
    virtual std::string get_location() const = 0;

    /**
     * @brief Get the UTC offset in hours.
     */
    virtual double get_utc_offset() const = 0;

    // Management-specific methods

    /**
     * @brief Get supported API versions.
     *
     * @return Vector of supported API version numbers (e.g., [1, 2, 3])
     */
    virtual std::vector<int> get_api_versions() const = 0;

    /**
     * @brief Get all configured devices.
     */
    virtual std::vector<DeviceCapabilities> get_configured_devices() const = 0;
};

} // namespace alpacacore

