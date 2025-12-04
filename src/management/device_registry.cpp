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

#include <alpacacore/device_registry.h>
#include <alpacacore/util/logging.h>
#include <algorithm>
#include <mutex>

namespace alpacacore::management {

namespace {
    std::mutex g_registry_mutex;
}

DeviceRegistry& DeviceRegistry::instance() {
    static DeviceRegistry registry;
    return registry;
}

bool DeviceRegistry::register_device(std::shared_ptr<AlpacaDriver> device) {
    if (!device) {
        ALPACA_LOG_ERROR("DeviceRegistry", "Attempted to register null device");
        return false;
    }

    std::lock_guard<std::mutex> lock(g_registry_mutex);
    
    DeviceType type = device->get_device_type();
    int number = device->get_device_number();

    // Check if device with same type and number already exists
    auto it = std::find_if(devices_.begin(), devices_.end(),
        [type, number](const DeviceEntry& entry) {
            return entry.type == type && entry.number == number;
        });

    if (it != devices_.end()) {
        ALPACA_LOG_WARN("DeviceRegistry", 
            "Device " + std::string(device_type_to_string(type)) + 
            " #" + std::to_string(number) + " already registered");
        return false;
    }

    DeviceEntry entry;
    entry.type = type;
    entry.number = number;
    entry.device = device;
    devices_.push_back(entry);

    ALPACA_LOG_INFO("DeviceRegistry",
        "Registered device: " + std::string(device_type_to_string(type)) +
        " #" + std::to_string(number) + " (" + device->get_name() + ")");

    return true;
}

bool DeviceRegistry::unregister_device(DeviceType device_type, int device_number) {
    std::lock_guard<std::mutex> lock(g_registry_mutex);

    auto it = std::find_if(devices_.begin(), devices_.end(),
        [device_type, device_number](const DeviceEntry& entry) {
            return entry.type == device_type && entry.number == device_number;
        });

    if (it == devices_.end()) {
        return false;
    }

    devices_.erase(it);
    ALPACA_LOG_INFO("DeviceRegistry",
        "Unregistered device: " + std::string(device_type_to_string(device_type)) +
        " #" + std::to_string(device_number));

    return true;
}

std::shared_ptr<AlpacaDriver> DeviceRegistry::get_device(DeviceType device_type, int device_number) const {
    std::lock_guard<std::mutex> lock(g_registry_mutex);

    auto it = std::find_if(devices_.begin(), devices_.end(),
        [device_type, device_number](const DeviceEntry& entry) {
            return entry.type == device_type && entry.number == device_number;
        });

    if (it == devices_.end()) {
        return nullptr;
    }

    return it->device;
}

std::vector<std::shared_ptr<AlpacaDriver>> DeviceRegistry::get_devices_by_type(DeviceType device_type) const {
    std::lock_guard<std::mutex> lock(g_registry_mutex);

    std::vector<std::shared_ptr<AlpacaDriver>> result;
    for (const auto& entry : devices_) {
        if (entry.type == device_type) {
            result.push_back(entry.device);
        }
    }

    return result;
}

std::vector<std::shared_ptr<AlpacaDriver>> DeviceRegistry::get_all_devices() const {
    std::lock_guard<std::mutex> lock(g_registry_mutex);

    std::vector<std::shared_ptr<AlpacaDriver>> result;
    result.reserve(devices_.size());
    for (const auto& entry : devices_) {
        result.push_back(entry.device);
    }

    return result;
}

std::vector<DeviceCapabilities> DeviceRegistry::get_all_device_capabilities() const {
    std::lock_guard<std::mutex> lock(g_registry_mutex);

    std::vector<DeviceCapabilities> capabilities;
    capabilities.reserve(devices_.size());

    for (const auto& entry : devices_) {
        DeviceCapabilities cap;
        cap.type = entry.type;
        cap.device_number = entry.number;
        cap.name = entry.device->get_name();
        cap.unique_id = entry.device->get_unique_id();
        cap.description = entry.device->get_description();
        cap.driver_info = entry.device->get_driver_info();
        cap.driver_version = entry.device->get_driver_version();
        cap.interface_version = entry.device->get_interface_version();
        capabilities.push_back(cap);
    }

    return capabilities;
}

void DeviceRegistry::clear() {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    devices_.clear();
    ALPACA_LOG_INFO("DeviceRegistry", "Cleared all registered devices");
}

size_t DeviceRegistry::device_count() const {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    return devices_.size();
}

} // namespace alpacacore::management
