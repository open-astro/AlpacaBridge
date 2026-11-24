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

#include <alpacacore/async_connectable.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/client_utc_warning.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/findhome_sensor_search.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/motion_guards.h>
#include <alpacacore/util/motion_owner.h>
#include <alpacacore/util/pointing.h>
#include <alpacacore/util/poll_settle.h>
#include <alpacacore/util/polled_state.h>
#include <alpacacore/util/telescope_command_gate.h>
#include <alpacacore/vendor/celestron/celestron_cgx_geometry.h>
#include <alpacacore/vendor/celestron/celestron_cgx_protocol_wrapper.h>
#include <alpacacore/vendor/celestron/celestron_cgx_telescope_driver.h>
#include <alpacacore/version.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <future>
#include <initializer_list>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace alpacacore::vendor::celestron {

namespace {

constexpr double kSiderealDegreesPerSecond = 15.0411 / 3600.0;
// The CGX's argument limits for the command gate (#2258). The state checks (interleaved with site, motor-model and
// motion checks) and MoveAxis (rate ignored, not implemented) stay the driver's own.
constexpr util::TelescopeCommandLimits kCommandLimits = [] {
    util::TelescopeCommandLimits limits;
    limits.pulse_guide_max_ms = 600000;
    limits.texts.pulse_direction = limits.texts.pulse_duration = "PulseGuide direction or duration is out of range";
    limits.guide_rate_ra_min = limits.guide_rate_dec_min = kSiderealDegreesPerSecond * 0.1;
    limits.guide_rate_ra_max = limits.guide_rate_dec_max = kSiderealDegreesPerSecond;
    limits.texts.guide_rate_not_finite = limits.texts.guide_rate_range =
        "Guide rates must be between 10% and 100% of sidereal";
    limits.tracking_rate_max = 2;
    limits.texts.tracking_rate = "TrackingRate must be Sidereal, Lunar, or Solar";
    return limits;
}();
constexpr double kSlewArrivalToleranceDegrees = 3.0 / 3600.0;
constexpr double kMaxSlewCorrectionDegrees = 1.0;
constexpr int kMaxSlewCorrections = 3;
// #2640: the most an axis may move away from its GOTO target, in encoder counts (2000, about 0.043 degrees or 155"),
// before the leg is stopped. The controller does not always take the shorter way around the 24-bit circle: from home
// the rig drove Dec the long way, over the pole, to a correct Normal-side target (HA +0.5 h, Dec -10), 506 counts the
// wrong way at the 0.2 s read and about 6,500 at 0.4 s. A leg that goes the right way only closes on its target, so
// the growth this allows is encoder noise (two counts) and any overshoot at the end of a leg, which the rig's SLOW
// corrections kept within about 0.5" (7 counts); 2000 counts leaves room for an unmeasured approach overshoot and
// still stops a wrong-way FAST leg at its second 200 ms poll.
constexpr std::int32_t kGotoWrongWayCounts = 2000;
// How far an axis may coast away from a new GOTO target before it turns, when the GOTO is sent while a slew, Park or
// FindHome still drives the axes (a retarget): 5 degrees. The controller cannot reverse an axis at once, and failure
// 0014 recorded Dec coasting 2.4 degrees in about 1.26 s while it decelerated from full rate. Until the axis first
// closes on the new target its distance may grow this much past its first read; from then on kGotoWrongWayCounts
// applies past the farthest point of the coast.
constexpr std::int32_t kGotoCoastCounts = static_cast<std::int32_t>(kCgxEncoderStepsPerRevolution / 72);
// The most a correction is aimed ahead of the sky (#2541). The lead is the previous correction's aim-to-tracking time,
// travel included, so a long correction would aim the next, short one far past its target. A short correction over
// WiFi read its arrival 140-270 ms after the aim (rig 2026-11-21), so this leaves those leads unchanged.
constexpr std::chrono::milliseconds kMaxCorrectionLead{300};
constexpr std::chrono::milliseconds kUtcStepTolerance{1000};
constexpr std::chrono::seconds kHostDisciplineResampleInterval{30};
// The state poll (decision 0010): 250 ms start to start, three failed polls latch the fault, and a frame older
// than 1 s reads stale (served all the same).
constexpr std::chrono::milliseconds kStatePollPeriod{250};
constexpr int kStatePollFaultThreshold = 3;
constexpr std::chrono::seconds kStatePollMaxAge{1};
// The longest MC_AUX_GUIDE packet (255 centiseconds), and the margin a slew, Park or FindHome gives the mount past it
// to report the packet a pulse it superseded left running inactive (#2263); the pulse body's own segment deadline
// uses the same margin.
constexpr std::chrono::milliseconds kGuidePacketMax{2550};
constexpr std::chrono::milliseconds kGuideEndMargin{500};
// A home search not done by then missed the sensor (#2213): on the rig a normal search ended within about 30 s, and a
// missed one fell back to a crawl that needed about 420 s, so the axis is stopped and re-homed once from where it is.
constexpr std::chrono::seconds kHomeSearchBound{60};
// #2640: how long an axis may read MC_LEVEL_DONE done after its MC_LEVEL_START before the home counts as one that ran
// no search. A search reads searching from its start: on the rig a home from the home position itself took about 2 s
// (docs/celestron-cgx-usb-motion.md), and over WiFi a lost MC_LEVEL_START reply is confirmed by MC_LEVEL_DONE reading
// searching 300 ms after it went out (decision 0011 rule 2).
constexpr std::chrono::milliseconds kHomeStartConfirmWindow{500};
// The home travel guard (docs/failures/0016-cgx-home-travel.md): each axis's encoder travel during one FindHome, Park
// or meridian home route, summed across the #2213 re-home. No legitimate home found needs more than about 230 deg
// (a direct home from the farthest slew position is 180 deg Dec; the #2213 rig miss with its re-home about 179 deg),
// and no home needs a full turn, so an axis past 270 deg is a runaway. A re-home is sent only while the axis is at
// 90 deg or less, leaving it the half turn any home can need: the #2213 miss had gone at most 83 deg at 60 s, while
// the Oct-28 shape (a fast wrong-way search that never finds the switch) had gone about 190 deg.
// The guard's first sample is its first supervise_home_search() call, after a LEVEL_DONE poll, so the few hundred ms of
// travel between MC_LEVEL_START and it are not counted (#2541). That is noise against 270 deg; a bound tight enough
// for it to matter must take the first sample at MC_LEVEL_START.
constexpr double kHomeTravelBoundDegrees = 270.0;
constexpr double kHomeRehomeTravelLimitDegrees = kHomeTravelBoundDegrees - 180.0;
// Added to every stop verification budget over TCP (slice 3): one WiFi status poll of both axes with a recovered
// re-send. USB keeps its 500 ms cleanup and 3 s AbortSlew budgets.
constexpr std::chrono::milliseconds kTcpStopVerifyAllowance{600};

// The raw encoder pair the poll or a control path measured, and the host time it was read at. Sky values are
// computed when a getter reads it, from the UTC model, site and mechanical hemisphere (served_sky_position_locked()).
struct CgxFrame {
    std::uint32_t ra_encoder = 0;
    std::uint32_t dec_encoder = 0;
    std::chrono::system_clock::time_point read_at{};
    std::uint64_t poll_sequence = 0;  // the poll that read the pair; a control path's write_frame() keeps it
};

double configured_site_coordinate(const std::optional<double>& coordinate, const char* name) {
    if (!coordinate) {
        throw AlpacaException(std::string("Celestron CGX ") + name + " is not configured",
                              AlpacaError::InvalidOperation);
    }
    return coordinate.value();
}

std::uint32_t offset_encoder_label(std::uint32_t label, std::int32_t delta) {
    return (label + static_cast<std::uint32_t>(delta)) & (kCgxEncoderStepsPerRevolution - 1);
}

void require_cgx_ack(const AuxFrame& response, const char* operation) {
    if (!response.payload.empty()) {
        throw AlpacaException(std::string("Celestron CGX ") + operation + " returned an unexpected payload",
                              AlpacaError::DriverException);
    }
}

[[noreturn]] void throw_missing_cgx_home_switches() {
    throw AlpacaException(
        "Celestron CGX/CGX-L home switches were not found on the AUX bus; verify the mount model and direct USB port",
        AlpacaError::DriverException);
}

}  // namespace

// Lock order (decision 0016): transition_mutex_ -> motion_start_mutex_ -> the protocol wrapper's I/O lock -> mutex_
// (leaf of the driver's own locks) -> firmware_mutex_; the AsyncOperation slot mutexes, the PolledState mutexes and
// the LinkLostStamp mutex are leaves under mutex_ too. Every AUX exchange runs with mutex_ free, and no getter touches
// the wire: the coordinate getters serve the state poll's frame (decision 0010). motion_start_mutex_ is the
// motion-ownership lock: motion starts and stages, PulseGuide, Sync, the tracking writes and the disconnect stop hold
// it across their exchanges, so ownership, the site and the reference cannot change under them; no getter takes it.
// firmware_mutex_ guards the firmware string alone; Connect publishes it under mutex_.
class CelestronCgxTelescopeDriver final : public TelescopeDriver, protected AsyncConnectable {
public:
    ALPACA_EXPOSE_CONNECT_ERROR()

    CelestronCgxTelescopeDriver(int device_number, CelestronCgxConnectionInfo info, util::TaskClock& clock,
                                CgxSystemNow system_now)
        : AsyncConnectable(info.host.empty() ? "Celestron CGX USB" : "Celestron CGX TCP"),
          device_number_(device_number),
          tcp_(!info.host.empty()),
          info_(std::move(info)),
          clock_(clock),
          system_now_(std::move(system_now)),
          motion_slot_("Celestron CGX motion", motion_slot_generation_, clock),
          pulse_slots_{{"Celestron CGX pulse guide (RA)", pulse_generations_[0], clock},
                       {"Celestron CGX pulse guide (Dec)", pulse_generations_[1], clock}},
          stop_slot_("Celestron CGX stop", stop_generation_, clock),
          mechanical_hemisphere_(info_.site_latitude_degrees.value_or(0.0) < 0.0 ? CgxHemisphere::South
                                                                                 : CgxHemisphere::North),
          aperture_diameter_m_(info_.aperture_diameter_meters),
          aperture_area_m2_(std::numbers::pi * info_.aperture_diameter_meters * info_.aperture_diameter_meters / 4.0),
          focal_length_m_(info_.focal_length_meters),
          polled_(
              clock_, poll_generation_, {kStatePollPeriod, kStatePollFaultThreshold, kStatePollMaxAge, "Celestron CGX"},
              [this] { return probe(); }, [this] { release_lost_link(); }) {
        if (!system_now_) system_now_ = [] { return std::chrono::system_clock::now(); };
        guide_rate_ = {kSiderealDegreesPerSecond * 0.5, kSiderealDegreesPerSecond * 0.5};
    }

    ~CelestronCgxTelescopeDriver() override {
        shutdown_connection();
        polled_.stop_and_join();
        stop_slot_.cancel_all_and_join();
        motion_slot_.cancel_all_and_join();
        for (auto& slot : pulse_slots_) slot.cancel_all_and_join();
        if (connected_.load()) {
            try {
                set_connected(false);
            } catch (...) {
                ALPACA_LOG_WARN("Celestron CGX", "Disconnect failed during destruction");
            }
        }
    }

    std::optional<std::string> get_device_firmware() const override {
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        if (firmware_version_.empty()) return std::nullopt;
        return firmware_version_;
    }

    int get_device_number() const override { return device_number_; }
    std::string get_name() const override {
        return unknown_motor_model_.load() ? "Celestron Telescope" : "Celestron CGX/CGX-L";
    }
    DeviceType get_device_type() const override { return DeviceType::Telescope; }
    std::string fallback_unique_id() const override { return "Celestron_CGX_" + std::to_string(device_number_); }
    std::string get_description() const override {
        if (!tcp_) return "Celestron CGX/CGX-L direct USB mount";
        return "Celestron CGX/CGX-L mount over SkyPortal WiFi (TCP); lost replies recovered: " +
               std::to_string(protocol_.recovered_retries());
    }
    std::string get_driver_info() const override { return "AlpacaCore Celestron CGX AUX Driver"; }
    std::string get_driver_version() const override { return alpacacore::kVersion; }
    int get_interface_version() const override { return 4; }
    // Lock-free. A poll fault standing for util::kLinkStalenessBound, or a pulled cable at once, loses the link
    // (decision 0009, link_lost()), and Connected reads false without a reconnect.
    bool get_connected() const override { return connected_.load() && !link_lost(); }
    // The one fault text (decision 0009 point 5): the wrapper's loss reason for a pulled cable, else the latched poll
    // fault, kept after a loss until Connect or Disconnect. The wrapper keeps its reason until the next Connect; a
    // Disconnect hides it with connected_.
    std::string get_link_fault() const override {
        if (connected_.load() && !protocol_.link_alive()) {
            auto lost = protocol_.link_fault();
            if (!lost.empty()) return lost;
        }
        auto fault = polled_.fault();
        if (!fault.empty()) return fault;
        return polled_.link_lost() ? "no answer to the state poll" : std::string();
    }
    void set_connected(bool connected) override;
    ALPACA_ASYNC_CONNECT_FORWARDERS()

    std::vector<std::string> get_supported_actions() const override {
        if (!get_connected()) return {};  // listed while connected, like every telescope action
        return {std::string(util::kFindHomeSensorSearchAction)};
    }
    std::string action(std::string_view, std::string_view) override;
    bool can_action(std::string_view action_name) const override {
        return get_connected() && util::is_findhome_sensor_search_action(action_name);
    }
    std::string command_blind(std::string_view, bool) override;
    bool command_bool(std::string_view, bool) override;
    std::string command_string(std::string_view, bool) override;

    AlignmentMode get_alignment_mode() const override;
    double get_altitude() const override;
    double get_aperture_diameter() const override {
        require_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return aperture_diameter_m_;
    }
    void set_aperture_diameter(double meters) override;
    double get_aperture_area() const override {
        require_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return aperture_area_m2_;
    }
    bool get_at_home() const override;
    bool get_at_park() const override;
    double get_azimuth() const override;
    bool get_can_find_home() const override { return true; }
    bool get_can_park() const override { return true; }
    bool get_can_pulse_guide() const override { return true; }
    bool get_is_pulse_guiding() const override;
    bool get_can_set_declination_rate() const override { return false; }
    bool get_can_set_guide_rates() const override { return true; }
    bool get_can_set_park() const override { return false; }
    bool get_can_set_pier_side() const override { return false; }
    bool get_can_set_right_ascension_rate() const override { return false; }
    bool get_can_set_tracking() const override { return true; }
    bool get_can_slew_alt_az() const override { return false; }
    bool get_can_slew_alt_az_async() const override { return false; }
    bool get_can_sync_alt_az() const override { return false; }
    bool get_can_slew() const override { return false; }
    bool get_can_slew_async() const override { return true; }
    bool get_can_sync() const override { return true; }
    bool get_can_unpark() const override { return true; }
    double get_declination() const override;
    double get_declination_rate() const override {
        require_connected();
        return 0.0;
    }
    void set_declination_rate(double) override;
    bool get_tracking() const override;
    void set_tracking(bool tracking) override;
    double get_focal_length() const override {
        require_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return focal_length_m_;
    }
    void set_focal_length(double meters) override;
    GuideRate get_guide_rate() const override;
    void set_guide_rate(const GuideRate&) override;
    void set_guide_rate_right_ascension(double value) override;
    void set_guide_rate_declination(double value) override;
    double get_right_ascension() const override;
    double get_right_ascension_rate() const override {
        require_connected();
        return 0.0;
    }
    void set_right_ascension_rate(double) override;
    int get_side_of_pier() const override;
    void set_side_of_pier(int) override;
    int get_destination_side_of_pier(double ra, double dec) const override;
    EquatorialSystem get_equatorial_system() const override;
    bool get_does_refraction() const override {
        require_connected();
        return false;
    }
    void set_does_refraction(bool) override;
    int get_slew_settle_time() const override {
        require_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return slew_settle_time_seconds_;
    }
    void set_slew_settle_time(int seconds) override;
    double get_sidereal_time() const override;
    double get_site_elevation() const override {
        require_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!info_.site_elevation_meters)
            throw AlpacaException("SiteElevation has not been set", AlpacaError::InvalidOperation);
        return *info_.site_elevation_meters;
    }
    void set_site_elevation(double elevation) override;
    double get_site_latitude() const override {
        require_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!info_.site_latitude_degrees)
            throw AlpacaException("SiteLatitude has not been set", AlpacaError::InvalidOperation);
        return *info_.site_latitude_degrees;
    }
    void set_site_latitude(double latitude) override;
    double get_site_longitude() const override {
        require_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!info_.site_longitude_degrees)
            throw AlpacaException("SiteLongitude has not been set", AlpacaError::InvalidOperation);
        return *info_.site_longitude_degrees;
    }
    void set_site_longitude(double longitude) override;
    bool get_slewing() const override;
    double get_target_declination() const override;
    void set_target_declination(double dec) override;
    double get_target_right_ascension() const override;
    void set_target_right_ascension(double ra) override;
    int get_tracking_rate() const override;
    void set_tracking_rate(int rate) override;
    std::vector<int> get_tracking_rates() const override { return {0, 1, 2}; }
    std::chrono::system_clock::time_point get_utc_date() const override;
    void set_utc_date(std::chrono::system_clock::time_point utc) override;
    void require_known_motor_model() const {
        if (unknown_motor_model_.load()) {
            throw AlpacaException(
                "This mount's motors report an unknown model, so CGX axis scaling is unverified; motion is refused",
                AlpacaError::InvalidOperation);
        }
    }
    void find_home() override;
    void park() override;
    void pulse_guide(int, int) override;
    void set_park() override;
    void slew_to_coordinates(double, double) override;
    void slew_to_coordinates_async(double ra, double dec) override;
    void slew_to_target() override;
    void slew_to_target_async() override;
    void sync_to_coordinates(double ra, double dec) override;
    void sync_to_target() override;
    void unpark() override;
    bool get_can_move_axis(int axis) const override;
    void move_axis(int axis, double rate) override;
    std::pair<double, double> get_axis_rate_range(int axis) const override;
    std::vector<std::pair<double, double>> get_axis_rate_ranges(int axis) const override;
    void abort_slew() override;
    void slew_to_alt_az(double, double) override;
    void slew_to_alt_az_async(double, double) override;
    void sync_to_alt_az(double, double) override;

    void set_state_poll_spawn_for_testing(std::function<std::thread(std::function<void()>)> spawn) {
        polled_.set_spawn_for_testing(std::move(spawn));
    }
    void set_state_poll_paused_for_testing(bool paused) { polled_.pause_for_testing(paused); }
    void poll_state_now_for_testing() { polled_.poll_now_for_testing(); }
    std::optional<util::TaskClock::clock::time_point> consume_link_lost_at_for_testing() {
        return link_lost_at_.consume();
    }
    void set_stop_worker_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_worker_hook_for_testing_ = std::move(hook);
    }
    void set_drain_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> lock(mutex_);
        drain_hook_for_testing_ = std::move(hook);
    }
    void set_pulse_poll_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> lock(mutex_);
        pulse_poll_hook_for_testing_ = std::move(hook);
    }
    void set_pulse_pre_lock_hook_for_testing(std::function<void(CgxPulsePreLockPoint)> hook) {
        std::lock_guard<std::mutex> lock(mutex_);
        pulse_pre_lock_hook_for_testing_ = std::move(hook);
    }
    void set_guide_end_read_hook_for_testing(std::function<void(CelestronCgxProtocol::Timeout)> hook) {
        std::lock_guard<std::mutex> lock(mutex_);
        guide_end_read_hook_for_testing_ = std::move(hook);
    }

protected:
    // The TimeStamp of DeviceState is when the frame was measured (decision 0010 rule 5).
    std::optional<std::chrono::nanoseconds> device_state_age() const override { return polled_.age(); }

private:
    enum class PulseGuideStart : std::uint8_t { Acknowledged, Ambiguous };

    // `supersede_motion`: a slew, Park or FindHome, which supersedes a slew, Park or FindHome in flight through its
    // own claim (#2263), and supersedes a running pulse without a join (decision 0006 rule 3), then waits for the
    // mount to report that pulse's guide packet inactive before it sends anything (wait_for_guide_end()). Every other
    // initiator (Sync, Tracking, TrackingRate) refuses while a slew, Park or FindHome runs, and cancels and joins the
    // pulse bodies, each waiting out its packet (#1156).
    class MotionTransition final {
    public:
        explicit MotionTransition(CelestronCgxTelescopeDriver& driver, bool supersede_motion = false);
        ~MotionTransition();
        MotionTransition(const MotionTransition&) = delete;
        MotionTransition& operator=(const MotionTransition&) = delete;

    private:
        CelestronCgxTelescopeDriver& driver_;
        // Own the entire handoff, including the mutex-released task joins and the wait on the mount's guide report.
        // Otherwise disconnect can see an empty slot whose body another initiator is still joining.
        std::unique_lock<std::mutex> connection_lock_;
        std::unique_lock<std::mutex> lock_;
    };

    // Runs a body's wire stage under motion_start_mutex_, the lock every claim is made under (this gate, not the
    // MotionTransition above, keeps a superseded or cancelled body silent), so a body that a newer
    // command superseded or a stop cancelled sends nothing once the stage starts (decision 0006 rule 6). An open
    // motion_transition_ does not stop it: a slew, Park or FindHome opens one while this body still owns the axes and
    // releases the lock for its wait on the guide packet before its claim, and may then be refused without claiming
    // (#2263). Every other transition refuses while a body runs, cancels it, or sets disconnecting_.
    template <typename Operation>
    bool run_motion_stage(util::OperationContext& context, const util::MotionClaim& claim, Operation&& operation) {
        std::lock_guard<std::mutex> lock(motion_start_mutex_);
        if (motion_stop_reason(context, claim) != util::StopReason::None || disconnecting_ || !connected_.load()) {
            return false;
        }
        std::forward<Operation>(operation)();
        return true;
    }

    // Why a body should stop (#2263): Superseded when a newer claim took one of its axes or a newer start replaced it
    // on its slot, otherwise the slot's or the owner's Cancelled, otherwise None. Superseded wins (decision 0006
    // rule 6). Reads atomics and the slot mutex only, so a body asks with or without mutex_.
    util::StopReason motion_stop_reason(const util::OperationContext& context, const util::MotionClaim& claim) const {
        const auto slot = context.stop_reason();
        const auto owner = motion_owner_.stop_reason(claim);
        if (slot == util::StopReason::Superseded || owner == util::StopReason::Superseded)
            return util::StopReason::Superseded;
        return slot != util::StopReason::None ? slot : owner;
    }
    bool superseded(const util::OperationContext& context, const util::MotionClaim& claim) const {
        return motion_stop_reason(context, claim) == util::StopReason::Superseded;
    }
    // A superseded body touches neither the axes nor the driver state: a throw it meets after a newer command took its
    // axes is logged at INFO and dropped, never kept as a failure of the command that owns them now.
    static void log_superseded_failure(const char* body, const std::exception_ptr& failure);
    // Ends a slew, Park or FindHome claim, so a PulseGuide is accepted again. Takes mutex_.
    void finish_slew_claim(const util::MotionClaim& claim) {
        std::lock_guard<std::mutex> lock(mutex_);
        motion_owner_.finished(claim);
    }
    // Ends a slew, Park or FindHome claim when its body returns or throws.
    class SlewClaimEnd final {
    public:
        SlewClaimEnd(CelestronCgxTelescopeDriver& driver, const util::MotionClaim& claim)
            : driver_(driver), claim_(claim) {}
        ~SlewClaimEnd() { driver_.finish_slew_claim(claim_); }
        SlewClaimEnd(const SlewClaimEnd&) = delete;
        SlewClaimEnd& operator=(const SlewClaimEnd&) = delete;

    private:
        CelestronCgxTelescopeDriver& driver_;
        const util::MotionClaim claim_;
    };
    static std::size_t pulse_index(CgxAxis axis) { return axis == CgxAxis::RightAscension ? 0 : 1; }
    // The pulse whose first packet's ACK was lost has watched that packet end: its axis takes a new pulse again.
    void end_ambiguous_pulse_start(std::size_t index) {
        std::lock_guard<std::mutex> lock(mutex_);
        pulse_start_ambiguous_[index] = false;
    }
    bool pulse_running() const { return pulse_slots_[0].running() || pulse_slots_[1].running(); }
    std::optional<std::string> pulse_failure() const {
        if (auto failure = pulse_slots_[0].last_failure()) return failure;
        return pulse_slots_[1].last_failure();
    }

    [[noreturn]] static void not_implemented(const char* member, bool property = false);
    void require_connected() const;
    void require_healthy_transport() const;
    // The decision 0011 guards every coordinate read passes, in order. Caller holds mutex_.
    void require_readable_locked() const;
    // The sky position of a raw encoder pair at the current UTC, site and mechanical hemisphere. Caller holds mutex_.
    // `age` shifts the sidereal time back: the sky position when a pair read `age` ago was measured.
    CgxSkyPosition sky_position_locked(std::uint32_t ra_encoder, std::uint32_t dec_encoder,
                                       std::chrono::system_clock::duration age = {}) const;
    // The coordinate getters: the guards, then the poll's frame (decision 0010; stale is served). Caller holds mutex_,
    // so a reference change and its frame write are seen together.
    CgxSkyPosition served_sky_position_locked() const;
    // A control path's own read: drops mutex_ for the axis exchanges, re-validates after them (decision 0016 rules 1
    // and 6) and writes the measured pair through to the frame. Every caller holds motion_start_mutex_, so ownership
    // and the reference cannot change while mutex_ is dropped; a failed exchange changes the session, which is checked.
    CgxSkyPosition read_sky_position(std::unique_lock<std::mutex>& lock, std::uint32_t* ra_encoder = nullptr,
                                     std::uint32_t* dec_encoder = nullptr) const;
    // A verified encoder pair a control path wrote or read, published to the frame at once (decision 0010 point 3).
    // Each write drops the poll in flight and keeps measured_at, so the frame still ages by the poll alone. A control
    // loop that called this every few milliseconds would starve the poll and, after util::kLinkStalenessBound, read
    // as a lost link mid-motion: keep the callers occasional (arrival, Sync, PulseGuide polarity, slew planning).
    // scripts/check_docs_drift.py check 26 pins these callers and read_sky_position()'s (#2035).
    void write_frame(std::uint32_t ra_encoder, std::uint32_t dec_encoder) const {
        polled_.write([ra_encoder, dec_encoder, read_at = system_now_()](CgxFrame& frame) {
            frame.ra_encoder = ra_encoder;
            frame.dec_encoder = dec_encoder;
            frame.read_at = read_at;
        });
    }
    // The poll's exchanges, on the poll thread with no driver lock held (decision 0016). A desynchronized stream is
    // flushed on the fd already held; resync never opens a port (decision 0009 rule 4) and never marks the link
    // healthy, only the next matched reply does. A pulled cable, met here or by a control path's exchange, latches
    // the loss now, so the poll ends in this tick and on_lost stamps it (decision 0009 rule 2a).
    CgxFrame probe() {
        try {
            if (!protocol_.is_healthy()) protocol_.resync();
            CgxFrame frame;
            frame.poll_sequence = ++poll_sequence_;
            frame.ra_encoder = protocol_.get_axis_position(CgxAxis::RightAscension);
            frame.read_at = system_now_();  // the RA encoder's read, the one tracking moves
            frame.dec_encoder = protocol_.get_axis_position(CgxAxis::Declination);
            return frame;
        } catch (...) {
            if (!protocol_.link_alive()) polled_.lose_link();
            throw;
        }
    }
    // Lost when the poll latched it (a fault past the staleness bound, or the probe's lose_link()) or when the
    // wrapper saw the port gone, in any exchange. Lock-free.
    bool link_lost() const { return polled_.link_lost() || !protocol_.link_alive(); }
    // The disconnect body, shared by Disconnect and a Connect over a lost link. Caller holds transition_mutex_.
    void teardown_locked();
    // A lost link (decision 0009 point 3): stamp it (the first detection wins), then release the fd and the port
    // claim. Only the client's Connect reopens it.
    void release_lost_link() {
        link_lost_at_.note(clock_.now());
        protocol_.disconnect();
    }
    // Releases mutex_ for a wire exchange and takes it back on scope exit, also when the exchange throws.
    class MutexRelease final {
    public:
        explicit MutexRelease(std::unique_lock<std::mutex>& lock) : lock_(lock) { lock_.unlock(); }
        ~MutexRelease() { lock_.lock(); }
        MutexRelease(const MutexRelease&) = delete;
        MutexRelease& operator=(const MutexRelease&) = delete;

    private:
        std::unique_lock<std::mutex>& lock_;
    };
    double current_utc_locked() const;
    std::chrono::system_clock::time_point utc_now_locked() const;
    bool client_utc_offset_survives_locked(std::chrono::system_clock::time_point system_now) const;
    void require_site_locked() const;
    void start_home_operation(bool park_after);
    void run_home_operation(util::OperationContext& context, const util::MotionClaim& claim, bool park_after);
    std::string home_axes_status();
    void log_home_wait(std::chrono::steady_clock::time_point& next_log);
    void start_slew_operation(double ra, double dec);
    // `axes_may_coast`: the GOTO was sent while a slew, Park or FindHome still drove the axes (#2640).
    void run_slew_operation(util::OperationContext& context, const util::MotionClaim& claim, double ra, double dec,
                            CgxPointingSide destination_side, std::pair<std::uint32_t, std::uint32_t> positions,
                            bool axes_may_coast);
    void run_flip_slew_operation(util::OperationContext& context, const util::MotionClaim& claim, double ra, double dec,
                                 CgxPointingSide destination_side);
    // The home searches of a FindHome, Park or meridian home route, per axis (RA, Dec): when the current search
    // started, whether it is the re-home, and whether the axis was seen home-done after the bound.
    struct HomeSearch {
        explicit HomeSearch(util::TaskClock::clock::time_point start) : started{start, start} {}
        std::array<util::TaskClock::clock::time_point, 2> started;
        std::array<bool, 2> rehomed{};
        std::array<bool, 2> done{};
        // The home travel guard: each axis's last encoder read and the travel summed from read to read, and the
        // poll_sequence of the poll frame it last took (or that was held at its own first read).
        std::array<std::optional<std::uint32_t>, 2> last_encoder;
        std::array<double, 2> travel_degrees{};
        std::optional<std::uint64_t> poll_sequence;
    };
    // Called by both home waits between their polls (#2213): an axis not home-done kHomeSearchBound after its
    // MC_LEVEL_START is stopped, waited still and sent MC_LEVEL_START once more; a re-home that misses the bound too,
    // or the deadline during the wait, throws `timeout_message` with home_axes_status(). First it takes both encoders
    // (the state poll's newest frame, else its own reads) and adds each axis's travel since the last reading: a sum
    // past kHomeTravelBoundDegrees, or a re-home due above kHomeRehomeTravelLimitDegrees, throws the travel-guard error
    // (the caller's catch stops the axes). The stop and the home send are run_motion_stage() stages on `claim`, and the
    // still-wait asks motion_stop_reason(), so a cancel or a supersede ends it sending nothing more; it then returns
    // early and the caller's loop handles the reason.
    void supervise_home_search(util::OperationContext& context, const util::MotionClaim& claim, HomeSearch& search,
                               util::TaskClock::clock::time_point deadline, const char* timeout_message);
    void publish_guide_end_frame(util::OperationContext& context, const util::MotionClaim& claim);
    // A slew, Park or FindHome's wait for the guide packet of each pulse it superseded (`axes`, RA then Dec): reads
    // the mount's MC_AUX_GUIDE_ACTIVE until it reports the packet inactive, never the pulse slot, bounded by
    // kGuidePacketMax plus kGuideEndMargin on the task clock; an AbortSlew or Unpark accepted meanwhile ends it.
    // Runs with no driver lock but transition_mutex_ held.
    void wait_for_guide_end(const std::array<bool, 2>& axes);
    // #2640: MC_LEVEL_DONE reads done whenever no search runs (home_done=1 on a stopped axis,
    // docs/failures/0014-cgx-stop-completion.md), so a home-done read proves a home only once the search was seen
    // running. Reads each of `axes` after its MC_LEVEL_START until it reports searching; an axis that reads done for
    // kHomeStartConfirmWindow ran no search, and this throws DriverException: the caller's catch stops the axes, and
    // no home label is written. `stopped` (a body's cancel or supersede) ends the wait early; false is returned then.
    bool confirm_home_search_started(std::initializer_list<CgxAxis> axes, const std::function<bool()>& stopped = {});
    void run_pulse_guide(util::OperationContext& context, const util::MotionClaim& claim, CgxAxis axis,
                         int velocity_percent, int first_chunk_centiseconds, int remaining_centiseconds,
                         const std::shared_future<PulseGuideStart>& start_result);
    // `require_still_encoders` (the explicit AbortSlew and Unpark stop, #2411): verified only once the status shows
    // both axes stopped and two encoder reads util::kStillSampleSpacing apart agree within util::kStillToleranceArcsec.
    void stop_axes_checked(std::chrono::milliseconds verification_budget = std::chrono::milliseconds(500),
                           bool require_still_encoders = false);
    // A cancelled or failed body's stop: skipped while an explicit stop owns cleanup, and once a newer command
    // superseded `claim` and owns the axes.
    void stop_motion_worker_checked(const util::MotionClaim& claim);
    void start_stop_operation(bool unpark);
    void run_stop_operation(util::OperationContext& context);
    // `lock` is the caller's held mutex_ lock: the tracking exchanges run with it dropped.
    void set_tracking_locked(bool tracking, std::unique_lock<std::mutex>& lock);
    void write_tracking_state_locked(bool tracking, int rate, std::unique_lock<std::mutex>& lock);
    // A GOTO leg's encoder target per axis (RA, Dec); nullopt for an axis the leg sent no GOTO.
    using GotoTargets = std::array<std::optional<std::uint32_t>, 2>;
    // `first_leg` is the GOTO the caller sent; each correction's targets replace it for the next wait.
    bool finish_slew_operation(util::OperationContext& context, const util::MotionClaim& claim,
                               util::TaskClock::clock::time_point deadline, double right_ascension_hours,
                               double declination_degrees, CgxPointingSide pointing_side, GotoTargets first_leg,
                               const char* timeout_message, bool first_leg_may_coast = false);
    // Waits for both axes to report the leg done. Each poll reads the position of every axis of `targets` still
    // moving: one whose shorter-way distance to its target has grown more than kGotoWrongWayCounts past the distance
    // at its first read went the wrong way (#2640), and this throws DriverException; the caller's catch stops it.
    // With `may_coast` (a GOTO sent while the axes still moved) an axis may first coast kGotoCoastCounts away.
    bool wait_for_slew_completion(util::OperationContext& context, const util::MotionClaim& claim,
                                  util::TaskClock::clock::time_point deadline, const char* timeout_message,
                                  const GotoTargets& targets,
                                  std::chrono::milliseconds poll_interval = std::chrono::milliseconds(200),
                                  bool may_coast = false);
    bool wait_for_slew_settle(util::OperationContext& context) const;
    void require_site_change_allowed_locked() const;
    void set_tracking_rate_locked(int rate, std::unique_lock<std::mutex>& lock);
    CgxHemisphere hemisphere_locked() const;
    bool reference_valid_locked() const {
        return calibrated_ && reference_generation_ == protocol_.session_generation();
    }
    std::pair<std::uint32_t, std::uint32_t> read_stopped_checkpoint();

    const int device_number_;
    const bool tcp_;  // SkyPortal WiFi (a configured host) rather than direct USB, for the device's lifetime
    CelestronCgxConnectionInfo info_;
    util::TaskClock& clock_;
    CgxSystemNow system_now_;
    mutable std::mutex mutex_;
    std::mutex transition_mutex_;
    mutable std::mutex motion_start_mutex_;
    mutable std::mutex firmware_mutex_;
    bool motion_transition_ = false;
    bool disconnecting_ = false;
    std::atomic<bool> connected_{false};
    std::atomic<bool> unknown_motor_model_{false};
    mutable CelestronCgxProtocol protocol_;
    // Who owns the axes (#2263; CONTEXT.md, "Cancelled vs superseded"): a slew, Park or FindHome claims both, a
    // PulseGuide its own, under motion_start_mutex_ and mutex_, and a stop cancels both. Each body asks it why to stop.
    util::MotionOwner motion_owner_;
    // The motion slot's own generation, which util::AsyncOperation needs; ownership is the owner's.
    util::OperationGeneration motion_slot_generation_;
    util::AsyncOperation motion_slot_;
    // One pulse slot per axis (RA, Dec), each with a generation of its own: a pulse supersedes the older pulse on its
    // axis and leaves the other axis's alone.
    std::array<util::OperationGeneration, 2> pulse_generations_;
    util::AsyncOperation pulse_slots_[2];
    // Per axis (RA, Dec): set while the pulse whose first packet's ACK was lost still watches that packet. A new pulse
    // on that axis is refused meanwhile: AUX has no transaction id, so the axis is not reused. Guarded by mutex_.
    std::array<bool, 2> pulse_start_ambiguous_{};
    util::OperationGeneration stop_generation_;
    util::AsyncOperation stop_slot_;
    std::atomic<bool> stop_pending_{false};  // Slewing reads stop progress before it takes mutex_ (#1402).
    // Set while a slew, Park or FindHome waits for the guide packet of a pulse it superseded (#2263): IsPulseGuiding
    // stays true until the mount reports the packet inactive, as it does while a pulse body watches its own packet.
    std::atomic<bool> guide_end_pending_{false};
    bool unpark_after_stop_ = false;
    // Test seam (set_stop_worker_hook_for_testing): runs at the top of stop_motion_worker_checked(), before it takes
    // motion_start_mutex_. Guarded by mutex_.
    std::function<void()> stop_worker_hook_for_testing_;
    // Test seam (set_drain_hook_for_testing): runs in MotionTransition's guide handoff (the join of the pulse
    // bodies, or a slew, Park or FindHome's wait on the mount), with motion_start_mutex_ released. Guarded by mutex_.
    std::function<void()> drain_hook_for_testing_;
    // Test seam (set_pulse_poll_hook_for_testing): runs on the pulse body's thread at the top of every guide poll,
    // before it reads its stop reason, with no driver lock held. Guarded by mutex_.
    std::function<void()> pulse_poll_hook_for_testing_;
    // Test seam (set_pulse_pre_lock_hook_for_testing): runs on the pulse body's thread just before it takes
    // motion_start_mutex_ for a guide poll's read or for publish_guide_end_frame(), with no driver lock held. Guarded
    // by mutex_.
    std::function<void(CgxPulsePreLockPoint)> pulse_pre_lock_hook_for_testing_;
    // Test seam (set_guide_end_read_hook_for_testing): runs on the initiator's thread in wait_for_guide_end() just
    // before each MC_AUX_GUIDE_ACTIVE read, passed that read's timeout, with transition_mutex_ held and no other driver
    // lock. Guarded by mutex_.
    std::function<void(CelestronCgxProtocol::Timeout)> guide_end_read_hook_for_testing_;
    template <typename... Args>
    void run_hook_for_testing(const std::function<void(Args...)>& seam, std::type_identity_t<Args>... args) {
        std::function<void(Args...)> hook;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hook = seam;
        }
        if (hook) hook(args...);
    }
    bool calibrated_ = false;
    std::uint64_t reference_generation_ = 0;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> disconnected_checkpoint_;
    std::string reference_firmware_;
    // FindHome establishes the mechanical frame; later site writes do not reverse it.
    CgxHemisphere mechanical_hemisphere_;
    bool needs_sync_ = false;
    bool sync_correction_active_ = false;
    // The pre-Sync pair, held while Sync writes and verifies the labels with mutex_ free: a poll in that window can
    // read a new RA label beside an old Dec label, so the getters serve this pair instead until Sync publishes the
    // verified one or fails. Guarded by mutex_. Its read_at is recorded but never shifts the position: Sync turns
    // Tracking off before the label writes, so the served age is zero while this pair is held.
    std::optional<CgxFrame> sync_served_pair_;
    bool motion_state_unverified_ = false;
    bool slew_in_progress_ = false;
    bool restore_tracking_after_slew_ = false;
    // Controller labels to restore after hardware homing. Sync shifts both
    // labels without moving the mount; retaining that shift preserves the
    // calibration when a meridian flip re-homes both axes.
    std::uint32_t ra_rehome_label_ = kCgxRaHomeEncoder;
    std::uint32_t dec_rehome_label_ = kCgxDecHomeEncoder;
    bool tracking_ = false;
    bool tracking_state_valid_ = true;
    bool parked_ = false;
    bool at_home_ = false;
    bool target_ra_set_ = false;
    bool target_dec_set_ = false;
    double target_ra_hours_ = 0.0;
    double target_dec_degrees_ = 0.0;
    double aperture_diameter_m_ = 0.0;
    double aperture_area_m2_ = 0.0;
    double focal_length_m_ = 0.0;
    mutable bool has_utc_offset_ = false;
    mutable std::chrono::system_clock::duration utc_offset_{};
    mutable std::chrono::system_clock::time_point utc_anchor_system_{};
    mutable util::TaskClock::clock::time_point utc_anchor_steady_{};
    mutable bool utc_offset_host_was_synchronized_ = false;
    mutable util::TaskClock::clock::time_point next_utc_discipline_resample_{};
    GuideRate guide_rate_{0.0, 0.0};
    int tracking_rate_ = 0;
    int slew_settle_time_seconds_ = 0;
    std::string firmware_version_;
    // When the link was lost (decision 0009 point 3): the first detection wins, and the next Connect consumes it.
    // Nothing reads the value yet (no #521 consumer).
    util::LinkLostStamp link_lost_at_;
    util::OperationGeneration poll_generation_;
    std::atomic<std::uint64_t> poll_sequence_{0};  // probe() only: numbers the poll's frames for the travel guard
    // The encoder pair the getters serve. Last: destroyed first, so its poll joins while protocol_, clock_ and the
    // stamp its callbacks use are alive.
    mutable util::PolledState<CgxFrame> polled_;
};

CelestronCgxTelescopeDriver::MotionTransition::MotionTransition(CelestronCgxTelescopeDriver& driver,
                                                                bool supersede_motion)
    : driver_(driver), connection_lock_(driver.transition_mutex_), lock_(driver.motion_start_mutex_) {
    driver_.require_connected();
    if (driver_.motion_transition_ || driver_.disconnecting_) {
        throw AlpacaException("Celestron CGX is changing motion ownership", AlpacaError::InvalidOperation);
    }
    if (driver_.stop_slot_.running()) {
        throw AlpacaException("Celestron CGX stop is still finishing", AlpacaError::InvalidOperation);
    }
    if (!supersede_motion && driver_.motion_slot_.running()) {
        throw AlpacaException("Celestron CGX is already moving", AlpacaError::InvalidOperation);
    }
    driver_.motion_transition_ = true;
    // An AUX guide packet cannot be interrupted, so the new command sends nothing while one runs (#1156).
    std::array<bool, 2> superseded_pulses{};
    // IsPulseGuiding stays true from the supersede until the wait below ends, on every exit: the woken body can end
    // its slot before this initiator reaches the wait (get_is_pulse_guiding()).
    struct GuideEndPending {
        std::atomic<bool>* pending = nullptr;
        ~GuideEndPending() {
            if (pending) pending->store(false);
        }
    } guide_end_pending;
    if (supersede_motion) {
        // A slew, Park or FindHome supersedes a running pulse without a join (decision 0006 rule 3, #2263): the bump
        // under motion_start_mutex_ marks the body Superseded, so it sends its axis nothing more, the next chunk, the
        // next guide poll and the end-of-pulse read included (each re-checks under that lock), and the cancel only
        // wakes it (Superseded wins). The packet it left on the mount is waited out below, on the mount's own report.
        std::lock_guard<std::mutex> state_lock(driver_.mutex_);
        for (std::size_t index = 0; index < 2; ++index)
            superseded_pulses[index] = driver_.pulse_slots_[index].running();
        if (superseded_pulses[0] || superseded_pulses[1]) {
            driver_.guide_end_pending_.store(true);  // before the bump, so no reader sees neither
            guide_end_pending.pending = &driver_.guide_end_pending_;
        }
        for (std::size_t index = 0; index < 2; ++index) {
            if (!superseded_pulses[index]) continue;
            static_cast<void>(driver_.pulse_generations_[index].bump());
            driver_.pulse_slots_[index].cancel();
        }
    } else {
        // Sync, Tracking and TrackingRate are not motion commands: they cancel and join the pulse bodies, each
        // waiting out its active <=2.55 s packet.
        for (auto& slot : driver_.pulse_slots_) slot.cancel();
    }
    lock_.unlock();
    try {
        driver_.run_hook_for_testing(driver_.drain_hook_for_testing_);
        if (supersede_motion) {
            driver_.wait_for_guide_end(superseded_pulses);
        } else {
            for (auto& slot : driver_.pulse_slots_) slot.cancel_all_and_join();
        }
    } catch (...) {
        lock_.lock();
        std::lock_guard<std::mutex> state_lock(driver_.mutex_);
        driver_.motion_transition_ = driver_.stop_pending_;
        throw;
    }
    lock_.lock();
    {
        std::lock_guard<std::mutex> state_lock(driver_.mutex_);
        // A stop accepted during the join owns cleanup; never dispatch the displaced command.
        if (driver_.stop_pending_)
            throw AlpacaException("Celestron CGX motion request interrupted by AbortSlew or Unpark",
                                  AlpacaError::InvalidOperation);
    }
    if (const auto failure = driver_.pulse_failure()) {
        driver_.motion_transition_ = false;
        throw AlpacaException("Celestron CGX pulse guide did not finish cleanly: " + *failure,
                              AlpacaError::DriverException);
    }
}

void CelestronCgxTelescopeDriver::log_superseded_failure(const char* body, const std::exception_ptr& failure) {
    std::string reason;
    try {
        std::rethrow_exception(failure);
    } catch (const std::exception& error) {
        reason = error.what();
    } catch (...) {
        reason = "unknown exception";
    }
    ALPACA_LOG_INFO("Celestron CGX", std::string(body) + " ended after a newer command took its axes: " + reason);
}

CelestronCgxTelescopeDriver::MotionTransition::~MotionTransition() { driver_.motion_transition_ = false; }

void CelestronCgxTelescopeDriver::wait_for_guide_end(const std::array<bool, 2>& axes) {
    if (!axes[0] && !axes[1]) return;
    // MotionTransition holds guide_end_pending_ true across this wait, so IsPulseGuiding stays true until the mount
    // reports the packet inactive (get_is_pulse_guiding()).
    const auto bound = kGuidePacketMax + kGuideEndMargin;
    // One deadline for both axes, on purpose: both packets started before the supersede, so neither outlives the
    // longest packet plus the margin from now; a per-axis deadline would let two packets wait twice as long.
    const auto deadline = clock_.now() + bound;
    for (const auto axis : {CgxAxis::RightAscension, CgxAxis::Declination}) {
        if (!axes[pulse_index(axis)]) continue;
        // A timeout on the RA axis throws before Dec is read: the command is refused either way, so a Dec read
        // would only spend time. A stop accepted meanwhile owns cleanup, and MotionTransition refuses the displaced
        // command. The deadline is checked before each read, and the read gets only the time left, so a read that
        // blocks near its I/O timeout cannot carry the wait past the bound.
        const auto timed_out = [&] {
            return AlpacaException(
                std::string("Celestron CGX still reports a ") + (axis == CgxAxis::RightAscension ? "RA" : "Dec") +
                    " guide packet active after " + std::to_string(bound.count()) + " ms; nothing was sent",
                AlpacaError::DriverException);
        };
        while (!stop_pending_.load()) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock_.now());
            if (remaining <= std::chrono::milliseconds::zero()) throw timed_out();
            run_hook_for_testing(guide_end_read_hook_for_testing_, CelestronCgxProtocol::Timeout{remaining});
            if (!protocol_.is_aux_guide_active(axis, remaining)) break;
            if (clock_.now() >= deadline) throw timed_out();
            clock_.sleep_for(std::chrono::milliseconds(20));
        }
    }
}

[[noreturn]] void CelestronCgxTelescopeDriver::not_implemented(const char* member, bool property) {
    throw AlpacaException(std::string("Celestron CGX USB does not support ") + member,
                          property ? AlpacaError::PropertyNotImplemented : AlpacaError::MethodNotImplemented);
}

void CelestronCgxTelescopeDriver::require_connected() const {
    if (!connected_.load()) {
        throw AlpacaException(tcp_ ? "Celestron CGX TCP is not connected" : "Celestron CGX USB is not connected",
                              AlpacaError::NotConnected);
    }
    if (link_lost()) {
        throw AlpacaException("Celestron CGX link lost; set Connected false then true to reconnect",
                              AlpacaError::NotConnected);
    }
}

void CelestronCgxTelescopeDriver::require_healthy_transport() const {
    if (!protocol_.is_healthy())
        throw AlpacaException("Celestron CGX communications compromised; verify the link and confirm a stop",
                              AlpacaError::DriverException);
}

// The one time source (decision 0001, #2166): UTCDate, SiderealTime and pointing all read it, so a
// client can explain SiderealTime from its own UTCDate and SiteLongitude reads. The motors have no
// clock: this is the host clock, plus a client's UTCDate offset only while the host is undisciplined.
std::chrono::system_clock::time_point CelestronCgxTelescopeDriver::utc_now_locked() const {
    const auto system_now = system_now_();
    const bool offset_survives = client_utc_offset_survives_locked(system_now);
    if (offset_survives && !utc_offset_host_was_synchronized_) {
        const auto steady_now = clock_.now();
        if (steady_now >= next_utc_discipline_resample_) {
            next_utc_discipline_resample_ = steady_now + kHostDisciplineResampleInterval;
            if (util::ClientUtcWarning::host_synchronized()) {
                utc_offset_host_was_synchronized_ = true;
                ALPACA_LOG_INFO("Celestron CGX",
                                "Host clock became NTP-disciplined after UTCDate; UTCDate, SiderealTime and "
                                "pointing now use the host clock");
            }
        }
    }
    return util::HostClock::pointing_uses_client_offset(offset_survives, utc_offset_host_was_synchronized_)
               ? system_now + utc_offset_
               : system_now;
}

double CelestronCgxTelescopeDriver::current_utc_locked() const {
    return std::chrono::duration<double>(utc_now_locked().time_since_epoch()).count();
}

bool CelestronCgxTelescopeDriver::client_utc_offset_survives_locked(
    std::chrono::system_clock::time_point system_now) const {
    if (!has_utc_offset_) return false;
    const auto system_elapsed = system_now - utc_anchor_system_;
    const auto steady_elapsed = clock_.now() - utc_anchor_steady_;
    if (util::HostClock::clock_was_stepped(system_elapsed, steady_elapsed, kUtcStepTolerance)) {
        has_utc_offset_ = false;
        utc_offset_ = {};
        ALPACA_LOG_INFO("Celestron CGX", "Host clock was stepped after UTCDate; dropping the client time offset");
        return false;
    }
    return true;
}

// ADR 0019 rule 3: the CGX controller stores no site, so Connect works without one and every member
// that reads the hemisphere or the longitude refuses until both coordinates are set.
void CelestronCgxTelescopeDriver::require_site_locked() const {
    if (!info_.site_latitude_degrees || !info_.site_longitude_degrees) {
        throw AlpacaException("Celestron CGX needs SiteLatitude and SiteLongitude to be set before this member works",
                              AlpacaError::InvalidOperation);
    }
}

CgxHemisphere CelestronCgxTelescopeDriver::hemisphere_locked() const { return mechanical_hemisphere_; }

void CelestronCgxTelescopeDriver::require_readable_locked() const {
    require_connected();
    require_site_locked();
    require_healthy_transport();  // A known fault refuses even a frame measured before it.
    if (stop_pending_)
        throw AlpacaException("Celestron CGX stop is still pending verification", AlpacaError::DriverException);
    if (motion_state_unverified_)
        throw AlpacaException("Celestron CGX motion may still be active; confirm a stop before reading coordinates",
                              AlpacaError::DriverException);
    if (!reference_valid_locked()) {
        throw AlpacaException(needs_sync_ ? "Celestron CGX pointing reference needs SyncToCoordinates"
                                          : "Celestron CGX encoder reference is unknown; run FindHome",
                              AlpacaError::DriverException);
    }
}

CgxSkyPosition CelestronCgxTelescopeDriver::sky_position_locked(std::uint32_t ra_encoder, std::uint32_t dec_encoder,
                                                                std::chrono::system_clock::duration age) const {
    const double longitude = configured_site_coordinate(info_.site_longitude_degrees, "SiteLongitude");
    const auto utc =
        std::chrono::system_clock::time_point{std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::duration<double>(current_utc_locked()))} -
        age;
    return cgx_encoder_position_to_sky(ra_encoder, dec_encoder, cgx_local_sidereal_time_hours(utc, longitude),
                                       hemisphere_locked());
}

CgxSkyPosition CelestronCgxTelescopeDriver::served_sky_position_locked() const {
    require_readable_locked();
    auto frame = polled_.served().value;  // the fault and loss checks run even while Sync holds a pair
    if (sync_served_pair_) frame = *sync_served_pair_;
    // A frame is up to a poll period old. While tracking, the encoders follow the sky, so the position is the one
    // when the pair was read: compute at that time (the UTC model applied to the host time of the read). Otherwise
    // the encoders stand still and the sky moves past them, so the position is the pair's at the current time.
    const auto age = tracking_ ? system_now_() - frame.read_at : std::chrono::system_clock::duration{};
    return sky_position_locked(frame.ra_encoder, frame.dec_encoder, age);
}

CgxSkyPosition CelestronCgxTelescopeDriver::read_sky_position(std::unique_lock<std::mutex>& lock,
                                                              std::uint32_t* ra_encoder,
                                                              std::uint32_t* dec_encoder) const {
    require_readable_locked();
    const auto session = protocol_.session_generation();
    std::uint32_t ra;
    std::uint32_t dec;
    {
        const MutexRelease unlocked(lock);  // The I/O lock is never taken under mutex_.
        // Dec first: while tracking, RA moves and Dec does not, so the RA count read last is the one closest to the
        // UTC the position is computed at. Read first, it lagged by the Dec exchange: ~0.3" at 21 ms over WiFi.
        dec = protocol_.get_axis_position(CgxAxis::Declination);
        ra = protocol_.get_axis_position(CgxAxis::RightAscension);
    }
    // The pair may be from a session that no longer exists: re-validate.
    require_readable_locked();
    if (session != protocol_.session_generation())
        throw AlpacaException("Celestron CGX session changed while reading coordinates", AlpacaError::DriverException);
    write_frame(ra, dec);
    if (ra_encoder != nullptr) *ra_encoder = ra;
    if (dec_encoder != nullptr) *dec_encoder = dec;
    return sky_position_locked(ra, dec);
}

std::pair<std::uint32_t, std::uint32_t> CelestronCgxTelescopeDriver::read_stopped_checkpoint() {
    // Every exchange shares the whole-check budget.
    const auto deadline = clock_.now() + std::chrono::milliseconds(500);
    const auto budget = [&] {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock_.now());
        if (remaining <= std::chrono::milliseconds::zero())
            throw AlpacaException("Celestron CGX reference continuity check timed out", AlpacaError::DriverException);
        return remaining;
    };
    for (const auto axis : {CgxAxis::RightAscension, CgxAxis::Declination}) {
        if (!protocol_.is_axis_slew_done(axis, budget()) || !protocol_.is_axis_home_done(axis, budget()) ||
            protocol_.is_aux_guide_active(axis, budget()))
            throw AlpacaException("Celestron CGX reference continuity check found unexpected motion",
                                  AlpacaError::DriverException);
    }
    util::ConsecutiveSettle settle(3, 15);
    std::optional<std::pair<std::uint32_t, std::uint32_t>> previous;
    while (true) {
        const auto ra = protocol_.get_axis_position(CgxAxis::RightAscension, budget());
        const auto dec = protocol_.get_axis_position(CgxAxis::Declination, budget());
        const auto last = previous.value_or(std::pair{ra, dec});
        const bool stable = previous.has_value() && std::abs(signed_cgx_encoder_delta(ra, last.first)) <= 2 &&
                            std::abs(signed_cgx_encoder_delta(dec, last.second)) <= 2;
        previous = std::pair{ra, dec};
        const auto state = settle.feed(stable);
        if (state == util::ConsecutiveSettle::State::Settled) return {ra, dec};
        if (state == util::ConsecutiveSettle::State::TimedOut)
            throw AlpacaException("Celestron CGX encoders did not settle for reference continuity",
                                  AlpacaError::DriverException);
        clock_.sleep_for(std::chrono::milliseconds(20));
    }
}

void CelestronCgxTelescopeDriver::set_connected(bool connected) {
    std::lock_guard<std::mutex> transition(transition_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // A lost link counts as disconnected for a connect: Connected=true against it reconnects instead of taking
        // the idempotency return (decision 0009 rule 4). The disconnect gates take the raw connected_, so
        // Connected=false still tears a lost link down.
        const bool live = connected_.load() && !link_lost();
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) return;
        if (connected && consume_pending_disconnect(live)) return;
        if (connected ? live : !connected_.load()) return;
    }
    if (!connected) {
        teardown_locked();
        return;
    }

    // A connect over a lost link tears the old session down first: its fd is released, so no stop or checkpoint
    // runs and the reference is dropped (decision 0011 rule 3: FindHome again).
    if (connected_.load()) teardown_locked();
    (void)link_lost_at_.consume();  // the relink consumes the loss stamp; nothing times motion by it yet
    // The one transport call site: a later beacon re-find of a silent adapter (station mode) wraps it.
    if (tcp_)
        protocol_.connect_tcp(info_.host, info_.tcp_port);
    else
        protocol_.connect_serial(info_.port_path);
    try {
        // The exchanges here and below take the transport's timeout: 500 ms over USB, 1000 ms over TCP.
        const AuxFrame ra_version =
            protocol_.exchange({0x00, static_cast<std::uint8_t>(CgxAxis::RightAscension), 0xfe, {}});
        const AuxFrame dec_version =
            protocol_.exchange({0x00, static_cast<std::uint8_t>(CgxAxis::Declination), 0xfe, {}});
        const bool modern_version = ra_version.payload.size() == 4 && dec_version.payload.size() == 4;
        if (!modern_version && (ra_version.payload.size() != 2 || dec_version.payload.size() != 2)) {
            throw AlpacaException("Celestron CGX motor-controller identity response is malformed",
                                  AlpacaError::DriverException);
        }
        if (modern_version) {
            // Raw AUX CGX 7.17.25 returns major/minor + a big-endian build.
            // MC_GET_MODEL identifies the motors: 0x1788 is the verified CGX, and 0x18xx the CGX-L (INDI
            // celestronaux formatModelString, unqualified here), which has the CGX's mechanics and 2^24-count axes.
            // A reply from each assumed switch address is not a valid identity gate. Encoder scaling and home
            // positions are the CGX's constants (#2204): another motor, or a pair of two families, is added, but
            // require_known_motor_model() refuses to drive it with them.
            std::vector<std::vector<std::uint8_t>> models;
            for (const auto axis : {CgxAxis::RightAscension, CgxAxis::Declination}) {
                const AuxFrame model = protocol_.exchange({0x00, static_cast<std::uint8_t>(axis), 0x05, {}});
                if (model.payload.size() != 2) {
                    throw AlpacaException("Celestron CGX motor model reply is malformed", AlpacaError::DriverException);
                }
                models.push_back(model.payload);
            }
            const auto family = [](const std::vector<std::uint8_t>& model) {
                return model == std::vector<std::uint8_t>{0x17, 0x88} ? 0x17 : model[0] == 0x18 ? 0x18 : 0;
            };
            const auto id = [](const std::vector<std::uint8_t>& model) {
                std::array<char, 8> text{};
                std::snprintf(text.data(), text.size(), "0x%02x%02x", model[0], model[1]);
                return std::string(text.data());
            };
            const std::string reported = "RA " + id(models[0]) + ", Dec " + id(models[1]);
            const bool unknown_motor = family(models[0]) == 0 || family(models[0]) != family(models[1]);
            if (unknown_motor) {
                ALPACA_LOG_WARN("Celestron CGX", "Motors report model " + reported +
                                                     ", not a CGX or CGX-L pair; connected, "
                                                     "motion refused");
            } else if (family(models[0]) == 0x18) {
                ALPACA_LOG_INFO("Celestron CGX", "Motors report CGX-L model " + reported +
                                                     "; driven as the CGX, the CGX-L is not yet qualified on hardware");
            }
            unknown_motor_model_.store(unknown_motor);
        } else {
            const auto query_home_switch = [this](std::uint8_t address) {
                try {
                    return protocol_.exchange({0x00, address, 0xfe, {}});
                } catch (const std::exception&) {
                    throw_missing_cgx_home_switches();
                }
            };
            const AuxFrame ra_home_switch = query_home_switch(0x30);
            if (ra_home_switch.payload.size() != 2) throw_missing_cgx_home_switches();
            const AuxFrame dec_home_switch = query_home_switch(0x31);
            if (dec_home_switch.payload.size() != 2) throw_missing_cgx_home_switches();
        }
        bool motion_state_unverified;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            motion_state_unverified = motion_state_unverified_;
        }
        // Over TCP the adapter may have served another client (the SkyPortal app) since this driver last saw the
        // mount, so every Connect takes it over with a verified stop; USB stops only after unverified motion.
        if (motion_state_unverified || tcp_) stop_axes_checked();
        const auto format_version = [](const auto& bytes) {
            std::string version = std::to_string(bytes[0]) + "." + std::to_string(bytes[1]);
            if (bytes.size() == 4) {
                version += "." + std::to_string((static_cast<std::uint16_t>(bytes[2]) << 8) | bytes[3]);
            }
            return version;
        };
        const std::string firmware =
            "RA " + format_version(ra_version.payload) + ", Dec " + format_version(dec_version.payload);
        {
            std::lock_guard<std::mutex> motion_lock(motion_start_mutex_);
            motion_transition_ = false;
            disconnecting_ = false;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        // Disconnect joined every pulse body, so no packet is still unresolved on either axis.
        pulse_start_ambiguous_ = {};
        bool retained = reference_valid_locked() && disconnected_checkpoint_ && reference_firmware_ == firmware;
        if (retained) {
            const auto saved_checkpoint = *disconnected_checkpoint_;
            const auto reference_generation = reference_generation_;
            lock.unlock();
            const auto checkpoint = read_stopped_checkpoint();
            lock.lock();
            retained = reference_valid_locked() && reference_generation_ == reference_generation &&
                       std::abs(signed_cgx_encoder_delta(checkpoint.first, saved_checkpoint.first)) <= 2 &&
                       std::abs(signed_cgx_encoder_delta(checkpoint.second, saved_checkpoint.second)) <= 2;
        }
        if (!retained) {
            calibrated_ = false;
            needs_sync_ = false;
            sync_correction_active_ = false;
            at_home_ = false;
            parked_ = false;
            ra_rehome_label_ = kCgxRaHomeEncoder;
            dec_rehome_label_ = kCgxDecHomeEncoder;
        }
        disconnected_checkpoint_.reset();
        reference_firmware_ = firmware;
        slew_in_progress_ = false;
        stop_pending_ = false;
        unpark_after_stop_ = false;
        restore_tracking_after_slew_ = false;
        tracking_ = false;
        tracking_state_valid_ = true;
        has_utc_offset_ = false;
        utc_offset_ = {};
        utc_offset_host_was_synchronized_ = false;
        target_ra_set_ = false;
        target_dec_set_ = false;
        motion_slot_.clear_failure();
        for (auto& slot : pulse_slots_) slot.clear_failure();
        stop_slot_.clear_failure();
        lock.unlock();
        // The first frame before Connected reads true, so a getter has an answer when Connect returns; the poll
        // starts with no driver lock held (decision 0006 rule 4). A connect whose retry also publishes nothing
        // fails: Connected true means the getters have a frame to serve.
        polled_.reset();
        if (!polled_.start())
            throw AlpacaException("Celestron CGX did not answer the first state poll", AlpacaError::DriverException);
        connected_.store(true);
        {
            std::lock_guard<std::mutex> firmware_lock(firmware_mutex_);
            firmware_version_ = firmware;
        }
    } catch (...) {
        polled_.stop_and_join();
        polled_.reset();
        {
            std::lock_guard<std::mutex> lock(firmware_mutex_);
            firmware_version_.clear();
        }
        connected_.store(false);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            calibrated_ = false;
            disconnected_checkpoint_.reset();
        }
        protocol_.disconnect();
        // A cable pulled during an exchange whose failure the connect turns into another error (the first state
        // poll, a legacy home-switch query) fails the Connect with NotConnected (decision 0009 rule 2a).
        if (!protocol_.link_alive()) {
            throw AlpacaException("Celestron CGX connect failed: " + protocol_.link_fault(), AlpacaError::NotConnected);
        }
        throw;
    }
}

void CelestronCgxTelescopeDriver::teardown_locked() {
    // The poll ends before the stop and checkpoint exchanges, so it neither interleaves with them nor sees the close.
    polled_.stop_and_join();
    // A link lost by a frame that aged past the bound, or by a pulled cable a control path met, may not have reached
    // on_lost yet: stamp and release it here, so no stop or checkpoint runs on it.
    if (link_lost()) release_lost_link();

    std::unique_lock<std::mutex> motion_start_lock(motion_start_mutex_);
    disconnecting_ = true;
    motion_transition_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        motion_owner_.cancel(util::MotionOwner::kBothAxes);
    }
    motion_slot_.cancel();
    for (auto& slot : pulse_slots_) slot.cancel();
    motion_start_lock.unlock();
    stop_slot_.cancel_all_and_join();
    motion_slot_.cancel_all_and_join();
    for (auto& slot : pulse_slots_) slot.cancel_all_and_join();
    motion_start_lock.lock();
    std::exception_ptr stop_failure;
    if (protocol_.is_connected()) {
        try {
            stop_axes_checked();
            std::unique_lock<std::mutex> lock(mutex_);
            disconnected_checkpoint_.reset();
            // Over TCP the socket closes below, and a new socket is a new session: no checkpoint could be used.
            if (!tcp_ && reference_valid_locked()) {
                const auto reference_generation = reference_generation_;
                lock.unlock();
                const auto checkpoint = read_stopped_checkpoint();
                lock.lock();
                if (reference_valid_locked() && reference_generation_ == reference_generation)
                    disconnected_checkpoint_ = checkpoint;
            }
        } catch (...) {
            stop_failure = std::current_exception();
        }
    }
    {
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        firmware_version_.clear();
    }
    connected_.store(false);
    polled_.reset();  // after Connected reads false: no getter meets a connected driver without a frame
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_failure || !disconnected_checkpoint_ || !reference_valid_locked()) {
            calibrated_ = false;
            needs_sync_ = false;
            sync_correction_active_ = false;
            disconnected_checkpoint_.reset();
            ra_rehome_label_ = kCgxRaHomeEncoder;
            dec_rehome_label_ = kCgxDecHomeEncoder;
            parked_ = false;
            at_home_ = false;
        }
        stop_pending_ = false;
        unpark_after_stop_ = false;
        tracking_ = false;
        tracking_state_valid_ = true;
        has_utc_offset_ = false;
        utc_offset_ = {};
        utc_offset_host_was_synchronized_ = false;
        if (!stop_failure) {
            slew_in_progress_ = false;
            restore_tracking_after_slew_ = false;
        }
        target_ra_set_ = false;
        target_dec_set_ = false;
    }
    // Keep the exclusive fd for the lifetime of this configured device, even
    // without a reference. Only a failed stop/initialization, a lost link (the
    // poll's on_lost, decision 0009) and destruction release ownership. A TCP
    // socket closes on every disconnect: the adapter session (and its backlog)
    // is not trusted across clients, so the reference goes with it.
    if (stop_failure || tcp_) protocol_.disconnect();
    motion_slot_.clear_failure();
    for (auto& slot : pulse_slots_) slot.clear_failure();
    stop_slot_.clear_failure();
    motion_transition_ = false;
    disconnecting_ = false;
    if (stop_failure) std::rethrow_exception(stop_failure);
}

std::string CelestronCgxTelescopeDriver::action(std::string_view action_name, std::string_view) {
    if (util::is_findhome_sensor_search_action(action_name)) {
        // Connect refuses a CGX whose home switches do not answer on the AUX bus, and FindHome sends
        // MC_LEVEL_START, which drives each axis to its switch: a real sensor search whenever connected.
        require_connected();
        return util::format_findhome_sensor_search(true, util::kFindHomeSwitchSearch, "");
    }
    throw AlpacaException("Action not supported: " + std::string(action_name), AlpacaError::ActionNotImplemented);
}
std::string CelestronCgxTelescopeDriver::command_blind(std::string_view, bool) { not_implemented("CommandBlind"); }
bool CelestronCgxTelescopeDriver::command_bool(std::string_view, bool) { not_implemented("CommandBool"); }
std::string CelestronCgxTelescopeDriver::command_string(std::string_view, bool) { not_implemented("CommandString"); }

AlignmentMode CelestronCgxTelescopeDriver::get_alignment_mode() const {
    require_connected();
    return AlignmentMode::GermanPolar;
}
double CelestronCgxTelescopeDriver::get_altitude() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto sky = served_sky_position_locked();
    const double latitude = configured_site_coordinate(info_.site_latitude_degrees, "SiteLatitude");
    const auto utc =
        std::chrono::system_clock::time_point{std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::duration<double>(current_utc_locked()))};
    const double lst =
        cgx_local_sidereal_time_hours(utc, configured_site_coordinate(info_.site_longitude_degrees, "SiteLongitude"));
    const double ha = (lst - sky.right_ascension_hours) * std::numbers::pi / 12.0;
    const double dec = sky.declination_degrees * std::numbers::pi / 180.0;
    const double lat = latitude * std::numbers::pi / 180.0;
    return std::asin(
               std::clamp(std::sin(dec) * std::sin(lat) + std::cos(dec) * std::cos(lat) * std::cos(ha), -1.0, 1.0)) *
           180.0 / std::numbers::pi;
}
void CelestronCgxTelescopeDriver::set_aperture_diameter(double meters) {
    if (!std::isfinite(meters) || meters < 0.0)
        throw AlpacaException("Aperture diameter must be non-negative", AlpacaError::InvalidValue);
    std::lock_guard<std::mutex> lock(mutex_);
    require_connected();
    aperture_diameter_m_ = meters;
    aperture_area_m2_ = std::numbers::pi * meters * meters / 4.0;
}
bool CelestronCgxTelescopeDriver::get_at_home() const {
    require_connected();
    std::lock_guard<std::mutex> lock(mutex_);
    require_healthy_transport();
    if (at_home_ && !reference_valid_locked())
        throw AlpacaException("Celestron CGX home reference is unknown; run FindHome", AlpacaError::DriverException);
    if (motion_state_unverified_)
        throw AlpacaException("Celestron CGX motion stop has not been verified", AlpacaError::DriverException);
    return at_home_;
}
bool CelestronCgxTelescopeDriver::get_at_park() const {
    require_connected();
    std::lock_guard<std::mutex> lock(mutex_);
    require_healthy_transport();
    if (parked_ && !reference_valid_locked())
        throw AlpacaException("Celestron CGX park reference is unknown; Unpark and run FindHome",
                              AlpacaError::DriverException);
    if (stop_pending_) return parked_;
    if (const auto failure = stop_slot_.last_failure())
        throw AlpacaException("Celestron CGX stop failed: " + *failure, AlpacaError::DriverException);
    if (motion_state_unverified_)
        throw AlpacaException("Celestron CGX motion stop has not been verified", AlpacaError::DriverException);
    return parked_;
}
double CelestronCgxTelescopeDriver::get_azimuth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto sky = served_sky_position_locked();
    const double latitude =
        configured_site_coordinate(info_.site_latitude_degrees, "SiteLatitude") * std::numbers::pi / 180.0;
    const auto utc =
        std::chrono::system_clock::time_point{std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::duration<double>(current_utc_locked()))};
    const double lst =
        cgx_local_sidereal_time_hours(utc, configured_site_coordinate(info_.site_longitude_degrees, "SiteLongitude"));
    const double ha = (lst - sky.right_ascension_hours) * std::numbers::pi / 12.0;
    const double dec = sky.declination_degrees * std::numbers::pi / 180.0;
    const double east = -std::cos(dec) * std::sin(ha);
    const double north = std::sin(dec) * std::cos(latitude) - std::cos(dec) * std::sin(latitude) * std::cos(ha);
    double azimuth = std::atan2(east, north) * 180.0 / std::numbers::pi;
    if (azimuth < 0.0) azimuth += 360.0;
    return azimuth;
}
double CelestronCgxTelescopeDriver::get_declination() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return served_sky_position_locked().declination_degrees;
}
bool CelestronCgxTelescopeDriver::get_tracking() const {
    require_connected();
    std::lock_guard<std::mutex> lock(mutex_);
    require_healthy_transport();
    if (tracking_ && !reference_valid_locked())
        throw AlpacaException("Celestron CGX tracking reference is unknown; confirm a stop and run FindHome",
                              AlpacaError::DriverException);
    if (!tracking_state_valid_)
        throw AlpacaException("Celestron CGX tracking state is unknown after a partial controller update",
                              AlpacaError::DriverException);
    return tracking_;
}
void CelestronCgxTelescopeDriver::set_tracking(bool tracking) {
    require_connected();
    if (tracking) require_known_motor_model();
    MotionTransition transition(*this);
    std::unique_lock<std::mutex> lock(mutex_);
    set_tracking_locked(tracking, lock);
}

void CelestronCgxTelescopeDriver::set_tracking_locked(bool tracking, std::unique_lock<std::mutex>& lock) {
    if (tracking && parked_) {
        throw AlpacaException("Cannot enable tracking while parked", AlpacaError::InvalidWhileParked);
    }
    if (tracking) require_site_locked();
    if (tracking && !reference_valid_locked()) {
        throw AlpacaException("Run FindHome before enabling Celestron CGX tracking", AlpacaError::InvalidOperation);
    }
    write_tracking_state_locked(tracking, tracking_rate_, lock);
    tracking_ = tracking;
    if (tracking) at_home_ = false;
}
void CelestronCgxTelescopeDriver::set_focal_length(double meters) {
    if (!std::isfinite(meters) || meters < 0.0)
        throw AlpacaException("Focal length must be non-negative", AlpacaError::InvalidValue);
    std::lock_guard<std::mutex> lock(mutex_);
    require_connected();
    focal_length_m_ = meters;
}
GuideRate CelestronCgxTelescopeDriver::get_guide_rate() const {
    require_connected();
    std::lock_guard<std::mutex> lock(mutex_);
    return guide_rate_;
}
// Each axis is checked before the connection, so a bad value is InvalidValue even while disconnected
// (open-astro#1786).
void CelestronCgxTelescopeDriver::set_guide_rate(const GuideRate& rate) {
    util::check_guide_rate_arguments(kCommandLimits, util::GuideRateAxis::RightAscension, rate.ra);
    util::check_guide_rate_arguments(kCommandLimits, util::GuideRateAxis::Declination, rate.dec);
    require_connected();
    std::lock_guard<std::mutex> lock(mutex_);
    guide_rate_ = rate;
}
// One axis under one lock: the base default reads the pair through the connection-checking getter first, so an
// out-of-range write on a disconnected mount answered NotConnected (open-astro#1786).
void CelestronCgxTelescopeDriver::set_guide_rate_right_ascension(double value) {
    util::check_guide_rate_arguments(kCommandLimits, util::GuideRateAxis::RightAscension, value);
    require_connected();
    std::lock_guard<std::mutex> lock(mutex_);
    guide_rate_.ra = value;
}
void CelestronCgxTelescopeDriver::set_guide_rate_declination(double value) {
    util::check_guide_rate_arguments(kCommandLimits, util::GuideRateAxis::Declination, value);
    require_connected();
    std::lock_guard<std::mutex> lock(mutex_);
    guide_rate_.dec = value;
}
bool CelestronCgxTelescopeDriver::get_is_pulse_guiding() const {
    require_connected();
    require_healthy_transport();
    if (const auto failure = pulse_failure()) {
        throw AlpacaException("Celestron CGX pulse guide failed: " + *failure, AlpacaError::DriverException);
    }
    // Driver state, not the wire: the guide body runs until the mount reports the packet inactive (the firmware times
    // it), and disconnect joins it; a pulse a slew, Park or FindHome superseded ends its body at once, and that
    // command's wait on the mount stands in for it (guide_end_pending_, #2263). A guide outlives both only when
    // another AUX client started it, which this backend does not support (decision 0011 Consequences). Either axis's
    // pulse counts.
    return pulse_running() || guide_end_pending_.load();
}
double CelestronCgxTelescopeDriver::get_right_ascension() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return served_sky_position_locked().right_ascension_hours;
}
void CelestronCgxTelescopeDriver::set_declination_rate(double) {
    require_connected();
    not_implemented("DeclinationRate writes", true);
}
void CelestronCgxTelescopeDriver::set_right_ascension_rate(double) {
    require_connected();
    not_implemented("RightAscensionRate writes", true);
}
int CelestronCgxTelescopeDriver::get_side_of_pier() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return served_sky_position_locked().pointing_side == CgxPointingSide::Normal ? 0 : 1;
}
void CelestronCgxTelescopeDriver::set_side_of_pier(int side) {
    if (side != 0 && side != 1) throw AlpacaException("SideOfPier must be 0 or 1", AlpacaError::InvalidValue);
    require_connected();
    not_implemented("SideOfPier writes", true);
}
int CelestronCgxTelescopeDriver::get_destination_side_of_pier(double ra, double dec) const {
    util::check_destination_side_of_pier_arguments(ra, dec);
    require_connected();
    std::lock_guard<std::mutex> lock(mutex_);
    require_site_locked();
    const auto utc =
        std::chrono::system_clock::time_point{std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::duration<double>(current_utc_locked()))};
    const double lst =
        cgx_local_sidereal_time_hours(utc, configured_site_coordinate(info_.site_longitude_degrees, "SiteLongitude"));
    const double ha = util::wrap_hour_angle(lst - ra);
    return util::pier_side_for_hour_angle(ha);
}
EquatorialSystem CelestronCgxTelescopeDriver::get_equatorial_system() const {
    require_connected();
    return EquatorialSystem::Topocentric;
}
void CelestronCgxTelescopeDriver::set_does_refraction(bool does_refraction) {
    require_connected();
    if (does_refraction) not_implemented("atmospheric refraction", true);
}
void CelestronCgxTelescopeDriver::set_slew_settle_time(int seconds) {
    if (seconds < 0 || seconds > 300)
        throw AlpacaException("SlewSettleTime must be in [0, 300] seconds", AlpacaError::InvalidValue);
    require_connected();
    std::lock_guard<std::mutex> lock(mutex_);
    slew_settle_time_seconds_ = seconds;
}
bool CelestronCgxTelescopeDriver::wait_for_slew_settle(util::OperationContext& context) const {
    int settle_seconds;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settle_seconds = slew_settle_time_seconds_;
    }
    return settle_seconds == 0 || context.wait_for(std::chrono::seconds(settle_seconds));
}

bool CelestronCgxTelescopeDriver::wait_for_slew_completion(util::OperationContext& context,
                                                           const util::MotionClaim& claim,
                                                           util::TaskClock::clock::time_point deadline,
                                                           const char* timeout_message, const GotoTargets& targets,
                                                           std::chrono::milliseconds poll_interval, bool may_coast) {
    // Per axis: the distance at the first read, the distance the limit counts from (the first read, then the farthest
    // point of a coast), and whether the axis has closed on its target yet.
    struct Watch {
        std::optional<std::int32_t> first;
        std::int32_t base = 0;
        bool closing = false;
    };
    std::array<Watch, 2> watch;
    while (true) {
        const auto reason = motion_stop_reason(context, claim);
        if (reason != util::StopReason::None) {
            if (reason == util::StopReason::Cancelled) stop_motion_worker_checked(claim);
            return false;
        }
        bool all_done = true;
        for (std::size_t index = 0; index < 2; ++index) {
            const CgxAxis axis = index == 0 ? CgxAxis::RightAscension : CgxAxis::Declination;
            // An axis without a GOTO this leg keeps the old short-circuit: its done flag is read only while every axis
            // before it is done. An axis with one is always read, and its position too while it moves (#2640).
            const auto& target = targets[index];
            if (!target.has_value() && !all_done) continue;
            const bool done = protocol_.is_axis_slew_done(axis);
            all_done = all_done && done;
            if (done || !target.has_value()) continue;
            const std::uint32_t position = protocol_.get_axis_position(axis);
            const std::int32_t distance = std::abs(signed_cgx_encoder_delta(position, target.value()));
            Watch& axis_watch = watch[index];
            if (!axis_watch.first.has_value()) {
                ALPACA_LOG_DEBUG("Celestron CGX", std::string("GOTO leg ") + (index == 0 ? "RA" : "Dec") +
                                                      " first read: position=" + std::to_string(position) +
                                                      " target=" + std::to_string(target.value()) +
                                                      " shorter-way distance=" + std::to_string(distance));
                axis_watch.first = distance;
                axis_watch.base = distance;
                axis_watch.closing = !may_coast;
            } else if (!axis_watch.closing) {
                if (distance < axis_watch.base)
                    axis_watch.closing = true;
                else
                    axis_watch.base = distance;
            }
            const std::int32_t limit = axis_watch.closing ? axis_watch.base + kGotoWrongWayCounts
                                                          : axis_watch.first.value_or(distance) + kGotoCoastCounts;
            if (distance > limit) {
                const std::string message =
                    std::string("Celestron CGX ") + (index == 0 ? "RA" : "Dec") +
                    " moved away from its GOTO target: " + std::to_string(distance) + " counts from it, past the " +
                    std::to_string(limit) + " allowed (" + std::to_string(axis_watch.first.value_or(distance)) +
                    " at the start of the leg); the controller took the long way round, so the mount was stopped";
                ALPACA_LOG_ERROR("Celestron CGX", message);
                throw AlpacaException(message, AlpacaError::DriverException);
            }
        }
        if (all_done) return true;
        if (clock_.now() >= deadline) throw AlpacaException(timeout_message, AlpacaError::DriverException);
        static_cast<void>(context.wait_for(poll_interval));
    }
}

bool CelestronCgxTelescopeDriver::finish_slew_operation(util::OperationContext& context, const util::MotionClaim& claim,
                                                        util::TaskClock::clock::time_point deadline,
                                                        double right_ascension_hours, double declination_degrees,
                                                        CgxPointingSide pointing_side, GotoTargets first_leg,
                                                        const char* timeout_message, bool first_leg_may_coast) {
    // The mount stands untracked from the moment a correction is aimed until it tracks, and the sky moves on: over WiFi
    // (~21 ms an exchange) the check read came 140-270 ms, 2-4", after the aim (rig 2026-11-21). So each leg starts
    // tracking as it lands, before the check reads it, and each correction is aimed at the sky that far ahead: its
    // lead is the time from aiming the previous correction to its tracking start (the first has none to measure),
    // capped at kMaxCorrectionLead.
    int corrections = 0;
    std::optional<util::TaskClock::clock::time_point> aimed_at;
    util::TaskClock::clock::duration lead{};
    GotoTargets leg = first_leg;
    while (true) {
        if (!wait_for_slew_completion(context, claim, deadline, timeout_message, leg,
                                      std::chrono::milliseconds(corrections == 0 ? 200 : 20),
                                      corrections == 0 && first_leg_may_coast))
            return false;
        bool arrived = false;
        if (!run_motion_stage(context, claim, [&] {
                std::unique_lock<std::mutex> lock(mutex_);
                set_tracking_locked(true, lock);
                if (aimed_at)
                    lead = std::min<util::TaskClock::clock::duration>(clock_.now() - *aimed_at, kMaxCorrectionLead);
                const CgxSkyPosition actual = read_sky_position(lock);
                const double ra_error_degrees =
                    std::abs(std::remainder(actual.right_ascension_hours - right_ascension_hours, 24.0)) * 15.0;
                const double dec_error_degrees = std::abs(actual.declination_degrees - declination_degrees);
                const std::string arrival_diagnostic =
                    "Arrival actualRA=" + std::to_string(actual.right_ascension_hours) +
                    "h actualDec=" + std::to_string(actual.declination_degrees) +
                    "deg targetRA=" + std::to_string(right_ascension_hours) +
                    "h targetDec=" + std::to_string(declination_degrees) +
                    "deg RAErrorArcsec=" + std::to_string(ra_error_degrees * 3600.0) +
                    " DecErrorArcsec=" + std::to_string(dec_error_degrees * 3600.0) +
                    " corrections=" + std::to_string(corrections);
                ALPACA_LOG_DEBUG("Celestron CGX", arrival_diagnostic);
                if (ra_error_degrees <= kSlewArrivalToleranceDegrees &&
                    dec_error_degrees <= kSlewArrivalToleranceDegrees) {
                    arrived = true;
                    return;
                }
                // Off before a further GOTO_SLOW or the failure stop. TODO: whether MC_GOTO_SLOW while the axis tracks
                // ends or adds to the tracking is not established on the controller; the stop's 0x24 ending tracking
                // is the driver's assumption on every stop path.
                set_tracking_locked(false, lock);
                if (ra_error_degrees > kMaxSlewCorrectionDegrees || dec_error_degrees > kMaxSlewCorrectionDegrees ||
                    corrections >= kMaxSlewCorrections) {
                    ALPACA_LOG_ERROR("Celestron CGX", arrival_diagnostic + " bounded refinement failed");
                    throw AlpacaException(
                        "Celestron CGX reported slew completion outside the bounded 3-arcsecond target tolerance: " +
                            arrival_diagnostic,
                        AlpacaError::DriverException);
                }
                if (clock_.now() >= deadline) {
                    ALPACA_LOG_ERROR("Celestron CGX", arrival_diagnostic + " refinement deadline exceeded");
                    throw AlpacaException(std::string(timeout_message) + ": " + arrival_diagnostic,
                                          AlpacaError::DriverException);
                }
                aimed_at = clock_.now();
                const auto utc =
                    std::chrono::system_clock::time_point{
                        std::chrono::duration_cast<std::chrono::system_clock::duration>(
                            std::chrono::duration<double>(current_utc_locked()))} +
                    std::chrono::duration_cast<std::chrono::system_clock::duration>(lead);
                const double lst = cgx_local_sidereal_time_hours(
                    utc, configured_site_coordinate(info_.site_longitude_degrees, "SiteLongitude"));
                const auto target = cgx_sky_position_to_encoders(right_ascension_hours, declination_degrees, lst,
                                                                 hemisphere_locked(), pointing_side);
                lock.unlock();  // The corrections run with mutex_ free (decision 0016).
                leg = {};
                if (ra_error_degrees > kSlewArrivalToleranceDegrees) {
                    leg[0] = target.first;
                    protocol_.goto_axis_slow(CgxAxis::RightAscension, target.first);
                }
                if (dec_error_degrees > kSlewArrivalToleranceDegrees) {
                    leg[1] = target.second;
                    protocol_.goto_axis_slow(CgxAxis::Declination, target.second);
                }
                ++corrections;
            })) {
            return false;
        }
        if (!arrived) continue;
        if (!wait_for_slew_settle(context)) {
            if (motion_stop_reason(context, claim) == util::StopReason::Cancelled) stop_motion_worker_checked(claim);
            return false;
        }
        return run_motion_stage(context, claim, [this] {
            std::lock_guard<std::mutex> lock(mutex_);
            slew_in_progress_ = false;
            restore_tracking_after_slew_ = false;
        });
    }
}

double CelestronCgxTelescopeDriver::get_sidereal_time() const {
    require_connected();
    std::lock_guard<std::mutex> lock(mutex_);
    require_site_locked();
    const auto utc =
        std::chrono::system_clock::time_point{std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::duration<double>(current_utc_locked()))};
    return cgx_local_sidereal_time_hours(utc,
                                         configured_site_coordinate(info_.site_longitude_degrees, "SiteLongitude"));
}

void CelestronCgxTelescopeDriver::set_tracking_rate_locked(int rate, std::unique_lock<std::mutex>& lock) {
    if (rate < 0 || rate > 2) {
        throw AlpacaException("TrackingRate must be Sidereal, Lunar, or Solar", AlpacaError::InvalidValue);
    }
    if (!tracking_state_valid_)
        throw AlpacaException("Set Tracking before changing TrackingRate after a partial controller update",
                              AlpacaError::InvalidOperation);
    write_tracking_state_locked(tracking_, rate, lock);
    tracking_rate_ = rate;
}

void CelestronCgxTelescopeDriver::write_tracking_state_locked(bool tracking, int rate,
                                                              std::unique_lock<std::mutex>& lock) {
    // Every nonzero rate path, including TrackingRate, needs the same reference.
    // Zero-rate cleanup remains available after a session fault.
    if (tracking && !reference_valid_locked())
        throw AlpacaException("Run FindHome before enabling Celestron CGX tracking", AlpacaError::InvalidOperation);
    const auto hemisphere = hemisphere_locked();  // Read before the exchanges may drop mutex_.
    const auto previous_rate = tracking_rate_;
    const auto write = [this, hemisphere](bool enabled, int selected_rate, const char* operation) {
        std::exception_ptr failure;
        const auto send = [&](CgxAxis axis, std::uint8_t command, std::vector<std::uint8_t> payload) {
            try {
                const AuxFrame request{0x00, static_cast<std::uint8_t>(axis), command, std::move(payload)};
                require_cgx_ack(protocol_.exchange(request), operation);
            } catch (...) {
                if (!failure) failure = std::current_exception();
            }
        };
        if (enabled) {
            send(CgxAxis::Declination, 0x06, {0, 0, 0});
            const auto command = hemisphere == CgxHemisphere::South ? 0x07 : 0x06;
            const auto payload = selected_rate == 0   ? std::vector<std::uint8_t>{0xff, 0xff}
                                 : selected_rate == 1 ? std::vector<std::uint8_t>{0xff, 0xfd}
                                                      : std::vector<std::uint8_t>{0xff, 0xfe};
            send(CgxAxis::RightAscension, static_cast<std::uint8_t>(command), payload);
        } else {
            send(CgxAxis::RightAscension, 0x06, {0, 0, 0});
            send(CgxAxis::Declination, 0x06, {0, 0, 0});
        }
        return failure;
    };

    // The exchanges run with the caller's mutex_ dropped, so the I/O lock is never taken under it (decision 0016).
    // Read before the release; safe because every caller holds motion_start_mutex_ and either transition_mutex_ or
    // the motion slot the stop worker joins before its stop writes, so no other writer changes tracking_state_valid_
    // while mutex_ is dropped.
    const bool previous_state_valid = tracking_state_valid_;
    std::exception_ptr failure;
    bool rollback_succeeded = false;
    bool rolled_back = false;
    {
        const MutexRelease unlocked(lock);
        failure = write(tracking, rate, "tracking command");
        if (failure) {
            try {
                // Lost matching invalidates the reference. Recover on the owned fd
                // and leave tracking off rather than resume an uncertain frame.
                // resync() only flushes the fd already held: a closed (lost) link throws NotConnected and is never
                // reopened here (decision 0009 rule 4), so no probe wait either (#1457).
                protocol_.resync();
                rollback_succeeded = !write(false, previous_rate, "tracking rollback");
                rolled_back = rollback_succeeded;
            } catch (...) {
                rollback_succeeded = false;
            }
        }
    }
    if (failure) {
        if (rolled_back) tracking_ = false;
        tracking_state_valid_ = previous_state_valid && rollback_succeeded;
        if (!rollback_succeeded) {
            ALPACA_LOG_ERROR("Celestron CGX", "Tracking update and rollback both failed; tracking readback is unknown");
        }
        std::rethrow_exception(failure);
    }
    require_connected();  // Re-check after the exchanges (decision 0016 rule 6).
    tracking_state_valid_ = true;
}
void CelestronCgxTelescopeDriver::set_site_elevation(double elevation) {
    if (!std::isfinite(elevation) || elevation < -300.0 || elevation > 10000.0)
        throw AlpacaException("SiteElevation must be in [-300, 10000] m", AlpacaError::InvalidValue);
    require_connected();
    std::lock_guard<std::mutex> lock(mutex_);
    info_.site_elevation_meters = elevation;
}

void CelestronCgxTelescopeDriver::require_site_change_allowed_locked() const {
    // Caller holds motion_start_mutex_ and mutex_. Refuse rather than cancelling
    // a guide or changing the coordinate frame underneath an active operation.
    if (motion_transition_ || disconnecting_ || slew_in_progress_ || stop_pending_ || motion_slot_.running() ||
        pulse_running() || stop_slot_.running())
        throw AlpacaException("Cannot change Celestron CGX site coordinates while motion is in progress",
                              AlpacaError::InvalidOperation);
}

void CelestronCgxTelescopeDriver::set_site_latitude(double latitude) {
    util::validate_latitude(latitude);
    std::lock_guard<std::mutex> start_lock(motion_start_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    require_connected();
    if (info_.site_latitude_degrees == latitude) return;
    require_site_change_allowed_locked();
    info_.site_latitude_degrees = latitude;
}
void CelestronCgxTelescopeDriver::set_site_longitude(double longitude) {
    util::validate_longitude(longitude);
    std::lock_guard<std::mutex> start_lock(motion_start_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    require_connected();
    if (info_.site_longitude_degrees == longitude) return;
    require_site_change_allowed_locked();
    info_.site_longitude_degrees = longitude;
}
bool CelestronCgxTelescopeDriver::get_slewing() const {
    require_connected();
    require_healthy_transport();  // Before the stop checks: a faulted link raises even with a stop pending (#1950).
    if (stop_pending_ || stop_slot_.running()) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    require_connected();
    require_healthy_transport();  // Again under the lock: a fault after the first check must not read a cached value.
    if (stop_pending_ || stop_slot_.running()) return true;
    if (const auto failure = stop_slot_.last_failure())
        throw AlpacaException("Celestron CGX stop failed: " + *failure, AlpacaError::DriverException);
    if (motion_state_unverified_)
        throw AlpacaException("Celestron CGX motion stop has not been verified", AlpacaError::DriverException);
    if (const auto failure = motion_slot_.last_failure())
        throw AlpacaException("Celestron CGX motion failed: " + *failure, AlpacaError::DriverException);
    return slew_in_progress_ || motion_slot_.running();
}
double CelestronCgxTelescopeDriver::get_target_declination() const {
    require_connected();
    std::lock_guard<std::mutex> lock(mutex_);
    if (parked_) {
        throw AlpacaException("TargetDeclination is unavailable while parked", AlpacaError::InvalidWhileParked);
    }
    if (!target_dec_set_) {
        throw AlpacaException("TargetDeclination has not been set", AlpacaError::InvalidOperation);
    }
    return target_dec_degrees_;
}
void CelestronCgxTelescopeDriver::set_target_declination(double dec) {
    util::check_target_declination_arguments(dec);
    require_connected();
    std::lock_guard<std::mutex> lock(mutex_);
    if (parked_) {
        throw AlpacaException("Cannot set TargetDeclination while parked", AlpacaError::InvalidWhileParked);
    }
    target_dec_degrees_ = dec;
    target_dec_set_ = true;
}
double CelestronCgxTelescopeDriver::get_target_right_ascension() const {
    require_connected();
    std::lock_guard<std::mutex> lock(mutex_);
    if (parked_) {
        throw AlpacaException("TargetRightAscension is unavailable while parked", AlpacaError::InvalidWhileParked);
    }
    if (!target_ra_set_) {
        throw AlpacaException("TargetRightAscension has not been set", AlpacaError::InvalidOperation);
    }
    return target_ra_hours_;
}
void CelestronCgxTelescopeDriver::set_target_right_ascension(double ra) {
    util::check_target_right_ascension_arguments(ra);
    require_connected();
    std::lock_guard<std::mutex> lock(mutex_);
    if (parked_) {
        throw AlpacaException("Cannot set TargetRightAscension while parked", AlpacaError::InvalidWhileParked);
    }
    target_ra_hours_ = ra;
    target_ra_set_ = true;
}
void CelestronCgxTelescopeDriver::set_tracking_rate(int rate) {
    util::check_tracking_rate_arguments(kCommandLimits, rate);
    require_connected();
    MotionTransition transition(*this);
    std::unique_lock<std::mutex> lock(mutex_);
    set_tracking_rate_locked(rate, lock);
}
std::chrono::system_clock::time_point CelestronCgxTelescopeDriver::get_utc_date() const {
    require_connected();
    std::lock_guard<std::mutex> lock(mutex_);
    return utc_now_locked();
}
void CelestronCgxTelescopeDriver::set_utc_date(std::chrono::system_clock::time_point utc) {
    require_connected();
    // Decision 0001 (#2166): an NTP-disciplined host has a better clock than the client, and the
    // interface lets a mount with its own time source refuse the write, so it throws and changes nothing.
    if (util::ClientUtcWarning::host_synchronized()) {
        throw AlpacaException(
            "UTCDate cannot be set: the host clock is NTP-synchronized, and the mount takes its time from it",
            AlpacaError::PropertyNotImplemented);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto system_now = system_now_();
    utc_offset_ = utc - system_now;
    utc_anchor_system_ = system_now;
    utc_anchor_steady_ = clock_.now();
    has_utc_offset_ = true;
    utc_offset_host_was_synchronized_ = false;
    next_utc_discipline_resample_ = clock_.now() + kHostDisciplineResampleInterval;
}
int CelestronCgxTelescopeDriver::get_tracking_rate() const {
    require_connected();
    std::lock_guard<std::mutex> lock(mutex_);
    require_healthy_transport();
    if (tracking_ && !reference_valid_locked())
        throw AlpacaException("Celestron CGX tracking reference is unknown; confirm a stop and run FindHome",
                              AlpacaError::DriverException);
    if (!tracking_state_valid_)
        throw AlpacaException("Celestron CGX tracking state is unknown after a partial controller update",
                              AlpacaError::DriverException);
    return tracking_rate_;
}
void CelestronCgxTelescopeDriver::find_home() { start_home_operation(false); }
void CelestronCgxTelescopeDriver::park() { start_home_operation(true); }
void CelestronCgxTelescopeDriver::pulse_guide(int direction, int duration_ms) {
    util::check_pulse_guide_arguments(kCommandLimits, direction, duration_ms);
    require_connected();
    require_known_motor_model();
    std::lock_guard<std::mutex> start_lock(motion_start_mutex_);
    require_connected();
    if (motion_transition_ || disconnecting_)
        throw AlpacaException("Celestron CGX is changing motion ownership", AlpacaError::InvalidOperation);
    const bool dec_axis = direction == 0 || direction == 1;
    const CgxAxis axis = dec_axis ? CgxAxis::Declination : CgxAxis::RightAscension;
    const std::size_t index = pulse_index(axis);
    constexpr const char* kOverlapRefusal = "PulseGuide cannot overlap telescope motion or another guide pulse";
    constexpr const char* kAmbiguousRefusal =
        "PulseGuide refused: the previous pulse on this axis started without an acknowledgement, and the driver is "
        "still waiting for the mount to report its packet inactive";
    {
        // A slew, Park or FindHome owns both axes, so the pulse is refused (#2263). A pulse on this axis whose first
        // packet's ACK was lost keeps it; any other pulse on this axis is superseded below, one on the other axis
        // keeps running.
        std::lock_guard<std::mutex> lock(mutex_);
        if (motion_owner_.slew_running()) throw AlpacaException(kOverlapRefusal, AlpacaError::InvalidOperation);
        if (pulse_start_ambiguous_[index]) throw AlpacaException(kAmbiguousRefusal, AlpacaError::InvalidOperation);
    }
    // Either axis's kept failure refuses every PulseGuide until a command clears it (#2467): IsPulseGuiding raises it,
    // so it never reports a pulse it cannot describe.
    if (const auto failure = pulse_failure()) {
        throw AlpacaException("Celestron CGX pulse guide remains unresolved: " + *failure,
                              AlpacaError::DriverException);
    }

    int velocity;
    util::MotionClaim claim;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        require_site_locked();
        if (parked_) throw AlpacaException("Cannot PulseGuide while parked", AlpacaError::InvalidWhileParked);
        if (!tracking_state_valid_)
            throw AlpacaException("Celestron CGX tracking state is unknown after a partial controller update",
                                  AlpacaError::DriverException);
        if (!tracking_)
            throw AlpacaException("PulseGuide requires tracking to be enabled", AlpacaError::InvalidOperation);
        if (!reference_valid_locked())
            throw AlpacaException("Run FindHome before PulseGuide", AlpacaError::InvalidOperation);
        if (duration_ms == 0) return;  // claims nothing: a pulse running on this axis keeps it
        const auto sky = read_sky_position(lock);
        const double guide_rate = dec_axis ? guide_rate_.dec : guide_rate_.ra;
        velocity = static_cast<int>(std::lround(guide_rate / kSiderealDegreesPerSecond * 100.0));
        // Normal northern CGX: positive Dec encoder motion lowers declination.
        // Evidence: docs/failures/0012-cgx-dec-guide-polarity.md.
        const bool positive = direction == 1 || direction == 3;  // South or West.
        if (!positive) velocity = -velocity;
        if (hemisphere_locked() == CgxHemisphere::South) {
            // TODO: confirm autoguider polarity on southern CGX/CGX-L hardware.
            velocity = -velocity;
        }
        if (dec_axis && sky.pointing_side == CgxPointingSide::BeyondPole) velocity = -velocity;
        // Claims this axis (decision 0006 rule 3): the older pulse on it is superseded, not joined, and sends this
        // axis nothing more; this pulse's first packet replaces its packet on the wire.
        // TODO: confirm on hardware that an MC_AUX_GUIDE sent while the axis's previous packet is active replaces it.
        claim = motion_owner_.claim(util::MotionCommand::PulseGuide, static_cast<int>(index), kOverlapRefusal);
    }

    const int total_centiseconds = std::max(1, static_cast<int>(std::lround(duration_ms / 10.0)));
    const int first_chunk = std::min(255, total_centiseconds);
    auto start_promise = std::make_shared<std::promise<PulseGuideStart>>();
    const auto start_result = start_promise->get_future().share();
    // A start() refused after the claim above (a stale slot that will not reap) keeps the claim: harmless, the next
    // claim on this axis supersedes it and no body runs for it. start() runs under motion_start_mutex_: with this
    // axis's current body and kMaxStaleBodies superseded ones still running it waits up to kStaleReapTimeout (2 s,
    // task clock) for one to return, and a stale body waiting for this lock (its guide-poll re-check or
    // publish_guide_end_frame()) cannot return meanwhile, so that wait can run its full 2 s and refuse. Left as is
    // (#2467): it takes a fifth pulse on one axis while the four before it there are all still running.
    pulse_slots_[index].start([this, claim, axis, index, velocity, first_chunk,
                               remaining = total_centiseconds - first_chunk,
                               start_result](util::OperationContext& context) {
        // Only the body whose own first packet went unacknowledged hands the axis back. A newer pulse on its axis never
        // supersedes it (pulse_start_ambiguous_ refuses one); a slew, Park or FindHome can, and it then returns at once
        // and clears the flag while that command owns the axis (#2263).
        const auto end_ambiguity = [&] {
            if (start_result.get() == PulseGuideStart::Ambiguous) end_ambiguous_pulse_start(index);
        };
        try {
            run_pulse_guide(context, claim, axis, velocity, first_chunk, remaining, start_result);
        } catch (...) {
            end_ambiguity();
            if (superseded(context, claim)) {
                log_superseded_failure("PulseGuide", std::current_exception());
                return;
            }
            throw;
        }
        end_ambiguity();
    });
    try {
        protocol_.pulse_guide_axis(axis, velocity, first_chunk);
        start_promise->set_value(PulseGuideStart::Acknowledged);
    } catch (...) {
        // The command may have applied even though its acknowledgement did not.
        // Keep the worker in charge of this <=2.55 s packet; never chain the
        // remaining request or let another motion reuse the axis meanwhile.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pulse_start_ambiguous_[index] = true;
        }
        start_promise->set_value(PulseGuideStart::Ambiguous);
        throw;
    }
}
void CelestronCgxTelescopeDriver::set_park() {
    require_connected();
    not_implemented("SetPark");
}
void CelestronCgxTelescopeDriver::slew_to_coordinates(double, double) {
    require_connected();
    not_implemented("SlewToCoordinates");
}
void CelestronCgxTelescopeDriver::slew_to_coordinates_async(double ra, double dec) { start_slew_operation(ra, dec); }
void CelestronCgxTelescopeDriver::slew_to_target() {
    require_connected();
    not_implemented("SlewToTarget");
}
void CelestronCgxTelescopeDriver::slew_to_target_async() {
    require_connected();
    double ra;
    double dec;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!target_ra_set_ || !target_dec_set_)
            throw AlpacaException("Both target coordinates must be set before SlewToTargetAsync",
                                  AlpacaError::InvalidOperation);
        ra = target_ra_hours_;
        dec = target_dec_degrees_;
    }
    start_slew_operation(ra, dec);
}
void CelestronCgxTelescopeDriver::sync_to_coordinates(double ra, double dec) {
    util::check_sync_arguments(ra, dec, "SyncToCoordinates");
    require_connected();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        require_site_locked();
        if (parked_) throw AlpacaException("Cannot sync while parked", AlpacaError::InvalidWhileParked);
    }
    MotionTransition transition(*this);
    std::unique_lock<std::mutex> lock(mutex_);
    if (parked_) throw AlpacaException("Cannot sync while parked", AlpacaError::InvalidWhileParked);
    if (!reference_valid_locked())
        throw AlpacaException("Run FindHome before SyncToCoordinates", AlpacaError::InvalidOperation);
    // #2411: a Sync far from the served position is refused before any tracking or label write, so the reference,
    // Tracking and the labels stay as they were (decision 0011). The served frame is at most a poll period old.
    const CgxSkyPosition served = served_sky_position_locked();
    util::check_sync_bound("SyncToCoordinates", {served.right_ascension_hours, served.declination_degrees}, {ra, dec},
                           util::kMaxSyncOffsetDegrees);
    const bool restore_tracking = tracking_;
    bool reference_write_started = false;
    try {
        if (restore_tracking) set_tracking_locked(false, lock);
        // A rate-off ACK can precede encoder standstill. Never re-label a
        // drifting axis, including when the client just switched tracking off.
        util::ConsecutiveSettle settle(3, 15);
        const auto settle_deadline = clock_.now() + std::chrono::milliseconds(500);
        std::optional<std::pair<std::uint32_t, std::uint32_t>> previous;
        // The settle and label-readback loops run on the wire with mutex_ dropped (decision 0016 rules 1 and 2).
        {
            const MutexRelease unlocked(lock);
            while (true) {
                const auto ra_encoder = protocol_.get_axis_position(CgxAxis::RightAscension);
                const auto dec_encoder = protocol_.get_axis_position(CgxAxis::Declination);
                const bool stable = previous && std::abs(signed_cgx_encoder_delta(ra_encoder, previous->first)) <= 2 &&
                                    std::abs(signed_cgx_encoder_delta(dec_encoder, previous->second)) <= 2;
                previous = std::pair{ra_encoder, dec_encoder};
                const auto state = settle.feed(stable);
                if (state == util::ConsecutiveSettle::State::Settled) break;
                if (state == util::ConsecutiveSettle::State::TimedOut || clock_.now() >= settle_deadline) {
                    throw AlpacaException("Celestron CGX encoders did not settle before Sync",
                                          AlpacaError::DriverException);
                }
                clock_.sleep_for(std::chrono::milliseconds(20));
            }
        }
        require_connected();  // Rule 6: re-check after the exchanges.
        if (parked_) throw AlpacaException("Cannot sync while parked", AlpacaError::InvalidWhileParked);
        if (!reference_valid_locked())
            throw AlpacaException("Run FindHome before SyncToCoordinates", AlpacaError::InvalidOperation);
        std::uint32_t current_ra_encoder;
        std::uint32_t current_dec_encoder;
        const CgxSkyPosition current = read_sky_position(lock, &current_ra_encoder, &current_dec_encoder);
        const auto utc =
            std::chrono::system_clock::time_point{std::chrono::duration_cast<std::chrono::system_clock::duration>(
                std::chrono::duration<double>(current_utc_locked()))};
        const double lst = cgx_local_sidereal_time_hours(
            utc, configured_site_coordinate(info_.site_longitude_degrees, "SiteLongitude"));
        const auto positions = cgx_sky_position_to_encoders(ra, dec, lst, hemisphere_locked(), current.pointing_side);
        const std::uint32_t next_ra_rehome_label =
            offset_encoder_label(ra_rehome_label_, signed_cgx_encoder_delta(positions.first, current_ra_encoder));
        const std::uint32_t next_dec_rehome_label =
            offset_encoder_label(dec_rehome_label_, signed_cgx_encoder_delta(positions.second, current_dec_encoder));
        reference_write_started = true;  // Even a failed ACK may have changed the controller labels.
        sync_served_pair_ = CgxFrame{current_ra_encoder, current_dec_encoder, system_now_()};
        {
            const MutexRelease unlocked(lock);
            protocol_.set_axis_position(CgxAxis::RightAscension, positions.first);
            protocol_.set_axis_position(CgxAxis::Declination, positions.second);
            // MC_SET_POSITION itself produces a brief readback transient even on
            // stationary hardware. Verify retained labels, not its first sample.
            util::ConsecutiveSettle reference_settle(3, 15);
            const auto reference_deadline = clock_.now() + std::chrono::milliseconds(500);
            while (true) {
                const auto ra_delta =
                    signed_cgx_encoder_delta(protocol_.get_axis_position(CgxAxis::RightAscension), positions.first);
                const auto dec_delta =
                    signed_cgx_encoder_delta(protocol_.get_axis_position(CgxAxis::Declination), positions.second);
                const auto state = reference_settle.feed(std::abs(ra_delta) <= 2 && std::abs(dec_delta) <= 2);
                if (state == util::ConsecutiveSettle::State::Settled) break;
                if (state == util::ConsecutiveSettle::State::TimedOut || clock_.now() >= reference_deadline) {
                    throw AlpacaException("Celestron CGX did not retain the synchronized encoder reference: RA delta=" +
                                              std::to_string(ra_delta) + " Dec delta=" + std::to_string(dec_delta) +
                                              " counts",
                                          AlpacaError::DriverException);
                }
                clock_.sleep_for(std::chrono::milliseconds(20));
            }
        }
        require_connected();  // Rule 6: re-check after the exchanges; the catch marks the labels unknown.
        // Sync re-labels the current mechanical position. Keep the homed
        // reference fixed, and remember the equivalent home labels in this
        // shifted frame for any later meridian-route re-home.
        ra_rehome_label_ = next_ra_rehome_label;
        dec_rehome_label_ = next_dec_rehome_label;
        write_frame(positions.first, positions.second);  // the verified labels, before the next poll
        sync_served_pair_.reset();
        at_home_ = false;
        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_ra_set_ = target_dec_set_ = true;
        sync_correction_active_ = true;
        needs_sync_ = false;
    } catch (...) {
        sync_served_pair_.reset();  // MutexRelease took mutex_ back
        if (reference_write_started) {
            calibrated_ = false;
            needs_sync_ = true;
            sync_correction_active_ = false;
        }
        throw;
    }
    if (restore_tracking) set_tracking_locked(true, lock);
}
void CelestronCgxTelescopeDriver::sync_to_target() {
    require_connected();
    double ra;
    double dec;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!target_ra_set_ || !target_dec_set_)
            throw AlpacaException("Both target coordinates must be set before SyncToTarget",
                                  AlpacaError::InvalidOperation);
        ra = target_ra_hours_;
        dec = target_dec_degrees_;
    }
    sync_to_coordinates(ra, dec);
}
void CelestronCgxTelescopeDriver::unpark() { start_stop_operation(true); }
bool CelestronCgxTelescopeDriver::get_can_move_axis(int axis) const {
    if (axis < 0 || axis > 2) throw AlpacaException("Axis must be 0, 1 or 2", AlpacaError::InvalidValue);
    return false;
}
void CelestronCgxTelescopeDriver::move_axis(int axis, double) {
    if (axis < 0 || axis > 2) throw AlpacaException("Axis must be 0, 1 or 2", AlpacaError::InvalidValue);
    require_connected();
    not_implemented("MoveAxis");
}
std::pair<double, double> CelestronCgxTelescopeDriver::get_axis_rate_range(int axis) const {
    if (axis < 0 || axis > 2) throw AlpacaException("Axis must be 0, 1 or 2", AlpacaError::InvalidValue);
    require_connected();
    return {0.0, 0.0};
}
std::vector<std::pair<double, double>> CelestronCgxTelescopeDriver::get_axis_rate_ranges(int axis) const {
    if (axis < 0 || axis > 2) throw AlpacaException("Axis must be 0, 1 or 2", AlpacaError::InvalidValue);
    require_connected();
    return {};
}
void CelestronCgxTelescopeDriver::abort_slew() { start_stop_operation(false); }

void CelestronCgxTelescopeDriver::start_stop_operation(bool unpark) {
    require_connected();
    std::lock_guard<std::mutex> start_lock(motion_start_mutex_);
    require_connected();
    if (disconnecting_) throw AlpacaException("Celestron CGX is disconnecting", AlpacaError::InvalidOperation);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!unpark && parked_) throw AlpacaException("Cannot AbortSlew while parked", AlpacaError::InvalidWhileParked);
        if (stop_pending_) {
            unpark_after_stop_ = unpark_after_stop_ || unpark;
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_pending_ = true;
        unpark_after_stop_ = unpark;
    }
    const bool was_transitioning = motion_transition_;
    motion_transition_ = true;
    try {
        stop_slot_.start([this](util::OperationContext& context) { run_stop_operation(context); });
    } catch (...) {
        motion_transition_ = was_transitioning;
        std::lock_guard<std::mutex> lock(mutex_);
        stop_pending_ = false;
        throw;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        motion_owner_.cancel(util::MotionOwner::kBothAxes);
    }
    motion_slot_.cancel();
    for (auto& slot : pulse_slots_) slot.cancel();
}

void CelestronCgxTelescopeDriver::run_stop_operation(util::OperationContext& context) {
    // Serialize the ENTIRE reap with MotionTransition and disconnect. A
    // cancelled waiter must exit without acquiring the lock its joiner owns.
    std::unique_lock<std::mutex> transition(transition_mutex_, std::defer_lock);
    while (!transition.try_lock()) {
        if (!context.wait_for(std::chrono::milliseconds(20))) return;
    }
    if (context.stop_reason() != util::StopReason::None) return;
    try {
        bool restore_tracking_requested;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            restore_tracking_requested = tracking_ || restore_tracking_after_slew_;
        }
        for (auto& slot : pulse_slots_) slot.cancel_all_and_join();
        motion_slot_.cancel_all_and_join();
        stop_axes_checked(std::chrono::seconds(3), true);
        motion_slot_.clear_failure();
        for (auto& slot : pulse_slots_) slot.clear_failure();
        std::lock_guard<std::mutex> start_lock(motion_start_mutex_);
        std::unique_lock<std::mutex> lock(mutex_);
        const bool restore_tracking = restore_tracking_requested && reference_valid_locked() && !parked_;
        if (unpark_after_stop_) parked_ = false;
        if (restore_tracking) set_tracking_locked(true, lock);
        slew_in_progress_ = false;
        restore_tracking_after_slew_ = false;
        stop_pending_ = false;
        motion_transition_ = false;
    } catch (...) {
        std::lock_guard<std::mutex> start_lock(motion_start_mutex_);
        std::lock_guard<std::mutex> lock(mutex_);
        stop_pending_ = false;
        motion_transition_ = false;
        throw;  // AsyncOperation retains this for Slewing / AtPark.
    }
}
void CelestronCgxTelescopeDriver::slew_to_alt_az(double, double) {
    require_connected();
    not_implemented("SlewToAltAz");
}
void CelestronCgxTelescopeDriver::slew_to_alt_az_async(double, double) {
    require_connected();
    not_implemented("SlewToAltAzAsync");
}
void CelestronCgxTelescopeDriver::sync_to_alt_az(double, double) {
    require_connected();
    not_implemented("SyncToAltAz");
}

void CelestronCgxTelescopeDriver::stop_motion_worker_checked(const util::MotionClaim& claim) {
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hook = stop_worker_hook_for_testing_;
    }
    if (hook) hook();
    // Serialize check + dispatch with the explicit-stop ownership publication.
    // Never take transition_mutex_: the reaper holds it while joining us.
    std::lock_guard<std::mutex> start_lock(motion_start_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_pending_) return;
        // A newer command owns the axes (decision 0006 rule 6): a stop now would end its motion.
        if (motion_owner_.stop_reason(claim) == util::StopReason::Superseded) return;
    }
    stop_axes_checked();
}

void CelestronCgxTelescopeDriver::stop_axes_checked(std::chrono::milliseconds verification_budget,
                                                    bool require_still_encoders) {
    const std::array axes{CgxAxis::RightAscension, CgxAxis::Declination};
    bool stop_acknowledged = false;
    // The stop write pair runs with mutex_ free (decision 0016) and tracking state is published once after it, so a
    // tracking read sees the state before the stop or after it. Unknown is a retained failure state, not an
    // observable normal-dispatch transient.
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::exception_ptr acknowledgement_failure;
        for (const auto axis : axes) {
            try {
                const AuxFrame request{0x00, static_cast<std::uint8_t>(axis), 0x24, {0}};
                require_cgx_ack(protocol_.exchange(request), "axis stop");
            } catch (...) {
                if (!acknowledgement_failure) acknowledgement_failure = std::current_exception();
                try {
                    // AUX synchronization is shared: a missing RA reply must
                    // not prevent the responsive Dec controller receiving stop.
                    // resync() only flushes the fd already held: a closed (lost) link throws NotConnected and is
                    // never reopened here (decision 0009 rule 4), so no probe wait either (#1457).
                    protocol_.resync();
                } catch (const std::exception& error) {
                    ALPACA_LOG_WARN(
                        "Celestron CGX",
                        std::string("Could not resynchronize AUX before retrying the axis stop: ") + error.what());
                } catch (...) {
                    ALPACA_LOG_WARN("Celestron CGX", "Could not resynchronize AUX before retrying the axis stop");
                }
                if (tcp_) {
                    // Over TCP the status could not confirm this stop: the stop and a zero rate go out at once,
                    // without waiting for replies, before the other axis and the next round (they cost no wait).
                    try {
                        protocol_.send_stop(axis);
                    } catch (const std::exception& error) {
                        ALPACA_LOG_WARN("Celestron CGX", std::string("Send-only TCP stop failed: ") + error.what());
                    }
                }
            }
        }
        if (!acknowledgement_failure) {
            stop_acknowledged = true;
            break;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tracking_ = false;
        tracking_state_valid_ = stop_acknowledged;
    }
    std::exception_ptr verification_failure;
    // Over TCP one status poll of both axes can take one recovered re-send (6-8 exchanges at the WiFi p99 of 33 ms plus
    // the 300 ms re-send point), so the budget grows by that much; it never verifies a status that reads busy.
    const auto stop_deadline =
        clock_.now() + verification_budget + (tcp_ ? kTcpStopVerifyAllowance : std::chrono::milliseconds{0});
    // A status can read done while an axis still moves (#2411). Each axis's angle is taken from its first read, so the
    // check never meets the encoder wrap.
    util::StillnessCheck still(clock_.now());
    std::optional<std::pair<std::uint32_t, std::uint32_t>> first_encoders;
    const auto axis_degrees = [](std::uint32_t encoder, std::uint32_t origin) {
        return signed_cgx_encoder_delta(encoder, origin) * 360.0 / kCgxEncoderStepsPerRevolution;
    };
    while (true) {
        std::string pending_status;
        for (const auto axis : axes) {
            try {
                const bool slew_done = protocol_.is_axis_slew_done(axis);
                const bool home_done = protocol_.is_axis_home_done(axis);
                const bool guide_active = protocol_.is_aux_guide_active(axis);
                if ((!slew_done || !home_done || guide_active) && pending_status.empty()) {
                    pending_status =
                        std::string("Celestron CGX did not confirm that both motor axes stopped: ") +
                        (axis == CgxAxis::RightAscension ? "RA" : "Dec") + " slew_done=" + std::to_string(slew_done) +
                        " home_done=" + std::to_string(home_done) + " guide_active=" + std::to_string(guide_active);
                }
            } catch (...) {
                if (!verification_failure) verification_failure = std::current_exception();
            }
        }
        if (!verification_failure && pending_status.empty() && require_still_encoders) {
            try {
                const auto ra_encoder = protocol_.get_axis_position(CgxAxis::RightAscension);
                const auto dec_encoder = protocol_.get_axis_position(CgxAxis::Declination);
                if (!first_encoders) first_encoders = std::pair{ra_encoder, dec_encoder};
                const util::MountPosition axes_angle{axis_degrees(ra_encoder, first_encoders->first) / 15.0,
                                                     axis_degrees(dec_encoder, first_encoders->second)};
                if (!still.feed(util::PositionReading{axes_angle, clock_.now()})) {
                    pending_status =
                        "Celestron CGX did not confirm that both motor axes stopped: the encoders still move";
                }
            } catch (...) {
                verification_failure = std::current_exception();
            }
        }
        if (verification_failure || pending_status.empty()) break;
        if (clock_.now() >= stop_deadline) {
            verification_failure =
                std::make_exception_ptr(AlpacaException(pending_status, AlpacaError::DriverException));
            break;
        }
        // A stop ACK accepts the command; the CGX may still be decelerating.
        // Only valid busy status is polled. Communication failures stay errors.
        clock_.sleep_for(std::chrono::milliseconds(20));
    }
    if (verification_failure) {
        std::lock_guard<std::mutex> lock(mutex_);
        motion_state_unverified_ = true;
        tracking_state_valid_ = false;
        std::rethrow_exception(verification_failure);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        motion_state_unverified_ = false;
        tracking_ = false;
        tracking_state_valid_ = stop_acknowledged;
    }
}

void CelestronCgxTelescopeDriver::start_home_operation(bool park_after) {
    require_connected();
    require_known_motor_model();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        require_site_locked();  // FindHome fixes the mechanical hemisphere from the latitude.
    }
    if (park_after && get_at_park()) return;
    MotionTransition transition(*this, true);
    if (park_after && get_at_park()) return;
    if (!park_after && get_at_park())
        throw AlpacaException("Cannot FindHome while parked; Unpark first", AlpacaError::InvalidWhileParked);
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (tracking_) set_tracking_locked(false, lock);
    }
    util::MotionClaim claim;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Both axes (#2263): a slew, Park or FindHome in flight is superseded; its body sends nothing more.
        claim = motion_owner_.claim(util::MotionCommand::Slew);
        calibrated_ = false;
        needs_sync_ = false;
        sync_correction_active_ = false;
        at_home_ = false;
        parked_ = false;
        slew_in_progress_ = true;
        restore_tracking_after_slew_ = false;
    }
    try {
        protocol_.start_axis_home(CgxAxis::RightAscension);
        protocol_.start_axis_home(CgxAxis::Declination);
        static_cast<void>(confirm_home_search_started({CgxAxis::RightAscension, CgxAxis::Declination}));
        motion_slot_.start([this, claim, park_after](util::OperationContext& context) {
            const SlewClaimEnd end(*this, claim);
            run_home_operation(context, claim, park_after);
        });
        stop_slot_.clear_failure();
    } catch (...) {
        finish_slew_claim(claim);
        try {
            stop_axes_checked();
        } catch (const std::exception& error) {
            ALPACA_LOG_ERROR("Celestron CGX", std::string("Could not verify home-operation stop: ") + error.what());
            throw;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        calibrated_ = false;
        at_home_ = false;
        parked_ = false;
        slew_in_progress_ = false;
        restore_tracking_after_slew_ = false;
        throw;
    }
}

// #2213: which axis a home is waiting on, and whether it is still moving.
std::string CelestronCgxTelescopeDriver::home_axes_status() {
    std::string status;
    for (const auto axis : {CgxAxis::RightAscension, CgxAxis::Declination}) {
        status += status.empty() ? "" : "; ";
        status += axis == CgxAxis::RightAscension ? "RA" : "Dec";
        try {
            status += " home_done=" + std::to_string(protocol_.is_axis_home_done(axis));
            status += " slew_done=" + std::to_string(protocol_.is_axis_slew_done(axis));
            status += " position=" + std::to_string(protocol_.get_axis_position(axis));
        } catch (const std::exception& error) {
            status += std::string(" read failed: ") + error.what();
        }
    }
    return status;
}

void CelestronCgxTelescopeDriver::log_home_wait(std::chrono::steady_clock::time_point& next_log) {
    if (logging::get_log_level() > logging::LogLevel::Debug || clock_.now() < next_log) return;
    next_log = clock_.now() + std::chrono::seconds(10);
    ALPACA_LOG_DEBUG("Celestron CGX", "Home wait: " + home_axes_status());
}

void CelestronCgxTelescopeDriver::run_home_operation(util::OperationContext& context, const util::MotionClaim& claim,
                                                     bool park_after) {
    const auto deadline = clock_.now() + std::chrono::seconds(180);
    auto next_log = clock_.now() + std::chrono::seconds(10);
    HomeSearch search(clock_.now());
    try {
        while (true) {
            const auto reason = motion_stop_reason(context, claim);
            // A newer command owns the axes and the state it set (decision 0006 rule 6).
            if (reason == util::StopReason::Superseded) return;
            if (reason == util::StopReason::Cancelled) {
                stop_motion_worker_checked(claim);
                std::lock_guard<std::mutex> lock(mutex_);
                parked_ = false;
                at_home_ = false;
                calibrated_ = false;
                needs_sync_ = false;
                sync_correction_active_ = false;
                return;
            }
            if (protocol_.is_axis_home_done(CgxAxis::RightAscension) &&
                protocol_.is_axis_home_done(CgxAxis::Declination)) {
                if (!wait_for_slew_settle(context)) {
                    if (motion_stop_reason(context, claim) == util::StopReason::Cancelled)
                        stop_motion_worker_checked(claim);
                    return;
                }
                if (!run_motion_stage(context, claim, [this, park_after] {
                        // #2640: the controller's own counts at the sensor, before the labels overwrite them. Where
                        // its GOTO planner wraps may be fixed in this native frame. DEBUG only: two extra reads.
                        if (logging::get_log_level() <= logging::LogLevel::Debug)
                            ALPACA_LOG_DEBUG("Celestron CGX", "Home search done, native counts: " + home_axes_status());
                        const auto reference_generation = protocol_.session_generation();
                        protocol_.set_axis_position(CgxAxis::RightAscension, kCgxRaHomeEncoder);
                        protocol_.set_axis_position(CgxAxis::Declination, kCgxDecHomeEncoder);
                        std::unique_lock<std::mutex> lock(mutex_);
                        if (reference_generation != protocol_.session_generation())
                            throw AlpacaException("Celestron CGX session changed while establishing the home reference",
                                                  AlpacaError::DriverException);
                        ra_rehome_label_ = kCgxRaHomeEncoder;
                        dec_rehome_label_ = kCgxDecHomeEncoder;
                        // With the reference: a getter never sees the new reference with the old labels.
                        write_frame(kCgxRaHomeEncoder, kCgxDecHomeEncoder);
                        mechanical_hemisphere_ = info_.site_latitude_degrees.value_or(0.0) < 0.0 ? CgxHemisphere::South
                                                                                                 : CgxHemisphere::North;
                        calibrated_ = true;
                        reference_generation_ = reference_generation;
                        needs_sync_ = false;
                        sync_correction_active_ = false;
                        motion_state_unverified_ = false;
                        at_home_ = true;
                        parked_ = park_after;
                        slew_in_progress_ = false;
                        restore_tracking_after_slew_ = false;
                        if (park_after && tracking_) set_tracking_locked(false, lock);
                    })) {
                    return;
                }
                return;
            }
            if (clock_.now() >= deadline)
                throw AlpacaException("Celestron CGX home timed out: " + home_axes_status(),
                                      AlpacaError::DriverException);
            supervise_home_search(context, claim, search, deadline, "Celestron CGX home timed out");
            log_home_wait(next_log);
            static_cast<void>(context.wait_for(std::chrono::milliseconds(200)));
        }
    } catch (...) {
        if (superseded(context, claim)) {
            log_superseded_failure("FindHome or Park", std::current_exception());
            return;
        }
        stop_motion_worker_checked(claim);
        bool superseded_during_stop;
        {
            // Checked again under mutex_, which every claim takes: a newer command can claim the axes while the stop
            // waits for motion_start_mutex_, and its state is then not this body's to write (decision 0006 rule 6).
            std::lock_guard<std::mutex> lock(mutex_);
            superseded_during_stop = superseded(context, claim);
            if (!superseded_during_stop) {
                parked_ = false;
                at_home_ = false;
                calibrated_ = false;
                needs_sync_ = false;
                sync_correction_active_ = false;
            }
        }
        if (superseded_during_stop) {
            log_superseded_failure("FindHome or Park", std::current_exception());
            return;
        }
        throw;
    }
}

bool CelestronCgxTelescopeDriver::confirm_home_search_started(std::initializer_list<CgxAxis> axes,
                                                              const std::function<bool()>& stopped) {
    for (const auto axis : axes) {
        // Each axis gets its own window from its first read: a slow read of the one before cannot use it up.
        std::optional<util::TaskClock::clock::time_point> deadline;
        while (protocol_.is_axis_home_done(axis)) {
            if (!deadline) deadline = clock_.now() + kHomeStartConfirmWindow;
            if (stopped && stopped()) return false;
            if (clock_.now() >= *deadline)
                throw AlpacaException(std::string("Celestron CGX ") + (axis == CgxAxis::RightAscension ? "RA" : "Dec") +
                                          " reports home done with no home search running after MC_LEVEL_START; "
                                          "the home reference was not written. Run FindHome",
                                      AlpacaError::DriverException);
            clock_.sleep_for(std::chrono::milliseconds(20));
        }
    }
    return true;
}

void CelestronCgxTelescopeDriver::supervise_home_search(util::OperationContext& context, const util::MotionClaim& claim,
                                                        HomeSearch& search, util::TaskClock::clock::time_point deadline,
                                                        const char* timeout_message) {
    const auto timed_out = [&] {
        return AlpacaException(std::string(timeout_message) + ": " + home_axes_status(), AlpacaError::DriverException);
    };
    // No status reads in this error: the catch's stop must go out at once (each read can take 1 s over WiFi).
    const auto travel_exceeded = [&](std::size_t index, const std::string& what) {
        return AlpacaException(std::string("Celestron CGX home travel guard: ") + (index == 0 ? "RA" : "Dec") +
                                   " moved " + std::to_string(static_cast<int>(search.travel_degrees[index])) +
                                   " deg without finding home" + what,
                               AlpacaError::DriverException);
    };
    // The state poll reads both encoders every 250 ms; reading them here too doubled the bus traffic during a home,
    // where the SkyPortal adapter lost requests in bursts (rig 2026-11-21). So a poll frame read since the last sample
    // is taken; otherwise, and always for the first sample (a frame from before the home is never the baseline), both
    // encoders are read here. Every sample is a reading, so the sum runs from the last good one and no travel goes
    // uncounted; a poll read that went out just before an own read can only add to the sum.
    const auto polled = polled_.read();
    const std::uint64_t held = polled ? polled->value.poll_sequence : 0;
    std::array<std::uint32_t, 2> encoders{};
    if (polled && search.poll_sequence && polled->value.poll_sequence != *search.poll_sequence) {
        encoders = {polled->value.ra_encoder, polled->value.dec_encoder};
    } else {
        encoders = {protocol_.get_axis_position(CgxAxis::RightAscension),
                    protocol_.get_axis_position(CgxAxis::Declination)};
    }
    search.poll_sequence = held;
    for (const std::size_t index : {std::size_t{0}, std::size_t{1}}) {
        const std::uint32_t encoder = encoders[index];
        std::optional<std::uint32_t>& last = search.last_encoder[index];
        if (last)
            search.travel_degrees[index] +=
                std::abs(signed_cgx_encoder_delta(encoder, *last)) * 360.0 / kCgxEncoderStepsPerRevolution;
        last = encoder;
        if (search.travel_degrees[index] > kHomeTravelBoundDegrees)
            throw travel_exceeded(index,
                                  " (bound " + std::to_string(static_cast<int>(kHomeTravelBoundDegrees)) + " deg)");
    }
    for (const auto axis : {CgxAxis::RightAscension, CgxAxis::Declination}) {
        const std::size_t index = axis == CgxAxis::RightAscension ? 0 : 1;
        if (search.done[index] || clock_.now() - search.started[index] < kHomeSearchBound) continue;
        if (protocol_.is_axis_home_done(axis)) {
            search.done[index] = true;
            continue;
        }
        // The one exception to the no-retry rule (celestron instructions): never a third home.
        if (search.rehomed[index]) throw timed_out();
        // A re-home must fit inside the same bound: a half turn of room.
        if (search.travel_degrees[index] > kHomeRehomeTravelLimitDegrees)
            throw travel_exceeded(index, "; not re-homing past " +
                                             std::to_string(static_cast<int>(kHomeRehomeTravelLimitDegrees)) + " deg");
        // This axis only: the other one may be home-done already.
        if (!run_motion_stage(context, claim, [&] {
                const AuxFrame request{0x00, static_cast<std::uint8_t>(axis), 0x24, {0}};
                require_cgx_ack(protocol_.exchange(request), "axis stop");
            }))
            return;
        // For RA this still-wait delays the Dec check below; the 180 s home deadline bounds it.
        while (!protocol_.is_axis_slew_done(axis)) {
            // A cancel (AbortSlew, disconnect) or a supersede (a newer slew, Park or FindHome) ends the still-wait.
            if (motion_stop_reason(context, claim) != util::StopReason::None) return;
            if (clock_.now() >= deadline) throw timed_out();
            if (!context.wait_for(std::chrono::milliseconds(20))) return;
        }
        const auto position = protocol_.get_axis_position(axis);
        const std::chrono::duration<double> elapsed = clock_.now() - search.started[index];
        if (!run_motion_stage(context, claim, [&] { protocol_.start_axis_home(axis); })) return;
        if (!confirm_home_search_started({axis},
                                         [&] { return motion_stop_reason(context, claim) != util::StopReason::None; }))
            return;
        search.rehomed[index] = true;
        search.started[index] = clock_.now();
        const char* name = axis == CgxAxis::RightAscension ? "RA" : "Dec";
        ALPACA_LOG_INFO("Celestron CGX", std::string("Home search missed the sensor; re-homing ") + name +
                                             " once from position " + std::to_string(position) + " after " +
                                             std::to_string(static_cast<int>(elapsed.count())) + " s");
    }
}

void CelestronCgxTelescopeDriver::start_slew_operation(double ra, double dec) {
    util::check_equatorial_slew_arguments(ra, dec, "SlewToCoordinates");
    require_connected();
    require_known_motor_model();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        require_site_locked();
    }
    MotionTransition transition(*this, true);
    bool sync_correction_active;
    CgxPointingSide side;
    double lst;
    CgxHemisphere hemisphere;
    double latitude;
    bool needs_home_route;
    bool axes_may_coast = false;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (parked_) throw AlpacaException("Cannot slew while parked", AlpacaError::InvalidWhileParked);
        // A retarget of this driver's own slew still in flight is not Tracking off (ascom-contract, "Equatorial slews
        // require Tracking"): that slew turned Tracking off for its GOTO and owes the restore this one takes over.
        const bool retarget = motion_owner_.slew_running() && restore_tracking_after_slew_;
        // A flip slew homing to its rebase has no reference yet; FindHome and Park owe no restore, so they keep the
        // FindHome advice.
        if (!reference_valid_locked() && retarget)
            throw AlpacaException("Celestron CGX is homing for a meridian flip; wait for it or AbortSlew first",
                                  AlpacaError::InvalidOperation);
        if (!reference_valid_locked())
            throw AlpacaException("Run FindHome before slewing the Celestron CGX", AlpacaError::InvalidOperation);
        if (!tracking_ && !retarget)
            throw AlpacaException("Enable tracking before starting an equatorial slew", AlpacaError::InvalidOperation);
        if (!tracking_state_valid_)
            throw AlpacaException("Celestron CGX tracking state is unknown after a partial controller update",
                                  AlpacaError::DriverException);
        const CgxSkyPosition current = read_sky_position(lock);
        sync_correction_active = sync_correction_active_;
        const auto utc =
            std::chrono::system_clock::time_point{std::chrono::duration_cast<std::chrono::system_clock::duration>(
                std::chrono::duration<double>(current_utc_locked()))};
        lst = cgx_local_sidereal_time_hours(utc,
                                            configured_site_coordinate(info_.site_longitude_degrees, "SiteLongitude"));
        const double hour_angle = util::wrap_hour_angle(lst - ra);
        side = hour_angle >= 0.0 ? CgxPointingSide::Normal : CgxPointingSide::BeyondPole;
        needs_home_route = side != current.pointing_side;
        hemisphere = hemisphere_locked();
        latitude = configured_site_coordinate(info_.site_latitude_degrees, "SiteLatitude");
    }
    const auto positions = cgx_sky_position_to_encoders(ra, dec, lst, hemisphere, side);
    // #2640: the pose guard, before the tracking-off write and the claim, so a refusal sends nothing and leaves a slew
    // in flight alone. The flip GOTO checks its own target again after the home route.
    if (const auto refusal = cgx_encoder_target_refusal(positions.first, positions.second, hemisphere, side, latitude))
        throw AlpacaException("Celestron CGX GOTO refused: " + *refusal, AlpacaError::InvalidValue);
    util::MotionClaim claim;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // Disable tracking before publishing the slew transition. If the
        // controller update fails, the prior calibrated/idle state remains,
        // and so does the owner of a slew in flight: the claim comes after it.
        // A retarget finds Tracking already off and sends nothing here.
        if (tracking_) set_tracking_locked(false, lock);
        // Both axes (#2263): a slew in flight is superseded; its body sends nothing more. Read before the claim: a
        // slew, Park or FindHome still running may leave the axes coasting into the new GOTO (#2640).
        axes_may_coast = motion_owner_.slew_running();
        claim = motion_owner_.claim(util::MotionCommand::Slew);
        at_home_ = false;
        parked_ = false;
        slew_in_progress_ = true;
        restore_tracking_after_slew_ = true;
        if (needs_home_route) calibrated_ = false;
    }
    try {
        const auto start_axis = [this, needs_home_route](CgxAxis axis, std::uint32_t position) {
            if (needs_home_route)
                protocol_.start_axis_home(axis);
            else
                protocol_.goto_axis_fast(axis, position);
        };
        start_axis(CgxAxis::RightAscension, positions.first);
        start_axis(CgxAxis::Declination, positions.second);
        if (needs_home_route)
            static_cast<void>(confirm_home_search_started({CgxAxis::RightAscension, CgxAxis::Declination}));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            target_ra_hours_ = ra;
            target_dec_degrees_ = dec;
            target_ra_set_ = target_dec_set_ = true;
        }
        motion_slot_.start(
            [this, claim, ra, dec, side, needs_home_route, positions, axes_may_coast](util::OperationContext& context) {
                const SlewClaimEnd end(*this, claim);
                if (needs_home_route) {
                    run_flip_slew_operation(context, claim, ra, dec, side);
                } else {
                    run_slew_operation(context, claim, ra, dec, side, positions, axes_may_coast);
                }
            });
        stop_slot_.clear_failure();
    } catch (...) {
        finish_slew_claim(claim);
        try {
            stop_axes_checked();
        } catch (const std::exception& error) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                calibrated_ = false;
                at_home_ = false;
                parked_ = false;
            }
            ALPACA_LOG_ERROR("Celestron CGX", std::string("Could not verify slew-operation stop: ") + error.what());
            throw;
        }
        if (needs_home_route) {
            std::lock_guard<std::mutex> lock(mutex_);
            calibrated_ = false;
            needs_sync_ = sync_correction_active;
            sync_correction_active_ = false;
            at_home_ = false;
            parked_ = false;
            slew_in_progress_ = false;
            restore_tracking_after_slew_ = false;
        } else {
            std::unique_lock<std::mutex> lock(mutex_);
            const bool restore_tracking = restore_tracking_after_slew_ && reference_valid_locked();
            slew_in_progress_ = false;
            restore_tracking_after_slew_ = false;
            if (restore_tracking) {
                // The client must see why the slew did not start, not the restore's error (#1945).
                try {
                    set_tracking_locked(true, lock);
                } catch (const std::exception& error) {
                    ALPACA_LOG_ERROR(
                        "Celestron CGX",
                        std::string("Could not restore tracking after a failed slew start: ") + error.what());
                }
            }
        }
        throw;
    }
}

void CelestronCgxTelescopeDriver::run_slew_operation(util::OperationContext& context, const util::MotionClaim& claim,
                                                     double ra, double dec, CgxPointingSide destination_side,
                                                     std::pair<std::uint32_t, std::uint32_t> positions,
                                                     bool axes_may_coast) {
    const auto deadline = clock_.now() + std::chrono::seconds(180);
    try {
        static_cast<void>(finish_slew_operation(context, claim, deadline, ra, dec, destination_side,
                                                {positions.first, positions.second}, "Celestron CGX slew timed out",
                                                axes_may_coast));
    } catch (...) {
        if (superseded(context, claim)) {
            log_superseded_failure("Slew", std::current_exception());
            return;
        }
        stop_motion_worker_checked(claim);
        // A newer command can claim the axes while the stop waits for motion_start_mutex_ (decision 0006 rule 6).
        if (superseded(context, claim)) {
            log_superseded_failure("Slew", std::current_exception());
            return;
        }
        throw;
    }
}

void CelestronCgxTelescopeDriver::run_flip_slew_operation(util::OperationContext& context,
                                                          const util::MotionClaim& claim, double ra, double dec,
                                                          CgxPointingSide destination_side) {
    bool reference_established = false;
    // #2640: the route's home search may relabel or leave the axes anywhere, so until its rebase restores the labels
    // the reference is unknown, whether the route is cancelled, superseded or fails; FindHome establishes it again.
    const auto clear_cancelled_state = [this, &reference_established] {
        std::lock_guard<std::mutex> lock(mutex_);
        parked_ = false;
        at_home_ = false;
        calibrated_ = reference_established;
        if (!reference_established) sync_correction_active_ = false;
    };
    const auto deadline = clock_.now() + std::chrono::seconds(180);
    auto next_log = clock_.now() + std::chrono::seconds(10);
    HomeSearch search(clock_.now());
    try {
        while (true) {
            const auto reason = motion_stop_reason(context, claim);
            if (reason != util::StopReason::None) {
                if (reason == util::StopReason::Cancelled) {
                    stop_motion_worker_checked(claim);
                    clear_cancelled_state();
                }
                return;
            }
            if (protocol_.is_axis_home_done(CgxAxis::RightAscension) &&
                protocol_.is_axis_home_done(CgxAxis::Declination)) {
                double lst;
                CgxHemisphere hemisphere;
                double latitude;
                std::uint32_t ra_rehome_label;
                std::uint32_t dec_rehome_label;
                GotoTargets flip_leg;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ra_rehome_label = ra_rehome_label_;
                    dec_rehome_label = dec_rehome_label_;
                }
                // The labels by value; the outputs (lst, hemisphere, the flags) and the arguments by reference.
                if (!run_motion_stage(context, claim, [&, ra_rehome_label, dec_rehome_label] {
                        const auto reference_generation = protocol_.session_generation();
                        protocol_.set_axis_position(CgxAxis::RightAscension, ra_rehome_label);
                        protocol_.set_axis_position(CgxAxis::Declination, dec_rehome_label);
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            if (reference_generation != protocol_.session_generation())
                                throw AlpacaException(
                                    "Celestron CGX session changed while restoring the flip reference",
                                    AlpacaError::DriverException);
                            calibrated_ = true;
                            reference_generation_ = reference_generation;
                            reference_established = true;
                            at_home_ = true;
                            write_frame(ra_rehome_label, dec_rehome_label);
                            lst = cgx_local_sidereal_time_hours(
                                std::chrono::system_clock::time_point{
                                    std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                        std::chrono::duration<double>(current_utc_locked()))},
                                configured_site_coordinate(info_.site_longitude_degrees, "SiteLongitude"));
                            hemisphere = hemisphere_locked();
                            latitude = configured_site_coordinate(info_.site_latitude_degrees, "SiteLatitude");
                        }
                        const auto target = cgx_sky_position_to_encoders(ra, dec, lst, hemisphere, destination_side);
                        // #2640: the target, at the sidereal time after the route, passes the pose guard again.
                        if (const auto refusal = cgx_encoder_target_refusal(target.first, target.second, hemisphere,
                                                                            destination_side, latitude))
                            throw AlpacaException("Celestron CGX GOTO refused: " + *refusal, AlpacaError::InvalidValue);
                        flip_leg = {target.first, target.second};
                        protocol_.goto_axis_fast(CgxAxis::RightAscension, target.first);
                        protocol_.goto_axis_fast(CgxAxis::Declination, target.second);
                        std::lock_guard<std::mutex> lock(mutex_);
                        at_home_ = false;
                    })) {
                    if (!superseded(context, claim)) clear_cancelled_state();
                    return;
                }
                if (!finish_slew_operation(context, claim, deadline, ra, dec, destination_side, flip_leg,
                                           "Celestron CGX flip slew timed out")) {
                    if (motion_stop_reason(context, claim) == util::StopReason::Cancelled) clear_cancelled_state();
                    return;
                }
                return;
            }
            if (clock_.now() >= deadline)
                throw AlpacaException("Celestron CGX meridian home route timed out: " + home_axes_status(),
                                      AlpacaError::DriverException);
            supervise_home_search(context, claim, search, deadline, "Celestron CGX meridian home route timed out");
            log_home_wait(next_log);
            static_cast<void>(context.wait_for(std::chrono::milliseconds(200)));
        }
    } catch (...) {
        if (superseded(context, claim)) {
            log_superseded_failure("Meridian slew", std::current_exception());
            return;
        }
        stop_motion_worker_checked(claim);
        bool superseded_during_stop;
        {
            // Checked again under mutex_, as in the home catch (decision 0006 rule 6).
            std::lock_guard<std::mutex> lock(mutex_);
            superseded_during_stop = superseded(context, claim);
            if (!superseded_during_stop) {
                parked_ = false;
                at_home_ = false;
                if (!reference_established) {
                    calibrated_ = false;
                    needs_sync_ = false;
                    sync_correction_active_ = false;
                }
            }
        }
        if (superseded_during_stop) {
            log_superseded_failure("Meridian slew", std::current_exception());
            return;
        }
        throw;
    }
}

// The pulse moved an axis since the last frame. Publish the pair where it stopped before the slot ends, so the first
// read after IsPulseGuiding goes false shows it without waiting for the next poll. One write per pulse is occasional
// (#2035). mutex_ stays free (decision 0016); a failed read publishes nothing, and the poll reports the link. Under
// motion_start_mutex_, which every supersede of a pulse is made under: a pulse superseded after its last guide read
// reads and writes nothing (decision 0006 rule 6). A cancelled pulse still publishes where it stopped.
void CelestronCgxTelescopeDriver::publish_guide_end_frame(util::OperationContext& context,
                                                          const util::MotionClaim& claim) {
    run_hook_for_testing(pulse_pre_lock_hook_for_testing_, CgxPulsePreLockPoint::GuideEndFrame);
    const std::lock_guard<std::mutex> start_lock(motion_start_mutex_);
    if (superseded(context, claim)) return;
    try {
        const auto ra = protocol_.get_axis_position(CgxAxis::RightAscension);
        const auto dec = protocol_.get_axis_position(CgxAxis::Declination);
        write_frame(ra, dec);
    } catch (const std::exception& error) {
        ALPACA_LOG_WARN("Celestron CGX", std::string("Could not read the axes after a pulse guide: ") + error.what());
    }
}

void CelestronCgxTelescopeDriver::run_pulse_guide(util::OperationContext& context, const util::MotionClaim& claim,
                                                  CgxAxis axis, int velocity_percent, int first_chunk_centiseconds,
                                                  int remaining_centiseconds,
                                                  const std::shared_future<PulseGuideStart>& start_result) {
    const PulseGuideStart start = start_result.get();
    bool cancelled = start == PulseGuideStart::Ambiguous;
    if (cancelled) {
        // A lost guide acknowledgement desynchronizes AUX. Flush the fd already held (a closed, lost link throws
        // NotConnected and is never reopened, decision 0009 rule 4), then observe only the first packet; do not
        // retransmit it.
        protocol_.resync();
    }
    const auto operation_deadline =
        clock_.now() + std::chrono::milliseconds(
                           static_cast<std::int64_t>(first_chunk_centiseconds + remaining_centiseconds) * 10 + 5000);
    int current_chunk = first_chunk_centiseconds;
    auto segment_deadline = clock_.now() + std::chrono::milliseconds(current_chunk * 10) + kGuideEndMargin;
    while (true) {
        run_hook_for_testing(pulse_poll_hook_for_testing_);
        const auto reason = motion_stop_reason(context, claim);
        // A newer pulse on this axis, or a slew, Park or FindHome, owns it now (decision 0006 rule 6): send it
        // nothing more, the end-of-pulse read included.
        if (reason == util::StopReason::Superseded) return;
        cancelled = cancelled || reason != util::StopReason::None;
        bool active;
        {
            // The check above ran with no lock, so a supersede can land before the read: re-check under
            // motion_start_mutex_, which every supersede of a pulse is made under, and hold it across the read, as
            // publish_guide_end_frame() does (decision 0006 rule 6). The lock order allows it: motion_start_mutex_,
            // then the protocol I/O lock.
            run_hook_for_testing(pulse_pre_lock_hook_for_testing_, CgxPulsePreLockPoint::GuidePoll);
            const std::lock_guard<std::mutex> start_lock(motion_start_mutex_);
            if (superseded(context, claim)) return;
            active = protocol_.is_aux_guide_active(axis);
        }
        if (active) {
            if (clock_.now() >= operation_deadline || (cancelled && clock_.now() >= segment_deadline))
                throw AlpacaException("Celestron CGX pulse guide did not finish its current segment",
                                      AlpacaError::DriverException);
            if (cancelled) {
                // AUX pulses cannot be interrupted; allow the current <=2.55 s
                // segment to finish before the slot reports completion.
                clock_.sleep_for(std::chrono::milliseconds(20));
            } else {
                static_cast<void>(context.wait_for(std::chrono::milliseconds(20)));
            }
            continue;
        }
        if (cancelled || remaining_centiseconds == 0) {
            publish_guide_end_frame(context, claim);
            return;
        }
        current_chunk = std::min(255, remaining_centiseconds);
        if (!run_motion_stage(context, claim, [this, axis, velocity_percent, current_chunk] {
                protocol_.pulse_guide_axis(axis, velocity_percent, current_chunk);
            })) {
            return;
        }
        remaining_centiseconds -= current_chunk;
        segment_deadline = clock_.now() + std::chrono::milliseconds(current_chunk * 10) + kGuideEndMargin;
    }
}

std::unique_ptr<TelescopeDriver> create_celestron_cgx_telescope(int device_number,
                                                                const CelestronCgxConnectionInfo& info,
                                                                util::TaskClock& clock, CgxSystemNow system_now) {
    const auto require_range = [](const std::optional<double>& value, double minimum, double maximum,
                                  const char* message) {
        if (value && (!std::isfinite(*value) || *value < minimum || *value > maximum)) {
            throw AlpacaException(message, AlpacaError::InvalidValue);
        }
    };
    if (info.site_latitude_degrees) util::validate_latitude(*info.site_latitude_degrees);
    if (info.site_longitude_degrees) util::validate_longitude(*info.site_longitude_degrees);
    require_range(info.site_elevation_meters, -300.0, 10000.0, "SiteElevation must be in [-300, 10000] m");
    if (!std::isfinite(info.aperture_diameter_meters) || info.aperture_diameter_meters < 0.0) {
        throw AlpacaException("Aperture diameter must be non-negative", AlpacaError::InvalidValue);
    }
    if (!std::isfinite(info.focal_length_meters) || info.focal_length_meters < 0.0) {
        throw AlpacaException("Focal length must be non-negative", AlpacaError::InvalidValue);
    }
    return std::make_unique<CelestronCgxTelescopeDriver>(device_number, info, clock, std::move(system_now));
}

void set_state_poll_spawn_for_testing(TelescopeDriver& driver,
                                      std::function<std::thread(std::function<void()>)> spawn) {
    if (auto* cgx = dynamic_cast<CelestronCgxTelescopeDriver*>(&driver)) {
        cgx->set_state_poll_spawn_for_testing(std::move(spawn));
    }
}

void set_state_poll_paused_for_testing(TelescopeDriver& driver, bool paused) {
    if (auto* cgx = dynamic_cast<CelestronCgxTelescopeDriver*>(&driver)) {
        cgx->set_state_poll_paused_for_testing(paused);
    }
}

void poll_state_now_for_testing(TelescopeDriver& driver) {
    if (auto* cgx = dynamic_cast<CelestronCgxTelescopeDriver*>(&driver)) {
        cgx->poll_state_now_for_testing();
    }
}

std::optional<util::TaskClock::clock::time_point> consume_link_lost_at_for_testing(TelescopeDriver& driver) {
    if (auto* cgx = dynamic_cast<CelestronCgxTelescopeDriver*>(&driver)) {
        return cgx->consume_link_lost_at_for_testing();
    }
    return std::nullopt;
}

void set_stop_worker_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* cgx = dynamic_cast<CelestronCgxTelescopeDriver*>(&driver)) {
        cgx->set_stop_worker_hook_for_testing(std::move(hook));
    }
}

void set_drain_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* cgx = dynamic_cast<CelestronCgxTelescopeDriver*>(&driver)) {
        cgx->set_drain_hook_for_testing(std::move(hook));
    }
}

void set_pulse_poll_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* cgx = dynamic_cast<CelestronCgxTelescopeDriver*>(&driver)) {
        cgx->set_pulse_poll_hook_for_testing(std::move(hook));
    }
}

void set_pulse_pre_lock_hook_for_testing(TelescopeDriver& driver, std::function<void(CgxPulsePreLockPoint)> hook) {
    if (auto* cgx = dynamic_cast<CelestronCgxTelescopeDriver*>(&driver)) {
        cgx->set_pulse_pre_lock_hook_for_testing(std::move(hook));
    }
}

void set_guide_end_read_hook_for_testing(TelescopeDriver& driver,
                                         std::function<void(std::optional<std::chrono::milliseconds>)> hook) {
    if (auto* cgx = dynamic_cast<CelestronCgxTelescopeDriver*>(&driver)) {
        cgx->set_guide_end_read_hook_for_testing(std::move(hook));
    }
}

}  // namespace alpacacore::vendor::celestron
