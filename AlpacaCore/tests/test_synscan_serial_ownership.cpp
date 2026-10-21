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

// Serial-port ownership of the SynScan wrapper (issue #1202), over a pty
// handset: the connected link holds its port through SerialLink, a connect to
// a port another device holds is refused with the holder's name, and a port
// another program retunes cannot hang a command or disconnect.
#ifndef _WIN32

#include <alpacacore/vendor/synscan/synscan_protocol_wrapper.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include <memory>
#include <string>

#include "catch2_compat.h"
#include "mount_serial_ownership_cases.h"
#include "pty_responder.h"

namespace {

using alpacacore::vendor::synscan::ConnectionInfo;
using alpacacore::vendor::synscan::ConnectionType;
using alpacacore::vendor::synscan::SynScanProtocolWrapper;
namespace cases = alpacacore::test::mount_serial;

ConnectionInfo serial_info(const std::string& path) {
    ConnectionInfo info;
    info.type = ConnectionType::Serial;
    info.port_path = path;
    info.baud_rate = 9600;
    return info;
}

}  // namespace

TEST_CASE("SynScan serial - a connect refuses a port another device holds", "[synscan][serial][issue1202]") {
    alpacacore::test::PtyResponder device(alpacacore::test::answer_handset_echo, "SynScan handset");
    cases::driver_refuses_held_port(device, [&] {
        return alpacacore::vendor::synscan::create_synscan_telescope(0, serial_info(device.slave_path()),
                                                                     alpacacore::vendor::synscan::SynScanVersion::Auto);
    });
}

TEST_CASE("SynScan serial - the link holds its port and releases it on disconnect", "[synscan][serial][issue1202]") {
    alpacacore::test::PtyResponder device(alpacacore::test::answer_handset_echo, "SynScan handset");
    cases::wrapper_holds_and_releases_port(SynScanProtocolWrapper::instance(), serial_info(device.slave_path()),
                                           device.slave_path());
}

TEST_CASE("SynScan serial - a command on a retuned port times out and the line settings come back",
          "[synscan][serial][issue1202]") {
    alpacacore::test::PtyResponder device(alpacacore::test::answer_handset_echo, "SynScan handset");
    auto& wrapper = SynScanProtocolWrapper::instance();
    cases::retuned_port_times_out_and_recovers(wrapper, serial_info(device.slave_path()), device,
                                               [&] { (void)wrapper.send_command("V", true, 300); });
}

#endif  // _WIN32
