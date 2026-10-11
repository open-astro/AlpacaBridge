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

#include <alpacacore/util/image_validation.h>

#include <functional>

#include "catch2_compat.h"

namespace {
void check_driver_exception(const std::function<void()>& call) {
    try {
        call();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
    }
}
}  // namespace

TEST_CASE("Camera image validation - rejects malformed shapes", "[camera][image][unit]") {
    alpacacore::ImageArray mono{{1, 2, 3, 4, 5, 6}, 2, 3, 2};
    const auto mono_shape = alpacacore::util::validate_image_array(mono);
    CHECK(mono_shape.width == 2);
    CHECK(mono_shape.height == 3);
    CHECK(mono_shape.channels == 1);
    CHECK(mono_shape.element_count == 6);

    alpacacore::ImageArray rgb{{1, 2, 3, 4, 5, 6}, 1, 2, 3};
    const auto rgb_shape = alpacacore::util::validate_image_array(rgb);
    CHECK(rgb_shape.channels == 3);
    CHECK(rgb_shape.element_count == 6);

    alpacacore::ImageArray invalid_rank{{}, 1, 1, 1};
    check_driver_exception([&] { (void)alpacacore::util::validate_image_array(invalid_rank); });

    alpacacore::ImageArray invalid_dimensions{{}, 0, 1, 2};
    check_driver_exception([&] { (void)alpacacore::util::validate_image_array(invalid_dimensions); });

    alpacacore::ImageArray short_mono{{1, 2, 3}, 2, 2, 2};
    check_driver_exception([&] { (void)alpacacore::util::validate_image_array(short_mono); });

    alpacacore::ImageArray short_rgb{{1, 2, 3, 4, 5}, 1, 2, 3};
    check_driver_exception([&] { (void)alpacacore::util::validate_image_array(short_rgb); });
}
