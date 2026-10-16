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
#include <alpacacore/util/logging.h>
#include <alpacacore/util/poll_settle.h>
#include <alpacacore/vendor/celestron/celestron_cgx_geometry.h>
#include <alpacacore/vendor/celestron/celestron_cgx_protocol_wrapper.h>
#include <alpacacore/vendor/celestron/celestron_cgx_telescope_driver.h>
#include <alpacacore/version.h>

#include <array>
#include <atomic>
#include <cmath>
#include <exception>
#include <future>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace alpacacore::vendor::celestron {

namespace {

constexpr double kSiderealDegreesPerSecond = 15.0411 / 3600.0;
constexpr int kMaxPulseGuideDurationMs = 600000;
constexpr std::uint32_t kRaHomeEncoder = kCgxEncoderStepsPerRevolution / 4;
constexpr std::uint32_t kDecHomeEncoder = kCgxEncoderStepsPerRevolution / 2;
constexpr double kSlewArrivalToleranceDegrees = 3.0 / 3600.0;
constexpr double kMaxSlewCorrectionDegrees = 1.0;
constexpr int kMaxSlewCorrections = 3;
constexpr std::chrono::milliseconds kUtcStepTolerance{1000};
constexpr std::chrono::seconds kHostDisciplineResampleInterval{30};

double configured_site_coordinate(const std::optional<double>& coordinate, const char* name) {
    if (!coordinate) {
        throw AlpacaException(std::string("Celestron CGX ") + name + " is not configured",
                              AlpacaError::DriverException);
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

class CelestronCgxTelescopeDriver final : public TelescopeDriver, protected AsyncConnectable {
public:
    ALPACA_EXPOSE_CONNECT_ERROR()

    CelestronCgxTelescopeDriver(int device_number, CelestronCgxConnectionInfo info, util::TaskClock& clock,
                                CgxSystemNow system_now)
        : AsyncConnectable("Celestron CGX USB"),
          device_number_(device_number),
          info_(std::move(info)),
          clock_(clock),
          system_now_(std::move(system_now)),
          motion_slot_("Celestron CGX motion", motion_generation_, clock),
          pulse_slot_("Celestron CGX pulse guide", pulse_generation_, clock),
          stop_slot_("Celestron CGX stop", stop_generation_, clock),
          mechanical_hemisphere_(info_.site_latitude_degrees.value_or(0.0) < 0.0 ? CgxHemisphere::South
                                                                                 : CgxHemisphere::North),
          aperture_diameter_m_(info_.aperture_diameter_meters),
          aperture_area_m2_(std::numbers::pi * info_.aperture_diameter_meters * info_.aperture_diameter_meters / 4.0),
          focal_length_m_(info_.focal_length_meters) {
        if (!system_now_) system_now_ = [] { return std::chrono::system_clock::now(); };
        guide_rate_ = {kSiderealDegreesPerSecond * 0.5, kSiderealDegreesPerSecond * 0.5};
    }

    ~CelestronCgxTelescopeDriver() override {
        shutdown_connection();
        stop_slot_.cancel_all_and_join();
        motion_slot_.cancel_all_and_join();
        pulse_slot_.cancel_all_and_join();
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
    std::string get_name() const override { return "Celestron CGX/CGX-L USB Telescope"; }
    DeviceType get_device_type() const override { return DeviceType::Telescope; }
    std::string get_unique_id() const override { return "Celestron_CGX_USB_" + std::to_string(device_number_); }
    std::string get_description() const override { return "Celestron CGX/CGX-L direct USB mount"; }
    std::string get_driver_info() const override { return "AlpacaCore Celestron CGX AUX Driver"; }
    std::string get_driver_version() const override { return alpacacore::kVersion; }
    int get_interface_version() const override { return 4; }
    bool get_connected() const override { return connected_.load(); }
    void set_connected(bool connected) override;
    void connect() override { start_connection_task(true); }
    void disconnect() override { start_connection_task(false); }
    bool get_connecting() const override { return connection_task_active(); }
    std::vector<std::string> get_supported_actions() const override { return {}; }
    std::string action(std::string_view, std::string_view) override;
    bool can_action(std::string_view) const override { return false; }
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

private:
    enum class PulseGuideStart : std::uint8_t { Acknowledged, Ambiguous };

    class MotionTransition final {
    public:
        explicit MotionTransition(CelestronCgxTelescopeDriver& driver, bool cancel_motion = false);
        ~MotionTransition();
        MotionTransition(const MotionTransition&) = delete;
        MotionTransition& operator=(const MotionTransition&) = delete;

    private:
        CelestronCgxTelescopeDriver& driver_;
        // Own the entire handoff, including the mutex-released task joins.
        // Otherwise disconnect can see an empty slot whose body another initiator is still joining.
        std::unique_lock<std::mutex> connection_lock_;
        std::unique_lock<std::mutex> lock_;
    };

    template <typename Operation>
    bool run_motion_stage(util::OperationContext& context, Operation&& operation) {
        std::lock_guard<std::mutex> lock(motion_start_mutex_);
        if (context.stop_reason() != util::StopReason::None || motion_transition_ || disconnecting_ ||
            !connected_.load()) {
            return false;
        }
        std::forward<Operation>(operation)();
        return true;
    }

    [[noreturn]] static void not_implemented(const char* member, bool property = false);
    void require_connected() const;
    void require_healthy_transport() const;
    CgxSkyPosition read_sky_position_locked(std::uint32_t* ra_encoder = nullptr, std::uint32_t* dec_encoder = nullptr,
                                            bool reuse_stationary_telemetry = false) const;
    void reset_telemetry_locked() const {
        stationary_raw_pair_.reset();
        ++telemetry_reference_epoch_;
    }
    double current_utc_locked() const;
    std::chrono::system_clock::time_point client_utc_now_locked() const;
    bool client_utc_offset_survives_locked(std::chrono::system_clock::time_point system_now) const;
    void validate_site_and_coordinates(double ra, double dec) const;
    void start_home_operation(bool park_after);
    void run_home_operation(util::OperationContext& context, bool park_after);
    void start_slew_operation(double ra, double dec);
    void run_slew_operation(util::OperationContext& context, double ra, double dec, CgxPointingSide destination_side);
    void run_flip_slew_operation(util::OperationContext& context, double ra, double dec,
                                 CgxPointingSide destination_side);
    void run_pulse_guide(util::OperationContext& context, CgxAxis axis, int velocity_percent,
                         int first_chunk_centiseconds, int remaining_centiseconds,
                         const std::shared_future<PulseGuideStart>& start_result);
    void stop_axes_checked(std::chrono::milliseconds verification_budget = std::chrono::milliseconds(500));
    void stop_motion_worker_checked();
    void start_stop_operation(bool unpark);
    void run_stop_operation(util::OperationContext& context);
    void set_tracking_locked(bool tracking);
    void write_tracking_state_locked(bool tracking, int rate);
    bool finish_slew_operation(util::OperationContext& context, util::TaskClock::clock::time_point deadline,
                               double right_ascension_hours, double declination_degrees, CgxPointingSide pointing_side,
                               const char* timeout_message);
    bool wait_for_slew_completion(util::OperationContext& context, util::TaskClock::clock::time_point deadline,
                                  const char* timeout_message,
                                  std::chrono::milliseconds poll_interval = std::chrono::milliseconds(200));
    bool wait_for_slew_settle(util::OperationContext& context) const;
    void require_site_change_allowed_locked() const;
    void set_tracking_rate_locked(int rate);
    CgxHemisphere hemisphere_locked() const;
    bool reference_valid_locked() const {
        return calibrated_ && reference_generation_ == protocol_.session_generation();
    }
    std::pair<std::uint32_t, std::uint32_t> read_stopped_checkpoint();

    const int device_number_;
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
    mutable CelestronCgxProtocol protocol_;
    util::OperationGeneration motion_generation_;
    util::AsyncOperation motion_slot_;
    util::OperationGeneration pulse_generation_;
    util::AsyncOperation pulse_slot_;
    util::OperationGeneration stop_generation_;
    util::AsyncOperation stop_slot_;
    bool stop_pending_ = false;
    bool unpark_after_stop_ = false;
    bool calibrated_ = false;
    std::uint64_t reference_generation_ = 0;
    // Only measured labels are cached; sky geometry is recomputed at current UTC.
    // Guarded by mutex_; telemetry also holds motion_start_mutex_ across the read.
    mutable std::optional<std::pair<std::uint32_t, std::uint32_t>> stationary_raw_pair_;
    mutable util::TaskClock::clock::time_point telemetry_filled_at_{};
    mutable std::uint64_t telemetry_reference_epoch_ = 0;
    mutable std::array<std::uint64_t, 5> telemetry_key_{};
    std::optional<std::pair<std::uint32_t, std::uint32_t>> disconnected_checkpoint_;
    std::string reference_firmware_;
    // FindHome establishes the mechanical frame; later site writes do not reverse it.
    CgxHemisphere mechanical_hemisphere_;
    bool needs_sync_ = false;
    bool sync_correction_active_ = false;
    bool motion_state_unverified_ = false;
    bool slew_in_progress_ = false;
    bool restore_tracking_after_slew_ = false;
    // Controller labels to restore after hardware homing. Sync shifts both
    // labels without moving the mount; retaining that shift preserves the
    // calibration when a meridian flip re-homes both axes.
    std::uint32_t ra_rehome_label_ = kRaHomeEncoder;
    std::uint32_t dec_rehome_label_ = kDecHomeEncoder;
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
    mutable std::chrono::steady_clock::time_point utc_anchor_steady_{};
    mutable bool utc_offset_host_was_synchronized_ = false;
    mutable std::chrono::steady_clock::time_point next_utc_discipline_resample_{};
    GuideRate guide_rate_{0.0, 0.0};
    int tracking_rate_ = 0;
    int slew_settle_time_seconds_ = 0;
    std::string firmware_version_;
};

CelestronCgxTelescopeDriver::MotionTransition::MotionTransition(CelestronCgxTelescopeDriver& driver, bool cancel_motion)
    : driver_(driver), connection_lock_(driver.transition_mutex_), lock_(driver.motion_start_mutex_) {
    driver_.require_connected();
    if (driver_.motion_transition_ || driver_.disconnecting_) {
        throw AlpacaException("Celestron CGX is changing motion ownership", AlpacaError::InvalidOperation);
    }
    if (driver_.stop_slot_.running()) {
        throw AlpacaException("Celestron CGX stop is still finishing", AlpacaError::InvalidOperation);
    }
    if (!cancel_motion && driver_.motion_slot_.running()) {
        throw AlpacaException("Celestron CGX is already moving", AlpacaError::InvalidOperation);
    }
    driver_.motion_transition_ = true;
    driver_.pulse_slot_.cancel();
    if (cancel_motion) driver_.motion_slot_.cancel();
    lock_.unlock();
    try {
        driver_.pulse_slot_.cancel_all_and_join();
        if (cancel_motion) driver_.motion_slot_.cancel_all_and_join();
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
    if (!cancel_motion) {
        if (const auto failure = driver_.pulse_slot_.last_failure()) {
            driver_.motion_transition_ = false;
            throw AlpacaException("Celestron CGX pulse guide did not finish cleanly: " + *failure,
                                  AlpacaError::DriverException);
        }
    }
}

CelestronCgxTelescopeDriver::MotionTransition::~MotionTransition() { driver_.motion_transition_ = false; }

[[noreturn]] void CelestronCgxTelescopeDriver::not_implemented(const char* member, bool property) {
    throw AlpacaException(std::string("Celestron CGX USB does not support ") + member,
                          property ? AlpacaError::PropertyNotImplemented : AlpacaError::MethodNotImplemented);
}

void CelestronCgxTelescopeDriver::require_connected() const {
    if (!connected_.load()) {
        throw AlpacaException("Celestron CGX USB is not connected", AlpacaError::NotConnected);
    }
}

void CelestronCgxTelescopeDriver::require_healthy_transport() const {
    if (!protocol_.is_healthy())
        throw AlpacaException("Celestron CGX communications compromised; verify the link and confirm a stop",
                              AlpacaError::DriverException);
}

double CelestronCgxTelescopeDriver::current_utc_locked() const {
    const auto system_now = system_now_();
    const bool offset_survives = client_utc_offset_survives_locked(system_now);
    if (offset_survives && !utc_offset_host_was_synchronized_) {
        const auto steady_now = std::chrono::steady_clock::now();
        if (steady_now >= next_utc_discipline_resample_) {
            next_utc_discipline_resample_ = steady_now + kHostDisciplineResampleInterval;
            if (util::ClientUtcWarning::host_synchronized()) {
                utc_offset_host_was_synchronized_ = true;
                ALPACA_LOG_INFO("Celestron CGX",
                                "Host clock became NTP-disciplined after UTCDate; pointing by host time while "
                                "UTCDate readback continues to honor the client");
            }
        }
    }
    const auto pointing_now =
        util::HostClock::pointing_uses_client_offset(offset_survives, utc_offset_host_was_synchronized_)
            ? system_now + utc_offset_
            : system_now;
    return std::chrono::duration<double>(pointing_now.time_since_epoch()).count();
}

std::chrono::system_clock::time_point CelestronCgxTelescopeDriver::client_utc_now_locked() const {
    const auto system_now = system_now_();
    return client_utc_offset_survives_locked(system_now) ? system_now + utc_offset_ : system_now;
}

bool CelestronCgxTelescopeDriver::client_utc_offset_survives_locked(
    std::chrono::system_clock::time_point system_now) const {
    if (!has_utc_offset_) return false;
    const auto system_elapsed = system_now - utc_anchor_system_;
    const auto steady_elapsed = std::chrono::steady_clock::now() - utc_anchor_steady_;
    if (util::HostClock::clock_was_stepped(system_elapsed, steady_elapsed, kUtcStepTolerance)) {
        has_utc_offset_ = false;
        utc_offset_ = {};
        ALPACA_LOG_INFO("Celestron CGX", "Host clock was stepped after UTCDate; dropping the client time offset");
        return false;
    }
    return true;
}

CgxHemisphere CelestronCgxTelescopeDriver::hemisphere_locked() const { return mechanical_hemisphere_; }

void CelestronCgxTelescopeDriver::validate_site_and_coordinates(double ra, double dec) const {
    if (!std::isfinite(ra) || ra < 0.0 || ra >= 24.0 || !std::isfinite(dec) || dec < -90.0 || dec > 90.0) {
        throw AlpacaException("Celestron CGX coordinates must use RA [0,24) hours and Dec [-90,90] degrees",
                              AlpacaError::InvalidValue);
    }
}

CgxSkyPosition CelestronCgxTelescopeDriver::read_sky_position_locked(std::uint32_t* ra_encoder,
                                                                     std::uint32_t* dec_encoder,
                                                                     bool reuse_stationary_telemetry) const {
    try {
        require_connected();
        require_healthy_transport();  // A known fault must refuse even a warm frame.
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
        // Control callers remain fresh by default. Only the five public coordinate
        // getters opt in, holding motion_start_mutex_ before mutex_ so ownership
        // cannot change between this gate and the pair read/cache hit.
        const bool stationary = reuse_stationary_telemetry && (at_home_ || parked_) && tracking_state_valid_ &&
                                !tracking_ && !needs_sync_ && !slew_in_progress_ && !restore_tracking_after_slew_ &&
                                !motion_transition_ && !disconnecting_ && !motion_slot_.running() &&
                                !pulse_slot_.running() && !stop_slot_.running() && !motion_slot_.last_failure() &&
                                !pulse_slot_.last_failure() && !stop_slot_.last_failure();
        if (!stationary) reset_telemetry_locked();  // Includes Sync's fresh read before its label writes.
        const std::array key{reference_generation_, telemetry_reference_epoch_, motion_generation_.current(),
                             pulse_generation_.current(), stop_generation_.current()};
        if (telemetry_key_ != key) stationary_raw_pair_.reset();
        std::pair<std::uint32_t, std::uint32_t> raw;
        // Unobserved link loss is bounded to 30 ms from refill; hits never extend it.
        if (!stationary_raw_pair_ || clock_.now() - telemetry_filled_at_ >= std::chrono::milliseconds(30)) {
            stationary_raw_pair_.reset();  // Never fall back after a failed/partial refill.
            const auto ra = protocol_.get_axis_position(CgxAxis::RightAscension);
            const auto dec = protocol_.get_axis_position(CgxAxis::Declination);
            require_healthy_transport();
            if (!reference_valid_locked())
                throw AlpacaException("Celestron CGX session changed while reading coordinates",
                                      AlpacaError::DriverException);
            raw = {ra, dec};
            if (stationary) {
                stationary_raw_pair_ = raw;
                telemetry_filled_at_ = clock_.now();
                telemetry_key_ = key;
            }
        } else {
            raw = *stationary_raw_pair_;
        }
        const auto [ra, dec] = raw;
        if (ra_encoder != nullptr) *ra_encoder = ra;
        if (dec_encoder != nullptr) *dec_encoder = dec;
        const double longitude = configured_site_coordinate(info_.site_longitude_degrees, "SiteLongitude");
        const auto utc =
            std::chrono::system_clock::time_point{std::chrono::duration_cast<std::chrono::system_clock::duration>(
                std::chrono::duration<double>(current_utc_locked()))};
        return cgx_encoder_position_to_sky(ra, dec, cgx_local_sidereal_time_hours(utc, longitude), hemisphere_locked());
    } catch (...) {
        reset_telemetry_locked();
        throw;
    }
}

std::pair<std::uint32_t, std::uint32_t> CelestronCgxTelescopeDriver::read_stopped_checkpoint() {
    // This connection check uses wall time, independently of the operational
    // clock used by motion workers. Every exchange shares the whole-check budget.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    const auto budget = [&] {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
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
        util::default_task_clock().sleep_for(std::chrono::milliseconds(20));
    }
}

void CelestronCgxTelescopeDriver::set_connected(bool connected) {
    std::lock_guard<std::mutex> transition(transition_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) return;
        if (connected && consume_pending_disconnect(connected_.load())) return;
        if (connected_.load() == connected) return;
        reset_telemetry_locked();
    }

    if (connected) {
        bool site_is_configured = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            site_is_configured = info_.site_latitude_degrees.has_value() && info_.site_longitude_degrees.has_value();
        }
        if (!site_is_configured) {
            throw AlpacaException(
                "Set Celestron CGX SiteLatitude and SiteLongitude in the device settings before connecting",
                AlpacaError::DriverException);
        }
        protocol_.connect_serial(info_.port_path);
        try {
            const AuxFrame ra_version = protocol_.exchange(
                {0x00, static_cast<std::uint8_t>(CgxAxis::RightAscension), 0xfe, {}}, std::chrono::milliseconds(500));
            const AuxFrame dec_version = protocol_.exchange(
                {0x00, static_cast<std::uint8_t>(CgxAxis::Declination), 0xfe, {}}, std::chrono::milliseconds(500));
            const bool modern_version = ra_version.payload.size() == 4 && dec_version.payload.size() == 4;
            if (!modern_version && (ra_version.payload.size() != 2 || dec_version.payload.size() != 2)) {
                throw AlpacaException("Celestron CGX motor-controller identity response is malformed",
                                      AlpacaError::DriverException);
            }
            if (modern_version) {
                // Raw AUX CGX 7.17.25 returns major/minor + a big-endian build.
                // MC_GET_MODEL identifies both motors as CGX (0x1788); a reply
                // from each assumed switch address is not a valid identity gate.
                for (const auto axis : {CgxAxis::RightAscension, CgxAxis::Declination}) {
                    const AuxFrame model = protocol_.exchange({0x00, static_cast<std::uint8_t>(axis), 0x05, {}},
                                                              std::chrono::milliseconds(500));
                    if (model.payload != std::vector<std::uint8_t>{0x17, 0x88}) {
                        throw AlpacaException(
                            "Celestron direct USB requires verified CGX motor model 0x1788 on both axes",
                            AlpacaError::DriverException);
                    }
                }
            } else {
                const auto query_home_switch = [this](std::uint8_t address) {
                    try {
                        return protocol_.exchange({0x00, address, 0xfe, {}}, std::chrono::milliseconds(500));
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
            if (motion_state_unverified) stop_axes_checked();
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
                ra_rehome_label_ = kRaHomeEncoder;
                dec_rehome_label_ = kDecHomeEncoder;
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
            pulse_slot_.clear_failure();
            stop_slot_.clear_failure();
            connected_.store(true);
            {
                std::lock_guard<std::mutex> firmware_lock(firmware_mutex_);
                firmware_version_ = firmware;
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(firmware_mutex_);
                firmware_version_.clear();
            }
            connected_.store(false);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                calibrated_ = false;
                disconnected_checkpoint_.reset();
                reset_telemetry_locked();
            }
            protocol_.disconnect();
            throw;
        }
        return;
    }

    std::unique_lock<std::mutex> motion_start_lock(motion_start_mutex_);
    disconnecting_ = true;
    motion_transition_ = true;
    motion_slot_.cancel();
    pulse_slot_.cancel();
    motion_start_lock.unlock();
    stop_slot_.cancel_all_and_join();
    motion_slot_.cancel_all_and_join();
    pulse_slot_.cancel_all_and_join();
    // Coordinate reads intentionally wait behind stop/checkpoint I/O; cached-only reads may proceed.
    motion_start_lock.lock();
    std::exception_ptr stop_failure;
    if (protocol_.is_connected()) {
        try {
            stop_axes_checked();
            std::unique_lock<std::mutex> lock(mutex_);
            disconnected_checkpoint_.reset();
            if (reference_valid_locked()) {
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_failure || !disconnected_checkpoint_ || !reference_valid_locked()) {
            calibrated_ = false;
            needs_sync_ = false;
            sync_correction_active_ = false;
            disconnected_checkpoint_.reset();
            ra_rehome_label_ = kRaHomeEncoder;
            dec_rehome_label_ = kDecHomeEncoder;
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
    // without a reference. Only a failed stop/initialization releases ownership.
    if (stop_failure) protocol_.disconnect();
    motion_slot_.clear_failure();
    pulse_slot_.clear_failure();
    stop_slot_.clear_failure();
    motion_transition_ = false;
    disconnecting_ = false;
    if (stop_failure) std::rethrow_exception(stop_failure);
}

std::string CelestronCgxTelescopeDriver::action(std::string_view action_name, std::string_view) {
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
    std::lock_guard<std::mutex> motion_lock(motion_start_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto sky = read_sky_position_locked(nullptr, nullptr, true);
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
    std::lock_guard<std::mutex> motion_lock(motion_start_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto sky = read_sky_position_locked(nullptr, nullptr, true);
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
    std::lock_guard<std::mutex> motion_lock(motion_start_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    return read_sky_position_locked(nullptr, nullptr, true).declination_degrees;
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
    MotionTransition transition(*this);
    std::lock_guard<std::mutex> lock(mutex_);
    set_tracking_locked(tracking);
}

void CelestronCgxTelescopeDriver::set_tracking_locked(bool tracking) {
    if (tracking && parked_) {
        throw AlpacaException("Cannot enable tracking while parked", AlpacaError::InvalidWhileParked);
    }
    if (tracking && !reference_valid_locked()) {
        throw AlpacaException("Run FindHome before enabling Celestron CGX tracking", AlpacaError::InvalidOperation);
    }
    write_tracking_state_locked(tracking, tracking_rate_);
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
void CelestronCgxTelescopeDriver::set_guide_rate(const GuideRate& rate) {
    const auto valid = [](double value) {
        return std::isfinite(value) && value >= kSiderealDegreesPerSecond * 0.1 && value <= kSiderealDegreesPerSecond;
    };
    if (!valid(rate.ra) || !valid(rate.dec)) {
        throw AlpacaException("Guide rates must be between 10% and 100% of sidereal", AlpacaError::InvalidValue);
    }
    require_connected();
    std::lock_guard<std::mutex> lock(mutex_);
    guide_rate_ = rate;
}
bool CelestronCgxTelescopeDriver::get_is_pulse_guiding() const {
    require_connected();
    require_healthy_transport();
    if (const auto failure = pulse_slot_.last_failure()) {
        throw AlpacaException("Celestron CGX pulse guide failed: " + *failure, AlpacaError::DriverException);
    }
    if (pulse_slot_.running()) return true;
    return protocol_.is_aux_guide_active(CgxAxis::RightAscension) ||
           protocol_.is_aux_guide_active(CgxAxis::Declination);
}
double CelestronCgxTelescopeDriver::get_right_ascension() const {
    std::lock_guard<std::mutex> motion_lock(motion_start_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    return read_sky_position_locked(nullptr, nullptr, true).right_ascension_hours;
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
    std::lock_guard<std::mutex> motion_lock(motion_start_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    return read_sky_position_locked(nullptr, nullptr, true).pointing_side == CgxPointingSide::Normal ? 0 : 1;
}
void CelestronCgxTelescopeDriver::set_side_of_pier(int side) {
    if (side != 0 && side != 1) throw AlpacaException("SideOfPier must be 0 or 1", AlpacaError::InvalidValue);
    require_connected();
    not_implemented("SideOfPier writes", true);
}
int CelestronCgxTelescopeDriver::get_destination_side_of_pier(double ra, double dec) const {
    validate_site_and_coordinates(ra, dec);
    require_connected();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto utc =
        std::chrono::system_clock::time_point{std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::duration<double>(current_utc_locked()))};
    const double lst =
        cgx_local_sidereal_time_hours(utc, configured_site_coordinate(info_.site_longitude_degrees, "SiteLongitude"));
    double ha = std::fmod(lst - ra + 12.0, 24.0);
    if (ha < 0.0) ha += 24.0;
    ha -= 12.0;
    return ha >= 0.0 ? 0 : 1;
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
                                                           util::TaskClock::clock::time_point deadline,
                                                           const char* timeout_message,
                                                           std::chrono::milliseconds poll_interval) {
    while (true) {
        const auto reason = context.stop_reason();
        if (reason != util::StopReason::None) {
            if (reason == util::StopReason::Cancelled) stop_motion_worker_checked();
            return false;
        }
        if (protocol_.is_axis_slew_done(CgxAxis::RightAscension) && protocol_.is_axis_slew_done(CgxAxis::Declination)) {
            return true;
        }
        if (clock_.now() >= deadline) throw AlpacaException(timeout_message, AlpacaError::DriverException);
        static_cast<void>(context.wait_for(poll_interval));
    }
}

bool CelestronCgxTelescopeDriver::finish_slew_operation(util::OperationContext& context,
                                                        util::TaskClock::clock::time_point deadline,
                                                        double right_ascension_hours, double declination_degrees,
                                                        CgxPointingSide pointing_side, const char* timeout_message) {
    int corrections = 0;
    while (true) {
        if (!wait_for_slew_completion(context, deadline, timeout_message,
                                      std::chrono::milliseconds(corrections == 0 ? 200 : 20)))
            return false;
        bool arrived = false;
        if (!run_motion_stage(context, [&] {
                std::lock_guard<std::mutex> lock(mutex_);
                const CgxSkyPosition actual = read_sky_position_locked();
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
                    set_tracking_locked(true);
                    arrived = true;
                    return;
                }
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
                const auto utc = std::chrono::system_clock::time_point{
                    std::chrono::duration_cast<std::chrono::system_clock::duration>(
                        std::chrono::duration<double>(current_utc_locked()))};
                const double lst = cgx_local_sidereal_time_hours(
                    utc, configured_site_coordinate(info_.site_longitude_degrees, "SiteLongitude"));
                const auto target = cgx_sky_position_to_encoders(right_ascension_hours, declination_degrees, lst,
                                                                 hemisphere_locked(), pointing_side);
                if (ra_error_degrees > kSlewArrivalToleranceDegrees)
                    protocol_.goto_axis_slow(CgxAxis::RightAscension, target.first);
                if (dec_error_degrees > kSlewArrivalToleranceDegrees)
                    protocol_.goto_axis_slow(CgxAxis::Declination, target.second);
                ++corrections;
            })) {
            return false;
        }
        if (!arrived) continue;
        if (!wait_for_slew_settle(context)) {
            if (context.stop_reason() == util::StopReason::Cancelled) stop_motion_worker_checked();
            return false;
        }
        return run_motion_stage(context, [this] {
            std::lock_guard<std::mutex> lock(mutex_);
            slew_in_progress_ = false;
            restore_tracking_after_slew_ = false;
        });
    }
}

double CelestronCgxTelescopeDriver::get_sidereal_time() const {
    require_connected();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto utc =
        std::chrono::system_clock::time_point{std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::duration<double>(current_utc_locked()))};
    return cgx_local_sidereal_time_hours(utc,
                                         configured_site_coordinate(info_.site_longitude_degrees, "SiteLongitude"));
}

void CelestronCgxTelescopeDriver::set_tracking_rate_locked(int rate) {
    if (rate < 0 || rate > 2) {
        throw AlpacaException("TrackingRate must be Sidereal, Lunar, or Solar", AlpacaError::InvalidValue);
    }
    if (!tracking_state_valid_)
        throw AlpacaException("Set Tracking before changing TrackingRate after a partial controller update",
                              AlpacaError::InvalidOperation);
    write_tracking_state_locked(tracking_, rate);
    tracking_rate_ = rate;
}

void CelestronCgxTelescopeDriver::write_tracking_state_locked(bool tracking, int rate) {
    // Every nonzero rate path, including TrackingRate, needs the same reference.
    // Zero-rate cleanup remains available after a session fault.
    if (tracking && !reference_valid_locked())
        throw AlpacaException("Run FindHome before enabling Celestron CGX tracking", AlpacaError::InvalidOperation);
    const auto write = [this](bool enabled, int selected_rate, const char* operation) {
        std::exception_ptr failure;
        const auto send = [&](CgxAxis axis, std::uint8_t command, std::vector<std::uint8_t> payload) {
            try {
                const AuxFrame request{0x00, static_cast<std::uint8_t>(axis), command, std::move(payload)};
                require_cgx_ack(protocol_.exchange(request, std::chrono::milliseconds(500)), operation);
            } catch (...) {
                if (!failure) failure = std::current_exception();
            }
        };
        if (enabled) {
            send(CgxAxis::Declination, 0x06, {0, 0, 0});
            const auto command = hemisphere_locked() == CgxHemisphere::South ? 0x07 : 0x06;
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

    if (const auto failure = write(tracking, rate, "tracking command")) {
        const bool previous_state_valid = tracking_state_valid_;
        bool rollback_succeeded = false;
        try {
            // Lost matching invalidates the reference. Recover on the owned fd
            // and leave tracking off rather than resume an uncertain frame.
            protocol_.connect_serial(info_.port_path);
            rollback_succeeded = !write(false, tracking_rate_, "tracking rollback");
            if (rollback_succeeded) tracking_ = false;
        } catch (...) {
            rollback_succeeded = false;
        }
        tracking_state_valid_ = previous_state_valid && rollback_succeeded;
        if (!rollback_succeeded) {
            ALPACA_LOG_ERROR("Celestron CGX", "Tracking update and rollback both failed; tracking readback is unknown");
        }
        std::rethrow_exception(failure);
    }
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
        pulse_slot_.running() || stop_slot_.running())
        throw AlpacaException("Cannot change Celestron CGX site coordinates while motion is in progress",
                              AlpacaError::InvalidOperation);
}

void CelestronCgxTelescopeDriver::set_site_latitude(double latitude) {
    if (!std::isfinite(latitude) || latitude < -90.0 || latitude > 90.0)
        throw AlpacaException("SiteLatitude must be in [-90, 90] degrees", AlpacaError::InvalidValue);
    std::lock_guard<std::mutex> start_lock(motion_start_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    require_connected();
    if (info_.site_latitude_degrees == latitude) return;
    require_site_change_allowed_locked();
    info_.site_latitude_degrees = latitude;
}
void CelestronCgxTelescopeDriver::set_site_longitude(double longitude) {
    if (!std::isfinite(longitude) || longitude < -180.0 || longitude > 180.0)
        throw AlpacaException("SiteLongitude must be in [-180, 180] degrees", AlpacaError::InvalidValue);
    std::lock_guard<std::mutex> start_lock(motion_start_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    require_connected();
    if (info_.site_longitude_degrees == longitude) return;
    require_site_change_allowed_locked();
    info_.site_longitude_degrees = longitude;
}
bool CelestronCgxTelescopeDriver::get_slewing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    require_connected();
    if (stop_pending_ || stop_slot_.running()) return true;
    require_healthy_transport();
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
    if (!std::isfinite(dec) || dec < -90.0 || dec > 90.0)
        throw AlpacaException("TargetDeclination must be in [-90, 90] degrees", AlpacaError::InvalidValue);
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
    if (!std::isfinite(ra) || ra < 0.0 || ra >= 24.0)
        throw AlpacaException("TargetRightAscension must be in [0, 24) hours", AlpacaError::InvalidValue);
    require_connected();
    std::lock_guard<std::mutex> lock(mutex_);
    if (parked_) {
        throw AlpacaException("Cannot set TargetRightAscension while parked", AlpacaError::InvalidWhileParked);
    }
    target_ra_hours_ = ra;
    target_ra_set_ = true;
}
void CelestronCgxTelescopeDriver::set_tracking_rate(int rate) {
    if (rate < 0 || rate > 2)
        throw AlpacaException("TrackingRate must be Sidereal, Lunar, or Solar", AlpacaError::InvalidValue);
    require_connected();
    MotionTransition transition(*this);
    std::lock_guard<std::mutex> lock(mutex_);
    set_tracking_rate_locked(rate);
}
std::chrono::system_clock::time_point CelestronCgxTelescopeDriver::get_utc_date() const {
    require_connected();
    std::lock_guard<std::mutex> lock(mutex_);
    return client_utc_now_locked();
}
void CelestronCgxTelescopeDriver::set_utc_date(std::chrono::system_clock::time_point utc) {
    require_connected();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto system_now = system_now_();
    utc_offset_ = utc - system_now;
    utc_anchor_system_ = system_now;
    utc_anchor_steady_ = std::chrono::steady_clock::now();
    has_utc_offset_ = true;
    utc_offset_host_was_synchronized_ = util::ClientUtcWarning::host_synchronized();
    next_utc_discipline_resample_ = std::chrono::steady_clock::now() + kHostDisciplineResampleInterval;
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
    if (direction < 0 || direction > 3 || duration_ms < 0 || duration_ms > kMaxPulseGuideDurationMs) {
        throw AlpacaException("PulseGuide direction or duration is out of range", AlpacaError::InvalidValue);
    }
    require_connected();
    std::lock_guard<std::mutex> start_lock(motion_start_mutex_);
    require_connected();
    if (motion_transition_ || disconnecting_)
        throw AlpacaException("Celestron CGX is changing motion ownership", AlpacaError::InvalidOperation);
    if (motion_slot_.running() || pulse_slot_.running()) {
        throw AlpacaException("PulseGuide cannot overlap telescope motion or another guide pulse",
                              AlpacaError::InvalidOperation);
    }
    if (const auto failure = pulse_slot_.last_failure()) {
        throw AlpacaException("Celestron CGX pulse guide remains unresolved: " + *failure,
                              AlpacaError::DriverException);
    }

    CgxAxis axis;
    int velocity;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (parked_) throw AlpacaException("Cannot PulseGuide while parked", AlpacaError::InvalidWhileParked);
        if (!tracking_state_valid_)
            throw AlpacaException("Celestron CGX tracking state is unknown after a partial controller update",
                                  AlpacaError::DriverException);
        if (!tracking_)
            throw AlpacaException("PulseGuide requires tracking to be enabled", AlpacaError::InvalidOperation);
        if (!reference_valid_locked())
            throw AlpacaException("Run FindHome before PulseGuide", AlpacaError::InvalidOperation);
        if (duration_ms == 0) return;
        const auto sky = read_sky_position_locked();
        const bool dec_axis = direction == 0 || direction == 1;
        axis = dec_axis ? CgxAxis::Declination : CgxAxis::RightAscension;
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
    }

    const int total_centiseconds = std::max(1, static_cast<int>(std::lround(duration_ms / 10.0)));
    const int first_chunk = std::min(255, total_centiseconds);
    auto start_promise = std::make_shared<std::promise<PulseGuideStart>>();
    const auto start_result = start_promise->get_future().share();
    pulse_slot_.start([this, axis, velocity, first_chunk, remaining = total_centiseconds - first_chunk,
                       start_result](util::OperationContext& context) {
        run_pulse_guide(context, axis, velocity, first_chunk, remaining, start_result);
    });
    try {
        protocol_.pulse_guide_axis(axis, velocity, first_chunk);
        start_promise->set_value(PulseGuideStart::Acknowledged);
    } catch (...) {
        // The command may have applied even though its acknowledgement did not.
        // Keep the worker in charge of this <=2.55 s packet; never chain the
        // remaining request or let another motion reuse the axis meanwhile.
        start_promise->set_value(PulseGuideStart::Ambiguous);
        throw;
    }
}
void CelestronCgxTelescopeDriver::set_park() { not_implemented("SetPark"); }
void CelestronCgxTelescopeDriver::slew_to_coordinates(double, double) {
    require_connected();
    not_implemented("SlewToCoordinates");
}
void CelestronCgxTelescopeDriver::slew_to_coordinates_async(double ra, double dec) { start_slew_operation(ra, dec); }
void CelestronCgxTelescopeDriver::slew_to_target() { not_implemented("SlewToTarget"); }
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
    validate_site_and_coordinates(ra, dec);
    require_connected();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (parked_) throw AlpacaException("Cannot sync while parked", AlpacaError::InvalidWhileParked);
    }
    MotionTransition transition(*this);
    std::lock_guard<std::mutex> lock(mutex_);
    if (parked_) throw AlpacaException("Cannot sync while parked", AlpacaError::InvalidWhileParked);
    if (!reference_valid_locked())
        throw AlpacaException("Run FindHome before SyncToCoordinates", AlpacaError::InvalidOperation);
    const bool restore_tracking = tracking_;
    bool reference_write_started = false;
    try {
        if (restore_tracking) set_tracking_locked(false);
        // A rate-off ACK can precede encoder standstill. Never re-label a
        // drifting axis, including when the client just switched tracking off.
        util::ConsecutiveSettle settle(3, 15);
        const auto settle_deadline = clock_.now() + std::chrono::milliseconds(500);
        std::optional<std::pair<std::uint32_t, std::uint32_t>> previous;
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
        std::uint32_t current_ra_encoder;
        std::uint32_t current_dec_encoder;
        const CgxSkyPosition current = read_sky_position_locked(&current_ra_encoder, &current_dec_encoder);
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
        // Sync re-labels the current mechanical position. Keep the homed
        // reference fixed, and remember the equivalent home labels in this
        // shifted frame for any later meridian-route re-home.
        ra_rehome_label_ = next_ra_rehome_label;
        dec_rehome_label_ = next_dec_rehome_label;
        at_home_ = false;
        target_ra_hours_ = ra;
        target_dec_degrees_ = dec;
        target_ra_set_ = target_dec_set_ = true;
        sync_correction_active_ = true;
        needs_sync_ = false;
    } catch (...) {
        if (reference_write_started) {
            calibrated_ = false;
            needs_sync_ = true;
            sync_correction_active_ = false;
        }
        throw;
    }
    if (restore_tracking) set_tracking_locked(true);
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
    motion_slot_.cancel();
    pulse_slot_.cancel();
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
        pulse_slot_.cancel_all_and_join();
        motion_slot_.cancel_all_and_join();
        stop_axes_checked(std::chrono::seconds(3));
        motion_slot_.clear_failure();
        pulse_slot_.clear_failure();
        std::lock_guard<std::mutex> start_lock(motion_start_mutex_);
        std::lock_guard<std::mutex> lock(mutex_);
        const bool restore_tracking = restore_tracking_requested && reference_valid_locked() && !parked_;
        if (unpark_after_stop_) parked_ = false;
        if (restore_tracking) set_tracking_locked(true);
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
void CelestronCgxTelescopeDriver::slew_to_alt_az(double, double) { not_implemented("SlewToAltAz"); }
void CelestronCgxTelescopeDriver::slew_to_alt_az_async(double, double) { not_implemented("SlewToAltAzAsync"); }
void CelestronCgxTelescopeDriver::sync_to_alt_az(double, double) { not_implemented("SyncToAltAz"); }

void CelestronCgxTelescopeDriver::stop_motion_worker_checked() {
    // Serialize check + dispatch with the explicit-stop ownership publication.
    // Never take transition_mutex_: the reaper holds it while joining us.
    std::lock_guard<std::mutex> start_lock(motion_start_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_pending_) return;
    }
    stop_axes_checked();
}

void CelestronCgxTelescopeDriver::stop_axes_checked(std::chrono::milliseconds verification_budget) {
    const std::array axes{CgxAxis::RightAscension, CgxAxis::Declination};
    bool stop_acknowledged = false;
    {
        // Freeze tracking reads across the complete stop write pair. Unknown is
        // a retained failure state, not an observable normal-dispatch transient.
        std::lock_guard<std::mutex> lock(mutex_);
        tracking_state_valid_ = false;
        for (int attempt = 0; attempt < 3; ++attempt) {
            std::exception_ptr acknowledgement_failure;
            for (const auto axis : axes) {
                try {
                    const AuxFrame request{0x00, static_cast<std::uint8_t>(axis), 0x24, {0}};
                    require_cgx_ack(protocol_.exchange(request, std::chrono::milliseconds(500)), "axis stop");
                } catch (...) {
                    if (!acknowledgement_failure) acknowledgement_failure = std::current_exception();
                    try {
                        // AUX synchronization is shared: a missing RA reply must
                        // not prevent the responsive Dec controller receiving stop.
                        protocol_.connect_serial(info_.port_path);
                    } catch (const std::exception& error) {
                        ALPACA_LOG_WARN(
                            "Celestron CGX",
                            std::string("Could not reconnect AUX before retrying the axis stop: ") + error.what());
                    } catch (...) {
                        ALPACA_LOG_WARN("Celestron CGX", "Could not reconnect AUX before retrying the axis stop");
                    }
                }
            }
            if (!acknowledgement_failure) {
                stop_acknowledged = true;
                break;
            }
        }
        tracking_ = false;
        tracking_state_valid_ = stop_acknowledged;
    }
    std::exception_ptr verification_failure;
    const auto stop_deadline = clock_.now() + verification_budget;
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
    if (park_after && get_at_park()) return;
    MotionTransition transition(*this);
    if (park_after && get_at_park()) return;
    if (!park_after && get_at_park())
        throw AlpacaException("Cannot FindHome while parked; Unpark first", AlpacaError::InvalidWhileParked);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tracking_) set_tracking_locked(false);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        calibrated_ = false;
        needs_sync_ = false;
        sync_correction_active_ = false;
        at_home_ = false;
        parked_ = false;
        slew_in_progress_ = true;
        reset_telemetry_locked();
        restore_tracking_after_slew_ = false;
    }
    try {
        protocol_.start_axis_home(CgxAxis::RightAscension);
        protocol_.start_axis_home(CgxAxis::Declination);
        motion_slot_.start(
            [this, park_after](util::OperationContext& context) { run_home_operation(context, park_after); });
        stop_slot_.clear_failure();
    } catch (...) {
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

void CelestronCgxTelescopeDriver::run_home_operation(util::OperationContext& context, bool park_after) {
    const auto deadline = clock_.now() + std::chrono::seconds(180);
    try {
        while (true) {
            const auto reason = context.stop_reason();
            if (reason != util::StopReason::None) {
                if (reason == util::StopReason::Cancelled) stop_motion_worker_checked();
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
                    if (context.stop_reason() == util::StopReason::Cancelled) stop_motion_worker_checked();
                    return;
                }
                if (!run_motion_stage(context, [this, park_after] {
                        const auto reference_generation = protocol_.session_generation();
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            reset_telemetry_locked();  // Invalidate before same-session home label writes.
                        }
                        protocol_.set_axis_position(CgxAxis::RightAscension, kRaHomeEncoder);
                        protocol_.set_axis_position(CgxAxis::Declination, kDecHomeEncoder);
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (reference_generation != protocol_.session_generation())
                            throw AlpacaException("Celestron CGX session changed while establishing the home reference",
                                                  AlpacaError::DriverException);
                        ra_rehome_label_ = kRaHomeEncoder;
                        dec_rehome_label_ = kDecHomeEncoder;
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
                        if (park_after && tracking_) set_tracking_locked(false);
                    })) {
                    return;
                }
                return;
            }
            if (clock_.now() >= deadline)
                throw AlpacaException("Celestron CGX home timed out", AlpacaError::DriverException);
            static_cast<void>(context.wait_for(std::chrono::milliseconds(200)));
        }
    } catch (...) {
        stop_motion_worker_checked();
        std::lock_guard<std::mutex> lock(mutex_);
        parked_ = false;
        at_home_ = false;
        calibrated_ = false;
        needs_sync_ = false;
        sync_correction_active_ = false;
        throw;
    }
}

void CelestronCgxTelescopeDriver::start_slew_operation(double ra, double dec) {
    validate_site_and_coordinates(ra, dec);
    require_connected();
    MotionTransition transition(*this);
    bool sync_correction_active;
    CgxPointingSide side;
    double lst;
    CgxHemisphere hemisphere;
    bool needs_home_route;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (parked_) throw AlpacaException("Cannot slew while parked", AlpacaError::InvalidWhileParked);
        if (!reference_valid_locked())
            throw AlpacaException("Run FindHome before slewing the Celestron CGX", AlpacaError::InvalidOperation);
        if (!tracking_)
            throw AlpacaException("Enable tracking before starting an equatorial slew", AlpacaError::InvalidOperation);
        if (!tracking_state_valid_)
            throw AlpacaException("Celestron CGX tracking state is unknown after a partial controller update",
                                  AlpacaError::DriverException);
        const CgxSkyPosition current = read_sky_position_locked();
        sync_correction_active = sync_correction_active_;
        const auto utc =
            std::chrono::system_clock::time_point{std::chrono::duration_cast<std::chrono::system_clock::duration>(
                std::chrono::duration<double>(current_utc_locked()))};
        lst = cgx_local_sidereal_time_hours(utc,
                                            configured_site_coordinate(info_.site_longitude_degrees, "SiteLongitude"));
        double hour_angle = std::fmod(lst - ra + 12.0, 24.0);
        if (hour_angle < 0.0) hour_angle += 24.0;
        hour_angle -= 12.0;
        side = hour_angle >= 0.0 ? CgxPointingSide::Normal : CgxPointingSide::BeyondPole;
        needs_home_route = side != current.pointing_side;
        hemisphere = hemisphere_locked();
    }
    const auto positions = cgx_sky_position_to_encoders(ra, dec, lst, hemisphere, side);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Disable tracking before publishing the slew transition. If the
        // controller update fails, the prior calibrated/idle state remains.
        set_tracking_locked(false);
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
        {
            std::lock_guard<std::mutex> lock(mutex_);
            target_ra_hours_ = ra;
            target_dec_degrees_ = dec;
            target_ra_set_ = target_dec_set_ = true;
        }
        motion_slot_.start([this, ra, dec, side, needs_home_route](util::OperationContext& context) {
            if (needs_home_route) {
                run_flip_slew_operation(context, ra, dec, side);
            } else {
                run_slew_operation(context, ra, dec, side);
            }
        });
        stop_slot_.clear_failure();
    } catch (...) {
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
            std::lock_guard<std::mutex> lock(mutex_);
            const bool restore_tracking = restore_tracking_after_slew_ && reference_valid_locked();
            slew_in_progress_ = false;
            restore_tracking_after_slew_ = false;
            if (restore_tracking) set_tracking_locked(true);
        }
        throw;
    }
}

void CelestronCgxTelescopeDriver::run_slew_operation(util::OperationContext& context, double ra, double dec,
                                                     CgxPointingSide destination_side) {
    const auto deadline = clock_.now() + std::chrono::seconds(180);
    try {
        static_cast<void>(
            finish_slew_operation(context, deadline, ra, dec, destination_side, "Celestron CGX slew timed out"));
    } catch (...) {
        stop_motion_worker_checked();
        throw;
    }
}

void CelestronCgxTelescopeDriver::run_flip_slew_operation(util::OperationContext& context, double ra, double dec,
                                                          CgxPointingSide destination_side) {
    bool rebase_started = false;
    bool reference_established = false;
    const auto clear_cancelled_state = [this, &rebase_started, &reference_established] {
        std::lock_guard<std::mutex> lock(mutex_);
        parked_ = false;
        at_home_ = false;
        if (rebase_started && !reference_established) {
            calibrated_ = false;
            sync_correction_active_ = false;
        } else {
            // Before rebase, or after restoring the synced home labels, the
            // existing encoder-to-sky calibration remains valid on cancellation.
            calibrated_ = true;
        }
    };
    const auto deadline = clock_.now() + std::chrono::seconds(180);
    try {
        while (true) {
            const auto reason = context.stop_reason();
            if (reason != util::StopReason::None) {
                if (reason == util::StopReason::Cancelled) {
                    stop_motion_worker_checked();
                    clear_cancelled_state();
                }
                return;
            }
            if (protocol_.is_axis_home_done(CgxAxis::RightAscension) &&
                protocol_.is_axis_home_done(CgxAxis::Declination)) {
                double lst;
                CgxHemisphere hemisphere;
                std::uint32_t ra_rehome_label;
                std::uint32_t dec_rehome_label;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ra_rehome_label = ra_rehome_label_;
                    dec_rehome_label = dec_rehome_label_;
                }
                if (!run_motion_stage(context, [this, &lst, &hemisphere, &rebase_started, &reference_established,
                                                ra_rehome_label, dec_rehome_label, ra, dec, destination_side] {
                        const auto reference_generation = protocol_.session_generation();
                        rebase_started = true;
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
                            lst = cgx_local_sidereal_time_hours(
                                std::chrono::system_clock::time_point{
                                    std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                        std::chrono::duration<double>(current_utc_locked()))},
                                configured_site_coordinate(info_.site_longitude_degrees, "SiteLongitude"));
                            hemisphere = hemisphere_locked();
                        }
                        const auto target = cgx_sky_position_to_encoders(ra, dec, lst, hemisphere, destination_side);
                        protocol_.goto_axis_fast(CgxAxis::RightAscension, target.first);
                        protocol_.goto_axis_fast(CgxAxis::Declination, target.second);
                        std::lock_guard<std::mutex> lock(mutex_);
                        at_home_ = false;
                    })) {
                    clear_cancelled_state();
                    return;
                }
                if (!finish_slew_operation(context, deadline, ra, dec, destination_side,
                                           "Celestron CGX flip slew timed out")) {
                    if (context.stop_reason() == util::StopReason::Cancelled) clear_cancelled_state();
                    return;
                }
                return;
            }
            if (clock_.now() >= deadline)
                throw AlpacaException("Celestron CGX meridian home route timed out", AlpacaError::DriverException);
            static_cast<void>(context.wait_for(std::chrono::milliseconds(200)));
        }
    } catch (...) {
        stop_motion_worker_checked();
        std::lock_guard<std::mutex> lock(mutex_);
        parked_ = false;
        at_home_ = false;
        if (!reference_established) {
            calibrated_ = !rebase_started;
            needs_sync_ = false;
            sync_correction_active_ = false;
        }
        throw;
    }
}

void CelestronCgxTelescopeDriver::run_pulse_guide(util::OperationContext& context, CgxAxis axis, int velocity_percent,
                                                  int first_chunk_centiseconds, int remaining_centiseconds,
                                                  const std::shared_future<PulseGuideStart>& start_result) {
    const PulseGuideStart start = start_result.get();
    bool cancelled = start == PulseGuideStart::Ambiguous;
    if (cancelled) {
        // A lost guide acknowledgement desynchronizes AUX. Reopen and flush,
        // then observe only the first packet; do not retransmit it.
        protocol_.connect_serial(info_.port_path);
    }
    const auto operation_deadline =
        clock_.now() + std::chrono::milliseconds(
                           static_cast<std::int64_t>(first_chunk_centiseconds + remaining_centiseconds) * 10 + 5000);
    int current_chunk = first_chunk_centiseconds;
    auto segment_deadline = clock_.now() + std::chrono::milliseconds(current_chunk * 10 + 500);
    while (true) {
        const auto reason = context.stop_reason();
        cancelled = cancelled || reason != util::StopReason::None;
        const bool active = protocol_.is_aux_guide_active(axis);
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
        if (cancelled || remaining_centiseconds == 0) return;
        current_chunk = std::min(255, remaining_centiseconds);
        if (!run_motion_stage(context, [this, axis, velocity_percent, current_chunk] {
                protocol_.pulse_guide_axis(axis, velocity_percent, current_chunk);
            })) {
            return;
        }
        remaining_centiseconds -= current_chunk;
        segment_deadline = clock_.now() + std::chrono::milliseconds(current_chunk * 10 + 500);
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
    require_range(info.site_latitude_degrees, -90.0, 90.0, "SiteLatitude must be in [-90, 90] degrees");
    require_range(info.site_longitude_degrees, -180.0, 180.0, "SiteLongitude must be in [-180, 180] degrees");
    require_range(info.site_elevation_meters, -300.0, 10000.0, "SiteElevation must be in [-300, 10000] m");
    if (!std::isfinite(info.aperture_diameter_meters) || info.aperture_diameter_meters < 0.0) {
        throw AlpacaException("Aperture diameter must be non-negative", AlpacaError::InvalidValue);
    }
    if (!std::isfinite(info.focal_length_meters) || info.focal_length_meters < 0.0) {
        throw AlpacaException("Focal length must be non-negative", AlpacaError::InvalidValue);
    }
    return std::make_unique<CelestronCgxTelescopeDriver>(device_number, info, clock, std::move(system_now));
}

}  // namespace alpacacore::vendor::celestron
