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

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>

namespace alpacacore::test {

// #2409: the smallest landing model for the NexStar hand-controller fakes. A GOTO the fake accepts on the
// hand-controller path ('r' or 'R') moves the reported position (the 'e'/'E' reply) to its target, so the driver's
// landing gate sees the mount arrive; a fake that sets `miss` keeps the old position, a GOTO that came to rest far
// from its target. A pass-through motor GOTO pair (a forced meridian flip) moves it too.
class CelestronLanding {
public:
    explicit CelestronLanding(std::string fixed = "12AB0500,20000500") : fixed_(std::move(fixed)) {}

    void land(const std::string& goto_command, bool miss = false) {
        if (!miss && goto_command.size() >= 7 && goto_command[0] == 'P' &&
            static_cast<unsigned char>(goto_command[3]) == 0x02) {
            land_motor(goto_command);
            return;
        }
        const auto comma = goto_command.find(',');
        if (miss || goto_command.empty() || (goto_command[0] != 'r' && goto_command[0] != 'R') ||
            comma == std::string::npos) {
            return;
        }
        std::string ra = goto_command.substr(1, comma - 1);
        std::string dec = goto_command.substr(comma + 1);
        if (goto_command[0] == 'R') {  // 16-bit fractions: the precise reply carries 32 bits
            ra += "0000";
            dec += "0000";
        }
        std::lock_guard<std::mutex> lock(mutex_);
        landed_ = ra + "," + dec;
    }

    /// The position reply without its '#': the last GOTO's target, or the fixed position before any.
    std::string position() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return landed_.empty() ? fixed_ : landed_;
    }

private:
    // A pass-through MC_GOTO_FAST per axis (device 0x10 RA, 0x11 Dec; 24-bit fractions of a turn), as the driver's
    // forced meridian flip sends them, RA first: the Dec GOTO lands the pair. A Dec encoder past 90 deg is the
    // through-the-pole side (RA + 12 h, Dec 180 - encoder).
    void land_motor(const std::string& command) {
        const std::uint32_t raw = (static_cast<std::uint32_t>(static_cast<unsigned char>(command[4])) << 16) |
                                  (static_cast<std::uint32_t>(static_cast<unsigned char>(command[5])) << 8) |
                                  static_cast<std::uint32_t>(static_cast<unsigned char>(command[6]));
        std::lock_guard<std::mutex> lock(mutex_);
        if (static_cast<unsigned char>(command[2]) == 0x10) {
            ra_motor_ = raw;
            return;
        }
        constexpr double kTurn = 16777216.0;
        double dec = raw / kTurn * 360.0;
        double ra = ra_motor_ / kTurn * 24.0;
        if (dec > 90.0 && dec < 270.0) {
            dec = 180.0 - dec;
            ra = std::fmod(ra + 12.0, 24.0);
        } else if (dec >= 270.0) {
            dec -= 360.0;
        }
        const auto ra_raw = static_cast<std::uint32_t>(ra / 24.0 * kTurn) & 0xFFFFFFu;
        const auto dec_raw =
            static_cast<std::uint32_t>(std::llround((dec < 0 ? dec + 360.0 : dec) / 360.0 * kTurn)) & 0xFFFFFFu;
        char text[32];
        std::snprintf(text, sizeof(text), "%06X00,%06X00", ra_raw, dec_raw);
        landed_ = text;
    }

    const std::string fixed_;
    std::uint32_t ra_motor_ = 0;
    mutable std::mutex mutex_;
    std::string landed_;
};

}  // namespace alpacacore::test
