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

#include <alpacacore/vendor/ioptron/ioptron_powerbox_wrapper.h>

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>

#include <gpiod.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace alpacacore::vendor::ioptron {

namespace {

constexpr const char* kLogCategory = "IOPTRON_POWERBOX";
constexpr const char* kGpioConsumer = "alpacabridge-imate-powerbox";

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

gpiod_line_value to_line_value(int v) {
    return v != 0 ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
}

} // namespace

class IoptronPowerboxWrapper::Impl {
public:
    Impl(std::string gpio_chip_path, std::vector<IoptronPowerPortConfig> ports)
        : gpio_chip_path_(std::move(gpio_chip_path))
        , ports_(std::move(ports))
        , chip_(nullptr)
        , request_(nullptr)
        , open_(false)
    {
        if (ports_.empty()) {
            throw AlpacaException("iMate PowerBox port configuration is empty",
                                  AlpacaError::InvalidValue);
        }
        // The cached value mirrors the level the GPIO is (or will be) driven
        // to. Controllable ports default to "on" (1) so connecting preserves
        // the stock boot state where the iMate drives the DC lines high — we
        // never want a connect to cut power to attached gear. The always-on
        // pass-through reports 1 and has no backing line.
        port_values_.reserve(ports_.size());
        for (std::size_t i = 0; i < ports_.size(); ++i) {
            port_values_.emplace_back(std::make_unique<std::atomic<int>>(1));
        }
    }

    ~Impl() {
        try {
            close();
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN(kLogCategory,
                            std::string("Error during iMate PowerBox wrapper destruction: ") +
                                e.what());
        }
    }

    void open() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (open_) {
            return;
        }

        // Collect the offsets/initial values for the ports that actually have
        // a GPIO line. The always-on pass-through is skipped entirely.
        std::vector<unsigned int> offsets;
        std::vector<gpiod_line_value> initial_values;
        offsets.reserve(ports_.size());
        initial_values.reserve(ports_.size());
        for (std::size_t i = 0; i < ports_.size(); ++i) {
            if (!ports_[i].has_line) {
                continue;
            }
            offsets.push_back(ports_[i].gpio_line);
            initial_values.push_back(to_line_value(port_values_[i]->load()));
        }

        if (offsets.empty()) {
            // No controllable lines (pass-through only); nothing to request,
            // but the device is still "connected" for client purposes.
            open_ = true;
            return;
        }

        chip_ = ::gpiod_chip_open(gpio_chip_path_.c_str());
        if (chip_ == nullptr) {
            int err = errno;
            throw AlpacaException("Failed to open GPIO chip '" + gpio_chip_path_ +
                                      "': " + strerror_safe(err),
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
            if (::gpiod_line_config_add_line_settings(line_cfg,
                                                      offsets.data(),
                                                      offsets.size(),
                                                      settings) != 0) {
                int err = errno;
                throw AlpacaException("gpiod_line_config_add_line_settings failed: " +
                                          strerror_safe(err),
                                      AlpacaError::DriverException);
            }
            if (::gpiod_line_config_set_output_values(line_cfg,
                                                      initial_values.data(),
                                                      initial_values.size()) != 0) {
                int err = errno;
                throw AlpacaException("gpiod_line_config_set_output_values failed: " +
                                          strerror_safe(err),
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
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) {
            return;
        }
        if (request_ != nullptr) {
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
        return port_values_[index]->load();
    }

    void set_value(std::size_t index, int value) {
        validate_index(index);
        const auto& cfg = ports_[index];
        if (!cfg.writable || !cfg.has_line) {
            throw AlpacaException("Port '" + cfg.name + "' is read-only",
                                  AlpacaError::NotImplemented);
        }
        if (value != 0 && value != 1) {
            throw AlpacaException("Boolean value must be 0 or 1", AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) {
            throw AlpacaException("GPIO chip is not open", AlpacaError::NotConnected);
        }
        if (::gpiod_line_request_set_value(request_, cfg.gpio_line, to_line_value(value)) != 0) {
            int err = errno;
            throw AlpacaException("gpiod_line_request_set_value failed: " + strerror_safe(err),
                                  AlpacaError::DriverException);
        }
        port_values_[index]->store(value);
    }

    std::string gpio_chip_path() const { return gpio_chip_path_; }

private:
    void validate_index(std::size_t index) const {
        if (index >= ports_.size()) {
            throw AlpacaException("Port index out of range", AlpacaError::InvalidValue);
        }
    }

    const std::string gpio_chip_path_;
    const std::vector<IoptronPowerPortConfig> ports_;

    mutable std::mutex mutex_;
    gpiod_chip* chip_;
    gpiod_line_request* request_;
    bool open_;
    std::vector<std::unique_ptr<std::atomic<int>>> port_values_;
};

IoptronPowerboxWrapper::IoptronPowerboxWrapper(std::string gpio_chip_path,
                                               std::vector<IoptronPowerPortConfig> ports)
    : impl_(std::make_unique<Impl>(std::move(gpio_chip_path), std::move(ports)))
{}

IoptronPowerboxWrapper::~IoptronPowerboxWrapper() = default;

void IoptronPowerboxWrapper::open() { impl_->open(); }
void IoptronPowerboxWrapper::close() { impl_->close(); }
bool IoptronPowerboxWrapper::is_open() const { return impl_->is_open(); }

std::size_t IoptronPowerboxWrapper::port_count() const { return impl_->port_count(); }
const IoptronPowerPortConfig& IoptronPowerboxWrapper::port_config(std::size_t index) const {
    return impl_->port_config(index);
}
int IoptronPowerboxWrapper::get_value(std::size_t index) const { return impl_->get_value(index); }
void IoptronPowerboxWrapper::set_value(std::size_t index, int value) {
    impl_->set_value(index, value);
}
std::string IoptronPowerboxWrapper::gpio_chip_path() const { return impl_->gpio_chip_path(); }

} // namespace alpacacore::vendor::ioptron
