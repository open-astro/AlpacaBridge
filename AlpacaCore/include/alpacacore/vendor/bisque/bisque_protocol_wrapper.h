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

#include <string>
#include <memory>

namespace alpacacore::vendor::bisque {

struct ConnectionInfo {
    std::string host = "localhost";
    int tcp_port = 3040;
    int response_timeout_ms = 3000;
};

struct Position {
    double ra_hours = 0.0;
    double dec_degrees = 0.0;
};

struct AltAz {
    double altitude_degrees = 0.0;
    double azimuth_degrees = 0.0;
};

class BisqueProtocolWrapper {
public:
    static BisqueProtocolWrapper& instance();

    bool connect(const ConnectionInfo& info);
    void disconnect();
    bool is_connected() const;

    // TheSkyX mount connection handshake.
    // Sends ConnectAndDoNotUnpark, checks IsConnected.
    // Special: returns "1" on success (no error prefix).
    bool handshake();

    // Current position queries.
    Position get_ra_dec();
    AltAz get_alt_az();

    // Slew (async — sets Asynchronous=true, fires SlewToRaDec).
    void slew_to_ra_dec(double ra_hours, double dec_degrees);
    bool is_slew_complete();

    // Sync to coordinates.
    void sync_to_coordinates(double ra_hours, double dec_degrees);

    // Abort all motion.
    void abort();

    // Parking.
    void park();
    void unpark();
    bool is_parked();
    void set_park_position();

    // Tracking control.
    // on: 1=enable, 0=disable.
    // ignore_rates: 1=use sidereal, 0=use custom ra_rate/dec_rate.
    // ra_rate/dec_rate: arcseconds per second.
    void set_tracking(bool on, bool ignore_rates, double ra_rate, double dec_rate);
    bool is_tracking();

    // Home.
    void find_home();

    // Pier side: 1 = west side of pier, else = east.
    int get_pier_side();

    // Open loop motion (manual jogging).
    // direction: 0=North, 1=South, 2=East, 3=West.
    // rate: 0-based index into slewspeeds table.
    void start_open_loop_motion(int direction, int rate);
    void stop_open_loop_motion();

    // Direct guide via sky6DirectGuide.MoveTelescope.
    // ra_arcsec/dec_arcsec: displacement in arcseconds.
    void guide(double ra_arcsec, double dec_arcsec);

    // Low-level command execution.
    // Wraps js_body in TheSkyX boilerplate, sends over TCP, reads response.
    // Returns the parsed value portion of the response.
    std::string send_command(const std::string& js_body, int timeout_ms = 0);

    // Wraps js_body in try/catch OK pattern.
    // Throws AlpacaException on failure.
    void send_ok_command(const std::string& js_body, int timeout_ms = 0);

private:
    BisqueProtocolWrapper();
    ~BisqueProtocolWrapper();

    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace alpacacore::vendor::bisque
