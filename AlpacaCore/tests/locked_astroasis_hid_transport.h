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

#include <alpacacore/vendor/astroasis/astroasis_hid_transport.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace alpacacore::test {

/**
 * Thread-safe decorator over any AstroasisHidTransport: open, close, write and
 * read forward to the inner transport under one mutex, the way a single-threaded
 * HID library serialises its entry points. Same shape as LockedZWOEAFSDK, except
 * that it owns the inner transport: the driver factory takes a unique_ptr, and
 * a HID handle is per-device state. Tests that need to see the inner keep their
 * own shared state (see FakeHidState).
 *
 * The mutex is a leaf (the inner transport never calls back into this
 * interface). It is held across a blocking read, as a real hid_read_timeout
 * would be held by a single-threaded library. Never gate Move (0x36) behind this
 * decorator: the mutex stays held while FakeHidTransport::hold_gate() waits, so
 * Halt would block. Keep gated cases on the bare fake.
 */
class LockedAstroasisHidTransport final : public vendor::astroasis::AstroasisHidTransport {
public:
    explicit LockedAstroasisHidTransport(std::unique_ptr<vendor::astroasis::AstroasisHidTransport> inner)
        : inner_(std::move(inner)) {}

    bool open(const std::string& hid_path) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return inner_->open(hid_path);
    }
    void close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        inner_->close();
    }
    int write(const std::uint8_t* data, std::size_t length) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return inner_->write(data, length);
    }
    int read(std::uint8_t* data, std::size_t length, int timeout_ms) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return inner_->read(data, length, timeout_ms);
    }

private:
    std::unique_ptr<vendor::astroasis::AstroasisHidTransport> inner_;
    std::mutex mutex_;
};

}  // namespace alpacacore::test
