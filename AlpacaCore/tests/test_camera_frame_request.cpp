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

// util/camera_frame_request.h (#2261): a table of (request, connected, camera) to what the client sees, the code or
// the validated value. The functions below stand in for a camera driver: they call the module's stages at the points
// of the #2259 order, with a connection check between them. No driver, no fake SDK, no clock. Each text is the name of
// its field, so a row names the check that refused it, never a driver's wording.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/camera_frame_request.h>
#include <alpacacore/util/error_handling.h>

#include <limits>
#include <string>
#include <vector>

#include "catch2_compat.h"

using alpacacore::AlpacaException;
using alpacacore::util::CameraFrame;
using alpacacore::util::CameraRequestTexts;
namespace AlpacaError = alpacacore::AlpacaError;
namespace util = alpacacore::util;

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// What a driver supplies: its connection and the device lookups.
struct Camera {
    bool connected = true;
    long min_us = 32;
    long max_us = 3'600'000'000L;
    std::vector<int> bins{1, 2, 4};
    int sensor_width = 1000;  // unbinned pixels
    int sensor_height = 800;
};

Camera disconnected() {
    Camera camera;
    camera.connected = false;
    return camera;
}

CameraRequestTexts field_names() {
    CameraRequestTexts texts;
    texts.duration_invalid = "duration_invalid";
    texts.duration_overflow = "duration_overflow";
    texts.duration_above_max = "duration_above_max";
    texts.bin_not_positive = "bin_not_positive";
    texts.bin_not_supported = "bin_not_supported";
    texts.size_not_positive = "size_not_positive";
    texts.start_negative = "start_negative";
    texts.frame_size_not_positive = "frame_size_not_positive";
    texts.frame_above_sensor = "frame_above_sensor";
    texts.frame_start_off_sensor = "frame_start_off_sensor";
    texts.frame_beyond_sensor = "frame_beyond_sensor";
    texts.setpoint_not_finite = "setpoint_not_finite";
    texts.setpoint_out_of_bounds = "setpoint_out_of_bounds";
    return texts;
}

const CameraRequestTexts kTexts = field_names();

void ensure_connected(const Camera& camera) {
    if (!camera.connected) {
        throw AlpacaException("not connected", AlpacaError::NotConnected);
    }
}

// StartExposure as a driver calls the stages; returns the microseconds the SDK gets.
long start_exposure(const Camera& camera, double duration, const CameraFrame& frame) {
    const double requested_us = util::check_exposure_duration(duration, kTexts);
    ensure_connected(camera);
    const long exposure_us = util::exposure_microseconds(requested_us, camera.min_us, camera.max_us, kTexts);
    util::check_frame_on_sensor(frame, camera.sensor_width / frame.bin_x, camera.sensor_height / frame.bin_y, kTexts);
    return exposure_us;
}

void set_bin(const Camera& camera, int bin) {
    util::check_bin(bin, kTexts);
    ensure_connected(camera);
    util::check_bin_supported(bin, camera.bins, kTexts);
}

void set_num_x(const Camera& camera, int num_x) {
    util::check_frame_size(num_x, kTexts);
    ensure_connected(camera);
}

void set_start_x(const Camera& camera, int start_x) {
    util::check_frame_start(start_x, kTexts);
    ensure_connected(camera);
}

// What a call answered: its code and the check's field name ("" and 0 when it passed), and the value it returned.
struct Outcome {
    int code = 0;
    std::string check;
    long value = 0;
};

template <typename F>
Outcome outcome_of(F&& call) {
    Outcome outcome;
    try {
        outcome.value = call();
    } catch (const AlpacaException& e) {
        outcome.code = e.error_code();
        outcome.check = e.error_code() == AlpacaError::InvalidValue ? e.what() : "";
    }
    return outcome;
}

CameraFrame full_frame() { return CameraFrame{1, 1, 1000, 800, 0, 0}; }

CameraFrame frame(int bin, int num_x, int num_y, int start_x, int start_y) {
    return CameraFrame{bin, bin, num_x, num_y, start_x, start_y};
}

}  // namespace

TEST_CASE("Camera frame request - StartExposure answers in the order duration, connected, range, frame",
          "[util][camera-frame-request]") {
    struct Row {
        const char* what;
        Camera camera;
        double duration;
        CameraFrame frame;
        int code;           // 0: accepted
        const char* check;  // the InvalidValue check that refused it
        long exposure_us;   // when accepted
    };
    const std::vector<Row> rows = {
        // Stage 1 precedes the connection check.
        {"NaN, disconnected", disconnected(), kNaN, full_frame(), AlpacaError::InvalidValue, "duration_invalid", 0},
        {"-inf, disconnected", disconnected(), -kInf, full_frame(), AlpacaError::InvalidValue, "duration_invalid", 0},
        {"+inf, disconnected", disconnected(), kInf, full_frame(), AlpacaError::InvalidValue, "duration_invalid", 0},
        {"-1 s, disconnected", disconnected(), -1.0, full_frame(), AlpacaError::InvalidValue, "duration_invalid", 0},
        {"1e15 s, disconnected", disconnected(), 1e15, full_frame(), AlpacaError::InvalidValue, "duration_overflow", 0},
        {"1e300 s, connected", Camera{}, 1e300, full_frame(), AlpacaError::InvalidValue, "duration_overflow", 0},
        // The connection precedes the device stages.
        {"1 s, disconnected", disconnected(), 1.0, full_frame(), AlpacaError::NotConnected, "", 0},
        {"above max, disconnected", disconnected(), 4000.0, full_frame(), AlpacaError::NotConnected, "", 0},
        {"bad frame, disconnected", disconnected(), 1.0, frame(1, 2000, 800, 0, 0), AlpacaError::NotConnected, "", 0},
        // Stage 3: rounded to whole microseconds, raised to the minimum, refused above the maximum.
        {"0 s runs at the minimum", Camera{}, 0.0, full_frame(), 0, "", 32},
        {"1 ns runs at the minimum", Camera{}, 1e-9, full_frame(), 0, "", 32},
        {"1 s", Camera{}, 1.0, full_frame(), 0, "", 1'000'000},
        {"1 s and 0.4 us rounds down", Camera{}, 1.0000004, full_frame(), 0, "", 1'000'000},
        {"exactly the maximum", Camera{}, 3600.0, full_frame(), 0, "", 3'600'000'000L},
        {"0.4 us above the maximum rounds to it", Camera{}, 3600.0000004, full_frame(), 0, "", 3'600'000'000L},
        {"1 s above the maximum", Camera{}, 3601.0, full_frame(), AlpacaError::InvalidValue, "duration_above_max", 0},
        {"above the maximum and a bad frame", Camera{}, 3601.0, frame(1, 2000, 800, 0, 0), AlpacaError::InvalidValue,
         "duration_above_max", 0},
        // Stage 4: the frame against the sensor at its bin, in order.
        {"NumX 0", Camera{}, 1.0, frame(1, 0, 800, 0, 0), AlpacaError::InvalidValue, "frame_size_not_positive", 0},
        {"NumY -1", Camera{}, 1.0, frame(1, 1000, -1, 0, 0), AlpacaError::InvalidValue, "frame_size_not_positive", 0},
        {"NumX 0 and StartX off the sensor", Camera{}, 1.0, frame(1, 0, 800, 5000, 0), AlpacaError::InvalidValue,
         "frame_size_not_positive", 0},
        {"NumX above the sensor", Camera{}, 1.0, frame(1, 1001, 800, 0, 0), AlpacaError::InvalidValue,
         "frame_above_sensor", 0},
        {"NumX above the binned sensor", Camera{}, 1.0, frame(2, 501, 400, 0, 0), AlpacaError::InvalidValue,
         "frame_above_sensor", 0},
        {"NumX above the sensor and StartX off it", Camera{}, 1.0, frame(1, 1001, 800, 2000, 0),
         AlpacaError::InvalidValue, "frame_above_sensor", 0},
        {"StartX -1", Camera{}, 1.0, frame(1, 10, 10, -1, 0), AlpacaError::InvalidValue, "frame_start_off_sensor", 0},
        {"StartY at the sensor edge", Camera{}, 1.0, frame(1, 10, 10, 0, 800), AlpacaError::InvalidValue,
         "frame_start_off_sensor", 0},
        {"StartX past INT_MAX - NumX", Camera{}, 1.0, frame(1, 10, 10, std::numeric_limits<int>::max(), 0),
         AlpacaError::InvalidValue, "frame_start_off_sensor", 0},
        {"StartX + NumX one past the edge", Camera{}, 1.0, frame(1, 100, 10, 901, 0), AlpacaError::InvalidValue,
         "frame_beyond_sensor", 0},
        {"StartY + NumY one past the binned edge", Camera{}, 1.0, frame(2, 10, 100, 0, 301), AlpacaError::InvalidValue,
         "frame_beyond_sensor", 0},
        {"a frame that ends at the edge", Camera{}, 1.0, frame(1, 100, 10, 900, 790), 0, "", 1'000'000},
        {"the binned full frame", Camera{}, 1.0, frame(4, 250, 200, 0, 0), 0, "", 1'000'000},
    };
    for (const Row& row : rows) {
        INFO(row.what);
        const Outcome outcome = outcome_of([&] { return start_exposure(row.camera, row.duration, row.frame); });
        CHECK(outcome.code == row.code);
        CHECK(outcome.check == row.check);
        if (row.code == 0) {
            CHECK(outcome.value == row.exposure_us);
        }
    }
}

TEST_CASE("Camera frame request - the geometry setters answer in the order value, connected, supported bin",
          "[util][camera-frame-request]") {
    Camera only_bin_one;
    only_bin_one.bins = {1};
    Camera unknown_bins;
    unknown_bins.bins.clear();
    struct Row {
        const char* what;
        Camera camera;
        int bin;
        int code;
        const char* check;
    };
    const std::vector<Row> bin_rows = {
        {"bin 0, disconnected", disconnected(), 0, AlpacaError::InvalidValue, "bin_not_positive"},
        {"bin -1, connected", Camera{}, -1, AlpacaError::InvalidValue, "bin_not_positive"},
        {"bin 3, disconnected", disconnected(), 3, AlpacaError::NotConnected, ""},
        {"bin 2, disconnected", disconnected(), 2, AlpacaError::NotConnected, ""},
        {"bin 3, connected", Camera{}, 3, AlpacaError::InvalidValue, "bin_not_supported"},
        {"bin 2 on a camera that bins 1 only", only_bin_one, 2, AlpacaError::InvalidValue, "bin_not_supported"},
        {"bin 1 on a camera with no bin list", unknown_bins, 1, AlpacaError::InvalidValue, "bin_not_supported"},
        {"bin 4, connected", Camera{}, 4, 0, ""},
    };
    for (const Row& row : bin_rows) {
        INFO(row.what);
        const Outcome outcome = outcome_of([&] {
            set_bin(row.camera, row.bin);
            return 0L;
        });
        CHECK(outcome.code == row.code);
        CHECK(outcome.check == row.check);
    }

    // NumX/NumY and StartX/StartY have no device stage: a value beyond the sensor is StartExposure's refusal.
    struct GeometryRow {
        const char* what;
        Camera camera;
        int value;
        int size_code;
        const char* size_check;
        int start_code;
        const char* start_check;
    };
    const std::vector<GeometryRow> geometry_rows = {
        {"-1, disconnected", disconnected(), -1, AlpacaError::InvalidValue, "size_not_positive",
         AlpacaError::InvalidValue, "start_negative"},
        {"0, disconnected", disconnected(), 0, AlpacaError::InvalidValue, "size_not_positive",
         AlpacaError::NotConnected, ""},
        {"0, connected", Camera{}, 0, AlpacaError::InvalidValue, "size_not_positive", 0, ""},
        {"5000, disconnected", disconnected(), 5000, AlpacaError::NotConnected, "", AlpacaError::NotConnected, ""},
        {"5000 beyond the sensor, connected", Camera{}, 5000, 0, "", 0, ""},
    };
    for (const GeometryRow& row : geometry_rows) {
        INFO(row.what);
        const Outcome size = outcome_of([&] {
            set_num_x(row.camera, row.value);
            return 0L;
        });
        CHECK(size.code == row.size_code);
        CHECK(size.check == row.size_check);
        const Outcome start = outcome_of([&] {
            set_start_x(row.camera, row.value);
            return 0L;
        });
        CHECK(start.code == row.start_code);
        CHECK(start.check == row.start_check);
    }
}

TEST_CASE("Camera frame request - a frame that fits the sensor has no misfit", "[util][camera-frame-request]") {
    CHECK(util::frame_sensor_misfit(full_frame(), 1000, 800) == nullptr);
    CHECK(util::frame_sensor_misfit(frame(1, 1, 1, 999, 799), 1000, 800) == nullptr);
    CHECK(util::frame_sensor_misfit(frame(1, 1001, 800, 0, 0), 1000, 800, kTexts) == std::string("frame_above_sensor"));
    // The drivers' static_assert: every default text is set, and a cleared one is caught.
    STATIC_REQUIRE(util::all_texts_set(CameraRequestTexts{}));
    CameraRequestTexts cleared;
    cleared.frame_beyond_sensor = nullptr;
    CHECK_FALSE(util::all_texts_set(cleared));
}
namespace {

// SetCCDTemperature as a driver calls the stages: the static check, then the connection.
std::string set_ccd_temperature(const Camera& camera, double setpoint, const util::CcdSetpointBounds& bounds = {}) {
    try {
        util::check_ccd_setpoint(setpoint, bounds, kTexts);
        ensure_connected(camera);
        return "ok";
    } catch (const AlpacaException& e) {
        return std::to_string(e.error_code()) + " " + e.what();
    }
}

}  // namespace

TEST_CASE("Camera frame request - a setpoint that is not finite is InvalidValue before the connection",
          "[util][camera-frame-request]") {
    const std::string not_finite = std::to_string(AlpacaError::InvalidValue) + " setpoint_not_finite";
    for (const double bad : {kNaN, kInf, -kInf}) {
        CHECK(set_ccd_temperature(Camera{}, bad) == not_finite);
        CHECK(set_ccd_temperature(disconnected(), bad) == not_finite);
    }
    // A finite value passes the static check; a disconnected camera then answers NotConnected.
    CHECK(set_ccd_temperature(Camera{}, -10.0) == "ok");
    CHECK(set_ccd_temperature(disconnected(), -10.0) == std::to_string(AlpacaError::NotConnected) + " not connected");
    // Defaults carry no bound: no number is invented for a camera whose SDK reports none.
    CHECK(set_ccd_temperature(Camera{}, -1e6) == "ok");
    CHECK(set_ccd_temperature(Camera{}, 1e6) == "ok");
}

TEST_CASE("Camera frame request - a setpoint outside the vendor's bounds is InvalidValue before the connection",
          "[util][camera-frame-request]") {
    const util::CcdSetpointBounds qhy{-273.15, 60.0};
    const std::string out = std::to_string(AlpacaError::InvalidValue) + " setpoint_out_of_bounds";
    CHECK(set_ccd_temperature(disconnected(), -273.15, qhy) == out);  // at the lower bound: refused
    CHECK(set_ccd_temperature(disconnected(), -300.0, qhy) == out);
    CHECK(set_ccd_temperature(disconnected(), 60.5, qhy) == out);
    CHECK(set_ccd_temperature(disconnected(), 60.0, qhy) ==  // at the upper bound: accepted, then NotConnected
          std::to_string(AlpacaError::NotConnected) + " not connected");
    CHECK(set_ccd_temperature(Camera{}, -273.0, qhy) == "ok");
    // Not finite stays its own text under bounds.
    CHECK(set_ccd_temperature(disconnected(), kNaN, qhy) ==
          std::to_string(AlpacaError::InvalidValue) + " setpoint_not_finite");
}
