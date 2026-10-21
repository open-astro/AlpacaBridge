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
#include <alpacacore/vendor/ioptron/ioptron_powerbox_wrapper.h>
#include <gpiod.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace alpacacore::vendor::ioptron {

namespace {

constexpr const char* kLogCategory = "IOPTRON_POWERBOX";
constexpr const char* kGpioConsumer = "alpacabridge-imate-powerbox";

gpiod_line_value to_line_value(int v) { return v != 0 ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE; }

// Production GPIO side: libgpiod v2, one request owning every claimed line.
class LibgpiodBackend final : public IoptronGpioBackend {
public:
    LibgpiodBackend() = default;
    ~LibgpiodBackend() { drop(); }

    void request(const std::string& chip_path, const std::vector<unsigned int>& offsets,
                 const std::vector<int>& initial_values) override {
        std::vector<gpiod_line_value> values;
        values.reserve(initial_values.size());
        for (int v : initial_values) {
            values.push_back(to_line_value(v));
        }
        chip_ = ::gpiod_chip_open(chip_path.c_str());
        if (chip_ == nullptr) {
            int err = errno;
            throw AlpacaException("Failed to open GPIO chip '" + chip_path + "': " + util::errno_string(err),
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
                throw AlpacaException("gpiod_line_settings_set_direction failed: " + util::errno_string(err),
                                      AlpacaError::DriverException);
            }

            line_cfg = ::gpiod_line_config_new();
            if (line_cfg == nullptr) {
                throw AlpacaException("gpiod_line_config_new failed", AlpacaError::DriverException);
            }
            if (::gpiod_line_config_add_line_settings(line_cfg, offsets.data(), offsets.size(), settings) != 0) {
                int err = errno;
                throw AlpacaException("gpiod_line_config_add_line_settings failed: " + util::errno_string(err),
                                      AlpacaError::DriverException);
            }
            if (::gpiod_line_config_set_output_values(line_cfg, values.data(), values.size()) != 0) {
                int err = errno;
                throw AlpacaException("gpiod_line_config_set_output_values failed: " + util::errno_string(err),
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
                throw AlpacaException("gpiod_chip_request_lines failed: " + util::errno_string(err),
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
    }

    int set_value(unsigned int offset, int value) override {
        return ::gpiod_line_request_set_value(request_, offset, to_line_value(value));
    }

    void release() override { drop(); }

private:
    void drop() {
        if (request_ != nullptr) {
            ::gpiod_line_request_release(request_);
            request_ = nullptr;
        }
        if (chip_ != nullptr) {
            ::gpiod_chip_close(chip_);
            chip_ = nullptr;
        }
    }

    gpiod_chip* chip_ = nullptr;
    gpiod_line_request* request_ = nullptr;
};

}  // namespace

class IoptronPowerboxWrapper::Impl {
public:
    Impl(std::string gpio_chip_path, std::vector<IoptronPowerPortConfig> ports, std::uint32_t pwm_frequency_hz,
         IoptronGpioBackend* backend)
        : gpio_chip_path_(std::move(gpio_chip_path)),
          ports_(std::move(ports)),
          pwm_frequency_hz_(pwm_frequency_hz),
          gpio_(backend != nullptr ? *backend : default_gpio_),
          open_(false) {
        if (ports_.empty()) {
            throw AlpacaException("iMate PowerBox port configuration is empty", AlpacaError::InvalidValue);
        }
        // The GPIO chip is a libgpiod character-device node; it must be an
        // absolute /dev/ path. Reject anything else up front (empty string,
        // relative path, a bare "gpiochip0") with a clear InvalidValue rather
        // than letting it reach gpiod_chip_open() as an opaque failure.
        if (gpio_chip_path_.rfind("/dev/", 0) != 0) {
            throw AlpacaException(
                "iMate PowerBox GPIO chip path must be an absolute /dev/ device node (got '" + gpio_chip_path_ + "')",
                AlpacaError::InvalidValue);
        }
        if (pwm_frequency_hz_ == 0 || pwm_frequency_hz_ > 100000) {
            throw AlpacaException("iMate PowerBox PWM frequency out of supported range (1..100000 Hz)",
                                  AlpacaError::InvalidValue);
        }
        // The cached value mirrors the level the GPIO is (or will be) driven
        // to. Controllable ports default to "on" so connecting preserves the
        // stock boot state where the iMate drives the DC lines high — we never
        // want a connect to cut power to attached gear. For a PWM port "on" is
        // full duty (100); for a boolean port it is 1. The always-on
        // pass-through reports 1 and has no backing line.
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
            ALPACA_LOG_WARN(kLogCategory, std::string("Error during iMate PowerBox wrapper destruction: ") + e.what());
        }
    }

    void open() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (open_) {
            return;
        }

        // Collect the offsets/initial values for the ports that actually have
        // a GPIO line. The always-on pass-through is skipped entirely. PWM
        // ports start at full-on (high); the worker takes over once requested.
        std::vector<unsigned int> offsets;
        std::vector<int> initial_values;
        offsets.reserve(ports_.size());
        initial_values.reserve(ports_.size());
        for (std::size_t i = 0; i < ports_.size(); ++i) {
            if (!ports_[i].has_line) {
                continue;
            }
            offsets.push_back(ports_[i].gpio_line);
            const int v = port_states_[i]->value.load();
            initial_values.push_back(ports_[i].pwm_enabled ? 1 : (v != 0 ? 1 : 0));
        }

        if (offsets.empty()) {
            // No controllable lines (pass-through only); nothing to request,
            // but the device is still "connected" for client purposes.
            open_.store(true, std::memory_order_release);
            return;
        }

        gpio_.request(gpio_chip_path_, offsets, initial_values);
        lines_claimed_ = true;

        open_.store(true, std::memory_order_release);

        for (std::size_t i = 0; i < ports_.size(); ++i) {
            if (ports_[i].has_line && ports_[i].pwm_enabled) {
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
        if (lines_claimed_) {
            // Preserve-power policy: a PWM port left mid-cycle could strand its
            // line low (cutting power on disconnect), so drive each PWM line to
            // a defined steady level matching its duty before releasing — duty
            // > 0 stays on, duty == 0 stays off. Boolean ports already hold
            // their last commanded level. We never drive a line low here for a
            // port the user left on.
            for (std::size_t i = 0; i < ports_.size(); ++i) {
                if (ports_[i].has_line && ports_[i].pwm_enabled) {
                    const int d = port_states_[i]->value.load();
                    int rc;
                    {
                        std::lock_guard<std::mutex> io(io_mutex_);
                        rc = gpio_.set_value(ports_[i].gpio_line, d > 0 ? 1 : 0);
                    }
                    if (rc != 0) {
                        const int err = errno;
                        ALPACA_LOG_WARN(kLogCategory, "iMate PowerBox: failed to settle PWM port on GPIO " +
                                                          std::to_string(ports_[i].gpio_line) +
                                                          " before release: " + util::errno_string(err) +
                                                          " (port may be left in an indeterminate state)");
                    }
                }
            }
            gpio_.release();
            lines_claimed_ = false;
        }
        open_.store(false, std::memory_order_release);
    }

    // Lock-free on purpose (issue #382): the driver answers get_connected()
    // with this, and alpaca-http-conformance.instructions.md wants that getter cheap. open() holds mutex_
    // for its whole sequence and close() holds it for its locked phases
    // (it drops the lock to join the PWM workers), so a locked read here
    // waited behind whichever phase was running. open_ is written only
    // inside those critical sections and published as an atomic; the
    // snapshot is momentary, which is what a connected-state read is anyway.
    bool is_open() const { return open_.load(std::memory_order_acquire); }

    std::size_t port_count() const { return ports_.size(); }

    const IoptronPowerPortConfig& port_config(std::size_t index) const {
        validate_index(index);
        return ports_[index];
    }

    int get_value(std::size_t index) const {
        validate_index(index);
        // The pass-through jack is hardwired live regardless of driver state.
        if (!ports_[index].has_line) {
            return 1;
        }
        return port_states_[index]->value.load();
    }

    void set_value(std::size_t index, int value) {
        validate_index(index);
        const auto& cfg = ports_[index];
        if (!cfg.writable || !cfg.has_line) {
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
            rc = gpio_.set_value(cfg.gpio_line, value);
        }
        if (rc != 0) {
            int err = errno;
            throw AlpacaException("gpiod_line_request_set_value failed: " + util::errno_string(err),
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
        IoptronGpioBackend* gpio = &gpio_;
        std::atomic<int>* duty = &state.value;
        std::atomic<bool>* stop = &state.stop;
        std::mutex* io_mutex = &io_mutex_;
        state.worker = std::thread([gpio, offset, duty, stop, period_us, io_mutex]() {
            // Every line write goes through io_mutex (held only around the
            // ioctl, never across a sleep) so PWM workers and the boolean
            // set_value path never write the shared gpiod_line_request
            // concurrently.
            auto locked_set = [&](gpiod_line_value v) -> int {
                std::lock_guard<std::mutex> io(*io_mutex);
                return gpio->set_value(offset, v == GPIOD_LINE_VALUE_ACTIVE ? 1 : 0);
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
                                     "iMate PowerBox PWM worker: gpiod_line_request_set_value failed on GPIO " +
                                         std::to_string(offset) + ": " + util::errno_string(err) +
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
                                     "iMate PowerBox PWM worker: gpiod_line_request_set_value(ACTIVE) failed on GPIO " +
                                         std::to_string(offset) + ": " + util::errno_string(err) +
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
                    ALPACA_LOG_ERROR(
                        kLogCategory,
                        "iMate PowerBox PWM worker: gpiod_line_request_set_value(INACTIVE) failed on GPIO " +
                            std::to_string(offset) + ": " + util::errno_string(err) + "; aborting worker thread");
                    stop->store(true);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(off_us));
            }
        });
    }

    const std::string gpio_chip_path_;
    const std::vector<IoptronPowerPortConfig> ports_;
    const std::uint32_t pwm_frequency_hz_;

    mutable std::mutex mutex_;
    // Serialises every gpiod_line_request_set_value call against the shared
    // gpiod_line_request — boolean writes, per-port PWM worker writes, and the
    // close-time settle writes — since libgpiod v2 does not document the
    // request as safe for concurrent writers and a mixed boolean+PWM config
    // issues writes from a worker and a client thread at once. Held only around
    // the ioctl, never across a PWM sleep.
    std::mutex io_mutex_;
    LibgpiodBackend default_gpio_;
    IoptronGpioBackend& gpio_;    // default_gpio_ or the caller's; touched only under mutex_/io_mutex_
    bool lines_claimed_ = false;  // under mutex_
    std::atomic<bool> open_;  // see is_open()
    std::vector<std::unique_ptr<PortState>> port_states_;
};

IoptronPowerboxWrapper::IoptronPowerboxWrapper(std::string gpio_chip_path, std::vector<IoptronPowerPortConfig> ports,
                                               std::uint32_t pwm_frequency_hz, IoptronGpioBackend* backend)
    : impl_(std::make_unique<Impl>(std::move(gpio_chip_path), std::move(ports), pwm_frequency_hz, backend)) {}

IoptronPowerboxWrapper::~IoptronPowerboxWrapper() = default;

void IoptronPowerboxWrapper::open() { impl_->open(); }
void IoptronPowerboxWrapper::close() { impl_->close(); }
bool IoptronPowerboxWrapper::is_open() const { return impl_->is_open(); }

std::size_t IoptronPowerboxWrapper::port_count() const { return impl_->port_count(); }
const IoptronPowerPortConfig& IoptronPowerboxWrapper::port_config(std::size_t index) const {
    return impl_->port_config(index);
}
int IoptronPowerboxWrapper::get_value(std::size_t index) const { return impl_->get_value(index); }
void IoptronPowerboxWrapper::set_value(std::size_t index, int value) { impl_->set_value(index, value); }
std::string IoptronPowerboxWrapper::gpio_chip_path() const { return impl_->gpio_chip_path(); }
std::uint32_t IoptronPowerboxWrapper::pwm_frequency_hz() const { return impl_->pwm_frequency_hz(); }

}  // namespace alpacacore::vendor::ioptron
