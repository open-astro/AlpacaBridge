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

#include <alpacacore/vendor/zwo/zwo_asiair_plus_protocol_wrapper.h>

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>

#include <pwm_gpio.h>  // vendored from external/ZWO/asiair-plus/

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

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
            throw AlpacaException("ASIAIR Plus port configuration is empty",
                                  AlpacaError::InvalidValue);
        }
        if (ports_.size() > 4) {
            throw AlpacaException("ASIAIR Plus exposes 4 DC ports; configuration lists more",
                                  AlpacaError::InvalidValue);
        }
        if (pwm_frequency_hz_ == 0 || pwm_frequency_hz_ > 100000) {
            throw AlpacaException(
                "ASIAIR Plus PWM frequency out of supported range (1..100000 Hz)",
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
                            std::string("Error during ASIAIR Plus wrapper destruction: ") + e.what());
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

        // Soft-PWM workers: spawn one thread per pwm_enabled port. Each
        // thread runs pwm_loop() which (a) initializes the kernel-side port
        // for GPIO output on its first iteration, (b) drives SET_LEVEL at
        // the configured frequency with the requested duty cycle, and
        // (c) idles cheaply when the duty is 0 or 100. The thread will
        // briefly contend on mutex_ to issue each ioctl — the constructor
        // exits this critical section before the thread can acquire it,
        // so there's no startup deadlock. Boolean ports get no thread;
        // their writes still go through apply_port_locked() synchronously
        // from set_value().
        for (std::size_t i = 0; i < ports_.size(); ++i) {
            if (!ports_[i].pwm_enabled) {
                continue;
            }
            port_states_[i]->stop_pwm.store(false);
            port_states_[i]->pwm_thread =
                std::thread(&Impl::pwm_loop, this, i);
        }
    }

    void close() {
        // Phase 1 (under mutex_): mark closed and signal all PWM workers to
        // exit. We cannot join() while holding mutex_ because each worker
        // also acquires mutex_ during its ioctl writes — joining under the
        // mutex would deadlock against any worker currently waiting on it.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_) {
                return;
            }
            open_ = false;
            for (auto& state : port_states_) {
                state->stop_pwm.store(true, std::memory_order_release);
            }
        }

        // Phase 2 (no mutex held): wait for every worker to wind down. Each
        // worker checks stop_pwm at the top of every cycle, so the maximum
        // delay is one PWM period plus one idle-sleep interval (20 ms for
        // static-value ports). At 1 kHz that's ~21 ms per port.
        for (auto& state : port_states_) {
            if (state->pwm_thread.joinable()) {
                state->pwm_thread.join();
            }
        }

        // Phase 3 (under mutex_ again): close the fd now that no worker can
        // touch it.
        std::lock_guard<std::mutex> lock(mutex_);
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
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

        if (cfg.pwm_enabled) {
            // PWM port: hand off to the worker thread by updating the
            // atomic. The worker reads this on every cycle and adjusts the
            // duty (or switches to static-on / static-off for 100 / 0).
            // We still check open_ so the caller gets the same
            // NotConnected error they'd get from a boolean path.
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!open_) {
                    throw AlpacaException("ASIAIR Plus device is not open",
                                          AlpacaError::NotConnected);
                }
            }
            port_states_[index]->value.store(value, std::memory_order_release);
            return;
        }

        // Boolean port: synchronous SET_MODE + ENABLE + SET_LEVEL.
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) {
            throw AlpacaException("ASIAIR Plus device is not open",
                                  AlpacaError::NotConnected);
        }
        apply_port_locked(index, value);
        port_states_[index]->value.store(value, std::memory_order_release);
    }

    const std::string& device_path() const { return device_path_; }
    std::uint32_t pwm_frequency_hz() const { return pwm_frequency_hz_; }

private:
    struct PortState {
        // Desired value, written by set_value(), read by the PWM thread (and
        // by get_value() over the public API). 0..1 for boolean ports, 0..100
        // for PWM ports.
        std::atomic<int> value{0};

        // Stop signal for the PWM worker thread. Set by close()/destructor;
        // checked by pwm_loop() at every iteration.
        std::atomic<bool> stop_pwm{false};

        // The PWM worker thread. Joinable only while the port is connected
        // AND the port is pwm_enabled. Boolean ports never start one.
        std::thread pwm_thread;
    };

    // Soft-PWM worker loop. Runs for the entire connected lifetime of a
    // pwm_enabled port. Bypasses the kernel module's broken PWM mode and
    // generates the duty cycle in userspace using SET_LEVEL ioctls — the
    // same approach the kernel module's own hrtimer callback would have
    // taken if we could trigger it. Reads the desired duty from
    // port_states_[index]->value on every cycle so set_value() updates take
    // effect immediately without any signalling.
    void pwm_loop(std::size_t index) {
        using clock_t = std::chrono::steady_clock;
        const int kernel_idx = kernel_index_for(index);
        auto& state = *port_states_[index];

        // Period of one PWM cycle. Constant for the lifetime of this thread
        // (the wrapper takes pwm_frequency_hz_ at construction and doesn't
        // mutate it).
        const auto period_ns = std::chrono::nanoseconds(
            1000000000ULL / static_cast<unsigned long long>(pwm_frequency_hz_));

        // Track whether the kernel-side port has had its initial SET_MODE +
        // ENABLE pair sent. We only need to do that once per connect — every
        // subsequent SET_LEVEL works without re-sending mode/enable. Doing
        // them on every toggle would triple the ioctl count and add jitter.
        bool initialized = false;

        // Last logical level we wrote (true = panel ON, false = OFF). Used to
        // skip redundant ioctls while value sits at a static endpoint (0 or
        // 100) and to keep our cache consistent with the pad state.
        int last_level = -1;  // -1 = not yet written

        // Acquire-mutex helper that handles fd validity + initial setup.
        // Returns false if the wrapper has been closed (then the thread
        // should exit). Holds mutex_ only across the ioctl(s) — never during
        // sleeps.
        auto write_level_locked = [&](bool logical_on) -> bool {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_ || fd_ < 0) {
                return false;
            }
            if (!initialized) {
                work_mode_t mode{};
                mode.index = kernel_idx;
                mode.mode = PWM_GPIO_MODE_GPIO;
                if (::ioctl(fd_, PWM_GPIO_SET_MODE, &mode) != 0) {
                    return false;
                }
                int enable_arg = kernel_idx;
                if (::ioctl(fd_, PWM_GPIO_ENABLE, &enable_arg) != 0) {
                    return false;
                }
                initialized = true;
            }
            gpio_level_t lvl{};
            lvl.index = kernel_idx;
            // Polarity inverted (see apply_port_locked for the long story).
            lvl.level = logical_on ? 0 : 1;
            return ::ioctl(fd_, PWM_GPIO_SET_LEVEL, &lvl) == 0;
        };

        while (!state.stop_pwm.load(std::memory_order_acquire)) {
            int duty = state.value.load(std::memory_order_acquire);
            if (duty < 0) duty = 0;
            if (duty > 100) duty = 100;

            if (duty == 0) {
                if (last_level != 0) {
                    if (!write_level_locked(false)) break;
                    last_level = 0;
                }
                // Idle poll: re-check value periodically without burning CPU.
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (duty == 100) {
                if (last_level != 1) {
                    if (!write_level_locked(true)) break;
                    last_level = 1;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            // 0 < duty < 100: active PWM cycle.
            const auto on_ns =
                std::chrono::nanoseconds(period_ns.count() * duty / 100);
            const auto off_ns = period_ns - on_ns;

            // Use absolute wakeup targets (`sleep_until`) instead of
            // `sleep_for` so timing skew from ioctl latency doesn't drift
            // the duty cycle over many cycles.
            const auto t_on_end = clock_t::now() + on_ns;
            if (!write_level_locked(true)) break;
            last_level = 1;
            std::this_thread::sleep_until(t_on_end);

            if (state.stop_pwm.load(std::memory_order_acquire)) break;

            const auto t_cycle_end = t_on_end + off_ns;
            if (!write_level_locked(false)) break;
            last_level = 0;
            std::this_thread::sleep_until(t_cycle_end);
        }
    }

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
        // **Polarity at the SET_LEVEL ioctl is INVERTED from typical gpiod
        // semantics on this hardware.** Empirically verified by reading
        // /sys/kernel/debug/gpio and physically observing a 12V flat
        // panel on Port 2:
        //
        //   SET_LEVEL(0) -> line resolves to `in hi`  (input, pulled high
        //                    externally) -> panel physically **ON**.
        //   SET_LEVEL(1) -> line resolves to `out lo` (output, driven low)
        //                    -> panel physically **OFF**.
        //
        // I.e. the kernel module's "level=0" releases the line and the
        // external pull-up powers the gear; "level=1" actively drives the
        // pad low and cuts power. This is non-standard — most gpiod-based
        // chips treat level=1 as drive-high — and the cause is probably a
        // bug or quirk in the closed-source pwm_gpio.ko's level-argument
        // interpretation (possibly: level=1 maps internally to
        // gpiod_direction_output_raw(0), level=0 maps to
        // gpiod_direction_input). Either way, the empirical mapping is
        // what we have to live with on stock Debian. Don't "fix" this by
        // flipping it back — the previous polarity produced the inverted-
        // feeling NINA behavior the user reported on 2026-05-30
        // ("when I flip it on it goes off").
        gpio_level_t level{};
        level.index = kernel_idx;
        level.level = value != 0 ? 0 : 1;
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
