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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/video/video_recorder.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "synthetic_video_capture.h"

using alpacacore::video::RecorderStats;
using alpacacore::video::VideoPixelFormat;
using alpacacore::video::VideoRecorder;
using alpacacore::video::VideoRequest;
using alpacacore::video::testing::SyntheticVideoCapture;

namespace {

constexpr std::size_t kHeaderBytes = 178;

std::string temp_path(const char* tag) {
    return (std::filesystem::temp_directory_path() / (std::string("video_recorder_") + tag + ".ser")).string();
}

VideoRequest small_request() {
    VideoRequest request;
    request.width = 32;
    request.height = 16;
    request.format = VideoPixelFormat::Mono8;
    request.exposure_ms = 1;
    return request;
}

void wait_for_captured(SyntheticVideoCapture& source, std::uint64_t count) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (source.produced() < count && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(source.produced() >= count);
}

}  // namespace

TEST_CASE("Video recorder - synthetic bench records every frame with honest accounting", "[video][recorder][unit]") {
    const std::string path = temp_path("bench");
    const std::uint64_t frame_count = 200;
    SyntheticVideoCapture source(frame_count, std::chrono::microseconds(50));
    VideoRecorder recorder(source);

    const VideoRequest request = small_request();
    VideoRecorder::Options options;
    options.ser.path = path;
    options.ser.observer = "Bench";
    options.ring_bytes = 1024 * 1024;

    recorder.start(request, options);
    wait_for_captured(source, frame_count);
    recorder.stop();

    const RecorderStats stats = recorder.stats();
    CHECK_FALSE(stats.running);
    CHECK(stats.error.empty());
    CHECK(stats.frames_captured == frame_count);
    // Honest accounting: every captured frame is either on disk or counted
    // as a drop — nothing silently lost.
    CHECK(stats.frames_written + stats.ring_dropped_frames == stats.frames_captured);
    CHECK(stats.achieved_fps > 0.0);

    const std::size_t frame_size = 32 * 16;
    const auto file_size = std::filesystem::file_size(path);
    CHECK(file_size == kHeaderBytes + stats.frames_written * frame_size + stats.frames_written * 8);
    CHECK(stats.bytes_written == file_size);

    std::remove(path.c_str());
}

TEST_CASE("Video recorder - frame payloads survive the ring into the SER file", "[video][recorder][unit]") {
    const std::string path = temp_path("payload");
    const std::uint64_t frame_count = 20;
    // No pacing delay + generous ring: all frames retained, order preserved.
    SyntheticVideoCapture source(frame_count, std::chrono::microseconds(0));
    VideoRecorder recorder(source);

    const VideoRequest request = small_request();
    VideoRecorder::Options options;
    options.ser.path = path;
    options.ring_bytes = 1024 * 1024;

    recorder.start(request, options);
    wait_for_captured(source, frame_count);
    recorder.stop();

    const RecorderStats stats = recorder.stats();
    REQUIRE(stats.error.empty());
    REQUIRE(stats.frames_written == frame_count);

    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::size_t frame_size = 32 * 16;
    // Synthetic frames are filled with their index (mod 251) — verify order.
    for (std::uint64_t i = 0; i < frame_count; ++i) {
        const std::size_t start = kHeaderBytes + static_cast<std::size_t>(i) * frame_size;
        REQUIRE(bytes[start] == static_cast<std::uint8_t>(i % 251));
        REQUIRE(bytes[start + frame_size - 1] == static_cast<std::uint8_t>(i % 251));
    }

    std::remove(path.c_str());
}

TEST_CASE("Video recorder - reports SDK-side drops from the source", "[video][recorder][unit]") {
    const std::string path = temp_path("sdkdrops");
    SyntheticVideoCapture source(10, std::chrono::microseconds(0));
    source.set_sdk_dropped(7);
    VideoRecorder recorder(source);

    VideoRecorder::Options options;
    options.ser.path = path;
    options.ring_bytes = 1024 * 1024;

    recorder.start(small_request(), options);
    wait_for_captured(source, 10);
    recorder.stop();

    CHECK(recorder.stats().sdk_dropped_frames == 7);
    std::remove(path.c_str());
}

TEST_CASE("Video recorder - double start throws, stop is idempotent", "[video][recorder][unit]") {
    const std::string path = temp_path("lifecycle");
    SyntheticVideoCapture source(5, std::chrono::microseconds(0));
    VideoRecorder recorder(source);

    VideoRecorder::Options options;
    options.ser.path = path;
    options.ring_bytes = 1024 * 1024;

    recorder.start(small_request(), options);
    CHECK_THROWS_AS(recorder.start(small_request(), options), alpacacore::AlpacaException);
    recorder.stop();
    recorder.stop();  // idempotent
    CHECK_FALSE(recorder.stats().running);

    std::remove(path.c_str());
}

TEST_CASE("Video recorder - start/stop storm survives", "[video][recorder][stress]") {
    const std::string path = temp_path("storm");
    for (int i = 0; i < 20; ++i) {
        SyntheticVideoCapture source(0, std::chrono::microseconds(10));
        VideoRecorder recorder(source);
        VideoRecorder::Options options;
        options.ser.path = path;
        options.ring_bytes = 256 * 1024;
        recorder.start(small_request(), options);
        std::this_thread::sleep_for(std::chrono::milliseconds(i % 5));
        recorder.stop();
        const RecorderStats stats = recorder.stats();
        CHECK(stats.error.empty());
        CHECK(stats.frames_written + stats.ring_dropped_frames == stats.frames_captured);
    }
    std::remove(path.c_str());
}
