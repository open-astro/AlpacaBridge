// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply
// with all SSPL v1 requirements.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/touptek/touptek_powerbox_wrapper.h>
#include <gpiod.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace alpacacore::vendor::touptek {

namespace {

constexpr const char* kLogCategory = "TOUPTEK_STELLAVITA";
constexpr const char* kGpioConsumer = "alpacabridge-stellavita-powerbox";

std::string strerror_safe(int err) {
    char buf[128]{};
#if defined(_GNU_SOURCE)
    return std::string(::strerror_r(err, buf, sizeof(buf)));
#else
    if (::strerror_r(err, buf, sizeof(buf)) != 0) {
        return std::to_string(err);
    }
    return std::string(buf);
#endif
}

gpiod_line_value to_line_value(int v) { return v != 0 ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE; }

}  // namespace

class TouptekPowerboxWrapper::Impl {
public:
    Impl(std::string gpio_chip_path, std::vector<StellaVitaPortConfig> ports, std::uint32_t pwm_frequency_hz)
        : gpio_chip_path_(std::move(gpio_chip_path)),
          ports_(std::move(ports)),
          pwm_frequency_hz_(pwm_frequency_hz),
          chip_(nullptr),
          request_(nullptr),
          open_(false) {
        if (ports_.empty()) {
            throw AlpacaException("StellaVita port configuration is empty", AlpacaError::InvalidValue);
        }
        // The GPIO chip is a libgpiod character-device node; it must be an
        // absolute /dev/ path. Reject anything else up front (empty string,
        // relative path, a bare "gpiochip0") with a clear InvalidValue rather
        // than letting it reach gpiod_chip_open() as an opaque failure.
        if (gpio_chip_path_.rfind("/dev/", 0) != 0) {
            throw AlpacaException(
                "StellaVita GPIO chip path must be an absolute /dev/ device node (got '" + gpio_chip_path_ + "')",
                AlpacaError::InvalidValue);
        }
        if (pwm_frequency_hz_ == 0 || pwm_frequency_hz_ > 100000) {
            throw AlpacaException("StellaVita PWM frequency out of supported range (1..100000 Hz)",
                                  AlpacaError::InvalidValue);
        }
        // The cached value mirrors the level the GPIO is (or will be) driven
        // to. Ports default to "on" so connecting preserves the StellaVita's
        // stock boot state where config.txt drives the DC lines high — we never
        // want a connect to cut power to attached gear. For a PWM port "on" is
        // full duty (100); for a boolean port it is 1.
        port_states_.reserve(ports_.size());
        for (std::size_t i = 0; i < ports_.size(); ++i) {
            port_states_.emplace_back(std::make_unique<PortState>());
            port_states_[i]->value.store(ports_[i].pwm_enabled ? 100 : 1);
        }
    }

    ~Impl() {
        try {
            close();
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN(kLogCategory, std::string("Error during StellaVita wrapper destruction: ") + e.what());
        }
    }

    void open() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (open_) {
            return;
        }

        // Collect the offsets/initial values for every port. PWM ports start at
        // full-on (high); the worker takes over once requested.
        std::vector<unsigned int> offsets;
        std::vector<gpiod_line_value> initial_values;
        offsets.reserve(ports_.size());
        initial_values.reserve(ports_.size());
        for (std::size_t i = 0; i < ports_.size(); ++i) {
            offsets.push_back(ports_[i].gpio_line);
            const int v = port_states_[i]->value.load();
            initial_values.push_back(to_line_value(ports_[i].pwm_enabled ? 1 : v));
        }

        chip_ = ::gpiod_chip_open(gpio_chip_path_.c_str());
        if (chip_ == nullptr) {
            int err = errno;
            throw AlpacaException("Failed to open GPIO chip '" + gpio_chip_path_ + "': " + strerror_safe(err),
                                  AlpacaError::NotConnected);
        }

        gpiod_line_settings* settings = nullptr;
        gpiod_line_config* line_cfg = nullptr;
        gpiod_request_config* req_cfg = nullptr;

        try {
            settings = ::gpiod_line_settings_new();
            if (settings == nullptr) {
                throw AlpacaException("gpiod_line_settings_new failed", AlpacaError::DriverException);
            }
            if (::gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT) != 0) {
                int err = errno;
                throw AlpacaException("gpiod_line_settings_set_direction failed: " + strerror_safe(err),
                                      AlpacaError::DriverException);
            }

            line_cfg = ::gpiod_line_config_new();
            if (line_cfg == nullptr) {
                throw AlpacaException("gpiod_line_config_new failed", AlpacaError::DriverException);
            }
            if (::gpiod_line_config_add_line_settings(line_cfg, offsets.data(), offsets.size(), settings) != 0) {
                int err = errno;
                throw AlpacaException("gpiod_line_config_add_line_settings failed: " + strerror_safe(err),
                                      AlpacaError::DriverException);
            }
            if (::gpiod_line_config_set_output_values(line_cfg, initial_values.data(), initial_values.size()) != 0) {
                int err = errno;
                throw AlpacaException("gpiod_line_config_set_output_values failed: " + strerror_safe(err),
                                      AlpacaError::DriverException);
            }

            req_cfg = ::gpiod_request_config_new();
            if (req_cfg == nullptr) {
                throw AlpacaException("gpiod_request_config_new failed", AlpacaError::DriverException);
            }
            ::gpiod_request_config_set_consumer(req_cfg, kGpioConsumer);

            request_ = ::gpiod_chip_request_lines(chip_, req_cfg, line_cfg);
            if (request_ == nullptr) {
                int err = errno;
                throw AlpacaException("gpiod_chip_request_lines failed: " + strerror_safe(err),
                                      AlpacaError::NotConnected);
            }
        } catch (...) {
            if (req_cfg) ::gpiod_request_config_free(req_cfg);
            if (line_cfg) ::gpiod_line_config_free(line_cfg);
            if (settings) ::gpiod_line_settings_free(settings);
            ::gpiod_chip_close(chip_);
            chip_ = nullptr;
            throw;
        }

        ::gpiod_request_config_free(req_cfg);
        ::gpiod_line_config_free(line_cfg);
        ::gpiod_line_settings_free(settings);

        open_ = true;

        for (std::size_t i = 0; i < ports_.size(); ++i) {
            if (ports_[i].pwm_enabled) {
                start_pwm_worker_locked(i);
            }
        }
    }

    void close() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!open_) {
            return;
        }
        for (auto& ps : port_states_) {
            ps->stop.store(true);
        }
        lock.unlock();

        for (auto& ps : port_states_) {
            if (ps->worker.joinable()) {
                ps->worker.join();
            }
        }

        lock.lock();
        // Re-arm the stop flags so a subsequent open() can start fresh PWM
        // workers — the wrapper supports close-then-reopen.
        for (auto& ps : port_states_) {
            ps->stop.store(false);
        }
        if (request_ != nullptr) {
            // Preserve-power policy: a PWM port left mid-cycle could strand its
            // line low (cutting power on disconnect), so drive each PWM line to
            // a defined steady level matching its duty before releasing — duty
            // > 0 stays on, duty == 0 stays off. Boolean ports already hold
            // their last commanded level. We never drive a line low here for a
            // port the user left on.
            for (std::size_t i = 0; i < ports_.size(); ++i) {
                if (ports_[i].pwm_enabled) {
                    const int d = port_states_[i]->value.load();
                    int rc;
                    {
                        std::lock_guard<std::mutex> io(io_mutex_);
                        rc =
                            ::gpiod_line_request_set_value(request_, ports_[i].gpio_line, to_line_value(d > 0 ? 1 : 0));
                    }
                    if (rc != 0) {
                        const int err = errno;
                        ALPACA_LOG_WARN(kLogCategory, "StellaVita: failed to settle PWM port on GPIO " +
                                                          std::to_string(ports_[i].gpio_line) +
                                                          " before release: " + strerror_safe(err) +
                                                          " (port may be left in an indeterminate state)");
                    }
                }
            }
            ::gpiod_line_request_release(request_);
            request_ = nullptr;
        }
        if (chip_ != nullptr) {
            ::gpiod_chip_close(chip_);
            chip_ = nullptr;
        }
        open_ = false;
    }

    bool is_open() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return open_;
    }

    std::size_t port_count() const { return ports_.size(); }

    const StellaVitaPortConfig& port_config(std::size_t index) const {
        validate_index(index);
        return ports_[index];
    }

    int get_value(std::size_t index) const {
        validate_index(index);
        return port_states_[index]->value.load();
    }

    void set_value(std::size_t index, int value) {
        validate_index(index);
        const auto& cfg = ports_[index];
        if (!cfg.writable) {
            throw AlpacaException("Port '" + cfg.name + "' is read-only", AlpacaError::NotImplemented);
        }
        if (cfg.pwm_enabled) {
            if (value < 0 || value > 100) {
                throw AlpacaException("PWM value out of range [0,100]", AlpacaError::InvalidValue);
            }
            // Mirror the boolean path's connection guard so the wrapper is
            // self-contained (the driver also checks, but a direct caller
            // shouldn't be able to set a duty while disconnected).
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_) {
                throw AlpacaException("GPIO chip is not open", AlpacaError::NotConnected);
            }
            // The worker thread observes the new duty on its next cycle; no
            // direct line write here.
            port_states_[index]->value.store(value);
            return;
        }
        if (value != 0 && value != 1) {
            throw AlpacaException("Boolean value must be 0 or 1", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) {
            throw AlpacaException("GPIO chip is not open", AlpacaError::NotConnected);
        }
        int rc;
        {
            std::lock_guard<std::mutex> io(io_mutex_);
            rc = ::gpiod_line_request_set_value(request_, cfg.gpio_line, to_line_value(value));
        }
        if (rc != 0) {
            int err = errno;
            throw AlpacaException("gpiod_line_request_set_value failed: " + strerror_safe(err),
                                  AlpacaError::DriverException);
        }
        port_states_[index]->value.store(value);
    }

    const std::string& gpio_chip_path() const { return gpio_chip_path_; }
    std::uint32_t pwm_frequency_hz() const { return pwm_frequency_hz_; }

private:
    struct PortState {
        std::atomic<int> value{0};
        std::atomic<bool> stop{false};
        std::thread worker;
    };

    void validate_index(std::size_t index) const {
        if (index >= ports_.size()) {
            throw AlpacaException("Port index out of range", AlpacaError::InvalidValue);
        }
    }

    void start_pwm_worker_locked(std::size_t index) {
        auto& state = *port_states_[index];
        const std::uint32_t period_us = 1000000u / pwm_frequency_hz_;
        const unsigned int offset = ports_[index].gpio_line;
        gpiod_line_request* request = request_;
        std::atomic<int>* duty = &state.value;
        std::atomic<bool>* stop = &state.stop;
        std::mutex* io_mutex = &io_mutex_;
        state.worker = std::thread([request, offset, duty, stop, period_us, io_mutex]() {
            // Every line write goes through io_mutex (held only around the
            // ioctl, never across a sleep) so PWM workers and the boolean
            // set_value path never write the shared gpiod_line_request
            // concurrently.
            auto locked_set = [&](gpiod_line_value v) -> int {
                std::lock_guard<std::mutex> io(*io_mutex);
                return ::gpiod_line_request_set_value(request, offset, v);
            };
            // Cache the last value driven onto the line so steady-state 0% /
            // 100% duty cycles skip the kernel ioctl (otherwise we'd write the
            // same value every period — 1000 wasted syscalls/s/port at 1 kHz).
            // -1 = nothing written yet; force the first write.
            int last_written = -1;
            auto write_line = [&](gpiod_line_value v) -> bool {
                const int desired = (v == GPIOD_LINE_VALUE_ACTIVE) ? 1 : 0;
                if (desired == last_written) {
                    return true;
                }
                if (locked_set(v) != 0) {
                    const int err = errno;
                    ALPACA_LOG_ERROR(kLogCategory,
                                     "StellaVita PWM worker: gpiod_line_request_set_value failed on GPIO " +
                                         std::to_string(offset) + ": " + strerror_safe(err) +
                                         "; aborting worker thread");
                    stop->store(true);
                    return false;
                }
                last_written = desired;
                return true;
            };
            while (!stop->load()) {
                const int d = duty->load();
                if (d <= 0) {
                    if (!write_line(GPIOD_LINE_VALUE_INACTIVE)) break;
                    std::this_thread::sleep_for(std::chrono::microseconds(period_us));
                    continue;
                }
                if (d >= 100) {
                    if (!write_line(GPIOD_LINE_VALUE_ACTIVE)) break;
                    std::this_thread::sleep_for(std::chrono::microseconds(period_us));
                    continue;
                }
                const std::uint32_t on_us =
                    static_cast<std::uint32_t>((static_cast<std::uint64_t>(d) * period_us) / 100u);
                const std::uint32_t off_us = period_us - on_us;
                // Mid-duty: cache is invalidated because we toggle every period.
                last_written = -1;
                if (locked_set(GPIOD_LINE_VALUE_ACTIVE) != 0) {
                    const int err = errno;
                    ALPACA_LOG_ERROR(kLogCategory,
                                     "StellaVita PWM worker: gpiod_line_request_set_value(ACTIVE) failed on GPIO " +
                                         std::to_string(offset) + ": " + strerror_safe(err) +
                                         "; aborting worker thread");
                    stop->store(true);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(on_us));
                if (stop->load()) {
                    break;
                }
                if (locked_set(GPIOD_LINE_VALUE_INACTIVE) != 0) {
                    const int err = errno;
                    ALPACA_LOG_ERROR(kLogCategory,
                                     "StellaVita PWM worker: gpiod_line_request_set_value(INACTIVE) failed on GPIO " +
                                         std::to_string(offset) + ": " + strerror_safe(err) +
                                         "; aborting worker thread");
                    stop->store(true);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(off_us));
            }
        });
    }

    const std::string gpio_chip_path_;
    const std::vector<StellaVitaPortConfig> ports_;
    const std::uint32_t pwm_frequency_hz_;

    mutable std::mutex mutex_;
    // Serialises every gpiod_line_request_set_value call against the shared
    // gpiod_line_request — boolean writes, per-port PWM worker writes, and the
    // close-time settle writes. libgpiod v2 does not document the request as
    // safe for concurrent writers, and a mixed config (one PWM port + one
    // boolean port) issues writes from a worker thread and a client thread at
    // the same time. This is a dedicated, tightly-scoped lock (held only around
    // the ioctl, never across a PWM sleep) so it never blocks the duty loop.
    std::mutex io_mutex_;
    gpiod_chip* chip_;
    gpiod_line_request* request_;
    bool open_;
    std::vector<std::unique_ptr<PortState>> port_states_;
};

TouptekPowerboxWrapper::TouptekPowerboxWrapper(std::string gpio_chip_path, std::vector<StellaVitaPortConfig> ports,
                                               std::uint32_t pwm_frequency_hz)
    : impl_(std::make_unique<Impl>(std::move(gpio_chip_path), std::move(ports), pwm_frequency_hz)) {}

TouptekPowerboxWrapper::~TouptekPowerboxWrapper() = default;

void TouptekPowerboxWrapper::open() { impl_->open(); }
void TouptekPowerboxWrapper::close() { impl_->close(); }
bool TouptekPowerboxWrapper::is_open() const { return impl_->is_open(); }

std::size_t TouptekPowerboxWrapper::port_count() const { return impl_->port_count(); }
const StellaVitaPortConfig& TouptekPowerboxWrapper::port_config(std::size_t index) const {
    return impl_->port_config(index);
}
int TouptekPowerboxWrapper::get_value(std::size_t index) const { return impl_->get_value(index); }
void TouptekPowerboxWrapper::set_value(std::size_t index, int value) { impl_->set_value(index, value); }
std::string TouptekPowerboxWrapper::gpio_chip_path() const { return impl_->gpio_chip_path(); }
std::uint32_t TouptekPowerboxWrapper::pwm_frequency_hz() const { return impl_->pwm_frequency_hz(); }

}  // namespace alpacacore::vendor::touptek
