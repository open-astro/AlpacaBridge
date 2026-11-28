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

#include <alpacacore/switch_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_switch_driver.h>

#include <memory>

#include "fake_gpio_line_backend.h"
#include "fake_task_clock.h"

namespace alpacacore::test {

// The iMate PowerBox's GPIO side for the tests and the fake ConformU run (decision 0027): a virtual clock and the
// FakeGpioLineBackend stamped by it, bundled so one object outlives the driver built over them. The driver is the
// production one, util::PowerPortSwitchDriver over util::SoftPwmPorts, so nothing here models the board: a
// case reads the backend's recorded events. On the default layout the backend's line index 0 is DC1 (GPIO 118)
// and 1 is DC2 (GPIO 114); DC3 has no line. Every line reads high until set_level() says otherwise, as the stock
// boot leaves DC1 and DC2.
//
// KNOWN PARITY GAPS (the real backend is harsher here): open() fails only after fail_open(); the real one fails
// on a missing chip, a busy line or a bad offset. The fake ConformU server drives the driver on the wall clock
// (it passes no clock), so a PWM port there runs in real time; the clock here only stamps the backend's events.
struct FakeIoptronGpio {
    FakeTaskClock clock;
    FakeGpioLineBackend gpio{clock};

    std::unique_ptr<SwitchDriver> make(const vendor::ioptron::IoptronSwitchConfig& config) {
        return vendor::ioptron::create_ioptron_switch(0, config, gpio, clock);
    }
};

}  // namespace alpacacore::test
