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
#include <alpacacore/vendor/touptek/touptek_switch_driver.h>

#include <memory>

#include "fake_gpio_line_backend.h"
#include "fake_task_clock.h"

namespace alpacacore::test {

// The StellaVita power box's GPIO side for the tests and the fake ConformU run (decision 0027): a virtual clock and
// the FakeGpioLineBackend stamped by it, bundled so one object outlives the driver built over them. The file keeps
// its name from the TouptekPowerboxSDK seam it replaced, so the ConformU report and run-list record of the row keep
// theirs. The driver is the production one, util::PowerPortSwitchDriver over util::SoftPwmPorts, so nothing here
// models the board: a case reads the backend's recorded events. On the default layout the backend's line index is
// the port index (Port 1 GPIO 18, Port 2 GPIO 10, Port 3 GPIO 17, Port 4 GPIO 4); a read-only port has no line and
// shifts the later ones down. Every line reads high until set_level() says otherwise, as config.txt leaves them.
//
// KNOWN PARITY GAPS (the real backend is harsher here): open() fails only after fail_open() or while its own
// claim is held; the real one also fails on a missing chip, a line another process holds or a bad offset. The fake
// ConformU server drives the driver on the wall clock (it passes no clock), so a PWM port there runs in real time; the
// clock here only stamps the backend's events.
struct FakeTouptekPowerbox {
    FakeTaskClock clock;
    FakeGpioLineBackend gpio{clock};

    std::unique_ptr<SwitchDriver> make(const vendor::touptek::TouptekSwitchConfig& config) {
        return vendor::touptek::create_touptek_switch(0, config, gpio, clock);
    }
};

}  // namespace alpacacore::test
