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

#include <alpacacore/util/gpio_line_backend.h>
#include <alpacacore/util/task_clock.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace alpacacore::util {

// One port of a GPIO power-port board.
struct PowerPort {
    std::string name;
    std::optional<unsigned int> line;  // none: no GPIO line (a pass-through), never handed to the backend
    bool writable = true;              // false: set_value answers NotImplemented, and the line is not claimed
    bool pwm = false;                  // 0-100 % duty from a worker thread; otherwise 0 or 1
};

struct PortsConfig {
    std::string device_path;  // what the backend opens: a gpiochip node, a misc device
    std::vector<PowerPort> ports;
    unsigned int pwm_frequency_hz = 50;  // 1..100000, the board's own default
    std::string model_name;              // for messages: "ASIAIR Pro"
    std::string log_category;
};

// The port table and the value/PWM engine of a GPIO 12 V power-port board (ASCOM Switch), over a
// GpioLineBackend. A board's wrapper holds one as a member (decision 0027). It is the wrapper
// interface util::PowerPortSwitchDriver takes: open(), close(), is_open(), port_count(),
// get_value() and set_value().
//
// - open() claims the writable lines through the backend and changes none (decision 0021). A
//   backend that reads its lines gives each cached value the line's level; one that cannot
//   keeps the "on" cache and leaves a PWM port unarmed: no worker writes it until a client
//   sets its duty, and close() does not settle it.
// - A PWM port has one worker thread, started by open() and joined by close(), that bit-bangs
//   its duty at pwm_frequency_hz. Waits and the next edge go through the TaskClock (decision
//   0005). Steady 0 % and 100 % write once, then nothing.
// - A failed write logs ERROR and stops that port's worker; set_value on the port then answers
//   DriverException until the next open(), never success for a port that is not driven.
// - close() stops the workers (signal under the state lock, join outside it), then drives each
//   armed PWM line to the steady level of its duty (above 0 high) and releases the lines. It
//   never drives a boolean line (never power-cycle on disconnect).
//
// Lock order: lifecycle_mutex_ (open, close) -> io_mutex_ (every backend call) -> mutex_ (state,
// a leaf: never held across a backend call or a join). is_open() and get_value() never wait on
// device I/O.
class SoftPwmPorts {
public:
    SoftPwmPorts(PortsConfig config, GpioLineBackend& backend, TaskClock& clock = default_task_clock());
    ~SoftPwmPorts();

    SoftPwmPorts(const SoftPwmPorts&) = delete;
    SoftPwmPorts& operator=(const SoftPwmPorts&) = delete;
    SoftPwmPorts(SoftPwmPorts&&) = delete;
    SoftPwmPorts& operator=(SoftPwmPorts&&) = delete;

    void open();
    void close();
    // Lock-free (issue #382): an atomic, so the driver's getter never waits behind open() or close().
    bool is_open() const { return open_.load(std::memory_order_acquire); }

    std::size_t port_count() const { return config_.ports.size(); }
    unsigned int pwm_frequency_hz() const { return config_.pwm_frequency_hz; }

    // Boolean port: 0 or 1. PWM port: the duty in percent, 0..100.
    int get_value(std::size_t index) const;
    // InvalidValue for an index or value out of range, NotImplemented for a read-only port (both
    // whether open or not), NotConnected while closed, DriverException when the write failed.
    void set_value(std::size_t index, int value);

private:
    struct PortState {
        int value = 0;               // under mutex_
        bool armed = false;          // the line's level is known: read at open(), or chosen by set_value()
        bool stop = false;           // the worker must exit
        bool failed = false;         // the worker exited after a failed write
        int opened_level = -1;       // the level open() read from the line, -1 when it could not
        std::size_t line_index = 0;  // the position in the vector the backend opened; claimed ports only
        bool claimed = false;
        std::condition_variable cv;  // wakes this port's worker on mutex_
        std::thread worker;
    };

    void close_locked();  // lifecycle_mutex_ held
    void pwm_loop(std::size_t index);
    // Writes through the backend under io_mutex_, without mutex_. False after logging the failure.
    bool write_line(std::size_t index, bool high);
    void check_index(std::size_t index) const;

    const PortsConfig config_;
    GpioLineBackend& backend_;
    TaskClock& clock_;
    const std::chrono::nanoseconds period_;

    std::mutex lifecycle_mutex_;
    std::mutex io_mutex_;
    mutable std::mutex mutex_;
    std::atomic<bool> open_{false};
    bool closing_ = false;         // under mutex_
    bool lines_readable_ = false;  // under mutex_: the last open() read the lines' levels
    std::vector<std::unique_ptr<PortState>> states_;
};

// A backend and the engine over it, built in place: the wrapper type of a board whose backend
// is constructed with the arguments after the config (util::PowerPortSwitchDriver builds its
// wrapper from them). The backend is declared first, so it outlives the engine.
template <typename Backend>
class OwnedSoftPwmPorts {
public:
    template <typename... BackendArgs>
    explicit OwnedSoftPwmPorts(PortsConfig config, BackendArgs&&... backend_args)
        : backend_(std::forward<BackendArgs>(backend_args)...), ports_(std::move(config), backend_) {}

    void open() { ports_.open(); }
    void close() { ports_.close(); }
    bool is_open() const { return ports_.is_open(); }
    std::size_t port_count() const { return ports_.port_count(); }
    int get_value(std::size_t index) const { return ports_.get_value(index); }
    void set_value(std::size_t index, int value) { ports_.set_value(index, value); }

private:
    Backend backend_;
    SoftPwmPorts ports_;
};

}  // namespace alpacacore::util
