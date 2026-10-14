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

// open-astro#880, SynScan: the driver reports the handset's own position after
// a GOTO, and a handset that lands ~29" off in RA (EQM-35 Pro, 1.96 s of RA)
// fails ConformU's +/-10" SlewToCoordinates check. The driver re-issues the
// GOTO from the handset's own readback until the landing is inside tolerance.
// The fake lands every GOTO a fixed RA offset away from what it was told, re-rolls that by one second of RA on
// every other GOTO when `reroll` is set (as the handset does: two GOTOs with an identical aim landed 194 counts
// apart), and executes the driver's timed guide-rate moves exactly. A variable-rate `P` slew REPLACES tracking on
// the handset (measured on the EQM-35 Pro, open-astro#1030): the rate is the RA motor's own speed, westward in the
// sidereal direction for code 6 north of the equator and code 7 south of it, so the fake integrates RA at
// (sidereal - westward speed) and a `T` with a tracking mode resumes sidereal speed at once.
//
// open-astro#1030: the handset advances its local sidereal time once a second, so its RA readback after a GOTO is
// a sawtooth, exact right after the tick and one second of RA (15") low just before the next. With `sawtooth` set
// the fake reports RA that way, from its own epoch, so a test can sample the driver at a chosen phase.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"  // settle_connected
#include "fake_mount_server.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kCountsMask = 0xFFFFFF;
// 1.96 s of RA (the offset measured on the EQM-35 Pro) in 24-bit counts: 1.96 / 86400 * 2^24.
constexpr uint32_t kRaOffsetCounts = 381;
// One second of RA in 24-bit counts: 2^24 / 86400.
constexpr double kCountsPerSecondOfRa = 194.18;
constexpr double kCountsPerArcsec = 1.0 / 0.0772;
constexpr double kSiderealArcsecPerSec = 15.0411;

struct LandingOffsetHandset {
    std::atomic<uint32_t> ra_raw{0x400000};
    std::atomic<uint32_t> dec_raw{0x100000};
    std::atomic<int> goto_count{0};
    std::atomic<Clock::rep> goto_started{0};
    std::atomic<Clock::rep> first_goto_at{0};
    std::atomic<Clock::rep> second_goto_at{0};
    std::atomic<bool> goto_stopped{false};
    // Each GOTO lands this many counts past the commanded RA; multiplied by the GOTO ordinal when `growing`.
    std::atomic<uint32_t> ra_offset_counts{kRaOffsetCounts};
    std::atomic<bool> growing{false};
    std::atomic<int> goto_ms{300};
    // When set, GOTO n (1-based, up to the array size) lands dec_offset_script[n-1] counts off the commanded Dec:
    // the random per-GOTO Dec scatter a SynScan handset shows on a real mount (#1027).
    std::atomic<bool> dec_scripted{false};
    std::array<std::atomic<int32_t>, 4> dec_offset_script{};
    // When set, GOTO n (1-based, up to the array size) lands ra_offset_script[n-1] further counts east: the ~49'
    // miss the EQM-35 Pro's handset makes on some targets (#1105). commanded_ra records each GOTO's aim.
    std::atomic<bool> ra_scripted{false};
    std::array<std::atomic<uint32_t>, 4> ra_offset_script{};
    std::array<std::atomic<uint32_t>, 4> commanded_ra{};
    std::array<std::atomic<uint32_t>, 4> commanded_dec{};
    // The once-a-second RA readback sawtooth (#1030), phased from `sawtooth_epoch`.
    std::atomic<bool> sawtooth{false};
    std::atomic<Clock::rep> sawtooth_epoch{0};
    // Every other GOTO lands one further second of RA east (#1030).
    std::atomic<bool> reroll{false};
    // A GOTO asked to move Dec by 16" or less lands this many counts off, alternating sign (backlash-shaped).
    std::atomic<int32_t> dec_short_move_scatter{0};
    // Timed moves: `P` variable-rate start records the axis velocity; the stop applies the displacement.
    std::atomic<bool> ignore_nudges{false};
    std::atomic<int> nudge_starts{0};
    std::atomic<int> nudge_count{0};  // moves ended, by a stop or (RA) a tracking command
    // The site's hemisphere as the `w` reply reports it; it decides which RA code runs westward.
    std::atomic<bool> southern{false};
    // The `m` reply: 50 is an EQ mount (GermanPolar); 128..159 and 160 are Alt-Az.
    std::atomic<int> model_id{50};
    // RA motor: westward speed in arcsec/s (sidereal while tracking, 0 stopped), integrated into ra_raw.
    std::mutex ra_motor_mutex;
    double ra_westward_arcsec_per_s = kSiderealArcsecPerSec;
    double ra_carry_counts = 0.0;
    bool ra_moving = false;
    Clock::time_point ra_motor_since = Clock::now();
    std::atomic<Clock::rep> last_nudge_stop_at{0};
    std::array<std::atomic<double>, 2> axis_counts_per_s{};
    std::array<std::atomic<Clock::rep>, 2> axis_move_started{};

    double sawtooth_phase() const {
        const auto epoch = Clock::time_point(Clock::duration(sawtooth_epoch.load()));
        const double since = std::chrono::duration<double>(Clock::now() - epoch).count();
        return since - std::floor(since);
    }

    // Brings ra_raw up to now under the current RA motor speed. Caller holds ra_motor_mutex.
    void advance_ra_locked() {
        const auto now = Clock::now();
        const double elapsed = std::chrono::duration<double>(now - ra_motor_since).count();
        ra_motor_since = now;
        if (ignore_nudges.load()) {
            return;
        }
        ra_carry_counts += (kSiderealArcsecPerSec - ra_westward_arcsec_per_s) * elapsed * kCountsPerArcsec;
        const auto whole = static_cast<int32_t>(std::trunc(ra_carry_counts));
        ra_carry_counts -= whole;
        ra_raw.store(static_cast<uint32_t>(static_cast<int32_t>(ra_raw.load()) + whole) & kCountsMask);
    }

    // Sets the RA motor speed; ends a running timed move when the new speed is a stop or tracking.
    void set_ra_motor(double westward_arcsec_per_s, bool starts_move) {
        std::lock_guard<std::mutex> lock(ra_motor_mutex);
        advance_ra_locked();
        ra_westward_arcsec_per_s = westward_arcsec_per_s;
        if (starts_move) {
            ra_moving = true;
            nudge_starts.fetch_add(1);
        } else if (ra_moving) {
            ra_moving = false;
            nudge_count.fetch_add(1);
            last_nudge_stop_at.store(Clock::now().time_since_epoch().count());
        }
    }

    double ra_motor_speed() {
        std::lock_guard<std::mutex> lock(ra_motor_mutex);
        return ra_westward_arcsec_per_s;
    }

    uint32_t ra_now() {
        std::lock_guard<std::mutex> lock(ra_motor_mutex);
        advance_ra_locked();
        return ra_raw.load();
    }

    bool goto_in_progress() const {
        if (goto_count.load() == 0) return false;
        const auto started = Clock::time_point(Clock::duration(goto_started.load()));
        return !goto_stopped.load() && Clock::now() - started < std::chrono::milliseconds(goto_ms.load());
    }
};

std::string hex24(uint32_t raw) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%06X00", raw & kCountsMask);
    return buf;
}

alpacacore::test::FakeMountServer::Responder responder(const std::shared_ptr<LandingOffsetHandset>& st) {
    return [st](const std::string& chunk) -> std::string {
        if (chunk.empty()) return "0#";
        switch (chunk[0]) {
            case 'K':
                return std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
            case 'e': {
                uint32_t ra = st->ra_now();
                if (st->sawtooth.load()) {
                    const auto low = static_cast<uint32_t>(std::lround(st->sawtooth_phase() * kCountsPerSecondOfRa));
                    ra = (ra + kCountsMask + 1 - low) & kCountsMask;
                }
                return hex24(ra) + "," + hex24(st->dec_raw.load()) + "#";
            }
            case 'r': {
                // "r" + 6 hex + "00" + "," + 6 hex + "00"
                if (chunk.size() < 18) return "";
                const uint32_t ra = static_cast<uint32_t>(std::strtoul(chunk.substr(1, 6).c_str(), nullptr, 16));
                const uint32_t dec = static_cast<uint32_t>(std::strtoul(chunk.substr(10, 6).c_str(), nullptr, 16));
                const int ordinal = st->goto_count.fetch_add(1) + 1;
                st->set_ra_motor(kSiderealArcsecPerSec, false);  // a GOTO lands tracking
                uint32_t offset = st->ra_offset_counts.load() * (st->growing.load() ? ordinal : 1U);
                if (st->reroll.load() && ordinal % 2 == 0) {
                    offset += static_cast<uint32_t>(std::lround(kCountsPerSecondOfRa));
                }
                if (ordinal <= static_cast<int>(st->commanded_ra.size())) {
                    st->commanded_ra[static_cast<std::size_t>(ordinal - 1)].store(ra);
                    st->commanded_dec[static_cast<std::size_t>(ordinal - 1)].store(dec);
                }
                if (st->ra_scripted.load() && ordinal <= static_cast<int>(st->ra_offset_script.size())) {
                    offset += st->ra_offset_script[static_cast<std::size_t>(ordinal - 1)].load();
                }
                const int32_t requested_dec_move = static_cast<int32_t>(dec) - static_cast<int32_t>(st->dec_raw.load());
                st->ra_raw.store((ra + offset) & kCountsMask);
                int32_t dec_offset = 0;
                if (st->dec_scripted.load() && ordinal <= static_cast<int>(st->dec_offset_script.size())) {
                    dec_offset = st->dec_offset_script[static_cast<std::size_t>(ordinal - 1)].load();
                }
                if (st->dec_short_move_scatter.load() != 0 && std::abs(requested_dec_move) <= 210) {
                    dec_offset +=
                        ordinal % 2 == 0 ? st->dec_short_move_scatter.load() : -st->dec_short_move_scatter.load();
                }
                st->dec_raw.store(static_cast<uint32_t>(static_cast<int32_t>(dec) + dec_offset) & kCountsMask);
                st->goto_started.store(Clock::now().time_since_epoch().count());
                if (ordinal == 1) st->first_goto_at.store(st->goto_started.load());
                if (ordinal == 2) st->second_goto_at.store(st->goto_started.load());
                st->goto_stopped.store(false);
                return "#";
            }
            case 'L':
                return st->goto_in_progress() ? "1#" : "0#";
            case 'M':
                st->goto_stopped.store(true);
                return "#";
            case 'P': {
                if (chunk.size() >= 8 && chunk[1] == 3 && chunk[2] == 16 && chunk[4] == 0 && chunk[5] == 0) {
                    st->goto_stopped.store(true);
                }
                if (chunk.size() >= 8 && chunk[1] == 2 && chunk[2] == 16 && chunk[4] == 0) {
                    st->set_ra_motor(0.0, false);  // fixed-rate RA stop
                }
                if (chunk.size() >= 8 && chunk[1] == 3 && chunk[2] == 16) {
                    const double rate_arcsec =
                        ((static_cast<unsigned char>(chunk[4]) << 8) | static_cast<unsigned char>(chunk[5])) / 4.0;
                    const int westward_code = st->southern.load() ? 7 : 6;
                    const double westward = chunk[3] == westward_code ? rate_arcsec : -rate_arcsec;
                    st->set_ra_motor(westward, rate_arcsec != 0.0);
                }
                if (chunk.size() >= 8 && chunk[1] == 3 && chunk[2] == 17) {
                    const std::size_t axis = 1;
                    const double rate_arcsec =
                        ((static_cast<unsigned char>(chunk[4]) << 8) | static_cast<unsigned char>(chunk[5])) / 4.0;
                    if (rate_arcsec == 0.0) {
                        const double counts_per_s = st->axis_counts_per_s[axis].exchange(0.0);
                        if (counts_per_s != 0.0) {
                            const auto started = Clock::time_point(Clock::duration(st->axis_move_started[axis].load()));
                            const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
                            const auto moved = static_cast<int32_t>(std::lround(counts_per_s * elapsed));
                            if (!st->ignore_nudges.load()) {
                                st->dec_raw.store(
                                    static_cast<uint32_t>(static_cast<int32_t>(st->dec_raw.load()) + moved) &
                                    kCountsMask);
                            }
                            st->nudge_count.fetch_add(1);
                            st->last_nudge_stop_at.store(Clock::now().time_since_epoch().count());
                        }
                    } else {
                        // Dec: code 6 raises Dec (no tracking on this axis, so the rate is the move).
                        st->axis_counts_per_s[axis].store((chunk[3] == 6 ? 1.0 : -1.0) * rate_arcsec *
                                                          kCountsPerArcsec);
                        st->axis_move_started[axis].store(Clock::now().time_since_epoch().count());
                        st->nudge_starts.fetch_add(1);
                    }
                }
                return "#";
            }
            case 'T':
                st->set_ra_motor(chunk.size() > 1 && chunk[1] != 0 ? kSiderealArcsecPerSec : 0.0, false);
                return "#";
            case 'm':
                return std::string(1, static_cast<char>(st->model_id.load())) + "#";
            case 't':  // tracking mode read: sidereal tracking on
                return std::string(1, '\x01') + "#";
            case 'w': {
                std::string location(8, '\0');
                if (st->southern.load()) {
                    location[0] = '\x29';  // 41 degrees
                    location[3] = '\x01';  // D: 1 = south
                }
                return location + "#";
            }
            case 'W':
                return "#";
            default:
                return "0#";
        }
    };
}

// A 15"/s guide rate (the driver's 1x sidereal ceiling) halves the timed landing moves in tests.
void use_fast_guide_rate(alpacacore::TelescopeDriver& driver) {
    driver.set_guide_rate(alpacacore::GuideRate{15.0 / 3600.0, 15.0 / 3600.0});
}

alpacacore::vendor::synscan::ConnectionInfo endpoint(int port) {
    alpacacore::vendor::synscan::ConnectionInfo info;
    info.type = alpacacore::vendor::synscan::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = 1000;
    return info;
}

double ra_error_arcsec(alpacacore::TelescopeDriver& driver, double target_hours) {
    return std::abs(driver.get_right_ascension() - target_hours) * 15.0 * 3600.0;
}

double dec_error_arcsec(alpacacore::TelescopeDriver& driver, double target_degrees) {
    return std::abs(driver.get_declination() - target_degrees) * 3600.0;
}

constexpr double kTargetRa = 5.5;
constexpr double kTargetDec = 20.0;
// ConformU's SlewToCoordinates tolerance.
constexpr double kConformUToleranceArcsec = 10.0;

// The EQM-35 Pro on its HC 06.03.00 handset (#1027): a GOTO lands RA +10.4" east (systematic) and Dec up to
// 10" off at random; a GOTO asked to move Dec by 16" or less lands worse (7.3" mean, 19.4" max on 2026-10-13),
// so a Dec residual is never chased with another GOTO.
void script_rig_dec_scatter(LandingOffsetHandset& st) {
    st.ra_offset_counts.store(135);  // 10.4"
    const std::array<int32_t, 4> dec_counts{124, 1, -139, -10};  // the first GOTO lands Dec 9.6" off
    st.dec_short_move_scatter.store(150);
    for (std::size_t i = 0; i < dec_counts.size(); ++i) {
        st.dec_offset_script[i].store(dec_counts[i]);
    }
    st.dec_scripted.store(true);
}

// Blocks until the fake's sawtooth is at `phase` (0..1) of its second, within 20 ms.
void wait_for_sawtooth_phase(const LandingOffsetHandset& st, double phase) {
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (std::abs(st.sawtooth_phase() - phase) > 0.02 && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// Counts the SynScan WARN that reports a landing still off after the last refinement pass. The sink may be
// called from the async slew thread, so the count is atomic.
struct LandingWarnCounter {
    std::atomic<int> warns{0};
    alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
    LandingWarnCounter() {
        alpacacore::logging::set_log_sink(
            [this](alpacacore::logging::LogLevel level, std::string_view component, std::string_view message) {
                if (level == alpacacore::logging::LogLevel::Warn && component == "SynScan" &&
                    message.find("refinement passes") != std::string_view::npos) {
                    warns.fetch_add(1);
                }
            });
    }
    ~LandingWarnCounter() { alpacacore::logging::set_log_sink(previous); }
    LandingWarnCounter(const LandingWarnCounter&) = delete;
    LandingWarnCounter& operator=(const LandingWarnCounter&) = delete;
};

}  // namespace

TEST_CASE(
    "SynScan GOTO landing - SlewToCoordinates finishes a GOTO the handset lands 29 arcsec off with a timed move (#880)",
    "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));

    CHECK(ra_error_arcsec(*driver, kTargetRa) < 3.0);
    CHECK(dec_error_arcsec(*driver, kTargetDec) < 3.0);
    CHECK(st->goto_count.load() == 1);  // never another GOTO: the handset re-rolls each landing by a second
    CHECK(st->nudge_count.load() >= 1);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - a Dec-only refinement pass leaves the RA aim alone (#1030)",
          "[synscan][telescope][goto-landing]") {
    // An Alt-Az mount finishes every landing round with a re-GOTO (no timed moves), so the round's aim decides where
    // RA ends up: RA inside the tolerance must keep its aim while Dec is corrected.
    auto st = std::make_shared<LandingOffsetHandset>();
    st->model_id.store(128);
    st->ra_offset_counts.store(20);       // 1.5" RA, inside the 3" tolerance
    st->dec_offset_script[0].store(124);  // a systematic 9.6" Dec offset, cancelled by the aim correction
    st->dec_offset_script[1].store(124);
    st->dec_scripted.store(true);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_alignment_mode() == alpacacore::AlignmentMode::AltAz);

    REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));

    CHECK(st->goto_count.load() == 2);
    CHECK(st->nudge_starts.load() == 0);
    // RA was inside its tolerance, so the second GOTO aims at the same RA as the first.
    CHECK(st->commanded_ra[1].load() == st->commanded_ra[0].load());
    CHECK(dec_error_arcsec(*driver, kTargetDec) < 3.0);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - the settle time follows the last landing move once (#880)",
          "[synscan][telescope][goto-landing]") {
    struct Timing {
        Clock::duration until_last_move;
        Clock::duration after_last_move;
    };
    // One SlewToCoordinates that needs a landing move, with the given settle time.
    auto run = [](int settle_seconds) {
        auto st = std::make_shared<LandingOffsetHandset>();
        alpacacore::test::FakeMountServer server(responder(st));
        REQUIRE(server.ok());
        auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
            0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        use_fast_guide_rate(*driver);
        driver->set_slew_settle_time(settle_seconds);
        REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));
        const auto returned = Clock::now();
        REQUIRE(st->goto_count.load() == 1);
        REQUIRE(st->nudge_count.load() >= 1);
        const auto first = Clock::time_point(Clock::duration(st->first_goto_at.load()));
        const auto last_move = Clock::time_point(Clock::duration(st->last_nudge_stop_at.load()));
        driver->set_connected(false);
        return Timing{last_move - first, returned - last_move};
    };
    constexpr int kSettleSeconds = 3;
    const Timing none = run(0);
    const Timing settled = run(kSettleSeconds);
    const auto slack = std::chrono::milliseconds(1500);

    // The landing moves go out before any settle wait: no settle inside the landing...
    CHECK(settled.until_last_move < none.until_last_move + slack);
    // ...and exactly one settle period follows the last move (none: dropped; twice: doubled).
    CHECK(settled.after_last_move >= none.after_last_move + std::chrono::seconds(kSettleSeconds) - slack);
    CHECK(settled.after_last_move < none.after_last_move + std::chrono::seconds(kSettleSeconds) + slack);
}

TEST_CASE("SynScan GOTO landing - SlewToTarget refines the landing too (#880)", "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->set_target_right_ascension(kTargetRa);
    driver->set_target_declination(kTargetDec);
    REQUIRE_NOTHROW(driver->slew_to_target());

    CHECK(ra_error_arcsec(*driver, kTargetRa) < kConformUToleranceArcsec);
    CHECK(st->goto_count.load() == 1);
    CHECK(st->nudge_count.load() >= 1);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - async slew keeps Slewing true until the refinement lands (#880)",
          "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
    const auto deadline = Clock::now() + std::chrono::seconds(40);
    while (driver->get_slewing() && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE_FALSE(driver->get_slewing());

    // Slewing fell only after the landing moves, so the landing is final.
    CHECK(st->goto_count.load() == 1);
    CHECK(st->nudge_count.load() >= 1);
    CHECK(ra_error_arcsec(*driver, kTargetRa) < kConformUToleranceArcsec);
    CHECK(dec_error_arcsec(*driver, kTargetDec) < kConformUToleranceArcsec);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - a landing that never converges is bounded and reported (#880)",
          "[synscan][telescope][goto-landing]") {
    LandingWarnCounter counter;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->ignore_nudges.store(true);  // the timed moves never take: the residual stays
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    // Reported as a WARN and the slew completes (#1027): the residual is the handset's, not a failed command.
    REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));
    CHECK(counter.warns.load() == 1);
    CHECK(st->goto_count.load() == 1);
    CHECK(st->nudge_count.load() == 3);  // one move per round, three rounds, no more
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - an async landing that never converges is bounded and reported (#880)",
          "[synscan][telescope][goto-landing]") {
    LandingWarnCounter counter;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->ignore_nudges.store(true);  // the timed moves never take: the residual stays
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
    const auto deadline = Clock::now() + std::chrono::seconds(60);
    bool slewing = true;
    while (slewing && Clock::now() < deadline) {
        // Reported as a WARN, not as a slew error on the next Slewing read (#1027).
        REQUIRE_NOTHROW(slewing = driver->get_slewing());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK_FALSE(slewing);
    CHECK(counter.warns.load() == 1);
    CHECK(st->goto_count.load() == 1);
    CHECK(st->nudge_count.load() == 3);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - Dec scatter of the GOTO is removed by a timed move, never another GOTO (#1027)",
          "[synscan][telescope][goto-landing]") {
    LandingWarnCounter counter;
    auto st = std::make_shared<LandingOffsetHandset>();
    script_rig_dec_scatter(*st);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));

    CHECK(st->goto_count.load() == 1);  // a short Dec re-GOTO would land anywhere within 16"
    CHECK(ra_error_arcsec(*driver, kTargetRa) < 3.0);
    CHECK(dec_error_arcsec(*driver, kTargetDec) < 3.0);
    CHECK(counter.warns.load() == 0);
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - async Dec scatter of the GOTO is removed by a timed move, never another GOTO (#1027)",
          "[synscan][telescope][goto-landing]") {
    LandingWarnCounter counter;
    auto st = std::make_shared<LandingOffsetHandset>();
    script_rig_dec_scatter(*st);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
    const auto deadline = Clock::now() + std::chrono::seconds(60);
    bool slewing = true;
    while (slewing && Clock::now() < deadline) {
        REQUIRE_NOTHROW(slewing = driver->get_slewing());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK_FALSE(slewing);
    CHECK(st->goto_count.load() == 1);
    CHECK(dec_error_arcsec(*driver, kTargetDec) < 3.0);
    CHECK(counter.warns.load() == 0);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - RightAscension is corrected for the handset's one-second readback sawtooth (#1030)",
          "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    st->ra_offset_counts.store(165);  // 12.7": the EQM-35 Pro's per-GOTO east landing offset, read at the tick
    st->sawtooth_epoch.store(Clock::now().time_since_epoch().count());
    st->sawtooth.store(true);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    use_fast_guide_rate(*driver);
    REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));
    CHECK(st->goto_count.load() == 1);

    // Each sample is a fresh handset read (the position cache lasts 2 s) at a chosen phase of the handset's second;
    // uncorrected, the phase-0.9 read is 13.5" low and the three spread by 11".
    for (const double phase : {0.15, 0.5, 0.9}) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        wait_for_sawtooth_phase(*st, phase);
        INFO("phase " << phase);
        CHECK(ra_error_arcsec(*driver, kTargetRa) < 3.0);
    }
    CHECK(dec_error_arcsec(*driver, kTargetDec) < kConformUToleranceArcsec);
    driver->set_connected(false);
}

// Reads RightAscension at phase 0.9 of the fake's second, on a fresh handset read (past the 2 s position cache).
double ra_error_at_phase_09(alpacacore::TelescopeDriver& driver, const LandingOffsetHandset& st) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    wait_for_sawtooth_phase(st, 0.9);
    return ra_error_arcsec(driver, kTargetRa);
}

TEST_CASE("SynScan GOTO landing - a tracking write, a time write and a reconnect drop the sawtooth anchor (#1030)",
          "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    st->ra_offset_counts.store(165);
    st->sawtooth_epoch.store(Clock::now().time_since_epoch().count());
    st->sawtooth.store(true);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);
    // Uncorrected, a phase-0.9 read is 0.9 s of RA (13.5") low; corrected it is within 3".
    constexpr double kRawPhase09Arcsec = 8.0;

    REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));
    CHECK(ra_error_at_phase_09(*driver, *st) < 3.0);
    driver->set_tracking(true);  // a T write, the same mode: the handset's regime may have changed
    CHECK(ra_error_at_phase_09(*driver, *st) > kRawPhase09Arcsec);

    REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));  // re-anchors
    CHECK(ra_error_at_phase_09(*driver, *st) < 3.0);
    driver->set_utc_date(std::chrono::system_clock::now());  // an H write resets the handset's clock
    CHECK(ra_error_at_phase_09(*driver, *st) > kRawPhase09Arcsec);

    REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));
    CHECK(ra_error_at_phase_09(*driver, *st) < 3.0);
    driver->set_connected(false);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    CHECK(ra_error_at_phase_09(*driver, *st) > kRawPhase09Arcsec);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - AbortSlew during a refinement pass stops it (#880)",
          "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->ra_offset_counts.store(150);  // 11.6", within reach of an 8 s move at 2"/s, which takes 4 s
    driver->set_guide_rate(alpacacore::GuideRate{2.0 / 3600.0, 2.0 / 3600.0});

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
    const auto deadline = Clock::now() + std::chrono::seconds(40);
    while (st->nudge_starts.load() < 1 && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(st->nudge_starts.load() == 1);  // the landing move is under way

    const auto abort_started = Clock::now();
    REQUIRE_NOTHROW(driver->abort_slew());
    CHECK(Clock::now() - abort_started < std::chrono::milliseconds(1500));  // not the move's remaining 4 s
    CHECK_FALSE(driver->get_slewing());
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    CHECK_FALSE(driver->get_slewing());
    CHECK(st->nudge_count.load() == 1);  // the move was stopped at once, not after its 4 s
    CHECK(st->goto_count.load() == 1);
    driver->set_connected(false);
}

TEST_CASE(
    "SynScan GOTO landing - a read on either side of a slightly misplaced tick is resolved by the last known RA "
    "(#1030)",
    "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    st->ra_offset_counts.store(165);
    st->sawtooth_epoch.store(Clock::now().time_since_epoch().count());
    st->sawtooth.store(true);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);
    REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));
    const auto epoch = st->sawtooth_epoch.load();

    // The handset's tick 150 ms later than the driver's anchor: a read 40 ms before the tick is at the bottom of
    // the ramp while the anchor says the second just began (12.8" low uncorrected). Then 150 ms earlier: a read
    // 40 ms after the tick is at the top while the anchor says the second is nearly over (12.8" high). Each is a
    // whole second off without the other side; the shift is well outside the anchor's own 25 ms uncertainty.
    // Once the side is right the residual is the shift itself (150 ms = 2.25") plus the anchor's and the phase
    // wait's own margins; the wrong side is 12-14" off.
    for (const auto& [shift_ms, phase] : {std::pair{150, 0.96}, std::pair{-150, 0.04}}) {
        st->sawtooth_epoch.store(epoch + Clock::duration(std::chrono::milliseconds(shift_ms)).count());
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        wait_for_sawtooth_phase(*st, phase);
        INFO("tick shifted " << shift_ms << " ms, read at phase " << phase);
        CHECK(ra_error_arcsec(*driver, kTargetRa) < 5.0);
    }
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - a landing the handset re-rolls by one second of RA still converges (#1030)",
          "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    st->ra_offset_counts.store(165);
    st->reroll.store(true);
    st->sawtooth_epoch.store(Clock::now().time_since_epoch().count());
    st->sawtooth.store(true);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    for (int slew = 0; slew < 2; ++slew) {  // the second GOTO is the re-rolled one
        REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));
        CHECK(ra_error_arcsec(*driver, kTargetRa) < 3.0);
        CHECK(dec_error_arcsec(*driver, kTargetDec) < 3.0);
    }
    CHECK(st->goto_count.load() == 2);  // one per slew
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - a residual beyond a timed move's reach gets one re-GOTO first (#1030)",
          "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    st->ra_offset_counts.store(1300);  // 100": beyond 8 s at the default 7.5"/s guide rate
    st->reroll.store(true);            // the re-GOTO lands a second east, so a move still finishes it
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));

    CHECK(st->goto_count.load() == 2);  // the GOTO and one re-GOTO, then timed moves
    CHECK(st->nudge_count.load() >= 1);
    CHECK(ra_error_arcsec(*driver, kTargetRa) < 3.0);
    driver->set_connected(false);
}

namespace {

// 38,100 counts (2,941"): the RA miss of the handset's first GOTO in the #1105 TRACE.
constexpr uint32_t kFarMissCounts = 38100;

// Waits for an async slew to end. Returns the DriverException message if a Slewing read throws, else "".
std::string wait_async_slew(alpacacore::TelescopeDriver& driver) {
    const auto deadline = Clock::now() + std::chrono::seconds(60);
    try {
        while (driver.get_slewing() && Clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
        return e.what();
    }
    return "";
}

}  // namespace

TEST_CASE("SynScan GOTO landing - a GOTO the handset ends 49 arcmin off is repeated once to the same target (#1105)",
          "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    st->ra_offset_script[0].store(kFarMissCounts);
    st->ra_scripted.store(true);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));

    CHECK(st->goto_count.load() == 2);
    // The repeat aims at the target itself: one aimed by the 49' residual would land 49' west.
    CHECK(st->commanded_ra[1].load() == st->commanded_ra[0].load());
    CHECK(ra_error_arcsec(*driver, kTargetRa) < 3.0);
    CHECK(dec_error_arcsec(*driver, kTargetDec) < 3.0);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - a GOTO that misses by 49 arcmin twice fails the slew (#1105)",
          "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    st->ra_offset_script[0].store(kFarMissCounts);
    st->ra_offset_script[1].store(kFarMissCounts);
    st->ra_scripted.store(true);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    bool threw = false;
    try {
        driver->slew_to_coordinates(kTargetRa, kTargetDec);
    } catch (const alpacacore::AlpacaException& e) {
        threw = true;
        CHECK(e.error_code() == alpacacore::AlpacaError::DriverException);
        CHECK(std::string(e.what()).find("arcsec") != std::string::npos);
    }
    CHECK(threw);
    CHECK(st->goto_count.load() == 2);  // one repeat, never a third GOTO
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - an async GOTO ending 49 arcmin off is repeated while Slewing stays true (#1105)",
          "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    st->ra_offset_script[0].store(kFarMissCounts);
    st->ra_scripted.store(true);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
    CHECK(wait_async_slew(*driver).empty());

    // Slewing fell only after the repeat GOTO and its landing moves.
    CHECK_FALSE(driver->get_slewing());
    CHECK(st->goto_count.load() == 2);
    CHECK(st->commanded_ra[1].load() == st->commanded_ra[0].load());
    CHECK(ra_error_arcsec(*driver, kTargetRa) < 3.0);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - an async GOTO that misses by 49 arcmin twice is reported through Slewing (#1105)",
          "[synscan][telescope][goto-landing]") {
    auto st = std::make_shared<LandingOffsetHandset>();
    st->ra_offset_script[0].store(kFarMissCounts);
    st->ra_offset_script[1].store(kFarMissCounts);
    st->ra_scripted.store(true);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
    const std::string error = wait_async_slew(*driver);

    CHECK(error.find("arcsec") != std::string::npos);
    CHECK(st->goto_count.load() == 2);
    driver->set_connected(false);
}

namespace {

// A GOTO that stays in progress for 6 s, started asynchronously. Every stop path below must return at once
// while the async slew task is still waiting on the handset (the Platform 7 Disconnect budget is 5 s).
constexpr auto kStopBudget = std::chrono::milliseconds(1500);

struct LongGoto {
    std::shared_ptr<LandingOffsetHandset> st = std::make_shared<LandingOffsetHandset>();
    std::unique_ptr<alpacacore::test::FakeMountServer> server;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;

    LongGoto() {
        st->goto_ms.store(6000);
        server = std::make_unique<alpacacore::test::FakeMountServer>(responder(st));
        REQUIRE(server->ok());
        driver = alpacacore::vendor::synscan::create_synscan_telescope(0, endpoint(server->port()),
                                                                       alpacacore::vendor::synscan::SynScanVersion::V4);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        REQUIRE(st->goto_count.load() == 1);
    }
};

}  // namespace

TEST_CASE("SynScan GOTO landing - Disconnect during an async GOTO returns at once (#880)",
          "[synscan][telescope][goto-landing]") {
    LongGoto g;
    const auto t0 = Clock::now();
    g.driver->set_connected(false);
    CHECK(Clock::now() - t0 < kStopBudget);
}

TEST_CASE("SynScan GOTO landing - MoveAxis during an async GOTO returns at once (#880)",
          "[synscan][telescope][goto-landing]") {
    LongGoto g;
    const auto t0 = Clock::now();
    try {
        g.driver->move_axis(0, 1.0);
    } catch (const alpacacore::AlpacaException&) {
        // the call may refuse the rate; only its latency is under test
    }
    CHECK(Clock::now() - t0 < kStopBudget);
    g.driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - a second async slew replaces a waiting one at once (#880)",
          "[synscan][telescope][goto-landing]") {
    LongGoto g;
    const auto t0 = Clock::now();
    REQUIRE_NOTHROW(g.driver->slew_to_coordinates_async(kTargetRa + 1.0, kTargetDec));
    CHECK(Clock::now() - t0 < kStopBudget);
    g.driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - destroying the driver during an async GOTO returns at once (#880)",
          "[synscan][telescope][goto-landing]") {
    LongGoto g;
    const auto t0 = Clock::now();
    g.driver.reset();
    CHECK(Clock::now() - t0 < kStopBudget);
}

// The handset's own RA after its motor speed is brought up to now, as a signed count difference from `from`.
int32_t ra_counts_since(LandingOffsetHandset& st, uint32_t from) {
    int32_t delta = static_cast<int32_t>((st.ra_now() - from) & kCountsMask);
    if (delta > static_cast<int32_t>(kCountsMask / 2)) delta -= static_cast<int32_t>(kCountsMask + 1);
    return delta;
}

// The true pointing error after a landing, from the fake's RA rather than the driver's report of it.
double true_ra_error_arcsec(LandingOffsetHandset& st, double target_hours) {
    const auto target = static_cast<uint32_t>(std::lround(target_hours / 24.0 * (kCountsMask + 1.0))) & kCountsMask;
    return std::abs(ra_counts_since(st, target)) / kCountsPerArcsec;
}

void wait_pulse_done(alpacacore::TelescopeDriver& driver) {
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (driver.get_is_pulse_guiding() && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

TEST_CASE("SynScan PulseGuide - a west RA pulse lowers RA and an east one raises it, in either hemisphere (#1030)",
          "[synscan][telescope][goto-landing]") {
    // On the handset a variable-rate P slew is the RA motor's own speed, not an offset from tracking (rig,
    // 2026-10-13: a 2 s west pulse at 7.5"/s moved RA 50.6" EAST, an east one 19.3" east). A pulse therefore runs
    // the motor westward at sidereal + guide rate (west) or sidereal - guide rate (east) and hands back to tracking.
    const bool southern = GENERATE(false, true);
    CAPTURE(southern);
    auto st = std::make_shared<LandingOffsetHandset>();
    st->southern.store(southern);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_tracking(true);
    driver->set_guide_rate(alpacacore::GuideRate{7.5 / 3600.0, 7.5 / 3600.0});
    const double expected_counts = 7.5 * kCountsPerArcsec;  // 1 s at 7.5"/s
    const double tolerance_counts = 1.5 * kCountsPerArcsec;

    uint32_t before = st->ra_now();
    driver->pulse_guide(3, 1000);  // West
    wait_pulse_done(*driver);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // tracking again: RA must stay put
    const int32_t west = ra_counts_since(*st, before);
    CHECK(std::abs(west + expected_counts) < tolerance_counts);

    before = st->ra_now();
    driver->pulse_guide(2, 1000);  // East
    wait_pulse_done(*driver);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const int32_t east = ra_counts_since(*st, before);
    CHECK(std::abs(east - expected_counts) < tolerance_counts);
    driver->set_connected(false);
}

TEST_CASE("SynScan MoveAxis - stopping the RA axis while tracking resumes tracking (#1030)",
          "[synscan][telescope][goto-landing]") {
    // A variable-rate P slew replaces tracking on the handset, so a plain stop leaves the RA motor stopped and RA
    // running away at the sidereal rate while Tracking still reads true. ASCOM resumes the tracking rate when
    // MoveAxis stops.
    auto st = std::make_shared<LandingOffsetHandset>();
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_tracking(true);

    driver->move_axis(0, 0.01);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    driver->move_axis(0, 0.0);
    const uint32_t stopped_at = st->ra_now();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    CHECK(std::abs(ra_counts_since(*st, stopped_at)) < 0.5 * kCountsPerArcsec);  // not 15" a second
    CHECK(driver->get_tracking());
    driver->set_connected(false);
}

TEST_CASE("SynScan MoveAxis - an RA jog after a landing drops the last known RA (#1030)",
          "[synscan][telescope][goto-landing]") {
    // The last known RA picks the side of the handset's tick for a read inside its uncertainty. A jog of 7.5" to
    // 22.5" leaves the true RA nearer the stale landing's one-second neighbour than the landing itself, so a stale
    // value turns every read for the rest of the anchor window one second of RA (15") wrong.
    auto st = std::make_shared<LandingOffsetHandset>();
    st->sawtooth.store(true);
    st->sawtooth_epoch.store(Clock::now().time_since_epoch().count());
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);
    REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));
    REQUIRE(true_ra_error_arcsec(*st, kTargetRa) < 3.0);

    // About 15" east: on the fake's motor model a westward speed of -14.4"/s raises RA at 29.4"/s.
    const double westward_code_rate = -14.4 / 3600.0;
    driver->move_axis(0, westward_code_rate);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    driver->move_axis(0, 0.0);
    const double jog_arcsec = true_ra_error_arcsec(*st, kTargetRa);
    REQUIRE(jog_arcsec > 8.0);
    REQUIRE(jog_arcsec < 22.0);

    const auto ra_counts = st->ra_now();
    const double true_ra_hours = ra_counts / (kCountsMask + 1.0) * 24.0;
    for (const double phase : {0.15, 0.5, 0.9}) {
        wait_for_sawtooth_phase(*st, phase);
        CAPTURE(phase);
        CHECK(ra_error_arcsec(*driver, true_ra_hours) < 3.0);
    }
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - a superseded synchronous landing move leaves the newer MoveAxis running (#1030)",
          "[synscan][telescope][goto-landing]") {
    // A synchronous slew's landing move sleeps with no cancel; a client that aborts and jogs meanwhile owns the
    // axis, so the move must not end it when it wakes.
    auto st = std::make_shared<LandingOffsetHandset>();
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_guide_rate(alpacacore::GuideRate{7.5 / 3600.0, 7.5 / 3600.0});  // the 29" RA residual: ~3.9 s

    std::thread slew([&] {
        try {
            driver->slew_to_coordinates(kTargetRa, kTargetDec);
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
            // superseded by the abort
        }
    });
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (st->nudge_starts.load() < 1 && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(st->nudge_starts.load() >= 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    driver->abort_slew();
    driver->move_axis(0, 0.5);
    slew.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(std::abs(st->ra_motor_speed() - 0.5 * 3600.0) < 1.0);  // the jog still runs
    driver->move_axis(0, 0.0);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - an Alt-Az mount finishes its landing with re-GOTOs, not axis moves (#1030)",
          "[synscan][telescope][goto-landing]") {
    // The landing residual is in RA/Dec; on an Alt-Az mount the motor axes are azimuth and altitude, so the timed
    // moves (and the sidereal base rate of the RA motor model) do not apply. The residual-aimed re-GOTO does.
    auto st = std::make_shared<LandingOffsetHandset>();
    st->model_id.store(128);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_alignment_mode() == alpacacore::AlignmentMode::AltAz);
    use_fast_guide_rate(*driver);

    REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));

    CHECK(st->nudge_starts.load() == 0);
    CHECK(st->goto_count.load() >= 2);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - the re-GOTO after a far-miss repeat is aimed by the repeat's residual alone (#1120)",
          "[synscan][telescope][goto-landing]") {
    // Alt-Az: every landing round is a re-GOTO. GOTO 1 lands 386" off in RA and 77" in Dec (past the RA tolerance, so
    // the carried RA aim moves too), so GOTO 2 is aimed by those residuals; GOTO 2 then misses 49' in RA, so GOTO 3
    // repeats the target; GOTO 3 lands 39" off in RA and Dec. GOTO 4 must be the target corrected by that 39" alone,
    // not by GOTO 1's miss as well, on both axes.
    auto st = std::make_shared<LandingOffsetHandset>();
    st->model_id.store(128);
    st->dec_offset_script[0].store(1000);
    st->dec_offset_script[2].store(500);
    st->dec_scripted.store(true);
    st->ra_offset_script[0].store(5000);
    st->ra_offset_script[1].store(kFarMissCounts);
    st->ra_offset_script[2].store(500);
    st->ra_scripted.store(true);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_alignment_mode() == alpacacore::AlignmentMode::AltAz);
    use_fast_guide_rate(*driver);

    REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));

    REQUIRE(st->goto_count.load() >= 4);
    const auto target_dec = static_cast<int32_t>(st->commanded_dec[0].load());
    CHECK(st->commanded_ra[2].load() == st->commanded_ra[0].load());  // the repeat aims at the target
    CHECK(static_cast<int32_t>(st->commanded_dec[2].load()) == target_dec);
    // GOTO 3 landed 500 counts east of the target in RA too, so GOTO 4 aims about 500 counts west of it. The RA
    // readback is quantised to the handset's one-second ticks, hence the margin; GOTO 1's 5000 counts, carried over,
    // would put the aim 5000 counts further west.
    CHECK(std::abs(static_cast<int32_t>(st->commanded_ra[3].load()) -
                   (static_cast<int32_t>(st->commanded_ra[0].load()) - 500)) <= 1500);
    // GOTO 3 landed 500 counts above the target, so GOTO 4 aims 500 counts below it (one count of rounding).
    CHECK(std::abs(static_cast<int32_t>(st->commanded_dec[3].load()) - (target_dec - 500)) <= 2);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - the RA landing move converges in either hemisphere (#1030)",
          "[synscan][telescope][goto-landing]") {
    // Rig, 2026-10-13: the RA landing moves ran the motor the wrong way and the landing ended 113" off after three
    // passes. Checked against the fake's own RA, not the driver's report of it.
    const bool southern = GENERATE(false, true);
    CAPTURE(southern);
    auto st = std::make_shared<LandingOffsetHandset>();
    st->southern.store(southern);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);
    LandingWarnCounter warns;

    REQUIRE_NOTHROW(driver->slew_to_coordinates(kTargetRa, kTargetDec));

    CHECK(true_ra_error_arcsec(*st, kTargetRa) < 3.0);
    CHECK(warns.warns.load() == 0);
    CHECK(st->goto_count.load() == 1);
    driver->set_connected(false);
}

#endif  // !_WIN32
