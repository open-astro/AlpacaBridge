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

// The NexStar hand-controller wire both FakeSynScanHandset and FakeCelestronHandset speak in ConformU mode (#2692):
// position pairs as fractions of a turn, and the eight-byte site and clock records. The motion is
// FakeMountMotionModel's, not here: these functions only convert between its degrees and the wire.

#ifndef _WIN32

#include <alpacacore/util/pointing.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

#include "fake_mount_motion_model.h"

namespace alpacacore::test::hc_wire {

constexpr double kCountsPerTurn24 = 16777216.0;
constexpr double kCountsPerTurn16 = 65536.0;
// Fixed-rate moves 1-9 as multiples of the sidereal rate (the hand control's rate keys).
constexpr double kFixedRateSidereal[10] = {0.0, 1.0, 2.0, 8.0, 16.0, 32.0, 64.0, 128.0, 600.0, 800.0};

// "RRRRRR00,DDDDDD00#" (precise) or "RRRR,DDDD#": each axis a fraction of a turn, as the handset reports it.
inline std::string axis_pair(double first_degrees, double second_degrees, bool precise) {
    const double denom = precise ? kCountsPerTurn24 : kCountsPerTurn16;
    const auto count = [denom](double degrees) {
        return static_cast<unsigned>(std::llround(util::wrap_degrees(degrees) / 360.0 * denom)) %
               static_cast<unsigned>(denom);
    };
    char out[32];
    if (precise) {
        std::snprintf(out, sizeof out, "%06X00,%06X00#", count(first_degrees), count(second_degrees));
    } else {
        std::snprintf(out, sizeof out, "%04X,%04X#", count(first_degrees), count(second_degrees));
    }
    return out;
}

// The two angles of an aim or sync command ("rRRRRRRRR,DDDDDDDD", "RRRRR,DDDD"), in degrees; false if malformed.
inline bool parse_aim(const std::string& chunk, double& first_degrees, double& second_degrees) {
    const bool precise = chunk.size() >= 18 && chunk[9] == ',';
    const bool coarse = chunk.size() >= 10 && chunk[5] == ',';
    if (!precise && !coarse) return false;
    const std::size_t digits = precise ? 6 : 4;
    const double denom = precise ? kCountsPerTurn24 : kCountsPerTurn16;
    try {
        first_degrees = std::stoul(chunk.substr(1, digits), nullptr, 16) / denom * 360.0;
        second_degrees = std::stoul(chunk.substr(chunk.find(',') + 1, digits), nullptr, 16) / denom * 360.0;
    } catch (const std::exception&) {
        return false;
    }
    if (second_degrees > 180.0) second_degrees -= 360.0;  // Dec counts wrap through 0: south is the top half
    return true;
}

// The site as eight bytes: latitude degrees, minutes, seconds, sign (1 south), then the longitude the same way.
inline std::string site_bytes(const FakeMountMotionModel& model) {
    std::string out;
    for (const double value : {model.site_latitude_degrees(), model.site_longitude_degrees()}) {
        const double total = std::fabs(value);
        const int degrees = static_cast<int>(total);
        const int minutes = static_cast<int>((total - degrees) * 60.0);
        const int seconds = static_cast<int>(std::lround(((total - degrees) * 60.0 - minutes) * 60.0));
        out += static_cast<char>(degrees);
        out += static_cast<char>(minutes);
        out += static_cast<char>(seconds);
        out += static_cast<char>(value < 0.0 ? 1 : 0);
    }
    return out;
}

// A 'W' site write: the eight bytes of site_bytes() after the command letter.
inline void apply_site(FakeMountMotionModel& model, const std::string& chunk) {
    if (chunk.size() < 9) return;
    const auto byte = [&](std::size_t i) { return static_cast<double>(static_cast<unsigned char>(chunk[i])); };
    const double lat = byte(1) + byte(2) / 60.0 + byte(3) / 3600.0;
    const double lon = byte(5) + byte(6) / 60.0 + byte(7) / 3600.0;
    model.set_site(chunk[4] != 0 ? -lat : lat, chunk[8] != 0 ? -lon : lon);
}

// The handset clock as eight bytes (hour, minute, second, month, day, year from 2000, time zone, daylight saving): the
// model's UTC plus the offset an H write set.
inline std::string time_bytes(const FakeMountMotionModel& model, std::int64_t offset_s) {
    const auto at = model.now() + std::chrono::seconds(offset_s);
    const std::time_t t = std::chrono::system_clock::to_time_t(at);
    std::tm utc{};
    gmtime_r(&t, &utc);
    std::string out;
    for (const int v : {utc.tm_hour, utc.tm_min, utc.tm_sec, utc.tm_mon + 1, utc.tm_mday, utc.tm_year - 100, 0, 0}) {
        out += static_cast<char>(v);
    }
    return out;
}

// An 'H' time write: keeps how far the written time is from the model's clock.
inline void apply_time(const FakeMountMotionModel& model, const std::string& chunk,
                       std::atomic<std::int64_t>& offset_s) {
    if (chunk.size() < 7) return;
    std::tm tm{};
    tm.tm_hour = static_cast<unsigned char>(chunk[1]);
    tm.tm_min = static_cast<unsigned char>(chunk[2]);
    tm.tm_sec = static_cast<unsigned char>(chunk[3]);
    tm.tm_mon = static_cast<unsigned char>(chunk[4]) - 1;
    tm.tm_mday = static_cast<unsigned char>(chunk[5]);
    tm.tm_year = static_cast<unsigned char>(chunk[6]) + 100;
    const auto written = std::chrono::system_clock::from_time_t(timegm(&tm));
    offset_s.store(std::chrono::duration_cast<std::chrono::seconds>(written - model.now()).count());
}

}  // namespace alpacacore::test::hc_wire

#endif  // !_WIN32
