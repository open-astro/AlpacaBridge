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

#include "image_payload.h"

#include <alpacacore/util/image_validation.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace alpacahttp {

namespace {
void append_int(std::string& out, std::int64_t value) { out.append(std::to_string(value)); }

// image.data is stored Y-major/X-minor (row-major, one scanline at a time --
// see the decode paths that fill it), but ASCOM's ImageArray wire format
// (both the imagebytes binary payload and the JSON [x][y] nesting below) is
// X-major/Y-minor. Reading it out with x as the outer loop variable directly
// against that row-major layout strides by a full row (width * 4 bytes --
// tens of KB for a real sensor) on every single element: a cache miss on
// nearly every pixel, and a page fault on many, across a multi-hundred-MB
// buffer.
//
// Both JSON and ImageBytes use the same blocked X-major traversal. Blocking
// changes only when a value is read relative to when it is written, never
// which output offset it ends up at.
//
// Measured 5.2x faster overall (5.4s -> 1.0s for a 6016x4016 Int32 frame) on
// an RK3568-class ARM SBC with AlpacaCore's libgphoto2 driver. Larger sensors
// and other cameras still need their own timing validation.
template <typename Visitor>
void for_each_xy_blocked(std::size_t width, std::size_t height, Visitor&& visit) {
    constexpr std::size_t kBlock = 64;
    for (std::size_t by = 0; by < height; by += kBlock) {
        const auto by_end = std::min(by + kBlock, height);
        for (std::size_t bx = 0; bx < width; bx += kBlock) {
            const auto bx_end = std::min(bx + kBlock, width);
            for (std::size_t y = by; y < by_end; ++y) {
                for (std::size_t x = bx; x < bx_end; ++x) {
                    visit(x, y);
                }
            }
        }
    }
}

using ImageShape = alpacacore::util::ImageShape;
using alpacacore::util::throw_invalid_camera_image;
using alpacacore::util::validate_image_data_length;
using alpacacore::util::validate_image_shape;

std::vector<std::int32_t> transpose_xy(const std::vector<std::int32_t>& data, std::size_t width, std::size_t height,
                                       std::size_t channels) {
    std::vector<std::int32_t> out(data.size());
    for_each_xy_blocked(width, height, [&](std::size_t x, std::size_t y) {
        const std::size_t src_base = (y * width + x) * channels;
        const std::size_t dst_base = (x * height + y) * channels;
        for (std::size_t c = 0; c < channels; ++c) {
            out[dst_base + c] = data[src_base + c];
        }
    });
    return out;
}

std::string build_image_array_payload(const alpacacore::ImageArray& image, int type, const DeviceCall& call) {
    const ImageShape shape = validate_image_shape(image);
    validate_image_data_length(image, shape);
    constexpr std::size_t kBaseJsonSize = 256;
    constexpr auto kMaxSize = std::numeric_limits<std::size_t>::max();
    const auto [min_value, max_value] = std::minmax_element(image.data.begin(), image.data.end());
    const std::size_t max_value_chars = std::max(std::to_string(*min_value).size(), std::to_string(*max_value).size());
    const std::size_t bytes_per_value = max_value_chars + 1;  // one delimiter per value
    const std::size_t pixels = shape.element_count / shape.channels;
    std::size_t bracket_groups = shape.width + 1;
    if (image.rank == 3) {
        if (pixels > kMaxSize - bracket_groups) {
            throw_invalid_camera_image("JSON payload size overflows addressable memory");
        }
        bracket_groups += pixels;
    }
    if (bracket_groups > (kMaxSize - kBaseJsonSize) / 2) {
        throw_invalid_camera_image("JSON payload size overflows addressable memory");
    }
    const std::size_t fixed_json_size = kBaseJsonSize + bracket_groups * 2;
    if (shape.element_count > (kMaxSize - fixed_json_size) / bytes_per_value) {
        throw_invalid_camera_image("JSON payload size overflows addressable memory");
    }
    const std::size_t estimate = fixed_json_size + shape.element_count * bytes_per_value;

    std::string body;
    body.reserve(estimate);
    body.append(call.success_head());
    body.append(",\"Type\":");
    append_int(body, type);
    body.append(",\"Rank\":");
    append_int(body, image.rank);
    body.append(",\"Value\":");

    if (image.rank == 2) {
        const auto width = static_cast<std::uint32_t>(shape.width);
        const auto height = static_cast<std::uint32_t>(shape.height);
        std::vector<std::int32_t> transposed = transpose_xy(image.data, shape.width, shape.height, shape.channels);
        body.push_back('[');
        for (std::uint32_t x = 0; x < width; ++x) {
            if (x > 0) {
                body.push_back(',');
            }
            body.push_back('[');
            for (std::uint32_t y = 0; y < height; ++y) {
                if (y > 0) {
                    body.push_back(',');
                }
                append_int(body, transposed[static_cast<std::size_t>(x) * height + y]);
            }
            body.push_back(']');
        }
        body.push_back(']');
    } else {
        const auto width = static_cast<std::uint32_t>(shape.width);
        const auto height = static_cast<std::uint32_t>(shape.height);
        std::vector<std::int32_t> transposed = transpose_xy(image.data, shape.width, shape.height, shape.channels);
        body.push_back('[');
        for (std::uint32_t x = 0; x < width; ++x) {
            if (x > 0) {
                body.push_back(',');
            }
            body.push_back('[');
            for (std::uint32_t y = 0; y < height; ++y) {
                if (y > 0) {
                    body.push_back(',');
                }
                body.push_back('[');
                std::size_t base = (static_cast<std::size_t>(x) * height + y) * 3;
                for (int c = 0; c < 3; ++c) {
                    if (c > 0) {
                        body.push_back(',');
                    }
                    append_int(body, transposed[base + static_cast<std::size_t>(c)]);
                }
                body.push_back(']');
            }
            body.push_back(']');
        }
        body.push_back(']');
    }

    body.push_back('}');
    return body;
}

constexpr std::uint32_t kImageTypeInt16 = 1;
constexpr std::uint32_t kImageTypeInt32 = 2;
constexpr std::uint32_t kImageTypeByte = 6;
constexpr std::uint32_t kImageTypeUInt16 = 8;

constexpr std::uint32_t kImageBytesMetadataVersion = 1;
constexpr std::size_t kImageBytesMetadataSize = 11 * sizeof(std::uint32_t);

struct ImageBytesFormat {
    std::uint32_t image_element_type = kImageTypeInt32;
    std::uint32_t transmission_element_type = kImageTypeUInt16;
    std::size_t bytes_per_element = sizeof(std::uint16_t);
};

ImageBytesFormat choose_image_bytes_format(const alpacacore::ImageArray& image, std::uint32_t image_element_type) {
    ImageBytesFormat format;
    format.image_element_type = image_element_type;

    if (image_element_type == kImageTypeByte) {
        format.transmission_element_type = kImageTypeByte;
        format.bytes_per_element = sizeof(std::uint8_t);
        return format;
    }
    if (image_element_type == kImageTypeUInt16) {
        format.transmission_element_type = kImageTypeUInt16;
        format.bytes_per_element = sizeof(std::uint16_t);
        return format;
    }
    if (image_element_type == kImageTypeInt16) {
        format.transmission_element_type = kImageTypeInt16;
        format.bytes_per_element = sizeof(std::int16_t);
        return format;
    }

    if (image.data.empty()) {
        format.transmission_element_type = kImageTypeUInt16;
        format.bytes_per_element = sizeof(std::uint16_t);
        return format;
    }

    auto minmax = std::minmax_element(image.data.begin(), image.data.end());
    std::int64_t min_val = static_cast<std::int64_t>(*minmax.first);
    std::int64_t max_val = static_cast<std::int64_t>(*minmax.second);

    if (min_val >= 0 && max_val <= std::numeric_limits<std::uint8_t>::max()) {
        format.transmission_element_type = kImageTypeByte;
        format.bytes_per_element = sizeof(std::uint8_t);
    } else if (min_val >= 0 && max_val <= std::numeric_limits<std::uint16_t>::max()) {
        format.transmission_element_type = kImageTypeUInt16;
        format.bytes_per_element = sizeof(std::uint16_t);
    } else if (min_val >= std::numeric_limits<std::int16_t>::min() &&
               max_val <= std::numeric_limits<std::int16_t>::max()) {
        format.transmission_element_type = kImageTypeInt16;
        format.bytes_per_element = sizeof(std::int16_t);
    } else {
        format.transmission_element_type = kImageTypeInt32;
        format.bytes_per_element = sizeof(std::int32_t);
    }

    return format;
}

void append_uint32_le(std::string& out, std::uint32_t value) {
    char bytes[4];
    bytes[0] = static_cast<char>(value & 0xFF);
    bytes[1] = static_cast<char>((value >> 8) & 0xFF);
    bytes[2] = static_cast<char>((value >> 16) & 0xFF);
    bytes[3] = static_cast<char>((value >> 24) & 0xFF);
    out.append(bytes, sizeof(bytes));
}

std::string build_image_bytes_payload(const alpacacore::ImageArray& image, const ImageBytesFormat& format,
                                      const DeviceCall& call) {
    const ImageShape shape = validate_image_shape(image);
    constexpr auto kMaxSize = std::numeric_limits<std::size_t>::max();
    std::size_t expected_bytes_per_element = 0;
    switch (format.transmission_element_type) {
        case kImageTypeByte:
            expected_bytes_per_element = sizeof(std::uint8_t);
            break;
        case kImageTypeUInt16:
            expected_bytes_per_element = sizeof(std::uint16_t);
            break;
        case kImageTypeInt16:
            expected_bytes_per_element = sizeof(std::int16_t);
            break;
        case kImageTypeInt32:
            expected_bytes_per_element = sizeof(std::int32_t);
            break;
        default:
            throw_invalid_camera_image("unsupported transmission element type");
    }
    if (format.bytes_per_element != expected_bytes_per_element) {
        throw_invalid_camera_image("transmission element size does not match its type");
    }
    if (shape.element_count > (kMaxSize - kImageBytesMetadataSize) / expected_bytes_per_element) {
        throw_invalid_camera_image("payload size overflows addressable memory");
    }
    validate_image_data_length(image, shape);
    const std::size_t data_bytes = shape.element_count * expected_bytes_per_element;
    const std::size_t payload_size = kImageBytesMetadataSize + data_bytes;

    std::string body;
    body.reserve(payload_size);

    append_uint32_le(body, kImageBytesMetadataVersion);
    append_uint32_le(body, 0);
    append_uint32_le(body, call.client_tx_id());
    append_uint32_le(body, call.server_tx_id());
    append_uint32_le(body, static_cast<std::uint32_t>(kImageBytesMetadataSize));
    append_uint32_le(body, format.image_element_type);
    append_uint32_le(body, format.transmission_element_type);
    // validate_image_shape() above restricts rank to 2 or 3 before this narrowing.
    append_uint32_le(body, static_cast<std::uint32_t>(image.rank));
    append_uint32_le(body, static_cast<std::uint32_t>(shape.width));
    append_uint32_le(body, static_cast<std::uint32_t>(shape.height));
    append_uint32_le(body, image.rank == 3 ? 3 : 0);
    body.resize(payload_size);

    const auto pack_pixels = [&](auto write_element) {
        auto* output = body.data() + kImageBytesMetadataSize;
        for_each_xy_blocked(shape.width, shape.height, [&](std::size_t x, std::size_t y) {
            const std::size_t src = (y * shape.width + x) * shape.channels;
            const std::size_t dst = (x * shape.height + y) * shape.channels;
            for (std::size_t channel = 0; channel < shape.channels; ++channel) {
                write_element(output + (dst + channel) * format.bytes_per_element, image.data[src + channel]);
            }
        });
    };

    switch (format.transmission_element_type) {
        case kImageTypeByte:
            pack_pixels([](char* output, std::int32_t value) {
                output[0] = static_cast<char>(static_cast<std::uint8_t>(
                    std::clamp<std::int32_t>(value, 0, std::numeric_limits<std::uint8_t>::max())));
            });
            break;
        case kImageTypeUInt16:
            pack_pixels([](char* output, std::int32_t value) {
                const auto packed = static_cast<std::uint16_t>(
                    std::clamp<std::int32_t>(value, 0, std::numeric_limits<std::uint16_t>::max()));
                output[0] = static_cast<char>(packed & 0xFF);
                output[1] = static_cast<char>((packed >> 8) & 0xFF);
            });
            break;
        case kImageTypeInt16:
            pack_pixels([](char* output, std::int32_t value) {
                const auto packed = static_cast<std::uint16_t>(std::clamp<std::int32_t>(
                    value, std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()));
                output[0] = static_cast<char>(packed & 0xFF);
                output[1] = static_cast<char>((packed >> 8) & 0xFF);
            });
            break;
        case kImageTypeInt32:
            pack_pixels([](char* output, std::int32_t value) {
                const auto packed = static_cast<std::uint32_t>(value);
                for (unsigned shift = 0; shift < 32; shift += 8) {
                    output[shift / 8] = static_cast<char>((packed >> shift) & 0xFF);
                }
            });
            break;
        default:
            throw_invalid_camera_image("unsupported transmission element type");
    }

    return body;
}

}  // namespace

bool accepts_imagebytes(std::string_view accept_header) {
    return to_lower_copy(std::string(accept_header)).find("application/imagebytes") != std::string::npos;
}

ImagePayload build(const alpacacore::ImageArray& image, std::string_view accept_header, const DeviceCall& call) {
    ImagePayload payload;
    if (accepts_imagebytes(accept_header)) {
        const ImageBytesFormat format = choose_image_bytes_format(image, kImageTypeInt32);
        payload.content_type = "application/imagebytes";
        payload.body = build_image_bytes_payload(image, format, call);
    } else {
        payload.content_type = "application/json";
        payload.body = build_image_array_payload(image, 2, call);
    }
    return payload;
}

namespace {

std::atomic<std::uint64_t> g_epoch{0};

// A finished build, shared by every request that joined it.
struct SharedBuild {
    std::string content_type;
    std::string body;               // encoded with the builder's transaction ids
    std::size_t json_head_len = 0;  // JSON only: length of the builder's success_head()
    bool imagebytes = false;
};

struct InFlight {
    std::shared_future<std::shared_ptr<const SharedBuild>> result;
    int joined = 0;  // guarded by g_inflight_mutex
};

using BuildKey = std::tuple<const void*, bool, std::uint64_t>;

std::mutex g_inflight_mutex;
std::map<BuildKey, std::shared_ptr<InFlight>> g_inflight;
// One image build at a time. A 4 GB Pi holds one frame, its transposed copy and
// the encoded body (about 1.3 GB for a 6000x4000 colour JSON frame) but not two.
std::mutex g_build_mutex;

void write_uint32_le(std::string& out, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        out[offset + i] = static_cast<char>((value >> (8 * i)) & 0xFF);
    }
}

// The builder's body with this request's transaction ids.
ImagePayload materialize(const SharedBuild& build, const DeviceCall& call) {
    ImagePayload payload;
    payload.content_type = build.content_type;
    if (build.imagebytes) {
        payload.body = build.body;
        write_uint32_le(payload.body, 8, call.client_tx_id());
        write_uint32_le(payload.body, 12, call.server_tx_id());
    } else {
        const std::string head = call.success_head();
        payload.body.reserve(head.size() + build.body.size() - build.json_head_len);
        payload.body.append(head);
        payload.body.append(build.body, build.json_head_len, std::string::npos);
    }
    return payload;
}

}  // namespace

ImageBuildEpoch::ImageBuildEpoch() { g_epoch.fetch_add(1); }
ImageBuildEpoch::~ImageBuildEpoch() { g_epoch.fetch_add(1); }

ImagePayload build_shared(const void* device, const std::function<alpacacore::ImageArray()>& fetch,
                          std::string_view accept_header, const DeviceCall& call) {
    const bool imagebytes = accepts_imagebytes(accept_header);
    const BuildKey key{device, imagebytes, g_epoch.load()};

    std::shared_ptr<InFlight> entry;
    std::promise<std::shared_ptr<const SharedBuild>> promise;
    bool joined_existing = false;
    {
        std::lock_guard<std::mutex> lock(g_inflight_mutex);
        auto it = g_inflight.find(key);
        if (it != g_inflight.end()) {
            entry = it->second;
            ++entry->joined;
            joined_existing = true;
        } else {
            entry = std::make_shared<InFlight>();
            entry->result = promise.get_future().share();
            g_inflight.emplace(key, entry);
        }
    }
    if (joined_existing) {
        return materialize(*entry->result.get(), call);  // rethrows the builder's failure
    }

    // Removes the entry so later GETs build afresh; returns how many GETs joined it.
    const auto retire = [&] {
        std::lock_guard<std::mutex> lock(g_inflight_mutex);
        g_inflight.erase(key);
        return entry->joined;
    };
    SharedBuild build;
    try {
        std::lock_guard<std::mutex> build_lock(g_build_mutex);
        const alpacacore::ImageArray image = fetch();
        ImagePayload payload = alpacahttp::build(image, accept_header, call);
        build.content_type = std::move(payload.content_type);
        build.body = std::move(payload.body);
    } catch (...) {
        retire();
        promise.set_exception(std::current_exception());
        throw;
    }
    build.imagebytes = imagebytes;
    build.json_head_len = call.success_head().size();
    if (retire() == 0) {
        return ImagePayload{std::move(build.content_type), std::move(build.body)};
    }
    const auto shared = std::make_shared<const SharedBuild>(std::move(build));
    promise.set_value(shared);
    return materialize(*shared, call);
}

}  // namespace alpacahttp
