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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"  // settle_connected
#include "driver_rig.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"

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
    std::atomic<int> position_reads{0};  // precise `e` reads served
    std::atomic<int> status_reads{0};    // `L` slew-status reads served
    // The `e` reply with this 1-based ordinal (0: none) is held, still unanswered, until hold_e is cleared or 6 s of
    // real time pass; e_held is set while it waits.
    std::atomic<int> hold_e_at{0};
    // When set, the GOTO command arms hold_e_at for the second `e` after it: the slew-end measurement is the first,
    // the landing hunt's first read the second.
    std::atomic<bool> hold_e_after_goto{false};
    std::atomic<bool> hold_e{false};
    std::atomic<bool> e_held{false};
    std::atomic<Clock::rep> sawtooth_epoch{0};
    // Every other GOTO lands one further second of RA east (#1030).
    std::atomic<bool> reroll{false};
    // A GOTO asked to move Dec by 16" or less lands this many counts off, alternating sign (backlash-shaped).
    std::atomic<int32_t> dec_short_move_scatter{0};
    // Timed moves: `P` variable-rate start records the axis velocity; the stop applies the displacement.
    std::atomic<bool> ignore_nudges{false};
    std::atomic<int> nudge_starts{0};
    std::atomic<int> nudge_count{0};  // moves ended, by a stop or (RA) a tracking command
    // When set, a `T` halts tracking (tracking_mode 0) and gets no reply, so the write times out (#1722).
    std::atomic<bool> fail_t{false};
    // The site's hemisphere as the `w` reply reports it; it decides which RA code runs westward.
    std::atomic<bool> southern{false};
    // The `m` reply: 50 is an EQ mount (GermanPolar); 128..159 and 160 are Alt-Az.
    std::atomic<int> model_id{50};
    // The tracking mode the last `T` wrote, which `t` reports: 1 (on) until the first write.
    std::atomic<int> tracking_mode{1};
    // RA motor: westward speed in arcsec/s (sidereal while tracking, 0 stopped), integrated into ra_raw.
    std::mutex ra_motor_mutex;
    double ra_westward_arcsec_per_s = kSiderealArcsecPerSec;
    double ra_carry_counts = 0.0;
    bool ra_moving = false;
    Clock::time_point ra_motor_since = Clock::now();
    // The handset's motors, GOTO timer and sawtooth read this clock: the real one by default, a case's
    // FakeTaskClock (the same one the driver waits on) after attach().
    alpacacore::util::TaskClock* clock = &alpacacore::util::default_task_clock();
    Clock::time_point now() const { return clock->now(); }
    void attach(alpacacore::util::TaskClock& task_clock) {
        std::lock_guard<std::mutex> lock(ra_motor_mutex);
        clock = &task_clock;
        ra_motor_since = task_clock.now();
    }
    std::atomic<Clock::rep> last_nudge_stop_at{0};
    // The motion commands (P, M, T, r) in the order they reached the handset.
    std::mutex wire_mutex;
    std::vector<std::string> wire;
    void record_wire(const std::string& chunk) {
        std::lock_guard<std::mutex> lock(wire_mutex);
        wire.push_back(chunk);
    }
    std::vector<std::string> wire_log() {
        std::lock_guard<std::mutex> lock(wire_mutex);
        return wire;
    }
    std::array<std::atomic<double>, 2> axis_counts_per_s{};
    std::array<std::atomic<Clock::rep>, 2> axis_move_started{};

    double sawtooth_phase() const {
        const auto epoch = Clock::time_point(Clock::duration(sawtooth_epoch.load()));
        const double since = std::chrono::duration<double>(now() - epoch).count();
        return since - std::floor(since);
    }

    // Brings ra_raw up to now under the current RA motor speed. Caller holds ra_motor_mutex.
    void advance_ra_locked() {
        const auto current = now();
        const double elapsed = std::chrono::duration<double>(current - ra_motor_since).count();
        ra_motor_since = current;
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
            last_nudge_stop_at.store(now().time_since_epoch().count());
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
        return !goto_stopped.load() && now() - started < std::chrono::milliseconds(goto_ms.load());
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
        if (chunk[0] == 'P' || chunk[0] == 'M' || chunk[0] == 'T' || chunk[0] == 'r') {
            st->record_wire(chunk);
        }
        switch (chunk[0]) {
            case 'K':
                return std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
            case 'e': {
                const int ordinal = st->position_reads.fetch_add(1) + 1;
                if (ordinal == st->hold_e_at.load()) {
                    st->e_held.store(true);
                    for (int i = 0; i < 6000 && st->hold_e.load(); ++i) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    st->e_held.store(false);
                }
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
                if (st->hold_e_after_goto.load()) st->hold_e_at.store(st->position_reads.load() + 2);
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
                st->goto_started.store(st->now().time_since_epoch().count());
                if (ordinal == 1) st->first_goto_at.store(st->goto_started.load());
                if (ordinal == 2) st->second_goto_at.store(st->goto_started.load());
                st->goto_stopped.store(false);
                return "#";
            }
            case 'L':
                st->status_reads.fetch_add(1);
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
                            const double elapsed = std::chrono::duration<double>(st->now() - started).count();
                            const auto moved = static_cast<int32_t>(std::lround(counts_per_s * elapsed));
                            if (!st->ignore_nudges.load()) {
                                st->dec_raw.store(
                                    static_cast<uint32_t>(static_cast<int32_t>(st->dec_raw.load()) + moved) &
                                    kCountsMask);
                            }
                            st->nudge_count.fetch_add(1);
                            st->last_nudge_stop_at.store(st->now().time_since_epoch().count());
                        }
                    } else {
                        // Dec: code 6 raises Dec (no tracking on this axis, so the rate is the move).
                        st->axis_counts_per_s[axis].store((chunk[3] == 6 ? 1.0 : -1.0) * rate_arcsec *
                                                          kCountsPerArcsec);
                        st->axis_move_started[axis].store(st->now().time_since_epoch().count());
                        st->nudge_starts.fetch_add(1);
                    }
                }
                return "#";
            }
            case 'T':
                if (st->fail_t.load()) {
                    st->tracking_mode.store(0);
                    return "";
                }
                st->tracking_mode.store(chunk.size() > 1 ? static_cast<unsigned char>(chunk[1]) : 0);
                st->set_ra_motor(chunk.size() > 1 && chunk[1] != 0 ? kSiderealArcsecPerSec : 0.0, false);
                return "#";
            case 'm':
                return std::string(1, static_cast<char>(st->model_id.load())) + "#";
            case 't':  // tracking mode read
                return std::string(1, static_cast<char>(st->tracking_mode.load())) + "#";
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

alpacacore::vendor::synscan::ConnectionInfo endpoint(int port, int response_timeout_ms = 1000) {
    alpacacore::vendor::synscan::ConnectionInfo info;
    info.type = alpacacore::vendor::synscan::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = response_timeout_ms;
    return info;
}

// Virtual time. The driver's waits (landing moves, settle, hunt ticks, the async tasks) and the fake handset's
// motors and sawtooth share one FakeTaskClock per case; a test advances it, it never sleeps.
constexpr auto kVirtualStep = std::chrono::milliseconds(10);
// A positive wait ends on the real clock: any parked task satisfies step_virtual, so virtual time can run out in
// milliseconds while the driver is still on the socket.
constexpr auto kRealLimit = std::chrono::seconds(60);

// One step: once the driver is parked on the clock, advance it by kVirtualStep and wait for the woken task to
// settle, so no step lands between two of its waits. Returns false when nothing was parked within 20 ms of real
// time (the driver is on the socket or the op is done).
bool step_virtual(alpacacore::test::FakeTaskClock& clock) {
    if (!clock.wait_for_waiters(1, std::chrono::milliseconds(20))) {
        return false;
    }
    clock.advance(kVirtualStep);
    clock.wait_for_woken_settled(std::chrono::seconds(2));
    return true;
}

// Moves virtual time by `duration` with nothing in flight (an idle gap between two reads).
void advance_idle(alpacacore::test::FakeTaskClock& clock, std::chrono::milliseconds duration) {
    clock.advance(duration);
    clock.wait_for_woken_settled(std::chrono::seconds(2));
}

// Runs a blocking driver call on its own thread and steps virtual time until it returns, then rethrows what it
// threw. Bounded by 120 s of real time.
template <typename Fn>
void run_virtual(alpacacore::test::FakeTaskClock& clock, Fn&& fn) {
    std::exception_ptr error;
    std::atomic<bool> done{false};
    std::thread caller([&] {
        try {
            fn();
        } catch (...) {
            error = std::current_exception();
        }
        done.store(true);
    });
    const auto real_deadline = Clock::now() + std::chrono::seconds(120);
    while (!done.load() && Clock::now() < real_deadline) {
        step_virtual(clock);
    }
    while (!done.load()) {
        clock.advance(std::chrono::hours(1));  // release a hung caller so the thread can join
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    caller.join();
    if (error) std::rethrow_exception(error);
}

// Steps `done` out of a parked thread: virtual time jumps ahead until the flag is set.
void release_until(alpacacore::test::FakeTaskClock& clock, const std::atomic<bool>& done) {
    while (!done.load()) {
        clock.advance(std::chrono::hours(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// Joins `thread` when the scope ends, after `release` has let it finish, so a REQUIRE that throws while the thread
// is still joinable reports as a failure instead of std::terminate. A thread already joined is left alone.
struct ScopedJoin {
    std::thread& thread;
    std::function<void()> release;
    ~ScopedJoin() {
        if (!thread.joinable()) return;
        release();
        thread.join();
    }
};

std::unique_ptr<alpacacore::TelescopeDriver> make_driver(int port, alpacacore::util::TaskClock& clock,
                                                         int response_timeout_ms = 1000) {
    return alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(port, response_timeout_ms), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt,
        std::nullopt, std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
}

// The getters serve the polled frame (decision 0010), so a case that reads the RA at a chosen sawtooth phase takes
// one frame at that phase first.
double ra_error_arcsec(alpacacore::TelescopeDriver& driver, double target_hours) {
    alpacacore::vendor::synscan::poll_status_now_for_testing(driver);
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

// Advances virtual time until the fake's sawtooth is at `phase` (0..1) of its second, to the millisecond.
void advance_to_sawtooth_phase(alpacacore::test::FakeTaskClock& clock, const LandingOffsetHandset& st, double phase) {
    double wait = phase - st.sawtooth_phase();
    if (wait < 0.0) wait += 1.0;
    advance_idle(clock, std::chrono::milliseconds(std::lround(wait * 1000.0)));
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
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    REQUIRE_NOTHROW(run_virtual(clock, [&] { driver->slew_to_coordinates(kTargetRa, kTargetDec); }));

    CHECK(ra_error_arcsec(*driver, kTargetRa) < 3.0);
    CHECK(dec_error_arcsec(*driver, kTargetDec) < 3.0);
    CHECK(st->goto_count.load() == 1);  // never another GOTO: the handset re-rolls each landing by a second
    CHECK(st->nudge_count.load() >= 1);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - a Dec-only refinement pass leaves the RA aim alone (#1030)",
          "[synscan][telescope][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    // An Alt-Az mount finishes every landing round with a re-GOTO (no timed moves), so the round's aim decides where
    // RA ends up: RA inside the tolerance must keep its aim while Dec is corrected.
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    st->model_id.store(128);
    st->ra_offset_counts.store(20);       // 1.5" RA, inside the 3" tolerance
    st->dec_offset_script[0].store(124);  // a systematic 9.6" Dec offset, cancelled by the aim correction
    st->dec_offset_script[1].store(124);
    st->dec_scripted.store(true);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_alignment_mode() == alpacacore::AlignmentMode::AltAz);

    REQUIRE_NOTHROW(run_virtual(clock, [&] { driver->slew_to_coordinates(kTargetRa, kTargetDec); }));

    CHECK(st->goto_count.load() == 2);
    CHECK(st->nudge_starts.load() == 0);
    // RA was inside its tolerance, so the second GOTO aims at the same RA as the first.
    CHECK(st->commanded_ra[1].load() == st->commanded_ra[0].load());
    CHECK(dec_error_arcsec(*driver, kTargetDec) < 3.0);
    driver->set_connected(false);
}

// The failed `T` write leaves the handset at mode 0, and the poll publishes that mode in its frame, so Tracking reads
// false after the landing move. Falsified by: synscan_telescope_driver.cpp, drop the `frame.tracking_mode = ...` copy
// in the poll frame; Tracking then stays unknown and the case fails at the setup read, before the landing move.
TEST_CASE("SynScan GOTO landing - Tracking reads the halted mode after a failed RA landing restore (#1722)",
          "[synscan][telescope][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);
    REQUIRE(driver->get_tracking());
    st->fail_t.store(true);

    CHECK_THROWS_AS(run_virtual(clock, [&] { driver->slew_to_coordinates(kTargetRa, kTargetDec); }),
                    alpacacore::AlpacaException);
    CHECK_FALSE(driver->get_tracking());
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - the settle time follows the last landing move once (#880)",
          "[synscan][telescope][goto-landing]") {
    struct Timing {
        Clock::duration until_last_move;
        Clock::duration after_last_move;
    };
    // One SlewToCoordinates that needs a landing move, with the given settle time, timed on the virtual clock.
    auto run = [](int settle_seconds) {
        alpacacore::test::FakeTaskClock clock;
        auto st = std::make_shared<LandingOffsetHandset>();
        st->attach(clock);
        alpacacore::test::FakeMountServer server(responder(st));
        REQUIRE(server.ok());
        auto driver = make_driver(server.port(), clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        use_fast_guide_rate(*driver);
        driver->set_slew_settle_time(settle_seconds);
        REQUIRE_NOTHROW(run_virtual(clock, [&] { driver->slew_to_coordinates(kTargetRa, kTargetDec); }));
        const auto returned = clock.now();
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
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->set_target_right_ascension(kTargetRa);
    driver->set_target_declination(kTargetDec);
    REQUIRE_NOTHROW(run_virtual(clock, [&] { driver->slew_to_target(); }));

    CHECK(ra_error_arcsec(*driver, kTargetRa) < kConformUToleranceArcsec);
    CHECK(st->goto_count.load() == 1);
    CHECK(st->nudge_count.load() >= 1);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - async slew keeps Slewing true until the refinement lands (#880)",
          "[synscan][telescope][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
    const auto deadline = Clock::now() + kRealLimit;
    while (driver->get_slewing() && Clock::now() < deadline) {
        step_virtual(clock);
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
    alpacacore::test::FakeTaskClock clock;
    LandingWarnCounter counter;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    st->ignore_nudges.store(true);  // the timed moves never take: the residual stays
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    // Reported as a WARN and the slew completes (#1027): the residual is the handset's, not a failed command.
    REQUIRE_NOTHROW(run_virtual(clock, [&] { driver->slew_to_coordinates(kTargetRa, kTargetDec); }));
    CHECK(counter.warns.load() == 1);
    CHECK(st->goto_count.load() == 1);
    CHECK(st->nudge_count.load() == 3);  // one move per round, three rounds, no more
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - an async landing that never converges is bounded and reported (#880)",
          "[synscan][telescope][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    LandingWarnCounter counter;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    st->ignore_nudges.store(true);  // the timed moves never take: the residual stays
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
    const auto deadline = Clock::now() + kRealLimit;
    bool slewing = true;
    while (slewing && Clock::now() < deadline) {
        // Reported as a WARN, not as a slew error on the next Slewing read (#1027).
        REQUIRE_NOTHROW(slewing = driver->get_slewing());
        step_virtual(clock);
    }
    CHECK_FALSE(slewing);
    CHECK(counter.warns.load() == 1);
    CHECK(st->goto_count.load() == 1);
    CHECK(st->nudge_count.load() == 3);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - Dec scatter of the GOTO is removed by a timed move, never another GOTO (#1027)",
          "[synscan][telescope][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    LandingWarnCounter counter;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    script_rig_dec_scatter(*st);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    REQUIRE_NOTHROW(run_virtual(clock, [&] { driver->slew_to_coordinates(kTargetRa, kTargetDec); }));

    CHECK(st->goto_count.load() == 1);  // a short Dec re-GOTO would land anywhere within 16"
    CHECK(ra_error_arcsec(*driver, kTargetRa) < 3.0);
    CHECK(dec_error_arcsec(*driver, kTargetDec) < 3.0);
    CHECK(counter.warns.load() == 0);
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - async Dec scatter of the GOTO is removed by a timed move, never another GOTO (#1027)",
          "[synscan][telescope][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    LandingWarnCounter counter;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    script_rig_dec_scatter(*st);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
    const auto deadline = Clock::now() + kRealLimit;
    bool slewing = true;
    while (slewing && Clock::now() < deadline) {
        REQUIRE_NOTHROW(slewing = driver->get_slewing());
        step_virtual(clock);
    }
    CHECK_FALSE(slewing);
    CHECK(st->goto_count.load() == 1);
    CHECK(dec_error_arcsec(*driver, kTargetDec) < 3.0);
    CHECK(counter.warns.load() == 0);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - RightAscension is corrected for the handset's one-second readback sawtooth (#1030)",
          "[synscan][telescope][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    st->ra_offset_counts.store(165);  // 12.7": the EQM-35 Pro's per-GOTO east landing offset, read at the tick
    st->sawtooth_epoch.store(st->now().time_since_epoch().count());
    st->sawtooth.store(true);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    use_fast_guide_rate(*driver);
    REQUIRE_NOTHROW(run_virtual(clock, [&] { driver->slew_to_coordinates(kTargetRa, kTargetDec); }));
    CHECK(st->goto_count.load() == 1);

    // Each sample is a fresh handset read (the position cache lasts 2 s) at a chosen phase of the handset's second;
    // uncorrected, the phase-0.9 read is 13.5" low and the three spread by 11".
    for (const double phase : {0.15, 0.5, 0.9}) {
        advance_idle(clock, std::chrono::milliseconds(2100));
        advance_to_sawtooth_phase(clock, *st, phase);
        INFO("phase " << phase);
        CHECK(ra_error_arcsec(*driver, kTargetRa) < 3.0);
    }
    CHECK(dec_error_arcsec(*driver, kTargetDec) < kConformUToleranceArcsec);
    driver->set_connected(false);
}

// Reads RightAscension at phase 0.9 of the fake's second, on a fresh handset read (past the 2 s position cache).
double ra_error_at_phase_09(alpacacore::test::FakeTaskClock& clock, alpacacore::TelescopeDriver& driver,
                            const LandingOffsetHandset& st) {
    advance_idle(clock, std::chrono::milliseconds(2100));
    advance_to_sawtooth_phase(clock, st, 0.9);
    return ra_error_arcsec(driver, kTargetRa);
}

TEST_CASE("SynScan GOTO landing - a tracking write, a time write and a reconnect drop the sawtooth anchor (#1030)",
          "[synscan][telescope][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    st->ra_offset_counts.store(165);
    st->sawtooth_epoch.store(st->now().time_since_epoch().count());
    st->sawtooth.store(true);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);
    // Uncorrected, a phase-0.9 read is 0.9 s of RA (13.5") low; corrected it is within 3".
    constexpr double kRawPhase09Arcsec = 8.0;

    REQUIRE_NOTHROW(run_virtual(clock, [&] { driver->slew_to_coordinates(kTargetRa, kTargetDec); }));
    CHECK(ra_error_at_phase_09(clock, *driver, *st) < 3.0);
    driver->set_tracking(true);  // a T write, the same mode: the handset's regime may have changed
    CHECK(ra_error_at_phase_09(clock, *driver, *st) > kRawPhase09Arcsec);

    REQUIRE_NOTHROW(run_virtual(clock, [&] { driver->slew_to_coordinates(kTargetRa, kTargetDec); }));  // re-anchors
    CHECK(ra_error_at_phase_09(clock, *driver, *st) < 3.0);
    driver->set_utc_date(std::chrono::system_clock::now());  // an H write resets the handset's clock
    CHECK(ra_error_at_phase_09(clock, *driver, *st) > kRawPhase09Arcsec);

    REQUIRE_NOTHROW(run_virtual(clock, [&] { driver->slew_to_coordinates(kTargetRa, kTargetDec); }));
    CHECK(ra_error_at_phase_09(clock, *driver, *st) < 3.0);
    driver->set_connected(false);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    CHECK(ra_error_at_phase_09(clock, *driver, *st) > kRawPhase09Arcsec);
    driver->set_connected(false);
}

// A handset without the sawtooth: the first landing spends the hunt's 1.3 s first-jump budget. On the fake clock a
// handset reply costs no virtual time and only the 50 ms hunt interval advances it, so that is about 27 reads; a
// later landing on the same connection reads once (#1107). Callers check >= 4 reads for a hunt and < 4 for a skipped
// one, which holds with wide margin on both sides. Returns the reads a landing took.
int landing_reads(alpacacore::test::FakeTaskClock& clock, alpacacore::TelescopeDriver& driver,
                  LandingOffsetHandset& st) {
    const int before = st.position_reads.load();
    REQUIRE_NOTHROW(run_virtual(clock, [&] { driver.slew_to_coordinates(kTargetRa, kTargetDec); }));
    return st.position_reads.load() - before;
}

TEST_CASE("SynScan GOTO landing - a handset without the sawtooth is hunted once per connection (#1107)",
          "[synscan][telescope][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    st->ra_offset_counts.store(0);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);
    // landing_reads() counts the hunt's reads: the status poll (one `e` per virtual second, through the reconnect
    // below too) would add about a dozen to a skipped landing.
    alpacacore::vendor::synscan::set_status_poll_paused_for_testing(*driver, true);
    constexpr int kHuntReads = 4;

    CHECK(landing_reads(clock, *driver, *st) >= kHuntReads);
    CHECK(landing_reads(clock, *driver, *st) < kHuntReads);  // skipped
    CHECK(landing_reads(clock, *driver, *st) < kHuntReads);

    driver->set_tracking(true);  // a tracking write drops the flag
    CHECK(landing_reads(clock, *driver, *st) >= kHuntReads);
    CHECK(landing_reads(clock, *driver, *st) < kHuntReads);

    driver->set_utc_date(std::chrono::system_clock::now());  // so does a time write
    CHECK(landing_reads(clock, *driver, *st) >= kHuntReads);
    CHECK(landing_reads(clock, *driver, *st) < kHuntReads);

    driver->set_connected(false);  // and a reconnect
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);
    CHECK(landing_reads(clock, *driver, *st) >= kHuntReads);
    CHECK(landing_reads(clock, *driver, *st) < kHuntReads);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - AbortSlew during a refinement pass stops it (#880)",
          "[synscan][telescope][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->ra_offset_counts.store(150);  // 11.6", within reach of an 8 s move at 2"/s, which takes 4 s
    driver->set_guide_rate(alpacacore::GuideRate{2.0 / 3600.0, 2.0 / 3600.0});

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
    const auto deadline = Clock::now() + kRealLimit;
    while (st->nudge_starts.load() < 1 && Clock::now() < deadline) {
        step_virtual(clock);
    }
    REQUIRE(st->nudge_starts.load() == 1);  // the landing move is under way

    // The abort must return while virtual time stands still: the move's remaining 4 s of it never comes, so an abort
    // that waited it out would hang here (and is released after the check below).
    const auto abort_started = clock.now();
    std::promise<void> abort_done;
    auto aborted = abort_done.get_future();
    std::thread aborter([&] {
        try {
            driver->abort_slew();
            abort_done.set_value();
        } catch (...) {
            abort_done.set_exception(std::current_exception());
        }
    });
    const bool returned = aborted.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    CHECK(returned);
    CHECK(clock.now() - abort_started < std::chrono::milliseconds(1500));  // not the move's remaining 4 s
    while (aborted.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready) {
        clock.advance(std::chrono::seconds(10));  // release the abort (it may park again) so the thread can join
    }
    aborter.join();
    CHECK_NOTHROW(aborted.get());
    CHECK_FALSE(driver->get_slewing());
    advance_idle(clock, std::chrono::milliseconds(600));
    CHECK_FALSE(driver->get_slewing());
    CHECK(st->nudge_count.load() == 1);  // the move was stopped at once, not after its 4 s
    CHECK(st->goto_count.load() == 1);
    driver->set_connected(false);
}

// A synchronous slew's landing nudge waits on the task clock, so a test drives it in virtual time. The caller
// runs on its own thread; the test parks it inside the nudge, aborts, and reads the clock, not the wall.
TEST_CASE(
    "SynScan GOTO landing - AbortSlew during a synchronous slew's landing nudge returns the caller at once (#1108)",
    "[synscan][telescope][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);  // the handset's GOTO timer runs on the same virtual clock the driver waits on
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, alpacacore::vendor::synscan::SynScanAlignmentSetting::Auto, clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    st->ra_offset_counts.store(150);  // 11.6", a 4 s move at 2"/s
    driver->set_guide_rate(alpacacore::GuideRate{2.0 / 3600.0, 2.0 / 3600.0});

    std::atomic<bool> returned{false};
    std::thread caller([&] {
        try {
            driver->slew_to_coordinates(kTargetRa, kTargetDec);
        } catch (const std::exception&) {
        }
        returned.store(true);
    });
    ScopedJoin caller_join{caller, [&] { release_until(clock, returned); }};
    // Advance virtual time until the landing move is under way.
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (st->nudge_starts.load() < 1 && Clock::now() < deadline) {
        clock.wait_for_waiters(1, std::chrono::milliseconds(100));
        if (st->nudge_starts.load() >= 1) break;
        clock.advance(std::chrono::milliseconds(250));
        clock.wait_for_woken_settled(std::chrono::seconds(2));
    }
    REQUIRE(st->nudge_starts.load() == 1);
    REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(5)));  // the caller is parked in the nudge wait
    const auto parked_at = clock.now();

    REQUIRE_NOTHROW(driver->abort_slew());
    const auto join_deadline = Clock::now() + std::chrono::seconds(5);
    while (!returned.load() && Clock::now() < join_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(returned.load());  // without the fix the caller sleeps out the 4 s of virtual time, which never comes
    CHECK(clock.now() - parked_at < std::chrono::milliseconds(500));
    if (!returned.load()) {
        clock.advance(std::chrono::seconds(10));  // release the caller so the thread can join
    }
    caller.join();
    CHECK(st->nudge_count.load() == 1);  // AbortSlew stopped the move; the woken nudge did not stop it again
    driver->set_connected(false);
}

namespace {

// A Dec landing move's commands: `P` variable rate (3) on the Dec motor (17); a zero rate is a stop.
bool is_dec_rate(const std::string& command) {
    return command.size() >= 8 && command[0] == 'P' && command[1] == 3 && command[2] == 17;
}
bool is_dec_rate_stop(const std::string& command) { return is_dec_rate(command) && command[4] == 0 && command[5] == 0; }

// An async slew whose landing ends with one long Dec move (11.6" at 2"/s, about 6 s), started and not yet stopped.
// The landing move keeps the wire turn from its start to its stop (decision 0006, "Claim"); a newer initiator that
// claims wakes the move wait at once, so it waits for the move's stop exchange, not for the move.
struct LongDecNudge {
    alpacacore::test::FakeTaskClock clock;
    std::shared_ptr<LandingOffsetHandset> st = std::make_shared<LandingOffsetHandset>();
    std::unique_ptr<alpacacore::test::FakeMountServer> server;
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    std::size_t nudge_start = 0;  // the move's start in the wire log
    LongDecNudge() {
        st->attach(clock);
        st->ra_offset_counts.store(0);        // RA lands on target
        st->dec_offset_script[0].store(150);  // Dec lands 11.6" off: inside a timed move's reach
        st->dec_scripted.store(true);
        server = std::make_unique<alpacacore::test::FakeMountServer>(responder(st));
        REQUIRE(server->ok());
        driver = make_driver(server->port(), clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        driver->set_guide_rate(alpacacore::GuideRate{2.0 / 3600.0, 2.0 / 3600.0});
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
        const auto deadline = Clock::now() + kRealLimit;
        while (st->nudge_starts.load() < 1 && Clock::now() < deadline) {
            step_virtual(clock);
        }
        REQUIRE(st->nudge_starts.load() == 1);
        const auto wire = st->wire_log();
        const auto start = std::find_if(wire.begin(), wire.end(), [](const auto& command) {
            return is_dec_rate(command) && !is_dec_rate_stop(command);
        });
        REQUIRE(start != wire.end());
        nudge_start = static_cast<std::size_t>(start - wire.begin());
    }

    // Runs `initiator` on its own thread with virtual time standing still, so the move's remaining seconds never
    // come: an initiator that waited for them would not return. Returns its real-time latency in ms (-1: it did not
    // return within 3 s), then releases it so the thread can join.
    long time_initiator(const std::function<void()>& initiator) {
        std::promise<void> done;
        auto returned = done.get_future();
        std::atomic<long> took_ms{-1};
        std::thread caller([&] {
            const auto t0 = Clock::now();
            try {
                initiator();
                took_ms.store(static_cast<long>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()));
                done.set_value();
            } catch (...) {
                done.set_exception(std::current_exception());
            }
        });
        const bool in_time = returned.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
        while (returned.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready) {
            clock.advance(std::chrono::seconds(10));  // release the initiator so the thread can join
        }
        caller.join();
        CHECK_NOTHROW(returned.get());
        std::printf("initiator during a landing move returned after %ld ms\n", took_ms.load());
        std::fflush(stdout);
        return in_time ? took_ms.load() : -1;
    }
};

// One stop exchange plus a margin, with the fake's instant answers.
constexpr long kWaiterBoundMs = 500;

}  // namespace

TEST_CASE("SynScan initiator supersede - AbortSlew during a long landing move waits for its stop only",
          "[synscan][initiator-supersede][goto-landing]") {
    LongDecNudge n;
    const long took = n.time_initiator([&] { n.driver->abort_slew(); });
    CHECK(took >= 0);
    CHECK(took < kWaiterBoundMs);
    advance_idle(n.clock, std::chrono::milliseconds(600));
    const auto wire = n.st->wire_log();
    std::string order;
    for (const auto& command : wire) order += command.empty() ? '?' : command[0];
    INFO("wire order " << order);
    // The move's stop goes out before AbortSlew's stops (its `M` first).
    const auto stop = std::find_if(wire.begin() + static_cast<std::ptrdiff_t>(n.nudge_start) + 1, wire.end(),
                                   [](const auto& command) { return is_dec_rate_stop(command); });
    const auto abort_m = std::find_if(wire.begin() + static_cast<std::ptrdiff_t>(n.nudge_start) + 1, wire.end(),
                                      [](const auto& command) { return command[0] == 'M'; });
    REQUIRE(stop != wire.end());
    REQUIRE(abort_m != wire.end());
    CHECK(stop < abort_m);
    CHECK(n.st->nudge_count.load() == 1);  // the move was ended once, by its own stop
    // No axis moves: Dec is stopped and RA tracks at the sidereal rate (AbortSlew's tracking restore).
    CHECK(n.st->axis_counts_per_s[1].load() == 0.0);
    CHECK(n.st->ra_motor_speed() == kSiderealArcsecPerSec);
    CHECK_FALSE(n.driver->get_slewing());
    n.driver->set_connected(false);
}

TEST_CASE("SynScan initiator supersede - MoveAxis on the axis of a long landing move waits for its stop only",
          "[synscan][initiator-supersede][goto-landing]") {
    LongDecNudge n;
    const long took = n.time_initiator([&] { n.driver->move_axis(1, 0.1); });
    CHECK(took >= 0);
    CHECK(took < kWaiterBoundMs);
    advance_idle(n.clock, std::chrono::milliseconds(600));
    const auto wire = n.st->wire_log();
    std::string order;
    for (const auto& command : wire) order += command.empty() ? '?' : command[0];
    INFO("wire order " << order);
    // After the move's start: its stop, then MoveAxis's rate, and no stop after that rate.
    const auto first = wire.begin() + static_cast<std::ptrdiff_t>(n.nudge_start) + 1;
    const auto stop = std::find_if(first, wire.end(), [](const auto& command) { return is_dec_rate_stop(command); });
    const auto jog = std::find_if(
        first, wire.end(), [](const auto& command) { return is_dec_rate(command) && !is_dec_rate_stop(command); });
    REQUIRE(stop != wire.end());
    REQUIRE(jog != wire.end());
    CHECK(stop < jog);
    CHECK(std::none_of(jog, wire.end(), [](const auto& command) { return is_dec_rate_stop(command); }));
    // The landing move's stop did not end the jog: Dec runs at the MoveAxis rate (0.1 deg/s, 360"/s), far above the
    // landing move's 2"/s.
    CHECK(n.st->axis_counts_per_s[1].load() > 100.0 * kCountsPerArcsec);
    CHECK(n.driver->get_slewing());
    REQUIRE_NOTHROW(n.driver->move_axis(1, 0.0));
    CHECK(n.st->axis_counts_per_s[1].load() == 0.0);  // no axis is left moving
    n.driver->set_connected(false);
}

namespace {

// A tracking write: `T` and the mode byte (0 is off).
bool is_tracking_write(const std::string& command) { return command.size() >= 2 && command[0] == 'T'; }
// An RA landing move's start: `P` variable rate (3) on the RA motor (16) at a non-zero rate.
bool is_ra_rate_start(const std::string& command) {
    return command.size() >= 8 && command[0] == 'P' && command[1] == 3 && command[2] == 16 &&
           (command[4] != 0 || command[5] != 0);
}

}  // namespace

// Tracking is not a motion owner: it claims nothing and the landing goes on, but its `T` waits for the wire turn
// the RA landing move keeps until its stop. On an equatorial mount that stop is the tracking restore `T<mode>`, so a
// write sent during the move would reach the handset first and the restore would turn tracking back on while
// Tracking reads false (decision 0006, "Claim").
TEST_CASE("SynScan initiator supersede - Tracking off during a long RA landing move goes out after its restore",
          "[synscan][initiator-supersede][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    st->ra_offset_counts.store(150);  // 11.6" off: one timed RA move of several seconds at 2"/s
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_guide_rate(alpacacore::GuideRate{2.0 / 3600.0, 2.0 / 3600.0});
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
    const auto deadline = Clock::now() + kRealLimit;
    while (st->nudge_starts.load() < 1 && Clock::now() < deadline) {
        step_virtual(clock);
    }
    REQUIRE(st->nudge_starts.load() == 1);
    const auto started = st->wire_log();
    const auto start = std::find_if(started.begin(), started.end(), is_ra_rate_start);
    REQUIRE(start != started.end());
    const auto move_start = static_cast<std::ptrdiff_t>(start - started.begin());

    std::promise<void> done;
    auto returned = done.get_future();
    std::thread writer([&] {
        try {
            driver->set_tracking(false);
            done.set_value();
        } catch (...) {
            done.set_exception(std::current_exception());
        }
    });
    // Virtual time stands still, so the move does not end and the write waits for its turn.
    CHECK(returned.wait_for(std::chrono::milliseconds(300)) == std::future_status::timeout);
    const auto release_by = Clock::now() + std::chrono::seconds(30);
    while (returned.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready && Clock::now() < release_by) {
        step_virtual(clock);  // the move ends, its restore goes out, then the write
    }
    const bool wrote = returned.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    if (!wrote) {
        driver->set_connected(false);  // ends the move so the writer can be joined
    }
    writer.join();
    REQUIRE(wrote);
    CHECK_NOTHROW(returned.get());
    // Let the landing finish; it goes on with tracking off.
    const auto settle_by = Clock::now() + kRealLimit;
    while (driver->get_slewing() && Clock::now() < settle_by) {
        step_virtual(clock);
    }
    CHECK_FALSE(driver->get_slewing());

    const auto wire = st->wire_log();
    std::string order;
    for (const auto& command : wire) order += command.empty() ? '?' : command[0];
    INFO("wire order " << order);
    std::vector<int> modes;  // every tracking write after the move's start, in wire order
    for (auto it = wire.begin() + move_start + 1; it != wire.end(); ++it) {
        if (is_tracking_write(*it)) modes.push_back(static_cast<unsigned char>((*it)[1]));
    }
    REQUIRE(modes.size() >= 2);
    CHECK(modes[0] != 0);      // the move's stop: the tracking restore
    CHECK(modes.back() == 0);  // the client's write goes out after it and is the last tracking write
    CHECK(std::count(modes.begin(), modes.end(), 0) == 1);
    CHECK(st->ra_motor_speed() == 0.0);  // the handset is not tracking
    CHECK(st->tracking_mode.load() == 0);
    CHECK_FALSE(driver->get_tracking());
    driver->set_connected(false);
}

namespace {

// A Tracking write queued behind the RA landing move, then `initiator` (AbortSlew, or MoveAxis(0, 0)) behind it: the
// write's `T0` goes out first, so the initiator's tracking restore must use the mode at its own turn (off), not the
// one cached when it was called.
void check_initiator_behind_queued_tracking_write(const std::function<void(alpacacore::TelescopeDriver&)>& initiator) {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    st->ra_offset_counts.store(150);  // 11.6" off: one timed RA move of several seconds at 2"/s
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_guide_rate(alpacacore::GuideRate{2.0 / 3600.0, 2.0 / 3600.0});
    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
    const auto deadline = Clock::now() + kRealLimit;
    while (st->nudge_starts.load() < 1 && Clock::now() < deadline) {
        step_virtual(clock);
    }
    REQUIRE(st->nudge_starts.load() == 1);
    const auto started = st->wire_log();
    const auto start = std::find_if(started.begin(), started.end(), is_ra_rate_start);
    REQUIRE(start != started.end());
    const auto move_start = static_cast<std::ptrdiff_t>(start - started.begin());

    std::promise<void> written;
    auto write_done = written.get_future();
    std::thread writer([&] {
        try {
            driver->set_tracking(false);
            written.set_value();
        } catch (...) {
            written.set_exception(std::current_exception());
        }
    });
    // Virtual time stands still: the write has its ticket and waits for the move's turn.
    CHECK(write_done.wait_for(std::chrono::milliseconds(300)) == std::future_status::timeout);
    // The initiator claims, which ends the move at once; it then waits for the write's turn.
    std::exception_ptr initiator_error;
    try {
        initiator(*driver);
    } catch (...) {
        initiator_error = std::current_exception();
    }
    const bool wrote = write_done.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
    if (!wrote) {
        driver->set_connected(false);  // ends the move so the writer can be joined
    }
    writer.join();
    REQUIRE(wrote);
    CHECK_NOTHROW(write_done.get());
    CHECK_FALSE(initiator_error);
    advance_idle(clock, std::chrono::milliseconds(600));

    const auto wire = st->wire_log();
    std::string order;
    for (const auto& command : wire) order += command.empty() ? '?' : command[0];
    INFO("wire order " << order);
    std::vector<int> modes;  // every tracking write after the move's start, in wire order
    for (auto it = wire.begin() + move_start + 1; it != wire.end(); ++it) {
        if (is_tracking_write(*it)) modes.push_back(static_cast<unsigned char>((*it)[1]));
    }
    REQUIRE_FALSE(modes.empty());
    CHECK(modes.back() == 0);  // nothing turned tracking back on after the client's write
    CHECK(st->ra_motor_speed() == 0.0);
    CHECK(st->tracking_mode.load() == 0);
    CHECK_FALSE(driver->get_tracking());
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

}  // namespace

TEST_CASE("SynScan initiator supersede - AbortSlew queued behind a Tracking write keeps tracking off",
          "[synscan][initiator-supersede][goto-landing]") {
    check_initiator_behind_queued_tracking_write([](auto& driver) { driver.abort_slew(); });
}

TEST_CASE("SynScan initiator supersede - an RA MoveAxis stop queued behind a Tracking write keeps tracking off",
          "[synscan][initiator-supersede][goto-landing]") {
    check_initiator_behind_queued_tracking_write([](auto& driver) { driver.move_axis(0, 0.0); });
}

// With no motion in flight the turn is free: the write goes out at once and is the same `T` as before.
TEST_CASE("SynScan initiator supersede - a Tracking write while idle sends its T at once",
          "[synscan][initiator-supersede][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    for (const bool on : {false, true}) {
        INFO("Tracking " << on);
        const auto before = st->wire_log().size();
        const auto t0 = Clock::now();
        REQUIRE_NOTHROW(driver->set_tracking(on));
        CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() < 500);
        const auto wire = st->wire_log();
        REQUIRE(wire.size() == before + 1);
        // 2: equatorial tracking (model 50 is an EQ mount), in either hemisphere.
        CHECK(wire.back() == (on ? std::string("T\x02", 2) : std::string("T\x00", 2)));
        CHECK(st->tracking_mode.load() == (on ? 2 : 0));
        CHECK(driver->get_tracking() == on);
    }
    driver->set_connected(false);
}

TEST_CASE(
    "SynScan GOTO landing - a read on either side of a slightly misplaced tick is resolved by the last known RA "
    "(#1030)",
    "[synscan][telescope][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    st->ra_offset_counts.store(165);
    st->sawtooth_epoch.store(st->now().time_since_epoch().count());
    st->sawtooth.store(true);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);
    REQUIRE_NOTHROW(run_virtual(clock, [&] { driver->slew_to_coordinates(kTargetRa, kTargetDec); }));
    const auto epoch = st->sawtooth_epoch.load();

    // The handset's tick 150 ms later than the driver's anchor: a read 40 ms before the tick is at the bottom of
    // the ramp while the anchor says the second just began (12.8" low uncorrected). Then 150 ms earlier: a read
    // 40 ms after the tick is at the top while the anchor says the second is nearly over (12.8" high). Each is a
    // whole second off without the other side; the shift is well outside the anchor's own 25 ms uncertainty.
    // Once the side is right the residual is the shift itself (150 ms = 2.25") plus the anchor's and the phase
    // wait's own margins; the wrong side is 12-14" off.
    for (const auto& [shift_ms, phase] : {std::pair{150, 0.96}, std::pair{-150, 0.04}}) {
        st->sawtooth_epoch.store(epoch + Clock::duration(std::chrono::milliseconds(shift_ms)).count());
        advance_idle(clock, std::chrono::milliseconds(2100));
        advance_to_sawtooth_phase(clock, *st, phase);
        INFO("tick shifted " << shift_ms << " ms, read at phase " << phase);
        CHECK(ra_error_arcsec(*driver, kTargetRa) < 5.0);
    }
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - a landing the handset re-rolls by one second of RA still converges (#1030)",
          "[synscan][telescope][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    st->ra_offset_counts.store(165);
    st->reroll.store(true);
    st->sawtooth_epoch.store(st->now().time_since_epoch().count());
    st->sawtooth.store(true);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    for (int slew = 0; slew < 2; ++slew) {  // the second GOTO is the re-rolled one
        REQUIRE_NOTHROW(run_virtual(clock, [&] { driver->slew_to_coordinates(kTargetRa, kTargetDec); }));
        CHECK(ra_error_arcsec(*driver, kTargetRa) < 3.0);
        CHECK(dec_error_arcsec(*driver, kTargetDec) < 3.0);
    }
    CHECK(st->goto_count.load() == 2);  // one per slew
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - a residual beyond a timed move's reach gets one re-GOTO first (#1030)",
          "[synscan][telescope][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    st->ra_offset_counts.store(1300);  // 100": beyond 8 s at the default 7.5"/s guide rate
    st->reroll.store(true);            // the re-GOTO lands a second east, so a move still finishes it
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    REQUIRE_NOTHROW(run_virtual(clock, [&] { driver->slew_to_coordinates(kTargetRa, kTargetDec); }));

    CHECK(st->goto_count.load() == 2);  // the GOTO and one re-GOTO, then timed moves
    CHECK(st->nudge_count.load() >= 1);
    CHECK(ra_error_arcsec(*driver, kTargetRa) < 3.0);
    driver->set_connected(false);
}

namespace {

// 38,100 counts (2,941"): the RA miss of the handset's first GOTO in the #1105 TRACE.
constexpr uint32_t kFarMissCounts = 38100;

// Waits for an async slew to end. Returns the DriverException message if a Slewing read throws, else "".
std::string wait_async_slew(alpacacore::test::FakeTaskClock& clock, alpacacore::TelescopeDriver& driver) {
    const auto deadline = Clock::now() + kRealLimit;
    try {
        while (driver.get_slewing() && Clock::now() < deadline) {
            step_virtual(clock);
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
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    st->ra_offset_script[0].store(kFarMissCounts);
    st->ra_scripted.store(true);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    REQUIRE_NOTHROW(run_virtual(clock, [&] { driver->slew_to_coordinates(kTargetRa, kTargetDec); }));

    CHECK(st->goto_count.load() == 2);
    // The repeat aims at the target itself: one aimed by the 49' residual would land 49' west.
    CHECK(st->commanded_ra[1].load() == st->commanded_ra[0].load());
    CHECK(ra_error_arcsec(*driver, kTargetRa) < 3.0);
    CHECK(dec_error_arcsec(*driver, kTargetDec) < 3.0);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - a GOTO that misses by 49 arcmin twice fails the slew (#1105)",
          "[synscan][telescope][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    st->ra_offset_script[0].store(kFarMissCounts);
    st->ra_offset_script[1].store(kFarMissCounts);
    st->ra_scripted.store(true);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    bool threw = false;
    try {
        run_virtual(clock, [&] { driver->slew_to_coordinates(kTargetRa, kTargetDec); });
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
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    st->ra_offset_script[0].store(kFarMissCounts);
    st->ra_scripted.store(true);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
    CHECK(wait_async_slew(clock, *driver).empty());

    // Slewing fell only after the repeat GOTO and its landing moves.
    CHECK_FALSE(driver->get_slewing());
    CHECK(st->goto_count.load() == 2);
    CHECK(st->commanded_ra[1].load() == st->commanded_ra[0].load());
    CHECK(ra_error_arcsec(*driver, kTargetRa) < 3.0);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - an async GOTO that misses by 49 arcmin twice is reported through Slewing (#1105)",
          "[synscan][telescope][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    st->ra_offset_script[0].store(kFarMissCounts);
    st->ra_offset_script[1].store(kFarMissCounts);
    st->ra_scripted.store(true);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);

    REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
    const std::string error = wait_async_slew(clock, *driver);

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

// A status tick held inside its sample (after its exchanges, before its publish) while an async GOTO lands: the
// landing hunt waits for such a tick so its reads do not queue behind it, and that wait must stay bounded.
struct HuntBehindHeldTick {
    alpacacore::test::FakeTaskClock clock;
    std::shared_ptr<LandingOffsetHandset> st = std::make_shared<LandingOffsetHandset>();
    alpacacore::test::FakeMountServer server{responder(st)};
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    std::atomic<bool> entered{false};
    std::thread tick;
    // Releases and joins the held tick on every exit, a REQUIRE that throws out of the constructor included.
    struct Joiner {
        std::promise<void>& release;
        std::thread& tick;
        ~Joiner() {
            try {
                release.set_value();
            } catch (const std::future_error&) {  // NOLINT(bugprone-empty-catch): already released
            }
            if (tick.joinable()) tick.join();
        }
    } joiner{release, tick};

    HuntBehindHeldTick() {
        st->attach(clock);
        REQUIRE(server.ok());
        driver = make_driver(server.port(), clock);
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        namespace sc = alpacacore::vendor::synscan;
        sc::set_status_poll_paused_for_testing(*driver, true);  // only the held tick below samples
        sc::set_status_poll_after_sample_hook_for_testing(*driver, [this] {
            entered = true;
            (void)released.wait_for(std::chrono::seconds(10));
        });
        tick = std::thread([this] { (void)sc::poll_status_now_for_testing(*driver); });
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (!entered.load() && Clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(entered.load());
        const auto started = clock.now();
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
        // The slew loop reports Slewing through the driver's 8 s start window whatever the handset says, waiting on
        // this clock between re-reads; the landing hunt waits for the held tick in 2 ms sleeps on the same clock, and
        // the paused status poll waits on neither, so the hunt is the only waiter once the window has ended.
        // step_virtual() waits for a woken task to park again, which a hunt sleeping in 2 ms steps does only after
        // each advance, so the clock is advanced by hand across the end of the window and the slew loop's last
        // 250 ms re-read.
        while (clock.now() - started < std::chrono::milliseconds(7900)) {
            step_virtual(clock);
        }
        while (clock.now() - started < std::chrono::milliseconds(8600)) {
            clock.advance(kVirtualStep);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        REQUIRE(clock.wait_for_waiters(1, std::chrono::seconds(2)));  // the hunt, sleeping out the held tick
    }

    ~HuntBehindHeldTick() { driver->set_connected(false); }
};

TEST_CASE("SynScan GOTO landing - the hunt reads past a held status tick once its wait bound runs out (#1247)",
          "[synscan][telescope][goto-landing]") {
    HuntBehindHeldTick h;
    // The slew-end measurement is already read; the next precise read is the hunt's. The tick stays held for 10 s,
    // and past the 1.5 s bound the hunt reads anyway.
    const int before = h.st->position_reads.load();
    const auto until = Clock::now() + std::chrono::seconds(3);
    while (h.st->position_reads.load() == before && Clock::now() < until) {
        h.clock.advance(kVirtualStep);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(h.st->position_reads.load() > before);
}

// The hunt waits for a probe inside its exchanges with mutex_ released (decision 0016 point 2), so a getter that takes
// mutex_ is not held up by it. Each getter runs on its own thread: with the lock held across the wait the thread
// blocks until the wait bound (1.5 s) or a clock step ends it, and the case still ends.
TEST_CASE("SynScan GOTO landing - getters stay in budget while the hunt waits on a held status tick",
          "[synscan][telescope][goto-landing]") {
    HuntBehindHeldTick h;
    const std::vector<std::pair<const char*, std::function<void()>>> getters = {
        {"Tracking", [&] { (void)h.driver->get_tracking(); }},
        {"GuideRate", [&] { (void)h.driver->get_guide_rate(); }},
        {"AtPark", [&] { (void)h.driver->get_at_park(); }},
        {"IsPulseGuiding", [&] { (void)h.driver->get_is_pulse_guiding(); }},
        {"RightAscension", [&] { (void)h.driver->get_right_ascension(); }},
    };
    for (const auto& [name, read] : getters) {
        CAPTURE(name);
        std::atomic<bool> done{false};
        const auto started = Clock::now();
        std::thread caller([&] {
            try {
                read();
            } catch (const alpacacore::AlpacaException&) {  // NOLINT(bugprone-empty-catch): an answer too
            }
            done = true;
        });
        while (!done.load() && Clock::now() - started < std::chrono::milliseconds(500)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const bool in_budget = done.load();
        while (!done.load()) {
            h.clock.advance(kVirtualStep);  // lets a mutated driver's wait end so the case can finish
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        caller.join();
        CHECK(in_budget);
    }
}

TEST_CASE("SynScan GOTO landing - a new slew while the hunt waits on a held status tick starts at once (#1247)",
          "[synscan][telescope][goto-landing]") {
    HuntBehindHeldTick h;
    // The new initiator cancels and reaps the landing task; the cancel must end the hunt's wait, not the bound.
    const auto t0 = Clock::now();
    REQUIRE_NOTHROW(h.driver->slew_to_coordinates_async(kTargetRa + 0.5, kTargetDec));
    CHECK(Clock::now() - t0 < std::chrono::milliseconds(1000));  // under the 1.5 s bound
}

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

// The paused status poll waits its period on the clock for good, so a waiter alone does not say the pulse's stop
// timer has parked, and a clock advanced before it has would end the pulse late: give the timer's thread real time.
void wait_pulse_done(alpacacore::test::FakeTaskClock& clock, alpacacore::TelescopeDriver& driver) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto deadline = Clock::now() + kRealLimit;
    while (driver.get_is_pulse_guiding() && Clock::now() < deadline) {
        // The flag also clears by a clock read once the pulse's completion delay has passed, with no task parked.
        if (!step_virtual(clock)) clock.advance(kVirtualStep);
    }
}

TEST_CASE("SynScan PulseGuide - a west RA pulse lowers RA and an east one raises it, in either hemisphere (#1030)",
          "[synscan][telescope][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    // On the handset a variable-rate P slew is the RA motor's own speed, not an offset from tracking (rig,
    // 2026-10-13: a 2 s west pulse at 7.5"/s moved RA 50.6" EAST, an east one 19.3" east). A pulse therefore runs
    // the motor westward at sidereal + guide rate (west) or sidereal - guide rate (east) and hands back to tracking.
    const bool southern = GENERATE(false, true);
    CAPTURE(southern);
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    st->southern.store(southern);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    // The case measures the motor over virtual seconds; a status poll tick sharing the wrapper with the pulse's stop
    // would add real-time jitter to that count.
    alpacacore::vendor::synscan::set_status_poll_paused_for_testing(*driver, true);
    driver->set_tracking(true);
    driver->set_guide_rate(alpacacore::GuideRate{7.5 / 3600.0, 7.5 / 3600.0});
    const double expected_counts = 7.5 * kCountsPerArcsec;  // 1 s at 7.5"/s
    const double tolerance_counts = 1.5 * kCountsPerArcsec;

    uint32_t before = st->ra_now();
    driver->pulse_guide(3, 1000);  // West
    wait_pulse_done(clock, *driver);
    advance_idle(clock, std::chrono::milliseconds(300));  // tracking again: RA must stay put
    const int32_t west = ra_counts_since(*st, before);
    CHECK(std::abs(west + expected_counts) < tolerance_counts);

    before = st->ra_now();
    driver->pulse_guide(2, 1000);  // East
    wait_pulse_done(clock, *driver);
    advance_idle(clock, std::chrono::milliseconds(300));
    const int32_t east = ra_counts_since(*st, before);
    CHECK(std::abs(east - expected_counts) < tolerance_counts);
    driver->set_connected(false);
}

TEST_CASE("SynScan MoveAxis - stopping the RA axis while tracking resumes tracking (#1030)",
          "[synscan][telescope][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    // A variable-rate P slew replaces tracking on the handset, so a plain stop leaves the RA motor stopped and RA
    // running away at the sidereal rate while Tracking still reads true. ASCOM resumes the tracking rate when
    // MoveAxis stops.
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_tracking(true);

    driver->move_axis(0, 0.01);
    advance_idle(clock, std::chrono::milliseconds(200));
    driver->move_axis(0, 0.0);
    const uint32_t stopped_at = st->ra_now();
    advance_idle(clock, std::chrono::milliseconds(1000));
    CHECK(std::abs(ra_counts_since(*st, stopped_at)) < 0.5 * kCountsPerArcsec);  // not 15" a second
    CHECK(driver->get_tracking());
    driver->set_connected(false);
}

TEST_CASE("SynScan MoveAxis - an RA jog after a landing drops the last known RA (#1030)",
          "[synscan][telescope][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    // The last known RA picks the side of the handset's tick for a read inside its uncertainty. A jog of 7.5" to
    // 22.5" leaves the true RA nearer the stale landing's one-second neighbour than the landing itself, so a stale
    // value turns every read for the rest of the anchor window one second of RA (15") wrong.
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    st->sawtooth.store(true);
    st->sawtooth_epoch.store(st->now().time_since_epoch().count());
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);
    REQUIRE_NOTHROW(run_virtual(clock, [&] { driver->slew_to_coordinates(kTargetRa, kTargetDec); }));
    REQUIRE(true_ra_error_arcsec(*st, kTargetRa) < 3.0);

    // About 15" east: on the fake's motor model a westward speed of -14.4"/s raises RA at 29.4"/s.
    const double westward_code_rate = -14.4 / 3600.0;
    driver->move_axis(0, westward_code_rate);
    advance_idle(clock, std::chrono::milliseconds(500));
    driver->move_axis(0, 0.0);
    const double jog_arcsec = true_ra_error_arcsec(*st, kTargetRa);
    REQUIRE(jog_arcsec > 8.0);
    REQUIRE(jog_arcsec < 22.0);

    const auto ra_counts = st->ra_now();
    const double true_ra_hours = ra_counts / (kCountsMask + 1.0) * 24.0;
    for (const double phase : {0.15, 0.5, 0.9}) {
        advance_to_sawtooth_phase(clock, *st, phase);
        CAPTURE(phase);
        CHECK(ra_error_arcsec(*driver, true_ra_hours) < 3.0);
    }
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - a superseded synchronous landing move leaves the newer MoveAxis running (#1030)",
          "[synscan][telescope][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    // A synchronous slew's landing move sleeps with no cancel; a client that aborts and jogs meanwhile owns the
    // axis, so the move must not end it when it wakes.
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_guide_rate(alpacacore::GuideRate{7.5 / 3600.0, 7.5 / 3600.0});  // the 29" RA residual: ~3.9 s

    std::atomic<bool> slew_done{false};
    std::thread slew([&] {
        try {
            driver->slew_to_coordinates(kTargetRa, kTargetDec);
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
            // superseded by the abort
        }
        slew_done.store(true);
    });
    ScopedJoin slew_join{slew, [&] { release_until(clock, slew_done); }};
    const auto deadline = Clock::now() + kRealLimit;
    while (st->nudge_starts.load() < 1 && Clock::now() < deadline) {
        step_virtual(clock);
    }
    REQUIRE(st->nudge_starts.load() >= 1);
    advance_idle(clock, std::chrono::milliseconds(300));
    driver->abort_slew();
    driver->move_axis(0, 0.5);
    while (!slew_done.load()) {
        step_virtual(clock);
    }
    slew.join();
    advance_idle(clock, std::chrono::milliseconds(200));
    CHECK(std::abs(st->ra_motor_speed() - 0.5 * 3600.0) < 1.0);  // the jog still runs
    driver->move_axis(0, 0.0);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - an Alt-Az mount finishes its landing with re-GOTOs, not axis moves (#1030)",
          "[synscan][telescope][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    // The landing residual is in RA/Dec; on an Alt-Az mount the motor axes are azimuth and altitude, so the timed
    // moves (and the sidereal base rate of the RA motor model) do not apply. The residual-aimed re-GOTO does.
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    st->model_id.store(128);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_alignment_mode() == alpacacore::AlignmentMode::AltAz);
    use_fast_guide_rate(*driver);

    REQUIRE_NOTHROW(run_virtual(clock, [&] { driver->slew_to_coordinates(kTargetRa, kTargetDec); }));

    CHECK(st->nudge_starts.load() == 0);
    CHECK(st->goto_count.load() >= 2);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - the re-GOTO after a far-miss repeat is aimed by the repeat's residual alone (#1120)",
          "[synscan][telescope][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    // Alt-Az: every landing round is a re-GOTO. GOTO 1 lands 386" off in RA and 77" in Dec (past the RA tolerance, so
    // the carried RA aim moves too), so GOTO 2 is aimed by those residuals; GOTO 2 then misses 49' in RA, so GOTO 3
    // repeats the target; GOTO 3 lands 39" off in RA and Dec. GOTO 4 must be the target corrected by that 39" alone,
    // not by GOTO 1's miss as well, on both axes.
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
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
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE(driver->get_alignment_mode() == alpacacore::AlignmentMode::AltAz);
    use_fast_guide_rate(*driver);

    REQUIRE_NOTHROW(run_virtual(clock, [&] { driver->slew_to_coordinates(kTargetRa, kTargetDec); }));

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
    alpacacore::test::FakeTaskClock clock;
    // Rig, 2026-10-13: the RA landing moves ran the motor the wrong way and the landing ended 113" off after three
    // passes. Checked against the fake's own RA, not the driver's report of it.
    const bool southern = GENERATE(false, true);
    CAPTURE(southern);
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    st->southern.store(southern);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);
    LandingWarnCounter warns;

    REQUIRE_NOTHROW(run_virtual(clock, [&] { driver->slew_to_coordinates(kTargetRa, kTargetDec); }));

    CHECK(true_ra_error_arcsec(*st, kTargetRa) < 3.0);
    CHECK(warns.warns.load() == 0);
    CHECK(st->goto_count.load() == 1);
    driver->set_connected(false);
}

TEST_CASE("SynScan GOTO landing - the hunt waits for a poll tick that is inside its exchanges",
          "[synscan][telescope][goto-landing]") {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);
    alpacacore::vendor::synscan::set_status_poll_paused_for_testing(*driver, true);

    // A tick parks after its exchanges with its sampling flag still up, as a slow link leaves it.
    std::atomic<bool> in_sample{false};
    std::atomic<bool> release{false};
    alpacacore::vendor::synscan::set_status_poll_after_sample_hook_for_testing(*driver, [&] {
        in_sample.store(true);
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    std::thread tick([&] { alpacacore::vendor::synscan::poll_status_now_for_testing(*driver); });
    ScopedJoin tick_join{tick, [&] { release.store(true); }};
    const auto real_deadline = Clock::now() + std::chrono::seconds(10);
    while (!in_sample.load() && Clock::now() < real_deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(in_sample.load());

    std::atomic<bool> done{false};
    std::thread slew([&] {
        try {
            driver->slew_to_coordinates(kTargetRa, kTargetDec);
        } catch (...) {
        }
        done.store(true);
    });
    // The GOTO ends and the hunt asks for the line: virtual time steps through the whole GOTO and the landing. The
    // hunt reads nothing while the tick holds the line, up to the 1.5 s bound, so a stretch of virtual time at least
    // the bound separates two precise reads that start after the GOTO's 8 s start window.
    auto last_reads = st->position_reads.load();
    const auto slew_started = clock.now();
    auto last_change = slew_started;
    std::chrono::nanoseconds longest_gap{};
    const auto hold_until = Clock::now() + std::chrono::seconds(60);
    while (!done.load() && Clock::now() < hold_until) {
        step_virtual(clock);
        if (st->position_reads.load() != last_reads) {
            if (last_change - slew_started >= std::chrono::seconds(8)) {
                longest_gap = std::max(longest_gap, clock.now() - last_change);
            }
            last_reads = st->position_reads.load();
            last_change = clock.now();
        }
    }
    CHECK(longest_gap >= std::chrono::milliseconds(1400));

    release.store(true);
    tick.join();
    const auto finish_deadline = Clock::now() + std::chrono::seconds(60);
    while (!done.load() && Clock::now() < finish_deadline) step_virtual(clock);
    CHECK(done.load());
    while (!done.load()) {
        clock.advance(std::chrono::hours(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    slew.join();
    alpacacore::vendor::synscan::set_status_poll_after_sample_hook_for_testing(*driver, nullptr);
    driver->set_connected(false);
}

#endif  // !_WIN32

namespace {

// Virtual time one slew_to_coordinates takes, stepped to completion. With `poll_in_flight` a poll tick is parked
// between its sample and its publish for the whole slew, so probe_sampling_ stays set at every tick hunt.
std::chrono::nanoseconds virtual_slew_duration(bool poll_in_flight) {
    alpacacore::test::FakeTaskClock clock;
    auto st = std::make_shared<LandingOffsetHandset>();
    st->attach(clock);
    alpacacore::test::FakeMountServer server(responder(st));
    REQUIRE(server.ok());
    auto driver = make_driver(server.port(), clock);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    use_fast_guide_rate(*driver);
    alpacacore::vendor::synscan::set_status_poll_paused_for_testing(*driver, true);

    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    std::thread poll;
    ScopedJoin poll_join{poll, [&] {
                             {
                                 std::lock_guard<std::mutex> lock(mutex);
                                 release = true;
                             }
                             cv.notify_all();
                         }};
    if (poll_in_flight) {
        alpacacore::vendor::synscan::set_status_poll_after_sample_hook_for_testing(*driver, [&] {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true;
            cv.notify_all();
            cv.wait_for(lock, std::chrono::seconds(60), [&] { return release; });
        });
        poll = std::thread([&] { alpacacore::vendor::synscan::poll_status_now_for_testing(*driver); });
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(10), [&] { return entered; }));
    }

    const auto started = clock.now();
    REQUIRE_NOTHROW(run_virtual(clock, [&] { driver->slew_to_coordinates(kTargetRa, kTargetDec); }));
    const auto elapsed = clock.now() - started;

    if (poll.joinable()) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            release = true;
        }
        cv.notify_all();
        poll.join();
        alpacacore::vendor::synscan::set_status_poll_after_sample_hook_for_testing(*driver, nullptr);
    }
    driver->set_connected(false);
    return elapsed;
}

}  // namespace

TEST_CASE("SynScan GOTO landing - the tick hunt waits out a poll tick in flight on the task clock (#1403)",
          "[synscan][telescope][goto-landing]") {
    const auto baseline = virtual_slew_duration(false);
    const auto held = virtual_slew_duration(true);
    // Each hunt waits kPollSampleWaitBound (1500 ms) for the parked tick; the wait is virtual time, so it shows in
    // the clock. A wait on the steady clock would add no virtual time (and 1.5 s of real time).
    CHECK(held - baseline >= std::chrono::milliseconds(1400));
}

// The landing hunt inside its first precise read, the reply held on the handset: the hunt owns the line (its
// HuntHold is taken) and the wrapper mutex is busy, with the status poll paused so only a test-driven probe samples.
// The handset holds the hunt's first read (hold_e_after_goto).
struct HuntOnTheLine {
    alpacacore::test::FakeTaskClock clock;
    std::shared_ptr<LandingOffsetHandset> st = std::make_shared<LandingOffsetHandset>();
    alpacacore::test::FakeMountServer server{responder(st)};
    std::unique_ptr<alpacacore::TelescopeDriver> driver;
    std::thread probe;
    std::atomic<bool> probe_done{false};

    HuntOnTheLine() {
        st->attach(clock);
        st->sawtooth_epoch.store(st->now().time_since_epoch().count());
        st->sawtooth.store(true);
        st->hold_e.store(true);
        st->hold_e_after_goto.store(true);
        REQUIRE(server.ok());
        driver = make_driver(server.port(), clock, 10000);  // longer than the 6 s the handset may hold the reply
        REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
        alpacacore::vendor::synscan::set_status_poll_paused_for_testing(*driver, true);
        REQUIRE_NOTHROW(driver->slew_to_coordinates_async(kTargetRa, kTargetDec));
        const auto deadline = Clock::now() + kRealLimit;
        while (!st->e_held.load() && Clock::now() < deadline) {
            step_virtual(clock);
        }
        REQUIRE(st->e_held.load());
    }

    // One full tick on its own thread; it parks on the hold until the hunt ends, the bound runs out or a disconnect.
    void start_probe() {
        const std::size_t parked = clock.waiter_count();  // the paused poll thread waits its period on the clock
        probe = std::thread([this] {
            alpacacore::vendor::synscan::poll_status_now_for_testing(*driver);
            probe_done = true;
        });
        REQUIRE(clock.wait_for_waiters(parked + 1, std::chrono::seconds(2)));  // the probe, parked on the hold
    }

    ~HuntOnTheLine() {
        st->hold_e.store(false);
        if (probe.joinable()) {
            release_until(clock, probe_done);
            probe.join();
        }
        run_virtual(clock, [this] { driver->set_connected(false); });
    }
};

TEST_CASE("SynScan GOTO landing - a status tick waits out the hunt and makes no exchange while it holds the line",
          "[synscan][telescope][goto-landing]") {
    HuntOnTheLine h;
    const int reads_e = h.st->position_reads.load();
    const int reads_l = h.st->status_reads.load();
    h.start_probe();  // a probe that sampled instead would queue on the wrapper mutex, not park on the clock
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(h.st->position_reads.load() == reads_e);
    CHECK(h.st->status_reads.load() == reads_l);
    CHECK_FALSE(h.probe_done.load());
    // The hunt ends within its 3.4 s budget, inside the probe's 4.9 s bound: the tick then runs.
    h.st->hold_e.store(false);
    const auto deadline = Clock::now() + kRealLimit;
    while (!h.probe_done.load() && Clock::now() < deadline) {
        step_virtual(h.clock);
    }
    CHECK(h.probe_done.load());
    CHECK(h.st->status_reads.load() > reads_l);
}

TEST_CASE("SynScan GOTO landing - a status tick held past its bound does not renew the served frame",
          "[synscan][telescope][goto-landing]") {
    HuntOnTheLine h;
    // The DeviceState TimeStamp is when the poll measured: an old frame served again as new would reset its age.
    const auto frame_age = [&] {
        const auto stamp = alpacacore::test::parse_timestamp(alpacacore::test::time_stamp_of(*h.driver));
        return std::chrono::duration<double>(std::chrono::system_clock::now() - stamp).count();
    };
    const double age_before = frame_age();  // the paused poll has served the connect frame since
    h.start_probe();
    h.clock.advance(std::chrono::seconds(5));  // past the 4.9 s bound, inside the handset's 1 s real timeout
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (!h.probe_done.load() && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(h.probe_done.load());
    CHECK(frame_age() > age_before + 4.5);
}

TEST_CASE("SynScan GOTO landing - set_connected(false) ends a tick waiting on the hunt's hold at once",
          "[synscan][telescope][goto-landing]") {
    HuntOnTheLine h;
    // The module's own poll thread takes the tick, so the disconnect has to join it.
    alpacacore::vendor::synscan::set_status_poll_paused_for_testing(*h.driver, false);
    h.clock.advance(std::chrono::seconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // the woken poll thread parks on the hold
    REQUIRE(h.st->e_held.load());  // the hunt's read is still unanswered, so its hold is still taken
    std::atomic<bool> disconnected{false};
    std::thread closer([&] {
        h.driver->set_connected(false);
        disconnected = true;
    });
    // The clock stays put: the probe's 4.9 s wait is virtual, so only the disconnect can end it. The handset
    // answers the hunt's held read now, so the hunt ends too; the probe's wake-up is the disconnect's own.
    h.st->hold_e.store(false);
    const auto deadline = Clock::now() + std::chrono::seconds(4);
    while (!disconnected.load() && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const bool in_budget = disconnected.load();
    release_until(h.clock, disconnected);
    closer.join();
    CHECK(in_budget);
}
