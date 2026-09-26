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
#include <alpacacore/vendor/astroasis/astroasis_protocol_wrapper.h>
#include <hidapi.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace alpacacore::vendor::astroasis {

namespace {

constexpr unsigned short kVendorId = 0x338F;
constexpr unsigned short kProductId = 0xA0F0;
constexpr std::size_t kReportSize = 65;  // report ID byte + 64-byte HID report
constexpr int kDefaultTimeoutMs = 100;
constexpr int kHandshakeTimeoutMs = 1000;  // cmd 0x11 gets the long timeout in the vendor SDK

// hidapi's library-level entry points touch process-global state -- hid_init's
// ref-counted context and the backend's udev/libusb bus scan behind
// hid_enumerate/hid_open_path -- and hidapi does not serialize them for us
// (upstream documents only "different hid_device handles from different
// threads" as safe). Impl::mutex_ is per instance, so it cannot order one
// instance's enumerate (the focuserIndex scan runs inside set_connected(true)
// since #659: on the connection thread for an async connect, on the HTTP
// thread for a sync PUT Connected=true) against a second instance's connect.
// Every call into those entry points, from any thread, takes this one lock.
//
// Deliberately NOT held across hid_write/hid_read_timeout: those are per-handle
// I/O that hidapi already supports concurrently across distinct handles, and the
// connect handshake alone blocks in hid_read_timeout for up to
// kHandshakeTimeoutMs -- a global lock there would stall every other focuser's
// I/O (and every enumeration) behind one device's transaction for no safety
// gain.
//
// Lock order (AGENTS.md "driver mutex_ -> operation lock -> SDK-wrapper
// mutex"): driver mutex_ -> Impl::mutex_ -> this. Nothing may take Impl::mutex_
// while holding it; enumerate_astroasis_focusers() takes only this one.
//
// File-local because Astroasis is the only vendor linking hidapi. If a second
// one ever does, this must be promoted to a shared header -- two separate
// mutexes would serialize nothing.
//
// Two known, accepted costs. (1) enumerate_astroasis_focusers() holds this
// across a full udev/libusb bus scan (unavoidable: devs is hidapi-owned memory
// that must be copied out before hid_free_enumeration), so a by-index device
// creation on an HTTP thread can stall another focuser's connect/disconnect for
// tens of milliseconds on a busy USB tree -- and, since AstroasisFocuserDriver
// holds its own mutex_ across protocol_.connect()/disconnect(), every property
// read on that focuser queued behind them for the same duration. Bounded,
// never a deadlock. (2) A
// function-local static is destroyed at exit, before any static-lifetime object
// constructed earlier. What keeps that unreachable is NOT heap ownership: the
// DeviceRegistry singleton is constructed at startup registration and this mutex
// on the first hidapi call, so the mutex is destroyed FIRST, and a driver still
// held by the registry at static-destruction time would lock a destroyed mutex
// in ~Impl. The invariant is the explicit
// DeviceRegistry::instance().clear() in AlpacaHTTP/examples/simple_server's
// main(), which destroys every driver before main returns. Drop that call, or
// give a driver static lifetime, and this mutex must be leaked instead
// (`static auto* m = new std::mutex;`).
std::mutex& hid_global_mutex() {
    static std::mutex m;
    return m;
}

std::uint32_t to_big_endian(std::uint32_t host) {
    return ((host & 0x000000FFu) << 24) | ((host & 0x0000FF00u) << 8) | ((host & 0x00FF0000u) >> 8) |
           ((host & 0xFF000000u) >> 24);
}

std::uint32_t read_be32(const std::vector<std::uint8_t>& buf, std::size_t offset) {
    return (static_cast<std::uint32_t>(buf[offset]) << 24) | (static_cast<std::uint32_t>(buf[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(buf[offset + 2]) << 8) | static_cast<std::uint32_t>(buf[offset + 3]);
}

// NTC thermistor curve (B=3380K, 25C reference, 12-bit ADC 0-4095) for the
// internal board sensor. Byte-exact port of a helper inside OasisFocuser64.dll
// -- the formula and constants come straight out of its data section, but
// this has not been checked against a real reading yet.
int raw_adc_to_centidegrees(std::int32_t raw) {
    std::int32_t clamped = raw;
    if (clamped <= 0) {
        clamped = 1;
    } else if (clamped >= 0xfff) {
        clamped = 0xffe;
    }
    const double ratio = static_cast<double>(0xfff - clamped) / static_cast<double>(clamped);
    // cppcheck-suppress invalidFunctionArg
    // clamped is bounded to [1, 0xffe] above, so ratio = (0xfff-clamped)/clamped is always > 0
    // (minimum 1/4094 when clamped == 0xffe); cppcheck's value-flow analysis doesn't track that.
    const double t_kelvin = 3380.0 / (std::log(ratio) + 11.336575508117676);
    const double t_celsius = t_kelvin - 273.1499938964844;
    const double rounded = t_celsius + (t_celsius >= 0.0 ? 0.004999999888241291 : -0.004999999888241291);
    return static_cast<int>(rounded * 100.0);
}

// DS18B20-style raw digital sensor (1/16 degC per count) for the external probe.
int raw_digital_to_centidegrees(std::int16_t raw) {
    return static_cast<int>(std::lround(static_cast<double>(raw) * 0.0625 * 100.0));
}

}  // namespace

class AstroasisProtocolWrapper::Impl {
public:
    ~Impl() { disconnect(); }

    void connect(const std::string& hid_path) {
        std::lock_guard<std::mutex> lock(mutex_);
        // The caller (the driver's own mutex_, AsyncConnectable obligations
        // 4/5) is what actually guarantees a single in-flight connect; this
        // makes that invariant local instead of only living in the caller's
        // head. A throw, not a silent guard that closes and reopens: closing
        // and reopening would mask the caller bug that let this be reached
        // (silently leaking the existing handle's caller-visible state)
        // instead of surfacing it. Not an assert: this file has no other
        // <cassert> use, and an assert here would abort the whole process in
        // every NDEBUG-undefined build (CI, dev, the TSan job) -- worse than
        // the bug it guards against.
        if (device_) {
            throw AlpacaException("Astroasis focuser: connect() called while already connected (caller bug)",
                                  AlpacaError::DriverException);
        }
        {
            std::lock_guard<std::mutex> hid_lock(hid_global_mutex());
            // Checked, like the enumerate path: a failed init would otherwise
            // surface one call later as an indistinguishable "failed to open"
            // naming the path, which sends the reader after a cabling or
            // permissions problem that isn't there.
            if (hid_init() != 0) {
                throw AlpacaException("hidapi initialization failed", AlpacaError::NotConnected);
            }
            device_ = hid_open_path(hid_path.c_str());
        }
        if (!device_) {
            throw AlpacaException("Failed to open Astroasis focuser HID device: " + hid_path,
                                  AlpacaError::NotConnected);
        }

        // Connect handshake reverse-engineered from AOFocuserOpen: cmd 0x11
        // carries a tick-count-like nonce in HOST byte order (the one command
        // NOT big-endian -- confirmed by disassembly) and expects a 1-byte
        // ack; cmd 0x10 has NO payload and expects a 4-byte response (on real
        // hardware this echoed back the cmd-0x11 nonce -- likely a session
        // token confirmation, or possibly the cached protocol/firmware
        // version the vendor SDK uses to pick the GetStatus/GetConfig
        // response layout; not confirmed which).
        std::uint32_t ticks = static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        std::array<std::uint8_t, 4> handshake1{};
        std::memcpy(handshake1.data(), &ticks, 4);
        try {
            send_command(0x11, handshake1.data(), 4, 1, kHandshakeTimeoutMs);
            send_command(0x10, nullptr, 0, 4, kDefaultTimeoutMs);
        } catch (...) {
            // connected_ is already false here: it's only ever true when
            // device_ != nullptr, and the already-connected throw above
            // guarantees device_ was nullptr on entry, in every build.
            close_device_locked();
            throw;
        }

        connected_ = true;
    }

    void disconnect() {
        std::lock_guard<std::mutex> lock(mutex_);
        // No protocol command here -- AOFocuserClose only tears down the OS
        // handle, it does not send anything to the device.
        close_device_locked();
        connected_ = false;
    }

    bool is_connected() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connected_;
    }

    AstroasisProtocolWrapper::Status get_status() {
        std::lock_guard<std::mutex> lock(mutex_);
        require_connected();
        // Try the older/shorter firmware response layout first, then the
        // newer/longer one -- see the header TODO: the vendor SDK instead
        // caches a protocol version from the connect handshake and picks the
        // layout from that, which this wrapper does not replicate.
        std::vector<std::uint8_t> resp;
        try {
            resp = send_command(0x32, nullptr, 0, 14, kDefaultTimeoutMs);
        } catch (const AlpacaException& ex) {
            if (ex.error_code() != AlpacaError::InvalidValue) {
                throw;
            }
            resp = send_command(0x32, nullptr, 0, 60, kDefaultTimeoutMs);
        }

        AstroasisProtocolWrapper::Status status;
        const auto raw_int = static_cast<std::int32_t>(read_be32(resp, 0));
        const auto raw_ext = static_cast<std::int32_t>(read_be32(resp, 4));
        status.temperature_internal = raw_adc_to_centidegrees(raw_int) / 100.0;
        if (raw_ext == static_cast<std::int32_t>(0x80000000)) {
            status.temperature_external_valid = false;
        } else {
            const auto raw_ext16 = static_cast<std::int16_t>(raw_ext & 0xFFFF);
            status.temperature_external = raw_digital_to_centidegrees(raw_ext16) / 100.0;
            status.temperature_external_valid = resp[8] != 0;  // temperatureDetection flag
        }
        status.moving = resp[9] != 0;
        status.position = static_cast<std::int32_t>(read_be32(resp, 10));
        return status;
    }

    int get_max_step() {
        std::lock_guard<std::mutex> lock(mutex_);
        require_connected();
        std::vector<std::uint8_t> resp;
        try {
            resp = send_command(0x30, nullptr, 0, 18, kDefaultTimeoutMs);
        } catch (const AlpacaException& ex) {
            if (ex.error_code() != AlpacaError::InvalidValue) {
                throw;
            }
            resp = send_command(0x3a, nullptr, 0, 40, kDefaultTimeoutMs);
        }
        // Response payload is [mask:4][maxStep:4][...] -- confirmed on real
        // hardware: offset 0 read back as 0xFFFFFFFF, matching the SDK's
        // MASK_ALL constant, not a plausible step count.
        return static_cast<std::int32_t>(read_be32(resp, 4));
    }

    void move_to(int position) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_connected();
        std::array<std::uint8_t, 4> payload{};
        const std::uint32_t be = to_big_endian(static_cast<std::uint32_t>(position));
        std::memcpy(payload.data(), &be, 4);
        send_command(0x36, payload.data(), 4, 1, kDefaultTimeoutMs);
    }

    void stop_move() {
        std::lock_guard<std::mutex> lock(mutex_);
        require_connected();
        send_command(0x37, nullptr, 0, 1, kDefaultTimeoutMs);
    }

private:
    // Caller must hold mutex_ and must NOT hold hid_global_mutex() -- it is
    // non-recursive, so widening a global-lock scope over a call site of this
    // (verified by mutation: all of connect()) self-deadlocks right here.
    // Closes under hid_global_mutex() and clears
    // device_ so the two close sites (the connect handshake's failure path and
    // disconnect()) cannot drift apart on either the lock or the clear.
    void close_device_locked() {
        if (!device_) {
            return;
        }
        std::lock_guard<std::mutex> hid_lock(hid_global_mutex());
        hid_close(device_);
        device_ = nullptr;
    }

    void require_connected() const {
        if (!connected_ || !device_) {
            throw AlpacaException("Astroasis focuser not connected", AlpacaError::NotConnected);
        }
    }

    // Sends [reportId=0][cmd][len][payload...] and reads back
    // [cmd echo][responseLen][responsePayload...], returning responsePayload.
    // Throws AlpacaError::InvalidValue on a cmd/length mismatch (the signal
    // callers use to retry with a different expected length) and
    // AlpacaError::DriverException on I/O failure.
    std::vector<std::uint8_t> send_command(std::uint8_t cmd, const std::uint8_t* payload, std::uint8_t payload_len,
                                           std::uint8_t expected_resp_len, int timeout_ms) {
        std::array<std::uint8_t, kReportSize> out{};
        out[0] = 0;  // report ID
        out[1] = cmd;
        out[2] = payload_len;
        if (payload != nullptr && payload_len > 0) {
            std::memcpy(out.data() + 3, payload, payload_len);
        }

        // Drain any stale input report before writing, matching the vendor
        // SDK's transact() helper (a non-blocking read right before the write).
        std::array<std::uint8_t, kReportSize - 1> drain{};
        hid_read_timeout(device_, drain.data(), drain.size(), 0);

        const int written = hid_write(device_, out.data(), out.size());
        if (written < 0) {
            throw AlpacaException("Astroasis HID write failed", AlpacaError::DriverException);
        }

        // hidapi's hidraw backend does not prefix reads with the report-ID
        // byte for a report-ID-0 device, so in[0] is the device's echoed cmd
        // byte directly -- unverified against real hardware.
        std::array<std::uint8_t, kReportSize - 1> in{};
        const int bytes_read = hid_read_timeout(device_, in.data(), in.size(), timeout_ms);
        if (bytes_read <= 0) {
            throw AlpacaException("Astroasis HID read timed out", AlpacaError::DriverException);
        }
        // hidraw delivers full 64-byte reports today, but guard against a
        // short read before slicing: a future variable-length command would
        // otherwise silently return zero-padded data (issue #154).
        if (bytes_read < 2 + static_cast<int>(expected_resp_len)) {
            throw AlpacaException("Astroasis HID response truncated", AlpacaError::DriverException);
        }
        if (in[0] != cmd) {
            throw AlpacaException("Astroasis HID response command mismatch", AlpacaError::DriverException);
        }
        if (in[1] != expected_resp_len) {
            throw AlpacaException("Astroasis HID response length mismatch", AlpacaError::InvalidValue);
        }
        return std::vector<std::uint8_t>(in.begin() + 2, in.begin() + 2 + expected_resp_len);
    }

    mutable std::mutex mutex_;
    hid_device* device_ = nullptr;
    bool connected_ = false;
};

AstroasisProtocolWrapper::AstroasisProtocolWrapper() : impl_(std::make_unique<Impl>()) {}
AstroasisProtocolWrapper::~AstroasisProtocolWrapper() = default;

void AstroasisProtocolWrapper::connect(const std::string& hid_path) { impl_->connect(hid_path); }

void AstroasisProtocolWrapper::disconnect() { impl_->disconnect(); }

bool AstroasisProtocolWrapper::is_connected() const { return impl_->is_connected(); }

AstroasisProtocolWrapper::Status AstroasisProtocolWrapper::get_status() { return impl_->get_status(); }

int AstroasisProtocolWrapper::get_max_step() { return impl_->get_max_step(); }

void AstroasisProtocolWrapper::move_to(int position) { impl_->move_to(position); }

void AstroasisProtocolWrapper::stop_move() { impl_->stop_move(); }

std::vector<AstroasisPortInfo> enumerate_astroasis_focusers() {
    std::vector<AstroasisPortInfo> results;
    // Held across the whole init/enumerate/copy/free sequence: devs points into
    // hidapi-owned memory that hid_free_enumeration must reclaim, so the list
    // has to be copied out before the lock drops.
    std::lock_guard<std::mutex> hid_lock(hid_global_mutex());
    if (hid_init() != 0) {
        return results;
    }

    hid_device_info* devs = hid_enumerate(kVendorId, kProductId);
    for (hid_device_info* cur = devs; cur != nullptr; cur = cur->next) {
        if (cur->path == nullptr) {
            continue;
        }
        AstroasisPortInfo info;
        info.hid_path = cur->path;
        results.push_back(std::move(info));
    }
    hid_free_enumeration(devs);
    return results;
}

}  // namespace alpacacore::vendor::astroasis
