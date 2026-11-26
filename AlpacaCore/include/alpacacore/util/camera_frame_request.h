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

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

// The camera frame request (#2261): the StartExposure and geometry checks every camera driver shares, in the one
// order of the ASCOM contract topic file ("Camera argument order", #2259). Each stage is its own function, and a
// driver calls them at the points of that order:
//
//   StartExposure: check_exposure_duration()        (1) InvalidValue, before the connection
//                  the driver's connection check    (2) NotConnected
//                  exposure_microseconds()          (3) raised to ExposureMin, InvalidValue above ExposureMax
//                  check_frame_on_sensor()          (4) InvalidValue
//                  the driver's exposing check      (5) InvalidOperation (the CaptureSession reservation, #2260)
//
//   BinX/BinY:     check_bin()                      (1) InvalidValue, before the connection
//                  the driver's connection check    (2) NotConnected
//                  check_bin_supported()            (3) InvalidValue
//                  the driver's exposing check      (4) InvalidOperation
//
//   SetCCDTemperature: check_ccd_setpoint()          (1) InvalidValue, before the connection
//                  the driver's connection check    (2) NotConnected
//                  the driver's device range        (3) InvalidValue (ZWO/SVBONY control caps, Player One)
//
//   NumX/NumY:     check_frame_size(), StartX/StartY: check_frame_start(), then the connection and the exposing
//                  checks. These setters have no device stage: StartExposure checks the frame against the sensor.
//
// What the module owns: the checks above, their order inside each stage, and the client's values (CameraFrame is
// the frame as asked, never aligned). What a driver supplies: its connection and exposing checks, the device range
// lookups (ExposureMin and ExposureMax in microseconds, the supported bins, the sensor size at the current bin),
// every SDK alignment rule (width multiples, even offsets, the ToupTek even span), every vendor limit (GPhoto's
// 3600 s cap, its nearest shutter speed, the iGuider exposure steps) and the InvalidValue texts: the codes are the
// same on every camera, and each driver keeps its own words (CameraRequestTexts).
//
// Free functions and plain values: no base class, no virtual, no lock, no SDK call and no state (decision 0013
// rule 5, decision 0016). A driver calls them under its own lock or before taking it; the module never decides
// whether the camera is connected or exposing.

namespace alpacacore::util {

/// The frame a client asked for: bin, NumX, NumY, StartX and StartY as written, in binned pixels. A driver aligns a
/// copy for its SDK; these values are what the client reads back.
struct CameraFrame {
    int bin_x = 1;
    int bin_y = 1;
    int num_x = 0;
    int num_y = 0;
    int start_x = 0;
    int start_y = 0;
};

/// The InvalidValue texts of the checks, in the driver's own words. Two fields may hold one text. Each field points
/// at a string literal (static storage): the module keeps the pointer, never a copy.
struct CameraRequestTexts {
    const char* duration_invalid = "Exposure duration must be finite and non-negative";  ///< NaN, infinite, negative
    const char* duration_overflow = "Exposure duration is out of range";                 ///< too large for microseconds
    const char* duration_above_max = "Exposure duration out of range";                   ///< above ExposureMax
    const char* bin_not_positive = "Bin value must be positive";
    const char* bin_not_supported = "Bin value not supported";
    const char* size_not_positive = "ROI size must be positive";
    const char* start_negative = "Start position must be non-negative";
    // StartExposure's frame check, in its order.
    const char* frame_size_not_positive = "ROI not valid";                        ///< NumX or NumY of 0 or less
    const char* frame_above_sensor = "ROI size exceeds sensor dimensions";        ///< NumX or NumY above the sensor
    const char* frame_start_off_sensor = "Start position outside sensor bounds";  ///< StartX/StartY off the sensor
    const char* frame_beyond_sensor = "ROI extends beyond sensor bounds";  ///< StartX + NumX past the sensor edge
    // SetCCDTemperature's static check.
    const char* setpoint_not_finite = "Target temperature must be finite";      ///< NaN or infinite
    const char* setpoint_out_of_bounds = "Target temperature is out of range";  ///< outside CcdSetpointBounds
};

/// Whether every text is set: a driver's kRequestTexts holds it in a static_assert, so a field cleared by mistake
/// fails the build instead of throwing an exception built from a null text.
constexpr bool all_texts_set(const CameraRequestTexts& texts) {
    return texts.duration_invalid != nullptr && texts.duration_overflow != nullptr &&
           texts.duration_above_max != nullptr && texts.bin_not_positive != nullptr &&
           texts.bin_not_supported != nullptr && texts.size_not_positive != nullptr &&
           texts.start_negative != nullptr && texts.frame_size_not_positive != nullptr &&
           texts.frame_above_sensor != nullptr && texts.frame_start_off_sensor != nullptr &&
           texts.frame_beyond_sensor != nullptr && texts.setpoint_not_finite != nullptr &&
           texts.setpoint_out_of_bounds != nullptr;
}

namespace camera_request_detail {

[[noreturn]] inline void refuse(const char* text) { throw AlpacaException(text, AlpacaError::InvalidValue); }

}  // namespace camera_request_detail

// --- StartExposure ---

/// Stage 1, before the connection check: a NaN, infinite or negative duration, or one whose microseconds do not fit
/// a long, is InvalidValue. Returns the requested duration in microseconds, unrounded.
inline double check_exposure_duration(double duration_s, const CameraRequestTexts& texts = {}) {
    if (!std::isfinite(duration_s) || duration_s < 0.0) {
        camera_request_detail::refuse(texts.duration_invalid);
    }
    const double requested_us = duration_s * 1'000'000.0;
    if (!std::isfinite(requested_us) || requested_us >= static_cast<double>(std::numeric_limits<long>::max())) {
        camera_request_detail::refuse(texts.duration_overflow);
    }
    return requested_us;
}

/// Stage 3, connected: the microseconds the SDK gets. The requested value is rounded, a value below the camera's
/// minimum (0, a bias frame, included) is raised to it, and one above the maximum is InvalidValue.
inline long exposure_microseconds(double requested_us, long min_us, long max_us, const CameraRequestTexts& texts = {}) {
    const long exposure_us = std::max(std::lround(requested_us), min_us);
    if (exposure_us > max_us) {
        camera_request_detail::refuse(texts.duration_above_max);
    }
    return exposure_us;
}

/// The first of the frame's misfits with the sensor, in stage 4's order, as its text; nullptr when the frame fits.
/// The sensor size is at the frame's bin (binned pixels). The frame's bin itself is the driver's: a bin is checked
/// against the supported list when it is written.
inline const char* frame_sensor_misfit(const CameraFrame& frame, int sensor_width, int sensor_height,
                                       const CameraRequestTexts& texts = {}) {
    if (frame.num_x <= 0 || frame.num_y <= 0) {
        return texts.frame_size_not_positive;
    }
    if (frame.num_x > sensor_width || frame.num_y > sensor_height) {
        return texts.frame_above_sensor;
    }
    if (frame.start_x < 0 || frame.start_y < 0 || frame.start_x >= sensor_width || frame.start_y >= sensor_height) {
        return texts.frame_start_off_sensor;
    }
    if (frame.num_x > sensor_width - frame.start_x || frame.num_y > sensor_height - frame.start_y) {
        return texts.frame_beyond_sensor;
    }
    return nullptr;
}

/// Stage 4, connected: a frame that does not fit the sensor is InvalidValue.
inline void check_frame_on_sensor(const CameraFrame& frame, int sensor_width, int sensor_height,
                                  const CameraRequestTexts& texts = {}) {
    if (const char* misfit = frame_sensor_misfit(frame, sensor_width, sensor_height, texts)) {
        camera_request_detail::refuse(misfit);
    }
}

// --- SetCCDTemperature ---

/// The static bounds of a setpoint in degrees C, as vendor data: InvalidValue at or below `above` and above
/// `at_most`. The default has none. Never invent a bound: a driver passes one only where its SDK or the device
/// reports it (QHY: above absolute zero, at most 60).
struct CcdSetpointBounds {
    double above = -std::numeric_limits<double>::infinity();
    double at_most = std::numeric_limits<double>::infinity();
};

/// SetCCDTemperature, before the connection check: a NaN or infinite setpoint, or one outside the static bounds, is
/// InvalidValue. A range only the connected device knows (the SDK's control caps, Player One's target_temp_min/max)
/// is the driver's, checked after the connection check.
inline void check_ccd_setpoint(double temperature, const CcdSetpointBounds& bounds = {},
                               const CameraRequestTexts& texts = {}) {
    if (!std::isfinite(temperature)) {
        camera_request_detail::refuse(texts.setpoint_not_finite);
    }
    if (temperature <= bounds.above || temperature > bounds.at_most) {
        camera_request_detail::refuse(texts.setpoint_out_of_bounds);
    }
}

// --- Geometry setters ---

/// BinX/BinY stage 1, before the connection check: a bin of 0 or less is InvalidValue.
inline void check_bin(int bin, const CameraRequestTexts& texts = {}) {
    if (bin <= 0) {
        camera_request_detail::refuse(texts.bin_not_positive);
    }
}

/// BinX/BinY stage 3, connected: a bin the camera does not list is InvalidValue. An empty list (a camera whose
/// properties are not known) supports none.
inline void check_bin_supported(int bin, std::span<const int> supported, const CameraRequestTexts& texts = {}) {
    if (std::find(supported.begin(), supported.end(), bin) == supported.end()) {
        camera_request_detail::refuse(texts.bin_not_supported);
    }
}

/// NumX/NumY, before the connection check: a size of 0 or less is InvalidValue. No device stage: a size beyond the
/// sensor is StartExposure's refusal (ConformU's "Reject Bad XSize/YSize").
inline void check_frame_size(int size, const CameraRequestTexts& texts = {}) {
    if (size <= 0) {
        camera_request_detail::refuse(texts.size_not_positive);
    }
}

/// StartX/StartY, before the connection check: a start below 0 is InvalidValue. No device stage, as for NumX/NumY.
inline void check_frame_start(int start, const CameraRequestTexts& texts = {}) {
    if (start < 0) {
        camera_request_detail::refuse(texts.start_negative);
    }
}

}  // namespace alpacacore::util
