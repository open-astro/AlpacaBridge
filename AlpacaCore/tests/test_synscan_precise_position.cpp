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

// SynScan precise position replies: "e" and "z" answer "XXXXXX00,YYYYYY00#",
// and the position is the UPPER 24 bits of each field; the last two digits are
// ignored (synscanserialcommunicationprotocol_version33.md, "precise GET
// AZM-ALT"). The replies below are verbatim from a SynScan V4 handset
// (04.28.00) on an EQM-35 Pro, 2026-10-02 (issue #785).

#ifndef _WIN32

#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include <cmath>
#include <memory>
#include <string>

#include "catch2_compat.h"
#include "fake_mount_server.h"

namespace {

using alpacacore::vendor::synscan::ConnectionInfo;
using alpacacore::vendor::synscan::ConnectionType;
using alpacacore::vendor::synscan::SynScanVersion;
using Fake = alpacacore::test::FakeMountServer;

std::string handset(const std::string& chunk) {
    if (chunk.empty()) return "0#";
    switch (chunk[0]) {
        case 'K':
            return std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
        case 'V':
            return "042A00#";
        case 'e':
            return "5CECE200,D001B900#";
        case 'z':
            return "8A4FDA00,0D527400#";
        case 'L':
            return "0#";
        default:
            return "#";
    }
}

}  // namespace

TEST_CASE("SynScan precise RA/Dec and Az/Alt decode the upper 24 bits of each field", "[synscan][telescope][unit]") {
    Fake fake(handset);
    REQUIRE(fake.ok());
    ConnectionInfo info;
    info.type = ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = fake.port();
    info.response_timeout_ms = 1000;
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(0, info, SynScanVersion::V4);
    driver->set_connected(true);

    // 0x5CECE2 / 2^24 * 24 h; 0xD001B9 / 2^24 * 360 deg - 360.
    CHECK(std::abs(driver->get_right_ascension() - (8.711749)) < 1e-5);
    CHECK(std::abs(driver->get_declination() - (-67.490537)) < 1e-5);
    // 0x8A4FDA / 2^24 * 360 deg; 0x0D5274 / 2^24 * 360 deg.
    CHECK(std::abs(driver->get_azimuth() - (194.501138)) < 1e-5);
    CHECK(std::abs(driver->get_altitude() - (18.734179)) < 1e-5);

    driver->set_connected(false);
}

#endif
