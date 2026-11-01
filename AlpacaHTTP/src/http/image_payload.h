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

#pragma once

#include <alpacacore/camera_driver.h>

#include <functional>
#include <string>
#include <string_view>

#include "device_call.h"

namespace alpacahttp {

struct ImagePayload {
    std::string content_type;
    std::string body;
};

// True when the Accept header value asks for application/imagebytes (any case).
bool accepts_imagebytes(std::string_view accept_header);

// The camera imagearray response: ImageBytes when the Accept header asks for it,
// else the JSON ImageArray envelope. Transaction ids come from `call`. Throws an
// AlpacaException with error code DriverException for an image whose shape, data
// length or encoded size is invalid.
ImagePayload build(const alpacacore::ImageArray& image, std::string_view accept_header, const DeviceCall& call);

// Marks every frame a camera may deliver from here on as a new one. Constructed
// around each non-GET request on a camera device (StartExposure, AbortExposure,
// StopExposure, Connected, Connect, Disconnect, ...): both the constructor and the
// destructor bump, so a request that arrives while the PUT runs or right after it
// never joins a build that began before it.
class ImageBuildEpoch {
public:
    ImageBuildEpoch();
    ~ImageBuildEpoch();
    ImageBuildEpoch(const ImageBuildEpoch&) = delete;
    ImageBuildEpoch& operator=(const ImageBuildEpoch&) = delete;
};

// build(fetch(), ...) with two limits on parallel imagearray GETs (#1716):
//  - At most one image build runs at a time, process wide. Requests beyond that wait.
//  - GETs for the same device, encoding and ImageBuildEpoch that arrive while a build
//    is running share it: fetch() runs once and each of them gets the encoded body,
//    carrying its own ClientTransactionID and ServerTransactionID. Nothing is kept
//    once the build is done, so a later GET fetches the current frame.
// A fetch() or build failure reaches every request that joined the build.
ImagePayload build_shared(const void* device, const std::function<alpacacore::ImageArray()>& fetch,
                          std::string_view accept_header, const DeviceCall& call);

}  // namespace alpacahttp
