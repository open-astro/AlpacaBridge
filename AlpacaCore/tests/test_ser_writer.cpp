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
#include <alpacacore/video/ser_writer.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "catch2_compat.h"

using alpacacore::AlpacaException;
using alpacacore::video::SerWriter;
using alpacacore::video::utc_ticks_from_unix_nanos;
using alpacacore::video::VideoPixelFormat;

namespace {

constexpr std::size_t kHeaderBytes = 178;

std::string temp_ser_path(const char* tag) {
    return (std::filesystem::temp_directory_path() / (std::string("ser_writer_") + tag + ".ser")).string();
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::int32_t get_i32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::int32_t>(bytes[offset]) | (static_cast<std::int32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::int32_t>(bytes[offset + 2]) << 16) | (static_cast<std::int32_t>(bytes[offset + 3]) << 24);
}

std::int64_t get_i64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::int64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | bytes[offset + static_cast<std::size_t>(i)];
    }
    return value;
}

}  // namespace

TEST_CASE("SER writer - header, frames, and timestamp trailer round-trip", "[video][ser][unit]") {
    const std::string path = temp_ser_path("roundtrip");
    const int width = 8;
    const int height = 6;
    const std::size_t frame_size = static_cast<std::size_t>(width) * height;

    SerWriter::Options options;
    options.path = path;
    options.width = width;
    options.height = height;
    options.format = VideoPixelFormat::Mono8;
    options.observer = "Bench";
    options.instrument = "SyntheticCam";
    options.telescope = "TestScope";

    const std::int64_t t0 = utc_ticks_from_unix_nanos(1'700'000'000'000'000'000LL);
    {
        SerWriter writer(options);
        std::vector<std::uint8_t> frame(frame_size);
        for (std::uint8_t i = 0; i < 3; ++i) {
            std::fill(frame.begin(), frame.end(), static_cast<std::uint8_t>(0x10 + i));
            writer.write_frame(frame.data(), frame.size(), t0 + i);
        }
        writer.finalize();
        CHECK(writer.frames_written() == 3);
    }

    const auto bytes = read_file(path);
    REQUIRE(bytes.size() == kHeaderBytes + 3 * frame_size + 3 * 8);

    CHECK(std::string(bytes.begin(), bytes.begin() + 14) == "LUCAM-RECORDER");
    CHECK(get_i32(bytes, 18) == 0);  // ColorID MONO
    CHECK(get_i32(bytes, 26) == width);
    CHECK(get_i32(bytes, 30) == height);
    CHECK(get_i32(bytes, 34) == 8);  // PixelDepthPerPlane
    CHECK(get_i32(bytes, 38) == 3);  // FrameCount patched at finalize
    CHECK(std::string(bytes.begin() + 42, bytes.begin() + 47) == "Bench");
    CHECK(get_i64(bytes, 170) > 0);  // DateTimeUTC stamped

    // Frame payloads, in order.
    for (std::size_t i = 0; i < 3; ++i) {
        const std::size_t start = kHeaderBytes + i * frame_size;
        CHECK(bytes[start] == 0x10 + i);
        CHECK(bytes[start + frame_size - 1] == 0x10 + i);
    }

    // Timestamp trailer: one int64 of UTC ticks per frame, in order.
    const std::size_t trailer = kHeaderBytes + 3 * frame_size;
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(get_i64(bytes, trailer + i * 8) == t0 + static_cast<std::int64_t>(i));
    }

    std::remove(path.c_str());
}

TEST_CASE("SER writer - 16-bit Bayer format stamps ColorID and depth", "[video][ser][unit]") {
    const std::string path = temp_ser_path("raw16");

    SerWriter::Options options;
    options.path = path;
    options.width = 4;
    options.height = 2;
    options.format = VideoPixelFormat::BayerRggb16;

    {
        SerWriter writer(options);
        REQUIRE(writer.frame_size() == 4 * 2 * 2);
        std::vector<std::uint8_t> frame(writer.frame_size(), 0xAB);
        writer.write_frame(frame.data(), frame.size(), 1);
        writer.finalize();
    }

    const auto bytes = read_file(path);
    CHECK(get_i32(bytes, 18) == 8);   // ColorID BAYER_RGGB
    CHECK(get_i32(bytes, 34) == 16);  // PixelDepthPerPlane
    CHECK(get_i32(bytes, 38) == 1);

    std::remove(path.c_str());
}

TEST_CASE("SER writer - large frames cross the staging boundary intact", "[video][ser][unit]") {
    const std::string path = temp_ser_path("staging");

    SerWriter::Options options;
    options.path = path;
    options.width = 512;
    options.height = 512;
    options.format = VideoPixelFormat::Mono16;
    options.staging_bytes = 64 * 1024;  // frame (512 KiB) >> staging (64 KiB)

    const std::size_t frame_size = 512 * 512 * 2;
    {
        SerWriter writer(options);
        std::vector<std::uint8_t> frame(frame_size);
        for (std::size_t i = 0; i < frame.size(); ++i) {
            frame[i] = static_cast<std::uint8_t>(i % 251);
        }
        writer.write_frame(frame.data(), frame.size(), 7);
        writer.finalize();
        CHECK(writer.bytes_written() >= kHeaderBytes + frame_size);
    }

    const auto bytes = read_file(path);
    REQUIRE(bytes.size() == kHeaderBytes + frame_size + 8);
    for (std::size_t i = 0; i < frame_size; i += 4093) {  // prime stride sample
        REQUIRE(bytes[kHeaderBytes + i] == static_cast<std::uint8_t>(i % 251));
    }

    std::remove(path.c_str());
}

TEST_CASE("SER writer - rejects mismatched frame size and write-after-finalize", "[video][ser][unit]") {
    const std::string path = temp_ser_path("errors");

    SerWriter::Options options;
    options.path = path;
    options.width = 8;
    options.height = 8;
    options.format = VideoPixelFormat::Mono8;

    SerWriter writer(options);
    std::vector<std::uint8_t> wrong(63);
    CHECK_THROWS_AS(writer.write_frame(wrong.data(), wrong.size(), 1), AlpacaException);

    std::vector<std::uint8_t> right(64);
    writer.write_frame(right.data(), right.size(), 1);
    writer.finalize();
    CHECK_THROWS_AS(writer.write_frame(right.data(), right.size(), 2), AlpacaException);
    writer.finalize();  // idempotent

    std::remove(path.c_str());
}
