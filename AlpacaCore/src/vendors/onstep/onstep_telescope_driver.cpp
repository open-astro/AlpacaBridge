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
#include <alpacacore/util/auto_detect.h>
#include <alpacacore/util/client_utc_warning.h>
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/hold_window.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/lock_gate.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/motion_policy.h>
#include <alpacacore/util/pointing.h>
#include <alpacacore/util/state_snapshot.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/util/utc_offset.h>
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>
#include <alpacacore/vendor/onstep/onstep_telescope_driver.h>
#include <alpacacore/version.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <limits>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace alpacacore::vendor::onstep {

namespace {

// OnStep accepts a true arbitrary MoveAxis rate via :RA[n.n]#/:RE[n.n]#
// (real degrees/second, confirmed against Command.ino/Guide.ino) — this is
// a continuous range, not a small set of discrete presets. See
// OnStepProtocolWrapper::move_axis_start().
constexpr double kMaxMoveAxisRateDegPerSec = 4.0;
// Decision 0010, OnStep adoption (#1783). One poll body samples the mount every kStatusPollPeriod, start to start on
// the task clock (ticks missed behind a slow link are skipped, never made up): :GU# and :GRa#/:GDe# on every tick,
// :GA#/:GZ# on every kSlowPollEvery-th, so Altitude and Azimuth are never older than the 2 s cache they replace. A
// full tick is five exchanges, about 0.13 s at 9600 baud on the HM-17 (#1242), so the line stays free for commands.
// kSnapshotMaxAge is the FRESHNESS bound: a frame older than it reads `stale` and is still served. Refusal is the
// link-loss policy of decision 0009 (the fault latch and util::kLinkStalenessBound), never staleness.
constexpr auto kStatusPollPeriod = std::chrono::milliseconds(1000);
constexpr int kSlowPollEvery = 2;
constexpr auto kSnapshotMaxAge = std::chrono::milliseconds(3000);
static_assert(kSnapshotMaxAge >= 2 * kStatusPollPeriod, "two poll periods, or a healthy link reads stale");
static_assert(kSnapshotMaxAge < util::kLinkStalenessBound, "freshness must undercut the link-loss bound");
// A pulse guide is measured once it has ended: the poll takes a tick this long after the pulse's end time, and
// IsPulseGuiding reads true until a frame sampled from then on is published (or kPulseMeasureBound after the end, on a
// link that does not answer), so a position read right after IsPulseGuiding goes false is not one taken mid-pulse.
constexpr auto kPulseGuideSettle = std::chrono::milliseconds(50);
constexpr auto kPulseMeasureBound = 2 * kStatusPollPeriod;
constexpr auto kSiteInfoRetryDelay = std::chrono::seconds(2);
// Decision 0022: Slewing reads true for this long after a GOTO goes out (slew_hold_), so a getter reads true before
// any :GU# is sampled. Landing never reads the window: it is a :GU# that shows no GOTO, and it clears the window.
// A stale reading cannot end the slew: a reading and the :MS# share the operation lock, and the dispatch
// write-through shows the GOTO until a later reading replaces it. The mount shows the GOTO 1 ms after :MS# answers
// (HM-17, OnStepX 10.23a, 2026-10-29).
constexpr auto kSlewForceGrace = std::chrono::seconds(2);
// open-astro#1378: how far inside the horizon and overhead limits a MoveAxis
// re-send must stay.
constexpr double kAltitudeLimitMarginDeg = 1.0;
// open-astro#1378: how often a running MoveAxis is re-sent so OnStepX's guide
// time limit (10 s by default) does not end it. Tests advance a FakeTaskClock.
constexpr auto kMoveAxisResendInterval = std::chrono::seconds(7);
// open-astro#1430: how long Park and FindHome wait for the motion they stopped
// to end, and how long they retry a park or home OnStepX refuses as "in motion"
// (an axis brakes for about 0.4 s at 0.5 deg/s on the HM-17; a home that just
// ended keeps its guide state a moment longer).
constexpr auto kStopSettleTimeout = std::chrono::seconds(6);
constexpr auto kStopSettlePoll = std::chrono::milliseconds(100);
constexpr auto kInMotionRetryWindow = std::chrono::seconds(3);
constexpr auto kInMotionRetryPoll = std::chrono::milliseconds(200);
// OnStepX :GE# codes (docs/COMMAND_REFERENCE.md, "Command Error Codes").
constexpr int kCeNone = 0;
// The longest PulseGuide the :Mg[nsew]#### command can carry (#1478).
constexpr int kMaxPulseGuideMs = 9999;
constexpr int kCeSlewInMotion = 22;
constexpr int kCeTrue = 25;
constexpr int kCeNotParked = 11;
constexpr int kCeParked = 9;       // "already parked" (OnStep 4.x answers a park of a parked mount with it)
constexpr int kCeSlewInPark = 18;  // "mount parked" (OnStep 4.x answers a home of a parked mount with it)

std::string describe_onstepx_error(int code) {
    switch (code) {
        case 8:
            return "not parked or at home";
        case 9:
            return "already parked";
        case 10:
            return "park failed";
        case 11:
            return "not parked";
        case 12:
            return "no park position set";
        case 13:
            return "goto failed";
        case 15:
            return "target below the horizon limit";
        case 16:
            return "target above the overhead limit";
        case 17:
            return "controller in standby";
        case 18:
            return "mount parked";
        case 19:
            return "goto already active";
        case 20:
            return "outside configured limits";
        case 21:
            return "hardware fault";
        case 22:
            return "mount still in motion";
        case 23:
            return "slew error";
        default:
            return "error code " + std::to_string(code);
    }
}

// open-astro#1414 (decision 0009): consecutive failed poll ticks (or control-path
// status reads) that latch a link fault, as on SynScan and Celestron. Each failed
// tick costs one response timeout, so a silent mount latches in about three poll
// periods plus three timeouts.
constexpr int kLinkFaultThreshold = 3;
// Informational only — OnStep does not expose a guide-rate query command in
// this driver, and CanSetGuideRates is false, so this value is never sent to
// or read from the mount; it merely satisfies the ASCOM GuideRate property.
// OnStepX numbers its releases from 10 (the HM-17 answers :GVN# with
// "10.23a"); legacy OnStep is 4.x and older. Only OnStepX knows :GXEE#.
bool is_onstepx_version(const std::string& version) {
    std::size_t digits = 0;
    while (digits < version.size() && std::isdigit(static_cast<unsigned char>(version[digits]))) ++digits;
    return digits > 0 && digits <= 4 && std::stoi(version.substr(0, digits)) >= 10;
}

constexpr double kInformationalGuideRateDegPerSec = (15.0411 / 3600.0) * 0.5;

// Standard spherical-trig horizontal-to-equatorial transform (azimuth
// measured from North through East). Used to implement SlewToAltAz/
// SlewToAltAzAsync via an ordinary equatorial slew — OnStep/LX200 has no
// native Alt/Az goto command — mirroring the approach other equatorial-only
// drivers in this project use for the same capability.
void alt_az_to_ra_dec(double alt_deg, double az_deg, double lat_deg, double lst_hours, double& ra_hours_out,
                      double& dec_deg_out) {
    const double lat_rad = lat_deg * std::numbers::pi / 180.0;
    const double alt_rad = alt_deg * std::numbers::pi / 180.0;
    const double az_rad = az_deg * std::numbers::pi / 180.0;

    double sin_dec = std::sin(alt_rad) * std::sin(lat_rad) + std::cos(alt_rad) * std::cos(lat_rad) * std::cos(az_rad);
    sin_dec = std::clamp(sin_dec, -1.0, 1.0);
    const double dec_rad = std::asin(sin_dec);

    double cos_ha = (std::sin(alt_rad) - std::sin(lat_rad) * sin_dec) / (std::cos(lat_rad) * std::cos(dec_rad));
    cos_ha = std::clamp(cos_ha, -1.0, 1.0);
    double ha_hours = std::acos(cos_ha) * 180.0 / std::numbers::pi / 15.0;
    if (std::sin(az_rad) > 0.0) {
        ha_hours = 24.0 - ha_hours;
    }

    ra_hours_out = util::wrap_hours(lst_hours - ha_hours);
    dec_deg_out = dec_rad * 180.0 / std::numbers::pi;
}

// One polled frame (decision 0010): what one tick measured, nothing derived. Small and copyable: the snapshot copies
// it under its mutex. `sampled_at` is the task-clock time the tick's first exchange went out; `sample_seq` numbers the
// samples in the order they were taken (each under the operation lock), so two taken at the same task-clock time
// still order.
struct PolledFrame {
    MountStatus status{};
    bool status_known = false;
    double ra_hours = 0.0;
    double dec_degrees = 0.0;
    bool equatorial_known = false;
    double alt_degrees = 0.0;
    double az_degrees = 0.0;
    bool altaz_known = false;
    util::TaskClock::clock::time_point sampled_at{};
    std::uint64_t sample_seq = 0;
};

bool same_status(const MountStatus& a, const MountStatus& b) {
    return a.raw_status == b.raw_status && a.is_tracking == b.is_tracking && a.is_slewing == b.is_slewing &&
           a.is_parked == b.is_parked && a.is_at_home == b.is_at_home && a.is_guide_active == b.is_guide_active &&
           a.is_homing == b.is_homing && a.is_parking == b.is_parking &&
           a.is_pulse_guide_active == b.is_pulse_guide_active && a.side_of_pier == b.side_of_pier;
}

}  // namespace

class OnStepTelescopeDriver : public TelescopeDriver, protected alpacacore::AsyncConnectable {
    // Decision 0016 (#1449): getters never wait on device I/O. Two locks, in this order:
    //   transition_mutex_ -> io_mutex_ (the operation lock) -> mutex_ (the state lock, a leaf)
    // io_mutex_ keeps one command's exchanges and state writes together: every command that reaches the wire
    // holds it for the whole command and releases it wherever it released mutex_ (a sleep, a wait). Some
    // sequences must not be split: :GE# answers for the command right before it, and :Sr#/:Sd# set the target
    // the :MS# or :CM# after them uses. mutex_ is never held ACROSS an exchange: every call through Wire drops
    // it for the call and takes it back after. The protocol wrapper's own lock serialises the wire itself.
    //
    // Decision 0010 (#1783): the polled getters and DeviceState read snapshot_, which one poll body
    // (status_poll_) fills every second, so a getter takes mutex_ alone (StateLock::Mode::State) and never
    // reaches the wire. The poll body takes the operation lock for its exchanges, like a command, and publishes
    // under mutex_ with the operation lock released. The StateSnapshot mutex is a leaf under mutex_.
    //
    // The gate itself (which locks this thread holds, StateLock's two modes, dropping mutex_ around an exchange)
    // is util::LockGate (lock_gate.h, #1729).

public:
    // The driver's state lock, with the operation lock above it in Mode::Io (util::StateLock). Mode::State
    // is for the getters, the setters that send nothing and the MoveAxis re-send's scheduling read: they take
    // no operation lock and never reach the wire.
    using StateLock = util::StateLock;

private:
    // Every exchange with the mount goes through this gate (the process-wide protocol wrapper behind it). The
    // caller holds the operation lock; it gives up the state lock for the call, if it holds it, and takes it
    // back after (also when the call throws). A caller without the operation lock is a defect: it throws
    // DriverException instead of reaching the wire.
    class Wire {
    public:
        explicit Wire(const OnStepTelescopeDriver& driver) : driver_(driver) {}

        template <typename F>
        decltype(auto) call(F&& f) const {
            auto& protocol = OnStepProtocolWrapper::instance();
            const util::LockGate& gate = driver_.lock_gate_;
            if (!gate.holds_io()) {
                throw AlpacaException("OnStep: a mount exchange without the operation lock",
                                      AlpacaError::DriverException);
            }
            return gate.exchange([&]() -> decltype(auto) { return f(protocol); });
        }

        // No I/O, no wrapper lock held across a wait: called directly.
        bool link_alive() const { return OnStepProtocolWrapper::instance().link_alive(); }

        bool connect(const ConnectionInfo& info) const {
            return call([&](auto& p) { return p.connect(info); });
        }
        std::string last_connect_error() const {
            return call([](auto& p) { return p.last_connect_error(); });
        }
        void disconnect() const {
            call([](auto& p) { p.disconnect(); });
        }
        void declare_link_lost(const std::string& reason) const {
            call([&](auto& p) { p.declare_link_lost(reason); });
        }
        std::string get_product_name() const {
            return call([](auto& p) { return p.get_product_name(); });
        }
        std::string get_version_number() const {
            return call([](auto& p) { return p.get_version_number(); });
        }
        std::optional<int> get_mount_coords_mode() const {
            return call([](auto& p) { return p.get_mount_coords_mode(); });
        }
        Position get_position() const {
            return call([](auto& p) { return p.get_position(); });
        }
        AltAz get_alt_az() const {
            return call([](auto& p) { return p.get_alt_az(); });
        }
        AltitudeLimits get_altitude_limits() const {
            return call([](auto& p) { return p.get_altitude_limits(); });
        }
        MountStatus get_status() const {
            return call([](auto& p) { return p.get_status(); });
        }
        void set_target_ra(double ra_hours) const {
            call([&](auto& p) { p.set_target_ra(ra_hours); });
        }
        void set_target_dec(double dec_degrees) const {
            call([&](auto& p) { p.set_target_dec(dec_degrees); });
        }
        std::string slew_to_target() const {
            return call([](auto& p) { return p.slew_to_target(); });
        }
        std::string sync_to_target() const {
            return call([](auto& p) { return p.sync_to_target(); });
        }
        void abort_slew() const {
            call([](auto& p) { p.abort_slew(); });
        }
        void start_tracking() const {
            call([](auto& p) { p.start_tracking(); });
        }
        bool set_tracking_checked(bool on) const {
            return call([&](auto& p) { return p.set_tracking_checked(on); });
        }
        bool park() const {
            return call([](auto& p) { return p.park(); });
        }
        bool set_park() const {
            return call([](auto& p) { return p.set_park(); });
        }
        bool unpark() const {
            return call([](auto& p) { return p.unpark(); });
        }
        void find_home() const {
            call([](auto& p) { p.find_home(); });
        }
        std::optional<int> get_last_command_error() const {
            return call([](auto& p) { return p.get_last_command_error(); });
        }
        void select_max_slew_rate() const {
            call([](auto& p) { p.select_max_slew_rate(); });
        }
        void move_axis_start(int direction, double rate_deg_per_sec) const {
            call([&](auto& p) { p.move_axis_start(direction, rate_deg_per_sec); });
        }
        void move_axis_stop(int direction) const {
            call([&](auto& p) { p.move_axis_stop(direction); });
        }
        void pulse_guide(int direction, int duration_ms) const {
            call([&](auto& p) { p.pulse_guide(direction, duration_ms); });
        }
        SiteInfo get_site_info() const {
            return call([](auto& p) { return p.get_site_info(); });
        }
        void set_latitude(double latitude_degrees) const {
            call([&](auto& p) { p.set_latitude(latitude_degrees); });
        }
        void set_longitude(double longitude_degrees) const {
            call([&](auto& p) { p.set_longitude(longitude_degrees); });
        }
        TimeInfo get_time() const {
            return call([](auto& p) { return p.get_time(); });
        }
        void set_time(const TimeInfo& info) const {
            call([&](auto& p) { p.set_time(info); });
        }
        std::string send_command(const std::string& command) const {
            return call([&](auto& p) { return p.send_command(command); });
        }
        void send_command_blind(const std::string& command) const {
            call([&](auto& p) { p.send_command_blind(command); });
        }

    private:
        const OnStepTelescopeDriver& driver_;
    };
    Wire wire() const { return Wire(*this); }

    // True when this thread holds the operation lock (a command, or the connect itself).
    bool holds_io_lock() const { return lock_gate_.holds_io(); }

    // A slew or sync to (ra, dec) sets both targets, except one a client wrote
    // while the command ran: the target setters take mutex_ alone (#1728
    // review), so a write can land during any exchange of the command, its
    // checks' :GU# or site reads included. Each entry point takes the snapshot
    // right after its StateLock, before any of them, so that write stands.
    struct TargetWrites {
        std::uint64_t ra;
        std::uint64_t dec;
    };
    TargetWrites target_writes_locked() const { return {target_ra_writes_, target_dec_writes_}; }
    void store_target_locked(double ra, double dec, TargetWrites before) {
        if (target_ra_writes_ == before.ra) {
            target_ra_hours_ = ra;
            target_ra_set_ = true;
        }
        if (target_dec_writes_ == before.dec) {
            target_dec_degrees_ = dec;
            target_dec_set_ = true;
        }
    }

public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    OnStepTelescopeDriver(int device_number, const ConnectionInfo& connection_info,
                          std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
                          std::optional<double> site_elevation_m, std::optional<bool> sync_time_on_connect,
                          util::ConnectionResolver<ConnectionInfo> connection_resolver = {},
                          util::TaskClock& clock = util::default_task_clock())
        : AsyncConnectable("OnStep"),
          clock_(clock),
          device_number_(device_number),
          connection_info_(connection_info),
          connection_resolver_(std::move(connection_resolver)),
          connected_(false),
          site_elevation_m_(site_elevation_m.value_or(std::numeric_limits<double>::quiet_NaN())),
          pending_site_latitude_(site_latitude_deg),
          pending_site_longitude_(site_longitude_deg),
          pending_site_elevation_(site_elevation_m),
          sync_time_on_connect_(sync_time_on_connect.value_or(false)) {}

    ~OnStepTelescopeDriver() override {
        // MUST be first, before members the async connection task touches
        // are destroyed (AsyncConnectable base contract). The other workers,
        // the MoveAxis re-send (#1378) and the Park/FindHome tail (#1430),
        // are joined next without the driver locks (they take them to send).
        shutdown_connection();
        stop_status_poller();
        move_axis_resend_.cancel_all_and_join();
        park_home_.cancel_all_and_join();
        if (connected_) {
            try {
                // OnStepTelescopeDriver is the most-derived class (no further
                // override exists to miss), so the analyzer's "bypasses
                // virtual dispatch" warning doesn't apply here.
                set_connected(false);  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
            } catch (...) {            // NOLINT(bugprone-empty-catch)
                // Best-effort teardown during destruction.
            }
        }
        // Last: the process-wide wrapper must not keep a clock that may die
        // with this driver (a test's FakeTaskClock).
        OnStepProtocolWrapper::instance().release_task_clock(clock_);
    }

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override { return "OnStep Telescope"; }

    DeviceType get_device_type() const override { return DeviceType::Telescope; }

    std::string fallback_unique_id() const override { return "OnStep_" + std::to_string(device_number_); }

    std::string get_description() const override { return "OnStep LX200-Protocol Telescope Driver"; }

    std::string get_driver_info() const override { return "AlpacaCore OnStep Driver v0.1"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // Product name + firmware version captured at connect; web-UI only.
    // The connect no longer holds mutex_ across its exchanges: it holds
    // transition_mutex_ and io_mutex_ for its whole multi-second run and
    // takes mutex_ only between exchanges (decision 0016), so mutex_ would
    // serve here now. firmware_mutex_ is kept anyway, held only for the copy:
    // it is the shared rule's guard (driver-lifecycle, "Device firmware / SDK
    // version"), the configureddevices listing then never queues behind the
    // state writes of a getter or a command, and the connect fills the cache
    // without firmware_mutex_ held across its :GVP#/:GVN# reads.
    std::optional<std::string> get_device_firmware() const override {
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        if (firmware_cache_.empty()) {
            return std::nullopt;
        }
        return firmware_cache_;
    }

    int get_interface_version() const override { return 4; }

    // Lock-free on purpose (open-astro#910, decision 0016 rule 3): set_connected(true) holds
    // transition_mutex_ and io_mutex_ for the whole connect (port open, handshake, site and time
    // sync, the first frame, and an auto-detect scan when the port is not explicit). The
    // router checks Connecting before most Connected reads, but DeviceState
    // calls this with no such check, and a read that lands just before the
    // connect task shows as running gets past it; with mutex_ taken here those
    // once waited for the whole connect. link_alive() takes no lock a connect holds: the wrapper mutex_
    // only by try_lock, otherwise only short leaf mutexes (a stat(), a few fields).
    // It reads published_connected_, raised only at the end of a connect, so
    // Connected stays false while Connecting is true. Same rule as SynScan
    // (#130); the blocking-driver list lives in async_connectable.h.
    bool get_connected() const override {
        // A removed or re-enumerated adapter reads as not connected (#1241).
        return published_connected_.load() && OnStepProtocolWrapper::instance().link_alive();
    }

    // NotConnected exactly when Connected reads false, without mutex_.
    void check_connected_lock_free() const {
        if (!get_connected()) {
            throw AlpacaException("Not connected to OnStep mount", AlpacaError::NotConnected);
        }
    }

    // The latched fault for the getters that read lock-free, so they never wait
    // behind serial I/O under mutex_ (#1449): link_fault_text_ has its own narrow
    // mutex. No staleness check here; the poll ticks and refused writes make a
    // standing fault a lost link.
    void throw_if_faulted_lock_free() const {
        std::string fault;
        {
            std::lock_guard<std::mutex> fault_lock(link_fault_mutex_);
            fault = link_fault_text_;
        }
        if (!fault.empty()) {
            throw AlpacaException("OnStep mount communications compromised: " + fault, AlpacaError::DriverException);
        }
    }

    // Why the serial link was lost, kept until the next Connect or Disconnect,
    // or else the latched fault of a silent mount (decision 0009). The wrapper's
    // reason and link_fault_text_ under its own narrow mutex: never waits on mutex_.
    std::string get_link_fault() const override {
        auto& protocol = OnStepProtocolWrapper::instance();
        (void)protocol.link_alive();  // notice an unplug the listing would otherwise miss
        const std::string reason = protocol.link_lost_reason();
        if (!reason.empty()) {
            return "Link to the mount lost (" + reason + "); reconnect";
        }
        // A latched fault: Connected still true, reads refused (#1414).
        std::lock_guard<std::mutex> fault_lock(link_fault_mutex_);
        return link_fault_text_.empty() ? std::string{} : "Mount not answering (" + link_fault_text_ + ")";
    }

    void connect() override { start_connection_task(true); }

    void disconnect() override { start_connection_task(false); }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        const Wire protocol = wire();
        // Decision 0016: the whole call runs under transition_mutex_, which no getter takes. Every exchange
        // below goes through Wire, with mutex_ free; io_mutex_ is held from the first state write to the end.
        // A getter meanwhile reads NotConnected (check_connected()) instead of waiting for the connect.
        std::lock_guard<std::mutex> transition(transition_mutex_);
        // The status poll ends before a disconnect or a relink closes the port, and is joined with no driver
        // lock held: its body takes them (decision 0006 rule 4). No poll body runs during a connect.
        if (!connected || (connected_ && !protocol.link_alive())) {
            stop_status_poller();
        }
        StateLock lock(lock_gate_);
        // A Connected=true against a lost link must reconnect, not return as
        // already connected (decision 0009 point 4, #1241).
        if (connected && connected_ && !protocol.link_alive()) {
            ALPACA_LOG_WARN("OnStep", "Connect requested on a lost link; reconnecting");
            published_connected_ = false;
            connected_ = false;  // before the close: a getter during it reads NotConnected (decision 0016)
            protocol.disconnect();
            reset_session_state_locked();
        }
        // Base gates BEFORE the idempotency check: a sync disconnect during
        // an in-flight connect looks idempotent (both sides see
        // disconnected) and would be silently dropped without the record; a
        // connect must honor a newer pending disconnect by staying down.
        if (!connected && record_disconnect_if_connect_in_flight(connected_)) {
            return;
        }
        if (connected && consume_pending_disconnect(connected_)) {
            return;
        }
        if (connected == connected_) {
            return;
        }

        if (connected) {
            // The wrapper's loss stamps read this driver's clock (decision 0005).
            OnStepProtocolWrapper::instance().set_task_clock(clock_);
            // An auto-detected mount resolves its port here, not in the factory (#659). The scan and the
            // open go through Wire like every exchange: the state lock is released for them and taken back
            // before the outcome or its exception leaves. The connection members the scan writes are read
            // only under transition_mutex_, which this call holds; io_mutex_ stays held.
            protocol.call([&](auto&) {
                util::connect_resolved(
                    connection_info_, connection_resolved_, connection_resolver_,
                    [&protocol](const ConnectionInfo& info) {
                        if (!protocol.connect(info)) {
                            // The wrapper's reason (a port another device holds, #1202)
                            // goes to the operator.
                            const std::string reason = protocol.last_connect_error();
                            const std::string message = reason.empty() ? "Failed to connect to OnStep mount"
                                                                       : "Failed to connect to OnStep mount: " + reason;
                            // Refused TCP connect, vanished serial node, or a port another connected
                            // device now holds (#1202): stale, re-scan.
                            if (info.type == ConnectionType::Network || util::serial_endpoint_stale(info.port_path)) {
                                throw util::StaleEndpoint(message);
                            }
                            throw AlpacaException(message);
                        }
                    },
                    "OnStep");
            });
            connected_ = true;
            stop_motion_after_long_outage_locked();
            snapshot_.reset();
            site_latitude_known_ = false;
            site_longitude_known_ = false;
            utc_offset_.clear();
            mount_clock_unreadable_.reset();
            client_disagreement_warned_ = false;
            firmware_reports_guide_move_ = false;
            link_health_.reset();
            set_link_fault_text("");
            target_ra_set_ = false;
            target_dec_set_ = false;
            clear_manual_motion_locked();
            slew_hold_.clear();
            clear_pulse_guides_locked();
            tracking_before_slew_ = false;
            status_read_since_move_ = false;
            last_site_info_attempt_ = std::chrono::steady_clock::time_point::min();
            site_read_warned_ = false;

            // The two reads run without firmware_mutex_: get_device_firmware()
            // takes only that lock and must not wait for a serial round trip
            // (decision 0016). Only the store is under it.
            std::string firmware_version;
            std::string firmware;
            try {
                std::string product = protocol.get_product_name();
                firmware_version = protocol.get_version_number();
                firmware = product + (firmware_version.empty() ? "" : (" " + firmware_version));
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                firmware.clear();
            }
            {
                std::lock_guard<std::mutex> fwlock(firmware_mutex_);
                firmware_cache_ = firmware;
            }

            // OnStepX applies refraction to the RA/Dec it reports unless it was
            // built for observed-place coordinates; :GXEE# says which (#1216).
            refraction_from_firmware_.reset();
            if (is_onstepx_version(firmware_version)) {
                try {
                    if (const auto mode = protocol.get_mount_coords_mode()) {
                        refraction_from_firmware_ = (*mode != 0);
                    }
                } catch (...) {  // NOLINT(bugprone-empty-catch)
                    // Unknown: DoesRefraction keeps the settable local flag.
                }
            }

            // Each coordinate is known only once its own write landed (or the
            // mount's pair was read): a field whose write failed stays unknown,
            // so its getter throws instead of serving a default 0.0, and the
            // pair counts as known (site_known_locked()) only with both. The
            // :Gt#/:Gg# read below, then the status poll, fetch the mount's
            // pair while it is not (ensure_site_info_cached_locked()).
            //
            // Neither firmware refuses :St#/:Sg# for the mount's state (aligned,
            // parked, slewing): OnStepX (Site.command.cpp) and OnStep 4.x
            // (Command.ino) answer 0 only for a value they cannot parse or that
            // is out of range. A failed write is therefore a bad config value or
            // a link that did not answer. The connect still succeeds, as before,
            // with the mount's own site read back below; the config values stay
            // as they are and are written again at the next connect.
            if (pending_site_latitude_.has_value()) {
                try {
                    protocol.set_latitude(pending_site_latitude_.value());
                    site_latitude_cached_ = pending_site_latitude_.value();
                    site_latitude_known_ = true;
                } catch (const std::exception& ex) {
                    ALPACA_LOG_WARN("OnStep", std::string("Site latitude from the device config not written to the "
                                                          "mount; using the mount's own site: ") +
                                                  ex.what());
                }
            }
            if (pending_site_longitude_.has_value()) {
                try {
                    protocol.set_longitude(pending_site_longitude_.value());
                    site_longitude_cached_ = pending_site_longitude_.value();
                    site_longitude_known_ = true;
                } catch (const std::exception& ex) {
                    ALPACA_LOG_WARN("OnStep", std::string("Site longitude from the device config not written to the "
                                                          "mount; using the mount's own site: ") +
                                                  ex.what());
                }
            }
            if (pending_site_elevation_.has_value()) {
                site_elevation_m_ = pending_site_elevation_.value();
            }
            // A failed time sync (the mount refused :SL#/:SC#/:SG#, or did not
            // answer) leaves the mount on its own clock, which the read below
            // then seeds UTCDate from. The connect still succeeds, as for the
            // site writes above (#1754).
            if (sync_time_on_connect_) {
                try {
                    set_utc_date_locked(std::chrono::system_clock::now());
                } catch (const std::exception& ex) {
                    ALPACA_LOG_WARN("OnStep", std::string("Time sync on connect failed; the mount keeps its own "
                                                          "clock: ") +
                                                  ex.what());
                }
            }
            // The mount clock is read here, once, so UTCDate (and so DeviceState)
            // performs no serial exchange of its own (#1478); a written UTCDate needs none.
            if (!utc_offset_.valid()) {
                seed_utc_from_mount_locked();
            }
            try {
                // Continuous-motion (:Mn#/:Ms#/:Me#/:Mw#) commands run at
                // whatever slew rate the firmware currently has selected;
                // pin it to max on connect so MoveAxis completes promptly.
                protocol.select_max_slew_rate();
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }

            // The first frame, so the first property reads, status flags included,
            // are served from memory the moment Connect returns (ConformU's
            // fast-response targets, #1242), and the mount's site when the
            // config did not set both coordinates.
            seed_snapshot_locked();
            published_connected_ = true;  // the connect is complete (#910)
            // The poll starts once the connect is complete, with no driver lock held (decision 0006 rule 4).
            lock.unlock();
            start_status_poller(kStatusPollPeriod);
        } else {
            // Decision 0016, item 4: Connected goes false under mutex_ BEFORE the close, which runs through
            // Wire with mutex_ released; a getter during the close reads NotConnected, not a closing link.
            published_connected_ = false;
            connected_ = false;
            reset_session_state_locked();
            protocol.call([this](auto& p) {
                run_disconnect_before_close_hook();
                p.disconnect();
            });
        }
    }

    std::vector<std::string> get_supported_actions() const override { return {}; }

    std::string action(std::string_view action_name, std::string_view action_parameters) override {
        (void)action_parameters;
        throw AlpacaException("Action not supported: " + std::string(action_name), AlpacaError::ActionNotImplemented);
    }

    bool can_action(std::string_view action_name) const override {
        (void)action_name;
        return false;
    }

    std::string command_blind(std::string_view command, bool raw) override {
        (void)raw;
        StateLock lock(lock_gate_);
        check_connected();
        wire().send_command_blind(std::string(command));
        return "";
    }

    bool command_bool(std::string_view command, bool raw) override {
        StateLock lock(lock_gate_);
        check_connected();
        const Wire protocol = wire();
        if (raw) {
            std::string response = protocol.send_command(std::string(command));
            return response == "1";
        }
        protocol.send_command_blind(std::string(command));
        return true;
    }

    std::string command_string(std::string_view command, bool raw) override {
        (void)raw;
        StateLock lock(lock_gate_);
        check_connected();
        return wire().send_command(std::string(command));
    }

    AlignmentMode get_alignment_mode() const override { return AlignmentMode::GermanPolar; }

    double get_altitude() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        return served_altaz_locked().altitude_degrees;
    }

    double get_aperture_diameter() const override { return aperture_diameter_m_; }

    void set_aperture_diameter(double meters) override {
        if (meters < 0.0) {
            throw AlpacaException("Aperture diameter must be non-negative", AlpacaError::InvalidValue);
        }
        aperture_diameter_m_ = meters;
        aperture_area_m2_ = meters > 0.0 ? (meters / 2.0) * (meters / 2.0) * std::numbers::pi : 0.0;
    }

    double get_aperture_area() const override { return aperture_area_m2_; }

    bool get_at_home() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        raise_park_home_error_locked(true);
        return served_status_locked().is_at_home;
    }

    bool get_at_park() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        raise_park_home_error_locked(false);
        return served_status_locked().is_parked;
    }

    double get_azimuth() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        return served_altaz_locked().azimuth_degrees;
    }

    bool get_can_find_home() const override { return true; }
    bool get_can_park() const override { return true; }
    bool get_can_pulse_guide() const override { return true; }

    bool get_is_pulse_guiding() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        throw_if_faulted_locked();  // commanded state, not a wire read: refused too (decision 0009 rule 1)
        const auto now = clock_.now();
        const auto reading = snapshot_.read();
        bool guiding = false;
        for (int axis = 0; axis < 2; ++axis) {  // ASCOM: true while either axis pulses
            if (!pulse_guiding_active_[axis]) {
                continue;
            }
            const util::HoldWindow& hold = pulse_hold_[axis];  // armed for the pulse's duration (decision 0022)
            // Past the pulse's end, until a frame sampled kPulseGuideSettle or more after it is published, the
            // position getters would serve one measured during the pulse, so the pulse still reads as running
            // (bounded by kPulseMeasureBound, for a link that does not answer); the poll takes that sample then.
            // `hold.active(t - d)` is `t < end + d`.
            const bool unmeasured = reading && hold.active(reading->value.sampled_at - kPulseGuideSettle) &&
                                    hold.active(now - kPulseMeasureBound);
            if (hold.active(now) || unmeasured) {
                guiding = true;
            } else {
                pulse_guiding_active_[axis] = false;
            }
        }
        return guiding;
    }

    // OnStep's custom tracking-rate commands were not confirmed against real
    // firmware during this implementation (see .github/instructions/onstep.instructions.md) —
    // default both to false rather than advertise a setter that would
    // silently no-op.
    bool get_can_set_declination_rate() const override { return false; }
    bool get_can_set_right_ascension_rate() const override { return false; }
    bool get_can_set_guide_rates() const override { return false; }
    bool get_can_set_park() const override { return true; }
    bool get_can_set_pier_side() const override { return false; }
    bool get_can_set_tracking() const override { return true; }
    bool get_can_slew_alt_az() const override { return true; }
    bool get_can_slew_alt_az_async() const override { return true; }
    bool get_can_sync_alt_az() const override { return false; }
    bool get_can_slew() const override { return true; }
    bool get_can_slew_async() const override { return true; }
    bool get_can_sync() const override { return true; }
    bool get_can_unpark() const override { return true; }

    double get_declination() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        return served_position_locked().dec_degrees;
    }

    // Rates are not supported (always 0), but an ASCOM read still throws
    // NotConnected while disconnected (#1478). Read lock-free, as Connected
    // is, so the getter never waits behind serial I/O under mutex_ (#1449).
    double get_declination_rate() const override {
        check_connected_lock_free();
        throw_if_faulted_lock_free();  // as Sky-Watcher's rate getters (decision 0009 rule 1)
        return 0.0;
    }

    void set_declination_rate(double rate) override {
        (void)rate;
        throw AlpacaException("Declination rate not supported", AlpacaError::PropertyNotImplemented);
    }

    bool get_tracking() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        return served_status_locked().is_tracking;
    }

    void set_tracking(bool tracking) override {
        StateLock lock(lock_gate_);
        check_connected();
        refuse_while_faulted_locked("Tracking");
        if (tracking) {
            check_not_parked_locked("Tracking");
        }
        // The reply is read, not drained: OnStep 4.x refuses :ST while slewing
        // (#1379); the internal tracking restores stay blind.
        if (!wire().set_tracking_checked(tracking)) {
            // With no :GE# code, refusal_code_locked() has just re-read :GU#: a
            // mount that now tracks as asked took it (#1379 review).
            if (const auto code = refusal_code_locked("Tracking");
                code && !(*code < 0 && frame_status_locked().is_tracking == tracking)) {
                throw mount_refused("Tracking", *code);
            }
        }
        // Write-through (decision 0010): the next Tracking read sees it before the next poll.
        write_status_locked([tracking](MountStatus& status) { status.is_tracking = tracking; });
        // The client has said what it wants; a pending GOTO restore must not
        // override it when the slew ends (#1230).
        tracking_before_slew_ = false;
    }

    double get_focal_length() const override { return focal_length_m_; }

    void set_focal_length(double meters) override {
        if (meters < 0.0) {
            throw AlpacaException("Focal length must be non-negative", AlpacaError::InvalidValue);
        }
        focal_length_m_ = meters;
    }

    GuideRate get_guide_rate() const override {
        return {kInformationalGuideRateDegPerSec, kInformationalGuideRateDegPerSec};
    }

    void set_guide_rate(const GuideRate& rate) override {
        (void)rate;
        throw AlpacaException("Guide rate is fixed by OnStep firmware", AlpacaError::PropertyNotImplemented);
    }

    double get_right_ascension() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        return served_position_locked().ra_hours;
    }

    double get_right_ascension_rate() const override {
        check_connected_lock_free();  // see get_declination_rate()
        throw_if_faulted_lock_free();
        return 0.0;
    }

    void set_right_ascension_rate(double rate) override {
        (void)rate;
        throw AlpacaException("Right ascension rate not supported", AlpacaError::PropertyNotImplemented);
    }

    int get_side_of_pier() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        // Always compute from hour angle — same convention as the project's
        // other GEM drivers (SynScan/iOptron/Celestron), and the only source
        // that satisfies ASCOM's SideOfPier contract (a pointing state
        // defined purely by hour-angle sign). :GU#'s E/W flag (see
        // .github/instructions/onstep.instructions.md) was tried as the primary source instead,
        // but ConformU proved it reports the mount's PHYSICAL pier
        // orientation — which only changes on an actual mechanical meridian
        // flip — not the ASCOM pointing state: "Reported SideofPier at HA
        // -9, +9: EE" (constant) instead of the required "WE" (flips with
        // HA sign), confirmed against real hardware. Do not reintroduce a
        // firmware-flag-preferred branch here without re-validating against
        // ConformU's SideofPier/DestinationSideofPier checks.
        //
        // The RA is the polled frame's (decision 0010): the hour-angle SIGN this needs tolerates the poll's
        // second of staleness, and the getter never reaches the mount (an earlier live :GRa#/:GDe# here cost
        // ConformU's 0.1 s FAST target, #1242). The site the connect or the poll read; without one the
        // longitude is 0, as before.
        const double ra = served_position_locked().ra_hours;
        const double lst = util::local_sidereal_hours(current_utc_time_locked(), site_longitude_cached_);
        const double hour_angle = util::shortest_ra_delta_hours(lst, ra);
        return util::pier_side_for_hour_angle(hour_angle);
    }

    void set_side_of_pier(int side) override {
        (void)side;
        throw AlpacaException("Pier side is not settable", AlpacaError::PropertyNotImplemented);
    }

    int get_destination_side_of_pier(double ra, double dec) const override {
        // Arguments first, even while disconnected (#1478).
        util::validate_ra_dec(ra, dec, "DestinationSideOfPier");
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        throw_if_faulted_locked();  // the site and clock the connect read (decision 0009 rule 1)
        const double lst = util::local_sidereal_hours(current_utc_time_locked(), site_longitude_cached_);
        const double hour_angle = util::shortest_ra_delta_hours(lst, ra);
        return util::pier_side_for_hour_angle(hour_angle);
    }

    EquatorialSystem get_equatorial_system() const override { return EquatorialSystem::Topocentric; }

    // OnStepX reports its refraction mode at connect (#1216). It is fixed when
    // the firmware is built, so only the value the firmware already uses can
    // be written. Older firmware keeps the local flag.
    bool get_does_refraction() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        throw_if_faulted_locked();  // the connect's :GXEE# value, a mount read (decision 0009 rule 1)
        return refraction_from_firmware_.value_or(does_refraction_);
    }

    void set_does_refraction(bool does_refraction) override {
        StateLock lock(lock_gate_, StateLock::Mode::State);  // no wire I/O, see set_target_declination()
        check_connected();
        if (refraction_from_firmware_.has_value()) {
            if (does_refraction != *refraction_from_firmware_) {
                throw AlpacaException(std::string("This OnStepX firmware ") +
                                          (*refraction_from_firmware_ ? "always applies" : "never applies") +
                                          " refraction to its coordinates; it is set when the firmware is built",
                                      AlpacaError::PropertyNotImplemented);
            }
            return;
        }
        does_refraction_ = does_refraction;
    }

    int get_slew_settle_time() const override { return slew_settle_time_seconds_; }

    void set_slew_settle_time(int seconds) override {
        if (seconds < 0) {
            throw AlpacaException("Slew settle time must be >= 0 seconds", AlpacaError::InvalidValue);
        }
        slew_settle_time_seconds_ = seconds;
    }

    double get_sidereal_time() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        throw_if_faulted_locked();  // the site and clock the connect read (decision 0009 rule 1)
        return util::local_sidereal_hours(current_utc_time_locked(), site_longitude_cached_);
    }

    // Unset (NaN) until the device config or a client sets it: 0 m is a real
    // place (decision 0019, #1478). Lock-free: the protocol has no elevation
    // command, so this never waits on the mount.
    double get_site_elevation() const override {
        const double elevation = site_elevation_m_.load();
        if (std::isnan(elevation)) {
            throw AlpacaException("SiteElevation has not been set", AlpacaError::InvalidOperation);
        }
        return elevation;
    }

    void set_site_elevation(double elevation) override {
        if (!std::isfinite(elevation) || elevation < -300.0 || elevation > 10000.0) {
            throw AlpacaException("SiteElevation must be in range -300 to 10000 meters", AlpacaError::InvalidValue);
        }
        // Client-side only: the LX200/OnStep protocol has no elevation command.
        site_elevation_m_ = elevation;
    }

    // Unknown until the device config, a client write or a :Gt#/:Gg# read set it: InvalidOperation, never a
    // default 0.0 (#1783, decision 0019, the ASCOM time and place FAQ). The poll retries an unread site; the
    // getter never reads the mount.
    double get_site_latitude() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        throw_if_faulted_locked();
        if (!site_latitude_known_) {
            throw AlpacaException(site_unknown_message("SiteLatitude"), AlpacaError::InvalidOperation);
        }
        return site_latitude_cached_;
    }

    void set_site_latitude(double latitude) override {
        util::validate_latitude(latitude);
        StateLock lock(lock_gate_);
        check_connected();
        refuse_while_faulted_locked("SiteLatitude");  // as the other writes (#1626)
        // Read the mount's pair first while it is not known, so the longitude is
        // the mount's and not a default. A failed read here is logged and leaves
        // it unknown, so the poll asks again; it does not refuse the write, whose
        // own refusal throws (#1754).
        ensure_site_info_cached_locked();
        wire().set_latitude(latitude);
        site_latitude_cached_ = latitude;
        site_latitude_known_ = true;
    }

    double get_site_longitude() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        throw_if_faulted_locked();
        if (!site_longitude_known_) {
            throw AlpacaException(site_unknown_message("SiteLongitude"), AlpacaError::InvalidOperation);
        }
        return site_longitude_cached_;
    }

    void set_site_longitude(double longitude) override {
        util::validate_longitude(longitude);
        StateLock lock(lock_gate_);
        check_connected();
        refuse_while_faulted_locked("SiteLongitude");  // as the other writes (#1626)
        // See set_site_latitude(): the mount's latitude first, while unknown.
        ensure_site_info_cached_locked();
        wire().set_longitude(longitude);
        site_longitude_cached_ = longitude;
        site_longitude_known_ = true;
    }

    bool get_slewing() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        if (park_home_request_) {
            // This answer asks the mount nothing, so it refuses a latched fault
            // itself (decision 0009 rule 1); AbortSlew and MoveAxis(axis, 0) still
            // send their stops while faulted, only this read refuses.
            throw_if_faulted_locked();
            return true;  // stopping, settling or retrying a Park or FindHome (#1430)
        }
        raise_park_home_error_locked(std::nullopt);
        return get_slewing_locked();
    }

    double get_target_declination() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        if (!target_dec_set_) {
            throw AlpacaException("Target declination has not been set", AlpacaError::ValueNotSet);
        }
        return target_dec_degrees_;
    }

    void set_target_declination(double dec) override {
        util::validate_dec(dec, "TargetDeclination");
        StateLock lock(lock_gate_, StateLock::Mode::State);  // no wire I/O: never waits behind a command (#1728 review)
        target_dec_degrees_ = dec;
        target_dec_set_ = true;
        ++target_dec_writes_;
    }

    double get_target_right_ascension() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        if (!target_ra_set_) {
            throw AlpacaException("Target right ascension has not been set", AlpacaError::ValueNotSet);
        }
        return target_ra_hours_;
    }

    void set_target_right_ascension(double ra) override {
        util::validate_ra(ra, "TargetRightAscension");
        StateLock lock(lock_gate_, StateLock::Mode::State);  // see set_target_declination()
        target_ra_hours_ = ra;
        target_ra_set_ = true;
        ++target_ra_writes_;
    }

    int get_tracking_rate() const override {
        check_connected_lock_free();
        throw_if_faulted_lock_free();  // as Sky-Watcher (decision 0009 rule 1)
        return 0;
    }

    void set_tracking_rate(int rate) override {
        if (rate != 0) {
            throw AlpacaException("Only sidereal tracking rate is supported", AlpacaError::PropertyNotImplemented);
        }
    }

    std::vector<int> get_tracking_rates() const override { return {0}; }

    std::chrono::system_clock::time_point get_utc_date() const override {
        StateLock lock(lock_gate_, StateLock::Mode::State);
        check_connected();
        throw_if_faulted_locked();  // no I/O, but a faulted read throws (decision 0009 rule 1)
        if (!utc_offset_.valid()) {
            // The host clock is not the mount's (ITelescopeV4 UTCDate, #1478). The
            // mount clock is read once at connect, so a getter never waits on it.
            throw AlpacaException("The mount clock could not be read at connect" +
                                      (mount_clock_unreadable_ ? ": " + *mount_clock_unreadable_ : std::string()) +
                                      "; write UTCDate to set it",
                                  AlpacaError::InvalidOperation);
        }
        return current_utc_time_locked();
    }

    // At connect (#1478): seeds utc_offset_ from :GL#/:GC#/:GG#, or keeps why it
    // could not, for UTCDate to report until a UTCDate write or the next connect.
    void seed_utc_from_mount_locked() {
        try {
            TimeInfo info = wire().get_time();
            using namespace std::chrono;
            int year = 2000 + info.year;
            sys_days date = sys_days{std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(info.month)} /
                                     std::chrono::day{static_cast<unsigned>(info.day)}};
            auto local_time = date + hours{info.hour} + minutes{info.minute} + seconds{info.second};
            auto offset = duration_cast<system_clock::duration>(
                std::chrono::duration<double, std::ratio<3600>>(info.utc_offset_hours));
            // OnStepX :GG# is the hours to add to local time to get UTC
            // (Site.command.cpp), so UTC = local + offset (#1626).
            utc_offset_.set(local_time + offset, clock_.now());
            mount_clock_unreadable_.reset();
        } catch (const std::exception& ex) {
            mount_clock_unreadable_ = ex.what();
        } catch (...) {
            mount_clock_unreadable_ = "unknown error";
        }
    }

    void set_utc_date(std::chrono::system_clock::time_point utc) override {
        StateLock lock(lock_gate_);
        check_connected();
        // Here, not in set_utc_date_locked(): the connect's own time sync runs
        // right after link_health_.reset(), where the check cannot fire (#1626).
        refuse_while_faulted_locked("UTCDate");
        set_utc_date_locked(utc);
    }

    // find_home()/park() are fire-and-forget, matching iOptron: send the
    // command and return immediately rather than blocking until the mount
    // physically finishes. ConformU classifies a Park/FindHome call that
    // blocks for the whole physical operation under the STANDARD (1.0s)
    // timing target and fails it outright; a call that returns immediately
    // and lets the client poll AtHome/AtPark afterward is classified
    // EXTENDED (600s) instead — confirmed against this project's iOptron
    // ConformU logs, where the real (tens-of-seconds) physical park/home
    // is entirely absorbed by the client's post-call AtPark/AtHome polling.
    // Not waited for: the physical park or home, nor (#1430) the end of a
    // motion the call had to stop first or a park or home the mount refuses
    // as "in motion": those finish in the park_home_ body, and a refusal it
    // meets is kept and raised by Slewing, AtPark or AtHome (the shared rule
    // "A failed async operation body keeps its error").
    void find_home() override {
        StateLock lock(lock_gate_);
        check_connected();
        refuse_while_faulted_locked("FindHome");
        check_not_parked_locked("FindHome");
        refresh_status_locked();  // a control-path read, written through (decision 0010)
        if (const MountStatus status = frame_status_locked(); status.is_at_home || status.is_homing) {
            park_home_error_.reset();  // a new command: an earlier body's refusal is not this call's
            return;  // there, or on its way
        }
        start_park_or_home_locked(lock, "FindHome", true);
    }

    void park() override {
        StateLock lock(lock_gate_);
        check_connected();
        refuse_while_faulted_locked("Park");
        refresh_status_locked();  // a control-path read, written through (decision 0010)
        if (const MountStatus status = frame_status_locked(); status.is_parked || status.is_parking) {
            park_home_error_.reset();  // a new command: an earlier body's refusal is not this call's
            return;  // ASCOM: Park() on an already-parked mount is a no-op; one already parking too.
        }
        start_park_or_home_locked(lock, "Park", false);
    }

    void pulse_guide(int direction, int duration) override {
        // Arguments before the connected, fault and parked checks (#1478).
        if (direction < 0 || direction > 3) {
            throw AlpacaException("Invalid PulseGuide direction", AlpacaError::InvalidValue);
        }
        // The wire command's duration field is 4 digits (:Mgn####), so a
        // longer pulse cannot be sent: refuse it instead of pulsing for less
        // than asked (#1478).
        if (duration < 0 || duration > kMaxPulseGuideMs) {
            throw AlpacaException("PulseGuide duration must be 0 to " + std::to_string(kMaxPulseGuideMs) + " ms",
                                  AlpacaError::InvalidValue);
        }
        StateLock lock(lock_gate_);
        check_connected();
        refuse_while_faulted_locked("PulseGuide");
        check_not_parked_locked("PulseGuide");
        // Hardware pulse guide: the mount times the pulse internally
        // (:Mgn####/:Mgs####/:Mge####/:Mgw####), so unlike a software-timed
        // implementation there is no background stop thread to manage here:
        // IsPulseGuiding reads the axis's hold window. Each axis has its own
        // (OnStepX times each axis on its own), so a shorter pulse on one axis
        // does not end the other's.
        const int axis = direction >= 2 ? 0 : 1;  // East/West is axis 0, North/South axis 1
        const auto now = clock_.now();
        const auto pulse = std::chrono::milliseconds(duration);
        // The other axis's pulse outlasts this one: its pulse-end body, which measures after the later end, stays.
        const bool other_ends_later = pulse_guiding_active_[1 - axis] && pulse_hold_[1 - axis].active(now + pulse);
        // Decision 0022 rule 2: armed before the command goes out, cleared if it does not.
        pulse_guiding_active_[axis] = true;
        pulse_hold_[axis].arm(now, pulse);
        try {
            wire().pulse_guide(direction, duration);
        } catch (...) {
            pulse_guiding_active_[axis] = false;
            pulse_hold_[axis].clear();
            throw;
        }
        drop_poll_in_flight_locked();  // the position changes: a tick that sampled before the pulse is dropped
        if (other_ends_later) {
            return;
        }
        // Supersede the pulse-end body of an earlier pulse here, under the locks (decision 0006 rule 3): a body
        // that wakes or takes the operation lock in the gap before start() replaces it sees the bump when its wait
        // returns or at its next stopped() check and acts on nothing (rule 6), so it sends no tracking restore and
        // publishes no frame. start() bumps again for the new body.
        pulse_measurement_generation_.bump();
        // One extra tick measures the position kPulseGuideSettle after the pulse ends (IsPulseGuiding waits for
        // that frame), in a slot of its own: the 1 s ticks run on through the pulse. Started with no driver lock
        // held (decision 0006 rule 4); a newer pulse that ends last replaces it.
        const auto wait = std::chrono::duration_cast<std::chrono::nanoseconds>(pulse + kPulseGuideSettle);
        lock.unlock();
        run_pulse_guide_gap_hook();
        // A Disconnect can land between the unlock and the start: the slot's body is then harmless, because
        // poll_status_once() checks the connection under the operation lock and the destructor joins the slot.
        start_pulse_measurement(wait);
    }

    void set_park() override {
        StateLock lock(lock_gate_);
        check_connected();
        refuse_while_faulted_locked("SetPark");
        // OnStepX refuses :hQ# while parked, during a GOTO or a guide move, or on a
        // motor fault (Park.cpp set()); a refusal used to read as success (#1379).
        if (!wire().set_park()) {
            // Unlike Unpark and Tracking, no :GU# letter shows the park position,
            // so a 0 with no :GE# code cannot be confirmed and stays a refusal.
            if (const auto code = refusal_code_locked("SetPark")) {
                throw mount_refused("SetPark", *code);
            }
        }
    }

    void slew_to_coordinates(double ra, double dec) override {
        StateLock lock(lock_gate_);
        const TargetWrites targets = target_writes_locked();  // before any check reads the mount
        check_equatorial_slew_locked(ra, dec, "SlewToCoordinates");
        start_slew_locked(ra, dec, "SlewToCoordinates", targets);
        wait_for_slew_complete_locked(lock);
    }

    void slew_to_coordinates_async(double ra, double dec) override {
        StateLock lock(lock_gate_);
        const TargetWrites targets = target_writes_locked();  // before any check reads the mount
        check_equatorial_slew_locked(ra, dec, "SlewToCoordinatesAsync");
        start_slew_locked(ra, dec, "SlewToCoordinatesAsync", targets);
    }

    // Under the target slew's own name, so a refusal names the member called (#1379).
    void slew_to_target() override {
        StateLock lock(lock_gate_);
        const TargetWrites targets = target_writes_locked();  // before any check reads the mount
        const auto [ra, dec] = target_or_throw_locked();
        check_equatorial_slew_locked(ra, dec, "SlewToTarget");
        start_slew_locked(ra, dec, "SlewToTarget", targets);
        wait_for_slew_complete_locked(lock);
    }

    void slew_to_target_async() override {
        StateLock lock(lock_gate_);
        const TargetWrites targets = target_writes_locked();  // before any check reads the mount
        const auto [ra, dec] = target_or_throw_locked();
        check_equatorial_slew_locked(ra, dec, "SlewToTargetAsync");
        start_slew_locked(ra, dec, "SlewToTargetAsync", targets);
    }

    std::pair<double, double> target_or_throw_locked() const {
        if (!target_ra_set_ || !target_dec_set_) {
            throw AlpacaException("Target coordinates have not been set", AlpacaError::ValueNotSet);
        }
        return {target_ra_hours_, target_dec_degrees_};
    }

    void sync_to_coordinates(double ra, double dec) override {
        StateLock lock(lock_gate_);
        const TargetWrites targets = target_writes_locked();  // before any check reads the mount
        sync_locked(ra, dec, "SyncToCoordinates", targets);
    }

    void sync_to_target() override {
        StateLock lock(lock_gate_);
        const TargetWrites targets = target_writes_locked();  // before any check reads the mount
        const auto [ra, dec] = target_or_throw_locked();
        sync_locked(ra, dec, "SyncToTarget", targets);
    }

    // Under the member's own name (#1379). A sync during a GOTO is refused by the
    // mount (Goto.cpp validate(): CE_SLEW_IN_SLEW) and used to read as success, as
    // did every :CM# refusal; the driver refuses first and reads the reply.
    void sync_locked(double ra, double dec, const char* who, TargetWrites targets) {
        util::validate_ra_dec(ra, dec, who);  // arguments first, even while disconnected (#1478)
        check_connected();
        refuse_while_faulted_locked(who);
        check_not_parked_locked(who);
        refuse_while_goto_running_locked(who);
        const Wire protocol = wire();
        // Use the mount's own sync command — do NOT maintain a driver-level
        // offset, which would cause coordinate divergence during subsequent
        // slews (project-wide rule; see the iOptron and Celestron files under .github/instructions/).
        protocol.set_target_ra(ra);
        protocol.set_target_dec(dec);
        const std::string reply = protocol.sync_to_target();
        if (reply.size() >= 2 && reply[0] == 'E' && std::isdigit(static_cast<unsigned char>(reply[1]))) {
            throw digit_refused(who, RefusedCommand::Sync, reply[1], reply);
        }
        store_target_locked(ra, dec, targets);
        // Write-through (decision 0010): the mount now reports the synced position; a tick that sampled before
        // the sync is dropped.
        snapshot_.write([ra, dec](PolledFrame& frame) {
            frame.ra_hours = ra;
            frame.dec_degrees = dec;
            frame.equatorial_known = true;
        });
    }

    void unpark() override {
        StateLock lock(lock_gate_);
        check_connected();
        refuse_while_faulted_locked("Unpark");
        supersede_park_or_home_locked();  // a newer motion command wins (#1430)
        // A refusal used to read as success (#1379): OnStepX refuses :hR# before the
        // date and time are set or on a motor fault (Park.cpp restore()). "Not
        // parked" (11) is ASCOM's no-op, not a refusal.
        if (!wire().unpark()) {
            // 11 (not parked) is ASCOM's no-op; with no :GE# code, the :GU# that
            // refusal_code_locked() has just re-read decides (#1379 review).
            if (const auto code = refusal_code_locked("Unpark");
                code && *code != kCeNotParked && !(*code < 0 && !frame_status_locked().is_parked)) {
                throw mount_refused("Unpark", *code);
            }
        }
        write_status_locked([](MountStatus& status) { status.is_parked = false; });  // decision 0010 write-through
    }

    bool get_can_move_axis(int axis) const override {
        if (axis != 0 && axis != 1 && axis != 2) {
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }
        return axis == 0 || axis == 1;
    }

    void move_axis(int axis, double rate) override {
        StateLock lock(lock_gate_);
        check_connected();
        check_not_parked_locked("MoveAxis");
        if (axis != 0 && axis != 1) {
            throw AlpacaException("MoveAxis axis must be 0 or 1", AlpacaError::InvalidValue);
        }
        if (std::isnan(rate) || std::isinf(rate)) {
            throw AlpacaException("MoveAxis rate must be finite", AlpacaError::InvalidValue);
        }
        if (std::abs(rate) > kMaxMoveAxisRateDegPerSec) {
            throw AlpacaException("MoveAxis rate exceeds supported range", AlpacaError::InvalidValue);
        }

        constexpr double kStopEpsilon = 1e-6;
        const bool moving = std::abs(rate) > kStopEpsilon;
        if (moving) {
            refuse_while_faulted_locked("MoveAxis");  // before it ends a waiting Park or FindHome
        }
        supersede_park_or_home_locked();  // a newer motion command wins (#1430)
        const Wire protocol = wire();

        // Direction codes: 0=North(Dec+), 1=South(Dec-), 2=East(RA+), 3=West(RA-).
        const int positive_dir = (axis == 0) ? 2 : 0;
        const int negative_dir = (axis == 0) ? 3 : 1;

        if (!moving) {
            const std::string stop_error = stop_axis_locked(axis);
            if (!stop_error.empty()) {
                throw AlpacaException("MoveAxis stop failed: " + stop_error, AlpacaError::DriverException);
            }
            // :Qe#/:Qw# and :Qn#/:Qs# end every guide on that axis, a pulse
            // guide included (OnStepX Guide.command.cpp stopAxis1/stopAxis2).
            pulse_guiding_active_[axis] = false;
            pulse_hold_[axis].clear();
        }
        std::uint64_t generation = 0;
        if (moving) {
            // Drop this axis's earlier move from the re-send first: the new
            // move sets it again only once its own command went out. This
            // only withdraws the old move, under mutex_ where no body runs;
            // decision 0006 rule 3's bump still comes before the new flags.
            manual_axis_resend_[axis] = false;
            const int dir = rate > 0.0 ? positive_dir : negative_dir;
            protocol.move_axis_start(dir, std::abs(rate));
            // Decision 0006 rule 3: supersede the running re-send under mutex_
            // before this move's flags change; start() below replaces it. Only
            // once the move went out: a failed send must leave the other
            // axis's re-send running.
            generation = move_axis_generation_.bump();
            manual_axis_dir_[axis] = dir;
            manual_axis_rate_[axis] = std::abs(rate);
            manual_axis_resend_[axis] = true;
            manual_axis_resend_due_[axis] = clock_.now() + kMoveAxisResendInterval;
            status_read_since_move_ = false;  // a :GU# from before the move must not end it (#1378)
            drop_poll_in_flight_locked();     // nor a poll tick that sampled before it
        }

        manual_axis_slewing_[axis] = moving;
        if (!moving) {
            if (!manual_axis_slewing_[0] && !manual_axis_slewing_[1]) {
                clear_manual_motion_locked();
            }
            // Sent, but a silent mount may not have heard it (#1414).
            refuse_while_faulted_locked("MoveAxis stop");
            return;
        }
        // Always a fresh body, never "already running": a body that has just
        // found nothing to re-send may still read as running while it exits.
        // The new one supersedes it.
        lock.unlock();  // start() without mutex_: decision 0006 rule 4, async_operation.h rule 10
        try {
            start_move_axis_resend();
        } catch (const AlpacaException& ex) {
            lock.lock();
            stop_move_without_resend_locked(axis, generation, ex);
        }
    }

    // Stops `axis` as MoveAxis(axis, 0) does and drops its re-send first: a
    // stop asks for none whatever its outcome. Defensive dual-stop, matching
    // the project's convention for fixed-direction (rather than signed-rate)
    // motion protocols. Both stops are always tried. The driver cannot tell
    // which direction the mount needed stopped, so any failed stop may be the
    // one that mattered: the first error is returned, and the caller keeps
    // the axis moving (#742). Empty when both went out.
    std::string stop_axis_locked(int axis) {
        manual_axis_resend_[axis] = false;
        // Direction codes: 0=North(Dec+), 1=South(Dec-), 2=East(RA+), 3=West(RA-).
        return send_stops_locked(false, {axis == 0 ? 2 : 0, axis == 0 ? 3 : 1});
    }

    // The stops this driver sends, from AbortSlew, MoveAxis(axis, 0) (stop_axis_locked()), the stops before
    // a Park or FindHome, a refused MoveAxis re-send and the relink after a long outage: :Q# when
    // `abort_goto`, then :Qn#/:Qs#/:Qe#/:Qw# for each direction in `directions`. Each is tried on its own,
    // so one lost command cannot skip the others (#781). They go through Wire: the operation lock keeps
    // them together, and mutex_ is free while they are on the wire (decision 0016, #1449). Returns the
    // first error, empty when every stop went out; `on_failure`, when set, also sees each failure with the
    // command it was.
    std::string send_stops_locked(
        bool abort_goto, std::initializer_list<int> directions,
        const std::function<void(const char*, const std::string&)>& on_failure = nullptr) const {
        const Wire protocol = wire();
        std::string stop_error;
        const auto try_stop = [&](const char* what, const std::function<void()>& stop) {
            std::string error;
            try {
                stop();
                return;
            } catch (const std::exception& ex) {
                error = ex.what();
            } catch (...) {
                error = "unknown exception";
            }
            if (stop_error.empty()) {
                stop_error = error;
            }
            if (on_failure) {
                on_failure(what, error);
            }
        };
        if (abort_goto) {
            try_stop(":Q#", [&protocol]() { protocol.abort_slew(); });
        }
        for (const int dir : directions) {
            try_stop("axis stop", [&protocol, dir]() { protocol.move_axis_stop(dir); });
        }
        return stop_error;
    }

    // #1458: the slot refused the re-send body (no thread, or replaced bodies
    // still finishing) after the move went out. The bump above has already
    // superseded the running body, so the move would end at the firmware's
    // time limit while Slewing read true: stop this axis and report it, as a
    // MoveAxis that did not happen. Nothing is stopped when a newer MoveAxis
    // went out meanwhile (the generation moved on), or when AbortSlew, a GOTO,
    // Park, FindHome or a disconnect ended the move meanwhile (the axis no
    // longer reads as moving): its stops would end that newer command. After
    // a link drop the stops fail and the message says so. The other axis keeps
    // moving, without a re-send until the next MoveAxis.
    void stop_move_without_resend_locked(int axis, std::uint64_t generation, const AlpacaException& refusal) {
        if (move_axis_generation_.current() != generation || !manual_axis_slewing_[axis]) {
            throw refusal;
        }
        std::string message = std::string("MoveAxis could not keep the move going (") + refusal.what() + ")";
        const std::string stop_error = stop_axis_locked(axis);
        if (stop_error.empty()) {
            manual_axis_slewing_[axis] = false;
            message += "; the axis was stopped";
        } else {
            message += "; stopping the axis failed too: " + stop_error;  // Slewing stays true (#742)
        }
        if (!manual_axis_slewing_[0] && !manual_axis_slewing_[1]) {
            clear_manual_motion_locked();
        }
        ALPACA_LOG_WARN("OnStep", message);
        throw AlpacaException(message, AlpacaError::DriverException);
    }

    std::pair<double, double> get_axis_rate_range(int axis) const override {
        if (axis == 2) {
            return {0.0, 0.0};
        }
        if (axis != 0 && axis != 1) {
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }
        // OnStep's continuous-motion commands run at a single
        // firmware-selected rate (pinned to max at connect — see
        // select_max_slew_rate()), not an arbitrary requested deg/s value.
        // The advertised range therefore covers what MoveAxis will accept
        // without throwing, not a literal achievable-velocity curve.
        return {0.0, kMaxMoveAxisRateDegPerSec};
    }

    std::vector<std::pair<double, double>> get_axis_rate_ranges(int axis) const override {
        if (axis == 2) {
            return {};  // Tertiary axis unsupported.
        }
        if (axis != 0 && axis != 1) {
            throw AlpacaException("Invalid axis: " + std::to_string(axis), AlpacaError::InvalidValue);
        }
        return {{0.0, kMaxMoveAxisRateDegPerSec}};
    }

    void abort_slew() override {
        StateLock lock(lock_gate_);
        check_connected();
        check_not_parked_locked("AbortSlew");
        supersede_park_or_home_locked();  // also ends a Park or FindHome still finishing (#1430)
        // Every stop on its own, so one lost command cannot skip the others (#781).
        const std::string stop_error = send_stops_locked(true, {0, 1, 2, 3});
        if (!stop_error.empty()) {
            // The mount may still be moving: leave Slewing as it was, but
            // stop re-sending a MoveAxis the client asked to end.
            manual_axis_resend_[0] = false;
            manual_axis_resend_[1] = false;
            throw AlpacaException("AbortSlew stop failed: " + stop_error, AlpacaError::DriverException);
        }
        slew_hold_.clear();
        clear_manual_motion_locked();
        clear_pulse_guides_locked();    // :Q# ends every guide, a pulse guide included (Guide::stop())
        drop_poll_in_flight_locked();   // a tick that sampled the motion before the stops is dropped
        // ASCOM: after AbortSlew, tracking returns to its state before the
        // slew. Decide now instead of at a later slew end the driver may never
        // see (#1230). Only an aborted GOTO costs the extra :GU#. A latched
        // fault gives the restore up without asking (the call refuses below).
        if (tracking_before_slew_) {
            try {
                if (link_health_.faulted()) {
                    throw AlpacaException("faulted", AlpacaError::DriverException);
                }
                refresh_status_locked();
                finish_slew_tracking_from_frame_locked();
            } catch (const AlpacaException& ex) {
                tracking_before_slew_ = false;  // status unknown: never restart blind
                if (ex.error_code() == AlpacaError::NotConnected) {
                    throw;  // the link is gone: reported, not swallowed (#1783)
                }
            } catch (...) {
                tracking_before_slew_ = false;
            }
        }
        // The stops went out, but a silent mount may not have heard them
        // (#1414). Checked after the tracking decision, so a faulted abort
        // still gives up the restore instead of leaving it for a later slew end.
        refuse_while_faulted_locked("AbortSlew");
    }

    // An Alt/Az slew is the opposite of an equatorial one (open-astro#1274): ITelescopeV4
    // refuses it while Tracking is true (#1377), since a tracking mount drifts off the
    // requested Alt/Az. It converts to RA/Dec and runs the same GOTO.
    void slew_to_alt_az(double altitude, double azimuth) override {
        double ra = 0.0;
        double dec = 0.0;
        StateLock lock(lock_gate_);
        const TargetWrites targets = target_writes_locked();  // before any check reads the mount
        check_alt_az_slew_locked(altitude, azimuth, ra, dec, "SlewToAltAz");
        start_slew_locked(ra, dec, "SlewToAltAz", targets);
        wait_for_slew_complete_locked(lock);
    }

    void slew_to_alt_az_async(double altitude, double azimuth) override {
        double ra = 0.0;
        double dec = 0.0;
        StateLock lock(lock_gate_);
        const TargetWrites targets = target_writes_locked();  // before any check reads the mount
        check_alt_az_slew_locked(altitude, azimuth, ra, dec, "SlewToAltAzAsync");
        start_slew_locked(ra, dec, "SlewToAltAzAsync", targets);
    }

    void sync_to_alt_az(double altitude, double azimuth) override {
        (void)altitude;
        (void)azimuth;
        throw AlpacaException("SyncToAltAz not supported", AlpacaError::MethodNotImplemented);
    }

    void set_park_home_before_start_for_testing(std::function<void()> hook) {
        StateLock lock(lock_gate_);
        park_home_before_start_ = std::move(hook);
    }

    void set_move_axis_resend_spawn_for_testing(std::function<std::thread(std::function<void()>)> spawn) {
        move_axis_resend_.set_spawn_for_testing(std::move(spawn));
    }

    void set_park_home_spawn_for_testing(std::function<std::thread(std::function<void()>)> spawn) {
        park_home_.set_spawn_for_testing(std::move(spawn));
    }

    void set_disconnect_before_close_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
        disconnect_before_close_hook_ = std::move(hook);
    }

    void set_move_axis_resend_before_send_hook_for_testing(std::function<bool()> hook) {
        std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
        move_axis_resend_before_send_hook_ = std::move(hook);
    }

    // Stops (true: the poll body is joined, so it parks no waiter on a fake task clock) or resumes (false) the
    // status poll. While paused a connect does not start it. Call with no driver lock held.
    void set_status_poll_paused_for_testing(bool paused) {
        status_poll_paused_for_testing_ = paused;
        if (paused) {
            stop_status_poller();
        } else if (published_connected_.load()) {
            start_status_poller(std::chrono::nanoseconds::zero());
        }
    }

    // One tick on the calling thread, paused or not (`full`: Alt/Az and the site retry too). False when the
    // connection is gone or lost.
    bool poll_status_now_for_testing(bool full) { return poll_status_once(nullptr, full ? 0 : 1); }

    // False once the poll body is cancelled (the slot hands it to the joiner), while it may still be sampling.
    bool status_poll_running_for_testing() const { return status_poll_.running(); }

    void set_status_poll_after_sample_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
        status_poll_after_sample_hook_ = std::move(hook);
    }

    // False once the pulse-end body has returned (or none was started).
    bool pulse_measurement_running_for_testing() const { return pulse_measurement_.running(); }

    void set_pulse_guide_gap_hook_for_testing(std::function<void()> hook) {
        std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
        pulse_guide_gap_hook_ = std::move(hook);
    }

private:
    // Test seam: runs in the re-send tick after its reads, under mutex_ and io_mutex_; true makes the tick
    // end the move there, as a cancel landing between the reads and the re-send would.
    bool run_move_axis_resend_before_send_hook() const {
        std::function<bool()> hook;
        {
            std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
            hook = move_axis_resend_before_send_hook_;
        }
        return hook && hook();
    }

    // Test seam: runs in a poll tick between its sample and its publish, with no driver lock held.
    void run_status_poll_after_sample_hook() const {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
            hook = status_poll_after_sample_hook_;
        }
        if (hook) {
            hook();
        }
    }

    // Test seam: runs in pulse_guide() after it unlocks and before the pulse-end body starts, with no driver lock
    // held: the gap in which an earlier pulse's body can take the operation lock.
    void run_pulse_guide_gap_hook() const {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
            hook = pulse_guide_gap_hook_;
        }
        if (hook) {
            hook();
        }
    }

    // Test seam: runs in a clean disconnect's close window, with mutex_ released, before the close.
    void run_disconnect_before_close_hook() const {
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> hook_lock(disconnect_hook_mutex_);
            hook = disconnect_before_close_hook_;
        }
        if (hook) {
            hook();
        }
    }

    void check_connected() const {
        // A connect in progress is not a connection to a client: until it has finished (published_connected_)
        // a getter reads NotConnected instead of waiting for it, as Connected does (#910). The connect's own
        // thread passes on connected_, since it holds the operation lock, which no getter holds (decision 0016).
        if (!connected_ || (!holds_io_lock() && !published_connected_.load())) {
            throw AlpacaException("Not connected to OnStep mount", AlpacaError::NotConnected);
        }
        // A removed or re-enumerated adapter is NotConnected, not a generic
        // send failure while Connected still reads true (#1241).
        auto& link = OnStepProtocolWrapper::instance();  // link_alive() does no I/O
        if (!link.link_alive()) {
            const std::string reason = link.link_lost_reason();
            throw AlpacaException("OnStep link lost" + (reason.empty() ? std::string{} : " (" + reason + ")") +
                                      "; set Connected true to reconnect",
                                  AlpacaError::NotConnected);
        }
    }

    // Ends the driver's record of a MoveAxis and its re-send (#1378).
    // cancel() may be called with mutex_ held (async_operation.h rule 10).
    // Slews, Park, FindHome and AbortSlew end the re-send here with cancel() rather
    // than a supersede (decision 0006 rule 3): safe, because the body checks
    // ctx.stop_reason() under mutex_ before its tick starts, and those commands
    // hold io_mutex_, so none of them lands inside a tick (decision 0016). A
    // Slewing read that finds the move ended in the polled frame calls this
    // with mutex_ alone, which a tick drops for each exchange: the tick
    // re-checks the stop reason and each axis's re-send flag under mutex_
    // right before it re-sends (resend_tick_locked()).
    // Every pulse guide ended (a :Q#, a connect or a disconnect): both axes' windows cleared (decision 0022 rule 2).
    void clear_pulse_guides_locked() const {
        for (int axis = 0; axis < 2; ++axis) {
            pulse_guiding_active_[axis] = false;
            pulse_hold_[axis].clear();
        }
    }

    void clear_manual_motion_locked() const {
        manual_axis_slewing_[0] = false;
        manual_axis_slewing_[1] = false;
        manual_axis_resend_[0] = false;
        manual_axis_resend_[1] = false;
        move_axis_resend_.cancel();
    }

    // :GU# says the firmware ended the running MoveAxis (#1378): no 'g',
    // from firmware that has shown 'g' since this connect. 'N' covers a GOTO
    // only. Firmware that never sends 'g' never ends it here, and a pulse
    // guide ('G') hides 'g' while it runs, so it is no evidence either. Only
    // a :GU# read after the move was sent counts (the firmware runs commands
    // in order), from the poll or a control path: the write-throughs of the
    // setters (Tracking, park, unpark, FindHome) keep its older 'g'.
    bool manual_move_ended_locked(const MountStatus& status) const {
        if (!status_read_since_move_ || status.is_pulse_guide_active) {
            return false;
        }
        if (status.is_guide_active) {
            firmware_reports_guide_move_ = true;
            return false;
        }
        return firmware_reports_guide_move_;
    }

    // open-astro#1378: OnStepX stops an equatorial mount at the horizon and
    // overhead limits only on the crossing (Limits::poll calls stop() when the
    // error turns on), and Guide::validate skips limits.isError() for a custom
    // rate, so a re-send after that crossing, or during the braking that
    // follows it, would carry the mount past the limit. Re-send only with the
    // altitude at least kAltitudeLimitMarginDeg inside both limits. The margin
    // covers the whole-degree :Gh#/:Go# replies and the pointing model. A
    // failed or out-of-range read sends nothing. The meridian, axis and switch
    // limits need no check here: OnStepX enforces them on every poll and
    // refuses a move further out (Guide::validAxis1/validAxis2).
    bool resend_within_altitude_limits_locked(const Wire& protocol) const {
        double altitude = 0.0;
        AltitudeLimits limits;
        try {
            altitude = protocol.get_alt_az().altitude_degrees;
            limits = protocol.get_altitude_limits();
        } catch (const std::exception& ex) {
            ALPACA_LOG_WARN("OnStep", std::string("MoveAxis re-send skipped, altitude read failed: ") + ex.what());
            return false;
        }
        if (limits.horizon_degrees < -30.0 || limits.horizon_degrees > 30.0 || limits.overhead_degrees < 60.0 ||
            limits.overhead_degrees > 90.0) {
            ALPACA_LOG_WARN("OnStep", "MoveAxis re-send skipped, altitude limits out of range");
            return false;
        }
        const bool overhead_on = limits.overhead_degrees < 90.0;
        if (altitude < limits.horizon_degrees + kAltitudeLimitMarginDeg ||
            (overhead_on && altitude > limits.overhead_degrees - kAltitudeLimitMarginDeg)) {
            char text[160];
            std::snprintf(
                text, sizeof(text),
                "MoveAxis not re-sent: altitude %.2f deg is within %.0f deg of, or past, a limit (horizon %.0f, "
                "overhead %.0f); the firmware ends the move",
                altitude, kAltitudeLimitMarginDeg, limits.horizon_degrees, limits.overhead_degrees);
            ALPACA_LOG_INFO("OnStep", text);  // a client can cause it: decision 0007 rule 9
            return false;
        }
        return true;
    }

    // open-astro#1378: OnStepX ends a :Mn#/:Ms#/:Me#/:Mw# move after
    // GUIDE_TIME_LIMIT (default 10 s, 1-120 s or 0 for none at firmware
    // build; Guide.command.cpp), but ASCOM runs a MoveAxis until the client
    // sends rate 0. Each repeat of the move restarts that timer
    // (Guide::startAxis1), so re-send every running move while one is set,
    // every 7 s by default: inside the 10 s default, and seldom, because a
    // move command that lands while an axis brakes after a stop restarts it
    // (Axis::autoSlew; 'g' stays set for about 0.4 s of braking at 0.5 deg/s
    // on the HM-17). Each tick reads :GU# first and re-sends only while the
    // mount shows the move ('g') in that read, so a stop from a hand
    // controller or another app is undone only when the tick lands in that
    // braking. Near the horizon and overhead limits nothing is re-sent (see
    // resend_within_altitude_limits_locked()). A failed read skips the tick.
    // A lost link ends the re-send; the #1241 path handles the link and the
    // firmware limit stops the mount, as before.
    void start_move_axis_resend() {
        move_axis_resend_.start([this](util::OperationContext& ctx) {
            const auto wants_resend = [this](int axis) {
                return manual_axis_slewing_[axis] && manual_axis_resend_[axis];
            };
            for (;;) {
                // Each axis is due on its own clock, as OnStepX times each
                // axis on its own: a MoveAxis on the other axis replaces this
                // body but must not push this axis's re-send back.
                std::chrono::steady_clock::duration wait{};
                {
                    // Reads the due times only: the state lock, no operation lock.
                    StateLock lock(lock_gate_, StateLock::Mode::State);
                    if (ctx.stop_reason() != util::StopReason::None || !connected_) {
                        return;
                    }
                    if (!wants_resend(0) && !wants_resend(1)) {
                        return;  // stopped, even if the stop failed: nothing left to keep going
                    }
                    auto earliest = std::chrono::steady_clock::time_point::max();
                    for (int axis = 0; axis < 2; ++axis) {
                        if (wants_resend(axis)) {
                            earliest = std::min(earliest, manual_axis_resend_due_[axis]);
                        }
                    }
                    wait = std::max(earliest - clock_.now(), std::chrono::steady_clock::duration::zero());
                }
                if (!ctx.wait_for(wait)) {
                    return;
                }
                // The tick holds the operation lock across its serial exchanges
                // (:GU#, the altitude reads, the re-send) and drops mutex_ for
                // each (decision 0016, #1449), so a getter never waits for it.
                StateLock lock(lock_gate_);
                if (ctx.stop_reason() != util::StopReason::None || !connected_) {
                    return;
                }
                const auto now = clock_.now();
                bool due[2] = {false, false};
                for (int axis = 0; axis < 2; ++axis) {
                    due[axis] = wants_resend(axis) && manual_axis_resend_due_[axis] <= now;
                }
                if (!due[0] && !due[1]) {
                    continue;  // a newer MoveAxis moved the due time
                }
                const bool keep_going = resend_tick_locked(ctx, due);
                // Due again a full interval after this tick ends, whatever it
                // did: a skipped tick never spins, and mutex_ is free between
                // ticks.
                const auto next = clock_.now() + kMoveAxisResendInterval;
                for (int axis = 0; axis < 2; ++axis) {
                    if (due[axis]) {
                        manual_axis_resend_due_[axis] = next;
                    }
                }
                if (!keep_going) {
                    return;
                }
            }
        });
    }

    // One re-send tick for the axes in `due` (#1378). False ends the body.
    bool resend_tick_locked(const util::OperationContext& ctx, const bool (&due)[2]) {
        const Wire protocol = wire();
        if (!protocol.link_alive()) {
            return false;
        }
        MountStatus status;
        try {
            status = protocol.get_status();
        } catch (const std::exception& ex) {
            ALPACA_LOG_WARN("OnStep", std::string("MoveAxis re-send skipped, status read failed: ") + ex.what());
            return true;
        }
        note_status_read_locked(status, true);  // a control-path read, written through (decision 0010)
        if (manual_move_ended_locked(status)) {
            clear_manual_motion_locked();  // cancel() from its own body is allowed
            return false;
        }
        // Re-send only a move the mount shows now. A move the firmware let
        // lapse stays ended: its time limit, or the 5 s GUIDE_TIME_NO_LIMITS
        // cap while its soft limits are off (Guide::finalizeTimeLimit), which
        // asks for a fresh command on purpose. Firmware without 'g' gets no
        // re-send, and a pulse guide ('G') skips the tick.
        if (!status.is_guide_active) {
            // No 'g' since this connect and no pulse guide hiding it: firmware
            // without the letter, or a move that lapsed before this tick. The
            // body ends instead of reading :GU# every 7 s for nothing (#1461);
            // Slewing stays true until the client stops.
            // (After 'g' was seen, a read without it and without 'G' already
            // ended the move above.)
            return status.is_pulse_guide_active;
        }
        if (!resend_within_altitude_limits_locked(protocol)) {
            return true;  // the firmware ends the move; 'g' then ends it here
        }
        if (run_move_axis_resend_before_send_hook()) {
            clear_manual_motion_locked();  // test seam: a cancel that landed in the gap
        }
        for (int axis = 0; axis < 2; ++axis) {
            // Re-checked under mutex_ after the reads above, which dropped it (decision 0006 rule 3):
            // a cancel, or a stop that cleared this axis's re-send, may have landed meanwhile.
            if (ctx.stop_reason() != util::StopReason::None) {
                return false;
            }
            if (!due[axis] || !manual_axis_resend_[axis]) {
                continue;
            }
            try {
                protocol.move_axis_start(manual_axis_dir_[axis], manual_axis_rate_[axis]);
            } catch (const std::exception& ex) {
                ALPACA_LOG_WARN("OnStep", std::string("MoveAxis re-send failed: ") + ex.what());
            }
        }
        return true;
    }

    // open-astro#1430: OnStepX refuses a park or a home while a guide move
    // (a MoveAxis, or its braking), a pulse guide, a GOTO or a home runs
    // (Park::request, Home::request: CE_SLEW_IN_MOTION). Park and FindHome
    // are asynchronous initiators (the 1 s STANDARD target), so they only do
    // what is quick here: when that motion runs they send AbortSlew's stops
    // (a stop that cannot be sent throws), and on a still mount they send the
    // park or home and read its answer at once, so a refusal a retry cannot
    // change (no park position set, outside limits) throws from the call.
    // The rest runs in park_home_ with both driver locks released between steps: wait
    // until :GU# shows the mount still (no 'g', 'G', GOTO or 'h'; up to
    // kStopSettleTimeout), then send and retry "in motion" refusals (up to
    // kInMotionRetryWindow). A refusal there is kept and raised by Slewing,
    // and by AtPark or AtHome, until the next motion command or a reconnect.
    // A newer motion command (supersede_park_or_home_locked()) ends the body
    // without sending anything. Park and home end with tracking off, so no
    // tracking restore is kept (#1218). The stops, the :GU# polls, :hP# and
    // :GE# (up to 1 s each without a reply; OnStep 4.x can take that long) and
    // :hC# hold the operation lock, so no other command lands between :hP# and
    // the :GE# that explains it, and run with mutex_ free (decision 0016, #1449):
    // a getter answers meanwhile.
    void start_park_or_home_locked(StateLock& lock, const char* who, bool home) {
        supersede_park_or_home_locked();
        const std::uint64_t claim = park_home_claim_;
        const bool stopped = stop_motion_before_park_or_home_locked(who);
        int code = -1;
        if (!stopped) {
            const ParkHomeTry tried = try_park_or_home_locked(who, home, code);
            if (tried == ParkHomeTry::Taken) {
                finish_park_or_home_locked(home);
                return;
            }
            if (tried == ParkHomeTry::AlreadyParked) {
                return;
            }
        }
        // Decision 0006 rule 3: claimed under mutex_ before the unlock, so a
        // newer motion command in the gap before start() still ends this one,
        // and Slewing reads true from here on. The body runs whatever request
        // is current when it starts, so a newer Park or FindHome whose start()
        // lands first is still carried out by the body started last.
        park_home_request_ = ParkHomeRequest{claim, who, home, stopped, code};
        // Test seam, read under mutex_.
        const auto before_start = park_home_before_start_;
        lock.unlock();  // start() without mutex_: decision 0006 rule 4
        if (before_start) {
            before_start();  // lands a call in the gap
        }
        try {
            park_home_.start([this](util::OperationContext& ctx) { run_park_or_home(ctx); });
        } catch (const AlpacaException&) {
            lock.lock();
            if (park_home_request_ && park_home_request_->claim == claim) {
                park_home_request_.reset();  // no body will finish it
            }
            throw;
        }
    }

    bool moving_locked() const {
        const MountStatus status = frame_status_locked();
        return manual_axis_slewing_[0] || manual_axis_slewing_[1] || status.is_slewing || status.is_guide_active ||
               status.is_pulse_guide_active || status.is_homing;
    }

    // Sends AbortSlew's stops when the mount moves: true when it did.
    bool stop_motion_before_park_or_home_locked(const char* who) {
        if (!moving_locked()) {
            return false;
        }
        const std::string stop_error = send_stops_locked(true, {0, 1, 2, 3});
        if (!stop_error.empty()) {
            // The mount may still be moving: leave Slewing as it was, but stop
            // re-sending a MoveAxis, as AbortSlew does (#781).
            manual_axis_resend_[0] = false;
            manual_axis_resend_[1] = false;
            throw AlpacaException(std::string(who) + ": could not stop the running motion first: " + stop_error,
                                  AlpacaError::DriverException);
        }
        clear_manual_motion_locked();
        clear_pulse_guides_locked();  // AbortSlew's stops end every guide, a pulse guide included
        slew_hold_.clear();
        tracking_before_slew_ = false;
        return true;
    }

    // One park (:hP#, '1' or '0') or home (:hC#, no reply) try, with :GE#
    // read right after it, before any other command replaces the code
    // (OnStepX and OnStep 4.x have it). True when the mount took it. A
    // refusal a retry may change sets `code` (22 "in motion", or -1 for a
    // park on firmware without :GE#, whose reason is unknown) and returns
    // false; any other refusal throws InvalidOperation. Without a code a
    // home counts as started, as before. A park the mount answers "already
    // parked" (9) is ASCOM's no-op and a home it answers "mount parked" (18)
    // is InvalidWhileParked, as when the call's :GU# read shows it parked: OnStep
    // 4.x (Park.ino park(), Home.ino goHome()) gives them when it parked after that read.
    enum class ParkHomeTry : std::uint8_t { Taken, AlreadyParked, Retry };
    ParkHomeTry try_park_or_home_locked(const char* who, bool home, int& code) {
        const Wire protocol = wire();
        if (home) {
            protocol.find_home();
            code = last_command_error_locked(who).value_or(kCeNone);
        } else {
            if (protocol.park()) {
                return ParkHomeTry::Taken;
            }
            code = last_command_error_locked(who).value_or(-1);
        }
        if (code == kCeNone || code == kCeTrue) {
            return ParkHomeTry::Taken;  // for a park: the reply was lost or garbled, but the mount took it
        }
        if (!home && code == kCeParked) {
            write_status_locked([](MountStatus& status) {  // the mount says so; AtPark reads it
                status.is_parked = true;
                status.is_slewing = false;
            });
            try {
                refresh_status_locked();  // best-effort: a failed read does not make the no-op a refusal
            } catch (const AlpacaException& ex) {
                // A latched fault is swallowed here too: the mount has just
                // answered "already parked", so the no-op stands (Park's call
                // refuses a fault before it gets here; its background retries
                // can reach this with one latched). Only a lost link is reported.
                if (ex.error_code() == AlpacaError::NotConnected) {
                    throw;  // the link is gone, as for any read
                }
            }
            return ParkHomeTry::AlreadyParked;
        }
        if (home && code == kCeSlewInPark) {
            throw AlpacaException(std::string(who) + " is not allowed while parked", AlpacaError::InvalidWhileParked);
        }
        if (code != kCeSlewInMotion && !(code < 0 && !home)) {
            throw mount_refused(who, code);
        }
        return ParkHomeTry::Retry;
    }

    // :GE# with no code is firmware without it, or a mount that stopped
    // answering (#1414): a :GU# read tells them apart. A silent mount counts
    // as a failed read toward the link fault, and the park or home fails
    // instead of reading as accepted. Unlike refresh_status_locked(), the
    // :GU# read here leaves status_read_since_move_ alone on purpose: the #1378
    // 'g' check then waits for the next status read, the safe side, so do not
    // "fix" it without a test for that check.
    std::optional<int> last_command_error_locked(const char* who) {
        const Wire protocol = wire();
        if (const auto code = protocol.get_last_command_error()) {
            return code;
        }
        try {
            const MountStatus status = protocol.get_status();
            note_read_ok_locked();
            note_status_read_locked(status, false);
        } catch (const std::exception& ex) {
            note_read_failure_locked(ex);  // throws once the fault latched or the link went
            if (const auto* alpaca = dynamic_cast<const AlpacaException*>(&ex);
                alpaca != nullptr && alpaca->error_code() == AlpacaError::NotConnected) {
                throw;  // the link is gone: NotConnected, as for any other call
            }
            throw AlpacaException(std::string(who) + ": the mount did not answer after the command (" + ex.what() + ")",
                                  AlpacaError::DriverException);
        }
        return std::nullopt;
    }

    // A numeric command the mount answered '0' (or garbled): the reason is the :GE#
    // code. As for :hP# (#1430), a :GE# that then says 0 or 25 means it was taken.
    // Returns the refusal code (-1 when :GE# gave none), or nullopt when accepted.
    std::optional<int> refusal_code_locked(const char* who) {
        const int code = last_command_error_locked(who).value_or(-1);
        if (code == kCeNone || code == kCeTrue) {
            return std::nullopt;
        }
        return code;
    }

    static AlpacaException mount_refused(const char* who, int code) {
        return AlpacaException(std::string(who) + " refused by the mount (" +
                                   (code < 0 ? std::string("no reason given") : describe_onstepx_error(code)) + ")",
                               AlpacaError::InvalidOperation);
    }

    // The mount took the park or home: it now moves to it with tracking off.
    void finish_park_or_home_locked(bool home) {
        tracking_before_slew_ = false;  // never restart tracking afterwards (#1218)
        write_status_locked([home](MountStatus& status) {  // decision 0010 write-through
            status.is_at_home = false;
            status.is_slewing = true;
            if (!home) {
                status.is_parked = false;
            }
        });
        if (!home) {
            clear_manual_motion_locked();
        }
    }

    // The park_home_ body: settle (when the call sent stops), then send and
    // retry. Waits on the task clock with both driver locks released (decision
    // 0005); its exchanges hold the operation lock only (decision 0016).
    void run_park_or_home(util::OperationContext& ctx) {
        StateLock lock(lock_gate_);
        // A body stopped before it runs takes no request: a newer Park or
        // FindHome may have stored its own after cancelling this body and
        // before its start() marked this one superseded, and this body must
        // not end that request (#1430). Leaving it is safe because every way
        // this slot is cancelled also clears or replaces the request
        // (supersede_park_or_home_locked(), a newer start): keep it so, or a
        // request no body runs leaves Slewing true.
        if (!park_home_request_ || ctx.stop_reason() != util::StopReason::None) {
            return;  // superseded or cancelled, or carried out by an earlier body
        }
        const ParkHomeRequest request = *park_home_request_;
        run_park_or_home_locked(lock, ctx, request.who, request.home, request.stopped, request.code, request.claim);
        // Done, unless a newer start() superseded this body: the body it
        // started runs the request. A newer claim already replaced it.
        if (ctx.stop_reason() != util::StopReason::Superseded && park_home_request_ &&
            park_home_request_->claim == request.claim) {
            park_home_request_.reset();
        }
    }

    // The steps of the body; returns with `lock` held.
    void run_park_or_home_locked(StateLock& lock, util::OperationContext& ctx, const char* who, bool home, bool settle,
                                 int code, std::uint64_t claim) {
        const auto still_ours = [&] {
            return ctx.stop_reason() == util::StopReason::None && connected_ && park_home_claim_ == claim;
        };
        const auto wait = [&](std::chrono::milliseconds d) {
            lock.unlock();
            const bool woke = ctx.wait_for(d);
            lock.lock();
            return woke && still_ours();
        };
        try {
            if (!still_ours()) {
                return;
            }
            if (settle) {
                const auto settle_deadline = clock_.now() + kStopSettleTimeout;
                for (;;) {
                    refresh_status_locked();
                    if (!moving_locked() || clock_.now() >= settle_deadline) {
                        break;  // still moving at the deadline: the refusal reports it
                    }
                    if (!wait(kStopSettlePoll)) {
                        return;
                    }
                }
            }
            const auto deadline = clock_.now() + kInMotionRetryWindow;
            bool try_now = settle;
            for (;;) {
                if (!try_now) {
                    if (clock_.now() >= deadline) {
                        throw mount_refused(who, code);
                    }
                    if (!wait(kInMotionRetryPoll)) {
                        return;
                    }
                }
                try_now = false;
                const ParkHomeTry tried = try_park_or_home_locked(who, home, code);
                if (tried == ParkHomeTry::Taken) {
                    finish_park_or_home_locked(home);
                    return;
                }
                if (tried == ParkHomeTry::AlreadyParked) {
                    return;
                }
            }
        } catch (const AlpacaException& ex) {
            if (still_ours()) {
                park_home_error_ = KeptParkHomeError{home, ex.what(), ex.error_code()};
            }
        }
    }

    // A newer motion command or a reconnect: a Park or FindHome still
    // finishing ends without sending anything, and its kept refusal is
    // dropped. cancel() never blocks, so mutex_ may be held (rule 10).
    void supersede_park_or_home_locked() {
        park_home_error_.reset();
        ++park_home_claim_;
        park_home_request_.reset();
        park_home_.cancel();
    }

    // Raises a refusal the park_home_ body kept: any for Slewing, only a
    // park's for AtPark and a home's for AtHome.
    void raise_park_home_error_locked(std::optional<bool> home) const {
        if (park_home_error_ && (!home || *home == park_home_error_->home)) {
            throw AlpacaException(park_home_error_->message, park_home_error_->code);
        }
    }

    // The per-session state a disconnect or a lost link leaves behind.
    void reset_session_state_locked() {
        supersede_park_or_home_locked();
        snapshot_.reset();  // decision 0010: also drops a poll tick in flight
        link_health_.reset();
        set_link_fault_text("");
        refraction_from_firmware_.reset();
        {
            std::lock_guard<std::mutex> fwlock(firmware_mutex_);
            firmware_cache_.clear();
        }
        target_ra_set_ = false;
        target_dec_set_ = false;
        clear_manual_motion_locked();
        slew_hold_.clear();
        clear_pulse_guides_locked();
        utc_offset_.clear();  // decision 0014: the written UTCDate lives until the next connect or disconnect
        mount_clock_unreadable_.reset();
    }

    // open-astro#521: OnStepX keeps running a GOTO while the serial link is
    // out (on the HM-17 a GOTO pulled mid-slew stopped only when this sent
    // the stops, #1241), and a MoveAxis for up to about 10 s before the
    // firmware ends it by itself (#1098). On the connect that follows a
    // loss, an outage at least as long as the preserve window means nobody
    // was in control of that motion, so stop it with the same commands
    // AbortSlew sends. A shorter outage is a glitch the client rides out,
    // and no recorded loss (e.g. a fresh start) preserves motion: no stamp
    // is not evidence that anything ran unattended. The outage runs from the
    // last time the link was seen alive to the end of this reconnect (an
    // auto-detect re-scan counts): rounds toward stopping. Tracking is kept.
    void stop_motion_after_long_outage_locked() {
        auto& link = OnStepProtocolWrapper::instance();  // the loss stamps: no I/O
        const auto lost_at = link.consume_link_lost_at();
        const auto alive_at = link.consume_last_alive_before_loss();
        if (!lost_at) {
            return;
        }
        // Timed from the last moment the link was known to be there, not from
        // when the loss was noticed: the status poll checks the link once a
        // second, but the first to notice may be the reconnect itself (the
        // router reads Connected before every Connect), which would make a
        // long unattended outage look like 0 s.
        // The wrapper stamped both times on clock_ (set at connect).
        const auto outage = clock_.now() - (alive_at ? *alive_at : *lost_at);
        if (outage < detail::relink_motion_preserve_window()) {
            return;
        }
        char seconds[32];
        std::snprintf(seconds, sizeof(seconds), "%.1f", std::chrono::duration<double>(outage).count());
        // A park still running is left to finish (#1289 item 5): :Q# would
        // abort it and leave the mount unparked where it stopped (OnStepX
        // Park::requestAborted()). A status that cannot be read stops anyway.
        try {
            if (wire().get_status().is_parking) {
                ALPACA_LOG_INFO("OnStep", std::string("Serial link was last seen alive ") + seconds +
                                              " s ago; the mount is parking, so the park is left to finish");
                return;
            }
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
            // unknown state: stop, as before
        }
        ALPACA_LOG_WARN("OnStep", std::string("Serial link was last seen alive ") + seconds +
                                      " s ago; stopping any slew or MoveAxis that ran unattended (open-astro#521)");
        // Each stop on its own, as AbortSlew does (#781): a failed :Q# must
        // not skip the per-axis stops that end a MoveAxis.
        (void)send_stops_locked(true, {0, 1, 2, 3}, [](const char* what, const std::string& error) {
            ALPACA_LOG_ERROR("OnStep", std::string("Stop after a long outage failed (") + what + "): " + error);
        });
    }

    // ITelescopeV4: an equatorial slew with Tracking false is InvalidOperation
    // (open-astro#1274). Runs after the parked check and argument validation and
    // before any target or GOTO command is sent, and reads Tracking as the
    // getter does, from the polled frame (a control-path :GU# read only while
    // the frame has no status), so a Tracking=false just written counts as off.
    void check_equatorial_slew_locked(double ra, double dec, const char* who) const {
        util::validate_ra_dec(ra, dec, who);  // arguments first, even while disconnected (#1478)
        check_connected();
        check_not_parked_locked(who);
        refuse_while_faulted_locked(who);  // named, before the Tracking read (#1414)
        if (!tracking_as_served_locked()) {
            throw AlpacaException(std::string(who) + " requires Tracking to be true", AlpacaError::InvalidOperation);
        }
    }

    // (#1377). Same order as the equatorial check: arguments, connected, parked,
    // fault, then Tracking read as the getter reads it, before anything is sent.
    void check_alt_az_slew_locked(double altitude, double azimuth, double& ra_out, double& dec_out,
                                  const char* who) const {
        validate_alt_az(altitude, azimuth);  // arguments first, even while disconnected (#1478)
        check_connected();
        check_not_parked_locked(who);
        refuse_while_faulted_locked(who);  // named, before the site and Tracking reads (#1414)
        compute_alt_az_target_locked(altitude, azimuth, ra_out, dec_out);
        if (tracking_as_served_locked()) {
            throw AlpacaException(std::string(who) + " is not allowed while Tracking is true",
                                  AlpacaError::InvalidOperation);
        }
    }

    // A GOTO sent while one runs makes OnStepX stop the running one and refuse the
    // new one (Goto.cpp request(): CE_SLEW_IN_SLEW -> abort(), :MS# reply 5), so the
    // driver refuses first and leaves it running (#1379): its own GOTO (also in the
    // grace before :GU# shows it), a park or home already moving, or a handset GOTO.
    // A home with home sensors runs as a guide move, not a GOTO (:GU# shows 'h',
    // not the missing 'N'): it is refused too, or the slew would end the waiting
    // FindHome and the mount would answer "already moving" (reply 8).
    // It reads :GU# itself, never the frame alone: a GOTO the frame has not seen
    // yet would be stopped by the :MS# (#1379).
    void refuse_while_goto_running_locked(const char* who) const {
        bool running = slew_hold_.active(clock_.now());
        if (!running) {
            refresh_status_locked();
            running = frame_status_locked().is_slewing;
        }
        if (running) {
            throw AlpacaException(std::string(who) + " is not allowed while a GOTO is running",
                                  AlpacaError::InvalidOperation);
        }
        if (frame_status_locked().is_homing) {
            throw AlpacaException(std::string(who) + " is not allowed while the mount is homing",
                                  AlpacaError::InvalidOperation);
        }
    }

    // The :MS# refusal digit (OnStepX Goto.command.cpp) as the error the client sees (#1379).
    static AlpacaException slew_refused(const char* who, const std::string& reply) {
        return digit_refused(who, RefusedCommand::Goto, reply.empty() ? '?' : reply[0], reply);
    }

    // The :MS# digits, also behind the E of a :CM# refusal (OnStep Command.ino).
    enum class RefusedCommand : std::uint8_t { Goto, Sync };  // :MS# and :CM#

    static AlpacaException digit_refused(const char* who, RefusedCommand refused, char code, const std::string& reply) {
        const bool is_goto = refused == RefusedCommand::Goto;
        const char* what = is_goto ? "GOTO" : "sync";
        const char* command = is_goto ? ":MS#" : ":CM#";
        const char* why = "an unknown reason";
        switch (code) {
            case '1':
                why = "the target is below the horizon limit";
                break;
            case '2':
                why = "the target is above the overhead limit";
                break;
            case '3':
                why = "the controller is in standby";
                break;
            case '4':
                why = "the mount is parked";
                break;
            case '5':
                why = is_goto ? "a GOTO was already running, and the mount stopped it" : "a GOTO is running";
                break;
            case '6':
                why = "the target is outside the limits";
                break;
            case '7':
                why = "a hardware fault";
                break;
            case '8':
                why = "the mount is already moving";
                break;
            case '9':
                why = "an unspecified error";
                break;
            default:
                break;
        }
        return AlpacaException(std::string(who) + ": the mount refused the " + what + ": " + why + " (" + command +
                                   " reply '" + reply + "')",
                               code == '4' ? AlpacaError::InvalidWhileParked : AlpacaError::InvalidOperation);
    }

    // Tracking as the getter serves it (the frame, a write-through included); a
    // frame with no status yet is filled by a control-path :GU# read first.
    bool tracking_as_served_locked() const {
        const auto reading = snapshot_.read();
        if (!reading || !reading->value.status_known) {
            refresh_status_locked();
        }
        return frame_status_locked().is_tracking;
    }

    void check_not_parked_locked(const char* operation) const {
        if (frame_status_locked().is_parked) {
            throw AlpacaException(std::string(operation) + " is not allowed while parked",
                                  AlpacaError::InvalidWhileParked);
        }
    }

    static void validate_alt_az(double altitude, double azimuth) {
        if (!std::isfinite(altitude) || altitude < -90.0 || altitude > 90.0) {
            throw AlpacaException("Altitude out of range", AlpacaError::InvalidValue);
        }
        if (!std::isfinite(azimuth) || azimuth < 0.0 || azimuth > 360.0) {
            throw AlpacaException("Azimuth out of range", AlpacaError::InvalidValue);
        }
    }

    void compute_alt_az_target_locked(double altitude, double azimuth, double& ra_out, double& dec_out) const {
        validate_alt_az(altitude, azimuth);
        ensure_site_info_cached_locked();
        const double lst = util::local_sidereal_hours(current_utc_time_locked(), site_longitude_cached_);
        alt_az_to_ra_dec(altitude, azimuth, site_latitude_cached_, lst, ra_out, dec_out);
    }

    void start_slew_locked(double ra, double dec, const char* context, TargetWrites targets) {
        util::validate_ra_dec(ra, dec, context);
        refuse_while_faulted_locked(context);
        refuse_while_goto_running_locked(context);  // before anything is cancelled or sent (#1379)
        supersede_park_or_home_locked();            // a newer motion command wins (#1430)
        const Wire protocol = wire();
        protocol.set_target_ra(ra);
        protocol.set_target_dec(dec);
        // Decision 0022 rule 2: armed before the :MS# goes out, cleared when it does not start the GOTO.
        slew_hold_.arm(clock_.now(), kSlewForceGrace);
        std::string reply;
        try {
            reply = protocol.slew_to_target();
        } catch (...) {
            slew_hold_.clear();
            throw;
        }
        if (reply.empty() || reply[0] != '0') {
            slew_hold_.clear();
            throw slew_refused(context, reply);
        }
        store_target_locked(ra, dec, targets);
        clear_manual_motion_locked();
        tracking_before_slew_ = frame_status_locked().is_tracking;
        // Write-through (decision 0010): Slewing reads true past the grace until a poll sampled after the :MS#
        // says otherwise; a tick that sampled before it is dropped.
        write_status_locked([](MountStatus& status) { status.is_slewing = true; });
    }

    // Slewing as the getter serves it, from the polled frame (decision 0010); the synchronous slew's wait reads
    // it after its own :GU# read, which is written through.
    bool get_slewing_locked() const {
        if (manual_axis_slewing_[0] || manual_axis_slewing_[1]) {
            // Slewing follows the mount once it has shown the move (#1378).
            throw_if_faulted_locked();  // a latched fault is reported, as by every read (#1414)
            const auto reading = snapshot_.read();
            if (!reading || !reading->value.status_known || !manual_move_ended_locked(reading->value.status)) {
                return true;  // keep reporting the move; an unread status is not a stop
            }
            clear_manual_motion_locked();
        }
        if (slew_hold_.active(clock_.now())) {
            throw_if_faulted_locked();  // the grace asks the mount nothing (decision 0009 rule 1)
            return true;
        }
        // Past the window the frame decides. Landing (a reading after the :MS# answer that shows no GOTO) has
        // cleared the window and written the status; a reading from before the answer is never published over
        // the dispatch's write-through (decision 0010 token).
        return served_status_locked().is_slewing;
    }

    // Uses and clears the restore flag that start_slew_locked() set: restore
    // tracking once if that slew started with tracking on and `status`, read
    // after its GOTO ended, reports it off. Every path that ends a coordinate
    // slew calls this (the status poll when it sees the GOTO end,
    // wait_for_slew_complete_locked(), abort_slew()), so the flag cannot
    // outlive its slew and fire at an unrelated one (#1230). Per the SynScan
    // tracking-restoration lesson, some LX200-family mounts stop tracking
    // during a GOTO; OnStepX keeps tracking through one (seen on the TeSeek
    // HM-17), so there this normally sends nothing. Park and FindHome clear the
    // flag: they end with tracking off on purpose, and restarting it moved the
    // mount off home (#1218). Every caller holds the operation lock, so no
    // command lands between the status read and the restore.
    //
    // A restore that fails is logged and given up, except a lost link, which
    // is reported (NotConnected): it is no longer swallowed (#1783).
    // `tracking_now` is what that read says; true is returned when the restore went out, and the caller writes
    // it through (the poll into the frame it is about to publish, which a write would drop).
    bool finish_slew_tracking_locked(bool tracking_now) const {
        const bool restore_tracking = tracking_before_slew_ && !tracking_now;
        tracking_before_slew_ = false;
        if (!restore_tracking) {
            return false;
        }
        try {
            wire().start_tracking();
        } catch (const std::exception& ex) {
            if (is_link_lost(ex)) {
                throw;
            }
            ALPACA_LOG_WARN("OnStep", std::string("Tracking restore after the GOTO failed: ") + ex.what());
            return false;
        }
        return true;
    }

    // finish_slew_tracking_locked() on the frame's status, written through.
    void finish_slew_tracking_from_frame_locked() const {
        if (finish_slew_tracking_locked(frame_status_locked().is_tracking)) {
            write_status_locked([](MountStatus& status) { status.is_tracking = true; });
        }
    }

    // Poll for a status condition, RELEASING the driver mutex around every
    // sleep so a synchronous Park/FindHome/SlewToCoordinates doesn't block
    // all GETs and Disconnect for the whole operation (Bisque/SynScan
    // unlock-sleep-relock pattern). `lock` must be held on entry and is held
    // again on return/throw.
    template <typename Predicate>
    void wait_for_condition_locked(StateLock& lock, Predicate done) const {
        const auto timeout = std::chrono::seconds(120);
        const auto start = clock_.now();
        while (!done()) {
            if (clock_.now() - start > timeout) {
                throw AlpacaException("Operation timed out waiting for mount status");
            }
            lock.unlock();
            clock_.sleep_for(std::chrono::milliseconds(250));
            lock.lock();
            check_connected();
            refresh_status_locked();
        }
    }

    void wait_for_slew_complete_locked(StateLock& lock) const {
        wait_for_condition_locked(lock, [this] { return !get_slewing_locked(); });
        // Decide the restore here; the status was just read (#1230).
        finish_slew_tracking_from_frame_locked();
        // The control path ends the motion: publish where the mount stopped, so the position read right after
        // Slewing goes false is not one the poll took during the GOTO (decision 0010).
        publish_measurement_locked();
        if (slew_settle_time_seconds_ > 0) {
            lock.unlock();
            clock_.sleep_for(std::chrono::seconds(slew_settle_time_seconds_));
            lock.lock();
            check_connected();
        }
    }

    // open-astro#1414, decision 0009: a mount that stops answering while its
    // adapter stays plugged in (mount unpowered, cable pulled at the mount)
    // latches a fault after kLinkFaultThreshold failed poll ticks (or
    // control-path status reads) in a row: Connected stays true, and reads
    // throw DriverException with the fault instead of serving the frame. A
    // fault that has stood for util::kLinkStalenessBound is a lost link: the
    // wrapper records it and closes the port as for a pulled adapter (#1241),
    // so Connected reads false and calls throw NotConnected. Only the client's
    // Connect relinks. The poll keeps asking while faulted, so one good tick
    // clears the fault, and its failed ticks find the bound (a getter, which
    // takes no operation lock, only throws the fault). Time is the task clock.
    void note_read_ok_locked() const {
        if (link_health_.on_reply()) {
            ALPACA_LOG_INFO("OnStep", "Mount answering again; link fault cleared");
            set_link_fault_text("");
        }
    }

    void note_read_failure_locked(const std::exception& ex) const {
        if (is_link_lost(ex)) {
            return;  // the link is already gone (#1241); not a silent mount
        }
        const auto now = clock_.now();
        if (const auto latched = link_health_.note_failure(ex.what(), kLinkFaultThreshold, now)) {
            ALPACA_LOG_ERROR("OnStep", "Mount not answering; link fault latched: " + *latched);
            set_link_fault_text(*latched);
        } else if (!link_health_.faulted()) {
            ALPACA_LOG_WARN("OnStep", "Mount read failed (" + std::to_string(link_health_.consecutive_failures()) +
                                          " in a row): " + ex.what());
        }
        lose_link_if_stale_locked(now);
        if (link_health_.faulted()) {
            throw_fault_locked();
        }
    }

    // A read that answers from the frame or the driver's own state (the
    // position and status getters, the site, UTCDate, SiderealTime,
    // DestinationSideOfPier, DoesRefraction, IsPulseGuiding and Slewing's
    // pending Park/FindHome and grace answers) throws the latched fault
    // (decision 0009 rule 1). A caller that holds the operation lock (a
    // command, the poll) checks the staleness bound first: closing a lost
    // port is a wire call, which a getter never makes.
    void throw_if_faulted_locked() const {
        if (link_health_.faulted()) {
            if (holds_io_lock()) {
                lose_link_if_stale_locked(clock_.now());
            }
            throw_fault_locked();
        }
    }

    // The latched fault as every faulted read reports it (decision 0009 rule 1),
    // one text for all of them (#1658 review).
    [[noreturn]] void throw_fault_locked() const {
        throw AlpacaException("OnStep mount communications compromised: " + link_health_.fault(),
                              AlpacaError::DriverException);
    }

    // A read that found the link gone.
    static bool is_link_lost(const std::exception& ex) {
        const auto* alpaca = dynamic_cast<const AlpacaException*>(&ex);
        return alpaca != nullptr && alpaca->error_code() == AlpacaError::NotConnected;
    }

    // A fault that has stood past util::kLinkStalenessBound is a lost link:
    // the wrapper closes the port and this throws NotConnected. Caller holds
    // the operation lock.
    void lose_link_if_stale_locked(std::chrono::steady_clock::time_point now) const {
        if (!link_health_.fault_stale(now, util::kLinkStalenessBound)) {
            return;
        }
        const std::string reason =
            "no reply for " +
            std::to_string(std::chrono::duration_cast<std::chrono::seconds>(util::kLinkStalenessBound).count()) +
            " s: " + link_health_.fault();
        wire().declare_link_lost(reason);  // Connected now reads false
        throw AlpacaException("OnStep link lost (" + reason + "); set Connected true to reconnect",
                              AlpacaError::NotConnected);
    }

    // Decision 0009 rule 1: while the fault is latched, writes throw as reads
    // do. Blind commands cannot fail on a healthy fd, so without this a
    // Tracking, Unpark or a slew would report success to a mount that is
    // not listening. AbortSlew and MoveAxis(axis, 0) send their stops first
    // and call this after, so the stop is still tried (#1414).
    void refuse_while_faulted_locked(const char* what) const {
        if (link_health_.faulted()) {
            // A client that only writes still sees the link lost once the
            // fault has stood past the bound, as a read would (#1414).
            lose_link_if_stale_locked(clock_.now());
            throw AlpacaException(
                std::string(what) + ": OnStep mount communications compromised: " + link_health_.fault(),
                AlpacaError::DriverException);
        }
    }

    void set_link_fault_text(const std::string& text) const {
        std::lock_guard<std::mutex> fault_lock(link_fault_mutex_);
        link_fault_text_ = text;
    }

    // ── Snapshot-served state (decision 0010, OnStep adoption, #1783) ──────────

    // The frame's status, for the control paths: default (idle, not parked) before any :GU# answered. Takes only
    // the snapshot's own mutex.
    MountStatus frame_status_locked() const {
        const auto reading = snapshot_.read();
        return reading && reading->value.status_known ? reading->value.status : MountStatus{};
    }

    // Write-through of a status the driver now knows (a command's effect). The write drops a poll tick in flight
    // (decision 0010 point 4) and leaves measured_at alone. With no frame held it is not kept: the next tick
    // measures the mount.
    template <typename Apply>
    void write_status_locked(Apply&& apply) const {
        snapshot_.write([&apply](PolledFrame& frame) { apply(frame.status); });
    }

    // A write that changed what the mount's position means (a pulse, a MoveAxis start, the stops): the tick in
    // flight sampled before it and is dropped; the next tick measures.
    void drop_poll_in_flight_locked() const {
        snapshot_.write([](PolledFrame&) {});
    }

    // A :GU# a control path read (decision 0010: control paths keep their direct reads and write through). A
    // status equal to the frame's is not written, so a control loop that re-reads :GU# (the synchronous slew,
    // the Park/FindHome settle) does not drop every poll tick and freeze the position. `counts_for_move`: a read
    // after the last MoveAxis start, which may end it (#1378).
    void note_status_read_locked(const MountStatus& status, bool counts_for_move) const {
        if (counts_for_move) {
            note_status_measured_locked(status);
        }
        const auto held = snapshot_.read();
        if (held && held->value.status_known && same_status(held->value.status, status)) {
            return;
        }
        snapshot_.write([&status](PolledFrame& frame) {
            frame.status = status;
            frame.status_known = true;
        });
    }

    // A :GU# measured after the last MoveAxis start reached the frame (#1378).
    void note_status_measured_locked(const MountStatus& status) const {
        status_read_since_move_ = true;
        if (status.is_guide_active && !status.is_pulse_guide_active) {
            firmware_reports_guide_move_ = true;
        }
    }

    // Control path: :GU# now, written through. A failed read counts toward the
    // link fault (and throws once it latched, or the link went); while the frame
    // knows a status the failure is otherwise not reported, as the status cache
    // was served before #1783 (#1414). Caller holds the operation lock.
    void refresh_status_locked() const {
        try {
            const MountStatus status = wire().get_status();
            note_read_ok_locked();
            if (!status.is_slewing) {
                slew_hold_.clear();  // landed: a reading with no GOTO (decision 0022 rules 2, 3)
            }
            note_status_read_locked(status, true);
        } catch (const std::exception& ex) {
            note_read_failure_locked(ex);
            const auto reading = snapshot_.read();
            if (!reading || !reading->value.status_known || is_link_lost(ex)) {
                throw;
            }
        }
    }

    // The frame a getter serves, or the refusal that applies: NotConnected (the caller checked), a latched fault
    // (DriverException, Connected stays true), or no frame yet. A frame past kSnapshotMaxAge is served as it
    // is: stale is not lost (decision 0010 rule 6).
    PolledFrame served_frame_locked() const {
        throw_if_faulted_locked();
        const auto reading = snapshot_.read();
        if (!reading) {
            throw AlpacaException("OnStep mount state is not available yet: the first status poll has not answered",
                                  AlpacaError::DriverException);
        }
        return reading->value;
    }

    MountStatus served_status_locked() const {
        const PolledFrame frame = served_frame_locked();
        if (!frame.status_known) {
            throw AlpacaException("OnStep mount status is not available: the mount has not answered :GU#",
                                  AlpacaError::DriverException);
        }
        return frame.status;
    }

    Position served_position_locked() const {
        const PolledFrame frame = served_frame_locked();
        if (!frame.equatorial_known) {
            throw AlpacaException("OnStep mount position is not available: the mount has not answered :GRa#/:GDe#",
                                  AlpacaError::DriverException);
        }
        return Position{frame.ra_hours, frame.dec_degrees};
    }

    AltAz served_altaz_locked() const {
        const PolledFrame frame = served_frame_locked();
        if (!frame.altaz_known) {
            throw AlpacaException("OnStep mount Alt/Az is not available: the mount has not answered :GA#/:GZ#",
                                  AlpacaError::DriverException);
        }
        return AltAz{frame.alt_degrees, frame.az_degrees};
    }

    // Decision 0010 rule 5: DeviceState's TimeStamp is when the poll measured. Task-clock age; takes only the
    // snapshot's own mutex.
    std::optional<std::chrono::nanoseconds> device_state_age() const override {
        const auto reading = snapshot_.read();
        if (!reading) {
            return std::nullopt;
        }
        return clock_.now() - reading->measured_at;
    }

    // One mount sample: :GU# first, so a frame that says "not slewing" carries a position read after the stop,
    // then :GRa#/:GDe#, and with `full` :GA#/:GZ#. Throws at the first failed exchange of the mandatory part
    // (a tick on a dead link waits out one timeout); a failed Alt/Az read keeps the previous value. Caller holds
    // the operation lock.
    PolledFrame sample_locked(const PolledFrame& previous, bool full) const {
        const Wire protocol = wire();
        PolledFrame frame = previous;
        frame.sampled_at = clock_.now();
        frame.sample_seq = ++sample_seq_;
        frame.status = protocol.get_status();
        frame.status_known = true;
        const Position position = protocol.get_position();
        frame.ra_hours = position.ra_hours;
        frame.dec_degrees = position.dec_degrees;
        frame.equatorial_known = true;
        if (full) {
            try {
                const AltAz altaz = protocol.get_alt_az();
                frame.alt_degrees = altaz.altitude_degrees;
                frame.az_degrees = altaz.azimuth_degrees;
                frame.altaz_known = true;
            } catch (const std::exception& ex) {
                if (is_link_lost(ex)) {
                    throw;
                }
                ALPACA_LOG_WARN("OnStep", std::string("Alt/Az read failed; the last value is kept: ") + ex.what());
            }
        }
        return frame;
    }

    // Where a control path ends a motion (the synchronous slew): reads the mount now and publishes a full frame,
    // so Slewing false is never served beside a position from mid-motion. A failed read is handled as a failed
    // poll tick: the last values stay served and the failure counts toward the link fault; only a lost link is
    // reported. Caller holds the operation lock.
    void publish_measurement_locked() const {
        const auto token = snapshot_.begin_poll();
        const auto previous = snapshot_.read();
        try {
            const PolledFrame frame = sample_locked(previous ? previous->value : PolledFrame{}, true);
            note_read_ok_locked();
            if (publish_sample_locked(token, frame)) {
                note_status_measured_locked(frame.status);
            }
        } catch (const std::exception& ex) {
            if (is_link_lost(ex)) {
                throw;
            }
            try {
                note_read_failure_locked(ex);
            } catch (const AlpacaException& counted) {
                if (counted.error_code() == AlpacaError::NotConnected) {
                    throw;  // the fault stood past the bound: the link is lost
                }
            }
        }
    }

    // The first frame, at the end of a connect, so the getters have an answer, status flags included, the
    // moment Connect returns. Each read on its own, as the cache warm-up did: a mount that answers none leaves a
    // frame with nothing known (the getters refuse it until a tick answers), and each failure counts toward the
    // link fault. The mount's site is read here too while the config did not set it.
    void seed_snapshot_locked() {
        const Wire protocol = wire();
        const auto token = snapshot_.begin_poll();
        PolledFrame frame;
        frame.sampled_at = clock_.now();
        frame.sample_seq = ++sample_seq_;
        const auto read = [this](const std::function<void()>& exchange) {
            try {
                exchange();
                note_read_ok_locked();
            } catch (const std::exception& ex) {
                try {
                    note_read_failure_locked(ex);
                } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): the connect still succeeds
                }
            }
        };
        read([&] {
            frame.status = protocol.get_status();
            frame.status_known = true;
        });
        read([&] {
            const Position position = protocol.get_position();
            frame.ra_hours = position.ra_hours;
            frame.dec_degrees = position.dec_degrees;
            frame.equatorial_known = true;
        });
        try {
            ensure_site_info_cached_locked();
        } catch (...) {  // NOLINT(bugprone-empty-catch): the connect still succeeds; the poll retries it
        }
        read([&] {
            const AltAz altaz = protocol.get_alt_az();
            frame.alt_degrees = altaz.altitude_degrees;
            frame.az_degrees = altaz.azimuth_degrees;
            frame.altaz_known = true;
        });
        snapshot_.publish(token, frame);
        if (frame.status_known) {
            note_status_measured_locked(frame.status);
        }
    }

    // One poll tick. Its exchanges hold the operation lock, like a command's, so they never land inside a
    // command's sequence (:hC# then :GE#), and mutex_ is dropped for each one (decision 0016); the frame is
    // published under mutex_ alone, so a client write between the sample and the publish drops it (decision
    // 0010 point 4). The tick ends at its first failed exchange. Returns false when the connection is gone or
    // lost and the body should end. `ctx` is null for the test seam's direct tick.
    bool poll_status_once(util::OperationContext* ctx, int tick) {
        const auto stopped = [ctx] { return ctx != nullptr && ctx->stop_reason() != util::StopReason::None; };
        const bool full = tick % kSlowPollEvery == 0;
        util::StateSnapshot<PolledFrame>::Token token{};
        PolledFrame frame;
        {
            StateLock lock(lock_gate_);
            if (!connected_.load() || !published_connected_.load() || stopped() ||
                !OnStepProtocolWrapper::instance().link_alive()) {
                return false;
            }
            const auto previous = snapshot_.read();
            token = snapshot_.begin_poll();
            try {
                frame = sample_locked(previous ? previous->value : PolledFrame{}, full);
                if (stopped()) {
                    return false;  // superseded or cancelled mid-sample: no health note, restore or site read
                }
                note_read_ok_locked();
            } catch (const std::exception& ex) {
                if (stopped()) {
                    return false;
                }
                try {
                    note_read_failure_locked(ex);
                } catch (const AlpacaException& thrown) {
                    if (thrown.error_code() == AlpacaError::NotConnected) {
                        return false;  // the bound has passed: the link is lost, and only a Connect relinks
                    }
                }
                return !is_link_lost(ex);
            }
            try {
                finish_slew_if_ended_locked(frame);
                if (full && !site_known_locked()) {
                    ensure_site_info_cached_locked();  // a site the connect could not read (#1783)
                }
            } catch (const std::exception& ex) {
                if (is_link_lost(ex)) {
                    return false;
                }
            }
        }
        run_status_poll_after_sample_hook();
        StateLock lock(lock_gate_, StateLock::Mode::State);
        if (!connected_.load() || stopped()) {
            return false;
        }
        if (publish_sample_locked(token, frame)) {
            note_status_measured_locked(frame.status);
        }
        return true;
    }

    // The regular tick, the pulse-end tick and the synchronous slew's end read all publish under mutex_, the ticks
    // after the operation lock is released, so they can publish out of the order they sampled in. A sample older
    // than the held frame's is skipped; a newer one in flight is left to publish (decision 0010: a client write
    // after its begin_poll() still drops it).
    bool publish_sample_locked(util::StateSnapshot<PolledFrame>::Token token, const PolledFrame& frame) const {
        const auto held = snapshot_.read();
        if (held && frame.sample_seq < held->value.sample_seq) {
            return false;
        }
        return snapshot_.publish(token, frame);
    }

    // The tracking restore a coordinate slew owes (#1230), on the first tick frame that shows no GOTO (inside the
    // hold window too, decision 0022 rule 3): before #1783 a Slewing read sent it, as a single read that waited for the
    // operation lock. It is not decided during a MoveAxis (a 4.x mount refuses :ST mid-move), nor while a Park or
    // FindHome is pending: one refused "in motion" and retried in the park_home_ body ends with tracking off, so a
    // restore sent between its tries would only be undone (#1743 review). The tick holds the operation lock, so no
    // command lands between its :GU# and the restore. A frame that shows no GOTO ends the slew; the hold window is not
    // read (decision 0022 rule 3). A frame cannot predate the :MS# answer: the tick holds the operation lock that :MS#
    // is sent under, and the dispatch write-through shows the GOTO until a later frame replaces it.
    void finish_slew_if_ended_locked(PolledFrame& frame) const {
        if (frame.status.is_slewing) {
            return;
        }
        slew_hold_.clear();  // landed: a frame with no GOTO (decision 0022 rules 2, 3)
        if (!tracking_before_slew_ || manual_axis_slewing_[0] || manual_axis_slewing_[1] || park_home_request_) {
            return;
        }
        if (finish_slew_tracking_locked(frame.status.is_tracking)) {
            frame.status.is_tracking = true;
        }
    }

    void run_status_poller(util::OperationContext& ctx, std::chrono::nanoseconds first_wait) {
        if (!ctx.wait_for(first_wait)) {
            return;
        }
        for (int tick = 0;; ++tick) {
            const auto started = clock_.now();
            if (!poll_status_once(&ctx, tick)) {
                return;
            }
            // Start to start: wait for the next tick, skipping any a slow tick ran over (no catch-up).
            const auto spent = clock_.now() - started;
            const auto ticks = spent / kStatusPollPeriod + 1;
            if (!ctx.wait_for(ticks * kStatusPollPeriod - spent)) {
                return;
            }
        }
    }

    // Call with no driver lock held (decision 0006 rule 4): the poll and pulse-end bodies take them.
    void stop_status_poller() {
        status_poll_.cancel_all_and_join();
        pulse_measurement_.cancel_all_and_join();
    }

    // One tick `wait` from now, the pulse's end plus kPulseGuideSettle, beside the regular cadence. Call with no
    // driver lock held (decision 0006 rule 4).
    void start_pulse_measurement(std::chrono::nanoseconds wait) {
        if (status_poll_paused_for_testing_.load()) {
            return;
        }
        try {
            pulse_measurement_.start([this, wait](util::OperationContext& ctx) {
                if (ctx.wait_for(wait)) {
                    (void)poll_status_once(&ctx, 1);  // :GU# and the position
                }
            });
        } catch (const std::exception& e) {
            // A burst of PulseGuide calls can cause this (decision 0007 rule 9: INFO); IsPulseGuiding still ends at
            // kPulseMeasureBound and the 1 s ticks go on.
            ALPACA_LOG_INFO("OnStep", std::string("Pulse-end measurement did not start: ") + e.what());
        }
    }

    // Call with no driver lock held (decision 0006 rule 4), at the end of a connect.
    void start_status_poller(std::chrono::nanoseconds first_wait) {
        if (status_poll_paused_for_testing_.load()) {
            return;
        }
        try {
            status_poll_.start([this, first_wait](util::OperationContext& ctx) { run_status_poller(ctx, first_wait); });
        } catch (const std::exception& e) {
            ALPACA_LOG_ERROR("OnStep", std::string("Status poll did not start: ") + e.what());
        }
    }

    // ── Site ───────────────────────────────────────────────────────────────────

    bool site_known_locked() const { return site_latitude_known_ && site_longitude_known_; }

    std::string site_unknown_message(const char* member) const {
        return std::string(member) + " is not known: the mount's site could not be read" +
               (site_read_error_.empty() ? std::string() : " (" + site_read_error_ + ")") +
               "; write it, or set the site in the device config";
    }

    // Control path (a connect, a site write, the poll): reads :Gt#/:Gg# while
    // the pair is not known, at most every kSiteInfoRetryDelay. Once known it
    // is never read again: the site writes set it themselves. Caller holds the
    // operation lock.
    void ensure_site_info_cached_locked() const {
        if (!connected_) {
            return;
        }
        // While the link fault is latched every read throws it (decision 0009
        // rule 1), and an unread site is not fetched: that fetch would wait out
        // the reply timeout and swallow the failure (#1659).
        throw_if_faulted_locked();
        if (site_known_locked()) {
            return;
        }
        auto now = clock_.now();
        if (last_site_info_attempt_ != std::chrono::steady_clock::time_point::min() &&
            (now - last_site_info_attempt_) < kSiteInfoRetryDelay) {
            return;
        }
        last_site_info_attempt_ = now;
        try {
            SiteInfo info = wire().get_site_info();
            site_latitude_cached_ = info.latitude_degrees;
            site_longitude_cached_ = info.longitude_degrees;
            site_latitude_known_ = true;
            site_longitude_known_ = true;
            site_read_error_.clear();
        } catch (const AlpacaException& ex) {
            if (ex.error_code() == AlpacaError::NotConnected) {
                throw;  // the link is lost: reported, and the state is left alone
            }
            note_site_read_failed_locked(ex.what());
        } catch (const std::exception& ex) {
            note_site_read_failed_locked(ex.what());
        } catch (...) {
            note_site_read_failed_locked("unknown error");
        }
    }

    // A failed :Gt#/:Gg# read is retried by the poll (after
    // kSiteInfoRetryDelay); it is logged once per connect (#1754). Its reason
    // is kept for the site getters' refusal.
    void note_site_read_failed_locked(const char* reason) const {
        site_read_error_ = reason;
        if (!site_read_warned_) {
            site_read_warned_ = true;
            ALPACA_LOG_WARN("OnStep", std::string("Could not read the site from the mount: ") + reason);
        }
    }

    // Body of set_utc_date with the driver locks already held — called from the
    // connect path (sync_time_on_connect_), which holds them; calling the
    // public locking method there would self-deadlock (non-recursive mutex).
    void set_utc_date_locked(std::chrono::system_clock::time_point utc) {
        check_connected();
        std::time_t utc_time_t = std::chrono::system_clock::to_time_t(utc);

        // Send UTC itself as the mount's "local" time with a 0 offset,
        // rather than converting to the host's local time/DST — confirmed
        // against real hardware: with a non-zero offset, the mount's
        // internal sidereal-time (:GS#) calculation came out ~85 minutes
        // wrong (not a clean ±1h double-application of the offset either,
        // so this is a genuine firmware quirk, not just a sign error like
        // the longitude one above). Skipping the offset entirely — long
        // established practice for LX200-family mounts precisely to dodge
        // per-firmware timezone/DST bugs like this — made :GS# match the
        // correct GMST-based value to within a second.
        std::tm utc_tm{};
#ifdef _WIN32
        gmtime_s(&utc_tm, &utc_time_t);
#else
        utc_tm = *std::gmtime(&utc_time_t);
#endif
        TimeInfo info;
        info.hour = utc_tm.tm_hour;
        info.minute = utc_tm.tm_min;
        info.second = utc_tm.tm_sec;
        info.month = utc_tm.tm_mon + 1;
        info.day = utc_tm.tm_mday;
        info.year = (utc_tm.tm_year + 1900) % 100;
        info.utc_offset_hours = 0.0;

        const auto client_minus_host = utc - std::chrono::system_clock::now();  // before the write (#409)
        wire().set_time(info);
        utc_offset_.set(utc, clock_.now());
        mount_clock_unreadable_.reset();
        // The mount now runs on the client's clock and so does the LST above;
        // on a disciplined host say so once (open-astro#409).
        alpacacore::util::ClientUtcWarning::warn_once("OnStep", client_minus_host, client_disagreement_warned_);
    }

    // The UTC the LST computations use: a written or mount-read UTCDate, else
    // the host clock. The host clock is not stored in utc_offset_, so UTCDate
    // never reports it as the mount's (#1478).
    std::chrono::system_clock::time_point current_utc_time_locked() const {
        if (const auto utc = utc_offset_.current(clock_.now())) {
            return *utc;
        }
        return std::chrono::system_clock::now();
    }

    // Decision 0005: every wait, sleep and deadline of this driver (#1378),
    // and the wrapper's link-loss stamps (set_task_clock() at connect).
    util::TaskClock& clock_;
    int device_number_;
    ConnectionInfo connection_info_;
    // Set by the auto-detect factory; empty for an explicit port or host.
    // connection_resolved_ is true once a connect has run the resolver, so a
    // later connect retries that endpoint before scanning again (#659).
    util::ConnectionResolver<ConnectionInfo> connection_resolver_;
    bool connection_resolved_ = false;
    // Decision 0016: see the lock-order comment at the top of the class. Both are mutable: a const helper
    // that reaches the wire (Wire::call) needs the operation lock already held and drops and retakes mutex_.
    // set_connected() against set_connected(); no getter takes it. Under it, set_connected() joins the
    // status-poll and pulse-end slots (stop_status_poller(), on a disconnect and a relink) and only cancels
    // the others (cancel() never blocks). No slot body takes it or calls set_connected(), so the join ends.
    std::mutex transition_mutex_;
    mutable std::mutex io_mutex_;
    mutable std::mutex mutex_;
    // Which of the two this thread holds, and the exchanges that drop mutex_ (#1729).
    util::LockGate lock_gate_{io_mutex_, mutex_, "OnStep"};
    // Read lock-free by check_connected(); written under io_mutex_ and mutex_.
    std::atomic<bool> connected_;
    // What get_connected() reports, read lock-free (#910). Raised only once a
    // connect has finished, so Connected never reads true while Connecting
    // does; dropped before any disconnect. Written under io_mutex_ and mutex_.
    std::atomic<bool> published_connected_{false};
    bool client_disagreement_warned_ = false;  // open-astro#409, re-armed on connect

    double target_ra_hours_ = 0.0;
    double target_dec_degrees_ = 0.0;
    // open-astro#346 (the shape #304 fixed on the Sky-Watcher driver): ASCOM
    // treats the two target properties as independent, so each must throw
    // ValueNotSet until that property itself has been written. One shared flag
    // let a write to either unlock both, and a client reading the one it did
    // not set got a default 0 instead of an error. The paths that legitimately
    // define both coordinates at once -- the slew and sync coordinate forms,
    // the position-override and arrival reads, and the connect/disconnect
    // resets -- still set or clear both.
    mutable bool target_ra_set_ = false;
    mutable bool target_dec_set_ = false;
    // Bumped by each target write (under mutex_); see store_target_locked().
    std::uint64_t target_ra_writes_ = 0;
    std::uint64_t target_dec_writes_ = 0;

    double aperture_diameter_m_ = 0.0;
    double aperture_area_m2_ = 0.0;
    double focal_length_m_ = 0.0;

    // Decision 0010 (#1783): the polled frame the getters and DeviceState serve. One publisher (the poll body,
    // plus the synchronous slew's end), any number of readers; a leaf lock under mutex_. Declared before the
    // poll slot below, so the slot (whose body publishes into it) is destroyed first.
    mutable util::StateSnapshot<PolledFrame> snapshot_{clock_, kSnapshotMaxAge};

    // The site, each coordinate known once the config, a client write or a :Gt#/:Gg# read set it (#1783); the
    // getters throw until then. Under mutex_.
    mutable double site_latitude_cached_ = 0.0;
    mutable double site_longitude_cached_ = 0.0;
    mutable bool site_latitude_known_ = false;
    mutable bool site_longitude_known_ = false;
    mutable std::string site_read_error_;    // why the last :Gt#/:Gg# read failed, for the getters' refusal
    mutable bool site_read_warned_ = false;  // a failed site read was logged (#1754); under mutex_
    mutable std::chrono::steady_clock::time_point last_site_info_attempt_;
    std::atomic<double> site_elevation_m_;  // NaN until set or configured

    // Decision 0014: the UTCDate a client wrote, or the mount clock the connect
    // read (seed_utc_from_mount_locked()), and the task-clock time since.
    // Cleared on connect and disconnect. Under mutex_.
    mutable util::UtcOffset utc_offset_;
    // Why the connect could not read the mount clock (#1478); under mutex_,
    // cleared on connect, disconnect and a UTCDate write.
    std::optional<std::string> mount_clock_unreadable_;

    // Decision 0022: Slewing reads true while it is active, kSlewForceGrace from the :MS#. Under mutex_.
    mutable util::HoldWindow slew_hold_;
    mutable bool manual_axis_slewing_[2] = {false, false};
    // The running MoveAxis per axis, for the re-send (#1378). Under mutex_.
    int manual_axis_dir_[2] = {0, 0};
    double manual_axis_rate_[2] = {0.0, 0.0};
    mutable bool manual_axis_resend_[2] = {false, false};
    std::chrono::steady_clock::time_point manual_axis_resend_due_[2]{};
    // :GU# has shown 'g' since this connect, so its absence ends a MoveAxis (#1378).
    mutable bool firmware_reports_guide_move_ = false;
    // A real :GU# read has landed since the last MoveAxis start (#1378).
    mutable bool status_read_since_move_ = false;
    // Set only by a coordinate slew; consumed when that slew completes (#1218).
    mutable bool tracking_before_slew_ = false;

    bool does_refraction_ = false;
    // From :GXEE# at connect on OnStepX; empty when unknown or disconnected.
    std::optional<bool> refraction_from_firmware_;
    int slew_settle_time_seconds_ = 0;

    std::optional<double> pending_site_latitude_;
    std::optional<double> pending_site_longitude_;
    std::optional<double> pending_site_elevation_;
    bool sync_time_on_connect_;

    // Per axis (0 East/West, 1 North/South): a pulse is pending, and its hold window for the pulse's duration
    // (decision 0022). IsPulseGuiding is true while either axis's pulse runs or is not yet measured. Under mutex_.
    mutable bool pulse_guiding_active_[2] = {false, false};
    mutable std::array<util::HoldWindow, 2> pulse_hold_;
    // Numbers each sample (PolledFrame::sample_seq); under mutex_, taken with the operation lock held.
    mutable std::uint64_t sample_seq_ = 0;

    // Decision 0009 fault latch for a silent mount (#1414). Under mutex_.
    mutable util::PolledLinkHealth link_health_;
    // Its text for get_link_fault(), which takes no mutex_: own narrow mutex.
    mutable std::mutex link_fault_mutex_;
    mutable std::string link_fault_text_;

    // Web-UI firmware copy, guarded by its own narrow mutex (not mutex_) so
    // the get_device_firmware() poll never blocks on the coarse connect lock.
    mutable std::mutex firmware_mutex_;
    std::string firmware_cache_;

    // Last, so it is destroyed (and its body joined) before every member the
    // body touches. The destructor joins it explicitly as well.
    util::OperationGeneration move_axis_generation_;
    mutable util::AsyncOperation move_axis_resend_{"OnStep MoveAxis re-send", move_axis_generation_, clock_};

    // #1430: a refusal the park_home_ body met, raised by Slewing and by
    // AtPark (a park's) or AtHome (a home's). Under mutex_.
    struct KeptParkHomeError {
        bool home;
        std::string message;
        int code;
    };
    std::optional<KeptParkHomeError> park_home_error_;
    // The current Park/FindHome's claim (decision 0006 rule 3), bumped by
    // every supersede, and the request its background part is still to
    // finish. Under mutex_.
    struct ParkHomeRequest {
        std::uint64_t claim;
        const char* who;
        bool home;
        bool stopped;
        int code;
    };
    std::uint64_t park_home_claim_ = 0;
    std::optional<ParkHomeRequest> park_home_request_;
    // Test seam: runs between the unlock and park_home_.start(). Set only
    // while no Park or FindHome is in flight.
    std::function<void()> park_home_before_start_;
    // Test seams: run before a clean disconnect's close, and in a poll tick between its sample and its
    // publish (a leaf lock of their own guards both).
    mutable std::mutex disconnect_hook_mutex_;
    std::function<void()> disconnect_before_close_hook_;
    std::function<void()> status_poll_after_sample_hook_;
    std::function<void()> pulse_guide_gap_hook_;
    std::function<bool()> move_axis_resend_before_send_hook_;
    // The tail of a Park or FindHome that must first wait for motion to end
    // (#1430). Declared last with the re-send slot, for the same reason.
    util::OperationGeneration park_home_generation_;
    mutable util::AsyncOperation park_home_{"OnStep Park/FindHome", park_home_generation_, clock_};
    // Decision 0010 (#1783): the status poll, one body per connection, started at the end of a connect (and by
    // the test pause seam, never after a pulse guide) and joined before a disconnect or relink closes the port.
    // Declared last with the other slots, after every member its body touches.
    std::atomic<bool> status_poll_paused_for_testing_{false};
    util::OperationGeneration status_poll_generation_;
    mutable util::AsyncOperation status_poll_{"OnStep status poll", status_poll_generation_, clock_};
    // The extra tick at the end of a pulse guide (#1783); a newer pulse replaces it.
    util::OperationGeneration pulse_measurement_generation_;
    mutable util::AsyncOperation pulse_measurement_{"OnStep pulse-end measurement", pulse_measurement_generation_,
                                                    clock_};
};

std::unique_ptr<TelescopeDriver> create_onstep_telescope(int device_number, const ConnectionInfo& connection_info,
                                                         util::TaskClock& clock) {
    return create_onstep_telescope_with_site(device_number, connection_info, std::nullopt, std::nullopt, std::nullopt,
                                             std::nullopt, clock);
}

std::unique_ptr<TelescopeDriver> create_onstep_telescope_with_site(
    int device_number, const ConnectionInfo& connection_info, std::optional<double> site_latitude_deg,
    std::optional<double> site_longitude_deg, std::optional<double> site_elevation_m,
    std::optional<bool> sync_time_on_connect, util::TaskClock& clock) {
    return std::make_unique<OnStepTelescopeDriver>(device_number, connection_info, site_latitude_deg,
                                                   site_longitude_deg, site_elevation_m, sync_time_on_connect,
                                                   util::ConnectionResolver<ConnectionInfo>{}, clock);
}

std::unique_ptr<TelescopeDriver> create_onstep_telescope_deferred(
    int device_number, util::ConnectionResolver<ConnectionInfo> connection_resolver,
    std::optional<double> site_latitude_deg, std::optional<double> site_longitude_deg,
    std::optional<double> site_elevation_m, std::optional<bool> sync_time_on_connect) {
    if (!connection_resolver) {
        throw AlpacaException("OnStep telescope: a connection resolver is required", AlpacaError::InvalidValue);
    }
    return std::make_unique<OnStepTelescopeDriver>(device_number, ConnectionInfo{}, site_latitude_deg,
                                                   site_longitude_deg, site_elevation_m, sync_time_on_connect,
                                                   std::move(connection_resolver));
}

ConnectionInfo resolve_onstep_serial_auto(int mount_index) {
    auto ports = enumerate_onstep_ports();
    if (ports.empty()) {
        throw AlpacaException(util::serial_auto_detect_failed_message("OnStep mount"));
    }
    if (mount_index < 0 || mount_index >= static_cast<int>(ports.size())) {
        throw AlpacaException("Mount index " + std::to_string(mount_index) + " out of range (found " +
                              std::to_string(ports.size()) + " mount(s))");
    }

    const auto& port = ports[static_cast<std::size_t>(mount_index)];
    ALPACA_LOG_INFO("OnStep", "Auto-detected mount on " + port.port_path + " (fw " + port.version_string + ")");

    ConnectionInfo conn;
    conn.type = ConnectionType::Serial;
    conn.port_path = port.port_path;
    return conn;
}

std::unique_ptr<TelescopeDriver> create_onstep_telescope_auto(int device_number, int mount_index,
                                                              std::optional<double> site_latitude_deg,
                                                              std::optional<double> site_longitude_deg,
                                                              std::optional<double> site_elevation_m,
                                                              std::optional<bool> sync_time_on_connect) {
    // The serial scan runs at connect time (#659), not here.
    return create_onstep_telescope_deferred(
        device_number, [mount_index] { return resolve_onstep_serial_auto(mount_index); }, site_latitude_deg,
        site_longitude_deg, site_elevation_m, sync_time_on_connect);
}

void set_park_home_before_start_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_park_home_before_start_for_testing(std::move(hook));
    }
}

void set_disconnect_before_close_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_disconnect_before_close_hook_for_testing(std::move(hook));
    }
}

void set_move_axis_resend_before_send_hook_for_testing(TelescopeDriver& driver, std::function<bool()> hook) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_move_axis_resend_before_send_hook_for_testing(std::move(hook));
    }
}

void set_status_poll_paused_for_testing(TelescopeDriver& driver, bool paused) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_status_poll_paused_for_testing(paused);
    }
}

bool poll_status_now_for_testing(TelescopeDriver& driver, bool full) {
    auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver);
    return onstep != nullptr && onstep->poll_status_now_for_testing(full);
}

bool status_poll_running_for_testing(const TelescopeDriver& driver) {
    const auto* onstep = dynamic_cast<const OnStepTelescopeDriver*>(&driver);
    return onstep != nullptr && onstep->status_poll_running_for_testing();
}

void set_status_poll_after_sample_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_status_poll_after_sample_hook_for_testing(std::move(hook));
    }
}

bool pulse_measurement_running_for_testing(const TelescopeDriver& driver) {
    const auto* onstep = dynamic_cast<const OnStepTelescopeDriver*>(&driver);
    return onstep != nullptr && onstep->pulse_measurement_running_for_testing();
}

void set_pulse_guide_gap_hook_for_testing(TelescopeDriver& driver, std::function<void()> hook) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_pulse_guide_gap_hook_for_testing(std::move(hook));
    }
}

void set_move_axis_resend_spawn_for_testing(TelescopeDriver& driver,
                                            std::function<std::thread(std::function<void()>)> spawn) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_move_axis_resend_spawn_for_testing(std::move(spawn));
    }
}

void set_park_home_spawn_for_testing(TelescopeDriver& driver, std::function<std::thread(std::function<void()>)> spawn) {
    if (auto* onstep = dynamic_cast<OnStepTelescopeDriver*>(&driver)) {
        onstep->set_park_home_spawn_for_testing(std::move(spawn));
    }
}

namespace detail {

namespace {
std::atomic<std::chrono::milliseconds::rep>& relink_window_ms() {
    static std::atomic<std::chrono::milliseconds::rep> window{
        std::chrono::duration_cast<std::chrono::milliseconds>(util::kRelinkMotionPreserveWindow).count()};
    return window;
}
}  // namespace

void set_relink_motion_preserve_window(std::chrono::milliseconds window) { relink_window_ms() = window.count(); }

std::chrono::milliseconds relink_motion_preserve_window() {
    return std::chrono::milliseconds(relink_window_ms().load());
}

}  // namespace detail

}  // namespace alpacacore::vendor::onstep
