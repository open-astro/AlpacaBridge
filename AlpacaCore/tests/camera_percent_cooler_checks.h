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

// Shared assertions for the ICameraV4 PercentCompleted and cooler rules (issue #1477), used by every camera
// driver test so one wording of the rule is pinned for all of them.
//
// - PercentCompleted raises InvalidOperation unless CameraState is Exposing, and is then absent from
//   DeviceState; while Exposing it is an integer 0..100 in both.
// - CoolerOn (get and set) and CoolerPower raise PropertyNotImplemented on a camera without a cooler.
// - LastExposureDuration and LastExposureStartTime raise InvalidOperation until an image has been successfully
//   acquired (issue #1477); a failed exposure never publishes them and never erases an earlier success.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/camera_driver.h>
#include <alpacacore/util/error_handling.h>

#include <cstdint>
#include <functional>
#include <optional>

#include "catch2_compat.h"

namespace alpacacore::test {

inline std::optional<std::int32_t> device_state_percent_completed(const CameraDriver& camera) {
    for (const auto& entry : camera.get_device_state()) {
        if (entry.name == "PercentCompleted") {
            const auto* value = std::get_if<std::int32_t>(&entry.value);
            REQUIRE(value != nullptr);  // present means an integer (ICameraV4 short), never a double
            return *value;
        }
    }
    return std::nullopt;
}

inline void require_error_code(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
    } catch (const AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
        return;
    }
    FAIL("Expected AlpacaException " << expected_code);
}

// No image has been successfully acquired: both LastExposure* getters raise InvalidOperation (ICameraV4), not
// ValueNotSet.
inline void require_last_exposure_unset(const CameraDriver& camera) {
    require_error_code([&] { (void)camera.get_last_exposure_duration(); }, AlpacaError::InvalidOperation);
    require_error_code([&] { (void)camera.get_last_exposure_start_time(); }, AlpacaError::InvalidOperation);
}

// CameraState is not Exposing: the GET path raises InvalidOperation and DeviceState omits the member.
inline void require_percent_completed_unavailable(const CameraDriver& camera) {
    require_error_code([&] { (void)camera.get_percent_completed_checked(); }, AlpacaError::InvalidOperation);
    CHECK_FALSE(device_state_percent_completed(camera).has_value());
}

// CameraState is Exposing: an integer-valued 0..100 in the GET path and in DeviceState.
inline void require_percent_completed_available(const CameraDriver& camera) {
    REQUIRE(camera.get_camera_state() == CameraState::Exposing);
    const double percent = camera.get_percent_completed_checked();
    CHECK(percent >= 0.0);
    CHECK(percent <= 100.0);
    const auto in_state = device_state_percent_completed(camera);
    REQUIRE(in_state.has_value());
    CHECK(*in_state >= 0);
    CHECK(*in_state <= 100);
}

inline void require_no_cooler_members(CameraDriver& camera) {
    require_error_code([&] { (void)camera.get_cooler_on(); }, AlpacaError::PropertyNotImplemented);
    require_error_code([&] { camera.set_cooler_on(true); }, AlpacaError::PropertyNotImplemented);
    require_error_code([&] { camera.set_cooler_on(false); }, AlpacaError::PropertyNotImplemented);
    require_error_code([&] { (void)camera.get_cooler_power(); }, AlpacaError::PropertyNotImplemented);
}

}  // namespace alpacacore::test
