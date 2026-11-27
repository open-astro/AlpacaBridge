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

#include <alpacacore/util/frame_layout.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"

using alpacacore::ImageArray;
using alpacacore::util::build_frame_image;
using alpacacore::util::FrameEdgeRule;
using alpacacore::util::FrameFormat;
using alpacacore::util::FrameLayout;
using alpacacore::util::FrameWindow;
using Bytes = std::vector<std::uint8_t>;
using Samples = std::vector<std::int32_t>;

namespace {

ImageArray build(const Bytes& buffer, FrameFormat format, std::size_t stride, int src_w, int src_h, FrameWindow window,
                 FrameEdgeRule edge = FrameEdgeRule::ZeroFill) {
    return build_frame_image(buffer, FrameLayout{format, stride, src_w, src_h}, window, edge, "Test");
}

void require_refusal(const std::function<void()>& call, const std::string& text) {
    try {
        call();
        FAIL("expected a refusal containing: " << text);
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(e.what()).find(text) != std::string::npos);
        CHECK(std::string(e.what()).find("Test ") != std::string::npos);
    }
}

}  // namespace

TEST_CASE("Frame layout - mono 8 copies each byte", "[frame_layout][unit]") {
    const auto image = build({1, 2, 3, 4, 5, 6}, FrameFormat::Mono8, 3, 3, 2, {0, 0, 3, 2});
    CHECK(image.rank == 2);
    CHECK(image.width == 3);
    CHECK(image.height == 2);
    CHECK(image.data == Samples{1, 2, 3, 4, 5, 6});
}

TEST_CASE("Frame layout - mono 16 reads little-endian past one byte", "[frame_layout][unit]") {
    // 0x1234, 0xFF01, 0x00FF, 0xABCD: both bytes matter and the low byte comes first.
    const auto image =
        build({0x34, 0x12, 0x01, 0xFF, 0xFF, 0x00, 0xCD, 0xAB}, FrameFormat::Mono16, 2, 2, 2, {0, 0, 2, 2});
    CHECK(image.data == Samples{0x1234, 0xFF01, 0x00FF, 0xABCD});
}

TEST_CASE("Frame layout - BGR24 comes out as RGB planes", "[frame_layout][unit]") {
    const auto image = build({10, 20, 30, 40, 50, 60}, FrameFormat::Bgr24, 2, 2, 1, {0, 0, 2, 1});
    CHECK(image.rank == 3);
    CHECK(image.data == Samples{30, 20, 10, 60, 50, 40});
}

TEST_CASE("Frame layout - RGB24 is copied unswapped", "[frame_layout][unit]") {
    const auto image = build({10, 20, 30, 40, 50, 60}, FrameFormat::Rgb24, 2, 2, 1, {0, 0, 2, 1});
    CHECK(image.rank == 3);
    CHECK(image.data == Samples{10, 20, 30, 40, 50, 60});
}

TEST_CASE("Frame layout - a stride wider than the window drops the padding", "[frame_layout][unit]") {
    // Rows of 4 pixels, 2 of them image: the padding bytes 99 never reach the output.
    const auto image = build({1, 2, 99, 99, 3, 4, 99, 99}, FrameFormat::Mono8, 4, 2, 2, {0, 0, 2, 2});
    CHECK(image.data == Samples{1, 2, 3, 4});
}

TEST_CASE("Frame layout - a crop offset starts inside the source", "[frame_layout][unit]") {
    const Bytes src{1, 2, 3, 4, 5, 6, 7, 8, 9};
    const auto image = build(src, FrameFormat::Mono8, 3, 3, 3, {1, 1, 2, 2});
    CHECK(image.data == Samples{5, 6, 8, 9});
}

TEST_CASE("Frame layout - zero-fill leaves a missing edge at 0", "[frame_layout][unit]") {
    const auto image = build({1, 2, 3, 4}, FrameFormat::Mono8, 2, 2, 2, {0, 0, 3, 3});
    CHECK(image.data == Samples{1, 2, 0, 3, 4, 0, 0, 0, 0});
}

TEST_CASE("Frame layout - replicate fills one missing row and column from the neighbour", "[frame_layout][unit]") {
    const auto image = build({1, 2, 3, 4}, FrameFormat::Mono8, 2, 2, 2, {0, 0, 3, 3}, FrameEdgeRule::ReplicateOne);
    CHECK(image.data == Samples{1, 2, 2, 3, 4, 4, 3, 4, 4});
}

TEST_CASE("Frame layout - replicate refuses more than one missing row or column", "[frame_layout][unit]") {
    const Bytes src{1, 2, 3, 4};
    require_refusal([&] { build(src, FrameFormat::Mono8, 2, 2, 2, {0, 0, 4, 2}, FrameEdgeRule::ReplicateOne); },
                    "more than one pixel smaller");
    require_refusal([&] { build(src, FrameFormat::Mono8, 2, 2, 2, {0, 0, 2, 4}, FrameEdgeRule::ReplicateOne); },
                    "more than one pixel smaller");
}

TEST_CASE("Frame layout - a buffer shorter than the layout needs is refused", "[frame_layout][unit]") {
    require_refusal([] { build({1, 2, 3}, FrameFormat::Mono8, 2, 2, 2, {0, 0, 2, 2}); }, "shorter than");
    require_refusal([] { build({1, 2, 3, 4, 5, 6, 7}, FrameFormat::Mono16, 2, 2, 2, {0, 0, 2, 2}); }, "shorter than");
    require_refusal([] { build({1, 2, 3, 4, 5}, FrameFormat::Bgr24, 1, 1, 2, {0, 0, 1, 2}); }, "shorter than");
}

TEST_CASE("Frame layout - size arithmetic that overflows is refused", "[frame_layout][unit]") {
    const auto huge = std::numeric_limits<std::size_t>::max() / 2 + 1;
    require_refusal([&] { build({1, 2, 3, 4}, FrameFormat::Mono8, huge, 1, 4, {0, 0, 1, 1}); }, "overflow");
    require_refusal(
        [&] {
            build({1, 2, 3, 4}, FrameFormat::Bgr24, std::numeric_limits<std::size_t>::max() / 2, 1, 2, {0, 0, 1, 1});
        },
        "overflow");
}

TEST_CASE("Frame layout - invalid dimensions are refused", "[frame_layout][unit]") {
    const Bytes src{1, 2, 3, 4};
    require_refusal([&] { build(src, FrameFormat::Mono8, 2, 2, 2, {0, 0, 0, 2}); }, "invalid dimensions");
    require_refusal([&] { build(src, FrameFormat::Mono8, 2, 0, 2, {0, 0, 2, 2}); }, "invalid dimensions");
    require_refusal([&] { build(src, FrameFormat::Mono8, 1, 2, 2, {0, 0, 2, 2}); }, "invalid dimensions");
    require_refusal([&] { build(src, FrameFormat::Mono8, 2, 2, 2, {-1, 0, 2, 2}); }, "invalid dimensions");
}
