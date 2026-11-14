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

// The libgpiod and ioctl() stubs of zwo_asiair_stubs.h (issues #772, #300, #1862). This is the only file in
// alpacacore_tests that may define these C symbols.

#include "zwo_asiair_stubs.h"

#include <dlfcn.h>
#include <gpiod.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace alpacacore::test {

namespace {

struct GpiodStub {
    std::mutex m;
    std::array<int, kStubLines> level{};
    std::array<int, kStubLines> level_at_release{};
    std::vector<unsigned int> requested;  // offsets of the live request, in request order
    int level_changes = 0;                // writes that changed a line's level
    bool claimed = false;                 // a request is live
    int requests = 0;
    int releases = 0;
    char chip = 0;     // identity of the stub chip
    char request = 0;  // identity of the stub request
};

GpiodStub& stub() {
    static GpiodStub s;
    return s;
}

gpiod_chip* stub_chip() { return reinterpret_cast<gpiod_chip*>(&stub().chip); }
gpiod_line_request* stub_request() { return reinterpret_cast<gpiod_line_request*>(&stub().request); }

template <typename Fn>
Fn real_symbol(const char* name) {
    return reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name));
}

void stub_drive_locked(unsigned int offset, int level) {
    if (offset < kStubLines && stub().level[offset] != level) {
        stub().level[offset] = level;
        ++stub().level_changes;
    }
}

// Drives the lines @p cfg sets to output, in its offset order; returns that order.
std::vector<unsigned int> stub_apply_config_locked(gpiod_line_config* cfg) {
    std::vector<unsigned int> offsets(::gpiod_line_config_get_num_configured_offsets(cfg));
    offsets.resize(::gpiod_line_config_get_configured_offsets(cfg, offsets.data(), offsets.size()));
    for (const unsigned int offset : offsets) {
        gpiod_line_settings* settings = ::gpiod_line_config_get_line_settings(cfg, offset);
        if (settings == nullptr) {
            continue;
        }
        if (::gpiod_line_settings_get_direction(settings) == GPIOD_LINE_DIRECTION_OUTPUT) {
            stub_drive_locked(offset,
                              ::gpiod_line_settings_get_output_value(settings) == GPIOD_LINE_VALUE_ACTIVE ? 1 : 0);
        }
        ::gpiod_line_settings_free(settings);
    }
    return offsets;
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

constexpr unsigned long kSetLevel = 0x40084302UL;  // PWM_GPIO_SET_LEVEL
constexpr unsigned long kSetMode = 0x40084307UL;   // PWM_GPIO_SET_MODE
constexpr unsigned long kEnable = 0x40044306UL;    // PWM_GPIO_ENABLE

}  // namespace

int stub_level_at_release(unsigned int offset) {
    std::lock_guard<std::mutex> lock(stub().m);
    return offset < kStubLines ? stub().level_at_release[offset] : -1;
}

void stub_reset_levels(int level) {
    std::lock_guard<std::mutex> lock(stub().m);
    stub().level.fill(level);
    stub().level_changes = 0;
}

void stub_set_level(unsigned int offset, int level) {
    std::lock_guard<std::mutex> lock(stub().m);
    stub().level[offset] = level;
}

int stub_level(unsigned int offset) {
    std::lock_guard<std::mutex> lock(stub().m);
    return stub().level[offset];
}

int stub_level_changes() {
    std::lock_guard<std::mutex> lock(stub().m);
    return stub().level_changes;
}

int stub_requests() {
    std::lock_guard<std::mutex> lock(stub().m);
    return stub().requests;
}

int stub_releases() {
    std::lock_guard<std::mutex> lock(stub().m);
    return stub().releases;
}

bool stub_claimed() {
    std::lock_guard<std::mutex> lock(stub().m);
    return stub().claimed;
}

IoctlStub& ioctl_stub() {
    static IoctlStub s;
    return s;
}

StubDeviceFile::StubDeviceFile() {
    const int fd = ::mkstemp(path);
    if (fd >= 0) ::close(fd);
    auto& st = ioctl_stub();
    std::lock_guard<std::mutex> lock(st.m);
    st.path = path;
    st.levels.clear();
    st.last_level.clear();
    st.off_writes = 0;
    st.requests = 0;
}

StubDeviceFile::~StubDeviceFile() {
    auto& st = ioctl_stub();
    {
        std::lock_guard<std::mutex> lock(st.m);
        st.path.clear();
    }
    ::unlink(path);
}

int StubDeviceFile::open_fds() const {
    int n = 0;
    for (int fd = 0; fd < 1024; ++fd) {
        char buf[256];
        const std::string link = "/proc/self/fd/" + std::to_string(fd);
        const ssize_t len = ::readlink(link.c_str(), buf, sizeof(buf) - 1);
        if (len > 0 && std::string(buf, static_cast<std::size_t>(len)) == path) ++n;
    }
    return n;
}

}  // namespace alpacacore::test

using alpacacore::test::kStubChip;
using alpacacore::test::real_symbol;
using alpacacore::test::stub;
using alpacacore::test::stub_chip;
using alpacacore::test::stub_request;

extern "C" {

gpiod_chip* gpiod_chip_open(const char* path) {
    if (path != nullptr && std::strcmp(path, kStubChip) == 0) {
        return stub_chip();
    }
    static const auto real = real_symbol<gpiod_chip* (*)(const char*)>("gpiod_chip_open");
    if (real == nullptr) {
        errno = ENOSYS;
        return nullptr;
    }
    return real(path);
}

void gpiod_chip_close(gpiod_chip* chip) {
    if (chip == stub_chip()) {
        return;
    }
    static const auto real = real_symbol<void (*)(gpiod_chip*)>("gpiod_chip_close");
    if (real != nullptr) {
        real(chip);
    }
}

gpiod_line_request* gpiod_chip_request_lines(gpiod_chip* chip, gpiod_request_config* req_cfg,
                                             gpiod_line_config* line_cfg) {
    if (chip == stub_chip()) {
        std::lock_guard<std::mutex> lock(stub().m);
        if (stub().claimed) {
            errno = EBUSY;  // the chip refuses a line another request holds
            return nullptr;
        }
        stub().requested = alpacacore::test::stub_apply_config_locked(line_cfg);
        stub().level_at_release.fill(-1);
        stub().claimed = true;
        ++stub().requests;
        (void)req_cfg;
        return stub_request();
    }
    static const auto real =
        real_symbol<gpiod_line_request* (*)(gpiod_chip*, gpiod_request_config*, gpiod_line_config*)>(
            "gpiod_chip_request_lines");
    if (real == nullptr) {
        errno = ENOSYS;
        return nullptr;
    }
    return real(chip, req_cfg, line_cfg);
}

int gpiod_line_request_set_value(gpiod_line_request* request, unsigned int offset, gpiod_line_value value) {
    if (request == stub_request()) {
        std::lock_guard<std::mutex> lock(stub().m);
        if (!stub().claimed) {
            errno = EBADF;  // a write through a released request
            return -1;
        }
        alpacacore::test::stub_drive_locked(offset, value == GPIOD_LINE_VALUE_ACTIVE ? 1 : 0);
        return 0;
    }
    static const auto real =
        real_symbol<int (*)(gpiod_line_request*, unsigned int, gpiod_line_value)>("gpiod_line_request_set_value");
    if (real == nullptr) {
        errno = ENOSYS;
        return -1;
    }
    return real(request, offset, value);
}

int gpiod_line_request_get_values(gpiod_line_request* request, gpiod_line_value* values) {
    if (request == stub_request()) {
        std::lock_guard<std::mutex> lock(stub().m);
        for (std::size_t i = 0; i < stub().requested.size(); ++i) {
            values[i] = stub().level[stub().requested[i]] != 0 ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
        }
        return 0;
    }
    static const auto real =
        real_symbol<int (*)(gpiod_line_request*, gpiod_line_value*)>("gpiod_line_request_get_values");
    if (real == nullptr) {
        errno = ENOSYS;
        return -1;
    }
    return real(request, values);
}

int gpiod_line_request_reconfigure_lines(gpiod_line_request* request, gpiod_line_config* config) {
    if (request == stub_request()) {
        std::lock_guard<std::mutex> lock(stub().m);
        alpacacore::test::stub_apply_config_locked(config);
        return 0;
    }
    static const auto real =
        real_symbol<int (*)(gpiod_line_request*, gpiod_line_config*)>("gpiod_line_request_reconfigure_lines");
    if (real == nullptr) {
        errno = ENOSYS;
        return -1;
    }
    return real(request, config);
}

void gpiod_line_request_release(gpiod_line_request* request) {
    if (request == stub_request()) {
        std::lock_guard<std::mutex> lock(stub().m);
        stub().level_at_release = stub().level;
        if (stub().claimed) ++stub().releases;
        stub().claimed = false;
        return;
    }
    static const auto real = real_symbol<void (*)(gpiod_line_request*)>("gpiod_line_request_release");
    if (real != nullptr) {
        real(request);
    }
}

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    if ((request == alpacacore::test::kSetLevel || request == alpacacore::test::kSetMode ||
         request == alpacacore::test::kEnable) &&
        alpacacore::test::fd_is_stub(fd)) {
        auto& st = alpacacore::test::ioctl_stub();
        std::lock_guard<std::mutex> lock(st.m);
        ++st.requests;
        if (request == alpacacore::test::kSetLevel) {
            const auto* level = static_cast<const std::int32_t*>(arg);  // gpio_level_t {index, level}
            st.levels.push_back(level[1]);
            st.last_level[level[0]] = level[1];
            if (level[1] == 1) {
                ++st.off_writes;
            }
        }
        st.cv.notify_all();
        return 0;
    }
    using Fn = int (*)(int, unsigned long, void*);
    static const auto real = real_symbol<Fn>("ioctl");
    if (real == nullptr) {
        errno = ENOSYS;
        return -1;
    }
    return real(fd, request, arg);
}

}  // extern "C"
