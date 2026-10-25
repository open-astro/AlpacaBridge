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

// A SynScan hand controller exposes the same Prolific / FTDI / CP210x adapter
// classes that several other serial auto-detect scans target (Sky-Watcher
// motor controllers, iOptron focusers and filter wheels), and it speaks its
// own protocol at 9600 8N1. A SynScan V4 handset (firmware 04.40.00, built-in
// PL2303 067b:23a3) stops answering serial ENTIRELY after it receives bytes at
// the wrong rate - one motor-controller probe at 115200 is enough - and only
// a power-cycle brings it back. Seen on the EQM-35 Pro rig in 2026-09: the
// handset was silent to every protocol at every baud until rebooted, and went
// silent again the moment 115200 traffic touched it, which is what the
// "hand-controller commands time out" report actually was.
//
// Every scan that may send non-9600 traffic to a Prolific-class port should
// therefore ask for the handset's echo first and leave the port alone when it
// answers. The echo ("K" + byte -> byte + "#") is the protocol's own link
// check and costs ~20-135 ms on a live handset (measured on hardware) - but a
// port that is NOT a handset (a real motor controller, or an unrelated
// device) can only be ruled out by waiting out the full timeout, since
// silence is the only signal absence gives. Every non-handset candidate a
// caller's scan probes therefore pays the whole `timeout_ms` before its own
// probe even starts; callers on a latency-sensitive path should size
// `timeout_ms` accordingly rather than assume this is always cheap.
//
// POSIX-only, like the serial scans it serves.

#ifndef _WIN32

#include <alpacacore/util/logging.h>
#include <alpacacore/util/serial_link.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/util/serial_read.h>
#include <termios.h>

#include <chrono>
#include <string>

namespace alpacacore::util {

/**
 * @brief Send the SynScan protocol echo command ("K" + byte) on an already-
 *        open, already-configured (9600 8N1, non-canonical) fd and wait up
 *        to @p timeout_ms for the exact reply (the same byte followed by
 *        '#'). Leaves the fd open; the caller owns it before and after.
 *        Every read goes through util::read_until() (and so tty_read()) for the
 *        time left before the deadline, so the bound holds even on a USB-serial adapter that
 *        ignores VMIN/VTIME, where a bare read() on a silent port parks
 *        forever (#836). Pass a NON-blocking fd (a SerialLink's) for a hard
 *        bound: on a blocking fd the bound is complete only in the common
 *        case, because such an adapter can report a spurious "readable" from
 *        poll() and the read() that follows then parks with no deadline. The
 *        fd is accepted either way.
 *
 * Strict, and tolerant of noise ahead of the real reply: only an exact
 * `#`-terminated token that is the echoed byte followed by '#' counts as a
 * handset. A `#`-terminated token that does NOT match (a stale reply still
 * draining from a command sent before this probe opened the port - e.g. this
 * scan running right after a service restart, with the driver's last query
 * still in flight) is discarded and reading continues until the deadline,
 * not treated as "not a handset": mistaking a live handset for a silent one
 * is the false negative this whole guard exists to prevent, since the caller
 * then proceeds to probe the port at another baud and can wedge it. Only
 * real silence, or nothing but non-echo tokens for the whole window, counts
 * as "not a handset".
 *
 * @param timeout_ms How long to wait for the echo before concluding "not a
 *        handset". A live handset answers in well under 200 ms (16-135 ms
 *        measured on hardware).
 */
inline bool exchange_synscan_echo_on_fd(int fd, int timeout_ms) {
    constexpr char kEchoByte = 'B';
    const char cmd[] = {'K', kEchoByte};
    if (!tty_write_all(fd, cmd, sizeof(cmd), timeout_ms)) {
        return false;
    }

    ReadSpec spec;
    spec.stop = ReadSpec::Stop::Terminator;
    spec.terminators = "#";
    spec.skip.clear();  // CR/LF are ordinary bytes here: they make a token a non-echo
    spec.max_len = 9;   // a token longer than 8 bytes ahead of '#' is garbage, not our echo
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const ReadResult token = read_until(fd, spec, deadline);
        if (token.outcome == ReadOutcome::Terminator) {
            if (token.bytes.size() == 2 && token.bytes[0] == kEchoByte) {
                return true;
            }
            continue;  // not our echo: keep listening past it, not away from it
        }
        if (token.outcome != ReadOutcome::Overflow) {
            break;  // silence until the deadline, or hangup or read error
        }
        // Overflow: runaway garbage ahead of '#', not our echo either.
    }
    return false;
}

/**
 * @brief True if a SynScan hand controller answers its protocol echo on
 *        @p port_path at 9600 8N1 within @p timeout_ms.
 *
 * Opens the port through SerialLink with a Probe claim (issue #1202), held
 * for exactly this check: a port a connected device holds is never opened
 * (false, "not a handset"), and a port another scan is probing is waited for.
 * Call it BEFORE opening your own SerialLink on the same port, never while
 * holding one: the claim would wait on the caller's own probe claim and then
 * report "not a handset". A caller that already holds the port runs
 * exchange_synscan_echo_on_fd() on its own fd at 9600 instead.
 *
 * @param timeout_ms How long to wait for the echo before concluding "not a
 *        handset". A live handset answers in well under 200 ms (16-135 ms
 *        measured on hardware); the default leaves headroom for a slower
 *        adapter/USB hub without unduly penalising the (far more common)
 *        non-handset port, which always pays this full wait.
 * @param owner Operator-facing name of the scan running the check, shown to
 *        a connect that has to wait for it.
 */
inline bool port_answers_synscan_echo(const std::string& port_path, int timeout_ms = 300,
                                      const std::string& owner = "SynScan hand controller check") {
    std::string error;
    SerialLink link = SerialLink::open(
        port_path, SerialClaimKind::Probe, owner,
        [](struct termios& tty) {
            make_raw_8n1(tty, B9600);
            tty.c_cflag &= ~static_cast<tcflag_t>(HUPCL);  // do not drop DTR on close: some adapters reset the far end
        },
        &error);
    if (!link) {
        ALPACA_LOG_DEBUG("Serial", "SynScan handset check skipped " + port_path + ": " + error);
        return false;
    }
    tcflush(link.fd(), TCIOFLUSH);
    return exchange_synscan_echo_on_fd(link.fd(), timeout_ms);
}

}  // namespace alpacacore::util

#endif  // !_WIN32
