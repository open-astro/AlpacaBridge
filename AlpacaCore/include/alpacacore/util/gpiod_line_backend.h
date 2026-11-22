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

// Include only from a translation unit that links libgpiod (>= 2.0), like gpiod_output_claim.h: the
// GPIO power-port boards that use libgpiod (ZWO ASIAIR Pro / Plus CM4, ToupTek StellaVita, iOptron
// iMate). AlpacaCore itself does not link it.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/gpio_line_backend.h>
#include <alpacacore/util/gpiod_output_claim.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/serial_io.h>
#include <gpiod.h>

#include <cerrno>
#include <string>
#include <vector>

namespace alpacacore::util {

// libgpiod v2 over one gpiod_line_request that owns every line together (the general GPIO rules in
// ascom-contract.instructions.md). Lines are claimed at the level they hold now and values go through
// gpiod_line_request_set_value; never the v1 per-line API.
class GpiodLineBackend final : public GpioLineBackend {
public:
    // @p consumer names the request in the kernel's line info, e.g. "alpacabridge-asiair".
    explicit GpiodLineBackend(std::string consumer) : consumer_(std::move(consumer)) {}
    ~GpiodLineBackend() { close(); }

    GpiodLineBackend(const GpiodLineBackend&) = delete;
    GpiodLineBackend& operator=(const GpiodLineBackend&) = delete;
    GpiodLineBackend(GpiodLineBackend&&) = delete;
    GpiodLineBackend& operator=(GpiodLineBackend&&) = delete;

    std::vector<int> open(const std::string& device_path, const std::vector<unsigned int>& lines) override {
        // The claim reads each line and changes none (issue #300): a connect neither powers a port on nor
        // reports one on that the boot left off.
        GpiodOutputClaim claim = claim_outputs_at_current_level(device_path, lines, consumer_.c_str());
        chip_ = claim.chip;
        request_ = claim.request;
        lines_ = lines;
        return claim.levels;
    }

    bool write(std::size_t line_index, bool high) override {
        if (request_ == nullptr || line_index >= lines_.size()) {
            return false;
        }
        const auto value = high ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
        if (::gpiod_line_request_set_value(request_, lines_[line_index], value) != 0) {
            const int err = errno;
            ALPACA_LOG_ERROR("GPIO", "gpiod_line_request_set_value failed on GPIO " +
                                         std::to_string(lines_[line_index]) + ": " + errno_string(err));
            return false;
        }
        return true;
    }

    void close() override {
        if (request_ != nullptr) {
            ::gpiod_line_request_release(request_);
            request_ = nullptr;
        }
        if (chip_ != nullptr) {
            ::gpiod_chip_close(chip_);
            chip_ = nullptr;
        }
    }

private:
    std::string consumer_;
    gpiod_chip* chip_ = nullptr;
    gpiod_line_request* request_ = nullptr;
    std::vector<unsigned int> lines_{};
};

}  // namespace alpacacore::util
