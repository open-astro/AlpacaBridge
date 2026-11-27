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

// Test-only: each telescope driver over its roster fake, for the fake ConformU server (fake_conformu_server.cpp, #2078
// slice 6). One factory per telescope row that is not Sky-Watcher (conformu_skywatcher_factory.h keeps its own, two
// fakes behind one driver), each guarded by the FAKE_CONFORMU_<VENDOR> macro the server's CMake sets when that vendor
// is built. Each fake is a function-local static, built when its row is served, so it outlives every driver the server
// builds. It lives here, not in AlpacaHTTP, so the vendor headers stay out of AlpacaHTTP (scripts/check_layering.py).

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/task_clock.h>

#include <memory>

#ifdef FAKE_CONFORMU_CELESTRON
#include <alpacacore/vendor/celestron/celestron_telescope_driver.h>

#include "fake_celestron_handset.h"
#include "fake_mount_server.h"
#endif
#ifdef FAKE_CONFORMU_SYNSCAN
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include "fake_mount_server.h"
#include "fake_synscan_handset.h"
#endif
#ifdef FAKE_CONFORMU_IOPTRON
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>

#include "fake_ioptron_mount.h"
#endif

namespace alpacacore::test::conformu {

#ifdef FAKE_CONFORMU_IOPTRON
// The HAE16 EQ (":MountInfo#" 0012) in ConformU mode: it answers from the shared motion model (#2614), so a GOTO
// takes its travel time and Park reports parked.
inline FakeIoptronMount& ioptron_mount() {
    static FakeIoptronMount mount("0012", FakeIoptronMount::conformu_motion_config());
    return mount;
}
// The driver over the fake's loopback TCP port, with the made-up Sydney Opera House site (never a real site).
inline std::unique_ptr<TelescopeDriver> make_ioptron_telescope(int device_number) {
    vendor::ioptron::ConnectionInfo connection;
    connection.type = vendor::ioptron::ConnectionType::Network;
    connection.host = "127.0.0.1";
    connection.tcp_port = ioptron_mount().port();
    return vendor::ioptron::create_ioptron_telescope_with_site(device_number, connection, -33.856784, 151.215297, 42.0,
                                                               /*sync_time_on_connect=*/true);
}
#endif

// The hand-controller rows (#2692): the handset fake in ConformU mode behind a FakeMountServer on loopback TCP, the
// driver on the real task clock and the made-up Sydney Opera House site (never a real site). The handset is declared
// first, so it outlives the server that holds its responder.
#ifdef FAKE_CONFORMU_CELESTRON
struct NexStarHost {
    FakeCelestronHandset handset{util::default_task_clock(), FakeCelestronHandset::conformu_motion_config()};
    FakeMountServer server{handset.responder()};
};
inline NexStarHost& nexstar_mount() {
    static NexStarHost host;
    return host;
}
inline std::unique_ptr<TelescopeDriver> make_nexstar_telescope(int device_number) {
    vendor::celestron::ConnectionInfo connection;
    connection.type = vendor::celestron::ConnectionType::Network;
    connection.host = "127.0.0.1";
    connection.tcp_port = nexstar_mount().server.port();
    return vendor::celestron::create_celestron_telescope_with_site(device_number, connection, -33.856784, 151.215297,
                                                                   42.0, /*sync_time_on_connect=*/true);
}
#endif

#ifdef FAKE_CONFORMU_SYNSCAN
struct SynScanHost {
    FakeSynScanHandset handset{util::default_task_clock(), FakeSynScanHandset::conformu_motion_config()};
    FakeMountServer server{handset.responder()};
};
inline SynScanHost& synscan_mount() {
    static SynScanHost host;
    return host;
}
inline std::unique_ptr<TelescopeDriver> make_synscan_telescope(int device_number) {
    vendor::synscan::ConnectionInfo connection;
    connection.type = vendor::synscan::ConnectionType::Network;
    connection.host = "127.0.0.1";
    connection.tcp_port = synscan_mount().server.port();
    return vendor::synscan::create_synscan_telescope_with_site(device_number, connection,
                                                               vendor::synscan::SynScanVersion::V4, -33.856784,
                                                               151.215297, 42.0, /*sync_time_on_connect=*/true);
}
#endif

}  // namespace alpacacore::test::conformu
