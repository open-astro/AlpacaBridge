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

#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/qhy/qhy_sdk_wrapper.h>

#include <chrono>
#include <sstream>
#include <utility>

namespace alpacacore::vendor::qhy::detail {

inline std::string sdk_result(uint32_t result) {
    std::ostringstream out;
    out << result << " (0x" << std::hex << result << ")";
    return out.str();
}

// Callers serialize the scan. Callables let tests exercise this exact path without loading libqhyccd (#321).
template <typename Scan, typename GetId, typename GetModel>
std::vector<QHYCameraInfo> enumerate_cameras(Scan scan, GetId get_id, GetModel get_model, std::size_t logical_handles) {
    const auto started = std::chrono::steady_clock::now();
    const uint32_t count = scan();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    const std::string summary = "ScanQHYCCD returned " + sdk_result(count) + " in " + std::to_string(elapsed.count()) +
                                " ms; logical handle entries=" + std::to_string(logical_handles);
    // Preserve the existing empty-list behavior; a failed scan is not evidence that a resource reset is safe.
    if (count > 64) {
        ALPACA_LOG_WARN("QHY", summary + "; invalid scan result (not a zero-camera scan)");
        return {};
    }
    ALPACA_LOG_DEBUG("QHY", summary);

    std::vector<QHYCameraInfo> cameras;
    cameras.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        char id[64] = {};
        const uint32_t result = get_id(i, id);
        if (result != 0) {
            ALPACA_LOG_WARN("QHY", "GetQHYCCDId(" + std::to_string(i) + ") failed: " + sdk_result(result));
            continue;
        }
        id[sizeof(id) - 1] = '\0';
        QHYCameraInfo info;
        info.camera_id = id;
        char model[64] = {};
        const uint32_t model_result = get_model(id, model);
        if (model_result == 0) {
            model[sizeof(model) - 1] = '\0';
            info.model = model;
        } else {
            ALPACA_LOG_DEBUG("QHY", "GetQHYCCDModel for scan index " + std::to_string(i) +
                                        " failed: " + sdk_result(model_result) + "; using camera ID as model");
            info.model = info.camera_id;
        }
        cameras.push_back(std::move(info));
    }
    ALPACA_LOG_DEBUG("QHY", "QHY enumeration produced " + std::to_string(cameras.size()) + " usable camera IDs from " +
                                std::to_string(count) + " scan entries");
    return cameras;
}

}  // namespace alpacacore::vendor::qhy::detail
