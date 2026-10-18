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

#include <alpacacore/telescope_driver.h>

#include <chrono>
#include <thread>

#include "catch2_compat.h"

namespace alpacacore::test {

// SiderealTime must follow the host clock to well under a second. A helper that
// cut UTC to whole seconds moved it in 1.0027 s steps, so an hour-angle park
// target landed a second of RA (15 arcsec) off whenever SetPark and Park
// straddled a second boundary (#1298). Reads twice, 300 ms apart, timing each
// read, and requires the change to equal the sidereal time that passed between
// the two clock samples to within 10 ms. A whole-second LST can only move by 0
// or 1.0027 s, which falls outside that window unless the reads are ~1 s apart.
inline void check_sidereal_time_follows_the_clock(const alpacacore::TelescopeDriver& driver) {
    using steady = std::chrono::steady_clock;
    constexpr double kSiderealPerSolar = 1.00273790935;
    const auto before_first = steady::now();
    const double first = driver.get_sidereal_time();
    const auto after_first = steady::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto before_second = steady::now();
    const double second = driver.get_sidereal_time();
    const auto after_second = steady::now();

    double moved_s = (second - first) * 3600.0;
    if (moved_s < -43200.0) {
        moved_s += 86400.0;  // wrapped past 24 h between the reads
    }
    const double shortest_s = std::chrono::duration<double>(before_second - after_first).count() * kSiderealPerSolar;
    const double longest_s = std::chrono::duration<double>(after_second - before_first).count() * kSiderealPerSolar;
    INFO("SiderealTime moved " << moved_s << " s; the reads were " << shortest_s << " to " << longest_s
                               << " sidereal s apart");
    CHECK(moved_s >= shortest_s - 0.01);
    CHECK(moved_s <= longest_s + 0.01);
}

}  // namespace alpacacore::test
