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

#include <alpacacore/util/serial_io.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <string>

namespace alpacacore::util {

/**
 * @brief Send the SynScan protocol echo command ("K" + byte) on an already-
 *        open, already-configured (9600 8N1, non-canonical) fd and wait up
 *        to @p timeout_ms for the exact reply (the same byte followed by
 *        '#'). Leaves the fd open; the caller owns it before and after.
 *        Every read waits on poll(POLLIN) for the time left before the
 *        deadline, so the bound holds even on a USB-serial adapter that
 *        ignores VMIN/VTIME, where a bare read() on a silent port parks
 *        forever (#836). The fd may be blocking or non-blocking.
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
    if (!write_all(fd, cmd, sizeof(cmd))) {
        return false;
    }

    std::string token;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) {
            break;
        }
        struct pollfd pfd {};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, static_cast<int>(remaining));
        if (pr == 0) {
            break;
        }
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        char ch = 0;
        const ssize_t r = read(fd, &ch, 1);
        if (r == 1) {
            token.push_back(ch);
            if (ch == '#') {
                if (token.size() == 2 && token[0] == kEchoByte) {
                    return true;
                }
                token.clear();  // not our echo — keep listening past it, not away from it
            } else if (token.size() > 8) {
                token.clear();  // runaway garbage ahead of '#', not our echo either
            }
        } else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
            break;  // hangup or read error; EAGAIN is a spurious "readable"
        }
    }
    return false;
}

/**
 * @brief True if a SynScan hand controller answers its protocol echo on
 *        @p port_path at 9600 8N1 within @p timeout_ms.
 *
 * @param timeout_ms How long to wait for the echo before concluding "not a
 *        handset". A live handset answers in well under 200 ms (16-135 ms
 *        measured on hardware); the default leaves headroom for a slower
 *        adapter/USB hub without unduly penalising the (far more common)
 *        non-handset port, which always pays this full wait.
 */
inline bool port_answers_synscan_echo(const std::string& port_path, int timeout_ms = 300) {
    const int fd = open(port_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }
    struct termios tty {};
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        return false;
    }
    cfmakeraw(&tty);
    cfsetospeed(&tty, B9600);
    cfsetispeed(&tty, B9600);
    tty.c_cflag |= CREAD | CLOCAL;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag &= ~HUPCL;  // do not drop DTR on close: some adapters reset the far end on it
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;
    // Stay non-blocking, as the Sky-Watcher ":e1" probe does: the echo
    // helper's poll() bounds the wait, and a read after a spurious
    // "readable" returns EAGAIN instead of parking.
    if (tcsetattr(fd, TCSANOW, &tty) != 0 || !set_nonblocking(fd)) {
        close(fd);
        return false;
    }
    tcflush(fd, TCIOFLUSH);

    const bool answered = exchange_synscan_echo_on_fd(fd, timeout_ms);
    close(fd);
    return answered;
}

}  // namespace alpacacore::util

#endif  // !_WIN32
