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

// Serial-port ownership of the ZWO mount wrapper (issue #1202), over a pty
// handset: the connected link holds its port through SerialLink, a connect to
// a port another device holds is refused with the holder's name, and a port
// another program retunes cannot hang a command or disconnect.
#ifndef _WIN32

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/zwo/zwo_mount_protocol_wrapper.h>
#include <alpacacore/vendor/zwo/zwo_telescope_driver.h>
#include <fcntl.h>
#include <unistd.h>

#include <memory>
#include <string>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_pty_write.h"
#include "mount_serial_ownership_cases.h"
#include "pty_responder.h"

namespace {

using alpacacore::vendor::zwo::ConnectionInfo;
using alpacacore::vendor::zwo::ConnectionType;
using alpacacore::vendor::zwo::ZWOMountProtocolWrapper;
namespace cases = alpacacore::test::mount_serial;

ConnectionInfo serial_info(const std::string& path) {
    ConnectionInfo info;
    info.type = ConnectionType::Serial;
    info.port_path = path;
    info.baud_rate = 9600;
    return info;
}

}  // namespace

TEST_CASE("ZWO mount serial - a connect refuses a port another device holds", "[zwo][serial][issue1202]") {
    alpacacore::test::PtyResponder device(alpacacore::test::answer_handset_echo, "ZWO mount handset");
    cases::driver_refuses_held_port(
        device, [&] { return alpacacore::vendor::zwo::create_zwo_telescope(0, serial_info(device.slave_path())); });
}

TEST_CASE("ZWO mount serial - the link holds its port and releases it on disconnect", "[zwo][serial][issue1202]") {
    alpacacore::test::PtyResponder device(alpacacore::test::answer_handset_echo, "ZWO mount handset");
    ZWOMountProtocolWrapper wrapper;
    cases::wrapper_holds_and_releases_port(wrapper, serial_info(device.slave_path()), device.slave_path());
}

TEST_CASE("ZWO mount serial - a command on a retuned port times out and the line settings come back",
          "[zwo][serial][issue1202]") {
    alpacacore::test::PtyResponder device(alpacacore::test::answer_handset_echo, "ZWO mount handset");
    ZWOMountProtocolWrapper wrapper;
    cases::retuned_port_times_out_and_recovers(wrapper, serial_info(device.slave_path()), device,
                                               [&] { (void)wrapper.send_command(":GVP#", true, 300); });
}

TEST_CASE("ZWO mount serial - independent links retain their own exclusive port claims", "[zwo][serial][isolation]") {
    const auto responder = [](const std::string&) { return std::string("AM5#"); };
    alpacacore::test::PtyResponder a(responder), b(responder);
    ZWOMountProtocolWrapper first, second, collision;
    const auto first_info = serial_info(a.slave_path());
    const auto second_info = serial_info(b.slave_path());
    REQUIRE(first.connect(first_info));
    REQUIRE(second.connect(second_info));
    CHECK(alpacacore::util::is_serial_port_in_use(a.slave_path()));
    CHECK(alpacacore::util::is_serial_port_in_use(b.slave_path()));
    CHECK_FALSE(collision.connect(first_info));
    CHECK(collision.last_connect_error().find("ZWO mount") != std::string::npos);
    CHECK(first.send_command(":GVP") == "AM5");
    second.cancel_pending_io();
    second.disconnect();
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(b.slave_path()));
    CHECK(alpacacore::util::is_serial_port_in_use(a.slave_path()));
    CHECK(first.send_command(":GVP") == "AM5");
    REQUIRE(second.connect(second_info));
    CHECK(second.send_command(":GVP") == "AM5");
    first.disconnect();
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(a.slave_path()));
    CHECK(alpacacore::util::is_serial_port_in_use(b.slave_path()));
    CHECK(second.send_command(":GVP") == "AM5");
}

// Issue #2262: over serial the ZWO mount's errno verdict is the shared util::SerialLink one, so EBADF on the mount's
// own fd is a lost link (the wrapper already counted it in its own list, now gone): link_lost(), which the driver's
// Connected reads, latches on the command that meets it, whether its write or its reply read does, and only the next
// connect clears it.
TEST_CASE("ZWO mount serial - EBADF on the mount's fd is a lost link until the next connect",
          "[zwo][mount][unit][link-lost][issue2262]") {
    alpacacore::test::PtyResponder device([](const std::string&) { return std::string("AM5#"); }, "ZWO mount");
    ZWOMountProtocolWrapper wrapper;
    const auto info = serial_info(device.slave_path());
    const auto before = alpacacore::test::fds_open_on(device.slave_path());
    REQUIRE(wrapper.connect(info));
    const int fd = alpacacore::test::new_fd(before, alpacacore::test::fds_open_on(device.slave_path()));
    REQUIRE(fd >= 0);
    REQUIRE(wrapper.send_command(":GVP") == "AM5");
    REQUIRE_FALSE(wrapper.link_lost());

    // On the command's write the call reports its own failed send, as for EIO; on its reply read, the lost transport.
    const bool on_write = GENERATE(true, false);
    INFO((on_write ? "EBADF on the command's write" : "EBADF on the reply read"));
    REQUIRE(alpacacore::test::replace_fd_on_tty(fd, device.slave_path(), on_write ? O_RDONLY : O_WRONLY));
    alpacacore::test::require_alpaca_error(
        [&] { (void)wrapper.send_command(":GVP"); },
        on_write ? alpacacore::AlpacaError::DriverException : alpacacore::AlpacaError::NotConnected);
    CHECK(wrapper.link_lost());

    wrapper.disconnect();
    REQUIRE(wrapper.connect(info));
    CHECK_FALSE(wrapper.link_lost());
    CHECK(wrapper.send_command(":GVP") == "AM5");
    wrapper.disconnect();
}

#endif  // _WIN32
