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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/serial_io.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/vendor/celestron/celestron_cgx_protocol_wrapper.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>
#include <string>
#include <utility>

namespace alpacacore::vendor::celestron {

namespace {

constexpr std::uint8_t kAuxStart = 0x3b;
constexpr std::size_t kMaximumFrameSize = 32;
constexpr auto kMaximumExchangeTimeout = std::chrono::seconds(5);
constexpr std::uint32_t kAuxPositionModulus = 1U << 24;

[[noreturn]] void throw_link_error(const std::string& message) {
    throw AlpacaException("Celestron CGX USB: " + message, AlpacaError::DriverException);
}

std::uint8_t axis_address(CgxAxis axis) {
    switch (axis) {
        case CgxAxis::RightAscension:
        case CgxAxis::Declination:
            return static_cast<std::uint8_t>(axis);
    }
    throw AlpacaException("Celestron CGX AUX axis is invalid", AlpacaError::InvalidValue);
}

void require_empty_ack(const AuxFrame& response, const char* operation) {
    if (!response.payload.empty()) {
        throw_link_error(std::string(operation) + " returned an unexpected payload");
    }
}

bool decode_axis_completion(const AuxFrame& response, const char* operation) {
    if (response.payload.size() != 1 || (response.payload[0] != 0x00 && response.payload[0] != 0xff)) {
        throw_link_error(std::string(operation) + " returned an invalid completion status");
    }
    return response.payload[0] == 0xff;
}

int poll_timeout_ms(std::chrono::steady_clock::time_point deadline) {
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
        return 0;
    }
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    return static_cast<int>(std::max<std::int64_t>(1, milliseconds.count()));
}

}  // namespace

CelestronCgxProtocol::~CelestronCgxProtocol() { disconnect(); }

void CelestronCgxProtocol::connect_serial(const std::string& port_path) {
    if (port_path.empty()) {
        throw_link_error("a serial port path is required");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code path_error;
    const std::filesystem::path canonical = std::filesystem::canonical(port_path, path_error);
    const std::string registry_path = path_error ? port_path : canonical.string();
    struct stat held {
    }, current{};
    if (serial_fd_ >= 0 && registry_path == registered_port_path_ && ::fstat(serial_fd_, &held) == 0 &&
        ::stat(port_path.c_str(), &current) == 0 && held.st_nlink != 0 && held.st_dev == current.st_dev &&
        held.st_ino == current.st_ino && held.st_rdev == current.st_rdev) {
        if (desynchronized_) {
            // Recover matching without an ownership gap. The failed exchange
            // already advanced the session generation and invalidated references.
            if (::tcflush(serial_fd_, TCIFLUSH) != 0) {
                disconnect_locked();
                throw_link_error("cannot clear stale AUX input before recovery");
            }
            receive_buffer_.clear();
            desynchronized_ = false;
            healthy_.store(true);
        }
        return;
    }
    disconnect_locked();
    if (!util::try_mark_serial_port_open(registry_path)) {
        throw AlpacaException("Celestron CGX USB port is already in use: " + port_path, AlpacaError::InvalidOperation);
    }

    const int fd = ::open(port_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        util::mark_serial_port_closed(registry_path);
        throw_link_error("cannot open " + port_path + ": " + util::errno_string(errno));
    }

    bool exclusive = false;
    try {
        // The process-local registry cannot exclude another daemon's serial probe.
        if (::ioctl(fd, TIOCEXCL) != 0) {
            throw_link_error("cannot claim exclusive serial access: " + util::errno_string(errno));
        }
        exclusive = true;
        struct termios settings {};
        if (::tcgetattr(fd, &settings) != 0) {
            throw_link_error("cannot read serial settings: " + util::errno_string(errno));
        }

        ::cfmakeraw(&settings);
        settings.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
        settings.c_cflag |= CS8 | CREAD | CLOCAL;
        settings.c_cc[VMIN] = 0;
        settings.c_cc[VTIME] = 0;
        if (::cfsetispeed(&settings, B115200) != 0 || ::cfsetospeed(&settings, B115200) != 0 ||
            ::tcsetattr(fd, TCSANOW, &settings) != 0) {
            throw_link_error("cannot configure 115200-baud serial link: " + util::errno_string(errno));
        }
        if (!util::set_nonblocking(fd)) {
            throw_link_error("cannot enable bounded serial I/O: " + util::errno_string(errno));
        }
        if (::tcflush(fd, TCIFLUSH) != 0) {
            throw_link_error("cannot clear stale AUX input before reconnect: " + util::errno_string(errno));
        }

        registered_port_path_ = registry_path;
        receive_buffer_.clear();
        serial_fd_ = fd;
        ++session_generation_;
        healthy_.store(true);
    } catch (...) {
        if (exclusive) (void)::ioctl(fd, TIOCNXCL);
        ::close(fd);
        util::mark_serial_port_closed(registry_path);
        registered_port_path_.clear();
        throw;
    }
}

void CelestronCgxProtocol::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    disconnect_locked();
}

bool CelestronCgxProtocol::is_connected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return serial_fd_ >= 0;
}

AuxFrame CelestronCgxProtocol::exchange(const AuxFrame& request, std::chrono::milliseconds timeout) {
    if (timeout <= std::chrono::milliseconds::zero() || timeout > kMaximumExchangeTimeout) {
        throw AlpacaException("Celestron CGX AUX timeout must be between 1 and 5000 ms", AlpacaError::InvalidValue);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (serial_fd_ < 0) {
        throw AlpacaException("Celestron CGX USB is not connected", AlpacaError::NotConnected);
    }
    if (desynchronized_) {
        throw_link_error("AUX response timed out previously; reconnect before issuing another command");
    }

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + timeout;
    const auto log_failure = [&](std::string_view reason) noexcept {
        try {
            const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started);
            ALPACA_LOG_ERROR("Celestron CGX", "AUX transaction failed: source=" + std::to_string(request.source) +
                                                  " destination=" + std::to_string(request.destination) +
                                                  " command=" + std::to_string(request.command) +
                                                  " elapsed_ms=" + std::to_string(elapsed.count()) + " timeout_ms=" +
                                                  std::to_string(timeout.count()) + " error=" + std::string(reason));
        } catch (...) {
            // Diagnostics must never replace the original transaction failure.
            return;
        }
    };
    std::vector<std::uint8_t> request_bytes;
    try {
        request_bytes = encode_aux_frame(request);
    } catch (const std::invalid_argument& error) {
        throw_link_error(std::string("invalid AUX request: ") + error.what());
    }
    bool request_started = false;
    try {
        request_started = true;
        write_frame_locked(request_bytes, deadline);

        while (true) {
            AuxFrame response;
            response = decode_aux_frame(read_frame_locked(deadline));
            if (response.source == request.destination && response.destination == request.source &&
                response.command == request.command) {
                return response;
            }
            // AUX carries no transaction id. Discard traffic for another address or
            // command while preserving the one deadline for this exchange.
        }
    } catch (const std::invalid_argument& error) {
        if (request_started) {
            log_failure(error.what());
            desynchronized_ = true;
            ++session_generation_;
            healthy_.store(false);
        }
        throw_link_error(std::string("invalid AUX response: ") + error.what());
    } catch (...) {
        if (request_started) {
            try {
                throw;
            } catch (const std::exception& error) {
                log_failure(error.what());
            } catch (...) {
                log_failure("non-standard exception");
            }
            desynchronized_ = true;
            ++session_generation_;
            healthy_.store(false);
        }
        throw;
    }
}

std::uint32_t CelestronCgxProtocol::get_axis_position(CgxAxis axis, std::chrono::milliseconds timeout) {
    const AuxFrame response =
        exchange(AuxFrame{.source = 0x00, .destination = axis_address(axis), .command = 0x01, .payload = {}}, timeout);
    if (response.payload.size() != 3) {
        throw_link_error("MC_GET_POSITION returned a payload that is not 24 bits");
    }
    return (static_cast<std::uint32_t>(response.payload[0]) << 16) |
           (static_cast<std::uint32_t>(response.payload[1]) << 8) | static_cast<std::uint32_t>(response.payload[2]);
}

void CelestronCgxProtocol::set_axis_position(CgxAxis axis, std::uint32_t position, std::chrono::milliseconds timeout) {
    if (position >= kAuxPositionModulus) {
        throw AlpacaException("Celestron CGX encoder position must fit in 24 bits", AlpacaError::InvalidValue);
    }
    const AuxFrame request{.source = 0x00,
                           .destination = axis_address(axis),
                           .command = 0x04,
                           .payload = {static_cast<std::uint8_t>(position >> 16),
                                       static_cast<std::uint8_t>(position >> 8), static_cast<std::uint8_t>(position)}};
    require_empty_ack(exchange(request, timeout), "MC_SET_POSITION");
}

void CelestronCgxProtocol::goto_axis_fast(CgxAxis axis, std::uint32_t position, std::chrono::milliseconds timeout) {
    goto_axis(axis, position, 0x02, "MC_GOTO_FAST", timeout);
}

void CelestronCgxProtocol::goto_axis_slow(CgxAxis axis, std::uint32_t position, std::chrono::milliseconds timeout) {
    goto_axis(axis, position, 0x17, "MC_GOTO_SLOW", timeout);
}

void CelestronCgxProtocol::goto_axis(CgxAxis axis, std::uint32_t position, std::uint8_t command, const char* operation,
                                     std::chrono::milliseconds timeout) {
    if (position >= kAuxPositionModulus) {
        throw AlpacaException("Celestron CGX GOTO position must fit in 24 bits", AlpacaError::InvalidValue);
    }
    const AuxFrame request{.source = 0x00,
                           .destination = axis_address(axis),
                           .command = command,
                           .payload = {static_cast<std::uint8_t>(position >> 16),
                                       static_cast<std::uint8_t>(position >> 8), static_cast<std::uint8_t>(position)}};
    require_empty_ack(exchange(request, timeout), operation);
}

bool CelestronCgxProtocol::is_axis_slew_done(CgxAxis axis, std::chrono::milliseconds timeout) {
    return decode_axis_completion(
        exchange(AuxFrame{.source = 0x00, .destination = axis_address(axis), .command = 0x13, .payload = {}}, timeout),
        "MC_SLEW_DONE");
}

void CelestronCgxProtocol::start_axis_home(CgxAxis axis, std::chrono::milliseconds timeout) {
    require_empty_ack(
        exchange(AuxFrame{.source = 0x00, .destination = axis_address(axis), .command = 0x0b, .payload = {}}, timeout),
        "MC_LEVEL_START");
}

bool CelestronCgxProtocol::is_axis_home_done(CgxAxis axis, std::chrono::milliseconds timeout) {
    return decode_axis_completion(
        exchange(AuxFrame{.source = 0x00, .destination = axis_address(axis), .command = 0x12, .payload = {}}, timeout),
        "MC_LEVEL_DONE");
}

void CelestronCgxProtocol::pulse_guide_axis(CgxAxis axis, int velocity_percent, int duration_centiseconds,
                                            std::chrono::milliseconds timeout) {
    if (velocity_percent < -100 || velocity_percent > 100 || duration_centiseconds < 1 || duration_centiseconds > 255) {
        throw AlpacaException("Celestron CGX pulse guide rate or duration is out of range", AlpacaError::InvalidValue);
    }
    const auto velocity = static_cast<std::uint8_t>(static_cast<std::int8_t>(velocity_percent));
    const AuxFrame request{.source = 0x00,
                           .destination = axis_address(axis),
                           .command = 0x26,
                           .payload = {velocity, static_cast<std::uint8_t>(duration_centiseconds)}};
    require_empty_ack(exchange(request, timeout), "MC_AUX_GUIDE");
}

bool CelestronCgxProtocol::is_aux_guide_active(CgxAxis axis, std::chrono::milliseconds timeout) {
    const AuxFrame response =
        exchange(AuxFrame{.source = 0x00, .destination = axis_address(axis), .command = 0x27, .payload = {}}, timeout);
    if (response.payload.size() != 1 || response.payload[0] > 1) {
        throw_link_error("MC_AUX_GUIDE_ACTIVE returned an invalid status");
    }
    return response.payload[0] == 1;
}

void CelestronCgxProtocol::disconnect_locked() {
    healthy_.store(false);
    if (serial_fd_ >= 0) {
        ++session_generation_;
        (void)::ioctl(serial_fd_, TIOCNXCL);
        ::close(serial_fd_);
        serial_fd_ = -1;
    }
    util::mark_serial_port_closed(registered_port_path_);
    registered_port_path_.clear();
    receive_buffer_.clear();
    desynchronized_ = false;
}

void CelestronCgxProtocol::wait_for_locked(short events, std::chrono::steady_clock::time_point deadline) {
    while (true) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw_link_error("AUX transaction timed out");
        }
        struct pollfd descriptor {};
        descriptor.fd = serial_fd_;
        descriptor.events = events;
        const int result = ::poll(&descriptor, 1, poll_timeout_ms(deadline));
        if (result > 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw_link_error("AUX transaction timed out");
            }
            if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                disconnect_locked();
                throw_link_error("serial device was removed or became unavailable");
            }
            if ((descriptor.revents & events) != 0) {
                return;
            }
            continue;
        }
        if (result == 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw_link_error("AUX transaction timed out");
            }
            continue;
        }
        if (errno != EINTR) {
            throw_link_error("serial poll failed: " + util::errno_string(errno));
        }
    }
}

void CelestronCgxProtocol::write_frame_locked(const std::vector<std::uint8_t>& bytes,
                                              std::chrono::steady_clock::time_point deadline) {
    std::size_t written = 0;
    while (written < bytes.size()) {
        wait_for_locked(POLLOUT, deadline);
        const ssize_t count = ::write(serial_fd_, bytes.data() + written, bytes.size() - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
        } else if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            const auto error = util::errno_string(errno);
            disconnect_locked();
            throw_link_error("serial write failed: " + error);
        }
    }
}

std::vector<std::uint8_t> CelestronCgxProtocol::read_frame_locked(std::chrono::steady_clock::time_point deadline) {
    while (true) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw_link_error("AUX transaction timed out");
        }
        const auto start = std::find(receive_buffer_.begin(), receive_buffer_.end(), kAuxStart);
        receive_buffer_.erase(receive_buffer_.begin(), start);
        if (receive_buffer_.size() >= 2) {
            const std::size_t length = receive_buffer_[1];
            if (length < 3 || length + 3 > kMaximumFrameSize) {
                receive_buffer_.erase(receive_buffer_.begin());
                continue;
            }
            const std::size_t frame_size = length + 3;
            if (receive_buffer_.size() >= frame_size) {
                std::vector<std::uint8_t> frame(receive_buffer_.begin(),
                                                receive_buffer_.begin() + static_cast<std::ptrdiff_t>(frame_size));
                receive_buffer_.erase(receive_buffer_.begin(),
                                      receive_buffer_.begin() + static_cast<std::ptrdiff_t>(frame_size));
                return frame;
            }
        }

        wait_for_locked(POLLIN, deadline);
        std::array<std::uint8_t, 128> incoming{};
        // The owned fd stays O_NONBLOCK; the lock serializes whole AUX exchanges.
        // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection)
        const ssize_t count = ::read(serial_fd_, incoming.data(), incoming.size());
        if (count > 0) {
            receive_buffer_.insert(receive_buffer_.end(), incoming.begin(),
                                   incoming.begin() + static_cast<std::ptrdiff_t>(count));
        } else if (count == 0) {
            disconnect_locked();
            throw_link_error("serial device returned end-of-file");
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            const auto error = util::errno_string(errno);
            disconnect_locked();
            throw_link_error("serial read failed: " + error);
        }
    }
}

}  // namespace alpacacore::vendor::celestron
