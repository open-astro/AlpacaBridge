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

// Test-only: the Sky-Watcher telescope driver over a FakeSkyWatcherMount's
// loopback UDP port, or over a FakeSkyWatcherSerialBoard's pty (#2078 slice 2),
// for the fake ConformU server (fake_conformu_server.cpp). Both fakes share the
// FakeSkyWatcherBoard axis model. It lives here, not in AlpacaHTTP, so the vendor
// header stays out of AlpacaHTTP (scripts/check_layering.py). The fake must
// outlive the returned driver.

#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>

#include <memory>

#include "fake_skywatcher_mount.h"
#include "fake_skywatcher_serial_board.h"

namespace alpacacore::test {

// Site values are made up (Sydney Opera House), never a real observing site.
inline std::unique_ptr<TelescopeDriver> make_fake_skywatcher_telescope(int device_number,
                                                                       const FakeSkyWatcherMount& mount) {
    vendor::skywatcher::ConnectionInfo connection;
    connection.type = vendor::skywatcher::ConnectionType::Network;
    connection.host = "127.0.0.1";
    connection.udp_port = mount.port();
    return vendor::skywatcher::create_skywatcher_telescope(device_number, connection, -33.856784, 151.215297, 42.0);
}

// The same driver over the serial transport (exchange_serial, settle_serial): the board's pty at 9600 baud, the
// EQM-35 Pro / handset-less wiring, with the same made-up site.
inline std::unique_ptr<TelescopeDriver> make_fake_skywatcher_serial_telescope(int device_number,
                                                                              const FakeSkyWatcherSerialBoard& board) {
    vendor::skywatcher::ConnectionInfo connection;
    connection.type = vendor::skywatcher::ConnectionType::Serial;
    connection.port_path = board.slave_path();
    connection.baud_rate = 9600;
    return vendor::skywatcher::create_skywatcher_telescope(device_number, connection, -33.856784, 151.215297, 42.0);
}

}  // namespace alpacacore::test
