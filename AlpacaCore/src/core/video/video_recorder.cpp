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
#include <alpacacore/util/logging.h>
#include <alpacacore/video/video_recorder.h>

#include <exception>
#include <string>
#include <vector>

namespace alpacacore::video {

namespace {
constexpr const char* kComponent = "VideoRecorder";
constexpr int kFrameWaitSlackMs = 500;
constexpr std::chrono::milliseconds kDrainPollInterval{100};
}  // namespace

VideoRecorder::VideoRecorder(VideoCapture& source) : source_(source) {}

VideoRecorder::~VideoRecorder() {
    try {
        stop();
    } catch (...) {
        // Destructor must not throw; stop() is the reporting path.
    }
}

void VideoRecorder::start(const VideoRequest& request, const Options& options) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (running_.load()) {
        throw AlpacaException("VideoRecorder: already recording", AlpacaError::InvalidOperation);
    }

    const std::size_t slot = frame_bytes(request);
    if (slot == 0) {
        throw AlpacaException("VideoRecorder: invalid frame geometry", AlpacaError::InvalidValue);
    }
    std::size_t ring_bytes = options.ring_bytes;
    if (ring_bytes == 0) {
        ring_bytes = adaptive_ring_bytes(read_mem_available_bytes());
    }

    SerWriter::Options ser = options.ser;
    ser.width = request.width;
    ser.height = request.height;
    ser.format = request.format;

    frames_captured_.store(0);
    frames_written_.store(0);
    ring_dropped_.store(0);
    bytes_written_.store(0);
    sdk_dropped_.store(0);
    capture_nanos_.store(0);
    {
        std::lock_guard<std::mutex> error_lock(error_mutex_);
        error_.clear();
    }
    stop_requested_.store(false);

    ring_ = std::make_unique<FrameRingBuffer>(slot, ring_bytes);
    writer_ = std::make_unique<SerWriter>(ser);

    source_.start(request);
    running_.store(true);
    capture_start_ = std::chrono::steady_clock::now();
    try {
        capture_thread_ = std::thread([this, request] { capture_loop(request); });
        drain_thread_ = std::thread([this] { drain_loop(); });
    } catch (...) {
        // Thread spawn failed: unwind to a fully-stopped state.
        stop_requested_.store(true);
        ring_->close();
        if (capture_thread_.joinable()) {
            capture_thread_.join();
        }
        source_.stop();
        running_.store(false);
        throw;
    }
    ALPACA_LOG_INFO(kComponent, "recording started: " + ser.path);
}

void VideoRecorder::capture_loop(VideoRequest request) {
    const int wait_ms = request.exposure_ms * 2 + kFrameWaitSlackMs;
    // Pre-allocated once: the drop path must not allocate at frame rate.
    std::vector<std::uint8_t> discard(ring_->slot_bytes());
    try {
        while (!stop_requested_.load()) {
            std::uint8_t* slot = ring_->begin_write();
            if (slot == nullptr) {
                // Ring full: pull the frame anyway so the SDK's own buffer
                // doesn't overflow, then honestly count it as dropped.
                if (source_.get_frame(discard.data(), discard.size(), wait_ms)) {
                    frames_captured_.fetch_add(1);
                    ring_dropped_.fetch_add(1);
                }
                continue;
            }
            if (!source_.get_frame(slot, ring_->slot_bytes(), wait_ms)) {
                continue;  // timeout; re-check stop flag
            }
            frames_captured_.fetch_add(1);
            ring_->commit_write(ring_->slot_bytes(), utc_ticks_now());
        }
    } catch (const std::exception& ex) {
        std::lock_guard<std::mutex> lock(error_mutex_);
        error_ = ex.what();
        ALPACA_LOG_ERROR(kComponent, std::string("capture failed: ") + ex.what());
    }
    capture_nanos_.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - capture_start_)
            .count());
    sdk_dropped_.store(source_.dropped_frames());
    ring_->close();
}

void VideoRecorder::drain_loop() {
    try {
        FrameRingBuffer::Frame frame;
        while (ring_->pop(frame, kDrainPollInterval) || !ring_->closed() || ring_->frames_queued() > 0) {
            if (frame.data == nullptr) {
                continue;
            }
            writer_->write_frame(frame.data, frame.size, frame.timestamp_utc_ticks);
            frames_written_.fetch_add(1);
            bytes_written_.store(writer_->bytes_written());
            ring_->release_read();
            frame = {};
        }
    } catch (const std::exception& ex) {
        std::lock_guard<std::mutex> lock(error_mutex_);
        if (error_.empty()) {
            error_ = ex.what();
        }
        ALPACA_LOG_ERROR(kComponent, std::string("drain failed: ") + ex.what());
    }
}

void VideoRecorder::stop() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!running_.load()) {
        return;
    }
    stop_requested_.store(true);
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
    source_.stop();
    if (drain_thread_.joinable()) {
        drain_thread_.join();
    }
    if (writer_) {
        try {
            writer_->finalize();
            bytes_written_.store(writer_->bytes_written());
        } catch (const std::exception& ex) {
            std::lock_guard<std::mutex> error_lock(error_mutex_);
            if (error_.empty()) {
                error_ = ex.what();
            }
            ALPACA_LOG_ERROR(kComponent, std::string("finalize failed: ") + ex.what());
        }
    }
    running_.store(false);
    ALPACA_LOG_INFO(kComponent, "recording stopped: captured " + std::to_string(frames_captured_.load()) +
                                    ", written " + std::to_string(frames_written_.load()) + ", ring-dropped " +
                                    std::to_string(ring_dropped_.load()) + ", sdk-dropped " +
                                    std::to_string(sdk_dropped_.load()));
}

RecorderStats VideoRecorder::stats() const {
    RecorderStats out;
    out.running = running_.load();
    out.frames_captured = frames_captured_.load();
    out.frames_written = frames_written_.load();
    out.ring_dropped_frames = ring_dropped_.load();
    out.sdk_dropped_frames = sdk_dropped_.load();
    out.bytes_written = bytes_written_.load();
    const std::int64_t nanos = capture_nanos_.load();
    if (nanos > 0) {
        out.achieved_fps = static_cast<double>(out.frames_captured) / (static_cast<double>(nanos) / 1e9);
    }
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        out.error = error_;
    }
    return out;
}

}  // namespace alpacacore::video
