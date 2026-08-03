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

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace alpacacore::video {

// Ring capacity per playbook §77.1: clamp(mem_available / 4, 64 MB, 512 MB).
std::size_t adaptive_ring_bytes(std::uint64_t mem_available_bytes);

// MemAvailable from /proc/meminfo, in bytes. Returns 0 when unknown (non-Linux
// dev hosts), which callers treat as "use the 64 MB floor".
std::uint64_t read_mem_available_bytes();

// Single-producer / single-consumer ring of fixed-size frame slots,
// pre-allocated up front — no allocation in the hot path (§77.1). The
// producer is the capture thread (vendor SDK copies straight into a slot);
// the consumer is the SER drain thread. When the ring is full the producer
// gets nullptr and counts the drop — capture never blocks on disk.
class FrameRingBuffer {
public:
    struct Frame {
        const std::uint8_t* data{};
        std::size_t size{};
        std::int64_t timestamp_utc_ticks{};  // .NET ticks (100 ns since 0001-01-01 UTC)
    };

    // capacity_bytes is divided into max(2, capacity/slot) slots of slot_bytes.
    FrameRingBuffer(std::size_t slot_bytes, std::size_t capacity_bytes);

    // Producer: pointer to the next free slot, or nullptr when the ring is
    // full (caller counts a drop) or closed.
    std::uint8_t* begin_write();

    // Producer: publish the slot returned by begin_write. size must be
    // <= slot_bytes.
    void commit_write(std::size_t size, std::int64_t timestamp_utc_ticks);

    // Consumer: block up to wait for the next frame. Returns false on
    // timeout, or when the ring is closed and drained.
    bool pop(Frame& out, std::chrono::milliseconds wait);

    // Consumer: release the slot returned by the last successful pop.
    void release_read();

    // Producer side is done: wake the consumer; pop returns false once the
    // remaining frames are drained.
    void close();

    bool closed() const;
    std::size_t slot_bytes() const { return slot_bytes_; }
    std::size_t slot_count() const { return slot_count_; }
    std::size_t frames_queued() const;

private:
    struct SlotMeta {
        std::size_t size{};
        std::int64_t timestamp_utc_ticks{};
    };

    std::size_t slot_bytes_;
    std::size_t slot_count_;
    std::vector<std::uint8_t> arena_;
    std::vector<SlotMeta> meta_;

    mutable std::mutex mutex_;
    std::condition_variable data_available_;
    std::size_t head_{};      // next slot to write
    std::size_t tail_{};      // next slot to read
    std::size_t queued_{};    // committed, not yet released
    bool write_open_{false};  // begin_write handed out, commit pending
    bool read_open_{false};   // pop handed out, release pending
    bool closed_{false};
};

}  // namespace alpacacore::video
