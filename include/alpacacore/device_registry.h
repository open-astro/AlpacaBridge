// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
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
#include <alpacacore/alpaca_defs.h>
#include <alpacacore/device_capabilities.h>
#include <memory>
#include <vector>
#include <string>

namespace alpacacore::management {

/**
 * @brief Device registry for managing all registered Alpaca devices.
 *
 * This registry allows higher-level servers (e.g., AlpacaHTTP) to:
 * - Register device instances
 * - Retrieve devices by type and number
 * - Get all configured devices
 */
class DeviceRegistry {
public:
    /**
     * @brief Get the singleton instance of the device registry.
     */
    static DeviceRegistry& instance();

    /**
     * @brief Register a device with the registry.
     *
     * @param device Shared pointer to the device driver
     * @return true if registration succeeded, false if device number already exists
     */
    bool register_device(std::shared_ptr<AlpacaDriver> device);

    /**
     * @brief Unregister a device from the registry.
     *
     * @param device_type Device type
     * @param device_number Device number
     * @return true if device was found and removed
     */
    bool unregister_device(DeviceType device_type, int device_number);

    /**
     * @brief Get a device by type and number.
     *
     * @param device_type Device type
     * @param device_number Device number
     * @return Shared pointer to device, or nullptr if not found
     */
    std::shared_ptr<AlpacaDriver> get_device(DeviceType device_type, int device_number) const;

    /**
     * @brief Get all devices of a specific type.
     *
     * @param device_type Device type
     * @return Vector of shared pointers to devices of that type
     */
    std::vector<std::shared_ptr<AlpacaDriver>> get_devices_by_type(DeviceType device_type) const;

    /**
     * @brief Get all registered devices.
     *
     * @return Vector of shared pointers to all devices
     */
    std::vector<std::shared_ptr<AlpacaDriver>> get_all_devices() const;

    /**
     * @brief Get capabilities for all registered devices.
     *
     * @return Vector of device capabilities
     */
    std::vector<DeviceCapabilities> get_all_device_capabilities() const;

    /**
     * @brief Clear all registered devices.
     */
    void clear();

    /**
     * @brief Get the number of registered devices.
     */
    size_t device_count() const;

private:
    DeviceRegistry() = default;
    ~DeviceRegistry() = default;
    DeviceRegistry(const DeviceRegistry&) = delete;
    DeviceRegistry& operator=(const DeviceRegistry&) = delete;

    struct DeviceEntry {
        DeviceType type;
        int number;
        std::shared_ptr<AlpacaDriver> device;
    };

    mutable std::vector<DeviceEntry> devices_;
};

} // namespace alpacacore::management


