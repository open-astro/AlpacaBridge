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
//
// util::port_answers_synscan_echo() is the guard every non-9600 serial probe
// runs before touching a Prolific-class port: a SynScan V4 handset stops
// answering serial entirely after bytes at the wrong rate and only a
// power-cycle recovers it (EQM-35 Pro rig, 2026-09). A pty stands in for the
// handset so the three outcomes - exact echo, silence, and a non-echo reply -
// are exercised hardware-free.
#ifndef _WIN32

#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/util/synscan_handset_probe.h>
#include <termios.h>

#include <chrono>
#include <string>

#include "catch2_compat.h"
#include "pty_responder.h"

namespace {

// The pty "handset": every chunk read from the master goes through the
// responder, whose reply (if any) is written straight back (pty_responder.h).
using FakeSerialHandset = alpacacore::test::PtyResponder;

long elapsed_ms(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
}

}  // namespace

TEST_CASE("SynScan handset probe - a handset's exact echo is recognised", "[util][synscan_handset_probe]") {
    FakeSerialHandset handset(alpacacore::test::answer_handset_echo);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(alpacacore::util::port_answers_synscan_echo(handset.slave_path(), 1000));
    CHECK(elapsed_ms(t0) < 500);        // answered at once, not at the deadline
    CHECK(handset.received() == "KB");  // exactly the echo went out, nothing at another baud
}

TEST_CASE("SynScan handset probe - a silent port is not a handset and returns at the deadline",
          "[util][synscan_handset_probe]") {
    FakeSerialHandset silent([](const std::string&) { return std::string(); });
    const auto t0 = std::chrono::steady_clock::now();
    CHECK_FALSE(alpacacore::util::port_answers_synscan_echo(silent.slave_path(), 300));
    const auto ms = elapsed_ms(t0);
    CHECK(ms >= 250);
    CHECK(ms < 1500);
}

TEST_CASE("SynScan handset probe - a non-echo reply is not mistaken for a handset", "[util][synscan_handset_probe]") {
    // A motor-controller board or another device answering *something* must
    // not make the scan skip the port: only the exact echo identifies a handset.
    FakeSerialHandset other([](const std::string&) { return std::string("0#"); });
    CHECK_FALSE(alpacacore::util::port_answers_synscan_echo(other.slave_path(), 500));
}

TEST_CASE("SynScan handset probe - a port a connected device holds is never opened",
          "[util][synscan_handset_probe][serial][issue1202]") {
    FakeSerialHandset handset(alpacacore::test::answer_handset_echo);
    struct termios before {};
    REQUIRE(tcgetattr(handset.keepalive_fd(), &before) == 0);
    cfsetispeed(&before, B19200);
    cfsetospeed(&before, B19200);
    REQUIRE(tcsetattr(handset.keepalive_fd(), TCSANOW, &before) == 0);

    auto held = alpacacore::util::SerialPortClaim::acquire(handset.slave_path(),
                                                           alpacacore::util::SerialClaimKind::Link, "Other device");
    REQUIRE(held);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK_FALSE(alpacacore::util::port_answers_synscan_echo(handset.slave_path(), 300));
    CHECK(elapsed_ms(t0) < 250);  // refused at once, not after the echo window
    CHECK(handset.received().empty());

    struct termios after {};
    REQUIRE(tcgetattr(handset.keepalive_fd(), &after) == 0);
    CHECK(cfgetospeed(&after) == B19200);  // never retuned to 9600 either

    // The probe's own claim is gone once it returns.
    held.release();
    CHECK(alpacacore::util::port_answers_synscan_echo(handset.slave_path(), 1000));
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(handset.slave_path()));
}

#endif  // !_WIN32
