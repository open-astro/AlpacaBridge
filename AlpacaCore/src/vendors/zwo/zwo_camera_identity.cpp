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

#include <alpacacore/vendor/zwo/zwo_camera_identity.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>

namespace alpacacore::vendor::zwo {

std::string trim_zwo_name(const std::string& name) {
    std::string::size_type end = name.size();
    while (end > 0 && (name[end - 1] == ' ' || name[end - 1] == '\0')) {
        --end;
    }
    return name.substr(0, end);
}

namespace {

ZwoResolveResult found_camera(const ZwoEnumeratedCamera& camera) {
    ZwoResolveResult result;
    result.camera = camera;
    return result;
}

ZwoResolveResult failed(ZwoResolveFailure failure, std::string message) {
    ZwoResolveResult result;
    result.failure = failure;
    result.message = std::move(message);
    return result;
}

}  // namespace

ZwoResolveResult resolve_zwo_camera(const ZwoConfiguredIdentity& entry, const std::vector<ZwoEnumeratedCamera>& found,
                                    const std::set<std::string>& serials_claimed_by_other_entries) {
    if (found.empty()) {
        return failed(ZwoResolveFailure::NoCameras, "No ZWO cameras detected");
    }

    if (!entry.serial.empty()) {
        for (const auto& camera : found) {
            if (camera.serial == entry.serial) {
                return found_camera(camera);
            }
        }
        return failed(ZwoResolveFailure::NotFound, "ZWO camera serial " + entry.serial + " not found");
    }

    const std::string wanted_name = trim_zwo_name(entry.camera_name);
    if (!wanted_name.empty()) {
        const ZwoEnumeratedCamera* first = nullptr;
        for (const auto& camera : found) {
            if (trim_zwo_name(camera.name) != wanted_name) {
                continue;
            }
            if (!camera.serial.empty() && serials_claimed_by_other_entries.count(camera.serial) != 0) {
                continue;
            }
            if (entry.camera_index.has_value() && camera.index == entry.camera_index.value()) {
                return found_camera(camera);
            }
            if (first == nullptr) {
                first = &camera;
            }
        }
        if (first != nullptr) {
            return found_camera(*first);
        }
        return failed(ZwoResolveFailure::NotFound, "ZWO camera " + wanted_name + " not found");
    }

    // An entry saved before serial binding: the id, then the index, as before.
    // A camera whose serial another entry claims is never taken here: the hint
    // may point at it after the enumeration order flipped.
    const auto claimed = [&](const ZwoEnumeratedCamera& camera) {
        return !camera.serial.empty() && serials_claimed_by_other_entries.count(camera.serial) != 0;
    };
    if (entry.camera_id.has_value()) {
        for (const auto& camera : found) {
            if (camera.camera_id == entry.camera_id.value() && !claimed(camera)) {
                return found_camera(camera);
            }
        }
        return failed(ZwoResolveFailure::NotFound,
                      "ZWO camera ID " + std::to_string(entry.camera_id.value()) + " not found");
    }
    if (entry.camera_index.has_value()) {
        const int index = entry.camera_index.value();
        if (index < 0 || index >= static_cast<int>(found.size())) {
            return failed(ZwoResolveFailure::IndexOutOfRange, "Camera index not found");
        }
        if (claimed(found[static_cast<std::size_t>(index)])) {
            return failed(ZwoResolveFailure::NotFound,
                          "ZWO camera at index " + std::to_string(index) + " belongs to another device entry");
        }
        return found_camera(found[static_cast<std::size_t>(index)]);
    }
    return failed(ZwoResolveFailure::NotFound, "Camera ID not specified");
}

std::string generate_zwo_unique_id() {
    std::random_device rd;
    const std::uint64_t value = (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    return std::string("ZWO_UID_") + buffer;
}

std::string zwo_unique_id(const std::string& serial, const std::string& stored_unique_id) {
    if (!serial.empty()) {
        return "ZWO_SN_" + serial;
    }
    return stored_unique_id;
}

}  // namespace alpacacore::vendor::zwo
