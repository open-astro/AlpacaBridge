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

#include <alpacacore/video/frame_ring_buffer.h>
#include <alpacacore/video/ser_writer.h>
#include <alpacacore/video/video_capture.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace alpacacore::video {

// Snapshot of a recording's live counters (§77.1 honest accounting: no
// silent loss, ever). frames_captured == frames_written + ring drops at all
// times once the recording has stopped.
struct RecorderStats {
    bool running{};
    std::uint64_t frames_captured{};
    std::uint64_t frames_written{};
    std::uint64_t ring_dropped_frames{};  // ring full — disk couldn't keep up
    std::uint64_t sdk_dropped_frames{};   // vendor SDK / USB side
    std::uint64_t bytes_written{};
    double achieved_fps{};  // measured over the whole recording
    std::string error;      // non-empty when the capture died
};

// Drives one SER recording: a capture thread pulls frames from a
// VideoCapture seam straight into the pre-allocated ring, and a drain thread
// writes them out through SerWriter. Capture never blocks on disk — a full
// ring counts a drop and moves on.
class VideoRecorder {
public:
    struct Options {
        SerWriter::Options ser;    // width/height/format filled from the request
        std::size_t ring_bytes{};  // 0 = adaptive_ring_bytes(read_mem_available_bytes())
    };

    explicit VideoRecorder(VideoCapture& source);
    ~VideoRecorder();

    VideoRecorder(const VideoRecorder&) = delete;
    VideoRecorder& operator=(const VideoRecorder&) = delete;

    // Start the source and both worker threads. Throws AlpacaException if
    // already running or the source/writer fails to start.
    void start(const VideoRequest& request, const Options& options);

    // Stop capture, drain the ring to disk, finalize the SER file, join both
    // threads. Safe to call when already stopped.
    void stop();

    RecorderStats stats() const;

private:
    void capture_loop(VideoRequest request);
    void drain_loop();

    VideoCapture& source_;

    mutable std::mutex lifecycle_mutex_;
    std::thread capture_thread_;
    std::thread drain_thread_;
    std::unique_ptr<FrameRingBuffer> ring_;
    std::unique_ptr<SerWriter> writer_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<std::uint64_t> frames_captured_{0};
    std::atomic<std::uint64_t> frames_written_{0};
    std::atomic<std::uint64_t> ring_dropped_{0};
    std::atomic<std::uint64_t> bytes_written_{0};
    std::atomic<std::uint64_t> sdk_dropped_{0};
    std::chrono::steady_clock::time_point capture_start_{};
    std::atomic<std::int64_t> capture_nanos_{0};

    mutable std::mutex error_mutex_;
    std::string error_;
};

}  // namespace alpacacore::video
