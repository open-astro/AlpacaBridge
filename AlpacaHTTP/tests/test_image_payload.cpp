// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

// image_payload (the camera imagearray encoders: ImageBytes and the JSON
// ImageArray) on synthetic images: no Router, no driver.
//
// Every case carries a "Falsified by:" line naming the mutation of
// image_payload.cpp that turns it red.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/camera_driver.h>
#include <alpacahttp/request.h>

#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "../src/http/device_call.h"
#include "../src/http/image_payload.h"
#include "test_assert.h"

namespace {

using alpacacore::ImageArray;
using alpacahttp::DeviceCall;
using alpacahttp::ImagePayload;

constexpr std::uint32_t kClientTx = 9;
constexpr std::uint32_t kServerTx = 77;
constexpr const char* kJson = "application/json";
constexpr const char* kBytes = "application/imagebytes";

std::unique_ptr<alpacahttp::Request> make_request() {
    auto request = std::make_unique<alpacahttp::Request>();
    EXPECT(request->parse("GET /x?ClientTransactionID=9 HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    return request;
}

ImagePayload build_payload(const ImageArray& image, const std::string& accept) {
    auto request = make_request();
    DeviceCall call(*request, kServerTx);
    return alpacahttp::build(image, accept, call);
}

std::uint32_t le32(const std::string& body, std::size_t index) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(body[index * 4 + i])) << (8 * i);
    }
    return value;
}

struct Header {
    std::uint32_t version, error, client_tx, server_tx, data_start, image_type, transmission_type, rank, dim1, dim2,
        dim3;
};

Header header_of(const std::string& body) {
    EXPECT(body.size() >= 44);
    return {le32(body, 0), le32(body, 1), le32(body, 2), le32(body, 3), le32(body, 4), le32(body, 5),
            le32(body, 6), le32(body, 7), le32(body, 8), le32(body, 9), le32(body, 10)};
}

struct Refusal {
    std::string message;
    int code = 0;
};

template <typename Fn>
Refusal refusal_of(Fn&& fn) {
    try {
        fn();
    } catch (const alpacacore::AlpacaException& e) {
        return {e.what(), e.error_code()};
    }
    return {};
}

// case: JSON ImageArray rank 2 is X-major with the envelope head
// A 3 wide, 2 high rank-2 image reaches the wire X-major: [[col0],[col1],[col2]].
// Falsified by: skipping the transpose; swapping the two ids in the envelope head; dropping Type or Rank.
void case_json_rank2() {
    const auto payload = build_payload({{0, 1, 2, 3, 4, 5}, 3, 2, 2}, kJson);
    EXPECT(payload.content_type == "application/json");
    EXPECT(payload.body ==
           "{\"ClientTransactionID\":9,\"ServerTransactionID\":77,\"ErrorNumber\":0,\"ErrorMessage\":\"\","
           "\"Type\":2,\"Rank\":2,\"Value\":[[0,3],[1,4],[2,5]]}");
}

// case: JSON ImageArray rank 3 nests x, y, channel
// Rank 3 nests [x][y][channel] and keeps each pixel's channels together.
// Falsified by: transposing channels with x/y; nesting rank 3 like rank 2.
void case_json_rank3() {
    const auto payload = build_payload({{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}, 2, 2, 3}, kJson);
    EXPECT(payload.content_type == "application/json");
    EXPECT(payload.body ==
           "{\"ClientTransactionID\":9,\"ServerTransactionID\":77,\"ErrorNumber\":0,\"ErrorMessage\":\"\","
           "\"Type\":2,\"Rank\":3,\"Value\":[[[1,2,3],[7,8,9]],[[4,5,6],[10,11,12]]]}");
}

// case: JSON ImageArray blocked transpose matches a plain X-major walk
// The blocked traversal must reach the same offsets as a plain X-major walk on an image wider than one block.
// Falsified by: a block edge off by one in for_each_xy_blocked.
void case_json_blocked_transpose_matches_naive() {
    constexpr int kWidth = 70;
    constexpr int kHeight = 67;
    ImageArray image;
    image.width = kWidth;
    image.height = kHeight;
    image.rank = 2;
    for (int i = 0; i < kWidth * kHeight; ++i) {
        image.data.push_back(i);
    }
    std::string expected = "[";
    for (int x = 0; x < kWidth; ++x) {
        expected += x ? ",[" : "[";
        for (int y = 0; y < kHeight; ++y) {
            expected += (y ? "," : "") + std::to_string(y * kWidth + x);
        }
        expected += "]";
    }
    expected += "]}";
    const auto payload = build_payload(image, kJson);
    EXPECT(payload.body.size() > expected.size());
    EXPECT(payload.body.compare(payload.body.size() - expected.size(), expected.size(), expected) == 0);
}

// case: Accept header picks ImageBytes or JSON
// Accept negotiation: only a header naming application/imagebytes (any case, among others) selects ImageBytes.
// Falsified by: comparing the Accept value case-sensitively; always choosing JSON; always choosing ImageBytes.
void case_accept_negotiation() {
    const ImageArray image{{1, 2, 3, 4}, 2, 2, 2};
    for (const char* accept : {"", "*/*", "application/json", "text/html"}) {
        EXPECT(!alpacahttp::accepts_imagebytes(accept));
        EXPECT(build_payload(image, accept).content_type == "application/json");
    }
    for (const char* accept :
         {"application/imagebytes", "Application/ImageBytes", "application/json, application/imagebytes;q=0.9"}) {
        EXPECT(alpacahttp::accepts_imagebytes(accept));
        const auto payload = build_payload(image, accept);
        EXPECT(payload.content_type == kBytes);
        EXPECT(header_of(payload.body).version == 1);
    }
}

// case: ImageBytes header carries both transaction ids, rank and dimensions
// The 44-byte ImageBytes header: both transaction ids, element types, rank and dimensions.
// Falsified by: swapping client and server ids; writing the wrong rank, dimension or data-start offset.
void case_imagebytes_header_fields() {
    const auto mono = build_payload({{0, 1, 2, 3, 4, 5}, 3, 2, 2}, kBytes);
    const Header m = header_of(mono.body);
    EXPECT(m.version == 1);
    EXPECT(m.error == 0);
    EXPECT(m.client_tx == kClientTx);
    EXPECT(m.server_tx == kServerTx);
    EXPECT(m.data_start == 44);
    EXPECT(m.image_type == 2);
    EXPECT(m.transmission_type == 6);
    EXPECT(m.rank == 2);
    EXPECT(m.dim1 == 3);
    EXPECT(m.dim2 == 2);
    EXPECT(m.dim3 == 0);
    EXPECT(mono.body.size() == 44 + 6);

    const auto color = build_payload({std::vector<std::int32_t>(12, 1), 2, 2, 3}, kBytes);
    const Header c = header_of(color.body);
    EXPECT(c.rank == 3);
    EXPECT(c.dim1 == 2);
    EXPECT(c.dim2 == 2);
    EXPECT(c.dim3 == 3);
    EXPECT(color.body.size() == 44 + 12);
}

// case: ImageBytes picks the narrowest element type and packs X-major little-endian
// The transmission type is the narrowest that holds the data, and pixels go out X-major little-endian.
// Falsified by: wrong range cut-offs in choose_image_bytes_format; big-endian packing; missing clamp.
void case_imagebytes_element_types() {
    struct Row {
        std::vector<std::int32_t> data;
        std::uint32_t type;
        std::string pixels;
    };
    const std::vector<Row> rows = {
        {{0, 255, 7, 8}, 6, std::string("\x00\x07\xff\x08", 4)},
        {{0, 256, 65535, 1}, 8, std::string("\x00\x00\xff\xff\x00\x01\x01\x00", 8)},
        {{-32768, -1, 32767, 0}, 1, std::string("\x00\x80\xff\x7f\xff\xff\x00\x00", 8)},
        {{-40000, 0, 70000, 1}, 2, std::string("\xc0\x63\xff\xff\x70\x11\x01\x00\x00\x00\x00\x00\x01\x00\x00\x00", 16)},
    };
    for (const auto& row : rows) {
        const auto payload = build_payload({row.data, 2, 2, 2}, kBytes);
        const Header h = header_of(payload.body);
        EXPECT(h.transmission_type == row.type);
        EXPECT(h.image_type == 2);
        EXPECT(payload.body.substr(44) == row.pixels);
    }
}

// case: Invalid images are refused, including the size overflow guard
// An image that cannot be encoded is refused with error code DriverException and a message, never a short body.
// Falsified by: removing the shape check, the data-length check or the size-overflow guard; throwing InvalidOperation
// instead of DriverException.
void case_invalid_images_refused() {
    const auto bad_rank = refusal_of([] { build_payload({{1}, 1, 1, 4}, kBytes); });
    EXPECT(bad_rank.code == alpacacore::AlpacaError::DriverException);
    EXPECT(bad_rank.message.find("invalid rank or dimensions") != std::string::npos);
    const auto bad_len_json = refusal_of([] { build_payload({{1, 2, 3}, 2, 2, 2}, kJson); });
    EXPECT(bad_len_json.code == alpacacore::AlpacaError::DriverException);
    EXPECT(bad_len_json.message.find("data length does not match dimensions") != std::string::npos);
    const auto bad_len_bytes = refusal_of([] { build_payload({{1, 2, 3}, 2, 2, 2}, kBytes); });
    EXPECT(bad_len_bytes.code == alpacacore::AlpacaError::DriverException);
    EXPECT(bad_len_bytes.message.find("data length does not match dimensions") != std::string::npos);
    const int huge = std::numeric_limits<int>::max();
    const auto overflow = refusal_of([&] { build_payload({{}, huge, huge, 3}, kBytes); });
    EXPECT(overflow.code == alpacacore::AlpacaError::DriverException);
    EXPECT(overflow.message.find("payload size overflows addressable memory") != std::string::npos);
}

}  // namespace

int main() {
    case_json_rank2();
    case_json_rank3();
    case_json_blocked_transpose_matches_naive();
    case_accept_negotiation();
    case_imagebytes_header_fields();
    case_imagebytes_element_types();
    case_invalid_images_refused();
    std::cout << "test_image_payload: all checks passed\n";
    return 0;
}
