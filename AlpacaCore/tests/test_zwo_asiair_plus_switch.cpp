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
#include <alpacacore/util/power_port_state_store.h>
#include <alpacacore/vendor/zwo/zwo_asiair_plus_switch_driver.h>
#include <alpacacore/version.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "power_port_test_dir.h"

// ---------------------------------------------------------------------------
// ioctl stub. This binary defines ioctl(), so the wrapper's pwm_gpio requests
// land here when the fd is the stub device file; every other call is
// forwarded to the real ioctl (found with RTLD_NEXT).
//
// The stub plays one of two kernel modules on the device named by
// ioctl_stub().path, chosen by ioctl_stub().module (pwm_gpio.h has both
// numberings):
// - StubModule::Legacy, ZWO's stock pwm_gpio.ko as the legacy decode sees it:
//   answers PWM_GPIO_SET_MODE, PWM_GPIO_ENABLE and PWM_GPIO_SET_LEVEL and
//   records each SET_LEVEL. Raw SET_LEVEL polarity is the legacy path's:
//   0 = ON, 1 = OFF.
// - StubModule::Compat, the OpenAstro image's pwm_gpio_compat (corrected
//   numbering): answers every 'C' ioctl on the device.
//   PWM_GPIO_ABI_SET_MODE takes index 0..11 and mode 1 or 2, else EINVAL;
//   PWM_GPIO_ABI_SET_LEVEL takes index 0..11, else EINVAL, and records the
//   level (1 = ON, 0 = OFF); every other command, the legacy codes among
//   them, is refused with EINVAL and recorded in `refused`.
// The wrapper picks its numbering from the device's sysfs driver link, which
// a StubDeviceFile models with a fake /sys/dev/char tree passed through
// create_zwo_asiair_plus_switch's sysfs overload. A plain temp file is not a
// character device, so a compat case opens /dev/null (`char_device`), which
// any user can open.
//
// KNOWN PARITY GAPS (prefer closing a gap):
// - Legacy: PWM_GPIO_GET_LEVEL and every other ioctl go to the real ioctl()
//   and fail on a plain file (ENOTTY), and the stub accepts the legacy
//   SET_LEVEL code before SET_MODE and ENABLE. The field-validated stock
//   stream always sends those two first; what stock does otherwise is
//   unverified under the corrected numbering (pwm_gpio.h). The legacy cases
//   pin the order instead (`calls`).
// - Compat, harsher than the module: the real one answers GET_LEVEL,
//   LEVEL_GET (the legacy SET_MODE code), GET_CONFIG, SET_CONFIG, PWM_ENABLE
//   and PWM_DISABLE (the legacy ENABLE code), and ENOTTY for an unknown 'C'
//   code; the stub refuses all of them with EINVAL, since the GPIO path must
//   send none. It does not model the module's PWM, its index 2 input line
//   (SET_LEVEL there is EPERM) or SET_MODE(GPIO) stopping a running PWM.
// - The compat device is /dev/null, not a 10:55 misc node; the sysfs tree is
//   keyed by its real major:minor.
// ---------------------------------------------------------------------------
namespace {

// The command codes the stub knows (pwm_gpio.h; the tests do not include that header).
constexpr unsigned long kLegacySetLevel = 0x40084302UL;  // PWM_GPIO_SET_LEVEL (legacy decode)
constexpr unsigned long kLegacySetMode = 0x40084307UL;   // PWM_GPIO_SET_MODE (legacy decode)
constexpr unsigned long kLegacyEnable = 0x40044306UL;    // PWM_GPIO_ENABLE (legacy decode)
constexpr unsigned long kAbiSetMode = 0x40084302UL;      // PWM_GPIO_ABI_SET_MODE
constexpr unsigned long kAbiPwmEnable = 0x40044305UL;    // PWM_GPIO_ABI_PWM_ENABLE
constexpr unsigned long kAbiSetLevel = 0x40084308UL;     // PWM_GPIO_ABI_SET_LEVEL

// Which kernel module the ioctl stub plays (comment above).
enum class StubModule : std::uint8_t { Legacy, Compat };

// One ioctl the stub answered or refused.
struct IoctlCall {
    unsigned long request = 0;
    std::int32_t index = -1;  // the argument's first field (every command's is the kernel index)
    std::int32_t value = 0;   // its second field for a {index, level} or {index, mode} argument, else 0
    bool refused = false;     // answered -1 (EINVAL)
    bool operator==(const IoctlCall&) const = default;
};

struct IoctlStub {
    std::mutex m;
    std::condition_variable cv;
    std::string path;                        // stub device file; empty = inactive
    StubModule module = StubModule::Legacy;  // the module the stub plays on `path`
    std::vector<std::int32_t> levels;        // raw level of each accepted SET_LEVEL, in order
    int off_writes = 0;                      // logical OFF SET_LEVELs: raw 1 on Legacy, raw 0 on Compat
    int requests = 0;                        // every stubbed ioctl, refused ones included
    std::vector<IoctlCall> calls;            // every stubbed ioctl, in order
    std::vector<unsigned long> refused;      // Compat: the request code of each refused ioctl
};

IoctlStub& ioctl_stub() {
    static IoctlStub s;
    return s;
}

bool fd_is_stub(int fd) {
    auto& st = ioctl_stub();
    std::lock_guard<std::mutex> lock(st.m);
    if (st.path.empty()) {
        return false;
    }
    char buf[256];
    const std::string link = "/proc/self/fd/" + std::to_string(fd);
    const ssize_t n = ::readlink(link.c_str(), buf, sizeof(buf) - 1);
    return n > 0 && st.path == std::string(buf, static_cast<std::size_t>(n));
}

// The ioctl type byte of every pwm_gpio command ('C').
bool is_pwm_gpio_request(unsigned long request) { return ((request >> 8) & 0xffUL) == 0x43UL; }

bool is_legacy_request(unsigned long request) {
    return request == kLegacySetLevel || request == kLegacySetMode || request == kLegacyEnable;
}

// The stock module as the legacy decode sees it: every one of its three commands is accepted.
int legacy_ioctl_locked(IoctlStub& st, unsigned long request, const std::int32_t* arg) {
    IoctlCall call{request, arg[0], 0, false};
    if (request == kLegacySetMode) {
        call.value = arg[1];  // work_mode_t {index, mode}
    } else if (request == kLegacySetLevel) {
        call.value = arg[1];  // gpio_level_t {index, level}
        st.levels.push_back(arg[1]);
        if (arg[1] == 1) {
            ++st.off_writes;
        }
    }
    st.calls.push_back(call);
    return 0;
}

// pwm_gpio_compat: SET_MODE and SET_LEVEL only; see the comment above for what is refused.
int compat_ioctl_locked(IoctlStub& st, unsigned long request, const std::int32_t* arg) {
    constexpr std::int32_t kLines = 12;  // the airplus-gpios node's 12 lines
    IoctlCall call{request, arg != nullptr ? arg[0] : -1, 0, false};
    bool ok = false;
    if (request == kAbiSetMode && arg != nullptr) {
        call.value = arg[1];
        ok = arg[0] >= 0 && arg[0] < kLines && (arg[1] == 1 || arg[1] == 2);
    } else if (request == kAbiSetLevel && arg != nullptr) {
        call.value = arg[1];
        ok = arg[0] >= 0 && arg[0] < kLines;
        if (ok) {
            st.levels.push_back(arg[1]);
            if (arg[1] == 0) {
                ++st.off_writes;
            }
        }
    }
    call.refused = !ok;
    st.calls.push_back(call);
    if (!ok) {
        st.refused.push_back(request);
        errno = EINVAL;
        return -1;
    }
    return 0;
}

}  // namespace

extern "C" int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    if (is_pwm_gpio_request(request) && fd_is_stub(fd)) {
        auto& st = ioctl_stub();
        std::lock_guard<std::mutex> lock(st.m);
        const auto* words = static_cast<const std::int32_t*>(arg);
        if (st.module == StubModule::Compat) {
            ++st.requests;
            const int rc = compat_ioctl_locked(st, request, words);
            st.cv.notify_all();
            return rc;
        }
        if (is_legacy_request(request)) {
            ++st.requests;
            const int rc = legacy_ioctl_locked(st, request, words);
            st.cv.notify_all();
            return rc;
        }
    }
    using Fn = int (*)(int, unsigned long, void*);
    static const auto real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "ioctl"));
    if (real == nullptr) {
        errno = ENOSYS;
        return -1;
    }
    return real(fd, request, arg);
}

namespace {

using alpacacore::test::require_alpaca_error;

} // namespace

TEST_CASE("ZWO ASIAIR Plus Switch Driver - Defaults", "[zwo][switch][asiair-plus][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(
        0, alpacacore::vendor::zwo::default_asiair_plus_rk3568_config());

    REQUIRE(driver != nullptr);
    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Switch);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE_FALSE(driver->get_connected());
    REQUIRE(driver->get_name() == "ZWO ASIAIR Plus Switch (RK3568)");
    REQUIRE(driver->get_max_switch() == 4);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - Device metadata",
          "[zwo][switch][asiair-plus][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(
        3, alpacacore::vendor::zwo::default_asiair_plus_rk3568_config());

    REQUIRE(driver != nullptr);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() ==
          "ZWO ASIAIR Plus (RK3568) 12V power switch (/dev/pwm-gpio-misc)");
    CHECK(driver->get_driver_info() == "AlpacaCore ZWO ASIAIR Plus Switch");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "ZWO_ASIAIR_PLUS_RK3568_3");
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - Not connected throws",
          "[zwo][switch][asiair-plus][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(
        0, alpacacore::vendor::zwo::default_asiair_plus_rk3568_config());

    REQUIRE_FALSE(driver->get_connected());

    require_alpaca_error([&]() { driver->get_switch(0); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch(0, true); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_switch_value(2); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_switch_value(2, 1.0); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_state_change_complete(0); },
                         alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - Unsupported actions",
          "[zwo][switch][asiair-plus][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(
        0, alpacacore::vendor::zwo::default_asiair_plus_rk3568_config());

    CHECK(driver->get_supported_actions().empty());
    CHECK_FALSE(driver->can_action("anything"));

    CHECK_THROWS_AS(driver->action("test", ""), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_blind("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_bool("test", false), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->command_string("test", false), alpacacore::AlpacaException);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - Per-port metadata",
          "[zwo][switch][asiair-plus][unit]") {
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.ports[3].pwm_enabled = true; // mark Port 4 (DC port 4) as a PWM channel
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg);

    REQUIRE(driver->get_max_switch() == 4);

    for (int i = 0; i < 4; ++i) {
        CHECK(driver->get_can_write(i));
        CHECK_FALSE(driver->get_can_async(i));
        CHECK(driver->get_switch_step(i) == Catch::Approx(1.0));
        CHECK(driver->get_min_switch_value(i) == Catch::Approx(0.0));
    }

    CHECK(driver->get_switch_name(0) == "Port 1");
    CHECK(driver->get_switch_name(1) == "Port 2");
    CHECK(driver->get_switch_name(2) == "Port 3");
    CHECK(driver->get_switch_name(3) == "Port 4");

    // Boolean channels have range [0, 1]; the PWM channel exposes [0, 100].
    CHECK(driver->get_max_switch_value(0) == Catch::Approx(1.0));
    CHECK(driver->get_max_switch_value(1) == Catch::Approx(1.0));
    CHECK(driver->get_max_switch_value(2) == Catch::Approx(1.0));
    CHECK(driver->get_max_switch_value(3) == Catch::Approx(100.0));

    CHECK(driver->get_switch_description(0) == "ASIAIR Plus DC port 1 (on/off)");
    CHECK(driver->get_switch_description(1) == "ASIAIR Plus DC port 2 (on/off)");
    CHECK(driver->get_switch_description(2) == "ASIAIR Plus DC port 3 (on/off)");
    CHECK(driver->get_switch_description(3) == "ASIAIR Plus DC port 4 (PWM 0-100%)");

    // User-renamed switches persist for the lifetime of the driver instance.
    driver->set_switch_name(0, "Mount Power");
    CHECK(driver->get_switch_name(0) == "Mount Power");
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - Switch ID range validation",
          "[zwo][switch][asiair-plus][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(
        0, alpacacore::vendor::zwo::default_asiair_plus_rk3568_config());

    require_alpaca_error([&]() { driver->get_switch_name(-1); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_name(4); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_can_write(99); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_description(4); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_min_switch_value(4); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_max_switch_value(4); },
                         alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_step(4); },
                         alpacacore::AlpacaError::InvalidValue);

    // ID validation must run before the connection check: an out-of-range ID
    // throws InvalidValue even while disconnected (ASCOM spec), not NotConnected.
    REQUIRE_FALSE(driver->get_connected());
    require_alpaca_error([&]() { driver->get_switch(4); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_switch_value(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->get_state_change_complete(4); }, alpacacore::AlpacaError::InvalidValue);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - State machine when disconnected",
          "[zwo][switch][asiair-plus][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(
        0, alpacacore::vendor::zwo::default_asiair_plus_rk3568_config());

    // ASCOM contract: a disconnected switch reports get_connected() = false,
    // get_connecting() = false, and get_device_state() returns the empty list.
    CHECK_FALSE(driver->get_connected());
    CHECK_FALSE(driver->get_connecting());
    {
        // DeviceState is the empty list while disconnected: the SwitchDriver
        // base builds it from the public getters, which throw NotConnected and
        // are omitted, and TimeStamp itself is withheld too (ASCOM read-all FAQ).
        const auto state = driver->get_device_state();
        REQUIRE(state.empty());
    }
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - Unsupported method error codes",
          "[zwo][switch][asiair-plus][unit]") {
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(
        0, alpacacore::vendor::zwo::default_asiair_plus_rk3568_config());

    // Async mutation isn't implemented; the disconnected-state check fires first
    // because ASCOM contract requires NotConnected before any other validation.
    require_alpaca_error([&]() { driver->set_async(0, true); },
                         alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_async_value(0, 1.0); },
                         alpacacore::AlpacaError::NotConnected);

    // Action/Command surfaces are unsupported on this device.
    require_alpaca_error([&]() { driver->action("test", ""); },
                         alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("test", false); },
                         alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->command_bool("test", false); },
                         alpacacore::AlpacaError::NotImplemented);
    require_alpaca_error([&]() { driver->command_string("test", false); },
                         alpacacore::AlpacaError::NotImplemented);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - Constructor rejects invalid configs",
          "[zwo][switch][asiair-plus][unit]") {
    // Empty ports list rejected at construction.
    alpacacore::vendor::zwo::AsiairPlusSwitchConfig empty_cfg;
    empty_cfg.ports.clear();
    require_alpaca_error(
        [&]() { alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, empty_cfg); },
        alpacacore::AlpacaError::InvalidValue);

    // More than four ports rejected (the hardware only has four DC outputs).
    auto too_many = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    too_many.ports.push_back({"Port 5", false});
    require_alpaca_error(
        [&]() { alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, too_many); },
        alpacacore::AlpacaError::InvalidValue);

    // PWM frequency of 0 Hz is rejected (would divide by zero in the period calc).
    auto zero_freq = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    zero_freq.pwm_frequency_hz = 0;
    require_alpaca_error(
        [&]() { alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, zero_freq); },
        alpacacore::AlpacaError::InvalidValue);

    // Absurdly high PWM frequency is rejected (above the empirically validated range).
    auto huge_freq = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    huge_freq.pwm_frequency_hz = 1000001;
    require_alpaca_error(
        [&]() { alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, huge_freq); },
        alpacacore::AlpacaError::InvalidValue);
}

// A PWM worker stops wherever it is in the cycle; close() must leave the port at
// the steady level of its duty. Raw SET_LEVEL polarity is inverted: 0 = ON.
TEST_CASE("ZWO ASIAIR Plus Switch Driver - close settles a partial-duty PWM port ON",
          "[zwo][switch][asiair-plus][unit]") {
    char tmpl[] = "/tmp/asiair-plus-stub-XXXXXX";
    const int tfd = ::mkstemp(tmpl);
    REQUIRE(tfd >= 0);
    ::close(tfd);
    auto& st = ioctl_stub();
    // Clear the stub and remove the temp file even when a REQUIRE below throws.
    struct Cleanup {
        const char* path;
        IoctlStub& stub;
        ~Cleanup() {
            {
                std::lock_guard<std::mutex> lock(stub.m);
                stub.path.clear();
            }
            ::unlink(path);
        }
    } cleanup{tmpl, st};
    {
        std::lock_guard<std::mutex> lock(st.m);
        st.path = tmpl;
        st.levels.clear();
        st.off_writes = 0;
    }

    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.device_path = tmpl;
    cfg.ports[3].pwm_enabled = true;
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg);
    driver->set_connected(true);
    driver->set_switch_value(3, 50.0);

    // Close right after an OFF write: the worker is in the off half of its
    // cycle and exits at the top of the loop without another write.
    {
        std::unique_lock<std::mutex> lock(st.m);
        REQUIRE(st.cv.wait_for(lock, std::chrono::seconds(5), [&] { return st.off_writes >= 1; }));
    }
    driver->set_connected(false);

    std::vector<std::int32_t> levels;
    {
        std::lock_guard<std::mutex> lock(st.m);
        levels = st.levels;
    }
    REQUIRE_FALSE(levels.empty());
    CHECK(levels.back() == 0);  // duty 50 settles logically ON (raw 0)
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - off at server start writes OFF to every port without connecting",
          "[zwo][switch][asiair-plus][unit][power-on]") {
    alpacacore::test::TempPowerPortDir dir;
    char tmpl[] = "/tmp/asiair-plus-stub-XXXXXX";
    const int tfd = ::mkstemp(tmpl);
    REQUIRE(tfd >= 0);
    ::close(tfd);
    auto& st = ioctl_stub();
    struct Cleanup {
        const char* path;
        IoctlStub& stub;
        ~Cleanup() {
            {
                std::lock_guard<std::mutex> lock(stub.m);
                stub.path.clear();
            }
            ::unlink(path);
        }
    } cleanup{tmpl, st};
    {
        std::lock_guard<std::mutex> lock(st.m);
        st.path = tmpl;
        st.levels.clear();
        st.off_writes = 0;
    }

    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.device_path = tmpl;
    auto leave = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg);
    leave->apply_power_on_state(true);
    {
        std::lock_guard<std::mutex> lock(st.m);
        CHECK(st.levels.empty());  // the default, Leave, writes nothing
    }

    cfg.power_on_state = alpacacore::util::PowerOnState::Off;
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(1, cfg);
    driver->apply_power_on_state(true);
    CHECK_FALSE(driver->get_connected());
    {
        std::lock_guard<std::mutex> lock(st.m);
        CHECK(st.off_writes == 4);  // raw level 1 = logical OFF, once per port
    }
    driver->set_connected(true);
    for (int i = 0; i < 4; ++i) {
        CHECK_FALSE(driver->get_switch(i));
    }
    driver->set_connected(false);
}

namespace {

// What a StubDeviceFile stands for.
struct StubDeviceOptions {
    StubModule module = StubModule::Legacy;
    // Open /dev/null, a character device, instead of a temp file: the wrapper reads sysfs only for one.
    bool char_device = false;
    // The driver the fake sysfs tree's <major>:<minor>/device/driver link names; empty = no link. A plain temp file
    // gets its entry under 0:0, its st_rdev.
    std::string sysfs_driver;
};

std::string mkdtemp_or_empty(const char* pattern) {
    char buf[64];
    std::strncpy(buf, pattern, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    return ::mkdtemp(buf) != nullptr ? std::string(buf) : std::string();
}

// Points the ioctl stub at a fresh device for one case (a temp file, or /dev/null), with a fake /sys/dev/char tree;
// restores the stub (Legacy, inactive) and removes the files it made on exit.
struct StubDeviceFile {
    char path[64] = "/tmp/asiair-plus-stub-XXXXXX";
    StubDeviceFile() : StubDeviceFile(StubDeviceOptions{}) {}
    explicit StubDeviceFile(const StubDeviceOptions& options) : char_device_(options.char_device) {
        if (char_device_) {
            std::strncpy(path, "/dev/null", sizeof(path) - 1);
            baseline_fds_ = count_fds();
        } else {
            const int fd = ::mkstemp(path);
            if (fd >= 0) ::close(fd);
        }
        // The fake /sys/dev/char: <major>:<minor>/device/driver -> .../drivers/<sysfs_driver>, the real link's shape.
        sysfs_root_ = mkdtemp_or_empty("/tmp/asiair-plus-sysfs-XXXXXX");
        struct stat sb {};
        if (!sysfs_root_.empty() && !options.sysfs_driver.empty() && ::stat(path, &sb) == 0) {
            const std::filesystem::path dev =
                std::filesystem::path(sysfs_root_) /
                (std::to_string(major(sb.st_rdev)) + ":" + std::to_string(minor(sb.st_rdev))) / "device";
            std::error_code ec;
            std::filesystem::create_directories(dev, ec);
            std::filesystem::create_symlink("../../../../bus/platform/drivers/" + options.sysfs_driver, dev / "driver",
                                            ec);
        }
        auto& st = ioctl_stub();
        std::lock_guard<std::mutex> lock(st.m);
        st.path = path;
        st.module = options.module;
        st.levels.clear();
        st.off_writes = 0;
        st.requests = 0;
        st.calls.clear();
        st.refused.clear();
    }
    ~StubDeviceFile() {
        auto& st = ioctl_stub();
        {
            std::lock_guard<std::mutex> lock(st.m);
            st.path.clear();
            st.module = StubModule::Legacy;
        }
        if (!char_device_) {
            ::unlink(path);
        }
        if (!sysfs_root_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(sysfs_root_, ec);
        }
    }
    StubDeviceFile(const StubDeviceFile&) = delete;
    StubDeviceFile& operator=(const StubDeviceFile&) = delete;

    // How many of this process's file descriptors are open on the stub device, beyond those open when it was made
    // (stdin may be /dev/null).
    int open_fds() const { return count_fds() - baseline_fds_; }

    // The fake /sys/dev/char root, for create_zwo_asiair_plus_switch(number, config, sysfs_root()).
    const std::string& sysfs_root() const { return sysfs_root_; }

private:
    int count_fds() const {
        int n = 0;
        for (int fd = 0; fd < 1024; ++fd) {
            char buf[256];
            const std::string link = "/proc/self/fd/" + std::to_string(fd);
            const ssize_t len = ::readlink(link.c_str(), buf, sizeof(buf) - 1);
            if (len > 0 && std::string(buf, static_cast<std::size_t>(len)) == path) ++n;
        }
        return n;
    }

    bool char_device_ = false;
    int baseline_fds_ = 0;
    std::string sysfs_root_;
};

}  // namespace

TEST_CASE("ZWO ASIAIR Plus Switch Driver - a client's write is saved per UniqueID and port",
          "[zwo][switch][asiair-plus][unit][power-on]") {
    alpacacore::test::TempPowerPortDir dir;
    StubDeviceFile stub;
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.device_path = stub.path;
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg);
    driver->set_connected(true);
    driver->set_switch(0, false);
    driver->set_switch(2, true);
    CHECK(alpacacore::util::PowerPortStateStore::load(driver->get_unique_id()) ==
          std::map<int, double>{{0, 0.0}, {2, 100.0}});
    driver->set_connected(false);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - a Disconnect before any Connect keeps the fd a start opened",
          "[zwo][switch][asiair-plus][unit][power-on]") {
    alpacacore::test::TempPowerPortDir dir;
    StubDeviceFile stub;
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.device_path = stub.path;
    cfg.power_on_state = alpacacore::util::PowerOnState::Off;
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg);
    driver->apply_power_on_state(true);
    REQUIRE(stub.open_fds() == 1);
    // A client's Disconnect with nothing connected must not close it (decision 0021 rule 5).
    driver->set_connected(false);
    CHECK(stub.open_fds() == 1);
    driver->set_connected(true);
    driver->set_connected(false);
    CHECK(stub.open_fds() == 0);
}

// Decision 0021 rule 1: a connect must not drive a PWM port to the cached 100 before a client sets it.
TEST_CASE("ZWO ASIAIR Plus Switch Driver - connect writes nothing to a PWM port no client set",
          "[zwo][switch][asiair-plus][unit][power-on]") {
    StubDeviceFile stub;
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.device_path = stub.path;
    cfg.ports[3].pwm_enabled = true;
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg);
    driver->set_connected(true);
    // Bounded wait for a write that must not come (the worker's first pass is immediate).
    auto& st = ioctl_stub();
    int requests = 0;
    {
        std::unique_lock<std::mutex> lock(st.m);
        st.cv.wait_for(lock, std::chrono::milliseconds(300), [&] { return st.requests > 0; });
        requests = st.requests;
    }
    driver->set_connected(false);
    CHECK(requests == 0);
    // Disconnect must not drive the unset line either: close() settles only a port a client armed.
    {
        std::lock_guard<std::mutex> lock(st.m);
        CHECK(st.requests == 0);
    }
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - a client's PWM write still reaches the fd after connect",
          "[zwo][switch][asiair-plus][unit][power-on]") {
    StubDeviceFile stub;
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.device_path = stub.path;
    cfg.ports[3].pwm_enabled = true;
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg);
    driver->set_connected(true);
    driver->set_switch_value(3, 50.0);
    auto& st = ioctl_stub();
    bool wrote = false;
    {
        std::unique_lock<std::mutex> lock(st.m);
        wrote = st.cv.wait_for(lock, std::chrono::seconds(5), [&] { return !st.levels.empty(); });
    }
    driver->set_connected(false);
    CHECK(wrote);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - connect with no PWM port writes nothing",
          "[zwo][switch][asiair-plus][unit][power-on]") {
    StubDeviceFile stub;
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.device_path = stub.path;
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg);
    driver->set_connected(true);
    auto& st = ioctl_stub();
    int requests = 0;
    {
        std::unique_lock<std::mutex> lock(st.m);
        st.cv.wait_for(lock, std::chrono::milliseconds(300), [&] { return st.requests > 0; });
        requests = st.requests;
    }
    driver->set_connected(false);
    CHECK(requests == 0);
}

// Decision 0021 rule 1: close() settles a PWM line to its steady level, so the next connect reports that level.
TEST_CASE("ZWO ASIAIR Plus Switch Driver - a reconnect reports the level close settled and writes nothing",
          "[zwo][switch][asiair-plus][unit][power-on]") {
    StubDeviceFile stub;
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.device_path = stub.path;
    cfg.ports[3].pwm_enabled = true;
    auto driver = alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg);
    auto& st = ioctl_stub();
    driver->set_connected(true);
    driver->set_switch_value(3, 50.0);
    {
        std::unique_lock<std::mutex> lock(st.m);
        REQUIRE(st.cv.wait_for(lock, std::chrono::seconds(5), [&] { return !st.levels.empty(); }));
    }
    driver->set_connected(false);
    {
        std::lock_guard<std::mutex> lock(st.m);
        st.requests = 0;
    }
    driver->set_connected(true);
    int requests = 0;
    {
        std::unique_lock<std::mutex> lock(st.m);
        st.cv.wait_for(lock, std::chrono::milliseconds(300), [&] { return st.requests > 0; });
        requests = st.requests;
    }
    const double reported = driver->get_switch_value(3);
    driver->set_connected(false);
    CHECK(requests == 0);
    CHECK(reported == 100.0);
}

// ---- The two ioctl numberings (pwm_gpio.h): pwm_gpio_compat gets the corrected one, anything else the legacy one ----
//
// The OpenAstro mainline image replaces ZWO's pwm_gpio.ko with the clean-room pwm_gpio_compat, which implements the
// module's real command numbers. The wrapper picks the numbering at open() from the device's sysfs driver link; these
// cases point it at a fake /sys/dev/char tree through the factory's sysfs overload, and the stub plays the module the
// tree names. The legacy cases above run on the default factory and are unchanged.
namespace {

StubDeviceOptions compat_device() {
    StubDeviceOptions options;
    options.module = StubModule::Compat;
    options.char_device = true;
    options.sysfs_driver = "pwm_gpio_compat";
    return options;
}

StubDeviceOptions legacy_device(bool char_device, std::string sysfs_driver) {
    StubDeviceOptions options;
    options.module = StubModule::Legacy;
    options.char_device = char_device;
    options.sysfs_driver = std::move(sysfs_driver);
    return options;
}

std::unique_ptr<alpacacore::SwitchDriver> make_on(const StubDeviceFile& device,
                                                  alpacacore::vendor::zwo::AsiairPlusSwitchConfig cfg =
                                                      alpacacore::vendor::zwo::default_asiair_plus_rk3568_config()) {
    cfg.device_path = device.path;
    return alpacacore::vendor::zwo::create_zwo_asiair_plus_switch(0, cfg, device.sysfs_root());
}

std::vector<IoctlCall> stub_calls() {
    auto& st = ioctl_stub();
    std::lock_guard<std::mutex> lock(st.m);
    return st.calls;
}

void clear_stub_calls() {
    auto& st = ioctl_stub();
    std::lock_guard<std::mutex> lock(st.m);
    st.calls.clear();
    st.levels.clear();
    st.off_writes = 0;
    st.requests = 0;
}

// The legacy stream for one boolean write to DC port 1 (kernel index 4): SET_MODE(GPIO), ENABLE, SET_LEVEL(raw).
std::vector<IoctlCall> legacy_boolean_write_port1(bool on) {
    return {
        {kLegacySetMode, 4, 1, false},
        {kLegacyEnable, 4, 0, false},
        {kLegacySetLevel, 4, on ? 0 : 1, false},
    };
}

// Connects a default driver on a fresh @p options device, sets Port @p port + 1 to @p on, reads it back and
// disconnects; returns every ioctl the stub saw.
std::vector<IoctlCall> calls_of_one_write(const StubDeviceOptions& options, int port, bool on) {
    alpacacore::test::TempPowerPortDir dir;
    StubDeviceFile device(options);
    auto driver = make_on(device);
    driver->set_connected(true);
    driver->set_switch(port, on);
    CHECK(driver->get_switch(port) == on);
    driver->set_connected(false);
    return stub_calls();
}

}  // namespace

TEST_CASE("ZWO ASIAIR Plus Switch Driver - pwm_gpio_compat: ON sends SET_MODE(GPIO), then SET_LEVEL(1)",
          "[zwo][switch][asiair-plus][unit][compat]") {
    CHECK(calls_of_one_write(compat_device(), 0, true) ==
          std::vector<IoctlCall>{{kAbiSetMode, 4, 1, false}, {kAbiSetLevel, 4, 1, false}});
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - pwm_gpio_compat: OFF sends SET_LEVEL(0)",
          "[zwo][switch][asiair-plus][unit][compat]") {
    CHECK(calls_of_one_write(compat_device(), 1, false) ==
          std::vector<IoctlCall>{{kAbiSetMode, 5, 1, false}, {kAbiSetLevel, 5, 0, false}});
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - pwm_gpio_compat: no PWM ENABLE or other command reaches the module",
          "[zwo][switch][asiair-plus][unit][compat]") {
    alpacacore::test::TempPowerPortDir dir;
    StubDeviceFile device(compat_device());
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.ports[3].pwm_enabled = true;
    auto driver = make_on(device, cfg);
    driver->set_connected(true);
    for (int i = 0; i < 3; ++i) {
        driver->set_switch(i, false);
        driver->set_switch(i, true);
    }
    driver->set_switch_value(3, 0.0);
    driver->set_switch_value(3, 100.0);
    {
        // The worker's steady 100 is a SET_LEVEL(1) on the PWM port.
        auto& st = ioctl_stub();
        std::unique_lock<std::mutex> lock(st.m);
        REQUIRE(st.cv.wait_for(lock, std::chrono::seconds(5), [&] {
            for (const auto& call : st.calls) {
                if (call.request == kAbiSetLevel && call.index == 7 && call.value == 1) {
                    return true;
                }
            }
            return false;
        }));
    }
    driver->set_connected(false);
    const auto calls = stub_calls();
    REQUIRE_FALSE(calls.empty());
    for (const auto& call : calls) {
        CHECK(call.request != kAbiPwmEnable);
        CHECK((call.request == kAbiSetMode || call.request == kAbiSetLevel));
        CHECK_FALSE(call.refused);
    }
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - pwm_gpio_compat: SET_MODE once per line since open, SET_LEVEL every write",
          "[zwo][switch][asiair-plus][unit][compat]") {
    alpacacore::test::TempPowerPortDir dir;
    StubDeviceFile device(compat_device());
    auto driver = make_on(device);
    driver->set_connected(true);
    driver->set_switch(0, false);
    driver->set_switch(0, true);
    driver->set_switch(0, false);
    driver->set_switch(2, true);
    driver->set_connected(false);
    CHECK(stub_calls() == std::vector<IoctlCall>{
                              {kAbiSetMode, 4, 1, false},
                              {kAbiSetLevel, 4, 0, false},
                              {kAbiSetLevel, 4, 1, false},
                              {kAbiSetLevel, 4, 0, false},
                              {kAbiSetMode, 6, 1, false},
                              {kAbiSetLevel, 6, 1, false},
                          });
    // A new open() starts over: the module is told the mode again before the line's next level.
    clear_stub_calls();
    driver->set_connected(true);
    driver->set_switch(0, true);
    driver->set_connected(false);
    CHECK(stub_calls() == std::vector<IoctlCall>{{kAbiSetMode, 4, 1, false}, {kAbiSetLevel, 4, 1, false}});
}

// Decision 0021 rule 1 and the RK3568 open() rule (zwo.instructions.md): a connect touches no kernel-side state.
TEST_CASE("ZWO ASIAIR Plus Switch Driver - pwm_gpio_compat: connect and disconnect write nothing",
          "[zwo][switch][asiair-plus][unit][compat][power-on]") {
    StubDeviceFile device(compat_device());
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.ports[3].pwm_enabled = true;
    auto driver = make_on(device, cfg);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(device.open_fds() == 1);
    auto& st = ioctl_stub();
    int requests = 0;
    {
        // Bounded wait for a write that must not come (a PWM worker's first pass is immediate).
        std::unique_lock<std::mutex> lock(st.m);
        st.cv.wait_for(lock, std::chrono::milliseconds(300), [&] { return st.requests > 0; });
        requests = st.requests;
    }
    for (int i = 0; i < 4; ++i) {
        CHECK(driver->get_switch(i));  // the "on" cache: nothing was read either
    }
    driver->set_connected(false);
    CHECK(device.open_fds() == 0);
    CHECK(requests == 0);
    std::lock_guard<std::mutex> lock(st.m);
    CHECK(st.requests == 0);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - pwm_gpio_compat: soft-PWM edges are SET_LEVEL only and close settles ON",
          "[zwo][switch][asiair-plus][unit][compat]") {
    alpacacore::test::TempPowerPortDir dir;
    StubDeviceFile device(compat_device());
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.ports[3].pwm_enabled = true;
    auto driver = make_on(device, cfg);
    driver->set_connected(true);
    driver->set_switch_value(3, 50.0);
    auto& st = ioctl_stub();
    {
        // Several edges of the 50 Hz waveform, both levels among them; stop right after an OFF edge, so the worker
        // exits in the off half of its cycle and only close()'s settle can leave the line ON.
        std::unique_lock<std::mutex> lock(st.m);
        REQUIRE(st.cv.wait_for(lock, std::chrono::seconds(5),
                               [&] { return st.off_writes >= 2 && st.levels.size() >= 4 && st.levels.back() == 0; }));
    }
    driver->set_connected(false);
    const auto calls = stub_calls();
    REQUIRE(calls.size() >= 5);
    CHECK(calls.front() == IoctlCall{kAbiSetMode, 7, 1, false});
    for (std::size_t i = 1; i < calls.size(); ++i) {
        CHECK(calls[i].request == kAbiSetLevel);
        CHECK(calls[i].index == 7);
        CHECK((calls[i].value == 0 || calls[i].value == 1));
        CHECK_FALSE(calls[i].refused);
    }
    CHECK(calls.back().value == 1);  // duty 50 settles ON, level 1 on this module
    // The next connect reports the level close settled and writes nothing.
    clear_stub_calls();
    driver->set_connected(true);
    CHECK(driver->get_switch_value(3) == 100.0);
    driver->set_connected(false);
    CHECK(stub_calls().empty());
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - pwm_gpio_compat: off at server start sends SET_LEVEL(0) to every port",
          "[zwo][switch][asiair-plus][unit][compat][power-on]") {
    alpacacore::test::TempPowerPortDir dir;
    StubDeviceFile device(compat_device());
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.power_on_state = alpacacore::util::PowerOnState::Off;
    auto driver = make_on(device, cfg);
    driver->apply_power_on_state(true);
    CHECK_FALSE(driver->get_connected());
    std::vector<IoctlCall> expected;
    for (std::int32_t index = 4; index <= 7; ++index) {
        expected.push_back({kAbiSetMode, index, 1, false});
        expected.push_back({kAbiSetLevel, index, 0, false});
    }
    CHECK(stub_calls() == expected);
    driver->set_connected(true);
    for (int i = 0; i < 4; ++i) {
        CHECK_FALSE(driver->get_switch(i));
    }
    driver->set_connected(false);
}

// Fail toward stock: only a character device whose sysfs driver is exactly pwm_gpio_compat gets the corrected
// numbering. Each of these runs the stock module's stub, and its stream is the legacy one byte for byte.
TEST_CASE("ZWO ASIAIR Plus Switch Driver - a plain file keeps the legacy numbering whatever sysfs names",
          "[zwo][switch][asiair-plus][unit][compat]") {
    CHECK(calls_of_one_write(legacy_device(false, "pwm_gpio_compat"), 0, true) == legacy_boolean_write_port1(true));
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - a character device driven by ZWO's pwm_gpio keeps the legacy numbering",
          "[zwo][switch][asiair-plus][unit][compat]") {
    CHECK(calls_of_one_write(legacy_device(true, "pwm_gpio"), 0, true) == legacy_boolean_write_port1(true));
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - a character device with no sysfs driver link keeps the legacy numbering",
          "[zwo][switch][asiair-plus][unit][compat]") {
    CHECK(calls_of_one_write(legacy_device(true, ""), 0, true) == legacy_boolean_write_port1(true));
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - a driver name that only begins with pwm_gpio_compat keeps the legacy one",
          "[zwo][switch][asiair-plus][unit][compat]") {
    CHECK(calls_of_one_write(legacy_device(true, "pwm_gpio_compat_old"), 0, true) == legacy_boolean_write_port1(true));
}

// The legacy stream itself (stock pwm_gpio.ko) is pinned too, so a change to the compat path cannot move it: every
// boolean write re-sends SET_MODE and ENABLE before SET_LEVEL, with the inverted raw level.
TEST_CASE("ZWO ASIAIR Plus Switch Driver - legacy numbering: every boolean write sends SET_MODE, ENABLE, SET_LEVEL",
          "[zwo][switch][asiair-plus][unit][compat]") {
    alpacacore::test::TempPowerPortDir dir;
    StubDeviceFile device(legacy_device(true, "pwm_gpio"));
    auto driver = make_on(device);
    driver->set_connected(true);
    driver->set_switch(0, false);
    driver->set_switch(0, true);
    driver->set_connected(false);
    auto expected = legacy_boolean_write_port1(false);
    const auto second = legacy_boolean_write_port1(true);
    expected.insert(expected.end(), second.begin(), second.end());
    CHECK(stub_calls() == expected);
}

TEST_CASE("ZWO ASIAIR Plus Switch Driver - legacy numbering: soft-PWM inits once and close settles raw 0",
          "[zwo][switch][asiair-plus][unit][compat]") {
    alpacacore::test::TempPowerPortDir dir;
    StubDeviceFile device(legacy_device(true, "pwm_gpio"));
    auto cfg = alpacacore::vendor::zwo::default_asiair_plus_rk3568_config();
    cfg.ports[3].pwm_enabled = true;
    auto driver = make_on(device, cfg);
    driver->set_connected(true);
    driver->set_switch_value(3, 50.0);
    auto& st = ioctl_stub();
    {
        std::unique_lock<std::mutex> lock(st.m);
        REQUIRE(st.cv.wait_for(lock, std::chrono::seconds(5),
                               [&] { return st.off_writes >= 2 && st.levels.size() >= 4 && st.levels.back() == 1; }));
    }
    driver->set_connected(false);
    const auto calls = stub_calls();
    REQUIRE(calls.size() >= 6);
    CHECK(calls[0] == IoctlCall{kLegacySetMode, 7, 1, false});
    CHECK(calls[1] == IoctlCall{kLegacyEnable, 7, 0, false});
    for (std::size_t i = 2; i < calls.size(); ++i) {
        CHECK(calls[i].request == kLegacySetLevel);
        CHECK(calls[i].index == 7);
        CHECK((calls[i].value == 0 || calls[i].value == 1));
    }
    CHECK(calls.back().value == 0);  // duty 50 settles ON, raw 0 on the legacy path
}
