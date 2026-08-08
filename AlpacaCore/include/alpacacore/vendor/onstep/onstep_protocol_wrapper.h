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

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace alpacacore::vendor::onstep {

struct OnStepPortInfo {
    std::string port_path;
    std::string device_id;
    std::string version_string;  // :GVN# response, captured for logging/diagnostics
};

std::vector<OnStepPortInfo> enumerate_onstep_ports();

/**
 * @brief Connection type for the OnStep protocol wrapper.
 *
 * OnStep controllers in this project are USB-serial only — Network exists
 * purely as an internal test seam (see AlpacaCore/tests/fake_mount_server.h)
 * so the mandatory concurrency stress test can drive the driver into a
 * CONNECTED state without real hardware. It is never surfaced through
 * AlpacaHTTP's router or web UI.
 */
enum class ConnectionType : std::uint8_t {
    Serial,
    Network  // Test-seam only — not exposed to users.
};

struct ConnectionInfo {
    ConnectionType type = ConnectionType::Serial;

    // Serial connection
    std::string port_path;
    int baud_rate = 9600;  // OnStep default per INDI reference driver

    // Network connection (test-seam only)
    std::string host;
    int tcp_port = 9999;

    // Per-command response timeout
    int response_timeout_ms = 5000;
};

struct Position {
    double ra_hours = 0.0;
    double dec_degrees = 0.0;
};

struct AltAz {
    double altitude_degrees = 0.0;
    double azimuth_degrees = 0.0;
};

/**
 * @brief Mount status decoded from the :GU# extended status string.
 *
 * side_of_pier: -1 = unknown/not reported, 0 = pier east, 1 = pier west.
 * Some OnStep/OnStepX firmware revisions omit the pier-side character from
 * :GU# entirely (see AGENTS.md OnStep notes) — callers must fall back to an
 * hour-angle computation when side_of_pier is -1.
 */
struct MountStatus {
    bool is_tracking = false;
    bool is_slewing = false;
    bool is_parked = false;
    bool is_at_home = false;
    int side_of_pier = -1;
    std::string raw_status;
};

struct SiteInfo {
    double latitude_degrees = 0.0;
    double longitude_degrees = 0.0;
};

/**
 * @brief Local date/time as reported by/sent to the mount over LX200 commands
 *        (:GL#/:GC#/:GG# and :SL#/:SC#/:SG#). The driver converts to/from UTC.
 */
struct TimeInfo {
    int hour = 0;
    int minute = 0;
    int second = 0;
    int month = 1;
    int day = 1;
    int year = 0;                   // two-digit year (e.g. 25 for 2025), per :SC#
    double utc_offset_hours = 0.0;  // :SG#/:GG#, e.g. -5.0 for EST
};

/**
 * @brief Clean C++ interface wrapping the OnStep (LX200-derived) serial
 *        protocol.
 *
 * This wrapper isolates the serial command protocol and transport layer from
 * the driver implementation. All platform-specific code is hidden using the
 * PIMPL pattern. See AGENTS.md for the OnStep command reference.
 */
class OnStepProtocolWrapper {
public:
    static OnStepProtocolWrapper& instance();

    bool connect(const ConnectionInfo& info);
    void disconnect();
    bool is_connected() const;

    /**
     * @brief Get the mount's reported product name.
     *
     * Sends :GVP#. Used both for auto-detect identification (must be
     * "OnStep" or "On-Step") and for driver metadata.
     */
    std::string get_product_name();

    /**
     * @brief Get the mount's firmware version string.
     *
     * Sends :GVN#.
     */
    std::string get_version_number();

    /**
     * @brief Get current RA/Dec position.
     *
     * Sends :GRa# and :GDe# (OnStep's high-precision reply variants —
     * fractional seconds/arcseconds — rather than :GR#/:GD#'s whole-second
     * resolution, which is too coarse for short PulseGuide pulses).
     */
    Position get_position();

    /**
     * @brief Get current Alt/Az position.
     *
     * Sends :GA# and :GZ#.
     */
    AltAz get_alt_az();

    /**
     * @brief Get mount status (tracking/slewing/parked/home/pier side).
     *
     * Sends :GU#.
     */
    MountStatus get_status();

    /**
     * @brief Set target RA.
     *
     * Sends :Sr HH:MM:SS#.
     */
    void set_target_ra(double ra_hours);

    /**
     * @brief Set target Dec.
     *
     * Sends :Sd sDD*MM:SS#.
     */
    void set_target_dec(double dec_degrees);

    /**
     * @brief Slew to the previously set target RA/Dec.
     *
     * Sends :MS#. Returns true if the mount accepted the slew (response "0"),
     * false if it reported the target as unreachable.
     */
    bool slew_to_target();

    /**
     * @brief Sync to the previously set target RA/Dec.
     *
     * Sends :CM#.
     */
    void sync_to_target();

    /**
     * @brief Abort any slew in progress on all axes.
     *
     * Sends :Q#.
     */
    void abort_slew();

    /**
     * @brief Start sidereal tracking.
     *
     * Sends :ST60.164275# — verified against real OnStep firmware
     * (Command.ino) and live hardware. OnStep's tracking on/off control is
     * :ST[freq]# (set tracking rate in legacy AC-mains-relative Hz), not a
     * dedicated on/off command; 60.164275 = 60.0 * 1.00273790935 solves for
     * an exact 1.0x (sidereal) rate. :To#/:Tn# (this wrapper's first guess,
     * inferred from the INDI driver's tracking-compensation switches) do
     * NOT toggle tracking — confirmed empirically against hardware.
     */
    void start_tracking();

    /**
     * @brief Stop tracking.
     *
     * Sends :ST0# — any frequency below 0.1 Hz stops tracking
     * (TrackingNone). See start_tracking().
     */
    void stop_tracking();

    /**
     * @brief Park the mount at its configured park position.
     *
     * Sends :hP#.
     */
    void park();

    /**
     * @brief Set the current position as the park position.
     *
     * Sends :hQ#.
     */
    void set_park();

    /**
     * @brief Unpark the mount.
     *
     * Sends :hR#.
     */
    void unpark();

    /**
     * @brief Seek the mount's home position using its home sensors.
     *
     * Sends :hC#.
     *
     * TODO: Verify against real OnStep firmware — the INDI reference driver
     * does not expose a dedicated find-home command distinct from park, so
     * this was inferred from general OnStep command documentation and needs
     * hardware confirmation.
     */
    void find_home();

    /**
     * @brief Select the mount's half-max slew rate for :Mn#/:Ms#/:Me#/:Mw#
     *        continuous-motion commands.
     *
     * Sends :RS#. Called once at connect as a safe default; move_axis_start()
     * selects the actual per-call rate via :RA[n.n]#/:RE[n.n]# (an arbitrary
     * custom rate) instead.
     */
    void select_max_slew_rate();

    /**
     * @brief Start continuous motion on one axis at the requested rate.
     *
     * Sends :RA[n.n]# (East/West, RA axis) or :RE[n.n]# (North/South, Dec
     * axis) to set a true arbitrary custom rate in real degrees/second,
     * then :Mn#/:Ms#/:Me#/:Mw# per direction.
     *
     * @param direction 0=North (Dec+), 1=South (Dec-), 2=East (RA+), 3=West (RA-)
     * @param rate_deg_per_sec Requested rate magnitude in degrees/second.
     */
    void move_axis_start(int direction, double rate_deg_per_sec);

    /**
     * @brief Stop continuous motion on one axis.
     *
     * Sends :Qn#/:Qs#/:Qe#/:Qw# per direction.
     *
     * @param direction 0=North, 1=South, 2=East, 3=West
     */
    void move_axis_stop(int direction);

    /**
     * @brief Hardware pulse guide with mount-side timing.
     *
     * Sends :Mgn####/:Mgs####/:Mge####/:Mgw#### (4-digit zero-padded
     * milliseconds). Fire-and-forget — the mount stops the guide pulse
     * itself after the requested duration.
     *
     * @param direction 0=North, 1=South, 2=East, 3=West
     * @param duration_ms Duration in milliseconds (0-9999)
     */
    void pulse_guide(int direction, int duration_ms);

    /**
     * @brief Get site latitude/longitude.
     *
     * Sends :Gt# and :Gg#.
     */
    SiteInfo get_site_info();

    /**
     * @brief Set site latitude.
     *
     * Sends :St sDD*MM#.
     */
    void set_latitude(double latitude_degrees);

    /**
     * @brief Set site longitude.
     *
     * Sends :Sg sDDD*MM#.
     *
     * TODO: Verify sign convention against real firmware. Modern OnStep
     * accepts signed East-positive longitude directly; classic LX200 used an
     * unsigned 0-360 West-positive convention. This wrapper sends the signed
     * ASCOM (East-positive) form.
     */
    void set_longitude(double longitude_degrees);

    /**
     * @brief Get local apparent sidereal time in hours.
     *
     * Sends :GS#.
     */
    double get_sidereal_time_hours();

    /**
     * @brief Get local date/time and UTC offset from the mount.
     *
     * Sends :GL#, :GC#, :GG#.
     */
    TimeInfo get_time();

    /**
     * @brief Set local date/time and UTC offset on the mount.
     *
     * Sends :SL#, :SC#, :SG#.
     */
    void set_time(const TimeInfo& info);

    // Low-level protocol access (for advanced use)

    /**
     * @brief Send command and get response.
     *
     * @param command LX200 command (without leading ':' or trailing '#' is
     *        acceptable — both are normalized).
     * @return Response string (without # terminator).
     */
    std::string send_command(const std::string& command, bool require_hash_terminator = true,
                             int timeout_ms_override = 0);

    /**
     * @brief Send command and discard any response, tolerating silence.
     *
     * Many OnStep "blind" commands still ack with a stray byte; this drains
     * whatever arrives within a short window instead of leaving it to
     * corrupt the next command's response framing.
     */
    void send_command_blind(const std::string& command);

    /**
     * @brief Drain any stale bytes from the serial/network input buffer.
     */
    void flush_input();

private:
    OnStepProtocolWrapper();
    ~OnStepProtocolWrapper();
    OnStepProtocolWrapper(const OnStepProtocolWrapper&) = delete;
    OnStepProtocolWrapper& operator=(const OnStepProtocolWrapper&) = delete;

    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

}  // namespace alpacacore::vendor::onstep
