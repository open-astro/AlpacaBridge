// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
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

#include <alpacacore/vendor/zwo/zwo_asiair_plus_protocol_wrapper.h>

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>

#include <pwm_gpio.h>  // vendored from external/ZWO/asiair-plus/

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace alpacacore::vendor::zwo {

namespace {

constexpr const char* kLogCategory = "ZWO_ASIAIR_PLUS";

// Kernel ioctl indices reserved by the airplus-gpios device tree node.
// See external/ZWO/asiair-plus/pwm_gpio.h for the full mapping.
constexpr int kKernelIndexMasterEnable = 3;
constexpr int kKernelIndexDcPortBase   = 4;  // DC ports 1..4 at indices 4..7

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

int kernel_index_for(std::size_t wrapper_index) {
    return kKernelIndexDcPortBase + static_cast<int>(wrapper_index);
}

} // namespace

class AsiairPlusProtocolWrapper::Impl {
public:
    Impl(std::string device_path,
         std::vector<AsiairPlusPortConfig> ports,
         std::uint32_t pwm_frequency_hz)
        : device_path_(std::move(device_path))
        , ports_(std::move(ports))
        , pwm_frequency_hz_(pwm_frequency_hz)
        , fd_(-1)
        , open_(false)
    {
        if (ports_.empty()) {
            throw AlpacaException("ASIair Plus port configuration is empty",
                                  AlpacaError::InvalidValue);
        }
        if (ports_.size() > 4) {
            throw AlpacaException("ASIair Plus exposes 4 DC ports; configuration lists more",
                                  AlpacaError::InvalidValue);
        }
        if (pwm_frequency_hz_ == 0 || pwm_frequency_hz_ > 100000) {
            throw AlpacaException(
                "ASIair Plus PWM frequency out of supported range (1..100000 Hz)",
                AlpacaError::InvalidValue);
        }
        port_states_.reserve(ports_.size());
        for (const auto& port : ports_) {
            port_states_.emplace_back(std::make_unique<PortState>());
            // Default to "on" so the user-perceived behavior matches the
            // kernel module's boot-time default-high configuration.
            port_states_.back()->value.store(port.pwm_enabled ? 100 : 1);
        }
    }

    ~Impl() {
        try {
            close();
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN(kLogCategory,
                            std::string("Error during ASIair Plus wrapper destruction: ") + e.what());
        }
    }

    void open() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (open_) {
            return;
        }
        const int fd = ::open(device_path_.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            const int err = errno;
            throw AlpacaException("Failed to open '" + device_path_ + "': " + strerror_safe(err),
                                  AlpacaError::NotConnected);
        }
        fd_ = fd;
        try {
            // Connect-time policy: read-only, do NOT touch any kernel-side
            // state. We learned the hard way (twice) that any write here
            // risks power-cycling gear plugged into the DC ports if our
            // model of the kernel module's semantics is even slightly off:
            //
            //   1. Driving the master-enable line (kernel index 3) to
            //      either polarity caused all four DC ports to physically
            //      go dark on connect, even though GET_LEVEL on the ports
            //      kept reading 1 and our cache reported ON to clients.
            //   2. Calling SET_MODE/SET_LEVEL on the DC ports themselves
            //      at connect time produced the same symptom — likely
            //      because the kernel module's mode-change path has a
            //      brief transient that real gear noticed.
            //
            // The kernel module is opaque (closed source, reverse-
            // engineered headers only) and the inventory comments are
            // ambiguous about polarity. The safe move is to leave whatever
            // boot or previous-client state the ports are in completely
            // alone, and only push writes to the kernel when the ASCOM
            // client actively calls SetSwitch / SetSwitchValue.
            //
            // We do NOT call PWM_GPIO_GET_LEVEL here. Empirically (probed
            // across all 12 kernel indices on a freshly-booted device with
            // physically-powered DC ports and a lit network LED), GET_LEVEL
            // returns 0 for **every** index — including ones that are
            // definitely outputting HIGH. The ioctl appears to read back
            // the kernel module's "last-written via SET_LEVEL" cache, not
            // the actual pin state, and that cache initializes to 0 at
            // module load regardless of what pinctrl drove the pads to.
            //
            // So reading GET_LEVEL into our wrapper's cache would silently
            // flip the UI to "all off" on the first connect after a fresh
            // boot, even though the gear is powered. Better to keep the
            // constructor's optimistic default — value=1 for boolean
            // ports, value=100 for PWM ports — which matches the typical
            // boot-time behavior of all four DC ports being live. If the
            // user has actively toggled a port via this wrapper instance,
            // that set_value() write already updated the cache, so the
            // cache is correct for the lifetime of the wrapper.
            //
            // Cost: if AlpacaBridge restarts mid-session with some ports
            // physically off, the first reconnect will briefly show those
            // ports as ON in the UI until the user toggles them. That's
            // accepted — the alternative is a worse first-impression bug.
        } catch (...) {
            ::close(fd_);
            fd_ = -1;
            throw;
        }
        open_ = true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) {
            return;
        }
        // Mirror the ASIair Pro disconnect policy: leave each port in its
        // last-set state. The kernel module retains the per-port mode +
        // level across file-descriptor opens, so simply releasing our fd
        // does not power-cycle any port.
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        open_ = false;
    }

    bool is_open() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return open_;
    }

    std::size_t port_count() const { return ports_.size(); }

    const AsiairPlusPortConfig& port_config(std::size_t index) const {
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
        if (cfg.pwm_enabled) {
            if (value < 0 || value > 100) {
                throw AlpacaException("PWM value out of range [0,100]",
                                      AlpacaError::InvalidValue);
            }
        } else {
            if (value != 0 && value != 1) {
                throw AlpacaException("Boolean value must be 0 or 1",
                                      AlpacaError::InvalidValue);
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) {
            throw AlpacaException("ASIair Plus device is not open",
                                  AlpacaError::NotConnected);
        }
        apply_port_locked(index, value);
        port_states_[index]->value.store(value);
    }

    std::string device_path() const { return device_path_; }
    std::uint32_t pwm_frequency_hz() const { return pwm_frequency_hz_; }

private:
    struct PortState {
        std::atomic<int> value{0};
    };

    void validate_index(std::size_t index) const {
        if (index >= ports_.size()) {
            throw AlpacaException("Port index out of range", AlpacaError::InvalidValue);
        }
    }

    // Caller must hold mutex_ and have verified fd_ is valid.
    void apply_port_locked(std::size_t index, int value) {
        const int kernel_idx = kernel_index_for(index);
        const auto& cfg = ports_[index];

        // The kernel module reverse-engineering yielded a non-obvious required
        // sequence (proved empirically: /sys/kernel/debug/gpio direction stayed
        // as "in" until PWM_GPIO_ENABLE was called, even after SET_MODE):
        //
        //   1. SET_MODE  — pick GPIO or PWM mode
        //   2. ENABLE    — internally calls gpiod_direction_output_raw, which
        //                  flips the line from input (boot default) to output.
        //                  Without this, SET_LEVEL/SET_CONFIG writes succeed
        //                  silently at the ioctl level but the kernel module
        //                  logs "NOT in GPIO OUTPUT mode" via printk and
        //                  refuses to touch the pad.
        //   3. SET_LEVEL or SET_CONFIG — actually write the value.
        //
        // This applies on EVERY transition (mode change OR re-enabling after a
        // DISABLE). The kernel module symbol table confirms it uses standard
        // gpiod_* APIs (devm_gpio_request_one, gpiod_direction_output_raw,
        // gpiod_set_raw_value, hrtimer_*, pinctrl_select_state) — see
        // AGENTS.md for the full forensic write-up.

        // Every port — including PWM-configured ports — currently uses the
        // boolean GPIO path. The kernel module's PWM mode is broken
        // end-to-end on stock Debian: SET_MODE(PWM) puts the pad into a
        // non-driven state (observed live with a flat-panel load on Port 2,
        // gear physically went dark the moment we sent SET_MODE(PWM)), and
        // subsequent SET_CONFIG calls have no visible effect at any duty
        // cycle even with ENABLE between them. Going through that path
        // would produce inverted-looking behavior in NINA (toggle ON would
        // appear to turn the load off because SET_MODE(PWM) drops the pad).
        // Until the missing piece is identified, PWM-configured ports
        // behave as binary switches with threshold > 0 → on. The persisted
        // config still records the PWM flag, so this auto-upgrades to real
        // PWM as soon as we fix the SET_MODE(PWM) path.
        (void)cfg; // pwm_enabled is intentionally unused on this code path
        work_mode_t mode{};
        mode.index = kernel_idx;
        mode.mode = PWM_GPIO_MODE_GPIO;
        if (::ioctl(fd_, PWM_GPIO_SET_MODE, &mode) != 0) {
            const int err = errno;
            throw AlpacaException(
                "PWM_GPIO_SET_MODE(GPIO) failed on kernel index " +
                    std::to_string(kernel_idx) + ": " + strerror_safe(err),
                AlpacaError::DriverException);
        }
        int enable_arg = kernel_idx;
        if (::ioctl(fd_, PWM_GPIO_ENABLE, &enable_arg) != 0) {
            const int err = errno;
            throw AlpacaException(
                "PWM_GPIO_ENABLE failed on kernel index " +
                    std::to_string(kernel_idx) + ": " + strerror_safe(err),
                AlpacaError::DriverException);
        }
        gpio_level_t level{};
        level.index = kernel_idx;
        level.level = value != 0 ? 1 : 0;
        if (::ioctl(fd_, PWM_GPIO_SET_LEVEL, &level) != 0) {
            const int err = errno;
            throw AlpacaException(
                "PWM_GPIO_SET_LEVEL failed on kernel index " +
                    std::to_string(kernel_idx) + ": " + strerror_safe(err),
                AlpacaError::DriverException);
        }
    }

    const std::string device_path_;
    const std::vector<AsiairPlusPortConfig> ports_;
    const std::uint32_t pwm_frequency_hz_;

    mutable std::mutex mutex_;
    int fd_;
    bool open_;
    std::vector<std::unique_ptr<PortState>> port_states_;
};

AsiairPlusProtocolWrapper::AsiairPlusProtocolWrapper(std::string device_path,
                                                     std::vector<AsiairPlusPortConfig> ports,
                                                     std::uint32_t pwm_frequency_hz)
    : impl_(std::make_unique<Impl>(std::move(device_path),
                                   std::move(ports),
                                   pwm_frequency_hz))
{}

AsiairPlusProtocolWrapper::~AsiairPlusProtocolWrapper() = default;

void AsiairPlusProtocolWrapper::open() { impl_->open(); }
void AsiairPlusProtocolWrapper::close() { impl_->close(); }
bool AsiairPlusProtocolWrapper::is_open() const { return impl_->is_open(); }

std::size_t AsiairPlusProtocolWrapper::port_count() const { return impl_->port_count(); }
const AsiairPlusPortConfig& AsiairPlusProtocolWrapper::port_config(std::size_t index) const {
    return impl_->port_config(index);
}
int AsiairPlusProtocolWrapper::get_value(std::size_t index) const {
    return impl_->get_value(index);
}
void AsiairPlusProtocolWrapper::set_value(std::size_t index, int value) {
    impl_->set_value(index, value);
}
std::string AsiairPlusProtocolWrapper::device_path() const { return impl_->device_path(); }
std::uint32_t AsiairPlusProtocolWrapper::pwm_frequency_hz() const {
    return impl_->pwm_frequency_hz();
}

} // namespace alpacacore::vendor::zwo
