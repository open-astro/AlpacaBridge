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

// OnStep getters and DeviceState served from the polled snapshot (decision 0010, OnStep adoption; link-loss policy
// of decision 0009; issue #1783). The driver runs on a FakeTaskClock, so its status poll ticks only when a case
// advances the clock or calls poll_status_now_for_testing(), and the fake mount counts every exchange: a getter
// that asked the mount would show in the count (and pay the injected latency).

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>
#include <alpacacore/vendor/onstep/onstep_telescope_driver.h>

#include <algorithm>
#include <atomic>
#include <catch2/catch_approx.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"

namespace onstep = alpacacore::vendor::onstep;
using Catch::Approx;

namespace {

using alpacacore::test::error_code_of;
using alpacacore::test::FakeTaskClock;
using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr auto kLatency = milliseconds(100);

// #2241: the fake mount's offset motion. The rate offsets are arcseconds per sidereal second (:SXTR/:SXTD, OnStepX
// Mount.command.cpp), a sidereal second being this many SI seconds, and the mount moves in whole steps of this many
// arcseconds on each axis (the TeSeek HM-17's 7182.222 steps per degree, :GXA1#/:GXA2#).
constexpr double kSiSecondsPerSiderealSecond = 0.9972695663;
constexpr double kStepArcsec = 0.5;
constexpr double kRaAxisArcsecPerHour = 54000.0;  // 15 x 3600: RA is compared as the axis steps

// "HH:MM:SS.SSSS#" and "sDD*MM:SS.SSS#", the high-precision :GRa#/:GDe# replies, rounded to their last digit. The
// parsers answer NaN for a reply that does not parse (they run on the fake's thread too, where no REQUIRE may).
std::string format_ra_reply(double hours) {
    constexpr long long kPerHour = 3600LL * 10000LL;
    long long total = std::llround(hours * static_cast<double>(kPerHour)) % (24LL * kPerHour);
    if (total < 0) total += 24LL * kPerHour;
    char out[40];
    std::snprintf(out, sizeof(out), "%02lld:%02lld:%02lld.%04lld#", total / kPerHour, total / 600000LL % 60LL,
                  total / 10000LL % 60LL, total % 10000LL);
    return out;
}
std::string format_dec_reply(double degrees) {
    const long long total = std::llround(std::fabs(degrees) * 3600.0 * 1000.0);
    char out[40];
    std::snprintf(out, sizeof(out), "%c%02lld*%02lld:%02lld.%03lld#", degrees < 0.0 ? '-' : '+', total / 3600000LL,
                  total / 60000LL % 60LL, total / 1000LL % 60LL, total % 1000LL);
    return out;
}
double parse_ra_reply(const std::string& text) {
    int h = 0;
    int m = 0;
    double sec = 0.0;
    if (std::sscanf(text.c_str(), "%d:%d:%lf", &h, &m, &sec) != 3) return std::numeric_limits<double>::quiet_NaN();
    return h + m / 60.0 + sec / 3600.0;
}
double parse_dec_reply(const std::string& text) {
    char sign = '+';
    int d = 0;
    int m = 0;
    double sec = 0.0;
    if (std::sscanf(text.c_str(), "%c%d*%d:%lf", &sign, &d, &m, &sec) != 4) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double value = d + m / 60.0 + sec / 3600.0;
    return sign == '-' ? -value : value;
}

// An OnStepX that answers what the driver asks, idle and tracking unless told otherwise, and counts every exchange.
struct SnapshotMount {
    std::atomic<int> exchanges{0};
    std::atomic<int> latency_ms{0};
    std::atomic<bool> mute{false};
    std::atomic<bool> garble_site{false};
    std::atomic<int> ra_seconds{0};            // :GRa# answers 12:00:<ra_seconds>
    std::atomic<bool> refuse_park{false};      // :hP# answers 0 and :GE# 22, "mount still in motion"
    std::atomic<bool> garble_position{false};  // :GRa# answers something that does not parse
    std::atomic<bool> garble_altaz{false};     // :GA# answers something that does not parse
    std::atomic<bool> block_status{false};     // :GU# holds its reply until this is cleared
    std::atomic<bool> handshake_only{false};   // answers the handshake and nothing else (not :GU#, position, Alt/Az)
    std::atomic<bool> silent_status{false};    // :GU# gets no reply, so its read times out
    std::atomic<bool> overlong_goto{false};    // :MS# answers more than one reply may hold, so its send throws
    std::atomic<int> alt_seconds{0};           // :GA# answers +45*00:<alt_seconds>
    std::mutex mutex;
    // The pier-side settings (#2179), under `mutex`. An empty reply is answered like an unknown command ("0#").
    std::string version = "10.23a";   // :GVN#
    std::string gx96;                 // :GX96# preferred side, E/W/B/A; empty: not answered
    std::string gx94 = "2";           // :GX94# flips switch, "<n>" on, "<n> N" off
    std::string gxe9 = "60";          // :GXE9# east meridian limit, minutes
    std::string gxea = "60";          // :GXEA# west meridian limit, minutes
    std::string gxee_min = "-180";    // :GXEe# RA-axis minimum, degrees
    std::string gxee_max = "180";     // :GXEw# RA-axis maximum, degrees
    std::string ms_reply = "0";       // :MS#
    // The rate commands (#2214), under `mutex`. An empty reply is answered like an unknown command ("0#").
    std::string gt;                   // :GT# tracking rate in Hz ("0" when not tracking)
    std::string gxtr;                 // :GXTR# RA rate offset; :SXTR,n# stores its value here
    std::string gxtd;                 // :GXTD# Dec rate offset; :SXTD,n# stores its value here
    std::string gx90;                 // :GX90# pulse-guide rate
    std::string sxt_reply = "1";      // :SXTR/:SXTD, a bare digit
    std::string ge_code;              // :GE# when not empty, e.g. "15#"
    std::string mn_reply = "0";       // :MNe#/:MNw# (empty: no reply)
    std::string ra_text;              // :GRa# when not empty
    std::string gxef = "0";           // :GXEF# tangent arm, a bare digit
    std::string gxeg = "0";           // :GXEG# sector gear, a bare digit
    std::string gx93;                 // :GX93# base slew period, us/step (empty: "0#")
    std::string gxe4;                 // :GXE4# axis 1 steps per degree (empty: "0#")
    std::string dec_text;             // :GDe# when not empty
    std::string status = "NpEW180#";  // tracking, no GOTO, not parked (under `mutex`)
    std::vector<std::string> log;     // under `mutex`
    // Runs on the server thread for each command before it is answered (under `mutex` to set, called outside it).
    std::function<void(const std::string&)> on_command;
    // #2241: with `offsets_move` set, :GRa#/:GDe# move with the virtual clock at the offsets :SXTR/:SXTD stored (in
    // gxtr/gxtd), in whole kStepArcsec steps of each axis; `stall_ra`/`stall_dec` stop an axis while its offset still
    // reads back. The motion is under `mutex`; `clock` is the Rig's.
    std::atomic<bool> offsets_move{false};
    std::atomic<bool> stall_ra{false};
    std::atomic<bool> stall_dec{false};
    std::atomic<const FakeTaskClock*> clock{nullptr};
    double ra_motion_arcsec = 0.0;
    double dec_motion_arcsec = 0.0;
    std::optional<FakeTaskClock::clock::time_point> motion_at;

    // The motion up to now at the stored offsets. Under `mutex`.
    void advance_motion_locked() {
        const auto now = clock.load()->now();
        if (motion_at) {
            const double sidereal =
                std::chrono::duration<double>(now - *motion_at).count() / kSiSecondsPerSiderealSecond;
            if (!stall_ra.load()) ra_motion_arcsec += std::strtod(gxtr.c_str(), nullptr) * sidereal;
            if (!stall_dec.load()) dec_motion_arcsec += std::strtod(gxtd.c_str(), nullptr) * sidereal;
        }
        motion_at = now;
    }
    // The whole steps the axis has made.
    static double stepped(double arcsec) { return std::trunc(arcsec / kStepArcsec) * kStepArcsec; }
    // What :GRa#/:GDe# answer now with the motion on. Under `mutex`.
    std::string moving_ra_locked() {
        advance_motion_locked();
        double base = 12.0 + ra_seconds.load() / 3600.0;
        if (!ra_text.empty()) base = parse_ra_reply(ra_text);
        return format_ra_reply(base + stepped(ra_motion_arcsec) / kRaAxisArcsecPerHour);
    }
    std::string moving_dec_locked() {
        advance_motion_locked();
        const double base = dec_text.empty() ? 45.0 : parse_dec_reply(dec_text);
        return format_dec_reply(std::clamp(base + stepped(dec_motion_arcsec) / 3600.0, -90.0, 90.0));
    }
    // The position :GRa#/:GDe# would answer now, without counting an exchange.
    double ra_now() {
        std::lock_guard<std::mutex> lock(mutex);
        return parse_ra_reply(moving_ra_locked());
    }
    double dec_now() {
        std::lock_guard<std::mutex> lock(mutex);
        return parse_dec_reply(moving_dec_locked());
    }
    // The mount's position, without the motion so far (which goes on from here).
    void set_position(double ra_hours, double dec_degrees) {
        std::lock_guard<std::mutex> lock(mutex);
        ra_text = format_ra_reply(ra_hours);
        dec_text = format_dec_reply(dec_degrees);
    }
    // Offsets set at the mount by something other than this driver (a handset, another client), from now on.
    void set_offsets_elsewhere(const std::string& ra, const std::string& dec) {
        std::lock_guard<std::mutex> lock(mutex);
        advance_motion_locked();
        gxtr = ra;
        gxtd = dec;
    }

    void set_on_command(std::function<void(const std::string&)> hook) {
        std::lock_guard<std::mutex> lock(mutex);
        on_command = std::move(hook);
    }
    void set_status(const std::string& s) {
        std::lock_guard<std::mutex> lock(mutex);
        status = s;
    }
    std::size_t count(const std::string& prefix) {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<std::size_t>(
            std::count_if(log.begin(), log.end(), [&](const std::string& c) { return c.rfind(prefix, 0) == 0; }));
    }
    // The commands sent so far, in order.
    std::vector<std::string> commands() {
        std::lock_guard<std::mutex> lock(mutex);
        return log;
    }
    void clear_log() {
        std::lock_guard<std::mutex> lock(mutex);
        log.clear();
    }
    // The mount points at hour angle `ha_hours` and Dec `dec_degrees`, for a driver whose sidereal time is `lst`.
    void point_at(double lst_hours, double ha_hours, double dec_degrees) {
        double ra = std::fmod(lst_hours - ha_hours, 24.0);
        if (ra < 0.0) ra += 24.0;
        const double total_seconds = std::round(ra * 3600.0 * 10000.0) / 10000.0;
        const int hours = static_cast<int>(total_seconds / 3600.0);
        const int minutes = static_cast<int>((total_seconds - hours * 3600.0) / 60.0);
        const double seconds = total_seconds - hours * 3600.0 - minutes * 60.0;
        char out[40];
        std::snprintf(out, sizeof(out), "%02d:%02d:%07.4f#", hours, minutes, seconds);
        const double abs_dec = std::fabs(dec_degrees);
        const int dec_d = static_cast<int>(abs_dec);
        char dec_out[40];
        std::snprintf(dec_out, sizeof(dec_out), "%c%02d*%02d:%06.3f#", dec_degrees < 0 ? '-' : '+', dec_d,
                      static_cast<int>((abs_dec - dec_d) * 60.0), std::fmod((abs_dec - dec_d) * 3600.0, 60.0));
        std::lock_guard<std::mutex> lock(mutex);
        ra_text = out;
        dec_text = dec_out;
    }

    std::string answer(const std::string& chunk) {
        if (chunk.rfind(":GVP", 0) == 0) return "OnStep#";
        if (chunk.rfind(":GVN", 0) == 0) {
            std::lock_guard<std::mutex> lock(mutex);
            return version + "#";
        }
        if (chunk.rfind(":GXEE", 0) == 0) return "1";
        if (chunk.rfind(":GXEF", 0) == 0 || chunk.rfind(":GXEG", 0) == 0) {
            std::lock_guard<std::mutex> lock(mutex);
            return chunk.rfind(":GXEF", 0) == 0 ? gxef : gxeg;  // bare, no '#'
        }
        if (chunk.rfind(":GX96", 0) == 0 || chunk.rfind(":GX94", 0) == 0 || chunk.rfind(":GXE9", 0) == 0 ||
            chunk.rfind(":GXEA", 0) == 0 || chunk.rfind(":GXEe", 0) == 0 || chunk.rfind(":GXEw", 0) == 0) {
            std::lock_guard<std::mutex> lock(mutex);
            const std::string& value = chunk.rfind(":GX96", 0) == 0   ? gx96
                                       : chunk.rfind(":GX94", 0) == 0 ? gx94
                                       : chunk.rfind(":GXE9", 0) == 0 ? gxe9
                                       : chunk.rfind(":GXEA", 0) == 0 ? gxea
                                       : chunk.rfind(":GXEe", 0) == 0 ? gxee_min
                                                                      : gxee_max;
            return value.empty() ? "0#" : value + "#";
        }
        if (silent_status.load() && chunk.rfind(":GU", 0) == 0) return "";
        if (overlong_goto.load() && chunk.rfind(":MS", 0) == 0) return std::string(65600, '0');
        if (handshake_only.load()) {
            if (chunk.rfind(":GU", 0) == 0) return "";  // no reply: the status parser would take "?#"
            if (chunk.rfind(":GR", 0) == 0 || chunk.rfind(":GD", 0) == 0 || chunk.rfind(":GA", 0) == 0 ||
                chunk.rfind(":GZ", 0) == 0) {
                return "?#";
            }
        }
        if (chunk.rfind(":GU", 0) == 0) {
            while (block_status.load()) std::this_thread::sleep_for(milliseconds(1));
            std::lock_guard<std::mutex> lock(mutex);
            return status;
        }
        if (chunk.rfind(":GRa", 0) == 0) {
            if (garble_position.load()) return "?#";
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (offsets_move.load()) return moving_ra_locked();
                if (!ra_text.empty()) return ra_text;
            }
            char out[32];
            std::snprintf(out, sizeof(out), "12:00:%02d.0000#", ra_seconds.load());
            return out;
        }
        if (chunk.rfind(":GDe", 0) == 0) {
            std::lock_guard<std::mutex> lock(mutex);
            if (offsets_move.load()) return moving_dec_locked();
            return dec_text.empty() ? "+45*00:00.000#" : dec_text;
        }
        if (chunk.rfind(":GA", 0) == 0) {
            if (garble_altaz.load()) return "?#";
            char out[32];
            std::snprintf(out, sizeof(out), "+45*00:%02d#", alt_seconds.load());
            return out;
        }
        if (chunk.rfind(":GZ", 0) == 0) return "180*00:00#";
        if (chunk.rfind(":GL", 0) == 0) return "10:20:30#";
        if (chunk.rfind(":GC", 0) == 0) return "10/28/26#";
        if (chunk.rfind(":GG", 0) == 0) return "+00:00#";
        if (chunk.rfind(":Gt", 0) == 0) return garble_site.load() ? "?#" : "+40*30#";
        if (chunk.rfind(":Gg", 0) == 0) return garble_site.load() ? "?#" : "+074*00#";
        if (chunk.rfind(":Gh", 0) == 0) return "-10*#";
        if (chunk.rfind(":Go", 0) == 0) return "90*#";
        if (chunk.rfind(":GE", 0) == 0) {
            std::lock_guard<std::mutex> lock(mutex);
            if (!ge_code.empty()) return ge_code;
            return refuse_park.load() ? "22#" : "00#";
        }
        if (chunk.rfind(":GT", 0) == 0 || chunk.rfind(":GXTR", 0) == 0 || chunk.rfind(":GXTD", 0) == 0 ||
            chunk.rfind(":GX90", 0) == 0 || chunk.rfind(":GX93", 0) == 0 || chunk.rfind(":GXE4", 0) == 0) {
            std::lock_guard<std::mutex> lock(mutex);
            const std::string& value = chunk.rfind(":GT", 0) == 0     ? gt
                                       : chunk.rfind(":GXTR", 0) == 0 ? gxtr
                                       : chunk.rfind(":GXTD", 0) == 0 ? gxtd
                                       : chunk.rfind(":GX93", 0) == 0 ? gx93
                                       : chunk.rfind(":GXE4", 0) == 0 ? gxe4
                                                                      : gx90;
            return value.empty() ? "0#" : value + "#";
        }
        if (chunk.rfind(":SXTR,", 0) == 0 || chunk.rfind(":SXTD,", 0) == 0) {
            std::lock_guard<std::mutex> lock(mutex);
            if (sxt_reply != "1") return sxt_reply;
            // The firmware clamps at +/-1800 and reads back eight decimals (Mount.command.cpp, af1d26bf).
            const double value = std::clamp(std::strtod(chunk.c_str() + 6, nullptr), -1800.0, 1800.0);
            if (offsets_move.load()) advance_motion_locked();  // the old offset up to now, the new one from here
            char text[40];
            std::snprintf(text, sizeof(text), "%.8f", value);
            (chunk[4] == 'R' ? gxtr : gxtd) = text;
            return "1";
        }
        if (chunk.rfind(":TQ", 0) == 0 || chunk.rfind(":TL", 0) == 0 || chunk.rfind(":TS", 0) == 0 ||
            chunk.rfind(":TK", 0) == 0) {
            return "";  // no reply (Mount.command.cpp, af1d26bf)
        }
        if (chunk.rfind(":hP", 0) == 0 && refuse_park.load()) return "0";
        if (chunk.rfind(":CM", 0) == 0) return "N/A#";
        if (chunk.rfind(":Te", 0) == 0) return "1";  // tracking enable (OnStepX): a bare digit
        if (chunk.rfind(":MS", 0) == 0) {
            std::lock_guard<std::mutex> lock(mutex);
            return ms_reply;
        }
        if (chunk.rfind(":MN", 0) == 0) {
            std::lock_guard<std::mutex> lock(mutex);
            return mn_reply;
        }
        if (chunk.rfind(":Q", 0) == 0 || chunk.rfind(":M", 0) == 0 || chunk.rfind(":R", 0) == 0) return "";
        if (chunk.rfind(":hC", 0) == 0) return "";
        if (chunk.rfind(":S", 0) == 0 || chunk.rfind(":h", 0) == 0) return "1";
        return "0#";
    }

    std::string reply(const std::string& chunk) {
        exchanges.fetch_add(1);
        std::function<void(const std::string&)> hook;
        {
            std::lock_guard<std::mutex> lock(mutex);
            log.push_back(chunk);
            hook = on_command;
        }
        if (hook) hook(chunk);
        if (mute.load()) return "";
        std::string r = answer(chunk);
        if (!r.empty() && latency_ms.load() > 0) std::this_thread::sleep_for(milliseconds(latency_ms.load()));
        return r;
    }
};

struct Rig {
    std::shared_ptr<SnapshotMount> mount = std::make_shared<SnapshotMount>();
    FakeTaskClock clock;
    alpacacore::test::FakeMountServer server{[m = mount](const std::string& chunk) { return m->reply(chunk); }};
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    onstep::ConnectionInfo info;

    // Without a site in the config the connect reads the mount's (:Gt#/:Gg#).
    explicit Rig(bool with_site = true, int response_timeout_ms = 300) {
        info.type = onstep::ConnectionType::Network;
        info.host = "127.0.0.1";
        info.tcp_port = server.port();
        info.response_timeout_ms = response_timeout_ms;
        driver = onstep::create_onstep_telescope_with_site(
            0, info, with_site ? std::optional<double>(-33.856784) : std::nullopt,
            with_site ? std::optional<double>(151.215297) : std::nullopt, std::nullopt, false, clock);
        mount->clock = &clock;
    }
    ~Rig() {
        mount->mute = false;
        mount->block_status = false;
        mount->latency_ms = 0;
        try {
            driver->set_connected(false);
        } catch (...) {  // NOLINT(bugprone-empty-catch): best-effort teardown
        }
        driver.reset();
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    void connect() {
        REQUIRE(server.ok());
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
    }
    void pause() { onstep::set_status_poll_paused_for_testing(*driver, true); }
    bool tick(bool full = true) { return onstep::poll_status_now_for_testing(*driver, full); }
};

bool has_entry(const alpacacore::TelescopeDriver& driver, const std::string& name) {
    const auto state = driver.get_device_state();
    return std::any_of(state.begin(), state.end(), [&](const auto& e) { return e.name == name; });
}

std::chrono::system_clock::time_point time_stamp(const alpacacore::TelescopeDriver& driver) {
    const std::string text = alpacacore::test::time_stamp_of(driver);
    REQUIRE_FALSE(text.empty());
    return alpacacore::test::parse_timestamp(text);
}

// Runs a synchronous SlewToCoordinates on a thread and steps the clock in the slew loop's 250 ms waits until it
// returns (the poll is paused, so the slew's wait is the only waiter). Rethrows the slew's exception.
void synchronous_slew(Rig& rig, double ra, double dec) {
    std::exception_ptr error;
    std::atomic<bool> done{false};
    std::thread slew([&] {
        try {
            rig.driver->slew_to_coordinates(ra, dec);
        } catch (...) {
            error = std::current_exception();
        }
        done = true;
    });
    const auto give_up = std::chrono::steady_clock::now() + seconds(20);
    while (!done.load() && std::chrono::steady_clock::now() < give_up) {
        if (rig.clock.wait_for_waiters(1, milliseconds(50))) {
            rig.clock.advance(milliseconds(250));
            (void)rig.clock.wait_for_woken_settled(seconds(5));
        }
    }
    slew.join();
    REQUIRE(done.load());
    if (error) std::rethrow_exception(error);
}

}  // namespace

TEST_CASE("OnStep snapshot - getters and DeviceState make no mount exchange at 100 ms per exchange",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.connect();
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));  // the poll is parked: the connect's reads are over
    // Past 2 s: a getter that still had a time-to-live would ask the mount from here on. The tick
    // this wakes runs to its end before the count is taken.
    rig.clock.advance(seconds(3));
    REQUIRE(rig.clock.wait_for_woken_settled(seconds(5)));
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));

    rig.mount->latency_ms = static_cast<int>(kLatency.count());
    const int before = rig.mount->exchanges.load();
    const std::vector<std::pair<const char*, std::function<void()>>> reads = {
        {"RightAscension", [&] { (void)rig.driver->get_right_ascension(); }},
        {"Declination", [&] { (void)rig.driver->get_declination(); }},
        {"Altitude", [&] { (void)rig.driver->get_altitude(); }},
        {"Azimuth", [&] { (void)rig.driver->get_azimuth(); }},
        {"Slewing", [&] { (void)rig.driver->get_slewing(); }},
        {"Tracking", [&] { (void)rig.driver->get_tracking(); }},
        {"AtPark", [&] { (void)rig.driver->get_at_park(); }},
        {"AtHome", [&] { (void)rig.driver->get_at_home(); }},
        {"SideOfPier", [&] { (void)rig.driver->get_side_of_pier(); }},
        {"SiteLatitude", [&] { (void)rig.driver->get_site_latitude(); }},
        {"SiderealTime", [&] { (void)rig.driver->get_sidereal_time(); }},
        {"UTCDate", [&] { (void)rig.driver->get_utc_date(); }},
        {"IsPulseGuiding", [&] { (void)rig.driver->get_is_pulse_guiding(); }},
        {"DeviceState", [&] { (void)rig.driver->get_device_state(); }},
    };
    for (const auto& [name, read] : reads) {
        CAPTURE(name);
        read();
    }
    CHECK(rig.mount->exchanges.load() == before);
    // DeviceState drops a getter that throws, so a read that quietly failed would still be fast: name what must
    // be in it.
    for (const char* name :
         {"RightAscension", "Declination", "Altitude", "Azimuth", "Slewing", "Tracking", "SideOfPier", "TimeStamp"}) {
        CAPTURE(name);
        CHECK(has_entry(*rig.driver, name));
    }
    rig.mount->latency_ms = 0;
}

TEST_CASE("OnStep snapshot - a write is read back at once and a poll that sampled before it does not undo it",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.connect();
    rig.pause();
    REQUIRE(rig.driver->get_tracking());

    rig.driver->set_tracking(false);
    CHECK_FALSE(rig.driver->get_tracking());  // read-after-write, with no poll between (the fake still says on)

    // A tick that sampled before a write publishes after it: the write wins.
    rig.driver->set_tracking(true);
    std::atomic<bool> once{false};
    onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, [&] {
        if (!once.exchange(true)) rig.driver->set_tracking(false);
    });
    REQUIRE(rig.tick());  // samples :GU# with tracking on, then the hook writes off
    onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, nullptr);
    CHECK_FALSE(rig.driver->get_tracking());

    // A sync, written through: the mount still answers 12:00 RA until the next tick.
    rig.driver->sync_to_coordinates(3.5, 12.25);
    CHECK(std::abs(rig.driver->get_right_ascension() - 3.5) < 1e-9);
    CHECK(std::abs(rig.driver->get_declination() - 12.25) < 1e-9);
    // A GOTO start reads as Slewing past its grace, though the fake still answers no GOTO, until a tick says so.
    rig.driver->set_tracking(true);
    rig.mount->set_status("NpEW180#");
    rig.driver->slew_to_coordinates_async(2.0, 20.0);
    rig.clock.advance(seconds(3));  // past the 2 s slew grace; the poll is paused, so no tick runs
    CHECK(rig.driver->get_slewing());
    REQUIRE(rig.tick());
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK(std::abs(rig.driver->get_right_ascension() - 12.0) < 1e-6);  // the mount's own answer again
}

TEST_CASE("OnStep snapshot - DeviceState TimeStamp is when the poll measured, not when it was asked",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.connect();
    rig.pause();
    REQUIRE(rig.tick());
    auto age = std::chrono::system_clock::now() - time_stamp(*rig.driver);
    CHECK(age < milliseconds(1000));

    // No tick while the task clock moves 5 s (inside the 30 s bound, no fault): the stamp keeps the frame's age.
    rig.clock.advance(seconds(5));
    REQUIRE(rig.driver->get_connected());
    age = std::chrono::system_clock::now() - time_stamp(*rig.driver);
    CHECK(age > milliseconds(4500));
    CHECK(age < milliseconds(6000));
    // A write-through is not a measurement: the stamp stays.
    rig.driver->set_tracking(false);
    age = std::chrono::system_clock::now() - time_stamp(*rig.driver);
    CHECK(age > milliseconds(4500));
    // The next tick is.
    REQUIRE(rig.tick());
    age = std::chrono::system_clock::now() - time_stamp(*rig.driver);
    CHECK(age < milliseconds(1000));
}

TEST_CASE(
    "OnStep snapshot - a stale frame is served, three failed ticks refuse it, and past the bound the link is "
    "lost",
    "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.connect();
    rig.pause();
    const double ra = rig.driver->get_right_ascension();

    // Stale is not lost: past kSnapshotMaxAge with no failed tick the frame is still served (decision 0010 rule 6).
    rig.clock.advance(seconds(10));
    CHECK(rig.driver->get_right_ascension() == ra);
    CHECK(rig.driver->get_link_fault().empty());

    rig.mount->mute = true;
    rig.mount->ra_seconds = 30;  // what an answering mount would now say
    CHECK(rig.tick());           // two failed ticks: the frame is still served
    CHECK(rig.tick());
    CHECK(rig.driver->get_right_ascension() == ra);
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.tick());  // the third latches the fault
    CHECK(rig.driver->get_connected());
    CHECK(error_code_of([&] { (void)rig.driver->get_right_ascension(); }) == alpacacore::AlpacaError::DriverException);
    CHECK(error_code_of([&] { (void)rig.driver->get_tracking(); }) == alpacacore::AlpacaError::DriverException);
    CHECK_FALSE(has_entry(*rig.driver, "RightAscension"));

    SECTION("a good tick clears the fault") {
        rig.mount->mute = false;
        CHECK(rig.tick());
        CHECK(rig.driver->get_link_fault().empty());
        CHECK(rig.driver->get_right_ascension() > ra);  // the new measurement
    }
    SECTION("a fault standing past the bound is a lost link") {
        rig.clock.advance(alpacacore::util::kLinkStalenessBound + seconds(1));
        CHECK_FALSE(rig.tick());  // the tick finds the bound and ends the body
        CHECK_FALSE(rig.driver->get_connected());
        CHECK(rig.driver->get_device_state().empty());
        CHECK(error_code_of([&] { (void)rig.driver->get_right_ascension(); }) == alpacacore::AlpacaError::NotConnected);
        rig.mount->mute = false;
        rig.driver->set_connected(true);  // only the client relinks
        CHECK(rig.driver->get_connected());
        CHECK(rig.driver->get_link_fault().empty());
    }
}

TEST_CASE("OnStep snapshot - the poll starts with the connect and ends with the disconnect",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    REQUIRE(rig.server.ok());
    CHECK(rig.clock.waiter_count() == 0);
    rig.driver->set_connected(true);
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));  // the poll body parks for its first tick
    const auto polls = rig.mount->count(":GU#");
    rig.clock.advance(seconds(1));
    REQUIRE(rig.clock.wait_for_woken_settled(seconds(5)));
    CHECK(rig.mount->count(":GU#") == polls + 1);  // one tick

    rig.driver->set_connected(false);
    CHECK(rig.clock.waiter_count() == 0);  // the body was joined
    const int exchanges = rig.mount->exchanges.load();
    rig.clock.advance(seconds(3));
    REQUIRE(rig.clock.wait_for_woken_settled(seconds(5)));  // a live poll would wake and read first
    CHECK(rig.mount->exchanges.load() == exchanges);        // nothing reads the closed link

    rig.driver->set_connected(true);  // a reconnect starts it again
    CHECK(rig.clock.wait_for_waiters(1, seconds(2)));
}

TEST_CASE("OnStep snapshot - the site getters throw until the site is known", "[onstep][telescope][snapshot]") {
    Rig rig(/*with_site=*/false);
    rig.mount->garble_site = true;  // the connect cannot read the mount's site
    rig.connect();
    rig.pause();
    for (const auto& read : std::vector<std::function<void()>>{[&] { (void)rig.driver->get_site_latitude(); },
                                                               [&] { (void)rig.driver->get_site_longitude(); }}) {
        try {
            read();
            FAIL("a site getter answered a site nobody set or read");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
            CHECK(std::string(ex.what()).find("Invalid OnStep site info response") != std::string::npos);
        }
    }
    // A client write makes that coordinate known; the other stays unknown.
    rig.clock.advance(seconds(3));  // past the site retry delay: the write's own pair read fails again
    rig.driver->set_site_latitude(12.5);
    CHECK(rig.driver->get_site_latitude() == 12.5);
    CHECK(error_code_of([&] { (void)rig.driver->get_site_longitude(); }) == alpacacore::AlpacaError::InvalidOperation);
    // The poll's full tick retries the pair once the mount answers it.
    rig.mount->garble_site = false;
    rig.clock.advance(seconds(3));
    REQUIRE(rig.tick(true));
    CHECK(std::abs(rig.driver->get_site_longitude() - (-74.0)) < 1e-9);  // west-positive on the wire
}

TEST_CASE(
    "OnStep snapshot - the poll restores tracking after a GOTO that ended without it, but not while a Park is "
    "pending",
    "[onstep][telescope][snapshot]") {
    // Declared before the rig: the held Park thread polls it until the rig's teardown joins that thread.
    std::atomic<bool> held{true};
    Rig rig;
    rig.connect();
    rig.pause();
    rig.driver->slew_to_coordinates_async(2.0, 20.0);  // starts with tracking on: a restore is owed
    rig.mount->set_status("nNpEW180#");                // the GOTO ended with tracking off
    rig.clock.advance(seconds(3));                     // past the slew grace

    SECTION("the tick that reads the end restores it") {
        REQUIRE(rig.tick());
        CHECK(rig.mount->count(":Te#") == 1);
        CHECK(rig.driver->get_tracking());
        REQUIRE(rig.tick());
        CHECK(rig.mount->count(":Te#") == 1);  // once
    }
    SECTION("a Park refused in motion and still retrying gets none") {
        // The park's first try is refused "in motion" (22), so its body is to retry; the spawn seam holds that
        // body, and the tick runs while the Park is pending.
        onstep::set_park_home_spawn_for_testing(*rig.driver, [&held](std::function<void()> body) {
            return std::thread([&held, body = std::move(body)] {
                while (held.load()) std::this_thread::sleep_for(milliseconds(1));
                body();
            });
        });
        struct Release {
            std::atomic<bool>& held;
            ~Release() { held = false; }
        } release{held};
        rig.mount->refuse_park = true;
        rig.driver->park();
        REQUIRE(rig.mount->count(":hP#") == 1);
        REQUIRE(rig.driver->get_slewing());  // the Park is pending
        REQUIRE(rig.tick());
        CHECK(rig.mount->count(":Te#") == 0);
        rig.mount->refuse_park = false;
        held = false;
    }
}

// A poll body cancelled while it samples touches neither the link's health nor the mount (decision 0006 rule 6): the
// tick that would send the GOTO's tracking restore, cancelled by a disconnect inside its :GU#, sends none.
TEST_CASE("OnStep snapshot - a poll body cancelled mid-sample sends no tracking restore",
          "[onstep][telescope][snapshot]") {
    Rig rig(/*with_site=*/true, /*response_timeout_ms=*/20000);  // the held :GU# must not time out
    rig.connect();
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));  // the poll body parks for its first tick
    rig.driver->slew_to_coordinates_async(2.0, 20.0);    // starts with tracking on: a restore is owed
    rig.mount->set_status("nNpEW180#");                  // the GOTO ended with tracking off
    const auto polls = rig.mount->count(":GU#");
    rig.mount->block_status = true;
    rig.clock.advance(seconds(3));  // past the slew grace: the tick reads the end
    for (int i = 0; i < 2000 && rig.mount->count(":GU#") == polls; ++i) std::this_thread::sleep_for(milliseconds(1));
    REQUIRE(rig.mount->count(":GU#") == polls + 1);  // the tick is inside its :GU#, holding the operation lock

    auto disconnect = std::async(std::launch::async, [&] { rig.driver->set_connected(false); });
    for (int i = 0; i < 5000 && onstep::status_poll_running_for_testing(*rig.driver); ++i) {
        std::this_thread::sleep_for(milliseconds(1));
    }
    REQUIRE_FALSE(onstep::status_poll_running_for_testing(*rig.driver));  // cancelled, still sampling
    rig.mount->block_status = false;
    REQUIRE(disconnect.wait_for(seconds(20)) == std::future_status::ready);
    disconnect.get();
    CHECK(rig.mount->count(":Te#") == 0);
}

TEST_CASE("OnStep snapshot - IsPulseGuiding stays true until a frame sampled after the pulse is published",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.connect();
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));
    const auto waits = rig.clock.wait_count();
    rig.driver->pulse_guide(2, 500);  // East, 0.5 s: the pulse starts its own slot (pulse_measurement_)
    // The status poll body is not touched; the pulse-end body's first wait is the next one counted.
    REQUIRE(rig.clock.wait_for_wait_count(waits + 1, seconds(2)));
    CHECK(rig.driver->get_is_pulse_guiding());
    const auto polls = rig.mount->count(":GRa#");

    // At the pulse's end the frame is still the one from before it.
    rig.clock.advance(milliseconds(500));
    CHECK(rig.driver->get_is_pulse_guiding());
    // 50 ms later the poll measures; then the pulse reads ended.
    rig.mount->ra_seconds = 7;
    rig.clock.advance(milliseconds(50));
    REQUIRE(rig.clock.wait_for_woken_settled(seconds(5)));
    CHECK(rig.mount->count(":GRa#") == polls + 1);
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
    CHECK(std::abs(rig.driver->get_right_ascension() - (12.0 + 7.0 / 3600.0)) < 1e-6);
}

// The 1 s ticks run on through a pulse guide; one extra tick 50 ms after its end ends the IsPulseGuiding hold.
TEST_CASE("OnStep snapshot - a pulse guide keeps the poll's 1 s ticks and adds one at the pulse's end",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.connect();
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));  // the poll waits for its first tick, 1 s away
    const auto waits = rig.clock.wait_count();
    rig.driver->pulse_guide(0, 3000);                               // North, 3 s
    REQUIRE(rig.clock.wait_for_wait_count(waits + 1, seconds(2)));  // the pulse-end tick waits too

    for (int second = 1; second <= 3; ++second) {
        CAPTURE(second);
        const auto polls = rig.mount->count(":GU#");
        rig.clock.advance(seconds(1));
        REQUIRE(rig.clock.wait_for_woken_settled(seconds(5)));
        CHECK(rig.mount->count(":GU#") == polls + 1);  // a regular tick every second, the pulse's end included
        CHECK(rig.driver->get_is_pulse_guiding());     // the tick at the end sampled before end + 50 ms
    }
    const auto polls = rig.mount->count(":GU#");
    rig.clock.advance(milliseconds(50));
    REQUIRE(rig.clock.wait_for_woken_settled(seconds(5)));
    CHECK(rig.mount->count(":GU#") == polls + 1);  // the pulse-end tick
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
}

// The synchronous slew publishes where it ended (decision 0010): read right after it returns, with no poll
// tick between, the getters serve the slew's end, not the frame from before it.
TEST_CASE("OnStep snapshot - a synchronous slew publishes the position where it ended",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.connect();
    rig.pause();
    REQUIRE(std::abs(rig.driver->get_right_ascension() - 12.0) < 1e-9);
    rig.mount->ra_seconds = 33;  // where the GOTO ends
    const auto polls = rig.mount->count(":GRa#");
    synchronous_slew(rig, 12.0, 45.0);
    CHECK(rig.mount->count(":GRa#") == polls + 1);  // the slew's own end read; the poll is paused
    CHECK(std::abs(rig.driver->get_right_ascension() - (12.0 + 33.0 / 3600.0)) < 1e-6);
    CHECK(std::abs(rig.driver->get_declination() - 45.0) < 1e-6);
    CHECK(std::abs(rig.driver->get_altitude() - 45.0) < 1e-6);
    CHECK(std::abs(rig.driver->get_azimuth() - 180.0) < 1e-6);
    CHECK_FALSE(rig.driver->get_slewing());
}

// A failed end read is a failed poll tick: the last values stay served and the failure counts toward the fault.
TEST_CASE("OnStep snapshot - a failed end-of-slew read keeps the last values and counts toward the fault",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.connect();
    rig.pause();
    const double ra = rig.driver->get_right_ascension();
    const double alt = rig.driver->get_altitude();
    rig.mount->garble_position = true;
    REQUIRE_NOTHROW(synchronous_slew(rig, 12.0, 45.0));  // the slew itself ended
    CHECK(rig.driver->get_right_ascension() == ra);
    CHECK(rig.driver->get_altitude() == alt);
    CHECK(rig.driver->get_link_fault().empty());  // one failure
    CHECK(rig.tick());                            // the second
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(rig.tick());  // the third latches: the end read counted as the first
    CHECK_FALSE(rig.driver->get_link_fault().empty());
    rig.mount->garble_position = false;
}

// A failed :GA#/:GZ# read is not a failed tick: the poll keeps the last Alt/Az and the link fault stays clear.
TEST_CASE("OnStep snapshot - a failed Alt/Az read keeps the last value and does not count toward the fault",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.connect();
    rig.pause();
    const double alt = rig.driver->get_altitude();
    REQUIRE(std::abs(alt - 45.0) < 1e-6);
    rig.mount->garble_altaz = true;
    rig.mount->alt_seconds = 36;  // what :GA# would say were it readable
    rig.mount->ra_seconds = 9;    // RA still answers: the new value is served
    // Alt/Az is read on every second tick: three full ticks, so at least the failed reads below happen.
    const auto reads = rig.mount->count(":GA#");
    for (int i = 0; i < 3; ++i) {
        CAPTURE(i);
        REQUIRE(rig.tick(true));
        CHECK(rig.driver->get_link_fault().empty());
    }
    CHECK(rig.mount->count(":GA#") == reads + 3);  // each full tick asked and failed
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(std::abs(rig.driver->get_right_ascension() - (12.0 + 9.0 / 3600.0)) < 1e-6);
    CHECK(rig.driver->get_altitude() == alt);
    rig.mount->garble_altaz = false;
}

// The connect's seed counts each failed read toward the fault, so a mount that passes the handshake but
// answers none of them connects with the fault latched (behaviour change 13).
TEST_CASE("OnStep snapshot - a mount that answers none of the connect's seed reads connects faulted",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.mount->handshake_only = true;
    rig.connect();
    rig.pause();
    CHECK_FALSE(rig.driver->get_link_fault().empty());
    CHECK_THROWS(rig.driver->get_right_ascension());
    CHECK_THROWS(rig.driver->get_altitude());
    rig.mount->handshake_only = false;
    REQUIRE(rig.tick(true));  // one good tick clears it
    CHECK(rig.driver->get_link_fault().empty());
    CHECK(std::abs(rig.driver->get_right_ascension() - 12.0) < 1e-6);
}

// A control path's :GU# read writes through only a status that differs from the frame's: an equal one leaves
// the poll sample in flight to publish, a different one drops it (decision 0010 point 4).
TEST_CASE("OnStep snapshot - a control-path status read drops the poll sample in flight only when it differs",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.mount->set_status("nNPEW180#");  // parked: Park() reads :GU# and returns, a no-op
    rig.connect();
    rig.pause();
    const double ra = rig.driver->get_right_ascension();
    rig.mount->ra_seconds = 10;  // what the tick samples
    std::string during;          // the :GU# Park() reads inside the tick's window
    onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, [&] {
        rig.mount->set_status(during);
        rig.driver->park();
    });

    SECTION("an equal status") {
        during = "nNPEW180#";
        REQUIRE(rig.tick());
        CHECK(std::abs(rig.driver->get_right_ascension() - (12.0 + 10.0 / 3600.0)) < 1e-6);  // published
    }
    SECTION("a different status") {
        during = "NPEW180#";  // still parked, now tracking
        REQUIRE(rig.tick());
        CHECK(rig.driver->get_right_ascension() == ra);  // the tick was dropped
        CHECK(rig.driver->get_tracking());               // the read was written through
    }
    onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, nullptr);
}

// The regular tick and the pulse-end tick both publish after they release the operation lock. A tick whose sample
// is older than the held frame's is skipped, so a stalled older tick cannot land its frame on top of a newer one.
TEST_CASE("OnStep snapshot - a tick's publish drops an older tick that is still between its sample and its publish",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.connect();
    rig.pause();
    rig.mount->ra_seconds = 10;  // what tick A samples
    std::atomic<bool> once{false};
    bool b_published = false;
    onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, [&] {
        if (once.exchange(true)) {
            return;  // tick B's own after-sample call
        }
        rig.mount->ra_seconds = 20;  // what tick B samples
        b_published = rig.tick();    // B samples and publishes inside A's gap
    });
    REQUIRE(rig.tick());  // A: samples 10, then B publishes 20, then A's publish is dropped
    onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, nullptr);
    REQUIRE(b_published);
    CHECK(std::abs(rig.driver->get_right_ascension() - (12.0 + 20.0 / 3600.0)) < 1e-6);
}

// A pulse-end body of an earlier pulse that takes the operation lock in the gap between a newer PulseGuide's
// unlock and its start() has not been replaced yet (decision 0006 rule 6): the newer pulse supersedes it under the
// lock, so the body's own check sees it and it sends no tracking restore and publishes no frame.
TEST_CASE("OnStep snapshot - a pulse-end body superseded by a newer PulseGuide sends no restore and publishes nothing",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.connect();
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));
    rig.driver->slew_to_coordinates_async(2.0, 20.0);  // starts with tracking on: a restore is owed
    rig.mount->set_status("npEW180#");                 // the GOTO runs, tracking off: the poll ticks restore nothing
    rig.clock.advance(seconds(3));                     // past the slew grace
    REQUIRE(rig.clock.wait_for_woken_settled(seconds(5)));
    const auto waits = rig.clock.wait_count();
    rig.driver->pulse_guide(2, 500);  // the old pulse-end body waits 550 ms
    REQUIRE(rig.clock.wait_for_wait_count(waits + 1, seconds(2)));
    rig.mount->set_status("nNpEW180#");  // the GOTO ended with tracking off: only a tick that reads it restores
    rig.mount->ra_seconds = 9;           // what a published frame would show
    const auto restores = rig.mount->count(":ST") + rig.mount->count(":Te");
    const auto reads = rig.mount->count(":GU#");
    bool body_returned = false;
    onstep::set_pulse_guide_gap_hook_for_testing(*rig.driver, [&] {
        // The newer pulse has unlocked and not yet started: the old body wakes now and takes the operation lock.
        rig.clock.advance(milliseconds(550));
        (void)rig.clock.wait_for_woken_settled(seconds(5));
        for (int i = 0; i < 5000 && onstep::pulse_measurement_running_for_testing(*rig.driver); ++i) {
            std::this_thread::sleep_for(milliseconds(1));  // real time for the body to return, not a driver timer
        }
        body_returned = !onstep::pulse_measurement_running_for_testing(*rig.driver);
    });
    rig.driver->pulse_guide(3, 500);  // the newer pulse supersedes it
    onstep::set_pulse_guide_gap_hook_for_testing(*rig.driver, nullptr);
    REQUIRE(body_returned);
    CHECK(rig.mount->count(":ST") + rig.mount->count(":Te") == restores);  // no restore from the old body
    CHECK(rig.mount->count(":GU#") == reads);                              // it did not even sample
    CHECK(std::abs(rig.driver->get_right_ascension() - 12.0) < 1e-6);      // its frame is not published
}

// Decision 0022 rule 2: the slew's hold window is armed before the :MS# goes out, so Slewing reads true while the
// GOTO is being sent and once the call returns, before any :GU# has been read.
TEST_CASE("OnStep snapshot - Slewing is true as soon as SlewToCoordinatesAsync returns",
          "[onstep][telescope][snapshot][hold-window]") {
    Rig rig;
    rig.connect();
    rig.pause();
    rig.mount->set_status("NpEW180#");  // no GOTO
    std::atomic<int> during{-1};        // Slewing read while the :MS# is in flight
    rig.mount->set_on_command([&](const std::string& chunk) {
        if (chunk.rfind(":MS", 0) == 0) during = rig.driver->get_slewing() ? 1 : 0;
    });
    rig.driver->slew_to_coordinates_async(2.0, 20.0);
    rig.mount->set_on_command(nullptr);
    CHECK(during.load() == 1);
    CHECK(rig.driver->get_slewing());
    rig.mount->set_status("npEW180#");  // the mount shows the GOTO from its first :GU# (rig, 2026-10-29)
    REQUIRE(rig.tick());
    CHECK(rig.driver->get_slewing());
}

// Decision 0022 rule 3: a :GU# read with no GOTO ends the slew at once, inside the 2 s grace. The mount shows the
// GOTO on one read and then reports stopped: the synchronous call returns on the next 250 ms poll, not at 2 s.
TEST_CASE("OnStep snapshot - a synchronous slew that lands inside the grace returns before the grace ends",
          "[onstep][telescope][snapshot][hold-window]") {
    Rig rig;
    rig.connect();
    rig.pause();
    rig.mount->set_status("NpEW180#");
    std::atomic<int> reads_after_goto{-1};  // -1: the :MS# has not gone out
    rig.mount->set_on_command([&](const std::string& chunk) {
        if (chunk.rfind(":MS", 0) == 0) {
            reads_after_goto = 0;
        } else if (chunk.rfind(":GU", 0) == 0 && reads_after_goto.load() >= 0) {
            rig.mount->set_status(reads_after_goto.fetch_add(1) == 0 ? "npEW180#" : "NpEW180#");
        }
    });
    const auto start = rig.clock.now();
    REQUIRE_NOTHROW(synchronous_slew(rig, 2.0, 20.0));
    const auto elapsed = rig.clock.now() - start;
    CHECK(elapsed < seconds(2));
    CHECK_FALSE(rig.driver->get_slewing());
}

// Decision 0022 rule 2: AbortSlew clears the window, so Slewing reads false at once, inside the 2 s.
TEST_CASE("OnStep snapshot - AbortSlew inside the hold clears Slewing at once",
          "[onstep][telescope][snapshot][hold-window]") {
    Rig rig;
    rig.connect();
    rig.pause();
    rig.mount->set_status("NpEW180#");
    rig.driver->slew_to_coordinates_async(2.0, 20.0);
    REQUIRE(rig.driver->get_slewing());
    rig.driver->abort_slew();
    CHECK_FALSE(rig.driver->get_slewing());
    rig.clock.advance(milliseconds(500));
    CHECK_FALSE(rig.driver->get_slewing());
}

// Decision 0022 rule 2: a reconnect starts without the old session's window.
TEST_CASE("OnStep snapshot - a reconnect inside the hold leaves Slewing false",
          "[onstep][telescope][snapshot][hold-window]") {
    Rig rig;
    rig.connect();
    rig.pause();
    rig.mount->set_status("NpEW180#");
    rig.driver->slew_to_coordinates_async(2.0, 20.0);
    REQUIRE(rig.driver->get_slewing());
    rig.driver->set_connected(false);
    rig.connect();
    CHECK_FALSE(rig.driver->get_slewing());
}

// Decision 0022 rule 4: the pulse's window is on the task clock; a frame sampled before the pulse's end keeps it.
TEST_CASE("OnStep snapshot - a pulse keeps its axis while virtual time has not reached its end",
          "[onstep][telescope][snapshot][hold-window]") {
    Rig rig;
    rig.connect();
    rig.pause();
    rig.driver->pulse_guide(0, 1000);  // North, 1 s
    rig.clock.advance(milliseconds(999));
    REQUIRE(rig.tick());
    CHECK(rig.driver->get_is_pulse_guiding());
    rig.clock.advance(milliseconds(51));  // the end plus kPulseGuideSettle
    REQUIRE(rig.tick());
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
}

// Each axis has its own window (OnStepX times each axis on its own): a shorter North pulse sent during a longer
// East one does not end the East pulse's IsPulseGuiding.
TEST_CASE("OnStep snapshot - a shorter pulse on one axis leaves the other axis's longer pulse guiding",
          "[onstep][telescope][snapshot][hold-window]") {
    Rig rig;
    rig.connect();
    rig.pause();
    rig.driver->pulse_guide(2, 3000);  // East, 3 s
    rig.driver->pulse_guide(0, 500);   // North, 0.5 s
    rig.clock.advance(milliseconds(600));
    REQUIRE(rig.tick());  // North has ended and is measured
    CHECK(rig.driver->get_is_pulse_guiding());
    rig.clock.advance(milliseconds(2400));
    REQUIRE(rig.tick());  // East's end, sampled before end + 50 ms
    CHECK(rig.driver->get_is_pulse_guiding());
    rig.clock.advance(milliseconds(50));
    REQUIRE(rig.tick());
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
}

// Decision 0022 rule 3: a reading taken before the :MS# answer does not end the slew, even once the window has run
// out. A tick samples "no GOTO" and, between its sample and its publish, a SlewToCoordinatesAsync goes out: the
// dispatch's write-through drops that tick's publish, and Slewing stays true past the 2 s window with no restore.
// A tick sampled after the answer that shows no GOTO is the landing.
TEST_CASE("OnStep snapshot - a reading taken before the :MS# answer does not end the slew",
          "[onstep][telescope][snapshot][hold-window]") {
    Rig rig;
    rig.connect();
    rig.pause();
    std::atomic<int> calls{0};
    onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, [&] {
        if (calls.fetch_add(1) != 0) return;
        rig.driver->slew_to_coordinates_async(2.0, 20.0);  // tracking on: a restore is owed at the end
    });
    REQUIRE(rig.tick());  // sampled "no GOTO" before the :MS#
    onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, nullptr);
    rig.mount->set_status("nNpEW180#");  // no GOTO, tracking off
    rig.clock.advance(seconds(3));       // past the window
    CHECK(rig.driver->get_slewing());
    CHECK(rig.mount->count(":Te#") == 0);
    REQUIRE(rig.tick());  // sampled after the answer: the landing, with no window to wait for
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK(rig.mount->count(":Te#") == 1);
}

// Decision 0022 rule 3 (the Sky-Watcher check, "a short goto reports landed before the 8 s hold ends"): landing is
// the mount's answer, never the 2 s window. The mount shows the GOTO (1 ms after :MS# answers on the rig), stops at
// 0.5 s, and the next poll tick reads Slewing false at 1 s.
TEST_CASE("OnStep snapshot - a short goto reports landed before the 2 s hold ends",
          "[onstep][telescope][snapshot][hold-window]") {
    Rig rig;
    rig.connect();
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));
    rig.driver->slew_to_coordinates_async(2.0, 20.0);
    rig.mount->set_status("npEW180#");  // the GOTO runs
    REQUIRE(rig.driver->get_slewing());
    rig.clock.advance(milliseconds(500));
    REQUIRE(rig.clock.wait_for_woken_settled(seconds(5)));
    CHECK(rig.driver->get_slewing());   // still running
    rig.mount->set_status("NpEW180#");  // the GOTO ends at about 0.5 s
    rig.clock.advance(milliseconds(500));
    REQUIRE(rig.clock.wait_for_woken_settled(seconds(5)));  // the 1 s tick
    CHECK_FALSE(rig.driver->get_slewing());                 // 1 s, inside the 2 s window
}

// A new pulse keeps the running pulse-end tick when the other axis's pulse ends later. With the poll running: East
// 3 s, then North 0.5 s. East's IsPulseGuiding stays true through the regular 1 s ticks and the 2 s bound, and ends
// on the tick 50 ms after 3 s; North adds no tick of its own.
TEST_CASE("OnStep snapshot - a shorter pulse on the other axis keeps the longer pulse's end tick",
          "[onstep][telescope][snapshot][hold-window]") {
    Rig rig;
    rig.connect();
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));
    const auto waits = rig.clock.wait_count();
    rig.driver->pulse_guide(2, 3000);  // East, 3 s: its end tick waits 3.05 s
    REQUIRE(rig.clock.wait_for_wait_count(waits + 1, seconds(2)));
    rig.driver->pulse_guide(0, 500);  // North, 0.5 s: must not replace it with a tick at 0.55 s

    for (int second = 1; second <= 3; ++second) {
        CAPTURE(second);
        const auto polls = rig.mount->count(":GU#");
        rig.clock.advance(seconds(1));
        REQUIRE(rig.clock.wait_for_woken_settled(seconds(5)));
        CHECK(rig.mount->count(":GU#") == polls + 1);  // the regular tick only
        CHECK(rig.driver->get_is_pulse_guiding());
    }
    const auto polls = rig.mount->count(":GU#");
    rig.clock.advance(milliseconds(50));
    REQUIRE(rig.clock.wait_for_woken_settled(seconds(5)));
    CHECK(rig.mount->count(":GU#") == polls + 1);  // East's pulse-end tick
    CHECK_FALSE(rig.driver->get_is_pulse_guiding());
}

// Publishes are ordered by sample: a tick that sampled after an older tick, and publishes after it, is published
// (before, the older tick's publish dropped every sample in flight, the newer one included).
TEST_CASE("OnStep snapshot - a newer sample is not dropped by an older tick's publish",
          "[onstep][telescope][snapshot][hold-window]") {
    Rig rig;
    rig.connect();
    rig.pause();
    rig.mount->ra_seconds = 10;  // what tick A samples
    std::atomic<int> calls{0};
    std::atomic<bool> b_sampled{false};
    std::atomic<bool> a_published{false};
    std::atomic<bool> b_result{false};
    std::thread b;
    const auto await = [](const std::atomic<bool>& flag) {
        for (int i = 0; i < 5000 && !flag.load(); ++i) std::this_thread::sleep_for(milliseconds(1));
    };
    onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, [&] {
        if (calls.fetch_add(1) == 0) {
            // Tick A, between its sample and its publish: tick B samples now and publishes after A.
            rig.mount->ra_seconds = 20;
            b = std::thread([&] { b_result = rig.tick(); });
            await(b_sampled);
            return;
        }
        b_sampled = true;  // tick B, between its sample and its publish
        await(a_published);
    });
    REQUIRE(rig.tick());  // A publishes 10
    a_published = true;
    if (b.joinable()) b.join();
    onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, nullptr);
    REQUIRE(b_sampled.load());
    REQUIRE(b_result.load());
    CHECK(std::abs(rig.driver->get_right_ascension() - (12.0 + 20.0 / 3600.0)) < 1e-6);
}

// A Disconnect that lands between a PulseGuide's unlock and its start() has already joined the pulse-end slot, so
// the body that start() then installs belongs to the old session. The next Connect joins it before the new
// session starts, so it neither waits on into the new session nor samples or publishes there.
TEST_CASE("OnStep snapshot - a reconnect joins a pulse-end body started after the disconnect's join",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.connect();
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));  // the status poll
    onstep::set_pulse_guide_gap_hook_for_testing(*rig.driver, [&] { rig.driver->set_connected(false); });
    rig.driver->pulse_guide(2, 500);  // unlocks, the Disconnect runs, then the pulse-end body starts
    onstep::set_pulse_guide_gap_hook_for_testing(*rig.driver, nullptr);
    REQUIRE_FALSE(rig.driver->get_connected());
    REQUIRE(onstep::pulse_measurement_running_for_testing(*rig.driver));  // waiting its 550 ms
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));
    CHECK(rig.clock.waiter_count() == 1);  // only that body: the poll was joined

    rig.driver->set_connected(true);
    REQUIRE(rig.driver->get_connected());
    CHECK_FALSE(onstep::pulse_measurement_running_for_testing(*rig.driver));  // joined by the connect
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));                       // the new session's poll
    CHECK(rig.clock.waiter_count() == 1);                                     // and nothing else
}

// #1893: with the site unknown, the members that use it refuse as the site getters do, instead of using latitude and
// longitude 0. An Alt/Az slew retries the site read first and sends nothing when it still fails.
TEST_CASE("OnStep snapshot - members that use the site refuse while it is unknown", "[onstep][telescope][snapshot]") {
    Rig rig(/*with_site=*/false);
    rig.mount->garble_site = true;       // the connect cannot read the mount's site
    rig.mount->set_status("nNpEo180#");  // Tracking off, so an Alt/Az slew is not refused for Tracking; no pier side
    rig.connect();
    rig.pause();
    const auto refuses = [](const std::function<void()>& call, const std::string& member) {
        try {
            call();
            FAIL(member + " answered with a site nobody set or read");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
            CHECK(std::string(ex.what()).find(member + " needs the site") != std::string::npos);
            CHECK(std::string(ex.what()).find("Invalid OnStep site info response") != std::string::npos);
        }
    };
    refuses([&] { (void)rig.driver->get_side_of_pier(); }, "SideOfPier");
    refuses([&] { (void)rig.driver->get_destination_side_of_pier(1.0, 20.0); }, "DestinationSideOfPier");
    refuses([&] { (void)rig.driver->get_sidereal_time(); }, "SiderealTime");
    CHECK_FALSE(has_entry(*rig.driver, "SideOfPier"));
    CHECK_FALSE(has_entry(*rig.driver, "SiderealTime"));

    // The mount's own pier-side letter needs no site (#2179); only the hour-angle fallback for `o` does.
    rig.mount->set_status("nNpEW180#");
    REQUIRE(rig.tick(false));
    CHECK(rig.driver->get_side_of_pier() == 1);
    rig.mount->set_status("nNpEo180#");
    REQUIRE(rig.tick(false));

    rig.clock.advance(seconds(3));  // past the site retry delay: the slew reads the site again first
    const auto site_reads = rig.mount->count(":Gt");
    refuses([&] { rig.driver->slew_to_alt_az_async(45.0, 180.0); }, "SlewToAltAzAsync");
    CHECK(rig.mount->count(":Gt") == site_reads + 1);
    refuses([&] { rig.driver->slew_to_alt_az(45.0, 180.0); }, "SlewToAltAz");
    CHECK(rig.mount->count(":Sr") == 0);
    CHECK(rig.mount->count(":Sd") == 0);
    CHECK(rig.mount->count(":MS") == 0);

    rig.mount->garble_site = false;  // the poll's full tick reads the site, and the members answer
    rig.clock.advance(seconds(3));
    REQUIRE(rig.tick(true));
    CHECK_NOTHROW((void)rig.driver->get_side_of_pier());
    CHECK_NOTHROW((void)rig.driver->get_sidereal_time());
}

// #1895: a poll that cannot start (no thread) fails the connect, which is undone as a Disconnect is, rather than
// serving the first frame for the whole session.
TEST_CASE("OnStep snapshot - a connect whose status poll cannot start fails and closes the port",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    REQUIRE(rig.server.ok());
    std::atomic<bool> refuse{true};
    onstep::set_status_poll_spawn_for_testing(*rig.driver, [&refuse](std::function<void()> body) {
        if (refuse.load()) {
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        }
        return std::thread(std::move(body));
    });
    std::string message;
    try {
        rig.driver->set_connected(true);
    } catch (const alpacacore::AlpacaException& ex) {
        message = ex.what();
    }
    CHECK(message.find("status poll did not start") != std::string::npos);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(onstep::detail::registered_link_for_testing(rig.info) == nullptr);  // the port was closed
    CHECK(error_code_of([&] { (void)rig.driver->get_right_ascension(); }) == alpacacore::AlpacaError::NotConnected);
    CHECK(rig.clock.waiter_count() == 0);

    refuse = false;  // the next Connect starts the poll
    rig.connect();
    CHECK(rig.clock.wait_for_waiters(1, seconds(2)));
}

// #2257: on a parked mount a bad MoveAxis axis or rate is InvalidValue, not InvalidWhileParked; a good one is still
// refused as parked.
TEST_CASE("OnStep snapshot - a parked mount answers InvalidValue to a bad MoveAxis argument",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.mount->set_status("nNPEW180#");  // parked
    rig.connect();
    REQUIRE(rig.driver->get_at_park());
    CHECK(error_code_of([&] { rig.driver->move_axis(5, 0.1); }) == alpacacore::AlpacaError::InvalidValue);
    CHECK(error_code_of([&] { rig.driver->move_axis(0, std::numeric_limits<double>::quiet_NaN()); }) ==
          alpacacore::AlpacaError::InvalidValue);
    CHECK(error_code_of([&] { rig.driver->move_axis(0, 1.0); }) == alpacacore::AlpacaError::InvalidWhileParked);
}

// #1895: a Tracking write on a connect whose :GU# read failed reads :GU# first, so the write-through lands in a
// status the getter serves, instead of in a frame with no status until the next good tick.
TEST_CASE("OnStep snapshot - a Tracking write after a connect that read no status is served at once",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.mount->silent_status = true;  // the connect's :GU# times out
    rig.connect();
    rig.pause();
    REQUIRE(error_code_of([&] { (void)rig.driver->get_tracking(); }) == alpacacore::AlpacaError::DriverException);
    rig.mount->silent_status = false;
    rig.driver->set_tracking(false);  // the mount still tracks ("NpEW180#") until the next tick reads it
    CHECK_FALSE(rig.driver->get_tracking());
}

// #1895: Unpark on a connect whose :GU# read failed reads :GU# first, so its write-through lands in a status the
// getter serves; while :GU# stays silent it refuses instead of unparking blind.
TEST_CASE("OnStep snapshot - an Unpark after a connect that read no status is served at once",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.mount->silent_status = true;  // the connect's :GU# times out
    rig.connect();
    rig.pause();
    rig.mount->set_status("nNPEW180#");  // parked, but nothing has read it
    CHECK(error_code_of([&] { rig.driver->unpark(); }) == alpacacore::AlpacaError::DriverException);
    CHECK(rig.mount->count(":hR") == 0);  // nothing was sent while the status was unknown

    rig.mount->silent_status = false;
    rig.driver->unpark();
    CHECK_FALSE(rig.driver->get_at_park());
}

// #1895: an unread status is not taken for "not parked": the commands that check for a park read :GU# first and
// refuse while it does not answer. The stops still go out.
TEST_CASE("OnStep snapshot - a status the connect could not read refuses the commands that check for a park",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.mount->silent_status = true;  // the connect's :GU# times out
    rig.connect();
    rig.pause();
    rig.mount->set_status("nNPEW180#");  // parked, but nothing has read it
    CHECK(error_code_of([&] { rig.driver->pulse_guide(0, 100); }) == alpacacore::AlpacaError::DriverException);
    CHECK(error_code_of([&] { rig.driver->move_axis(0, 1.0); }) == alpacacore::AlpacaError::DriverException);
    CHECK(rig.mount->count(":M") == 0);  // no :Mg, :RA or :Me went out
    CHECK(rig.mount->count(":RA") == 0);
    CHECK_NOTHROW(rig.driver->move_axis(0, 0.0));  // a stop is not held up by the unread status
    CHECK_NOTHROW(rig.driver->abort_slew());

    rig.mount->silent_status = false;  // once :GU# answers, the guard reads it
    CHECK(error_code_of([&] { rig.driver->find_home(); }) == alpacacore::AlpacaError::InvalidWhileParked);
    CHECK(error_code_of([&] { rig.driver->set_tracking(true); }) == alpacacore::AlpacaError::InvalidWhileParked);
}

// Turning tracking off is a stop: an unread status never holds it up, as for AbortSlew and MoveAxis(axis, 0).
TEST_CASE("OnStep snapshot - Tracking off after a connect that read no status still stops tracking",
          "[onstep][telescope][snapshot]") {
    Rig rig;
    rig.mount->silent_status = true;  // the connect's :GU# times out
    rig.connect();
    rig.pause();
    CHECK_NOTHROW(rig.driver->set_tracking(false));  // the :GU# read inside it fails too
    CHECK(rig.mount->count(":ST0") == 1);
}

// #1887 (decision 0022 rule 2): the slew's hold window is cleared when the :MS# send itself throws, so Slewing
// reads false right after, with no virtual time advanced (the 2 s hold would otherwise still be open). A silent
// :MS# does not reach that path: it reads as an empty reply, which the refusal branch clears.
TEST_CASE("OnStep snapshot - a GOTO whose :MS# send fails leaves Slewing false at once",
          "[onstep][telescope][snapshot][hold-window]") {
    Rig rig;
    rig.connect();
    rig.pause();
    rig.mount->set_status("NpEW180#");  // tracking, no GOTO
    rig.mount->overlong_goto = true;
    CHECK(error_code_of([&] { rig.driver->slew_to_coordinates_async(2.0, 20.0); }) ==
          alpacacore::AlpacaError::DriverException);
    REQUIRE(rig.mount->count(":MS") == 1);
    CHECK_FALSE(rig.driver->get_slewing());
}

// #1887 (decision 0022 rule 2), #2220: the pulse's hold window is cleared when the :Mg send throws. :Mg is sent
// blind, so a silent mount is not a failed send: the controller hangs up as :Mg arrives instead, after the call's
// connection check. That is a lost link (decision 0009 rule 2a), so the call throws NotConnected and IsPulseGuiding
// answers NotConnected, never a pulse still running. (Before #2220 a dropped TCP link failed the write with the link
// still alive and IsPulseGuiding read false; no fake fails a send without a loss now, so that read is reviewed.)
TEST_CASE("OnStep snapshot - a pulse guide whose :Mg send meets a hangup loses the link",
          "[onstep][telescope][snapshot][hold-window][network]") {
    Rig rig;
    rig.connect();
    rig.pause();
    rig.server.hang_up_on(":Mg");
    CHECK(error_code_of([&] { rig.driver->pulse_guide(0, 2000); }) == alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(error_code_of([&] { (void)rig.driver->get_is_pulse_guiding(); }) == alpacacore::AlpacaError::NotConnected);
}

// ── Pier side (#2179) ─────────────────────────────────────────────────────────────────────────────────────────────
// A scripted OnStepX 10.23a with the settings of the HM-17 rig: preferred side Best, flips on, meridian limits 60/60
// min, RA axis +/-180 degrees. The status letters: `E` mount type GEM, then `T` pier east, `W` pier west or `o` none.

namespace {

constexpr double kThirdOfAnHour = 20.0 / 60.0;
const char* const kWestStatus = "NpEW180#";     // tracking, no GOTO, pier west
const char* const kEastStatus = "NpET180#";     // tracking, no GOTO, pier east
const char* const kNoSideStatus = "NpEo180#";   // pier side none
const char* const kHomeStatus = "NHpEo180#";    // at home, no side
const char* const kParkedStatus = "NPHEo180#";  // parked at home
const char* const kNotTrackingStatus = "nNpEW180#";
const char* const kGotoStatus = "pEW180#";  // tracking, a GOTO runs

void set_pier(Rig& rig, const std::string& preferred, const std::string& flips = "2") {
    std::lock_guard<std::mutex> lock(rig.mount->mutex);
    rig.mount->gx96 = preferred;
    rig.mount->gx94 = flips;
}

// The mount points at hour angle `ha_hours` (Dec 30) with `status`, and the driver has sampled it.
void place(Rig& rig, double ha_hours, const char* status) {
    rig.mount->point_at(rig.driver->get_sidereal_time(), ha_hours, 30.0);
    rig.mount->set_status(status);
    REQUIRE(rig.tick());
}

// An RA that has hour angle `ha_hours` now.
double ra_for(Rig& rig, double ha_hours) {
    double ra = std::fmod(rig.driver->get_sidereal_time() - ha_hours, 24.0);
    return ra < 0.0 ? ra + 24.0 : ra;
}

// A connected, paused rig (north site, from the mount) for the pier cases.
void connect_paused(Rig& rig) {
    rig.connect();
    rig.pause();
}

std::size_t index_of(const std::vector<std::string>& commands, const std::string& prefix, std::size_t from = 0) {
    for (std::size_t i = from; i < commands.size(); ++i) {
        if (commands[i].rfind(prefix, 0) == 0) return i;
    }
    return commands.size();
}

}  // namespace

// The mount's letter, not the hour angle: OnStepX Best stays on the west side past the meridian (HM-17, step B of
// #2179), and the east side before it (step E).
// Falsified by: get_side_of_pier() computing from the hour-angle sign (West at HA +20 min would read East).
TEST_CASE("OnStep pier side - SideOfPier follows the :GU# letter on both sides of HA 0",
          "[onstep][telescope][pierside]") {
    Rig rig(false);
    connect_paused(rig);
    place(rig, kThirdOfAnHour, kWestStatus);
    CHECK(rig.driver->get_side_of_pier() == 1);
    place(rig, -kThirdOfAnHour, kEastStatus);
    CHECK(rig.driver->get_side_of_pier() == 0);
    place(rig, -kThirdOfAnHour, kWestStatus);
    CHECK(rig.driver->get_side_of_pier() == 1);
    place(rig, kThirdOfAnHour, kEastStatus);
    CHECK(rig.driver->get_side_of_pier() == 0);
}

// Falsified by: the letter branch also answering for `o`, or the fallback losing the hour-angle rule.
TEST_CASE("OnStep pier side - SideOfPier falls back to the hour-angle rule when the mount reports no side",
          "[onstep][telescope][pierside]") {
    Rig rig(false);
    connect_paused(rig);
    place(rig, 1.0, kNoSideStatus);
    CHECK(rig.driver->get_side_of_pier() == 0);
    place(rig, -1.0, kNoSideStatus);
    CHECK(rig.driver->get_side_of_pier() == 1);
    place(rig, -1.0, "NpE180#");  // firmware that sends no side letter at all
    CHECK(rig.driver->get_side_of_pier() == 1);
}

// Untested firmware and mount types keep the old answer: the letter is served only on an OnStepX German mount.
// Falsified by: dropping the `onstepx_` or `is_gem` condition from get_side_of_pier()'s letter branch.
TEST_CASE("OnStep pier side - SideOfPier keeps the hour-angle rule on OnStep 4.x and on a fork mount",
          "[onstep][telescope][pierside]") {
    SECTION("OnStep 4.x German mount") {
        Rig rig(false);
        rig.mount->version = "4.24s";
        connect_paused(rig);
        place(rig, kThirdOfAnHour, kWestStatus);
        CHECK(rig.driver->get_side_of_pier() == 0);
        place(rig, -kThirdOfAnHour, kEastStatus);
        CHECK(rig.driver->get_side_of_pier() == 1);
    }
    SECTION("OnStepX fork mount") {
        Rig rig(false);
        connect_paused(rig);
        place(rig, kThirdOfAnHour, "NpKW180#");
        CHECK(rig.driver->get_side_of_pier() == 0);
        place(rig, -kThirdOfAnHour, "NpKT180#");
        CHECK(rig.driver->get_side_of_pier() == 1);
    }
}

// AlignmentMode answers from the mount-type letter of the polled :GU# frame, with no device I/O (#2214). Alt-alt has no
// ASCOM value and is answered AltAz, as clients treat it the same way. A frame with no type letter is not a
// supported firmware's answer (OnStepX writes E/K/A/L, 4.x E/K/A) and throws DriverException (decision 0010 rule 8).
// Falsified by: get_alignment_mode() returning a constant, ignoring the type letter, mapping 'L' to anything but
// AltAz, or answering GermanPolar for a frame with no type letter.
TEST_CASE("OnStep AlignmentMode - answers from the :GU# mount type", "[onstep][telescope][unit]") {
    Rig rig(false);
    connect_paused(rig);
    SECTION("fork K is Polar") {
        place(rig, 1.0, "NpKW180#");
        CHECK(rig.driver->get_alignment_mode() == alpacacore::AlignmentMode::Polar);
    }
    SECTION("alt-az A is AltAz") {
        place(rig, 1.0, "NpAo180#");
        CHECK(rig.driver->get_alignment_mode() == alpacacore::AlignmentMode::AltAz);
    }
    SECTION("alt-alt L is AltAz") {
        place(rig, 1.0, "NpLo180#");
        CHECK(rig.driver->get_alignment_mode() == alpacacore::AlignmentMode::AltAz);
    }
    SECTION("GEM E is GermanPolar") {
        place(rig, 1.0, "NpEW180#");
        CHECK(rig.driver->get_alignment_mode() == alpacacore::AlignmentMode::GermanPolar);
    }
    SECTION("a frame with no type letter throws DriverException") {
        place(rig, 1.0, "Np180#");
        CHECK(error_code_of([&] { (void)rig.driver->get_alignment_mode(); }) ==
              alpacacore::AlpacaError::DriverException);
    }
}

// OnStep 4.x sends the type letter too (Command.ino writes E, K or A), so a fork frame reads Polar there.
// Falsified by: get_alignment_mode() gated on OnStepX (is_onstepx_version()).
TEST_CASE("OnStep AlignmentMode - OnStep 4.x reports the mount type", "[onstep][telescope][unit]") {
    Rig rig(false);
    rig.mount->version = "4.24s";
    connect_paused(rig);
    place(rig, 1.0, "NpKW180#");
    CHECK(rig.driver->get_alignment_mode() == alpacacore::AlignmentMode::Polar);
}

// A fork frame must not leak a non-GEM answer once the driver is disconnected.
// Falsified by: a driver that keeps serving the polled frame after set_connected(false).
TEST_CASE("OnStep AlignmentMode - a disconnected driver reports GermanPolar", "[onstep][telescope][unit]") {
    Rig rig(false);
    connect_paused(rig);
    place(rig, 1.0, "NpKW180#");
    REQUIRE(rig.driver->get_alignment_mode() == alpacacore::AlignmentMode::Polar);
    rig.driver->set_connected(false);
    CHECK(rig.driver->get_alignment_mode() == alpacacore::AlignmentMode::GermanPolar);
}

// Rig steps B, D and E of #2179, and the home start of step A, through the driver, with the settings the mount
// answers.
// Falsified by: get_destination_side_of_pier() answering from the hour-angle sign (B, E and A would be wrong), or
// ignoring the preferred side (D would read West).
TEST_CASE("OnStep pier side - DestinationSideOfPier models the firmware's choice", "[onstep][telescope][pierside]") {
    Rig rig(false);
    SECTION("B: Best on the west side stays West at HA +20 min") {
        set_pier(rig, "B");
        connect_paused(rig);
        place(rig, -0.5, kWestStatus);
        CHECK(rig.driver->get_destination_side_of_pier(ra_for(rig, kThirdOfAnHour), 30.0) == 1);
    }
    SECTION("D: East preferred flips a west mount to East at HA +20 min") {
        set_pier(rig, "E");
        connect_paused(rig);
        place(rig, -0.5, kWestStatus);
        CHECK(rig.driver->get_destination_side_of_pier(ra_for(rig, kThirdOfAnHour), 30.0) == 0);
    }
    SECTION("E: Best on the east side stays East at HA -20 min") {
        set_pier(rig, "B");
        connect_paused(rig);
        place(rig, 0.5, kEastStatus);
        CHECK(rig.driver->get_destination_side_of_pier(ra_for(rig, -kThirdOfAnHour), 30.0) == 0);
    }
    SECTION("A: Best at home goes West at HA -20 min") {
        set_pier(rig, "B");
        connect_paused(rig);
        place(rig, 6.0, kHomeStatus);
        CHECK(rig.driver->get_destination_side_of_pier(ra_for(rig, -kThirdOfAnHour), 30.0) == 1);
    }
    SECTION("Best on West past the west limit takes East") {
        set_pier(rig, "B");
        connect_paused(rig);
        place(rig, 0.5, kWestStatus);
        CHECK(rig.driver->get_destination_side_of_pier(ra_for(rig, 2.0), 30.0) == 0);
    }
    SECTION("a site in the southern hemisphere uses the southern west axis") {
        Rig southern(true);  // the configured site is at latitude -33.9
        set_pier(southern, "B");
        connect_paused(southern);
        southern.mount->point_at(southern.driver->get_sidereal_time(), 0.0, -30.0);
        southern.mount->set_status(kHomeStatus);
        REQUIRE(southern.tick());
        CHECK(southern.driver->get_destination_side_of_pier(ra_for(southern, -kThirdOfAnHour), -30.0) == 1);
    }
}

// The hour-angle rule is the fallback: settings the mount does not answer, a fork mount, a target no side reaches.
// Falsified by: the model answering with default settings, or a -1 from the model being returned.
TEST_CASE("OnStep pier side - DestinationSideOfPier falls back to the hour-angle rule",
          "[onstep][telescope][pierside]") {
    Rig rig(false);
    SECTION(":GX96# not answered") {
        connect_paused(rig);
        place(rig, -0.5, kWestStatus);
        // Best would stay West; the hour-angle rule says East for HA +20 min.
        CHECK(rig.driver->get_destination_side_of_pier(ra_for(rig, kThirdOfAnHour), 30.0) == 0);
        CHECK(rig.driver->get_destination_side_of_pier(ra_for(rig, -kThirdOfAnHour), 30.0) == 1);
        CHECK(rig.mount->count(":GX94") == 0);  // the read stopped at :GX96#
    }
    SECTION("a limit that does not parse") {
        set_pier(rig, "B");
        {
            std::lock_guard<std::mutex> lock(rig.mount->mutex);
            rig.mount->gxe9 = "sixty";
        }
        connect_paused(rig);
        place(rig, -0.5, kWestStatus);
        CHECK(rig.driver->get_destination_side_of_pier(ra_for(rig, kThirdOfAnHour), 30.0) == 0);
        CHECK_FALSE(rig.driver->get_can_set_pier_side());
    }
    SECTION("a fork mount") {
        set_pier(rig, "B");
        connect_paused(rig);
        place(rig, -0.5, "NpKo180#");
        CHECK(rig.driver->get_destination_side_of_pier(ra_for(rig, kThirdOfAnHour), 30.0) == 0);
    }
    SECTION("no side reaches the target") {
        set_pier(rig, "B");
        {
            std::lock_guard<std::mutex> lock(rig.mount->mutex);
            rig.mount->gxee_min = "-90";
            rig.mount->gxee_max = "90";
        }
        connect_paused(rig);
        place(rig, 0.0, kEastStatus);
        // HA +8 h is beyond +/-90 degrees on the east side and beyond the west side's range: the firmware would
        // refuse the GOTO, and the hour-angle rule answers East.
        CHECK(rig.driver->get_destination_side_of_pier(ra_for(rig, 8.0), 30.0) == 0);
    }
}

// The poll reads the settings again every tenth tick, so a preference changed at a handset is followed.
// Falsified by: no refresh in the poll (the cache would stay on B), or a refresh on every tick (the count check).
TEST_CASE("OnStep pier side - the poll refreshes the settings every tenth tick", "[onstep][telescope][pierside]") {
    Rig rig(false);
    set_pier(rig, "B");
    connect_paused(rig);
    place(rig, -0.5, kWestStatus);
    const double target = ra_for(rig, kThirdOfAnHour);
    CHECK(rig.driver->get_destination_side_of_pier(target, 30.0) == 1);  // Best stays West

    set_pier(rig, "E");  // changed at the handset
    const auto reads_before = rig.mount->count(":GX96");
    for (int tick = 1; tick < 10; ++tick) {
        REQUIRE(onstep::poll_status_tick_for_testing(*rig.driver, tick));
    }
    CHECK(rig.mount->count(":GX96") == reads_before);
    CHECK(rig.driver->get_destination_side_of_pier(target, 30.0) == 1);  // not read yet
    REQUIRE(onstep::poll_status_tick_for_testing(*rig.driver, 10));
    CHECK(rig.mount->count(":GX96") == reads_before + 1);
    CHECK(rig.driver->get_destination_side_of_pier(target, 30.0) == 0);  // East preferred now

    // A failed refresh keeps the previous settings.
    set_pier(rig, "");
    REQUIRE(onstep::poll_status_tick_for_testing(*rig.driver, 20));
    CHECK(rig.driver->get_destination_side_of_pier(target, 30.0) == 0);
    CHECK(rig.driver->get_can_set_pier_side());
}

// Falsified by: any one of the conditions being dropped from can_set_pier_side_locked().
TEST_CASE("OnStep pier side - CanSetPierSide needs OnStepX, a GEM, flips on and a read preferred side",
          "[onstep][telescope][pierside]") {
    Rig rig(false);
    SECTION("the full set") {
        set_pier(rig, "B");
        CHECK_FALSE(rig.driver->get_can_set_pier_side());  // disconnected
        connect_paused(rig);
        CHECK(rig.driver->get_can_set_pier_side());
        rig.driver->set_connected(false);
        CHECK_FALSE(rig.driver->get_can_set_pier_side());
    }
    SECTION("the other letters") {
        for (const char* letter : {"E", "W", "A"}) {
            Rig other(false);
            set_pier(other, letter);
            connect_paused(other);
            INFO(letter);
            CHECK(other.driver->get_can_set_pier_side());
        }
    }
    SECTION("flips off") {
        set_pier(rig, "E", "0 N");
        connect_paused(rig);
        CHECK_FALSE(rig.driver->get_can_set_pier_side());
    }
    SECTION("a fork mount") {
        set_pier(rig, "B");
        rig.mount->set_status("NpKo180#");
        connect_paused(rig);
        CHECK_FALSE(rig.driver->get_can_set_pier_side());
    }
    SECTION("OnStep 4.x") {
        set_pier(rig, "B");
        {
            std::lock_guard<std::mutex> lock(rig.mount->mutex);
            rig.mount->version = "4.24s";
        }
        connect_paused(rig);
        CHECK_FALSE(rig.driver->get_can_set_pier_side());
        CHECK(rig.mount->count(":GX96") == 0);  // never asked
    }
    SECTION(":GX96# not answered") {
        connect_paused(rig);
        CHECK_FALSE(rig.driver->get_can_set_pier_side());
    }
}

// The flip is the mount's own one-command GOTO to the current position on the other side.
// Falsified by: the :SX96 preference juggle or a :Sr/:Sd/:MS# re-slew coming back, Slewing not set, or the targets
// changing.
TEST_CASE("OnStep pier side - SideOfPier to the east sends :MNe# and nothing else", "[onstep][telescope][pierside]") {
    Rig rig(false);
    set_pier(rig, "B");
    connect_paused(rig);
    place(rig, kThirdOfAnHour, kWestStatus);
    rig.driver->set_target_right_ascension(5.0);
    rig.driver->set_target_declination(10.0);
    rig.mount->clear_log();

    REQUIRE_NOTHROW(rig.driver->set_side_of_pier(0));

    CHECK(rig.mount->count(":MNe#") == 1);
    CHECK(rig.mount->count(":MNw#") == 0);
    CHECK(rig.mount->count(":SX96") == 0);
    CHECK(rig.mount->count(":Sr") == 0);
    CHECK(rig.mount->count(":Sd") == 0);
    CHECK(rig.mount->count(":MS#") == 0);
    CHECK(rig.driver->get_slewing());
    CHECK(rig.driver->get_target_right_ascension() == Approx(5.0));
    CHECK(rig.driver->get_target_declination() == Approx(10.0));
    CHECK(rig.driver->get_can_set_pier_side());
}

// Falsified by: the west flip sending :MNe#, or going through :SX96/:MS#.
TEST_CASE("OnStep pier side - SideOfPier to the west sends :MNw#", "[onstep][telescope][pierside]") {
    Rig rig(false);
    set_pier(rig, "B");
    connect_paused(rig);
    place(rig, -kThirdOfAnHour, kEastStatus);
    rig.mount->clear_log();
    REQUIRE_NOTHROW(rig.driver->set_side_of_pier(1));
    CHECK(rig.mount->count(":MNw#") == 1);
    CHECK(rig.mount->count(":MNe#") == 0);
    CHECK(rig.mount->count(":SX96") == 0);
    CHECK(rig.mount->count(":MS#") == 0);
}

// Falsified by: a refused flip reporting success, leaving Slewing true, or sending a follow-up command.
TEST_CASE("OnStep pier side - a refused flip raises and Slewing stays false", "[onstep][telescope][pierside]") {
    Rig rig(false);
    set_pier(rig, "B");
    connect_paused(rig);
    place(rig, kThirdOfAnHour, kWestStatus);
    {
        std::lock_guard<std::mutex> lock(rig.mount->mutex);
        rig.mount->mn_reply = "6";  // outside limits
    }
    rig.mount->clear_log();
    CHECK(error_code_of([&] { rig.driver->set_side_of_pier(0); }) == alpacacore::AlpacaError::InvalidOperation);
    const auto commands = rig.mount->commands();
    const auto flip = index_of(commands, ":MNe#");
    REQUIRE(flip < commands.size());
    CHECK(rig.mount->count(":MNe#") == 1);
    CHECK(rig.mount->count(":SX96") == 0);
    CHECK(rig.mount->count(":MS#") == 0);
    for (std::size_t i = flip + 1; i < commands.size(); ++i) {
        CHECK(commands[i].rfind(":S", 0) != 0);  // no setting is written after the refusal
    }
    CHECK_FALSE(rig.driver->get_slewing());
}

// Falsified by: an empty flip reply being taken as accepted, or the slew hold staying armed.
TEST_CASE("OnStep pier side - a flip that gets no reply raises", "[onstep][telescope][pierside]") {
    Rig rig(false);
    set_pier(rig, "B");
    connect_paused(rig);
    place(rig, kThirdOfAnHour, kWestStatus);
    {
        std::lock_guard<std::mutex> lock(rig.mount->mutex);
        rig.mount->mn_reply = "";
    }
    rig.mount->clear_log();
    CHECK(error_code_of([&] { rig.driver->set_side_of_pier(0); }) != 0);
    CHECK(rig.mount->count(":MNe#") == 1);
    CHECK_FALSE(rig.driver->get_slewing());
}

// At home OnStepX overrides the requested side with the target's hour angle, so the flip is not sent.
// Falsified by: the no-side check being dropped (a :MNe# would go out at home).
TEST_CASE("OnStep pier side - SideOfPier is refused while the mount reports no pier side",
          "[onstep][telescope][pierside]") {
    Rig rig(false);
    set_pier(rig, "B");
    connect_paused(rig);
    place(rig, kThirdOfAnHour, "NpEo180#");  // tracking, no GOTO, not parked, no pier side
    rig.mount->clear_log();
    CHECK(error_code_of([&] { rig.driver->set_side_of_pier(0); }) == alpacacore::AlpacaError::InvalidOperation);
    CHECK(rig.mount->count(":MN") == 0);
    CHECK(rig.mount->count(":SX96") == 0);
    CHECK(rig.mount->count(":MS#") == 0);
}

// Falsified by: the already-there check being dropped (a flip would be sent for nothing).
TEST_CASE("OnStep pier side - SideOfPier to the side the mount is on sends nothing", "[onstep][telescope][pierside]") {
    Rig rig(false);
    set_pier(rig, "B");
    connect_paused(rig);
    place(rig, kThirdOfAnHour, kWestStatus);
    rig.mount->clear_log();
    REQUIRE_NOTHROW(rig.driver->set_side_of_pier(1));
    CHECK(rig.mount->count(":MN") == 0);
    CHECK(rig.mount->count(":MS#") == 0);
    CHECK(rig.mount->count(":Sr") == 0);
    CHECK_FALSE(rig.driver->get_slewing());
}

// Falsified by: any refusal moving after the first command is sent, or the value check following the connection check.
TEST_CASE("OnStep pier side - SideOfPier refusals send nothing", "[onstep][telescope][pierside]") {
    Rig rig(false);
    set_pier(rig, "B");
    const auto nothing_sent = [&] {
        CHECK(rig.mount->count(":MN") == 0);
        CHECK(rig.mount->count(":SX96") == 0);
        CHECK(rig.mount->count(":Sr") == 0);
        CHECK(rig.mount->count(":MS#") == 0);
    };

    SECTION("a value other than 0 or 1 is InvalidValue, also while disconnected") {
        CHECK(error_code_of([&] { rig.driver->set_side_of_pier(2); }) == alpacacore::AlpacaError::InvalidValue);
        CHECK(error_code_of([&] { rig.driver->set_side_of_pier(-1); }) == alpacacore::AlpacaError::InvalidValue);
        CHECK(error_code_of([&] { rig.driver->set_side_of_pier(0); }) == alpacacore::AlpacaError::NotConnected);
        connect_paused(rig);
        CHECK(error_code_of([&] { rig.driver->set_side_of_pier(2); }) == alpacacore::AlpacaError::InvalidValue);
        nothing_sent();
    }
    SECTION("parked") {
        connect_paused(rig);
        place(rig, 0.3, kParkedStatus);
        CHECK(error_code_of([&] { rig.driver->set_side_of_pier(0); }) == alpacacore::AlpacaError::InvalidWhileParked);
        nothing_sent();
    }
    SECTION("Tracking off") {
        connect_paused(rig);
        place(rig, 0.3, kNotTrackingStatus);
        CHECK(error_code_of([&] { rig.driver->set_side_of_pier(0); }) == alpacacore::AlpacaError::InvalidOperation);
        nothing_sent();
    }
    SECTION("a GOTO running") {
        connect_paused(rig);
        place(rig, 0.3, kGotoStatus);
        CHECK(error_code_of([&] { rig.driver->set_side_of_pier(0); }) == alpacacore::AlpacaError::InvalidOperation);
        nothing_sent();
    }
    SECTION("a side that cannot reach the position") {
        connect_paused(rig);
        place(rig, -2.0, kWestStatus);  // 2 h east of the meridian: beyond the 1 h east limit
        try {
            rig.driver->set_side_of_pier(0);
            FAIL("SideOfPier to the east was accepted past the east limit");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
            CHECK(std::string(ex.what()).find("east side cannot reach") != std::string::npos);
        }
        nothing_sent();
    }
    SECTION("CanSetPierSide false: PropertyNotImplemented") {
        set_pier(rig, "");  // :GX96# not answered
        connect_paused(rig);
        place(rig, 0.3, kWestStatus);
        CHECK(error_code_of([&] { rig.driver->set_side_of_pier(0); }) ==
              alpacacore::AlpacaError::PropertyNotImplemented);
        nothing_sent();
    }
}

// ── #2214 slice 2: tracking rates, rate offsets, the pulse-guide rate and SyncToAltAz ───────────────────────────────
// Through the driver's ASCOM members against the loopback OnStepX; what reached the wire is read from its log.
// Firmware facts are from OnStepX af1d26bf (Status.command.cpp, Mount.command.cpp, Guide.command.cpp).

namespace {

const char* const kLunarStatus = "Np(EW180#";
const char* const kSolarStatus = "NpOEW180#";
const char* const kKingStatus = "NpkEW180#";
const char* const kCompensatedStatus = "NprEW180#";  // refraction compensation: the frame names no rate
const char* const kCompensatedIdleStatus = "nNprEW180#";
const char* const kHomeResetStatus = "NHpEo180#";  // at home

// No relative slack: the guide rates differ from the informational one by parts in a million.
Approx exact(double value) { return Approx(value).epsilon(0.0).margin(1e-12); }

// The mount shows `status` and the driver has sampled it.
void show(Rig& rig, const char* status, bool full = false) {
    rig.mount->set_status(status);
    REQUIRE(rig.tick(full));
}

// A connected, paused rig whose mount answers `version`.
void connect_rates(Rig& rig, const char* version = "10.23a") {
    {
        std::lock_guard<std::mutex> lock(rig.mount->mutex);
        rig.mount->version = version;
    }
    connect_paused(rig);
}

}  // namespace

// Falsified by: parse_status() ignoring the '(' 'O' 'k' letters (every frame would read driveSidereal).
TEST_CASE("OnStepX rates - TrackingRate follows the :GU# rate letter", "[onstep][telescope][rates]") {
    Rig rig;
    connect_rates(rig);
    CHECK(rig.driver->get_tracking_rates() == std::vector<int>{0, 1, 2, 3});
    CHECK(rig.driver->get_tracking_rate() == 0);  // no letter: sidereal
    show(rig, kLunarStatus);
    CHECK(rig.driver->get_tracking_rate() == 1);
    show(rig, kSolarStatus);
    CHECK(rig.driver->get_tracking_rate() == 2);
    show(rig, kKingStatus);
    CHECK(rig.driver->get_tracking_rate() == 3);
    show(rig, kWestStatus);
    CHECK(rig.driver->get_tracking_rate() == 0);
    show(rig, "nNp(EW180#");  // the letters do not depend on Tracking being on
    CHECK(rig.driver->get_tracking_rate() == 1);
}

// With tracking compensation on the frame carries 'r' or 't' and no rate letter, so the driver's last known rate
// answers; the slow tick asks :GT# only while compensation is on and the mount tracks, and keeps the rate when the
// reply is 0 or none of the four.
// Falsified by: tracking_rate_served_locked() answering the frame's (absent) letter while compensation is on (it
// would read sidereal), or the :GT# mapping using a tolerance wider than 0.01 Hz (60.164 would match King).
TEST_CASE("OnStepX rates - a compensation frame keeps the last known rate and :GT# corrects it",
          "[onstep][telescope][rates]") {
    Rig rig;
    connect_rates(rig);
    show(rig, kLunarStatus);
    show(rig, kCompensatedStatus);
    CHECK(rig.driver->get_tracking_rate() == 1);  // the letter is hidden: still lunar
    rig.mount->clear_log();

    show(rig, kCompensatedStatus, /*full=*/false);
    CHECK(rig.mount->count(":GT#") == 0);  // only the slow tick asks
    {
        std::lock_guard<std::mutex> lock(rig.mount->mutex);
        rig.mount->gt = "60.00000";
    }
    show(rig, kCompensatedStatus, /*full=*/true);
    CHECK(rig.mount->count(":GT#") == 1);
    CHECK(rig.driver->get_tracking_rate() == 2);  // solar, from the mount

    for (const char* hz : {"61.50000", "0", "60.15000"}) {  // none of the four, not tracking, 14 mHz off sidereal
        INFO(hz);
        {
            std::lock_guard<std::mutex> lock(rig.mount->mutex);
            rig.mount->gt = hz;
        }
        show(rig, kCompensatedStatus, /*full=*/true);
        CHECK(rig.driver->get_tracking_rate() == 2);
    }
    rig.mount->clear_log();
    show(rig, kCompensatedIdleStatus, /*full=*/true);  // not tracking: no :GT# read
    CHECK(rig.mount->count(":GT#") == 0);
    {
        std::lock_guard<std::mutex> lock(rig.mount->mutex);
        rig.mount->gt = "60.16427";
    }
    show(rig, kCompensatedStatus, /*full=*/true);
    CHECK(rig.driver->get_tracking_rate() == 0);  // sidereal
    {
        std::lock_guard<std::mutex> lock(rig.mount->mutex);
        rig.mount->gt = "60.13600";
    }
    show(rig, kCompensatedStatus, /*full=*/true);
    CHECK(rig.driver->get_tracking_rate() == 3);  // King
}

// The connect reads :GT# too: a mount already on a hidden rate is not taken for sidereal.
// Falsified by: dropping the :GT# read from seed_rate_state_locked() (the compensated mount would read sidereal).
TEST_CASE("OnStepX rates - the connect reads the rate the compensation frame hides", "[onstep][telescope][rates]") {
    Rig rig;
    rig.mount->set_status(kCompensatedStatus);
    {
        std::lock_guard<std::mutex> lock(rig.mount->mutex);
        rig.mount->gt = "57.90000";
    }
    connect_rates(rig);
    CHECK(rig.driver->get_tracking_rate() == 1);
}

// Falsified by: set_tracking_rate() skipping the command (nothing on the wire), skipping the write-through (the
// read right after would still say sidereal), or sending the wrong letter.
TEST_CASE("OnStepX rates - setting the TrackingRate sends the command and reads back at once",
          "[onstep][telescope][rates]") {
    Rig rig;
    connect_rates(rig);
    const std::vector<std::pair<int, const char*>> rates = {{1, ":TL#"}, {2, ":TS#"}, {3, ":TK#"}, {0, ":TQ#"}};
    for (const auto& [rate, command] : rates) {
        INFO(command);
        rig.mount->clear_log();
        rig.driver->set_tracking_rate(rate);
        CHECK(rig.mount->count(command) == 1);
        CHECK(rig.mount->count(":ST") == 0);             // the rate commands only
        CHECK(rig.driver->get_tracking_rate() == rate);  // before any poll saw it
    }
    // The poll that follows says the same, and with compensation on the written rate stands.
    rig.driver->set_tracking_rate(3);
    show(rig, kKingStatus);
    CHECK(rig.driver->get_tracking_rate() == 3);
    show(rig, kCompensatedStatus);
    CHECK(rig.driver->get_tracking_rate() == 3);
    // 0-3 only, before the connection check (ITelescopeV4 DriveRates).
    CHECK(error_code_of([&] { rig.driver->set_tracking_rate(4); }) == alpacacore::AlpacaError::InvalidValue);
    CHECK(error_code_of([&] { rig.driver->set_tracking_rate(-1); }) == alpacacore::AlpacaError::InvalidValue);
    rig.driver->set_connected(false);
    CHECK(error_code_of([&] { rig.driver->set_tracking_rate(4); }) == alpacacore::AlpacaError::InvalidValue);
    CHECK(error_code_of([&] { rig.driver->set_tracking_rate(1); }) == alpacacore::AlpacaError::NotConnected);
}

// "When tracking is turned on, the mount must use the last selected TrackingRate": :ST60.164275# would set the rate
// back to sidereal on OnStepX (Mount.command.cpp 173-185), :Te# starts tracking at the selected one.
// Falsified by: set_tracking_checked() sending :ST60.164275# on OnStepX (the wire would carry it and no :Te#).
TEST_CASE("OnStepX rates - turning Tracking on sends :Te# and keeps the selected rate", "[onstep][telescope][rates]") {
    Rig rig;
    connect_rates(rig);
    rig.driver->set_tracking_rate(1);
    show(rig, "nNp(EW180#");  // lunar, not tracking
    CHECK_FALSE(rig.driver->get_tracking());
    rig.mount->clear_log();
    rig.driver->set_tracking(true);
    CHECK(rig.mount->count(":Te#") == 1);
    CHECK(rig.mount->count(":ST") == 0);
    CHECK(rig.driver->get_tracking());
    CHECK(rig.driver->get_tracking_rate() == 1);
    show(rig, kLunarStatus);  // the mount tracks, still lunar
    CHECK(rig.driver->get_tracking());
    CHECK(rig.driver->get_tracking_rate() == 1);
    rig.mount->clear_log();
    rig.driver->set_tracking(false);
    CHECK(rig.mount->count(":ST0#") == 1);  // stopping is :ST0# everywhere
}

// Falsified by: set_tracking_checked() sending :Te# on every firmware (OnStep 4.x answers an unknown command 0, so
// Tracking could never be turned on there).
TEST_CASE("OnStep 4.x rates - Tracking on still sends :ST60.164275# and the rate stays sidereal",
          "[onstep][telescope][rates]") {
    Rig rig;
    connect_rates(rig, "4.24s");
    CHECK(rig.driver->get_tracking_rates() == std::vector<int>{0});
    show(rig, "nNpEW180#");
    rig.mount->clear_log();
    rig.driver->set_tracking(true);
    CHECK(rig.mount->count(":ST60.164275#") == 1);
    CHECK(rig.mount->count(":Te#") == 0);
    CHECK(rig.driver->get_tracking_rate() == 0);
    show(rig, kLunarStatus);  // 4.x writes no rate letter; one that appears means nothing there
    CHECK(rig.driver->get_tracking_rate() == 0);
    // Sidereal is accepted and sends nothing; the others are not offered.
    rig.mount->clear_log();
    CHECK_NOTHROW(rig.driver->set_tracking_rate(0));
    CHECK(error_code_of([&] { rig.driver->set_tracking_rate(1); }) == alpacacore::AlpacaError::InvalidValue);
    CHECK(error_code_of([&] { rig.driver->set_tracking_rate(3); }) == alpacacore::AlpacaError::InvalidValue);
    CHECK(rig.mount->count(":T") == 0);
    // The offsets are an OnStepX feature: not offered, never asked.
    CHECK_FALSE(rig.driver->get_can_set_right_ascension_rate());
    CHECK_FALSE(rig.driver->get_can_set_declination_rate());
    CHECK(error_code_of([&] { rig.driver->set_right_ascension_rate(0.1); }) ==
          alpacacore::AlpacaError::PropertyNotImplemented);
    CHECK(error_code_of([&] { rig.driver->set_declination_rate(0.1); }) ==
          alpacacore::AlpacaError::PropertyNotImplemented);
    CHECK(rig.driver->get_right_ascension_rate() == 0.0);
    CHECK(rig.driver->get_declination_rate() == 0.0);
    CHECK(rig.mount->count(":GXT") == 0);
    CHECK(rig.mount->count(":GT#") == 0);
    CHECK(rig.mount->count(":SXT") == 0);
}

// ASCOM RightAscensionRate is seconds of RA per sidereal second (x 15 for the firmware's arcseconds), DeclinationRate
// arcseconds per SI second (x 0.9972695663 for arcseconds per sidereal second); a read divides back.
// Falsified by: the RA conversion multiplying by 1 or the Dec one dropping the 0.9972695663 (the wire values change),
// or the getters not dividing back.
TEST_CASE("OnStepX rates - the rate offsets convert units on the wire and on the read", "[onstep][telescope][rates]") {
    Rig rig;
    {
        std::lock_guard<std::mutex> lock(rig.mount->mutex);
        rig.mount->gxtr = "30.00000000";  // 2 seconds of RA per sidereal second
        rig.mount->gxtd = "-0.99726957";  // -1 arcsecond per SI second
    }
    connect_rates(rig);
    CHECK(rig.driver->get_can_set_right_ascension_rate());
    CHECK(rig.driver->get_can_set_declination_rate());
    CHECK(rig.driver->get_right_ascension_rate() == Approx(2.0).margin(1e-6));
    CHECK(rig.driver->get_declination_rate() == Approx(-1.0).margin(1e-6));

    rig.mount->clear_log();
    rig.driver->set_right_ascension_rate(0.5);
    rig.driver->set_declination_rate(2.0);
    const auto commands = rig.mount->commands();
    CHECK(index_of(commands, ":SXTR,7.500000#") < commands.size());
    CHECK(index_of(commands, ":SXTD,1.994539#") < commands.size());
    CHECK(rig.driver->get_right_ascension_rate() == Approx(0.5).margin(1e-6));  // read back from the mount
    CHECK(rig.driver->get_declination_rate() == Approx(2.0).margin(1e-6));
    CHECK(rig.mount->count(":GXTR#") >= 1);
    CHECK(rig.mount->count(":GXTD#") >= 1);
    rig.driver->set_right_ascension_rate(-0.25);
    CHECK(rig.driver->get_right_ascension_rate() == Approx(-0.25).margin(1e-6));
    rig.mount->clear_log();
    CHECK(rig.driver->get_right_ascension_rate() == Approx(-0.25).margin(1e-6));
    CHECK(rig.mount->count(":") == 0);  // a getter asks the mount nothing
}

// The firmware clamps beyond +/-1800 arcseconds per sidereal second without saying so, so the driver refuses the
// request itself, before the connection check.
// Falsified by: dropping the range check (121 s/s would send :SXTR,1815.000000# and the mount would clamp it) or
// moving it after check_connected() (the disconnected calls would read NotConnected).
TEST_CASE("OnStepX rates - a rate beyond the firmware's limit is InvalidValue, before the connection check",
          "[onstep][telescope][rates]") {
    Rig rig;
    const auto invalid = [&](auto&& call) { return error_code_of(call) == alpacacore::AlpacaError::InvalidValue; };
    SECTION("disconnected") {
        CHECK(invalid([&] { rig.driver->set_right_ascension_rate(121.0); }));
        CHECK(invalid([&] { rig.driver->set_right_ascension_rate(-121.0); }));
        CHECK(invalid([&] { rig.driver->set_declination_rate(1805.0); }));  // x 0.9972695663 = 1800.07
        CHECK(invalid([&] { rig.driver->set_declination_rate(-1805.0); }));
        CHECK(invalid([&] { rig.driver->set_right_ascension_rate(std::nan("")); }));
        CHECK(invalid([&] { rig.driver->set_declination_rate(std::numeric_limits<double>::infinity()); }));
        CHECK(error_code_of([&] { rig.driver->set_right_ascension_rate(120.0); }) ==
              alpacacore::AlpacaError::NotConnected);
        CHECK(error_code_of([&] { rig.driver->set_declination_rate(1804.0); }) ==
              alpacacore::AlpacaError::NotConnected);
    }
    SECTION("connected") {
        connect_rates(rig);
        rig.mount->clear_log();
        CHECK(invalid([&] { rig.driver->set_right_ascension_rate(121.0); }));
        CHECK(invalid([&] { rig.driver->set_declination_rate(-1805.0); }));
        CHECK(rig.mount->count(":SXT") == 0);
        rig.driver->set_right_ascension_rate(120.0);  // exactly the limit
        rig.driver->set_declination_rate(-1804.0);    // x 0.9972695663 = -1799.07
        CHECK(rig.mount->count(":SXTR,1800.000000#") == 1);
        CHECK(rig.mount->count(":SXTD,-1799.07") == 1);
    }
}

// ITelescopeV4: reading either rate returns zero unless TrackingRate is driveSidereal, writing it is
// InvalidOperation then.
// Falsified by: dropping the sidereal condition from either getter (the stored offset would be served on lunar) or
// from set_rate_offset() (the write would go out under lunar).
TEST_CASE("OnStepX rates - the offsets read zero and refuse writes while TrackingRate is not sidereal",
          "[onstep][telescope][rates]") {
    Rig rig;
    {
        std::lock_guard<std::mutex> lock(rig.mount->mutex);
        rig.mount->gxtr = "15.00000000";
        rig.mount->gxtd = "0.99726957";
    }
    connect_rates(rig);
    CHECK(rig.driver->get_right_ascension_rate() == Approx(1.0).margin(1e-6));
    show(rig, kLunarStatus);
    CHECK(rig.driver->get_right_ascension_rate() == 0.0);
    CHECK(rig.driver->get_declination_rate() == 0.0);
    rig.mount->clear_log();
    CHECK(error_code_of([&] { rig.driver->set_right_ascension_rate(0.5); }) ==
          alpacacore::AlpacaError::InvalidOperation);
    CHECK(error_code_of([&] { rig.driver->set_declination_rate(0.5); }) == alpacacore::AlpacaError::InvalidOperation);
    CHECK(rig.mount->count(":SXT") == 0);
    show(rig, kWestStatus);  // sidereal again: the offsets were never reset
    CHECK(rig.driver->get_right_ascension_rate() == Approx(1.0).margin(1e-6));
    CHECK(rig.driver->get_declination_rate() == Approx(1.0).margin(1e-6));
    // A TrackingRate write to the same rate does not touch them.
    rig.driver->set_tracking_rate(0);
    CHECK(rig.driver->get_right_ascension_rate() == Approx(1.0).margin(1e-6));
    CHECK(rig.mount->count(":SXT") == 0);
}

// Decision 0010 rule 8: with no :GU# answer since the connect the rate is a value only the mount supplies, so the
// rate getters throw as Tracking does instead of answering sidereal / 0. OnStep 4.x has no rate to read and stays 0.
// Falsified by: tracking_rate_served_locked() falling back to tracking_rate_known_ when no status is held (the
// getters would answer 0).
TEST_CASE("OnStepX rates - the rate getters throw while the connect's :GU# never answered",
          "[onstep][telescope][rates]") {
    Rig rig;
    rig.mount->silent_status = true;
    SECTION("OnStepX") {
        connect_rates(rig);
        REQUIRE(error_code_of([&] { (void)rig.driver->get_tracking(); }) == alpacacore::AlpacaError::DriverException);
        CHECK(error_code_of([&] { (void)rig.driver->get_tracking_rate(); }) ==
              alpacacore::AlpacaError::DriverException);
        CHECK(error_code_of([&] { (void)rig.driver->get_right_ascension_rate(); }) ==
              alpacacore::AlpacaError::DriverException);
        CHECK(error_code_of([&] { (void)rig.driver->get_declination_rate(); }) ==
              alpacacore::AlpacaError::DriverException);
        CHECK(error_code_of([&] { rig.driver->set_right_ascension_rate(0.5); }) ==
              alpacacore::AlpacaError::DriverException);
        rig.mount->silent_status = false;
        show(rig, kWestStatus);
        CHECK(rig.driver->get_tracking_rate() == 0);
    }
    SECTION("OnStep 4.x has no rate to read and stays sidereal") {
        connect_rates(rig, "4.24s");
        CHECK(rig.driver->get_tracking_rate() == 0);
        CHECK(rig.driver->get_right_ascension_rate() == 0.0);
    }
}

// The OnTrack model's 't' hides the rate letter just as refraction's 'r' does.
// Falsified by: parse_status() treating only 'r' as compensation (a 't' frame would read sidereal).
TEST_CASE("OnStepX rates - a 't' compensation frame keeps the last known rate", "[onstep][telescope][rates]") {
    Rig rig;
    connect_rates(rig);
    show(rig, kLunarStatus);
    show(rig, "NptEW180#");
    CHECK(rig.driver->get_tracking_rate() == 1);
}

// Falsified by: set_tracking_rate() dropping its read_rate_offsets_locked() (the getter would keep the old offset).
TEST_CASE("OnStepX rates - the offsets are read again after a TrackingRate write", "[onstep][telescope][rates]") {
    Rig rig;
    {
        std::lock_guard<std::mutex> lock(rig.mount->mutex);
        rig.mount->gxtr = "15.00000000";
    }
    connect_rates(rig);
    CHECK(rig.driver->get_right_ascension_rate() == Approx(1.0).margin(1e-6));
    {
        std::lock_guard<std::mutex> lock(rig.mount->mutex);
        rig.mount->gxtr = "30.00000000";
    }
    rig.mount->clear_log();
    rig.driver->set_tracking_rate(0);
    CHECK(rig.mount->count(":GXTR#") == 1);
    CHECK(rig.driver->get_right_ascension_rate() == Approx(2.0).margin(1e-6));
}

// OnStepX keeps applying the offsets on a lunar/solar/King rate (Mount.cpp 476-515, af1d26bf) while the getters read
// 0 and writes are refused, so a rate change zeroes both first (as the Sky-Watcher driver does).
// Falsified by: set_tracking_rate() dropping the :SXTR,0#/:SXTD,0# (the offsets would stay on the mount), or sending
// them with zero offsets.
TEST_CASE("OnStepX rates - changing the TrackingRate zeroes non-zero offsets first", "[onstep][telescope][rates]") {
    Rig rig;
    connect_rates(rig);
    rig.mount->clear_log();
    rig.driver->set_tracking_rate(1);  // zero offsets: nothing to clear
    CHECK(rig.mount->count(":SXT") == 0);
    rig.driver->set_tracking_rate(0);
    rig.driver->set_right_ascension_rate(0.5);
    rig.driver->set_declination_rate(0.5);
    rig.mount->clear_log();
    rig.driver->set_tracking_rate(0);  // same rate: kept
    CHECK(rig.mount->count(":SXT") == 0);
    rig.driver->set_tracking_rate(1);
    const auto commands = rig.mount->commands();
    const auto tr = index_of(commands, ":SXTR,0.000000#");
    const auto td = index_of(commands, ":SXTD,0.000000#");
    const auto tl = index_of(commands, ":TL#");
    REQUIRE(tr < commands.size());
    REQUIRE(td < commands.size());
    REQUIRE(tl < commands.size());
    CHECK(tr < tl);
    CHECK(td < tl);
    rig.driver->set_tracking_rate(0);
    CHECK(rig.driver->get_right_ascension_rate() == 0.0);
    CHECK(rig.driver->get_declination_rate() == 0.0);
}

// A 0 from :SXTR/:SXTD is a refusal, read rather than drained (#1379), with the :GE# reason.
// Falsified by: set_rate_offset() ignoring the reply (the write would report success).
TEST_CASE("OnStepX rates - a refused rate offset throws with the :GE# reason", "[onstep][telescope][rates]") {
    Rig rig;
    connect_rates(rig);
    {
        std::lock_guard<std::mutex> lock(rig.mount->mutex);
        rig.mount->sxt_reply = "0";
        rig.mount->ge_code = "15#";
    }
    for (const bool ra : {true, false}) {
        try {
            if (ra) {
                rig.driver->set_right_ascension_rate(0.5);
            } else {
                rig.driver->set_declination_rate(0.5);
            }
            FAIL("a refused rate offset was accepted");
        } catch (const alpacacore::AlpacaException& ex) {
            CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidOperation);
            CHECK(std::string(ex.what()).find("refused by the mount (target below the horizon limit)") !=
                  std::string::npos);
        }
    }
    CHECK(rig.driver->get_right_ascension_rate() == 0.0);  // nothing was taken
}

// OnStepX zeroes both offsets on every home reset (a find home, the boot, the unpark path): the poll reads them again
// when the frame's rate, tracking, parked or at-home flags differ from the previous frame's, and only then.
// Falsified by: dropping the RateWatch comparison from refresh_rate_state_locked() (no re-read after the unpark), or
// comparing nothing (a read on every tick).
TEST_CASE("OnStepX rates - the offsets are read again when the parked or at-home flags change",
          "[onstep][telescope][rates]") {
    Rig rig;
    {
        std::lock_guard<std::mutex> lock(rig.mount->mutex);
        rig.mount->gxtr = "15.00000000";
    }
    rig.mount->set_status("NPHEo180#");  // parked at home before the connect
    connect_rates(rig);
    CHECK(rig.driver->get_right_ascension_rate() == Approx(1.0).margin(1e-6));
    rig.mount->clear_log();
    show(rig, "NPHEo180#");
    show(rig, "NPHEo180#");
    CHECK(rig.mount->count(":GXTR#") == 0);  // the same frame: nothing to read again
    {
        std::lock_guard<std::mutex> lock(rig.mount->mutex);
        rig.mount->gxtr = "0.00000000";  // unpark: the home reset zeroed them
        rig.mount->gxtd = "0.00000000";
    }
    show(rig, kHomeResetStatus);  // unparked, still at home
    CHECK(rig.mount->count(":GXTR#") == 1);
    CHECK(rig.mount->count(":GXTD#") == 1);
    CHECK(rig.driver->get_right_ascension_rate() == 0.0);
    {
        std::lock_guard<std::mutex> lock(rig.mount->mutex);
        rig.mount->gxtr = "15.00000000";
    }
    show(rig, kWestStatus);  // leaves home
    CHECK(rig.mount->count(":GXTR#") == 2);
    CHECK(rig.driver->get_right_ascension_rate() == Approx(1.0).margin(1e-6));
    show(rig, kNotTrackingStatus);  // tracking off
    CHECK(rig.mount->count(":GXTR#") == 3);
    show(rig, kLunarStatus);  // the rate
    CHECK(rig.mount->count(":GXTR#") == 4);
    show(rig, kLunarStatus);
    CHECK(rig.mount->count(":GXTR#") == 4);
}

// A failed offset read keeps the previous values and does not end the tick (the pier settings' rule).
// Falsified by: read_rate_offsets_locked() zeroing the offsets on an unparsable reply.
TEST_CASE("OnStepX rates - an offset read that does not parse keeps the previous values",
          "[onstep][telescope][rates]") {
    Rig rig;
    {
        std::lock_guard<std::mutex> lock(rig.mount->mutex);
        rig.mount->gxtr = "15.00000000";
    }
    connect_rates(rig);
    {
        std::lock_guard<std::mutex> lock(rig.mount->mutex);
        rig.mount->gxtr.clear();  // an unknown command's "0"
    }
    show(rig, kHomeResetStatus);
    CHECK(rig.driver->get_right_ascension_rate() == Approx(1.0).margin(1e-6));
    CHECK(rig.driver->get_link_fault().empty());
}

// The pulse-guide rate is :GX90# (a multiple of sidereal) times 15.0410686 arcseconds per second, in degrees per
// second, on both axes; read at connect and again when the :GU# pulse-guide digit changes.
// Falsified by: reading :GX90# only at connect (the digit change would not be followed), reading it on every tick
// (the unchanged-digit count), or not converting to degrees per second.
TEST_CASE("OnStep guide rate - GuideRate follows :GX90# and re-reads when the :GU# pulse-guide digit changes",
          "[onstep][telescope][rates]") {
    Rig rig;
    {
        std::lock_guard<std::mutex> lock(rig.mount->mutex);
        rig.mount->gx90 = "1.00";
    }
    connect_rates(rig);
    const double sidereal = 15.0410686 / 3600.0;
    auto rate = rig.driver->get_guide_rate();
    CHECK(rate.ra == exact(sidereal));
    CHECK(rate.dec == exact(sidereal));
    CHECK_FALSE(rig.driver->get_can_set_guide_rates());
    CHECK(error_code_of([&] { rig.driver->set_guide_rate({sidereal, sidereal}); }) ==
          alpacacore::AlpacaError::PropertyNotImplemented);

    {
        std::lock_guard<std::mutex> lock(rig.mount->mutex);
        rig.mount->gx90 = "2.00";
    }
    rig.mount->clear_log();
    show(rig, "NpEW180#");  // the digit has not changed
    show(rig, "NpEW180#");
    CHECK(rig.mount->count(":GX90#") == 0);
    CHECK(rig.driver->get_guide_rate().ra == exact(sidereal));
    show(rig, "NpEW280#");  // pulse-guide digit 2
    CHECK(rig.mount->count(":GX90#") == 1);
    rate = rig.driver->get_guide_rate();
    CHECK(rate.ra == exact(2.0 * sidereal));
    CHECK(rate.dec == exact(2.0 * sidereal));
    rig.mount->clear_log();
    CHECK(rig.driver->get_guide_rate().ra == exact(2.0 * sidereal));
    CHECK(rig.mount->count(":") == 0);  // a getter asks the mount nothing
}

// Falsified by: the 4.x gate on the :GX90# read (the command is on OnStep 4.x too, Command.ino:922), or by a reply
// of "0" (an unknown command) being taken as a rate.
TEST_CASE("OnStep guide rate - OnStep 4.x answers :GX90#, and a firmware that does not keeps the informational rate",
          "[onstep][telescope][rates]") {
    const double informational = 0.5 * 15.0411 / 3600.0;
    SECTION("OnStep 4.x") {
        Rig rig;
        {
            std::lock_guard<std::mutex> lock(rig.mount->mutex);
            rig.mount->gx90 = "0.25";
        }
        connect_rates(rig, "4.24s");
        CHECK(rig.driver->get_guide_rate().ra == exact(0.25 * 15.0410686 / 3600.0));
    }
    SECTION("no :GX90# answer") {
        Rig rig;
        connect_rates(rig);
        CHECK(rig.driver->get_guide_rate().ra == exact(informational));
        CHECK(rig.driver->get_guide_rate().dec == exact(informational));
        show(rig, "NpEW280#");  // the digit changes, the read finds nothing again
        CHECK(rig.driver->get_guide_rate().ra == exact(informational));
    }
    SECTION("a reply that is not a rate") {
        Rig rig;
        {
            std::lock_guard<std::mutex> lock(rig.mount->mutex);
            rig.mount->gx90 = "fast";
        }
        connect_rates(rig);
        CHECK(rig.driver->get_guide_rate().ra == exact(informational));
    }
}

// GuideRate is the mount's :GX90# value, so a latched fault throws it (decision 0009 rule 1).
// Falsified by: get_guide_rate() dropping throw_if_faulted_locked() (it would answer 0.25 x sidereal through the
// fault).
TEST_CASE("OnStep guide rate - GuideRate throws the latched fault", "[onstep][telescope][rates]") {
    Rig rig;
    {
        std::lock_guard<std::mutex> lock(rig.mount->mutex);
        rig.mount->gx90 = "0.25";
    }
    connect_rates(rig);
    CHECK(rig.driver->get_guide_rate().ra == exact(0.25 * 15.0410686 / 3600.0));
    rig.mount->mute = true;
    CHECK(rig.tick());
    CHECK(rig.tick());
    CHECK(rig.tick());  // the third latches the fault
    REQUIRE(rig.driver->get_connected());
    CHECK(error_code_of([&] { (void)rig.driver->get_guide_rate(); }) == alpacacore::AlpacaError::DriverException);
    rig.mount->mute = false;
    CHECK(rig.tick());  // a good tick clears it
    CHECK(rig.driver->get_guide_rate().ra == exact(0.25 * 15.0410686 / 3600.0));
}

// Decision 0010 rule 8: a connected AlignmentMode with no :GU# answer since the connect is a value only the mount
// supplies, so it throws instead of answering GermanPolar.
// Falsified by: alignment_mode_locked() returning GermanPolar when no status is held (the mount_type_seen_ check
// dropped).
TEST_CASE("OnStep AlignmentMode - throws DriverException while the connect's :GU# never answered",
          "[onstep][telescope][unit]") {
    Rig rig;
    rig.mount->silent_status = true;
    connect_rates(rig);
    CHECK(error_code_of([&] { (void)rig.driver->get_alignment_mode(); }) == alpacacore::AlpacaError::DriverException);
    rig.mount->silent_status = false;
    show(rig, kWestStatus);
    CHECK(rig.driver->get_alignment_mode() == alpacacore::AlignmentMode::GermanPolar);
}

// A latched fault throws from AlignmentMode as from SideOfPier (decision 0009 rule 1).
// Falsified by: alignment_mode_locked() dropping the fault check (it would answer the last letter through the fault).
TEST_CASE("OnStep AlignmentMode - a latched fault throws", "[onstep][telescope][unit]") {
    Rig rig;
    connect_rates(rig);
    show(rig, kWestStatus);
    CHECK(rig.driver->get_alignment_mode() == alpacacore::AlignmentMode::GermanPolar);
    rig.mount->mute = true;
    CHECK(rig.tick());
    CHECK(rig.tick());
    CHECK(rig.tick());  // the third latches the fault
    REQUIRE(rig.driver->get_connected());
    CHECK(error_code_of([&] { (void)rig.driver->get_alignment_mode(); }) == alpacacore::AlpacaError::DriverException);
    rig.mount->mute = false;
    CHECK(rig.tick());  // a good tick clears it
    CHECK(rig.driver->get_alignment_mode() == alpacacore::AlignmentMode::GermanPolar);
}

// SyncToAltAz converts as SlewToAltAz does and syncs as SyncToCoordinates does. The ITelescopeV4 "may throw if
// Tracking is True" is optional, and plate-solve syncs run while the mount tracks, so it is not refused.
// Falsified by: copying check_alt_az_slew_locked()'s Tracking refusal into SyncToAltAz (the tracking case would
// throw), or leaving CanSyncAltAz false.
TEST_CASE("OnStep SyncToAltAz - syncs while tracking through :Sr, :Sd and :CM#", "[onstep][telescope][rates]") {
    Rig rig;
    connect_paused(rig);
    CHECK(rig.driver->get_can_sync_alt_az());
    REQUIRE(rig.driver->get_tracking());
    rig.mount->clear_log();
    CHECK_NOTHROW(rig.driver->sync_to_alt_az(45.0, 135.0));
    const auto commands = rig.mount->commands();
    const auto sr = index_of(commands, ":Sr");
    const auto sd = index_of(commands, ":Sd");
    const auto cm = index_of(commands, ":CM#");
    REQUIRE(cm < commands.size());
    CHECK(sr < sd);
    CHECK(sd < cm);
    CHECK(rig.mount->count(":MS#") == 0);  // a sync, not a slew
    // The target and the served position are the converted RA/Dec, as after SyncToCoordinates.
    CHECK(rig.driver->get_right_ascension() == Approx(rig.driver->get_target_right_ascension()).margin(1e-9));
    CHECK(rig.driver->get_declination() == Approx(rig.driver->get_target_declination()).margin(1e-9));
    // The slew's opposite rule stays with the slew.
    CHECK(error_code_of([&] { rig.driver->slew_to_alt_az_async(45.0, 135.0); }) ==
          alpacacore::AlpacaError::InvalidOperation);
}

// The order SlewToAltAz uses: the arguments (even disconnected), connected, parked, a latched fault, then the
// GOTO check shared with SyncToCoordinates.
// Falsified by: moving validate_alt_az() after check_connected() (the disconnected InvalidValue), or dropping the
// GOTO check from sync_to_mount_locked().
TEST_CASE("OnStep SyncToAltAz - validation order matches SlewToAltAz", "[onstep][telescope][rates]") {
    Rig rig;
    CHECK(error_code_of([&] { rig.driver->sync_to_alt_az(95.0, 10.0); }) == alpacacore::AlpacaError::InvalidValue);
    CHECK(error_code_of([&] { rig.driver->sync_to_alt_az(45.0, 361.0); }) == alpacacore::AlpacaError::InvalidValue);
    CHECK(error_code_of([&] { rig.driver->sync_to_alt_az(45.0, 90.0); }) == alpacacore::AlpacaError::NotConnected);
    connect_paused(rig);
    rig.mount->clear_log();

    SECTION("parked") {
        show(rig, kParkedStatus);
        CHECK(error_code_of([&] { rig.driver->sync_to_alt_az(95.0, 10.0); }) == alpacacore::AlpacaError::InvalidValue);
        CHECK(error_code_of([&] { rig.driver->sync_to_alt_az(45.0, 90.0); }) ==
              alpacacore::AlpacaError::InvalidWhileParked);
        CHECK(error_code_of([&] { rig.driver->slew_to_alt_az_async(45.0, 90.0); }) ==
              alpacacore::AlpacaError::InvalidWhileParked);
        CHECK(rig.mount->count(":CM#") == 0);
    }
    SECTION("a latched fault, after the parked check") {
        rig.mount->mute = true;
        for (int i = 0; i < 3; ++i) CHECK(rig.tick());
        rig.mount->mute = false;
        REQUIRE_FALSE(rig.driver->get_link_fault().empty());
        rig.mount->clear_log();
        CHECK(error_code_of([&] { rig.driver->sync_to_alt_az(95.0, 10.0); }) == alpacacore::AlpacaError::InvalidValue);
        CHECK(error_code_of([&] { rig.driver->sync_to_alt_az(45.0, 90.0); }) ==
              alpacacore::AlpacaError::DriverException);
        CHECK(rig.mount->count(":CM#") == 0);
    }
    SECTION("a GOTO running") {
        show(rig, kGotoStatus);
        CHECK(error_code_of([&] { rig.driver->sync_to_alt_az(45.0, 90.0); }) ==
              alpacacore::AlpacaError::InvalidOperation);
        CHECK(rig.mount->count(":CM#") == 0);
        CHECK(rig.mount->count(":Sr") == 0);
    }
}

// ── #2241: RightAscension and Declination while a rate offset is set ────────────────────────────────────────────────
// Against the loopback OnStepX whose :GRa#/:GDe# move with the virtual clock at the stored offsets in 0.5 arcsec
// steps. The poll is paused and ticked by hand once a second, so a read lands at a known point between polls. Every
// case reads the position through the public getters, as a client would.

namespace {

// A position read the same as `value` (degrees or hours): no relative slack, which Catch's default epsilon would add
// (about 1.9 arcsec at 45 degrees), only the parse's rounding.
Approx same_position(double value) { return Approx(value).epsilon(0.0).margin(1e-9); }

// A connected, paused OnStepX whose position follows its rate offsets.
void connect_moving(Rig& rig, const char* version = "10.23a") {
    rig.mount->offsets_move = true;
    connect_rates(rig, version);
}

// Moves the virtual time `span` forward from `elapsed` (since the last tick) with a poll tick at every whole second
// since it, as the 1 s poll would run. Returns the time since the last tick.
milliseconds run_polls(Rig& rig, milliseconds elapsed, milliseconds span) {
    while (elapsed + span >= seconds(1)) {
        const auto step = seconds(1) - elapsed;
        rig.clock.advance(step);
        REQUIRE(rig.tick(false));
        span -= step;
        elapsed = milliseconds(0);
    }
    rig.clock.advance(span);
    return elapsed + span;
}

double served(Rig& rig, bool ra) { return ra ? rig.driver->get_right_ascension() : rig.driver->get_declination(); }

// From one read of the axis to another, in axis arcseconds (RA: hours x 54000, the short way round).
double axis_arcsec(bool ra, double from, double to) {
    if (!ra) return (to - from) * 3600.0;
    double hours = std::fmod(to - from, 24.0);
    if (hours > 12.0) hours -= 24.0;
    if (hours < -12.0) hours += 24.0;
    return hours * kRaAxisArcsecPerHour;
}

// ConformU's method (#2241): a poll tick, `phase` later the rate write, a read, 10 s, a read. The mount polls every
// second from the first tick. Returns the motion the two reads show, in axis arcseconds.
double measured_motion(Rig& rig, bool ra, double rate, milliseconds phase) {
    REQUIRE(rig.tick(false));
    rig.clock.advance(phase);
    if (ra) {
        rig.driver->set_right_ascension_rate(rate);
    } else {
        rig.driver->set_declination_rate(rate);
    }
    const double before = served(rig, ra);
    (void)run_polls(rig, phase, seconds(10));
    return axis_arcsec(ra, before, served(rig, ra));
}

// The motion a rate gives over `si_seconds`, in axis arcseconds: DeclinationRate is arcseconds per SI second;
// RightAscensionRate is seconds of RA per sidereal second, 15 axis arcseconds each.
double expected_motion(bool ra, double rate, double si_seconds) {
    return ra ? rate * 15.0 * si_seconds / kSiSecondsPerSiderealSecond : rate * si_seconds;
}

// The ASCOM rate for `arcsec` on each axis: DeclinationRate `arcsec` per SI second, as ConformU sets it, and
// RightAscensionRate `arcsec` axis arcseconds per sidereal second (`arcsec` / 15 seconds of RA).
double ascom_rate(bool ra, double arcsec) { return ra ? arcsec / 15.0 : arcsec; }

}  // namespace

// Red on the code before #2241: the getters served the last frame, up to 1 s old, so the second read lagged the mount
// by the time since the last tick (10, 20 and 30 arcsec at 250, 500 and 750 ms). The tolerance is the model's guard,
// 2 arcsec, 0.5 percent of the 400 arcsec the window holds.
// Falsified by: served_sky_position_locked() returning the frame position instead of offset_model_position_locked().
TEST_CASE("OnStep rate offsets - a 40 arcsec/s offset shows in two reads 10 s apart from any point between polls",
          "[onstep][telescope][rates][offset-model]") {
    for (const bool ra : {false, true}) {
        for (const int phase_ms : {0, 250, 500, 750}) {
            CAPTURE(ra, phase_ms);
            Rig rig;
            connect_moving(rig);
            const double rate = ascom_rate(ra, 40.0);
            const double motion = measured_motion(rig, ra, rate, milliseconds(phase_ms));
            CHECK(motion == Approx(expected_motion(ra, rate, 10.0)).margin(2.0));
        }
    }
}

// Red on the code before #2241: the mount makes one 0.5 arcsec step about every 10 s at 0.05 arcsec/s, so the frame
// read 10 s after the rate write showed no step unless a tick landed after it (0 at 250, 500 and 750 ms). The model
// shows the commanded rate times the window, not a count of steps; the tolerance is 2 percent of the 0.5 arcsec.
// Falsified by: offset_model_position() leaving out the elapsed time (the model would hold the anchor).
TEST_CASE("OnStep rate offsets - a 0.05 arcsec/s offset shows in two reads 10 s apart from any point between polls",
          "[onstep][telescope][rates][offset-model]") {
    for (const bool ra : {false, true}) {
        for (const int phase_ms : {0, 250, 500, 750}) {
            CAPTURE(ra, phase_ms);
            Rig rig;
            connect_moving(rig);
            const double rate = ascom_rate(ra, 0.05);
            const double expected = expected_motion(ra, rate, 10.0);
            CHECK(measured_motion(rig, ra, rate, milliseconds(phase_ms)) == Approx(expected).margin(0.02 * expected));
        }
    }
}

// The model starts from the measured position at the rate write and runs at the commanded rate with SI seconds
// converted to sidereal ones: 40 arcsec per sidereal second is 40.11 per SI second on the RA axis.
// Falsified by: offset_model_position() dividing by kSiSecondsPerSiderealSecond nowhere (the RA motion would be 400).
TEST_CASE("OnStep rate offsets - the model converts SI seconds to sidereal seconds",
          "[onstep][telescope][rates][offset-model]") {
    Rig rig;
    connect_moving(rig);
    REQUIRE(rig.tick(false));
    rig.driver->set_right_ascension_rate(40.0 / 15.0);  // 40 axis arcseconds per sidereal second at the mount
    const double before = rig.driver->get_right_ascension();
    rig.clock.advance(milliseconds(900));  // inside one poll period: the model alone
    CHECK(axis_arcsec(true, before, rig.driver->get_right_ascension()) ==
          Approx(40.0 * 0.9 / kSiSecondsPerSiderealSecond).margin(1e-3));
}

// The guard: a poll that measures an axis more than 2 arcsec from the model re-anchors on the measured position, so
// a stalled axis shows within one poll at 40 arcsec/s, and within about 40 s at 0.05 arcsec/s.
// Falsified by: adopt_offset_model_frame_locked() keeping the anchor whatever the frame measured.
TEST_CASE("OnStep rate offsets - a stalled axis re-anchors the model on the measured position",
          "[onstep][telescope][rates][offset-model]") {
    Rig rig;
    connect_moving(rig);
    REQUIRE(rig.tick(false));

    SECTION("at 40 arcsec/s, within one poll") {
        rig.driver->set_declination_rate(40.0);
        (void)run_polls(rig, milliseconds(0), seconds(2));
        rig.mount->stall_dec = true;
        rig.clock.advance(seconds(1));
        const double model = rig.driver->get_declination();  // the model ran on for the second
        REQUIRE(rig.tick(false));
        const double measured = rig.mount->dec_now();
        CHECK(axis_arcsec(false, measured, model) == Approx(40.0).margin(1.0));
        CHECK(rig.driver->get_declination() == same_position(measured));  // re-anchored on the poll
    }
    SECTION("at 0.05 arcsec/s, once the model is 2 arcsec ahead") {
        rig.mount->stall_dec = true;
        rig.clock.advance(milliseconds(500));
        rig.driver->set_declination_rate(0.05);  // half way between polls: they land 0.5, 1.5, ... s after it
        (void)run_polls(rig, milliseconds(500), seconds(39));  // the last tick 38.5 s after the write
        rig.clock.advance(milliseconds(500));
        REQUIRE(rig.tick(false));  // 39.5 s: 1.975 arcsec apart, the anchor holds
        CHECK(axis_arcsec(false, rig.mount->dec_now(), rig.driver->get_declination()) == Approx(1.975).margin(1e-3));
        rig.clock.advance(seconds(1));
        REQUIRE(rig.tick(false));  // 40.5 s: 2.025 arcsec, past the guard
        CHECK(rig.driver->get_declination() == same_position(rig.mount->dec_now()));
    }
}

// The hold cap: 30 minutes after its anchor the model re-anchors on the measured position even inside the guard.
// Falsified by: adopt_offset_model_frame_locked() skipping the kOffsetModelHold check.
TEST_CASE("OnStep rate offsets - the model re-anchors on the measured position after 30 minutes",
          "[onstep][telescope][rates][offset-model]") {
    Rig rig;
    connect_moving(rig);
    REQUIRE(rig.tick(false));
    rig.driver->set_declination_rate(0.05);
    // The mount's Dec moves 1 arcsec away from the model's (inside the 2 arcsec guard): only the cap re-anchors.
    rig.mount->set_position(12.0, 45.0 + 1.0 / 3600.0);
    for (int minute = 1; minute < 30; ++minute) {
        rig.clock.advance(seconds(60));
        REQUIRE(rig.tick(false));
    }
    rig.clock.advance(seconds(59));
    REQUIRE(rig.tick(false));  // 29 min 59 s: the anchor holds
    const double apart = axis_arcsec(false, rig.driver->get_declination(), rig.mount->dec_now());
    CHECK(apart > 0.4);
    CHECK(apart < 2.0);
    rig.clock.advance(seconds(2));
    REQUIRE(rig.tick(false));  // 30 min 1 s: re-anchored on the measurement
    const double measured = rig.mount->dec_now();
    CHECK(rig.driver->get_declination() == same_position(measured));
    rig.clock.advance(milliseconds(500));  // and the model runs on from there
    CHECK(axis_arcsec(false, measured, rig.driver->get_declination()) == Approx(0.05 * 0.5).margin(1e-6));
}

// A stalled link: the model extrapolates for kSnapshotMaxAge (3 s) after the last good poll, then the last frame is
// served as it is (stale, never refused).
// Falsified by: offset_model_position_locked() ignoring the reading's stale flag.
TEST_CASE("OnStep rate offsets - the model stops 3 s after the last good poll",
          "[onstep][telescope][rates][offset-model]") {
    Rig rig;
    connect_moving(rig);
    REQUIRE(rig.tick(false));
    const double frame = rig.driver->get_declination();
    rig.driver->set_declination_rate(40.0);
    rig.mount->mute = true;
    rig.clock.advance(seconds(1));
    CHECK(rig.tick(false));  // two failed ticks: no fault latched
    rig.clock.advance(seconds(1));
    CHECK(rig.tick(false));
    REQUIRE(rig.driver->get_link_fault().empty());
    rig.clock.advance(milliseconds(900));  // 2.9 s after the last good poll
    CHECK(axis_arcsec(false, frame, rig.driver->get_declination()) == Approx(40.0 * 2.9).margin(1e-6));
    rig.clock.advance(milliseconds(200));  // 3.1 s
    CHECK(rig.driver->get_declination() == same_position(frame));
    CHECK(has_entry(*rig.driver, "Declination"));
}

// A rate write re-anchors under the old offsets first, so the new rate never applies to the time before it.
// Falsified by: set_rate_offset() assigning the new offset without reanchor_offset_model_locked() before the send.
TEST_CASE("OnStep rate offsets - a rate write re-anchors under the old rate first",
          "[onstep][telescope][rates][offset-model]") {
    Rig rig;
    connect_moving(rig);
    REQUIRE(rig.tick(false));
    rig.driver->set_declination_rate(40.0);
    (void)run_polls(rig, milliseconds(0), milliseconds(3500));
    const double before = rig.driver->get_declination();
    CHECK(axis_arcsec(false, rig.mount->dec_now(), before) == Approx(0.0).margin(kStepArcsec + 1e-6));
    rig.driver->set_declination_rate(10.0);
    CHECK(rig.driver->get_declination() == same_position(before));  // no jump at the write
    (void)run_polls(rig, milliseconds(500), seconds(1));
    CHECK(axis_arcsec(false, before, rig.driver->get_declination()) == Approx(10.0).margin(1e-6));
}

// A TrackingRate change that zeroes the offsets re-anchors first: the position holds there until a poll measures it,
// instead of jumping back to the older frame.
// Falsified by: set_tracking_rate() zeroing the offsets without reanchor_offset_model_locked().
TEST_CASE("OnStep rate offsets - a TrackingRate change that zeroes the offsets does not move the position",
          "[onstep][telescope][rates][offset-model]") {
    Rig rig;
    connect_moving(rig);
    REQUIRE(rig.tick(false));
    rig.driver->set_declination_rate(40.0);
    (void)run_polls(rig, milliseconds(0), milliseconds(2500));
    const double before = rig.driver->get_declination();
    rig.mount->set_status(kLunarStatus);
    rig.driver->set_tracking_rate(1);
    REQUIRE(rig.driver->get_declination_rate() == 0.0);
    CHECK(rig.driver->get_declination() == same_position(before));
    rig.clock.advance(milliseconds(300));
    CHECK(rig.driver->get_declination() == same_position(before));  // the offsets are zero: no motion
    rig.clock.advance(milliseconds(200));
    REQUIRE(rig.tick(false));  // the frame again, where the mount stopped
    CHECK(axis_arcsec(false, before, rig.driver->get_declination()) == Approx(0.0).margin(kStepArcsec + 1e-6));
}

// An offset change the poll finds (set at the handset or by another client) re-anchors under the old offsets first.
// Falsified by: read_rate_offsets_locked() storing the offsets without set_rate_offsets_locked()'s re-anchor.
TEST_CASE("OnStep rate offsets - an offset change the poll finds re-anchors under the old offsets",
          "[onstep][telescope][rates][offset-model]") {
    Rig rig;
    connect_moving(rig);
    REQUIRE(rig.tick(false));
    rig.driver->set_declination_rate(1.0);
    (void)run_polls(rig, milliseconds(0), seconds(1));
    const double at_tick = rig.driver->get_declination();
    rig.clock.advance(seconds(1));
    // The handset sets 0.25 arcsec/s; the frame's compensation letter changes, so the tick reads the offsets again.
    rig.mount->set_offsets_elsewhere("0.00000000", "0.24931739");
    rig.mount->set_status(kCompensatedStatus);
    REQUIRE(rig.tick(false));
    REQUIRE(rig.driver->get_declination_rate() == Approx(0.25).margin(1e-6));
    rig.clock.advance(milliseconds(500));
    CHECK(axis_arcsec(false, at_tick, rig.driver->get_declination()) == Approx(1.0 + 0.25 * 0.5).margin(1e-4));
}

// A sync anchors the model at the synced position.
// Falsified by: sync_to_mount_locked() leaving the model's anchor where it was.
TEST_CASE("OnStep rate offsets - a sync anchors the model at the synced position",
          "[onstep][telescope][rates][offset-model]") {
    Rig rig;
    connect_moving(rig);
    REQUIRE(rig.tick(false));
    rig.driver->set_declination_rate(40.0);
    (void)run_polls(rig, milliseconds(0), milliseconds(2500));
    rig.driver->sync_to_coordinates(6.5, 20.0);
    CHECK(rig.driver->get_right_ascension() == same_position(6.5));
    CHECK(rig.driver->get_declination() == same_position(20.0));
    rig.clock.advance(milliseconds(400));
    CHECK(axis_arcsec(false, 20.0, rig.driver->get_declination()) == Approx(16.0).margin(1e-6));
}

// RA stays in [0, 24) hours and Dec within +/-90 degrees under the model.
// Falsified by: offset_model_position() without util::wrap_hours() or the clamp.
TEST_CASE("OnStep rate offsets - the model wraps RA at 24 h and clamps Dec at the poles",
          "[onstep][telescope][rates][offset-model]") {
    Rig rig;
    connect_moving(rig);
    SECTION("RA wraps") {
        // 2.67 s of RA short of 24 h. 40 axis arcsec per sidereal second is 2.674 s of RA per SI second.
        constexpr double kStartSeconds = 2.67;
        const double ra_per_si_second = 40.0 / 15.0 / kSiSecondsPerSiderealSecond;
        rig.mount->set_position(24.0 - kStartSeconds / 3600.0, 45.0);
        REQUIRE(rig.tick(false));
        rig.driver->set_right_ascension_rate(40.0 / 15.0);
        rig.clock.advance(milliseconds(500));
        CHECK((24.0 - rig.driver->get_right_ascension()) * 3600.0 ==
              Approx(kStartSeconds - ra_per_si_second * 0.5).margin(1e-6));
        rig.clock.advance(milliseconds(500));
        // The model has passed 24 h and the mount, 40 whole steps on, has not: the guard compares the short way round
        // and keeps the anchor.
        REQUIRE(rig.tick(false));
        REQUIRE(rig.mount->ra_now() > 23.9);
        rig.clock.advance(milliseconds(500));
        const double ra = rig.driver->get_right_ascension();
        CHECK(ra >= 0.0);
        CHECK(ra < 24.0);
        CHECK(ra * 3600.0 == Approx(ra_per_si_second * 1.5 - kStartSeconds).margin(1e-6));
    }
    for (const double pole : {90.0, -90.0}) {
        DYNAMIC_SECTION("Dec clamps at " << pole) {
            rig.mount->set_position(12.0, pole - std::copysign(0.01, pole));
            REQUIRE(rig.tick(false));
            rig.driver->set_declination_rate(std::copysign(40.0, pole));
            rig.clock.advance(seconds(1));
            CHECK(rig.driver->get_declination() == pole);
        }
    }
}

// The model applies only on OnStepX, with a non-zero offset, while the frame says the mount tracks and is not slewing,
// guiding, moving an axis, homing, parking or parked: otherwise two reads between polls serve the same frame.
// Not red on the code before #2241 (the frame was always served): it pins what the model must not change.
// Falsified by: offset_model_allowed_locked() returning true whatever the frame's status. The 4.x section pins the
// 4.x behaviour only: 4.x never reads an offset and refuses the writes, so no offset is stored there and no model
// runs; the `!onstepx_` term in offset_model_allowed_locked() is defensive and this section does not falsify it.
TEST_CASE("OnStep rate offsets - no model while the mount is not just tracking, nor on OnStep 4.x",
          "[onstep][telescope][rates][offset-model]") {
    const std::vector<std::pair<const char*, const char*>> states = {
        {"slewing", kGotoStatus},       {"parked", kParkedStatus},       {"not tracking", kNotTrackingStatus},
        {"pulse guiding", "NpGEW180#"}, {"moving an axis", "NpgEW180#"}, {"homing", "NphEW180#"},
        {"parking", "NpIEW180#"},
    };
    for (const auto& [name, status] : states) {
        CAPTURE(name);
        Rig rig;
        connect_moving(rig);
        REQUIRE(rig.tick(false));
        rig.driver->set_declination_rate(40.0);
        rig.mount->set_status(status);
        rig.clock.advance(seconds(1));
        REQUIRE(rig.tick(false));
        const double frame = rig.mount->dec_now();
        CHECK(rig.driver->get_declination() == same_position(frame));
        rig.clock.advance(milliseconds(800));
        CHECK(rig.driver->get_declination() == same_position(frame));
    }
    SECTION("OnStep 4.x") {
        Rig rig;
        {
            std::lock_guard<std::mutex> lock(rig.mount->mutex);
            rig.mount->gxtd = "39.89078300";  // an offset at the mount the driver never reads on 4.x
        }
        connect_moving(rig, "4.24q");
        REQUIRE(rig.tick(false));
        const double frame = rig.driver->get_declination();
        rig.clock.advance(milliseconds(800));
        CHECK(rig.driver->get_declination() == frame);
        CHECK(rig.driver->get_declination_rate() == 0.0);
    }
}

// DeviceState serves the same position as the getters (maintainer decision on #2241); its TimeStamp stays the
// poll's measurement time.
// Falsified by: get_right_ascension() reading the frame while a DeviceState path reads the model, or the reverse.
TEST_CASE("OnStep rate offsets - DeviceState serves the getters' position",
          "[onstep][telescope][rates][offset-model]") {
    Rig rig;
    connect_moving(rig);
    REQUIRE(rig.tick(false));
    const double frame = rig.driver->get_declination();
    rig.driver->set_declination_rate(40.0);
    rig.clock.advance(milliseconds(700));
    const auto state = rig.driver->get_device_state();
    const auto entry = std::find_if(state.begin(), state.end(), [](const auto& e) { return e.name == "Declination"; });
    REQUIRE(entry != state.end());
    const double from_state = std::get<double>(entry->value);
    CHECK(from_state == same_position(rig.driver->get_declination()));
    CHECK(axis_arcsec(false, frame, from_state) == Approx(28.0).margin(1e-6));  // the model, not the frame
}

// ── #2241 review: what the driver itself does while an offset runs ─────────────────────────────────────────────────

namespace {

// DeclinationRate 40 written right after a tick, a tick 1 s later, then 400 ms on: the model serves the Dec that tick
// measured plus 16 arcsec. Returns that measured Dec (what a getter serves when the frame is served).
double model_running(Rig& rig) {
    connect_moving(rig);
    REQUIRE(rig.tick(false));
    rig.driver->set_declination_rate(40.0);
    rig.clock.advance(seconds(1));
    REQUIRE(rig.tick(false));
    const double frame = rig.mount->dec_now();
    rig.clock.advance(milliseconds(400));
    REQUIRE(axis_arcsec(false, frame, rig.driver->get_declination()) == Approx(16.0).margin(1e-4));
    return frame;
}

// `wait` on, a tick: the model anchors on that measurement and runs at 40 arcsec/s from it again.
void model_restarts(Rig& rig, milliseconds wait) {
    rig.clock.advance(wait);
    REQUIRE(rig.tick(false));
    const double measured = rig.mount->dec_now();
    CHECK(rig.driver->get_declination() == same_position(measured));
    rig.clock.advance(milliseconds(400));
    CHECK(axis_arcsec(false, measured, rig.driver->get_declination()) == Approx(16.0).margin(1e-4));
}

// The mount lands 1 arcsec north of where the offset took it (inside the 2 arcsec guard), `wait` on a tick measures
// it, and the getters serve that measurement: the model anchored on it, not on its anchor from before the command.
void lands_on_measurement(Rig& rig, milliseconds wait) {
    rig.mount->set_position(12.0, 45.0 + 1.0 / 3600.0);
    rig.clock.advance(wait);
    REQUIRE(rig.tick(false));
    CHECK(rig.driver->get_declination() == same_position(rig.mount->dec_now()));
}

// Runs one tick on its own thread, stopped after its sample until `release()`; `done` once it has published.
struct HeldTick {
    alpacacore::test::Latch sampled;
    std::atomic<bool> once{false};
    std::atomic<bool> done{false};
    std::thread thread;
    Rig& rig;

    explicit HeldTick(Rig& r) : rig(r) {
        onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, [this] {
            if (!once.exchange(true)) sampled.block();
        });
        thread = std::thread([this] {
            (void)rig.tick(false);
            done = true;
        });
        REQUIRE(sampled.wait_entered());
    }
    // From the mount's server thread, inside a command's exchange: the tick publishes now.
    void publish_now() {
        sampled.release();
        const auto give_up = std::chrono::steady_clock::now() + seconds(5);
        while (!done.load() && std::chrono::steady_clock::now() < give_up) std::this_thread::sleep_for(milliseconds(1));
    }
    ~HeldTick() {
        sampled.release();
        if (thread.joinable()) thread.join();
        onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, nullptr);
    }
    HeldTick(const HeldTick&) = delete;
    HeldTick& operator=(const HeldTick&) = delete;
};

}  // namespace

// A tick that sampled before a write that sets the offsets to zero, and publishes after it, must not end the hold
// of the position the old offsets reached: the getters would jump back to the older frame (20 arcsec at 40 arcsec/s
// and 500 ms). Only a frame measured after the write ends it.
// Falsified by: adopt_offset_model_frame_locked() dropping the model on a zero offset before the
// offset_model_floor_ check.
TEST_CASE("OnStep rate offsets - a tick sampled before a write that zeroes the offsets does not move the position",
          "[onstep][telescope][rates][offset-model]") {
    Rig rig;
    connect_moving(rig);
    REQUIRE(rig.tick(false));
    rig.driver->set_declination_rate(40.0);
    rig.clock.advance(seconds(1));
    REQUIRE(rig.tick(false));
    rig.clock.advance(seconds(1));
    double before = std::numeric_limits<double>::quiet_NaN();

    SECTION("DeclinationRate 0") {
        std::atomic<bool> once{false};
        onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, [&] {
            if (once.exchange(true)) return;
            rig.clock.advance(milliseconds(500));
            before = rig.driver->get_declination();
            rig.driver->set_declination_rate(0.0);
        });
        REQUIRE(rig.tick(false));  // sampled 500 ms before the write, published after it
        onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, nullptr);
    }
    SECTION("a TrackingRate change, the tick publishing during its rate command") {
        HeldTick held(rig);
        rig.clock.advance(milliseconds(500));
        before = rig.driver->get_declination();
        rig.mount->set_on_command([&](const std::string& chunk) {
            if (chunk.rfind(":TL", 0) == 0) held.publish_now();
        });
        rig.driver->set_tracking_rate(1);
        rig.mount->set_on_command(nullptr);
        REQUIRE(held.done.load());
        REQUIRE(rig.driver->get_declination_rate() == 0.0);
    }
    CHECK(rig.driver->get_declination() == same_position(before));
    rig.clock.advance(milliseconds(500));
    REQUIRE(rig.tick(false));  // measured after the write: the frame again, where the mount stopped
    CHECK(rig.driver->get_declination() == same_position(rig.mount->dec_now()));
    CHECK(axis_arcsec(false, before, rig.driver->get_declination()) == Approx(0.0).margin(kStepArcsec + 1e-6));
}

// A rate write while the frame is stale (no good poll for 3 s) starts no model from that frame: carried forward under
// the old offset it would be 1.75 arcsec off a stalled axis, inside the guard, so the next poll would keep it.
// Falsified by: reanchor_offset_model_locked() carrying a stale frame forward (no reading->stale check).
TEST_CASE("OnStep rate offsets - a rate write on a stale frame starts the model from the next measurement",
          "[onstep][telescope][rates][offset-model]") {
    Rig rig;
    connect_moving(rig);
    REQUIRE(rig.tick(false));
    rig.driver->set_declination_rate(0.5);
    rig.mount->stall_dec = true;
    const double frame = rig.mount->dec_now();
    rig.clock.advance(milliseconds(3500));  // no poll: the frame is 3.5 s old
    CHECK(rig.driver->get_declination() == same_position(frame));
    rig.driver->set_declination_rate(0.25);
    CHECK(rig.driver->get_declination() == same_position(frame));
    REQUIRE(rig.tick(false));
    CHECK(rig.driver->get_declination() == same_position(rig.mount->dec_now()));
    rig.clock.advance(milliseconds(400));
    CHECK(axis_arcsec(false, frame, rig.driver->get_declination()) == Approx(0.25 * 0.4).margin(1e-6));
}

// An offset the poll finds while the mount tracks at another rate runs no model: ITelescopeV4 reads the offsets as 0
// there, and the model has no lunar, solar or King term.
// Falsified by: offset_model_allowed_locked() not requiring tracking_rate_known_ 0 while an offset is set.
TEST_CASE("OnStep rate offsets - an offset found at the lunar rate runs no model",
          "[onstep][telescope][rates][offset-model]") {
    Rig rig;
    connect_moving(rig);
    REQUIRE(rig.tick(false));
    const auto reads = rig.mount->count(":GXTD");
    rig.mount->set_offsets_elsewhere("0.00000000", "39.89078300");  // 40 arcsec per SI second at the mount
    show(rig, kLunarStatus);  // the rate letter changes: the tick reads the offsets again
    REQUIRE(rig.mount->count(":GXTD") > reads);
    REQUIRE(rig.driver->get_tracking_rate() == 1);
    const double frame = rig.mount->dec_now();
    CHECK(rig.driver->get_declination() == same_position(frame));
    rig.clock.advance(milliseconds(400));
    CHECK(rig.driver->get_declination() == same_position(frame));
}

// A PulseGuide: the frame is served from the moment it goes out until a frame sampled 50 ms or more after its end
// anchors the model again.
// Falsified by: offset_model_allowed_locked() not refusing an armed pulse_hold_ (the read inside the :Mg exchange);
// pulse_guide() not calling drop_offset_model_locked() (the read past the end); adopt_offset_model_frame_locked()
// ignoring after_pulses (the read after the tick inside the settle).
TEST_CASE("OnStep rate offsets - a PulseGuide serves the frame until a frame measured after its end",
          "[onstep][telescope][rates][offset-model]") {
    Rig rig;
    const double frame = model_running(rig);
    double during = std::numeric_limits<double>::quiet_NaN();
    rig.mount->set_on_command([&](const std::string& chunk) {
        if (chunk.rfind(":Mg", 0) == 0) during = rig.driver->get_declination();
    });
    rig.driver->pulse_guide(0, 500);  // North, 0.5 s
    rig.mount->set_on_command(nullptr);
    CHECK(during == same_position(frame));  // the hold is armed before the command goes out
    CHECK(rig.driver->get_declination() == same_position(frame));
    rig.clock.advance(milliseconds(510));  // past the pulse's end, no tick yet
    CHECK(rig.driver->get_declination() == same_position(frame));
    rig.clock.advance(milliseconds(10));
    REQUIRE(rig.tick(false));  // sampled 20 ms after the end, inside the 50 ms settle
    const double settling = rig.mount->dec_now();
    CHECK(rig.driver->get_declination() == same_position(settling));
    rig.clock.advance(milliseconds(200));
    CHECK(rig.driver->get_declination() == same_position(settling));
    model_restarts(rig, milliseconds(800));
}

// A MoveAxis: no model while the axis moves, and none from a tick that sampled the move (or a pulse a stop ended)
// and published after the stop.
// Falsified by: offset_model_allowed_locked() not refusing manual_axis_slewing_ (a frame measured during the move);
// move_axis()'s stop not calling drop_offset_model_locked() (a tick published after the stop, for a move and for a
// pulse).
TEST_CASE("OnStep rate offsets - a MoveAxis serves the frame until a frame measured after it",
          "[onstep][telescope][rates][offset-model]") {
    Rig rig;
    const double frame = model_running(rig);

    SECTION("a frame measured during the move") {
        rig.driver->move_axis(0, 0.5);
        CHECK(rig.driver->get_declination() == same_position(frame));
        rig.clock.advance(milliseconds(600));
        REQUIRE(rig.tick(false));
        const double moving = rig.mount->dec_now();
        CHECK(rig.driver->get_declination() == same_position(moving));
        rig.clock.advance(milliseconds(400));
        CHECK(rig.driver->get_declination() == same_position(moving));
        rig.driver->move_axis(0, 0.0);
        model_restarts(rig, milliseconds(600));
    }
    SECTION("a tick that sampled the move, published after the stop") {
        rig.driver->move_axis(0, 0.5);
        rig.clock.advance(milliseconds(600));
        std::atomic<bool> once{false};
        double moved = std::numeric_limits<double>::quiet_NaN();
        onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, [&] {
            if (once.exchange(true)) return;
            moved = rig.mount->dec_now();          // what the tick sampled
            rig.clock.advance(milliseconds(100));  // the stop goes out after the sample
            rig.driver->move_axis(0, 0.0);
        });
        REQUIRE(rig.tick(false));
        onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, nullptr);
        CHECK(rig.driver->get_declination() == same_position(moved));
        rig.clock.advance(milliseconds(400));
        CHECK(rig.driver->get_declination() == same_position(moved));
        model_restarts(rig, milliseconds(500));
    }
    SECTION("a tick that sampled a pulse, published after the stop that ended it") {
        rig.driver->pulse_guide(0, 3000);  // North, 3 s
        rig.clock.advance(milliseconds(600));
        std::atomic<bool> once{false};
        double pulsed = std::numeric_limits<double>::quiet_NaN();
        onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, [&] {
            if (once.exchange(true)) return;
            pulsed = rig.mount->dec_now();         // what the tick sampled
            rig.clock.advance(milliseconds(100));  // the stop goes out after the sample
            rig.driver->move_axis(1, 0.0);         // :Qn#/:Qs# end the pulse
        });
        REQUIRE(rig.tick(false));
        onstep::set_status_poll_after_sample_hook_for_testing(*rig.driver, nullptr);
        CHECK(rig.driver->get_declination() == same_position(pulsed));
        rig.clock.advance(milliseconds(400));
        CHECK(rig.driver->get_declination() == same_position(pulsed));
        model_restarts(rig, milliseconds(600));
    }
}

// A GOTO, Park or FindHome: no model from the moment the command goes out, and the first frame measured after it
// anchors the model on its measurement, even one inside the guard of the anchor from before.
// Falsified by: offset_model_allowed_locked() not refusing slew_hold_ (the read inside the :MS# exchange) or
// park_home_request_ (a Park the mount refused "in motion"); dispatch_goto_locked() or finish_park_or_home_locked()
// not calling drop_offset_model_locked() (the landing frame).
TEST_CASE("OnStep rate offsets - a GOTO, Park or FindHome serves the frame until a frame measured after it",
          "[onstep][telescope][rates][offset-model]") {
    Rig rig;
    const double frame = model_running(rig);

    SECTION("a GOTO") {
        double during = std::numeric_limits<double>::quiet_NaN();
        rig.mount->set_on_command([&](const std::string& chunk) {
            if (chunk.rfind(":MS", 0) == 0) during = rig.driver->get_declination();
        });
        rig.driver->slew_to_coordinates_async(12.0, 45.0);
        rig.mount->set_on_command(nullptr);
        CHECK(during == same_position(frame));  // the slew hold is armed before :MS# goes out
        CHECK(rig.driver->get_declination() == same_position(frame));
        lands_on_measurement(rig, milliseconds(600));
    }
    SECTION("a Park the mount refuses while in motion") {
        rig.mount->refuse_park = true;
        rig.driver->park();  // pending: the body retries
        CHECK(rig.driver->get_declination() == same_position(frame));
    }
    SECTION("a Park the mount takes") {
        rig.driver->park();
        CHECK(rig.driver->get_declination() == same_position(frame));
        lands_on_measurement(rig, milliseconds(600));
    }
    SECTION("a FindHome the mount takes") {
        rig.driver->find_home();
        CHECK(rig.driver->get_declination() == same_position(frame));
        lands_on_measurement(rig, milliseconds(600));
    }
}

// AbortSlew and a Tracking write drop the model; the next frame measured anchors it again.
// Falsified by: abort_slew() or set_tracking() not calling drop_offset_model_locked().
TEST_CASE("OnStep rate offsets - AbortSlew and a Tracking write serve the frame until the next measurement",
          "[onstep][telescope][rates][offset-model]") {
    Rig rig;
    const double frame = model_running(rig);
    SECTION("AbortSlew") { rig.driver->abort_slew(); }
    SECTION("Tracking true") { rig.driver->set_tracking(true); }
    CHECK(rig.driver->get_declination() == same_position(frame));
    rig.clock.advance(milliseconds(300));
    CHECK(rig.driver->get_declination() == same_position(frame));
    model_restarts(rig, milliseconds(300));
}

namespace {

// The error code and message a call throws: {0, ""} when it does not throw.
std::pair<int, std::string> thrown_by(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const alpacacore::AlpacaException& e) {
        return {e.error_code(), e.what()};
    }
    return {0, ""};
}

bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

// A paused rig whose mount answers `version`, with a coordinate GOTO started from a tracking, idle mount (a #1230
// tracking restore is owed when it ends) and the slew grace run out.
void goto_started(Rig& rig, const char* version = "10.23a", const char* before = "NpEW180#") {
    connect_rates(rig, version);
    show(rig, before);
    rig.driver->slew_to_coordinates_async(2.0, 20.0);
    rig.clock.advance(seconds(3));
}

}  // namespace

// #2296: OnStepX stops a GOTO that crosses a limit (Limits::poll() -> stop(): goTo.abort(), tracking off) and says
// why only in the :GU# general error, the last character ('0' + code). The frame that ends a GOTO the driver started
// with a stop code, away from its target, fails the slew: Slewing throws until the next motion command, and the #1230
// tracking restore is not sent (OnStepX does not limit-check :Te#, so tracking would carry the mount further past the
// limit). The fake's position, 12 h +45, is far from the 2 h +20 target.
TEST_CASE("OnStep limit stop - a GOTO the mount stops at a limit fails Slewing and gets no tracking restore",
          "[onstep][telescope][snapshot][limit-stop]") {
    Rig rig;
    SECTION("stopped at the horizon limit (2)") {
        goto_started(rig);
        show(rig, "nNpEW182#");  // the GOTO ended, tracking off
        const auto [code, message] = thrown_by([&] { (void)rig.driver->get_slewing(); });
        CHECK(code == alpacacore::AlpacaError::DriverException);
        CHECK(contains(message, "SlewToCoordinatesAsync"));
        CHECK(contains(message, "horizon limit"));
        CHECK(rig.mount->count(":Te#") == 0);
        CHECK_NOTHROW(rig.driver->get_at_park());  // a slew's failure, not a park's
        show(rig, "nNpEW182#");                    // kept: a later frame neither clears it nor restores
        CHECK(thrown_by([&] { (void)rig.driver->get_slewing(); }).first == alpacacore::AlpacaError::DriverException);
        CHECK(rig.mount->count(":Te#") == 0);
    }
    SECTION("stopped at the meridian limit (7)") {
        goto_started(rig);
        show(rig, "nNpEW187#");
        const auto [code, message] = thrown_by([&] { (void)rig.driver->get_slewing(); });
        CHECK(code == alpacacore::AlpacaError::DriverException);
        CHECK(contains(message, "meridian limit"));
        CHECK(rig.mount->count(":Te#") == 0);
    }
    SECTION("a synchronous SlewToCoordinates stopped at the overhead limit (12, '<') throws") {
        connect_rates(rig);
        show(rig, "NpEW180#");
        std::atomic<int> reads_after_goto{-1};  // -1: the :MS# has not gone out
        rig.mount->set_on_command([&](const std::string& chunk) {
            if (chunk.rfind(":MS", 0) == 0) {
                reads_after_goto = 0;
            } else if (chunk.rfind(":GU", 0) == 0 && reads_after_goto.load() >= 0) {
                rig.mount->set_status(reads_after_goto.fetch_add(1) == 0 ? "npEW180#" : "nNpEW18<#");
            }
        });
        const auto [code, message] = thrown_by([&] { synchronous_slew(rig, 2.0, 20.0); });
        rig.mount->set_on_command(nullptr);
        CHECK(code == alpacacore::AlpacaError::DriverException);
        CHECK(contains(message, "SlewToCoordinates"));
        CHECK(contains(message, "overhead limit"));
        CHECK(rig.mount->count(":Te#") == 0);
    }
    SECTION("OnStepX: a GOTO accepted while the meridian limit already stood ends with the same code (7)") {
        // OnStepX's code is the limits as they stand: past the meridian limit it accepts the GOTO and aborts it on
        // its next limit check (Limits::poll()), so the same code at the end is this GOTO's stop.
        goto_started(rig, "10.23a", "NpEW187#");
        show(rig, "nNpEW187#");
        const auto [code, message] = thrown_by([&] { (void)rig.driver->get_slewing(); });
        CHECK(code == alpacacore::AlpacaError::DriverException);
        CHECK(contains(message, "meridian limit"));
        CHECK(rig.mount->count(":Te#") == 0);
    }
    SECTION("OnStep 4.x: stopped short with a stop code") {
        goto_started(rig, "4.24s");
        show(rig, "nNpEW182#");
        CHECK(thrown_by([&] { (void)rig.driver->get_slewing(); }).first == alpacacore::AlpacaError::DriverException);
        CHECK(rig.mount->count(":ST60") == 0);
    }
}

// The other side of #2296: with no stop code a GOTO lands as before, and with one where the mount ends decides. On
// OnStep 4.x, which latches the code until :Te#, an unpark, a home reset or a reboot (Command.ino initGeneralError(),
// OnStep.ino), a GOTO at its target with a latched code lands. These landings end as before, with the #1230 tracking
// restore.
TEST_CASE("OnStep limit stop - a GOTO that lands with no stop code, or at its target with a 4.x latched code, lands",
          "[onstep][telescope][snapshot][limit-stop]") {
    Rig rig;
    SECTION("error 0") {
        goto_started(rig);
        show(rig, "nNpEW180#");
        CHECK(rig.mount->count(":Te#") == 1);
    }
    SECTION("a code that is not a stop (10, goto sync)") {
        goto_started(rig);
        show(rig, "nNpEW18:#");
        CHECK(rig.mount->count(":Te#") == 1);
    }
    SECTION("no trailing code characters") {
        goto_started(rig);
        show(rig, "nNpEW#");
        CHECK(rig.mount->count(":Te#") == 1);
    }
    SECTION("OnStep 4.x: at the target with a latched code") {
        goto_started(rig, "4.24s", "NpEW187#");
        rig.mount->set_position(2.0, 20.0);  // landed: the target (a stop code makes the end position decide)
        show(rig, "nNpEW187#");
        CHECK(rig.mount->count(":ST60.164275#") == 1);
    }
    CHECK_NOTHROW(rig.driver->get_slewing());
    CHECK_FALSE(rig.driver->get_slewing());
}

// #2296: a park a limit stops ends unparked (OnStepX Park::requestAborted()) with the stop code, and a park whose
// park-sense check fails shows 'F' (PS_PARK_FAILED; OnStep 4.x ParkFailed); a home a limit stops ends away from home
// with the code. AtPark (a park's) or AtHome (a home's) and Slewing raise it, as they raise a refusal the park_home_
// body kept.
TEST_CASE("OnStep limit stop - a park or home the mount stops, or a park it reports failed, fails AtPark or AtHome",
          "[onstep][telescope][snapshot][limit-stop]") {
    Rig rig;
    connect_rates(rig);
    show(rig, "NpEW180#");
    SECTION("a park stopped at the meridian limit (7)") {
        rig.driver->park();
        REQUIRE(rig.mount->count(":hP#") == 1);
        show(rig, "nNpEW187#");
        const auto [code, message] = thrown_by([&] { (void)rig.driver->get_at_park(); });
        CHECK(code == alpacacore::AlpacaError::DriverException);
        CHECK(contains(message, "Park"));
        CHECK(contains(message, "meridian limit"));
        CHECK(thrown_by([&] { (void)rig.driver->get_slewing(); }).first == alpacacore::AlpacaError::DriverException);
        CHECK_NOTHROW(rig.driver->get_at_home());
    }
    SECTION("a park the mount reports failed ('F')") {
        rig.driver->park();
        show(rig, "nNFEW180#");
        const auto [code, message] = thrown_by([&] { (void)rig.driver->get_at_park(); });
        CHECK(code == alpacacore::AlpacaError::DriverException);
        CHECK(contains(message, "failed"));
    }
    SECTION("a home stopped at a limit switch (3)") {
        rig.driver->find_home();
        show(rig, "nphEW180#");  // the home GOTO runs ('h' with no 'N')
        show(rig, "nNpEW183#");
        const auto [code, message] = thrown_by([&] { (void)rig.driver->get_at_home(); });
        CHECK(code == alpacacore::AlpacaError::DriverException);
        CHECK(contains(message, "FindHome"));
        CHECK(contains(message, "limit switch"));
        CHECK_NOTHROW(rig.driver->get_at_park());
    }
    SECTION("a park that ends parked, with a stop code still set, is done") {
        rig.driver->park();
        show(rig, "nNPEW187#");
        CHECK(rig.driver->get_at_park());
        CHECK_FALSE(rig.driver->get_slewing());
    }
}

// #2296: the mount stops the GOTO at the horizon limit and, before any poll tick reads it, the client calls AbortSlew.
// The abort's own :GU# read is the first to see the end: it must find the stop and send no #1230 tracking restore
// (OnStepX does not limit-check :Te#). The abort is a new motion command, so it drops the failure it found. On OnStep
// 4.x only the watch the abort keeps decides this (the abort's stop-code guard is OnStepX's): a GOTO stopped short gets
// no :ST60.164275#.
TEST_CASE("OnStep limit stop - an AbortSlew that is first to read a GOTO the mount stopped sends no tracking restore",
          "[onstep][telescope][snapshot][limit-stop]") {
    Rig rig;
    SECTION("OnStepX") {
        goto_started(rig);
        rig.mount->set_status("nNpEW182#");  // stopped at the horizon, tracking off; no tick has read it
        rig.driver->abort_slew();
        CHECK(rig.mount->count(":Te#") == 0);
        CHECK_NOTHROW(rig.driver->get_slewing());
        REQUIRE(rig.tick());
        CHECK(rig.mount->count(":Te#") == 0);
        CHECK_NOTHROW(rig.driver->get_slewing());
    }
    SECTION("OnStep 4.x, stopped short") {
        goto_started(rig, "4.24s");
        rig.mount->set_status("nNpEW182#");  // the fake's position is far from the 2 h +20 target
        rig.driver->abort_slew();
        CHECK(rig.mount->count(":ST60") == 0);
        CHECK_NOTHROW(rig.driver->get_slewing());
    }
}

namespace {

// Sets the #521 relink preserve window for one case and puts the default back.
struct PreserveWindow {
    explicit PreserveWindow(std::chrono::milliseconds window) {
        onstep::detail::set_relink_motion_preserve_window(window);
    }
    ~PreserveWindow() {
        onstep::detail::set_relink_motion_preserve_window(
            std::chrono::duration_cast<std::chrono::milliseconds>(alpacacore::util::kRelinkMotionPreserveWindow));
    }
    PreserveWindow(const PreserveWindow&) = delete;
    PreserveWindow& operator=(const PreserveWindow&) = delete;
};

// The controller hangs up (a lost link, decision 0009) and the client's Connect relinks at once.
void lose_and_relink(Rig& rig) {
    REQUIRE(rig.server.drop_connections());
    REQUIRE_FALSE(rig.driver->get_connected());
    rig.driver->set_connected(true);
    REQUIRE(rig.driver->get_connected());
}

}  // namespace

// #2296 with #521: a relink inside the preserve window leaves the mount's GOTO running (and a park, which it never
// stops), so the watch on that motion carries over and a limit stop after the relink still fails it. A relink past
// the window sends AbortSlew's stops: the motion is the driver's own stop then, and its end is not checked.
TEST_CASE("OnStep limit stop - a GOTO or park a relink leaves running still fails when the mount stops it",
          "[onstep][telescope][snapshot][limit-stop]") {
    Rig rig;
    SECTION("a GOTO, relinked inside the window, stopped at the meridian limit (7)") {
        PreserveWindow window(std::chrono::hours(1));
        goto_started(rig);
        rig.mount->set_status("npEW180#");  // the GOTO runs through the outage
        lose_and_relink(rig);
        CHECK(rig.mount->count(":Q") == 0);  // motion left alone
        show(rig, "nNpEW187#");
        const auto [code, message] = thrown_by([&] { (void)rig.driver->get_slewing(); });
        CHECK(code == alpacacore::AlpacaError::DriverException);
        CHECK(contains(message, "SlewToCoordinatesAsync"));
        CHECK(contains(message, "meridian limit"));
        CHECK(rig.mount->count(":Te#") == 0);
    }
    SECTION("a park, relinked inside the window, stopped at the horizon limit (2)") {
        PreserveWindow window(std::chrono::hours(1));
        connect_rates(rig);
        show(rig, "NpEW180#");
        rig.driver->park();
        rig.mount->set_status("nIEW180#");  // parking
        lose_and_relink(rig);
        show(rig, "nNpEW182#");
        const auto [code, message] = thrown_by([&] { (void)rig.driver->get_at_park(); });
        CHECK(code == alpacacore::AlpacaError::DriverException);
        CHECK(contains(message, "horizon limit"));
    }
    SECTION("a park, relinked past the window while it parks, that the mount reports failed ('F')") {
        PreserveWindow window(std::chrono::milliseconds(0));  // the #1289 rule leaves a park running anyway
        connect_rates(rig);
        show(rig, "NpEW180#");
        rig.driver->park();
        rig.mount->set_status("nIEW180#");
        lose_and_relink(rig);
        CHECK(rig.mount->count(":Q") == 0);
        show(rig, "nNFEW180#");
        CHECK(thrown_by([&] { (void)rig.driver->get_at_park(); }).first == alpacacore::AlpacaError::DriverException);
    }
    SECTION("a GOTO the relink stops past the window is not checked") {
        PreserveWindow window(std::chrono::milliseconds(0));
        goto_started(rig);
        rig.mount->set_status("npEW180#");
        lose_and_relink(rig);
        REQUIRE(rig.mount->count(":Q#") == 1);  // the #521 stops
        show(rig, "nNpEW187#");
        CHECK_NOTHROW(rig.driver->get_slewing());
        CHECK(rig.mount->count(":Te#") == 0);
    }
    // Item 9: past the window the relink stops a GOTO but leaves a park running (#1289), so the park's watch carries
    // over there too. Its bound still runs from the Park command (the outage counts: it is part of the park's time).
    SECTION("a park the relink past the window leaves running") {
        PreserveWindow window(std::chrono::milliseconds(0));
        connect_rates(rig);
        show(rig, "NpEW180#");
        rig.driver->park();
        const auto parked_at = rig.clock.now();
        rig.mount->set_status("nIEW180#");
        lose_and_relink(rig);
        REQUIRE(rig.mount->count(":Q") == 0);
        SECTION("ends parked") {
            show(rig, "nNPEW180#");
            CHECK(rig.driver->get_at_park());
        }
        SECTION("ends with a stop code") {
            show(rig, "nNpEW187#");
            const auto [code, message] = thrown_by([&] { (void)rig.driver->get_at_park(); });
            CHECK(code == alpacacore::AlpacaError::DriverException);
            CHECK(contains(message, "meridian limit"));
        }
        SECTION("ends without parking and without a code") {
            show(rig, "nNpEW180#");
            CHECK(contains(thrown_by([&] { (void)rig.driver->get_at_park(); }).second,
                           "the mount ended the park without parking"));
        }
        SECTION("never ends: the bound runs from the Park command") {
            rig.clock.advance(std::chrono::minutes(20) - (rig.clock.now() - parked_at) - milliseconds(1));
            show(rig, "nIEW180#");
            CHECK_NOTHROW(rig.driver->get_at_park());
            rig.clock.advance(milliseconds(1));
            CHECK(contains(thrown_by([&] { (void)rig.driver->get_at_park(); }).second,
                           "did not finish the park within 20 minutes"));
        }
    }
}

// Item 7 (#2296): OnStepX writes the limits as they stand, so a GOTO that lands close to a limit and then tracks
// across it shows the stop code in its first end frame. Where the mount is decides: at the target it landed (no
// failure; tracking is not restarted past the limit), away from it the GOTO stopped short.
TEST_CASE("OnStep limit stop - an OnStepX GOTO whose end frame shows a stop code is judged by where it ends",
          "[onstep][telescope][snapshot][limit-stop]") {
    Rig rig;
    goto_started(rig);
    SECTION("at the target: landed, then the limit stopped tracking") {
        rig.mount->set_position(2.0, 20.0);
        show(rig, "nNpEW187#");
        CHECK_NOTHROW(rig.driver->get_slewing());
        CHECK_FALSE(rig.driver->get_slewing());
        CHECK(rig.mount->count(":Te#") == 0);
    }
    SECTION("away from the target: stopped short") {
        rig.mount->set_position(2.5, 20.0);  // 7 degrees short in RA
        show(rig, "nNpEW187#");
        CHECK(contains(thrown_by([&] { (void)rig.driver->get_slewing(); }).second, "meridian limit"));
        CHECK(rig.mount->count(":Te#") == 0);
    }
}

// Item 4 (#2296): OnStep 4.x latches the code until :Te#, an unpark, a home reset or a reboot, so the end frame of a
// GOTO can carry a code a limit stop then writes again, or an older stop's. Where the mount ends decides; a 4.x landing
// keeps its #1230 restore (4.x re-checks its limits on every pass and stops tracking again past one).
TEST_CASE("OnStep limit stop - an OnStep 4.x GOTO whose end frame shows a latched stop code is judged by where it ends",
          "[onstep][telescope][snapshot][limit-stop]") {
    Rig rig;
    goto_started(rig, "4.24s", "NpEW187#");
    SECTION("stopped short by the same code") {
        show(rig, "nNpEW187#");  // the fake's position, 12 h +45, is far from the 2 h +20 target
        CHECK(contains(thrown_by([&] { (void)rig.driver->get_slewing(); }).second, "meridian limit"));
        CHECK(rig.mount->count(":ST60") == 0);
    }
    SECTION("landed with the latched code") {
        rig.mount->set_position(2.0, 20.0);
        show(rig, "nNpEW187#");
        CHECK_NOTHROW(rig.driver->get_slewing());
        CHECK(rig.mount->count(":ST60.164275#") == 1);
    }
}

// Item 8 (#2296): a 4.x GOTO carried over a relink inside the preserve window is judged by the same end rules from
// the relink's first :GU#: the latched code can change while the link is down, and only where the mount ends counts.
TEST_CASE("OnStep limit stop - an OnStep 4.x GOTO carried over a relink is judged by where it ends",
          "[onstep][telescope][snapshot][limit-stop]") {
    PreserveWindow window(std::chrono::hours(1));
    Rig rig;
    SECTION("the latched code changes while it runs, then it lands") {
        goto_started(rig, "4.24s");
        rig.mount->set_status("pEW187#");  // still running; a code latched during the outage
        lose_and_relink(rig);
        rig.mount->set_position(2.0, 20.0);
        show(rig, "NpEW187#");
        CHECK_NOTHROW(rig.driver->get_slewing());
        CHECK_FALSE(rig.driver->get_slewing());
    }
    SECTION("stopped during the outage: the relink's first status ends it") {
        goto_started(rig, "4.24s");
        rig.mount->set_status("nNpEW182#");
        lose_and_relink(rig);
        CHECK(contains(thrown_by([&] { (void)rig.driver->get_slewing(); }).second, "horizon limit"));
        CHECK(rig.mount->count(":ST60") == 0);
    }
    SECTION("the latch cleared during the outage, then a stop with the old code") {
        goto_started(rig, "4.24s", "NpEW187#");
        rig.mount->set_status("pEW180#");
        lose_and_relink(rig);
        show(rig, "nNpEW187#");
        CHECK(contains(thrown_by([&] { (void)rig.driver->get_slewing(); }).second, "meridian limit"));
    }
}

// Item 5 (#2296): a watched park fails when the mount ends it without parking (a handset abort, a park that gave up),
// once a status showed it running, and when it has not ended 20 minutes after the Park command (a time a GOTO waits
// at home, 'w', not counted). The bound only reports: nothing is sent to the mount.
TEST_CASE("OnStep park watch - a park ended without parking or never ended fails AtPark, a slow park parks",
          "[onstep][telescope][snapshot][limit-stop]") {
    Rig rig;
    connect_rates(rig);
    show(rig, "NpEW180#");
    rig.driver->park();
    show(rig, "nIEW180#");  // parking
    SECTION("ended without parking") {
        show(rig, "nNpEW180#");
        const auto [code, message] = thrown_by([&] { (void)rig.driver->get_at_park(); });
        CHECK(code == alpacacore::AlpacaError::DriverException);
        CHECK(contains(message, "Park failed: the mount ended the park without parking"));
    }
    SECTION("never ends: fails at the bound, not one step before") {
        rig.clock.advance(std::chrono::minutes(20) - milliseconds(1));
        CHECK_NOTHROW(rig.driver->get_at_park());
        rig.clock.advance(milliseconds(1));
        CHECK(contains(thrown_by([&] { (void)rig.driver->get_at_park(); }).second,
                       "did not finish the park within 20 minutes"));
        CHECK(rig.mount->count(":Q") == 0);
    }
    SECTION("a wait at home restarts the bound") {
        rig.clock.advance(std::chrono::minutes(20) - milliseconds(1));
        show(rig, "nIwEW180#");
        rig.clock.advance(milliseconds(1));
        CHECK_NOTHROW(rig.driver->get_at_park());
    }
    SECTION("a slow park that takes most of the bound parks") {
        rig.clock.advance(std::chrono::minutes(20) - seconds(1));
        show(rig, "nIEW180#");
        show(rig, "nNPEW180#");
        rig.clock.advance(seconds(5));
        CHECK(rig.driver->get_at_park());
        CHECK_FALSE(rig.driver->get_slewing());
    }
}

// Item 6 (#2296): a home that ran as a GOTO and ends away from home fails AtHome; one that never ends fails at the
// bound. A home with switches is left to the bound: after the switches OnStepX moves on to the sense offset with no
// sign in :GU# (Home.cpp guideDone()), and a home is not judged before a status shows it running.
TEST_CASE("OnStep home watch - a home ended away from home or never ended fails AtHome, a switch home reaches home",
          "[onstep][telescope][snapshot][limit-stop]") {
    Rig rig;
    connect_rates(rig);
    show(rig, "NpEW180#");
    rig.driver->find_home();
    SECTION("a GOTO home ended away from home") {
        show(rig, "nphEW180#");
        show(rig, "nNpEW180#");
        const auto [code, message] = thrown_by([&] { (void)rig.driver->get_at_home(); });
        CHECK(code == alpacacore::AlpacaError::DriverException);
        CHECK(contains(message, "FindHome failed: the mount ended the home without reaching home"));
    }
    SECTION("never ends: fails at the bound, not one step before") {
        show(rig, "nNphgEW180#");  // a home with switches
        rig.clock.advance(std::chrono::minutes(20) - milliseconds(1));
        CHECK_NOTHROW(rig.driver->get_at_home());
        rig.clock.advance(milliseconds(1));
        CHECK(contains(thrown_by([&] { (void)rig.driver->get_at_home(); }).second,
                       "did not finish the home within 20 minutes"));
    }
    SECTION("first frames show nothing, then switches, an invisible offset move, and home") {
        show(rig, "nNpEW180#");
        show(rig, "nNpEW180#");
        CHECK_NOTHROW(rig.driver->get_at_home());
        show(rig, "nNphgEW180#");
        show(rig, "nNpEW180#");  // the move to the sense offset
        show(rig, "nNpEW180#");
        CHECK_NOTHROW(rig.driver->get_at_home());
        show(rig, "nNpHEo180#");
        CHECK(rig.driver->get_at_home());
    }
    SECTION("a home whose first status after :hC# ends it with a stop code") {
        show(rig, "nNpEW183#");
        const auto [code, message] = thrown_by([&] { (void)rig.driver->get_at_home(); });
        CHECK(code == alpacacore::AlpacaError::DriverException);
        CHECK(contains(message, "limit switch"));
    }
    SECTION("a GOTO home reaches home") {
        show(rig, "nphEW180#");
        show(rig, "nNpHEo180#");
        CHECK(rig.driver->get_at_home());
    }
}

// Review round 2, item 1 (#2296): an Alt/Az slew runs with Tracking off, and both firmwares then hold the target's hour
// angle, so the RA the mount reports at the end has moved on by the sidereal time since the command. A 4.x code
// latched before it (:ST60.164275# does not clear it, only :Te# does) must not fail a slow Alt/Az slew that sits at the
// requested Alt/Az.
TEST_CASE("OnStep limit stop - a slow Alt/Az slew with a latched 4.x code lands at the requested Alt/Az",
          "[onstep][telescope][snapshot][limit-stop]") {
    Rig rig;
    connect_rates(rig, "4.24s");
    show(rig, "nNpEW182#");  // Tracking off, horizon code 2 latched
    rig.mount->clear_log();
    rig.driver->slew_to_alt_az_async(40.0, 0.0);
    double ra = 0.0;
    double dec = 0.0;
    for (const auto& command : rig.mount->commands()) {
        int h = 0;
        int m = 0;
        int sec = 0;
        char sign = '+';
        if (std::sscanf(command.c_str(), ":Sr%d:%d:%d#", &h, &m, &sec) == 3) ra = h + m / 60.0 + sec / 3600.0;
        if (std::sscanf(command.c_str(), ":Sd%c%d*%d:%d#", &sign, &h, &m, &sec) == 4) {
            dec = (sign == '-' ? -1.0 : 1.0) * (h + m / 60.0 + sec / 3600.0);
        }
    }
    REQUIRE(ra != 0.0);
    rig.clock.advance(std::chrono::minutes(6));  // a slow slew
    // At the requested Alt/Az: the RA has moved on by 6 sidereal minutes (1.5 degrees of RA).
    rig.mount->set_position(ra + 6.0 * 1.00273790935 / 60.0, dec);
    show(rig, "nNpEW182#");
    CHECK_NOTHROW(rig.driver->get_slewing());
    CHECK_FALSE(rig.driver->get_slewing());
}

// Review round 2, item 2 (#2296): OnStepX stops a GOTO at a limit through goTo.abort(), so its GOTO flag stays set
// while it brakes and tracking is already off. An AbortSlew whose :GU# reads that frame sends no tracking restore.
TEST_CASE("OnStep limit stop - an AbortSlew that reads a limit stop still braking sends no tracking restore",
          "[onstep][telescope][snapshot][limit-stop]") {
    Rig rig;
    goto_started(rig);
    rig.mount->set_status("npEW182#");  // GOTO still braking, not tracking, horizon code 2
    rig.driver->abort_slew();
    CHECK(rig.mount->count(":Te#") == 0);
    CHECK_NOTHROW(rig.driver->get_slewing());
}

// Review round 2, item 3 (#2296): a stop code at the end of a GOTO whose position cannot be read is a stop, never a
// landing: the synchronous slew throws instead of returning success, and sends no tracking restore.
TEST_CASE("OnStep limit stop - a synchronous slew whose stop code comes with an unreadable position fails",
          "[onstep][telescope][snapshot][limit-stop]") {
    Rig rig;
    connect_rates(rig);
    show(rig, "NpEW180#");
    std::atomic<int> stage{-1};  // -1: the :MS# has not gone out
    rig.mount->set_on_command([&](const std::string& chunk) {
        if (chunk.rfind(":MS", 0) == 0) {
            stage = 0;
        } else if (chunk.rfind(":GU", 0) == 0 && stage.load() == 0) {
            rig.mount->set_status("npEW180#");  // the GOTO runs
            stage = 1;
        } else if (chunk.rfind(":GU", 0) == 0 && stage.load() == 1) {
            rig.mount->set_status("nNpEW182#");  // it ends with a stop code
            rig.mount->garble_position = true;   // and the position read that follows fails, once
            stage = 2;
        } else if (stage.load() == 2 && chunk.rfind(":GRa", 0) != 0) {
            rig.mount->garble_position = false;
            stage = 3;
        }
    });
    const auto [code, message] = thrown_by([&] { synchronous_slew(rig, 2.0, 20.0); });
    rig.mount->set_on_command(nullptr);
    CHECK(code == alpacacore::AlpacaError::DriverException);
    CHECK(contains(message, "horizon limit"));
    CHECK(contains(message, "position could not be read"));
    CHECK(rig.mount->count(":Te#") == 0);
}

// Review round 2, item 4 (#2296): OnStepX derives the pier side from axis2 alone, so a flip stopped part-way can
// already show the requested side. Its end position must also be at the position it slews back to.
TEST_CASE("OnStep limit stop - a flip a limit stops part-way fails even when the new pier side shows",
          "[onstep][telescope][snapshot][limit-stop][pierside]") {
    Rig rig(false);
    set_pier(rig, "B");
    connect_paused(rig);
    place(rig, kThirdOfAnHour, kWestStatus);
    REQUIRE_NOTHROW(rig.driver->set_side_of_pier(0));  // the flip east
    SECTION("stopped part-way: east shown, far from the position") {
        rig.mount->point_at(rig.driver->get_sidereal_time(), 3.0, 30.0);
        show(rig, "nNpET187#");
        const auto [code, message] = thrown_by([&] { (void)rig.driver->get_slewing(); });
        CHECK(code == alpacacore::AlpacaError::DriverException);
        CHECK(contains(message, "SideOfPier"));
    }
    SECTION("landed: east shown, at the position") {
        show(rig, "NpET187#");  // the position is unchanged
        CHECK_NOTHROW(rig.driver->get_slewing());
    }
}

// Review round 2, item 6 (#2296): a relink that judges a carried-over GOTO on its seed frame, with no position in that
// frame, reads :GRa# itself; a link lost on that read fails the Connect as any lost seed read does, rather than
// escaping set_connected() as NotConnected.
TEST_CASE("OnStep limit stop - a relink whose end check loses the link fails the Connect",
          "[onstep][telescope][snapshot][limit-stop]") {
    PreserveWindow window(std::chrono::hours(1));
    Rig rig;
    goto_started(rig);
    rig.mount->set_status("nNpEW182#");  // stopped during the outage
    REQUIRE(rig.server.drop_connections());
    REQUIRE_FALSE(rig.driver->get_connected());
    std::atomic<int> position_reads{0};
    rig.mount->set_on_command([&](const std::string& chunk) {
        if (chunk.rfind(":GRa", 0) == 0 && position_reads.fetch_add(1) == 0) {
            rig.mount->garble_position = true;  // the seed's read: no position in the frame
            rig.server.hang_up_on(":GRa");      // the next one, the end check's, loses the link
        } else {
            rig.mount->garble_position = false;
        }
    });
    const auto [code, message] = thrown_by([&] { rig.driver->set_connected(true); });
    rig.mount->set_on_command(nullptr);
    INFO(message);
    CHECK(contains(message, "Failed to connect to OnStep mount: the link was lost during the connect"));
    CHECK_FALSE(rig.driver->get_connected());
}

// The kept stop is raised until the next motion command or a reconnect clears it (the shared rule "A failed async
// operation body keeps its error"), as a refusal the park_home_ body kept is.
TEST_CASE("OnStep limit stop - the next motion command or a reconnect clears a kept slew stop",
          "[onstep][telescope][snapshot][limit-stop]") {
    Rig rig;
    goto_started(rig);
    show(rig, "nNpEW182#");
    REQUIRE(thrown_by([&] { (void)rig.driver->get_slewing(); }).first == alpacacore::AlpacaError::DriverException);
    SECTION("a new GOTO") {
        show(rig, "NpEW180#");  // tracking again: an equatorial slew needs it (#1274)
        rig.driver->slew_to_coordinates_async(3.0, 30.0);
        CHECK(rig.driver->get_slewing());
    }
    SECTION("AbortSlew") {
        rig.driver->abort_slew();
        CHECK_FALSE(rig.driver->get_slewing());
    }
    SECTION("MoveAxis") {
        rig.driver->move_axis(0, 1.0);
        CHECK(rig.driver->get_slewing());
        rig.driver->move_axis(0, 0.0);
    }
    SECTION("Park") {
        rig.driver->park();
        CHECK(rig.driver->get_slewing());
    }
    SECTION("a reconnect") {
        rig.driver->set_connected(false);
        connect_paused(rig);
        CHECK_FALSE(rig.driver->get_slewing());
    }
}

namespace {

// Counts the OnStep WARN and INFO lines that contain `needle`, at an Info log level; restores the sink and the level
// when it goes (the level filters lines before the sink sees them).
class LevelCounter {
public:
    explicit LevelCounter(std::string needle) : needle_(std::move(needle)) {
        alpacacore::logging::set_log_level(alpacacore::logging::LogLevel::Info);
        alpacacore::logging::set_log_sink(
            [this](alpacacore::logging::LogLevel level, std::string_view component, std::string_view message) {
                if (component != "OnStep" || message.find(needle_) == std::string_view::npos) return;
                if (level == alpacacore::logging::LogLevel::Warn) ++warn_;
                if (level == alpacacore::logging::LogLevel::Info) ++info_;
            });
    }
    ~LevelCounter() {
        alpacacore::logging::set_log_sink(previous_sink_);
        alpacacore::logging::set_log_level(previous_level_);
    }
    LevelCounter(const LevelCounter&) = delete;
    LevelCounter& operator=(const LevelCounter&) = delete;

    int warn() const { return warn_.load(); }
    int info() const { return info_.load(); }

private:
    alpacacore::logging::LogSink previous_sink_ = alpacacore::logging::get_log_sink();
    alpacacore::logging::LogLevel previous_level_ = alpacacore::logging::get_log_level();
    std::string needle_;
    std::atomic<int> warn_{0};
    std::atomic<int> info_{0};
};

// The mount tracks again (a client's Tracking write) and the driver starts another GOTO to 2 h +20, past its grace.
void goto_again(Rig& rig, const char* before) {
    show(rig, before);
    rig.driver->slew_to_coordinates_async(2.0, 20.0);
    rig.clock.advance(seconds(3));
}

// The mount's base slew rate replies (:GX93# us/step, :GXE4# steps per degree), set before the connect reads them.
void base_rate(Rig& rig, const char* us_per_step, const char* steps_per_degree) {
    std::lock_guard<std::mutex> lock(rig.mount->mutex);
    rig.mount->gx93 = us_per_step;
    rig.mount->gxe4 = steps_per_degree;
}

}  // namespace

// #2296 leftover: a mount built with a tangent arm (OnStep 4.x, OnStepX) or a sector gear (OnStepX) centres that axis
// on FindHome, and neither firmware then reports 'H' (4.x MoveTo.ino sets atHome only without a tangent arm; OnStepX
// Home::request() and guideDone() leave no home state). So that home is not watched: AtHome stays false and no failure
// is kept. The flags are read once a session, at the first FindHome that sends a home; a reply that is neither '1' nor
// '0' leaves them unknown, so that home is watched and the next FindHome reads them again.
// A firmware whose version did not read at connect is asked :GXEG# too: only one identified as 4.x has no sector gear.
// Falsified by: finish_park_or_home_locked() watching every home again (dropping the home_moves_one_axis_ check),
// read_home_geometry_locked() caching a reply that is neither digit as false (the fourth section), or asking :GXEG#
// only when onstepx_ is set (the last), or get_bare_flag() comparing a '#'-framed reply with its '#' (the fifth).
TEST_CASE("OnStep home watch - a tangent-arm or sector-gear mount's FindHome ends without AtHome and is not failed",
          "[onstep][telescope][snapshot][limit-stop]") {
    Rig rig;
    SECTION("OnStep 4.x tangent arm, home as a GOTO") {
        {
            std::lock_guard<std::mutex> lock(rig.mount->mutex);
            rig.mount->gxef = "1";
        }
        connect_rates(rig, "4.24s");
        show(rig, "NpEW180#");
        rig.driver->find_home();
        show(rig, "nphEW180#");  // the home GOTO to the arm centre
        show(rig, "nNpEW180#");  // ended, with no 'H'
        CHECK_NOTHROW(rig.driver->get_at_home());
        CHECK_FALSE(rig.driver->get_at_home());
        CHECK_NOTHROW(rig.driver->get_slewing());
    }
    SECTION("OnStepX sector gear, home with switches") {
        {
            std::lock_guard<std::mutex> lock(rig.mount->mutex);
            rig.mount->gxeg = "1";
        }
        connect_rates(rig);
        show(rig, "NpEW180#");
        rig.driver->find_home();
        show(rig, "nNphgEW180#");
        show(rig, "nNpEW180#");
        rig.clock.advance(std::chrono::minutes(21));  // past the 20-minute bound
        CHECK_NOTHROW(rig.driver->get_at_home());
        CHECK_NOTHROW(rig.driver->get_slewing());
    }
    SECTION("a mount without either is still watched, and the flags are read once") {
        connect_rates(rig, "4.24s");
        show(rig, "NpEW180#");
        rig.driver->find_home();
        show(rig, "nphEW180#");
        show(rig, "nNpEW180#");
        CHECK(contains(thrown_by([&] { (void)rig.driver->get_at_home(); }).second,
                       "the mount ended the home without reaching home"));
        show(rig, "nNpEW180#");
        rig.driver->find_home();
        CHECK(rig.mount->count(":GXEF") == 1);
        CHECK(rig.mount->count(":GXEG") == 0);  // OnStep 4.x is not asked
    }
    SECTION("a flag reply that is neither digit is read again at the next FindHome") {
        {
            std::lock_guard<std::mutex> lock(rig.mount->mutex);
            rig.mount->gxef = "x";  // a garbled first reply from a tangent-arm mount
        }
        connect_rates(rig, "4.24s");
        show(rig, "NpEW180#");
        rig.driver->find_home();
        show(rig, "nphEW180#");
        show(rig, "nNpEW180#");  // unknown: watched, as before
        CHECK(contains(thrown_by([&] { (void)rig.driver->get_at_home(); }).second,
                       "the mount ended the home without reaching home"));
        {
            std::lock_guard<std::mutex> lock(rig.mount->mutex);
            rig.mount->gxef = "1";
        }
        rig.driver->find_home();
        CHECK(rig.mount->count(":GXEF") == 2);
        show(rig, "nphEW180#");
        show(rig, "nNpEW180#");  // ended, with no 'H'
        CHECK_NOTHROW(rig.driver->get_at_home());
        CHECK_FALSE(rig.driver->get_at_home());
    }
    SECTION("a flag answered with a '#' is read as the digit, and leaves nothing behind") {
        {
            std::lock_guard<std::mutex> lock(rig.mount->mutex);
            rig.mount->gxef = "1#";  // a build that frames the reply: the bare read stops at the '#' and drops it
        }
        connect_rates(rig, "4.24s");
        show(rig, "NpEW180#");
        rig.driver->find_home();
        show(rig, "nphEW180#");
        show(rig, "nNpEW180#");
        // Not watched (read as a tangent arm), and the home and status exchanges after the flag read are not shifted
        // by a stray '#': a leftover would end the next '#' reply at once and fail the home.
        CHECK(rig.mount->count(":GXEF") == 1);
        CHECK_NOTHROW(rig.driver->get_at_home());
        CHECK_FALSE(rig.driver->get_at_home());
        CHECK_NOTHROW(rig.driver->get_slewing());
    }
    SECTION("an OnStepX sector gear whose firmware version did not read is still asked") {
        {
            std::lock_guard<std::mutex> lock(rig.mount->mutex);
            rig.mount->gxeg = "1";
        }
        connect_rates(rig, "");  // :GVN# gives no version: onstepx_ stays false
        show(rig, "NpEW180#");
        rig.driver->find_home();
        CHECK(rig.mount->count(":GXEG") == 1);
        show(rig, "nNphgEW180#");
        show(rig, "nNpEW180#");
        rig.clock.advance(std::chrono::minutes(21));  // past the 20-minute bound
        CHECK_NOTHROW(rig.driver->get_at_home());
        CHECK_NOTHROW(rig.driver->get_slewing());
    }
}

// Decision 0007 rule 9 with #2296: OnStepX keeps the meridian code (7) while the mount stands past the limit and still
// accepts :MS# (Goto::validate() leaves the meridian out of isGotoError()), then stops each GOTO at once. A client that
// repeats the GOTO gets the failure every time, but the log has one WARN per reason and code, whichever member started
// it; the repeats are INFO, and a status with another code arms the WARN again.
// Falsified by: fail_watched_motion_locked() and the landed path logging every stop at WARN again (no
// log_motion_stop_locked()), log_motion_stop_locked() keyed on the code alone (the third section), keeping only the
// last key logged at WARN (the fourth), or keyed on the whole message, which names the member (the last).
TEST_CASE("OnStep limit stop - GOTOs repeated into a standing meridian limit log one WARN, the repeats at INFO",
          "[onstep][telescope][snapshot][limit-stop]") {
    Rig rig;
    SECTION("repeats at INFO, re-armed by another code") {
        goto_started(rig);
        LevelCounter counter("meridian limit");
        show(rig, "nNpEW187#");
        CHECK(thrown_by([&] { (void)rig.driver->get_slewing(); }).first == alpacacore::AlpacaError::DriverException);
        for (int i = 0; i < 3; ++i) {
            goto_again(rig, "NpEW187#");  // the limit still stands
            show(rig, "nNpEW187#");
            const auto [code, message] = thrown_by([&] { (void)rig.driver->get_slewing(); });
            CHECK(code == alpacacore::AlpacaError::DriverException);
            CHECK(contains(message, "meridian limit"));
        }
        CHECK(counter.warn() == 1);
        CHECK(counter.info() == 3);
        show(rig, "nNpEW180#");  // back inside the limit
        goto_again(rig, "NpEW180#");
        show(rig, "nNpEW187#");
        CHECK(counter.warn() == 2);
    }
    SECTION("landings at the target with a standing code") {
        goto_started(rig);
        rig.mount->set_position(2.0, 20.0);
        LevelCounter counter("reached its target");
        show(rig, "nNpEW182#");
        CHECK_NOTHROW(rig.driver->get_slewing());
        goto_again(rig, "NpEW182#");
        show(rig, "nNpEW182#");
        CHECK_NOTHROW(rig.driver->get_slewing());
        CHECK(counter.warn() == 1);
        CHECK(counter.info() == 1);
    }
    SECTION("a 4.x stop after a landing with the same latched code still warns") {
        goto_started(rig, "4.24s");
        rig.mount->set_position(2.0, 20.0);
        LevelCounter counter("stopped the GOTO");
        show(rig, "nNpEW187#");  // landed, with the code: WARN "reached its target"
        CHECK_NOTHROW(rig.driver->get_slewing());
        rig.mount->set_position(12.0, 45.0);
        goto_again(rig, "NpEW187#");
        show(rig, "nNpEW187#");  // stopped short, with the same code
        CHECK(thrown_by([&] { (void)rig.driver->get_slewing(); }).first == alpacacore::AlpacaError::DriverException);
        CHECK(counter.warn() == 1);
    }
    SECTION("a client alternating a landing GOTO and a stopped one gets one WARN for each") {
        goto_started(rig);
        LevelCounter counter("general error 7");
        for (int i = 0; i < 3; ++i) {
            rig.mount->set_position(2.0, 20.0);  // at the target: lands, with the code
            if (i > 0) {
                goto_again(rig, "NpEW187#");
            }
            show(rig, "nNpEW187#");
            CHECK_NOTHROW(rig.driver->get_slewing());
            rig.mount->set_position(12.0, 45.0);  // away from it: stopped short
            goto_again(rig, "NpEW187#");
            show(rig, "nNpEW187#");
            CHECK(thrown_by([&] { (void)rig.driver->get_slewing(); }).first ==
                  alpacacore::AlpacaError::DriverException);
        }
        CHECK(counter.warn() == 2);
        CHECK(counter.info() == 4);
    }
    SECTION("a client alternating SlewToCoordinatesAsync and SlewToTargetAsync gets one WARN") {
        goto_started(rig);
        LevelCounter counter("meridian limit");
        show(rig, "nNpEW187#");
        CHECK(thrown_by([&] { (void)rig.driver->get_slewing(); }).first == alpacacore::AlpacaError::DriverException);
        for (int i = 0; i < 2; ++i) {
            show(rig, "NpEW187#");
            rig.driver->set_target_right_ascension(2.0);
            rig.driver->set_target_declination(20.0);
            rig.driver->slew_to_target_async();
            rig.clock.advance(seconds(3));
            show(rig, "nNpEW187#");
            CHECK(contains(thrown_by([&] { (void)rig.driver->get_slewing(); }).second, "SlewToTargetAsync failed"));
            goto_again(rig, "NpEW187#");
            show(rig, "nNpEW187#");
            CHECK(
                contains(thrown_by([&] { (void)rig.driver->get_slewing(); }).second, "SlewToCoordinatesAsync failed"));
        }
        CHECK(counter.warn() == 1);
        CHECK(counter.info() == 4);
    }
}

// #2296 leftover: the park and home bound relies on the status poll reading :GU# for as long as the driver is
// connected (expire_watched_motion_locked()), so the end frame of a park is read before the bound passes. This runs
// the real poll on the task clock: it reads the parking and parked frames, and goes on reading while parked, so a park
// that finished is never failed at 20 minutes.
// Falsified by: run_status_poller()/poll_status_once() skipping the :GU# sample while a park is pending or watched, or
// while the frame shows parked.
TEST_CASE(
    "OnStep park watch - the status poll reads :GU# through a park and while parked, so a finished park "
    "outlives the bound",
    "[onstep][telescope][snapshot][limit-stop]") {
    Rig rig;
    rig.mount->set_status("NpEW180#");
    rig.connect();
    REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));  // the poll is parked
    rig.driver->park();
    REQUIRE(rig.mount->count(":hP#") == 1);  // sent from the call: the mount showed no motion to stop first
    const auto step = [&](std::chrono::milliseconds by) {
        rig.clock.advance(by);
        REQUIRE(rig.clock.wait_for_woken_settled(seconds(5)));
        REQUIRE(rig.clock.wait_for_waiters(1, seconds(2)));
    };
    rig.mount->set_status("nIEW180#");  // parking
    step(seconds(1));
    rig.mount->set_status("nNPEW180#");  // parked
    step(seconds(1));
    const auto reads = rig.mount->count(":GU");
    for (int i = 0; i < 21; ++i) {
        step(std::chrono::minutes(1));
    }
    CHECK(rig.mount->count(":GU") >= reads + 21);  // a read every tick while parked
    CHECK_NOTHROW(rig.driver->get_at_park());
    CHECK(rig.driver->get_at_park());
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK(rig.mount->count(":Q") == 0);
}

// #2296 leftover: a slow mount can take more than 20 minutes for a healthy park or home (OnStep 4.x searches for its
// home switches at a quarter of the base rate at the slowest preset). The bound is 2160 s divided by the base slew
// rate (:GX93# us/step, :GXE4# steps per degree, read at connect), never under 20 minutes and never over 24 hours
// (capped in double before the cast), and a watch keeps the bound
// it was taken with, so a relink that reads the rate again after judging the carried watch does not shorten it.
// Falsified by: expire_watched_motion_locked() comparing against the fixed 20-minute floor again (the first three
// sections), a session-level bound that the relink's reset_session_state_locked() puts back to the floor before the
// carried watch is judged (the third), seed_park_home_bound_locked() dropping the floor (the fourth), or
// dropping the std::min cap (the last), or kParkHomeTravelSeconds back at 1080 (the first: 72 minutes). A rate that
// does not read keeps 20 minutes: the default mount answers "0#" to both, as in "OnStep park watch - a park ended
// without parking or never ended fails AtPark, a slow park parks".
TEST_CASE("OnStep park watch - the park and home bound follows a slow mount's base slew rate, never under 20 minutes",
          "[onstep][telescope][snapshot][limit-stop]") {
    Rig rig;
    SECTION("a 0.25 deg/s base gives 144 minutes") {
        base_rate(rig, "1000.000", "4000");
        connect_rates(rig);
        show(rig, "NpEW180#");
        rig.driver->park();
        const auto parked_at = rig.clock.now();
        show(rig, "nIEW180#");
        rig.clock.advance(std::chrono::minutes(20) + seconds(1));
        CHECK_NOTHROW(rig.driver->get_at_park());
        rig.clock.advance(std::chrono::minutes(144) - (rig.clock.now() - parked_at) - milliseconds(1));
        CHECK_NOTHROW(rig.driver->get_at_park());
        rig.clock.advance(milliseconds(1));
        CHECK(contains(thrown_by([&] { (void)rig.driver->get_at_park(); }).second,
                       "did not finish the park within 144 minutes"));
        CHECK(rig.mount->count(":Q") == 0);
    }
    SECTION("a FindHome on the same mount") {
        base_rate(rig, "1000.000", "4000");
        connect_rates(rig);
        show(rig, "NpEW180#");
        rig.driver->find_home();
        show(rig, "nNphgEW180#");  // a home with switches
        rig.clock.advance(std::chrono::minutes(21));
        CHECK_NOTHROW(rig.driver->get_at_home());
    }
    SECTION("a park a relink carries over keeps its bound") {
        PreserveWindow window(milliseconds(0));  // the #1289 rule leaves a park running
        base_rate(rig, "1000.000", "4000");
        connect_rates(rig);
        show(rig, "NpEW180#");
        rig.driver->park();
        rig.mount->set_status("nIEW180#");
        rig.clock.advance(std::chrono::minutes(30));
        lose_and_relink(rig);
        REQUIRE(rig.mount->count(":Q") == 0);
        CHECK_NOTHROW(rig.driver->get_at_park());
        show(rig, "nNPEW180#");
        CHECK(rig.driver->get_at_park());
    }
    SECTION("a fast mount keeps the floor") {
        base_rate(rig, "100.000", "4000");  // 2.5 deg/s: 864 s, under the floor
        connect_rates(rig);
        show(rig, "NpEW180#");
        rig.driver->park();
        show(rig, "nIEW180#");
        rig.clock.advance(std::chrono::minutes(20) - milliseconds(1));
        CHECK_NOTHROW(rig.driver->get_at_park());
        rig.clock.advance(milliseconds(1));
        CHECK(contains(thrown_by([&] { (void)rig.driver->get_at_park(); }).second,
                       "did not finish the park within 20 minutes"));
    }
    SECTION("a very slow mount is capped at 24 hours") {
        base_rate(rig, "1000000.000", "4000");  // 0.00025 deg/s: 50 days of travel
        connect_rates(rig);
        show(rig, "NpEW180#");
        rig.driver->park();
        show(rig, "nIEW180#");
        rig.clock.advance(std::chrono::hours(24) - milliseconds(1));
        CHECK_NOTHROW(rig.driver->get_at_park());
        rig.clock.advance(milliseconds(1));
        CHECK(contains(thrown_by([&] { (void)rig.driver->get_at_park(); }).second,
                       "did not finish the park within 1440 minutes"));
    }
}

#endif  // !_WIN32
