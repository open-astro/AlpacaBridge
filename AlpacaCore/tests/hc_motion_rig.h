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

// A hand-controller fake in ConformU mode (FakeSynScanHandset or FakeCelestronHandset, #2692) on a clock the case
// advances, and the reads a case makes of it: the cases talk to the responder directly, as
// test_ioptron_fake_motion_mode.cpp does, so a case moves time by hand and never sleeps.

#ifndef _WIN32

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>

#include "fake_mount_motion_model.h"
#include "fake_task_clock.h"
#include "nexstar_hc_wire.h"

namespace alpacacore::test {

template <typename Handset>
struct HcMotionRig {
    FakeTaskClock task_clock;
    std::shared_ptr<FakeMountMotionModel::TimePoint> now = std::make_shared<FakeMountMotionModel::TimePoint>(
        FakeMountMotionModel::TimePoint{std::chrono::seconds(1767225600)});  // 2026-01-01 00:00:00 UTC
    Handset handset;

    HcMotionRig() : handset(task_clock, config(now)) {}

    static FakeMountMotionModel::Config config(const std::shared_ptr<FakeMountMotionModel::TimePoint>& clock) {
        auto c = Handset::conformu_motion_config();
        c.clock = [clock] { return *clock; };
        return c;
    }
    void advance(std::chrono::seconds by) { *now += by; }
    std::string ask(const std::string& command) { return handset.responder()(command); }

    // The precise "RRRRRR00,DDDDDD00" pair a position command carries, without the reply's '#'.
    static std::string pair_of(double first_degrees, double second_degrees) {
        std::string pair = hc_wire::axis_pair(first_degrees, second_degrees, true);
        pair.pop_back();
        return pair;
    }
    void read_pair(double& first_degrees, double& second_degrees) {
        const std::string reply = ask("e");
        first_degrees = std::stoul(reply.substr(0, 6), nullptr, 16) / hc_wire::kCountsPerTurn24 * 360.0;
        second_degrees = std::stoul(reply.substr(9, 6), nullptr, 16) / hc_wire::kCountsPerTurn24 * 360.0;
        if (second_degrees > 180.0) second_degrees -= 360.0;
    }
    double ra_hours() {
        double ra = 0.0, dec = 0.0;
        read_pair(ra, dec);
        return ra / 15.0;
    }
    double dec_degrees() {
        double ra = 0.0, dec = 0.0;
        read_pair(ra, dec);
        return dec;
    }
    bool slewing() { return ask("L") == "1#"; }
    // The shortest wrapped difference of two RA values, hours.
    static double ra_gap_hours(double a, double b) { return std::remainder(a - b, 24.0); }
};

}  // namespace alpacacore::test

#endif  // !_WIN32
