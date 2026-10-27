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

// The one way a vendor wrapper opens a serial port (issue #1202).
//
// SerialLink::open() does, in this order:
//   1. claim the port in the registry (serial_port_registry.h): a port another
//      connected driver holds is refused, a port another probe has open is
//      waited for;
//   2. open it O_RDWR | O_NOCTTY | O_NONBLOCK;
//   3. take TIOCEXCL, so another program (a GPS scanner, ModemManager, an
//      older AlpacaBridge probe) cannot open the port and retune it;
//   4. apply the caller's line settings and record what the kernel kept.
//
// The fd stays NON-BLOCKING. Every read goes through tty_read() and every
// write through tty_write_all(), both bounded with poll(). VMIN/VTIME have no
// effect on a non-blocking fd, so a retune to VMIN=1/VTIME=0 cannot park a
// reader in n_tty_read and hang Disconnect or service stop (the #1202
// WandererRotator and OnStep hangs). Some USB CDC-ACM ports ignore VTIME
// anyway (the Sky-Watcher rule in the serial-autodetect instructions); this
// makes that the only shape.
//
// When a read times out, tty_read() checks the line settings against the
// ones SerialLink recorded and puts them back if another program changed
// them, logging a warning. A retuned port therefore costs one timed-out
// reply instead of a link that never recovers (the PDH in #1202 stayed
// faulted until restart). Streaming readers that never time out (garbage
// arrives at the wrong baud) call restore_serial_line_settings() from their
// link-fault path.
//
// close() (and the destructor) releases TIOCEXCL before close(): on a tty that
// another fd keeps open (a pty fake's keep-alive, a second program) the
// exclusive flag would otherwise outlive this fd and refuse the next connect.
//
// POSIX-only, like serial_io.h; callers include it inside their existing
// `#ifndef _WIN32` branches.

#ifndef _WIN32

#include <alpacacore/util/logging.h>
#include <alpacacore/util/serial_io.h>
#include <alpacacore/util/serial_port_registry.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace alpacacore::util {

namespace detail {

struct SerialLineRecord {
    std::string port;
    struct termios applied {};
};

// fd -> the line settings SerialLink applied to it. Lets tty_read() restore a
// retuned port without every wrapper threading its SerialLink through its
// static read helpers. An entry lives exactly as long as the open fd: it is
// erased before ::close(), so a reused fd number never inherits it.
struct SerialLineRegistry {
    std::mutex mutex;
    std::map<int, SerialLineRecord> lines;
};
// Never destroyed, for the same reason as detail::serial_registry().
inline SerialLineRegistry& serial_line_registry() {
    static auto* r = new SerialLineRegistry();  // NOLINT(cppcoreguidelines-owning-memory)
    return *r;
}

inline bool same_line_settings(const struct termios& a, const struct termios& b) {
    return a.c_iflag == b.c_iflag && a.c_oflag == b.c_oflag && a.c_cflag == b.c_cflag && a.c_lflag == b.c_lflag &&
           cfgetispeed(&a) == cfgetispeed(&b) && cfgetospeed(&a) == cfgetospeed(&b) && a.c_cc[VMIN] == b.c_cc[VMIN] &&
           a.c_cc[VTIME] == b.c_cc[VTIME];
}

/// Whole milliseconds left to @p t, rounded up, so a wait never ends early: a
/// wait under 1 ms polls 1 ms, not 0 (a 0 returns at once and spins, #1696).
/// 0 only once @p t has passed.
inline int wait_ms_until(std::chrono::steady_clock::time_point t) {
    const auto left = t - std::chrono::steady_clock::now();
    if (left <= std::chrono::steady_clock::duration::zero()) return 0;
    return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(left).count());
}

}  // namespace detail

/**
 * @brief Put back the line settings SerialLink applied to @p fd if another
 *        program changed them.
 * @return true if a change was found (and the restore attempted).
 *
 * A no-op for an fd SerialLink did not open. Cheap: one tcgetattr().
 */
inline bool restore_serial_line_settings(int fd) {
    // The registry lock is a leaf: held only to copy the record out. tcgetattr()
    // and tcsetattr() can park on a stalled adapter, and the lock is shared by
    // every port. The fd belongs to the caller, so it cannot be closed (and its
    // number reused) under this call; no re-check under the lock is needed.
    detail::SerialLineRecord record;
    {
        auto& reg = detail::serial_line_registry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        auto it = reg.lines.find(fd);
        if (it == reg.lines.end()) return false;
        record = it->second;
    }
    struct termios now {};
    if (tcgetattr(fd, &now) != 0) return false;
    if (detail::same_line_settings(now, record.applied)) return false;
    const bool ok = tcsetattr(fd, TCSANOW, &record.applied) == 0;
    const int err = errno;
    const std::string& port = record.port;
    // Logged with no lock held.
    ALPACA_LOG_WARN("Serial", "Line settings of " + port +
                                  " were changed by another program while this driver held it; " +
                                  (ok ? std::string("restored them") : "restoring them failed: " + errno_string(err)));
    return true;
}

/**
 * @brief Read up to @p len bytes from @p fd, waiting at most @p timeout_ms.
 * @return bytes read (> 0); 0 when nothing arrived in time; -1 on an error
 *         (errno set). A 0 return from read() itself (hangup, a pty whose
 *         master closed) is reported as -1 with errno EIO, so a caller's
 *         "nothing yet" path cannot spin on a dead link.
 *
 * Use on a non-blocking fd (SerialLink keeps it so): a byte that another
 * reader took between poll() and read() then costs an EAGAIN and another
 * poll(), never a blocked read().
 */
inline ssize_t tty_read(int fd, void* buf, std::size_t len, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        struct pollfd pfd {};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, detail::wait_ms_until(deadline));
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr == 0) {
            const int saved = errno;
            restore_serial_line_settings(fd);
            errno = saved;
            return 0;
        }
        if ((pfd.revents & POLLNVAL) != 0) {
            errno = EBADF;
            return -1;
        }
        // Callers hold their wrapper mutex across I/O. This read cannot block: the fd is
        // non-blocking and poll() just reported data (or it returns EAGAIN at once).
        const ssize_t n = ::read(fd, buf, len);  // NOLINT(clang-analyzer-unix.BlockInCriticalSection)
        if (n > 0) return n;
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            if (detail::wait_ms_until(deadline) == 0) return 0;
            continue;
        }
        return -1;
    }
}

/**
 * @brief Write the whole of @p data to @p fd within @p timeout_ms.
 *
 * The non-blocking counterpart of write_all(): an EAGAIN (the output queue is
 * full, a flow-controlled or stalled adapter) waits on poll(POLLOUT) instead
 * of failing at once or blocking forever. Returns false on a hard error or
 * when the deadline passes (errno ETIMEDOUT).
 */
inline bool tty_write_all(int fd, const void* data, std::size_t len, int timeout_ms = 2000) {
    const auto* p = static_cast<const char*>(data);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);
    std::size_t total = 0;
    while (total < len) {
        const ssize_t n = ::write(fd, p + total, len - total);
        if (n > 0) {
            total += static_cast<std::size_t>(n);
            continue;
        }
        if (n == 0) {
            errno = EIO;
            return false;
        }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
        const int left = detail::wait_ms_until(deadline);
        if (left == 0) {
            errno = ETIMEDOUT;
            return false;
        }
        struct pollfd pfd {};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        if (::poll(&pfd, 1, left) < 0 && errno != EINTR) return false;
    }
    return true;
}

/// Overload for string payloads.
inline bool tty_write_all(int fd, const std::string& data, int timeout_ms = 2000) {
    return tty_write_all(fd, data.data(), data.size(), timeout_ms);
}

/// Adjusts the line settings SerialLink applies: baud, framing, flow control.
/// VMIN/VTIME are ignored (the fd is non-blocking).
using SerialLineConfigure = std::function<void(struct termios&)>;

/// 8N1, raw, no flow control, CLOCAL|CREAD, at @p baud: the shape almost every
/// wrapper builds by hand. Callers that need more (HUPCL off, RTS/CTS) start
/// from this and adjust.
inline void make_raw_8n1(struct termios& tty, speed_t baud) {
    cfmakeraw(&tty);
    cfsetispeed(&tty, baud);
    cfsetospeed(&tty, baud);
    tty.c_cflag &= ~static_cast<tcflag_t>(PARENB | CSTOPB | CSIZE | CRTSCTS);
    tty.c_cflag |= CS8 | CLOCAL | CREAD;
    tty.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | IXANY);
}

/// Whether SerialLink::open() fails when the kernel refuses TIOCEXCL. Best
/// effort logs a warning and keeps the port (the default); Required refuses
/// it, for a link that must not share its tty (the Celestron CGX, decision
/// 0011). The general rule is best effort (decision 0017).
enum class SerialExclusive : std::uint8_t {
    BestEffort,
    Required,
};

/**
 * @brief A serial port this process owns: registry claim + fd + TIOCEXCL.
 *
 * Move-only. Thread-compatible like the int fd it replaces: the owning wrapper
 * serialises open/close against its I/O with its own mutex.
 */
class SerialLink {
public:
    SerialLink() = default;
    ~SerialLink() { close(); }
    SerialLink(const SerialLink&) = delete;
    SerialLink& operator=(const SerialLink&) = delete;
    SerialLink(SerialLink&& other) noexcept
        : claim_(std::move(other.claim_)), fd_(std::exchange(other.fd_, -1)), path_(std::move(other.path_)) {}
    SerialLink& operator=(SerialLink&& other) noexcept {
        if (this != &other) {
            close();
            claim_ = std::move(other.claim_);
            fd_ = std::exchange(other.fd_, -1);
            path_ = std::move(other.path_);
        }
        return *this;
    }

    /**
     * @brief Claim, open, lock and configure @p path.
     *
     * @param kind  Link for a connected driver, Probe for an auto-detect probe.
     * @param owner Operator-facing device name, shown to whoever is refused.
     * @param error Receives the operator-facing reason on failure. A probe
     *              refused because a connected driver holds the port gets an
     *              error too; probes normally just skip that port.
     * @param exclusive Required makes a refused TIOCEXCL a failure.
     * @return an open link, or a closed one on failure (nothing held).
     */
    static SerialLink open(const std::string& path, SerialClaimKind kind, const std::string& owner,
                           const SerialLineConfigure& configure, std::string* error = nullptr,
                           std::chrono::milliseconds probe_wait = kSerialProbeClaimWait,
                           SerialExclusive exclusive = SerialExclusive::BestEffort) {
        SerialLink link;
        auto fail = [&](const std::string& why) {
            if (error != nullptr) *error = why;
            link.close();
            return std::move(link);
        };
        SerialPortHolder holder;
        link.claim_ = SerialPortClaim::acquire(path, kind, owner, probe_wait, &holder);
        if (!link.claim_) return fail(serial_port_held_message(path, holder));
        link.path_ = path;

        link.fd_ = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (link.fd_ < 0) {
            const int err = errno;
            std::string why = "Failed to open serial port " + path + ": " + errno_string(err);
            if (err == EBUSY) why += " (another program holds it open exclusively)";
            return fail(why);
        }
        if (::ioctl(link.fd_, TIOCEXCL) != 0) {
            if (exclusive == SerialExclusive::Required) {
                return fail("Could not lock serial port " + path + " for exclusive use: " + errno_string(errno));
            }
            ALPACA_LOG_WARN("Serial", "Could not lock " + path + " for exclusive use (" + errno_string(errno) +
                                          "); another program can still open it");
        }
        if (!link.apply(configure)) {
            return fail("Failed to configure serial port " + path + ": " + errno_string(errno));
        }
        return link;
    }

    bool is_open() const { return fd_ >= 0; }
    explicit operator bool() const { return is_open(); }
    int fd() const { return fd_; }
    /// The path as the caller passed it.
    const std::string& path() const { return path_; }
    /// The canonical path the registry holds.
    const std::string& key() const { return claim_.key(); }

    /// Change the line settings (a baud-rate step in a probe). Keeps the fd
    /// non-blocking and records the new settings for restore.
    bool reconfigure(const SerialLineConfigure& configure) { return is_open() && apply(configure); }

    /// See restore_serial_line_settings().
    bool restore_line_settings() { return is_open() && restore_serial_line_settings(fd_); }

    /**
     * @brief True while the device node at path() is still the one this fd
     *        opened. False once the adapter is unplugged (the node is gone, or
     *        it now names a different device). No I/O.
     */
    bool node_present() const {
        if (!is_open()) return false;
        struct stat held {};
        struct stat now {};
        if (::fstat(fd_, &held) != 0) return false;
        if (::stat(path_.c_str(), &now) != 0) return false;
        return held.st_rdev == now.st_rdev && held.st_nlink > 0;
    }

    /// Release TIOCEXCL, close the fd and release the claim. Idempotent, never throws.
    void close() noexcept {
        if (fd_ >= 0) {
            {
                auto& reg = detail::serial_line_registry();
                std::lock_guard<std::mutex> lock(reg.mutex);
                reg.lines.erase(fd_);
            }
            (void)::ioctl(fd_, TIOCNXCL);
            (void)::close(fd_);
            fd_ = -1;
        }
        claim_.release();
    }

private:
    bool apply(const SerialLineConfigure& configure) {
        struct termios tty {};
        if (tcgetattr(fd_, &tty) != 0) return false;
        if (configure) configure(tty);
        // Non-blocking reads ignore these, but a blocking reader that reuses the
        // fd by mistake should still return rather than park.
        tty.c_cc[VMIN] = 0;
        tty.c_cc[VTIME] = 0;
        if (tcsetattr(fd_, TCSANOW, &tty) != 0) return false;
        if (!set_nonblocking(fd_)) return false;
        struct termios kept {};
        if (tcgetattr(fd_, &kept) != 0) return false;
        auto& reg = detail::serial_line_registry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        reg.lines[fd_] = detail::SerialLineRecord{path_, kept};
        return true;
    }

    SerialPortClaim claim_;
    int fd_ = -1;
    std::string path_;
};

}  // namespace alpacacore::util

#endif  // _WIN32
