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

#include <cstring>
#include <thread>

#include "catch2_compat.h"

using alpacacore::video::adaptive_ring_bytes;
using alpacacore::video::FrameRingBuffer;

namespace {
constexpr std::size_t kMiB = 1024ULL * 1024;
}

TEST_CASE("Video ring - adaptive sizing clamps per §77.1", "[video][unit]") {
    // clamp(mem/4, 64 MB, 512 MB)
    CHECK(adaptive_ring_bytes(0) == 64 * kMiB);
    CHECK(adaptive_ring_bytes(100 * kMiB) == 64 * kMiB);           // floor
    CHECK(adaptive_ring_bytes(1024 * kMiB) == 256 * kMiB);         // 2 GB iMate class
    CHECK(adaptive_ring_bytes(8ULL * 1024 * kMiB) == 512 * kMiB);  // Pi 5 class, ceiling
}

TEST_CASE("Video ring - push/pop round-trip preserves data and order", "[video][unit]") {
    FrameRingBuffer ring(16, 64);  // 4 slots

    for (std::uint8_t i = 0; i < 3; ++i) {
        std::uint8_t* slot = ring.begin_write();
        REQUIRE(slot != nullptr);
        std::memset(slot, i + 1, 16);
        ring.commit_write(16, 1000 + i);
    }
    CHECK(ring.frames_queued() == 3);

    for (std::uint8_t i = 0; i < 3; ++i) {
        FrameRingBuffer::Frame frame;
        REQUIRE(ring.pop(frame, std::chrono::milliseconds(100)));
        CHECK(frame.size == 16);
        CHECK(frame.timestamp_utc_ticks == 1000 + i);
        CHECK(frame.data[0] == i + 1);
        CHECK(frame.data[15] == i + 1);
        ring.release_read();
    }
    CHECK(ring.frames_queued() == 0);
}

TEST_CASE("Video ring - full ring returns nullptr instead of blocking", "[video][unit]") {
    FrameRingBuffer ring(16, 32);  // 2 slots
    REQUIRE(ring.slot_count() == 2);

    for (int i = 0; i < 2; ++i) {
        std::uint8_t* slot = ring.begin_write();
        REQUIRE(slot != nullptr);
        ring.commit_write(16, i);
    }
    CHECK(ring.begin_write() == nullptr);  // full — caller counts the drop

    FrameRingBuffer::Frame frame;
    REQUIRE(ring.pop(frame, std::chrono::milliseconds(100)));
    // Slot not yet released: still full from the producer's side.
    CHECK(ring.begin_write() == nullptr);
    ring.release_read();
    CHECK(ring.begin_write() != nullptr);
}

TEST_CASE("Video ring - close drains remaining frames then pop returns false", "[video][unit]") {
    FrameRingBuffer ring(8, 32);
    std::uint8_t* slot = ring.begin_write();
    REQUIRE(slot != nullptr);
    ring.commit_write(8, 42);

    ring.close();
    CHECK(ring.begin_write() == nullptr);  // no writes after close

    FrameRingBuffer::Frame frame;
    REQUIRE(ring.pop(frame, std::chrono::milliseconds(100)));
    CHECK(frame.timestamp_utc_ticks == 42);
    ring.release_read();
    CHECK_FALSE(ring.pop(frame, std::chrono::milliseconds(10)));
}

TEST_CASE("Video ring - pop times out on an empty open ring", "[video][unit]") {
    FrameRingBuffer ring(8, 32);
    FrameRingBuffer::Frame frame;
    CHECK_FALSE(ring.pop(frame, std::chrono::milliseconds(10)));
}

TEST_CASE("Video ring - close wakes a blocked consumer", "[video][unit]") {
    FrameRingBuffer ring(8, 32);
    std::thread closer([&ring] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ring.close();
    });
    FrameRingBuffer::Frame frame;
    // Wait far longer than the close delay: the close must wake us early.
    CHECK_FALSE(ring.pop(frame, std::chrono::milliseconds(5000)));
    closer.join();
}
