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

// open-astro#547 review findings: direct, hardware-free coverage of
// TelescopeDriver::stop_motion_if_client_silent()'s exception handling and
// in-flight-request bookkeeping, over a minimal stub rather than a vendor
// fake -- these are base-class behaviors, not driver-specific ones. Written
// RED FIRST against the code the review examined: before this fix,
// finding 2's case aborted (abort_slew()'s throw skipped the per-axis stop
// entirely) and finding 3's case left the watchdog permanently disarmed
// (silence_check_pending_ stayed false after a throwing probe).

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/logging.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "catch2_compat.h"

namespace {

// Minimal stub exercising only what stop_motion_if_client_silent() touches:
// get_connected(), get_slewing(), abort_slew(), get_can_move_axis(),
// move_axis(). Every other pure virtual returns an inert default. Throw
// behavior is scripted per call via the *_throw_countdown_ fields: N means
// "throw on the next N calls, then stop", 0 means "never throw".
class WatchdogUnitStubDriver final : public alpacacore::TelescopeDriver {
public:
    int get_device_number() const override { return 1; }
    std::string get_name() const override { return "Watchdog Unit Stub"; }
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::Telescope; }
    std::string get_unique_id() const override { return "watchdog-unit-stub"; }
    std::string get_description() const override { return "unit-test stub"; }
    std::string get_driver_info() const override { return "unit-test stub driver"; }
    std::string get_driver_version() const override { return "0.0.1"; }
    int get_interface_version() const override { return 4; }
    bool get_connected() const override { return true; }
    void set_connected(bool) override {}
    std::vector<std::string> get_supported_actions() const override { return {}; }
    std::string action(std::string_view, std::string_view) override { return ""; }
    bool can_action(std::string_view) const override { return false; }
    std::string command_blind(std::string_view, bool) override { return ""; }
    bool command_bool(std::string_view, bool) override { return false; }
    std::string command_string(std::string_view, bool) override { return ""; }
    std::chrono::system_clock::time_point get_utc_date() const override { return {}; }
    void set_utc_date(std::chrono::system_clock::time_point) override {}
    alpacacore::AlignmentMode get_alignment_mode() const override { return alpacacore::AlignmentMode::GermanPolar; }
    double get_altitude() const override { return 0.0; }
    double get_aperture_diameter() const override { return 0.0; }
    void set_aperture_diameter(double) override {}
    double get_aperture_area() const override { return 0.0; }
    bool get_at_home() const override { return false; }
    bool get_at_park() const override { return false; }
    double get_azimuth() const override { return 0.0; }
    bool get_can_find_home() const override { return false; }
    bool get_can_park() const override { return false; }
    bool get_can_pulse_guide() const override { return false; }
    bool get_is_pulse_guiding() const override { return false; }
    bool get_can_set_declination_rate() const override { return false; }
    bool get_can_set_guide_rates() const override { return false; }
    bool get_can_set_park() const override { return false; }
    bool get_can_set_pier_side() const override { return false; }
    bool get_can_set_right_ascension_rate() const override { return false; }
    bool get_can_set_tracking() const override { return false; }
    bool get_can_slew_alt_az() const override { return false; }
    bool get_can_slew_alt_az_async() const override { return false; }
    bool get_can_sync_alt_az() const override { return false; }
    bool get_can_slew() const override { return false; }
    bool get_can_slew_async() const override { return false; }
    bool get_can_sync() const override { return false; }
    bool get_can_unpark() const override { return false; }
    double get_declination() const override { return 0.0; }
    double get_declination_rate() const override { return 0.0; }
    void set_declination_rate(double) override {}
    bool get_tracking() const override { return true; }
    void set_tracking(bool) override {}
    double get_focal_length() const override { return 0.0; }
    void set_focal_length(double) override {}
    alpacacore::GuideRate get_guide_rate() const override { return alpacacore::GuideRate{}; }
    void set_guide_rate(const alpacacore::GuideRate&) override {}
    double get_right_ascension() const override { return 0.0; }
    double get_right_ascension_rate() const override { return 0.0; }
    void set_right_ascension_rate(double) override {}
    int get_side_of_pier() const override { return 0; }
    void set_side_of_pier(int) override {}
    int get_destination_side_of_pier(double, double) const override { return 0; }
    alpacacore::EquatorialSystem get_equatorial_system() const override {
        return alpacacore::EquatorialSystem::Topocentric;
    }
    bool get_does_refraction() const override { return false; }
    void set_does_refraction(bool) override {}
    int get_slew_settle_time() const override { return 0; }
    void set_slew_settle_time(int) override {}
    double get_sidereal_time() const override { return 0.0; }
    double get_site_elevation() const override { return 0.0; }
    void set_site_elevation(double) override {}
    double get_site_latitude() const override { return 0.0; }
    void set_site_latitude(double) override {}
    double get_site_longitude() const override { return 0.0; }
    void set_site_longitude(double) override {}

    // --- The fields the watchdog actually probes --------------------------
    std::atomic<bool> slewing{false};
    std::atomic<int> get_slewing_throw_countdown{0};
    bool get_slewing() const override {
        if (get_slewing_throw_countdown.load() > 0) {
            const_cast<std::atomic<int>&>(get_slewing_throw_countdown).fetch_sub(1);
            throw std::runtime_error("simulated transient link fault");
        }
        return slewing.load();
    }

    std::atomic<int> aborts{0};
    std::atomic<int> abort_throw_countdown{0};
    void abort_slew() override {
        ++aborts;
        if (abort_throw_countdown.load() > 0) {
            abort_throw_countdown.fetch_sub(1);
            throw std::runtime_error("simulated abort_slew failure");
        }
        slewing.store(false);
    }

    std::atomic<int> move_axis_calls{0};
    std::atomic<int> move_axis_throw_countdown{0};
    void move_axis(int, double) override {
        ++move_axis_calls;
        if (move_axis_throw_countdown.load() > 0) {
            move_axis_throw_countdown.fetch_sub(1);
            throw std::runtime_error("simulated move_axis failure");
        }
    }

    double get_target_declination() const override { return 0.0; }
    void set_target_declination(double) override {}
    double get_target_right_ascension() const override { return 0.0; }
    void set_target_right_ascension(double) override {}
    int get_tracking_rate() const override { return 0; }
    void set_tracking_rate(int) override {}
    std::vector<int> get_tracking_rates() const override { return {}; }
    void find_home() override {}
    void park() override {}
    void pulse_guide(int, int) override {}
    void set_park() override {}
    void slew_to_coordinates(double, double) override {}
    void slew_to_coordinates_async(double, double) override {}
    void slew_to_target() override {}
    void slew_to_target_async() override {}
    void sync_to_coordinates(double, double) override {}
    void sync_to_target() override {}
    void unpark() override {}
    bool get_can_move_axis(int) const override { return true; }
    std::pair<double, double> get_axis_rate_range(int) const override { return {0.0, 0.0}; }
    void slew_to_alt_az(double, double) override {}
    void slew_to_alt_az_async(double, double) override {}
    void sync_to_alt_az(double, double) override {}
};

}  // namespace

// Finding 2: a throwing abort_slew() must not skip the per-axis stop --
// abort_slew() is "belt", move_axis(axis, 0) is "braces", and the whole
// point of having both is that one working when the other doesn't.
TEST_CASE("TelescopeDriver watchdog - a throwing abort_slew still runs the per-axis stop",
          "[telescope][watchdog][unit]") {
    WatchdogUnitStubDriver driver;
    driver.slewing.store(true);
    driver.abort_throw_countdown.store(1);  // abort_slew() throws exactly once

    const auto t0 = std::chrono::steady_clock::now();
    driver.note_client_activity(t0);

    REQUIRE(driver.stop_motion_if_client_silent(t0 + std::chrono::seconds(31), std::chrono::seconds(30)));

    CHECK(driver.aborts.load() == 1);
    CHECK(driver.move_axis_calls.load() == 2);  // both axes, despite the throw
}

// Finding 3: a probe (get_slewing()) that throws must re-arm rather than
// permanently disarm the watchdog -- otherwise a transient link fault (the
// exact case open-astro#521 exists for) silences this device until another
// client request happens to land, which during a runaway may never come.
TEST_CASE("TelescopeDriver watchdog - a throwing probe re-arms for the next tick", "[telescope][watchdog][unit]") {
    WatchdogUnitStubDriver driver;
    driver.slewing.store(true);
    driver.get_slewing_throw_countdown.store(1);  // get_slewing() throws exactly once

    const auto t0 = std::chrono::steady_clock::now();
    driver.note_client_activity(t0);

    // First tick: the probe throws. Nothing was found slewing (we never
    // got that far), nothing was stopped, and the watchdog must retry.
    CHECK_FALSE(driver.stop_motion_if_client_silent(t0 + std::chrono::seconds(31), std::chrono::seconds(30)));
    CHECK(driver.aborts.load() == 0);

    // Second tick, no further client activity: the probe now succeeds and
    // finds the mount still slewing. A watchdog that failed to re-arm after
    // the first tick's exception would silently skip this forever.
    CHECK(driver.stop_motion_if_client_silent(t0 + std::chrono::seconds(32), std::chrono::seconds(30)));
    CHECK(driver.aborts.load() == 1);
}

// Review finding (#628): a probe that keeps throwing (a connected-but-faulted
// mount, e.g. iOptron's device_faulted_ state) must keep retrying every tick
// but must not write one ERROR line per tick for the whole silence episode.
TEST_CASE("TelescopeDriver watchdog - a persistently throwing probe retries every tick but logs once",
          "[telescope][watchdog][unit]") {
    struct ErrorCounter {
        std::atomic<int> retry_lines{0};
        alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
        ErrorCounter() {
            alpacacore::logging::set_log_sink(
                [this](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
                    if (level == alpacacore::logging::LogLevel::Error &&
                        message.find("will retry") != std::string_view::npos) {
                        ++retry_lines;
                    }
                });
        }
        ~ErrorCounter() { alpacacore::logging::set_log_sink(previous); }
    } counter;

    WatchdogUnitStubDriver driver;
    driver.slewing.store(true);
    driver.get_slewing_throw_countdown.store(1000);

    const auto t0 = std::chrono::steady_clock::now();
    driver.note_client_activity(t0);

    for (int tick = 1; tick <= 5; ++tick) {
        CHECK_FALSE(
            driver.stop_motion_if_client_silent(t0 + std::chrono::seconds(30 + tick), std::chrono::seconds(30)));
    }
    CHECK(driver.get_slewing_throw_countdown.load() == 1000 - 5);  // still probed on every tick
    CHECK(counter.retry_lines.load() == 1);

    // A new client request starts a new silence episode, which may log again.
    const auto t1 = t0 + std::chrono::seconds(100);
    driver.note_client_activity(t1);
    CHECK_FALSE(driver.stop_motion_if_client_silent(t1 + std::chrono::seconds(31), std::chrono::seconds(30)));
    CHECK(counter.retry_lines.load() == 2);
}

// Red-team finding: when every stop call throws (a transient link fault at
// the moment of the stop), nothing was stopped, so the watchdog must re-arm
// and retry on the next tick -- the same reasoning as a throwing probe.
// Otherwise the axis keeps running with the watchdog disarmed until another
// client request lands, which during a runaway may never come.
TEST_CASE("TelescopeDriver watchdog - a stop where every call throws re-arms for the next tick",
          "[telescope][watchdog][unit]") {
    WatchdogUnitStubDriver driver;
    driver.slewing.store(true);
    driver.abort_throw_countdown.store(1);      // abort_slew() throws once
    driver.move_axis_throw_countdown.store(2);  // both per-axis stops throw once

    const auto t0 = std::chrono::steady_clock::now();
    driver.note_client_activity(t0);

    // First tick: every stop call throws, so nothing was stopped.
    CHECK_FALSE(driver.stop_motion_if_client_silent(t0 + std::chrono::seconds(31), std::chrono::seconds(30)));
    CHECK(driver.aborts.load() == 1);
    CHECK(driver.move_axis_calls.load() == 2);

    // Second tick, no further client activity: the stop now succeeds.
    CHECK(driver.stop_motion_if_client_silent(t0 + std::chrono::seconds(32), std::chrono::seconds(30)));
    CHECK(driver.aborts.load() == 2);
}

// Finding 1's mechanism, at the base-class level: a request the router
// marked "in flight" (begin_client_request()) must be treated as activity
// for as long as it stays in flight, even with no further
// note_client_activity() call and even past the interval.
TEST_CASE("TelescopeDriver watchdog - an in-flight request is never treated as silence",
          "[telescope][watchdog][unit]") {
    WatchdogUnitStubDriver driver;
    driver.slewing.store(true);

    const auto t0 = std::chrono::steady_clock::now();
    driver.note_client_activity(t0);
    driver.begin_client_request();

    // Ticks well past the interval, with the request still in flight and no
    // further note_client_activity() -- must never trip.
    CHECK_FALSE(driver.stop_motion_if_client_silent(t0 + std::chrono::seconds(31), std::chrono::seconds(30)));
    CHECK_FALSE(driver.stop_motion_if_client_silent(t0 + std::chrono::seconds(62), std::chrono::seconds(30)));
    CHECK(driver.aborts.load() == 0);

    // Once the request completes, silence starts counting again from here.
    driver.end_client_request();
    CHECK_FALSE(driver.stop_motion_if_client_silent(t0 + std::chrono::seconds(63), std::chrono::seconds(30)));
    CHECK(driver.stop_motion_if_client_silent(t0 + std::chrono::seconds(93), std::chrono::seconds(30)));
    CHECK(driver.aborts.load() == 1);
}
