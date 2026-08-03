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

#include <alpacacore/video/frame_ring_buffer.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace alpacacore::video {

namespace {
constexpr std::size_t kMinRingBytes = 64ULL * 1024 * 1024;
constexpr std::size_t kMaxRingBytes = 512ULL * 1024 * 1024;
}  // namespace

std::size_t adaptive_ring_bytes(std::uint64_t mem_available_bytes) {
    const std::uint64_t quarter = mem_available_bytes / 4;
    return static_cast<std::size_t>(std::clamp<std::uint64_t>(quarter, kMinRingBytes, kMaxRingBytes));
}

std::uint64_t read_mem_available_bytes() {
    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.rfind("MemAvailable:", 0) == 0) {
            std::istringstream fields(line.substr(13));
            std::uint64_t kib = 0;
            if (fields >> kib) {
                return kib * 1024;
            }
            return 0;
        }
    }
    return 0;
}

FrameRingBuffer::FrameRingBuffer(std::size_t slot_bytes, std::size_t capacity_bytes)
    : slot_bytes_(slot_bytes), slot_count_(std::max<std::size_t>(2, slot_bytes ? capacity_bytes / slot_bytes : 0)) {
    if (slot_bytes_ == 0) {
        throw std::invalid_argument("FrameRingBuffer: slot_bytes must be > 0");
    }
    arena_.resize(slot_bytes_ * slot_count_);
    meta_.resize(slot_count_);
}

std::uint8_t* FrameRingBuffer::begin_write() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || write_open_) {
        return nullptr;
    }
    // Full when every slot is either committed or handed to the reader.
    const std::size_t in_use = queued_ + (read_open_ ? 1 : 0);
    if (in_use >= slot_count_) {
        return nullptr;
    }
    write_open_ = true;
    return arena_.data() + head_ * slot_bytes_;
}

void FrameRingBuffer::commit_write(std::size_t size, std::int64_t timestamp_utc_ticks) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!write_open_) {
        throw std::logic_error("FrameRingBuffer: commit_write without begin_write");
    }
    if (size > slot_bytes_) {
        throw std::invalid_argument("FrameRingBuffer: frame larger than slot");
    }
    meta_[head_] = {size, timestamp_utc_ticks};
    head_ = (head_ + 1) % slot_count_;
    ++queued_;
    write_open_ = false;
    data_available_.notify_one();
}

bool FrameRingBuffer::pop(Frame& out, std::chrono::milliseconds wait) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (read_open_) {
        throw std::logic_error("FrameRingBuffer: pop without release_read");
    }
    if (!data_available_.wait_for(lock, wait, [this] { return queued_ > 0 || closed_; })) {
        return false;
    }
    if (queued_ == 0) {
        return false;  // closed and drained
    }
    out.data = arena_.data() + tail_ * slot_bytes_;
    out.size = meta_[tail_].size;
    out.timestamp_utc_ticks = meta_[tail_].timestamp_utc_ticks;
    read_open_ = true;
    return true;
}

void FrameRingBuffer::release_read() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!read_open_) {
        throw std::logic_error("FrameRingBuffer: release_read without pop");
    }
    tail_ = (tail_ + 1) % slot_count_;
    --queued_;
    read_open_ = false;
}

void FrameRingBuffer::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    data_available_.notify_all();
}

bool FrameRingBuffer::closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

std::size_t FrameRingBuffer::frames_queued() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queued_;
}

}  // namespace alpacacore::video
