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

#include <alpacacore/video/video_capture.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace alpacacore::video {

// Convert a system-clock time point already expressed as nanoseconds since
// the Unix epoch to .NET ticks (100 ns units since 0001-01-01 UTC) — the
// timestamp unit SER files use.
std::int64_t utc_ticks_from_unix_nanos(std::int64_t unix_nanos);

// Current UTC time as .NET ticks.
std::int64_t utc_ticks_now();

// Sequential SER (v3) file writer for the §77.1 drain thread. Writes the
// 178-byte header, raw frames, and the optional per-frame UTC timestamp
// trailer. On Linux the file is opened O_DIRECT and all frame data flows
// through an aligned staging buffer in full 4 KiB blocks, bypassing the page
// cache — writeback caching would eat all free RAM on a 2 GB box mid-capture
// (§77.1). Not thread-safe; owned and driven by a single drain thread.
class SerWriter {
public:
    struct Options {
        std::string path;
        int width{};
        int height{};
        VideoPixelFormat format{VideoPixelFormat::Mono8};
        std::string observer;
        std::string instrument;
        std::string telescope;
        std::size_t staging_bytes{4ULL * 1024 * 1024};
    };

    explicit SerWriter(const Options& options);
    ~SerWriter();

    SerWriter(const SerWriter&) = delete;
    SerWriter& operator=(const SerWriter&) = delete;

    // Append one frame (size must equal frame_bytes for the configured
    // geometry). Throws AlpacaException on I/O failure.
    void write_frame(const std::uint8_t* data, std::size_t size, std::int64_t timestamp_utc_ticks);

    // Flush the staging tail, append the timestamp trailer, patch the
    // header's FrameCount, and close the file. Idempotent.
    void finalize();

    std::uint64_t frames_written() const { return frames_written_; }
    std::uint64_t bytes_written() const { return bytes_written_; }
    std::size_t frame_size() const { return frame_size_; }

private:
    void flush_full_blocks();
    void write_all_direct(const std::uint8_t* data, std::size_t size);

    Options options_;
    std::size_t frame_size_;
    int fd_{-1};
    bool finalized_{false};
    std::uint8_t* staging_{};
    std::size_t staging_capacity_{};
    std::size_t staging_used_{};
    std::uint64_t frames_written_{};
    std::uint64_t bytes_written_{};
    std::vector<std::int64_t> timestamps_;
};

// Measure the sustained sequential write rate of the filesystem holding
// directory, in bytes/second, by writing (and deleting) a temporary file of
// test_bytes with the same direct-I/O path SerWriter uses. Used for the
// §77.1 honest-accounting check at capture start. Returns 0 on failure.
std::uint64_t measure_disk_write_rate(const std::string& directory, std::size_t test_bytes);

}  // namespace alpacacore::video
