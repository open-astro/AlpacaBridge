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
#include <alpacacore/util/power_port_state_store.h>
#include <alpacacore/util/soft_pwm_ports.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <optional>

namespace alpacacore::util {

SoftPwmPorts::SoftPwmPorts(PortsConfig config, GpioLineBackend& backend, TaskClock& clock)
    : config_(std::move(config)),
      backend_(backend),
      clock_(clock),
      period_(std::chrono::nanoseconds(1000000000LL / std::max(1u, config_.pwm_frequency_hz))) {
    if (config_.ports.empty()) {
        throw AlpacaException(config_.model_name + " port configuration is empty", AlpacaError::InvalidValue);
    }
    if (config_.pwm_frequency_hz == 0 || config_.pwm_frequency_hz > 100000) {
        throw AlpacaException(config_.model_name + " PWM frequency out of supported range (1..100000 Hz)",
                              AlpacaError::InvalidValue);
    }
    states_.reserve(config_.ports.size());
    for (const auto& port : config_.ports) {
        if (port.writable && !port.line) {
            throw AlpacaException(config_.model_name + " port '" + port.name + "' is writable but has no GPIO line",
                                  AlpacaError::InvalidValue);
        }
        auto state = std::make_unique<PortState>();
        // "On" until open() reads the line (or, on a backend that cannot, until a client writes): the
        // first claim replaces it with the level the boot left (reconcile_cached_value).
        state->value = port.pwm ? 100 : 1;
        state->claimed = port.writable && port.line.has_value();
        states_.push_back(std::move(state));
    }
}

SoftPwmPorts::~SoftPwmPorts() {
    try {
        close();
    } catch (const std::exception& e) {
        ALPACA_LOG_WARN(config_.log_category, "Error during " + config_.model_name + " GPIO close: " + e.what());
    }
}

void SoftPwmPorts::check_index(std::size_t index) const {
    if (index >= config_.ports.size()) {
        throw AlpacaException("Port index out of range", AlpacaError::InvalidValue);
    }
}

void SoftPwmPorts::open() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    if (open_.load()) {
        return;
    }
    std::vector<unsigned int> lines;
    for (std::size_t i = 0; i < states_.size(); ++i) {
        if (states_[i]->claimed) {
            states_[i]->line_index = lines.size();
            lines.push_back(*config_.ports[i].line);
        }
    }
    std::vector<int> levels;
    {
        std::lock_guard<std::mutex> io(io_mutex_);
        levels = backend_.open(config_.device_path, lines);
        if (!levels.empty() && levels.size() != lines.size()) {
            backend_.close();
            throw AlpacaException(config_.model_name + " GPIO backend read " + std::to_string(levels.size()) +
                                      " levels for " + std::to_string(lines.size()) + " lines",
                                  AlpacaError::DriverException);
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_readable_ = !levels.empty();
        closing_ = false;
        for (std::size_t i = 0; i < states_.size(); ++i) {
            PortState& s = *states_[i];
            s.stop = false;
            s.failed = false;
            s.armed = false;
            s.opened_level = -1;
            if (s.claimed && lines_readable_) {
                s.opened_level = levels[s.line_index];
                s.value = reconcile_cached_value(s.value, s.opened_level, config_.ports[i].pwm ? 100 : 1);
                s.armed = true;
            }
        }
        open_.store(true, std::memory_order_release);
    }
    try {
        for (std::size_t i = 0; i < states_.size(); ++i) {
            if (states_[i]->claimed && config_.ports[i].pwm) {
                states_[i]->worker = std::thread(&SoftPwmPorts::pwm_loop, this, i);
            }
        }
    } catch (...) {
        close_locked();
        throw;
    }
}

void SoftPwmPorts::close() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    close_locked();
}

void SoftPwmPorts::close_locked() {
    if (!open_.load()) {
        return;
    }
    // Phase 1: stop the workers under the state lock. They are joined outside it: a worker takes
    // it between its waits, so joining under it would deadlock.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
        for (auto& s : states_) {
            s->stop = true;
            s->cv.notify_all();
        }
    }
    // Phase 2: no lock held.
    for (auto& s : states_) {
        if (s->worker.joinable()) {
            s->worker.join();
        }
    }
    // Phase 3: settle and release. A worker stops wherever it is in its cycle, so its line may be
    // low: drive each armed PWM line to the steady level of its duty (above 0 stays on) before the
    // lines go. A port no client armed was never written, and settling it would drive its cached
    // value (decision 0021). A boolean line already holds its commanded level.
    {
        std::lock_guard<std::mutex> io(io_mutex_);
        for (std::size_t i = 0; i < states_.size(); ++i) {
            PortState& s = *states_[i];
            if (!s.claimed || !config_.ports[i].pwm) {
                continue;
            }
            bool high = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!s.armed) {
                    continue;
                }
                high = s.value > 0;
            }
            if (!backend_.write(s.line_index, high)) {
                ALPACA_LOG_WARN(config_.log_category,
                                config_.model_name + ": failed to settle PWM port '" + config_.ports[i].name +
                                    "' before release (port may be left in an indeterminate state)");
            } else if (!lines_readable_) {
                // The next open() writes nothing and cannot read the line back, so the cache must
                // report what the line holds now, not the duty.
                std::lock_guard<std::mutex> lock(mutex_);
                s.value = high ? 100 : 0;
            }
        }
        backend_.close();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& s : states_) {
        s->armed = false;
        s->stop = false;
        s->failed = false;
    }
    closing_ = false;
    open_.store(false, std::memory_order_release);
}

int SoftPwmPorts::get_value(std::size_t index) const {
    check_index(index);
    std::lock_guard<std::mutex> lock(mutex_);
    return states_[index]->value;
}

void SoftPwmPorts::set_value(std::size_t index, int value) {
    check_index(index);
    const PowerPort& port = config_.ports[index];
    PortState& s = *states_[index];
    if (!port.writable) {
        throw AlpacaException("Port '" + port.name + "' is read-only", AlpacaError::NotImplemented);
    }
    if (port.pwm ? (value < 0 || value > 100) : (value != 0 && value != 1)) {
        throw AlpacaException(port.pwm ? "PWM value out of range [0,100]" : "Boolean value must be 0 or 1",
                              AlpacaError::InvalidValue);
    }
    const std::string not_open = config_.model_name + " GPIO device is not open";
    if (port.pwm) {
        // Hand the duty to the worker, which reads it on every cycle.
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_.load() || closing_) {
            throw AlpacaException(not_open, AlpacaError::NotConnected);
        }
        if (s.failed) {
            throw AlpacaException(config_.model_name + " PWM worker for '" + port.name +
                                      "' stopped after a failed write; reconnect to restart it",
                                  AlpacaError::DriverException);
        }
        s.value = value;
        s.armed = true;
        s.cv.notify_all();
        return;
    }
    // io_mutex_ is held from the open check to the write, so close() cannot release the lines between them.
    std::lock_guard<std::mutex> io(io_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_.load() || closing_) {
            throw AlpacaException(not_open, AlpacaError::NotConnected);
        }
    }
    if (!backend_.write(s.line_index, value != 0)) {
        throw AlpacaException(config_.model_name + " GPIO write failed on port '" + port.name + "'",
                              AlpacaError::DriverException);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    s.value = value;
}

bool SoftPwmPorts::write_line(std::size_t index, bool high) {
    std::lock_guard<std::mutex> io(io_mutex_);
    return backend_.write(states_[index]->line_index, high);
}

void SoftPwmPorts::pwm_loop(std::size_t index) {
    PortState& s = *states_[index];
    std::unique_lock<std::mutex> lock(mutex_);
    // The level the line holds, -1 until known: the one open() read, or unknown. Not derived from the duty
    // here: a set_value() may have changed it before this thread first ran.
    int last_level = s.opened_level;
    std::optional<TaskClock::clock::time_point> next_edge;  // the start of the next period, while cycling
    // Drives the line without the state lock. False after the worker's last word: the port is marked failed
    // (set_value reports it), then the failure is logged outside the lock.
    const auto drive = [&](bool high) {
        lock.unlock();
        const bool ok = write_line(index, high);
        lock.lock();
        if (ok) {
            return true;
        }
        s.failed = true;
        lock.unlock();
        ALPACA_LOG_ERROR(config_.log_category, config_.model_name + ": write to port '" + config_.ports[index].name +
                                                   "' failed; stopping its PWM worker");
        lock.lock();
        return false;
    };
    const auto stopping = [&s] { return s.stop; };
    // Waits for @p deadline on the clock; true when asked to stop first.
    const auto wait_until = [&](TaskClock::clock::time_point deadline) {
        return clock_.wait_for(lock, s.cv, deadline - clock_.now(), stopping);
    };

    while (!s.stop) {
        if (!s.armed) {
            next_edge.reset();
            s.cv.wait(lock, [&s] { return s.stop || s.armed; });
            continue;
        }
        const int duty = s.value;
        if (duty <= 0 || duty >= 100) {
            // Steady: one write, then nothing until the duty changes.
            next_edge.reset();
            const int level = duty >= 100 ? 1 : 0;
            if (level != last_level) {
                if (!drive(level != 0)) {
                    return;
                }
                last_level = level;
            }
            s.cv.wait(lock, [&s, duty] { return s.stop || s.value != duty; });
            continue;
        }
        // Edges are absolute (the sleep_until of the rule), so the latency of a write does not drift the
        // duty; a worker that fell a whole period behind restarts at now instead of bursting to catch up.
        const auto now = clock_.now();
        auto start = next_edge.value_or(now);
        if (now - start > period_) {
            start = now;
        }
        const auto on = period_ * duty / 100;
        if (!drive(true)) {
            return;
        }
        last_level = 1;
        if (wait_until(start + on)) {
            break;
        }
        if (!drive(false)) {
            return;
        }
        last_level = 0;
        next_edge = start + period_;
        if (wait_until(start + period_)) {
            break;
        }
    }
}

}  // namespace alpacacore::util
