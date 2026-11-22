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

#include <cstddef>
#include <string>
#include <vector>

namespace alpacacore::util {

// The one thing that reaches a GPIO kernel interface for a power-port board: libgpiod
// (util::GpiodLineBackend) or ZWO's pwm_gpio.ko ioctls (the ASIAIR Plus RK3568). A
// util::SoftPwmPorts engine drives it; a test drives the engine over a fake
// (alpacacore::test::FakeGpioLineBackend).
//
// A backend is not thread-safe and does not need to be: the engine serialises every
// call under its I/O lock. Protected non-virtual destructor, the shape of the other
// seams (testing.instructions.md, "Prefer the QHY form for a new seam"): the engine holds a
// reference, and whoever built the backend outlives the engine.
class GpioLineBackend {
public:
    // Claims @p lines as outputs on @p device_path and changes none of them (decision 0021).
    // Returns the level each line holds now, 0 or 1, in the order given; a backend that
    // cannot read its lines (the RK3568's GET_LEVEL reports the module's cache, not the
    // pad) returns an empty vector, and the engine keeps its cached "on". Throws
    // AlpacaException (NotConnected when the device cannot be had) and holds nothing then.
    virtual std::vector<int> open(const std::string& device_path, const std::vector<unsigned int>& lines) = 0;

    // Drives the line at @p line_index (a position in the vector open() took) high or low.
    // Returns false when the write failed, after logging why. Only valid while open.
    virtual bool write(std::size_t line_index, bool high) = 0;

    // Releases every line, changing none. A no-op when not open.
    virtual void close() = 0;

protected:
    GpioLineBackend() = default;
    GpioLineBackend(const GpioLineBackend&) = default;
    GpioLineBackend& operator=(const GpioLineBackend&) = default;
    ~GpioLineBackend() = default;
};

}  // namespace alpacacore::util
