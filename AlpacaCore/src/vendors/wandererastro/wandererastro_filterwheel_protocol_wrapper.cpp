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
#include <alpacacore/util/serial_by_id_scan.h>
#include <alpacacore/util/serial_io.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/vendor/wandererastro/wandererastro_filterwheel_protocol_wrapper.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace alpacacore::vendor::wandererastro {

namespace {

// Both wheel models stream a token beginning with this prefix (WSFW508 for the
// SFW50/SFW50S, WSFW368 for the SFW36S).
constexpr char kFilterWheelModelPrefix[] = "WSFW";
constexpr int MAX_TOKEN_LEN = 64;

using util::is_serial_port_in_use;
using util::mark_serial_port_closed;
using util::mark_serial_port_open;

// Configure an already-open POSIX fd for 19200 8N1 raw I/O. HUPCL is cleared so
// DTR stays asserted on close (CH340 adapters pulse the MCU reset line via DTR
// on open; keeping DTR high across close avoids a reset on reopen).
#ifndef _WIN32
bool configure_serial_fd(int fd) {
    struct termios tty {};
    if (tcgetattr(fd, &tty) != 0) {
        return false;
    }
    cfsetospeed(&tty, B19200);
    cfsetispeed(&tty, B19200);
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CREAD | CLOCAL;
    tty.c_cflag &= ~HUPCL;  // keep DTR high on close — avoids CH340 MCU reset on reopen
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_oflag &= ~OPOST;
    tty.c_oflag &= ~ONLCR;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 5;  // 0.5s per-read timeout so the reader loop can poll its stop flag
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        return false;
    }
    if (!util::clear_nonblocking(fd)) {
        return false;
    }
    tcflush(fd, TCIOFLUSH);
    return true;
}
#endif

bool token_is_model(const std::string& token) { return token.rfind(kFilterWheelModelPrefix, 0) == 0; }

// Incremental 'A'-delimited token-stream parser, anchored on the model token.
//
// The wheel's status frame is
//   <model>A<firmware>A<position>A<letters>A<8 per-filter fields>A<deviceID>A
// but line terminators may be omitted entirely while the wheel is moving, so a
// line-based reader (like the WandererCover's) can stall mid-move. Instead the
// stream is split on 'A' (filter letters are restricted to B-Z by the vendor
// precisely so 'A' stays unambiguous as the delimiter); CR/LF also end a token
// and empty tokens are skipped. Every model token restarts the field counter,
// so the parser resynchronises on each frame regardless of where it joined the
// stream.
class TokenStreamParser {
public:
    // Feed one character. Returns true when a field was committed into `status`
    // (the caller decides what to do with the update).
    bool feed(char ch, FilterWheelStatus& status) {
        if (ch == 'A' || ch == '\r' || ch == '\n') {
            if (token_.empty()) {
                return false;
            }
            std::string token;
            token.swap(token_);
            return handle_token(token, status);
        }
        if (token_.size() < MAX_TOKEN_LEN) {
            token_ += ch;
        } else {
            token_.clear();  // runaway garbage — drop and resync on the next model token
            field_index_ = -1;
        }
        return false;
    }

private:
    bool handle_token(const std::string& token, FilterWheelStatus& status) {
        if (token_is_model(token)) {
            pending_ = FilterWheelStatus{};
            pending_.model = token;
            field_index_ = 0;
            return false;
        }
        if (field_index_ < 0) {
            return false;  // not synced to a frame yet
        }
        ++field_index_;
        try {
            switch (field_index_) {
                case 1:
                    pending_.firmware_version = std::stoi(token);
                    return false;
                case 2: {
                    const int position = std::stoi(token);
                    if (position < 1 || position > kFilterWheelSlotCount) {
                        field_index_ = -1;  // implausible — resync on the next model token
                        return false;
                    }
                    pending_.position = position;
                    pending_.valid = true;
                    // Model + firmware + position are enough to publish; the
                    // remaining fields refine the same frame below.
                    status = merge_optional_fields(pending_, status);
                    return true;
                }
                case 3:
                    if (pending_.valid) {
                        pending_.letters = token;
                        status = merge_optional_fields(pending_, status);
                        return true;
                    }
                    return false;
                case 12:
                    if (pending_.valid) {
                        pending_.device_id = std::stoi(token);
                        status = merge_optional_fields(pending_, status);
                        return true;
                    }
                    return false;
                default:
                    return false;  // per-filter fields (4-11) are not exposed
            }
        } catch (const std::exception&) {
            field_index_ = -1;  // malformed numeric field — resync on the next model token
            return false;
        }
    }

    // Carry best-effort fields (letters, device ID) forward from the previous
    // published status when this frame hasn't reached them yet, so a position
    // update mid-frame doesn't blank them.
    static FilterWheelStatus merge_optional_fields(const FilterWheelStatus& fresh, const FilterWheelStatus& prev) {
        FilterWheelStatus merged = fresh;
        if (merged.letters.empty()) {
            merged.letters = prev.letters;
        }
        if (merged.device_id == 0) {
            merged.device_id = prev.device_id;
        }
        return merged;
    }

    std::string token_;
    FilterWheelStatus pending_;
    int field_index_ = -1;  // -1 = waiting for a model token
};

// Open a candidate port and listen passively for up to ~3s for the streamed
// status frame. Returns true if the port identifies a Wanderer filter wheel.
// No bytes are written during the probe.
bool probe_port(const std::string& port_path, FilterWheelPortInfo& info) {
#ifndef _WIN32
    int fd = open(port_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }
    if (!configure_serial_fd(fd)) {
        close(fd);
        return false;
    }

    // Re-check after opening: a driver may have claimed this port in the window
    // between the caller's is_serial_port_in_use() check and this open(). Bail
    // rather than stealing bytes from that device's stream.
    if (is_serial_port_in_use(port_path)) {
        close(fd);
        return false;
    }

    TokenStreamParser parser;
    FilterWheelStatus status;
    bool found = false;
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() <
           3000) {
        char ch = 0;
        ssize_t r = read(fd, &ch, 1);
        if (r == 1) {
            if (parser.feed(ch, status) && status.valid) {
                found = true;
                break;
            }
        } else if (r < 0 && errno != EINTR) {
            // Persistent read error (device unplugged mid-probe): VTIME
            // rate-limits only the no-data path, so back off explicitly.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    // Re-apply HUPCL-clear before closing so DTR stays asserted.
    struct termios tty {};
    if (tcgetattr(fd, &tty) == 0) {
        tty.c_cflag &= ~HUPCL;
        tcsetattr(fd, TCSANOW, &tty);
    }
    close(fd);

    if (found) {
        info.port_path = port_path;
        info.model = status.model;
        info.firmware_version = status.firmware_version;
        return true;
    }
#else
    (void)port_path;
    (void)info;
#endif
    return false;
}

#ifndef _WIN32
// The raw /dev/ttyUSBn fallback below has no by-id symlink name to filter
// on, so without this check it would open EVERY serial device on the box --
// unrelated FTDI dongles, GPS receivers, or a mount controller sharing the
// ttyUSB namespace -- not just CH340/CH341-class filter wheel hardware.
// Filters the raw fallback exactly as narrowly as the by-id loop below.
bool raw_port_looks_like_filterwheel_candidate(const std::string& port_path) {
    auto descriptor = alpacacore::util::read_raw_tty_usb_descriptor(port_path);
    if (!descriptor) return false;
    return alpacacore::util::usb_tty_descriptor_matches(*descriptor, {"1a86", "CH340", "CH341", "USB_Serial"});
}
#endif  // _WIN32

}  // namespace

std::vector<FilterWheelPortInfo> enumerate_wanderer_filterwheel_ports() {
    std::vector<FilterWheelPortInfo> results;

#ifndef _WIN32
    // Resolved (canonical) paths already probed via by-id, so the raw-node
    // pass below never re-opens a port the by-id pass already tried.
    std::set<std::string> probed;

    const std::filesystem::path serial_by_id("/dev/serial/by-id");
    if (std::filesystem::exists(serial_by_id)) {
        for (const auto& sym : alpacacore::util::list_serial_by_id(serial_by_id)) {
            const std::string& name = sym.name;

            // Wanderer boards use a CH340/CH341 USB-serial adapter (vendor 1a86).
            bool is_candidate = (name.find("USB_Serial") != std::string::npos) ||
                                (name.find("CH340") != std::string::npos) ||
                                (name.find("CH341") != std::string::npos) || (name.find("1a86") != std::string::npos);
            if (!is_candidate) continue;

            // Use the error_code overload: a device unplugged between the directory
            // scan and here leaves a dangling symlink, and the throwing canonical()
            // would abort the whole enumeration. Skip the stale entry.
            std::error_code ec;
            std::string resolved = std::filesystem::canonical(sym.path, ec).string();
            if (ec) continue;
            probed.insert(resolved);

            // Don't probe a port another connected device is streaming on.
            if (is_serial_port_in_use(resolved)) continue;
            std::string probe_msg = "Probing ";
            probe_msg.append(resolved).append(" (").append(name).append(") for filter wheel...");
            ALPACA_LOG_INFO("WandererAstro", probe_msg);

            FilterWheelPortInfo info;
            if (probe_port(resolved, info)) {
                info.device_id = name;
                ALPACA_LOG_INFO("WandererAstro", "Found " + info.model + " on " + resolved + " (firmware " +
                                                     std::to_string(info.firmware_version) + ")");
                results.push_back(info);
            }
        }
    }

    // Always also probe raw /dev/ttyUSB* nodes directly, not only when
    // /dev/serial/by-id is absent. Generic CH340/CH341 adapters (used by
    // this filter wheel, and commonly also by other devices on the same box)
    // report identical descriptor strings with no per-device serial number,
    // so when two such adapters are plugged in at once, udev's by-id naming
    // collides and only ONE of them gets a symlink -- the other is silently
    // absent from by-id even though the directory itself exists, so the loop
    // above never sees it. Deduplicated by resolved canonical path so a port
    // already tried via by-id isn't opened (and reset) a second time.
    for (int i = 0; i < 10; ++i) {
        std::string port = "/dev/ttyUSB" + std::to_string(i);
        if (!std::filesystem::exists(port)) continue;

        std::error_code ec;
        std::string resolved = std::filesystem::canonical(port, ec).string();
        if (ec) continue;
        if (probed.count(resolved) != 0) continue;
        if (!raw_port_looks_like_filterwheel_candidate(resolved)) continue;
        probed.insert(resolved);

        if (is_serial_port_in_use(resolved)) continue;  // held by another connected serial device
        ALPACA_LOG_INFO("WandererAstro", "Probing " + resolved + " for filter wheel...");
        FilterWheelPortInfo info;
        if (probe_port(resolved, info)) {
            ALPACA_LOG_INFO("WandererAstro", "Found " + info.model + " on " + resolved + " (firmware " +
                                                 std::to_string(info.firmware_version) + ")");
            results.push_back(info);
        }
    }
#endif

    return results;
}

class WandererFilterWheelProtocolWrapper::Impl {
public:
    Impl() = default;

    ~Impl() { disconnect(); }

    std::string connect(const FilterWheelConnectionConfig& config) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Precondition: connect() requires a disconnected wrapper. While a
            // session is live the reader thread is joinable; re-spawning it
            // would overwrite a joinable std::thread and call std::terminate().
            // Every connect() failure path joins the reader before returning,
            // so connected_ == false guarantees no live thread.
            if (connected_) {
                throw AlpacaException("Wanderer filter wheel already connected; call disconnect() first",
                                      AlpacaError::InvalidOperation);
            }
            config_ = config;
            open_serial();
        }

        // Start the background reader that keeps the latest streamed status.
        running_.store(true);
        reader_thread_ = std::thread([this] { reader_loop(); });

        // Wait for the first valid frame that identifies a Wanderer wheel.
        auto start = std::chrono::steady_clock::now();
        const int timeout_ms = config.serial_timeout_s * 1000;
        FilterWheelStatus s;
        while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() <
               timeout_ms) {
            s = get_status();
            if (s.valid) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (!s.valid) {
            teardown_after_failed_connect();
            throw AlpacaException("No Wanderer filter wheel status detected on " + config.serial_port,
                                  AlpacaError::NotConnected);
        }
        if (s.firmware_version < kFilterWheelMinFirmware) {
            teardown_after_failed_connect();
            throw AlpacaException("Wanderer filter wheel firmware " + std::to_string(s.firmware_version) +
                                      " is too old (minimum " + std::to_string(kFilterWheelMinFirmware) +
                                      "); update it with the vendor's WandererEmpire application",
                                  AlpacaError::NotConnected);
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            connected_ = true;
        }
        ALPACA_LOG_INFO("WandererAstro", "Connected to " + s.model + " filter wheel (firmware " +
                                             std::to_string(s.firmware_version) + ")");
        return s.model;
    }

    void disconnect() {
        stop_reader();  // no concurrent writer of status_ after this returns
        // Invalidate the cached status BEFORE clearing connected_, so there is
        // no window where connected_ is already false but get_status() still
        // returns a stale valid=true frame.
        {
            std::lock_guard<std::mutex> status_lock(status_mutex_);
            status_ = FilterWheelStatus{};
            firmware_date_.clear();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = false;
        close_serial();
    }

    bool is_connected() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connected_;
    }

    FilterWheelStatus get_status() const {
        std::lock_guard<std::mutex> lock(status_mutex_);
        return status_;
    }

    std::optional<std::string> get_firmware_date() const {
        std::lock_guard<std::mutex> lock(status_mutex_);
        if (firmware_date_.empty()) {
            return std::nullopt;
        }
        return firmware_date_;
    }

    void select_filter(int slot) {
        // Wrapper-level precondition; the driver validates the (0-based) Alpaca
        // position range before translating to this 1-based slot.
        if (slot < 1 || slot > kFilterWheelSlotCount) {
            throw AlpacaException("Filter slot " + std::to_string(slot) + " out of range [1, " +
                                      std::to_string(kFilterWheelSlotCount) + "]",
                                  AlpacaError::InvalidValue);
        }
        send_command(std::to_string(2000 + slot));
    }

    void calibrate() { send_command("1500002"); }

private:
    void ensure_connected_locked() const {
        if (!connected_) {
            throw AlpacaException("Wanderer filter wheel not connected", AlpacaError::NotConnected);
        }
    }

    void send_command(const std::string& code) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        // Commands are '\r'-terminated (unlike the WandererCover's '\n'),
        // matching the vendor's INDI reference byte-for-byte.
        const std::string payload = code + "\r";
        ALPACA_LOG_TRACE("WandererAstro", "FilterWheel command: " + code);
#ifdef _WIN32
        std::size_t total = 0;
        while (total < payload.size()) {
            DWORD written = 0;
            // Guard written == 0: a WriteFile that returns TRUE with 0 bytes
            // would otherwise spin this loop forever while holding mutex_.
            if (!WriteFile(serial_handle_, payload.data() + total, static_cast<DWORD>(payload.size() - total), &written,
                           nullptr) ||
                written == 0) {
                throw AlpacaException("Serial write failed", AlpacaError::DriverException);
            }
            total += written;
        }
#else
        if (!util::write_all(serial_fd_, payload.data(), payload.size())) {
            throw AlpacaException("Serial write failed: " + std::string(std::strerror(errno)),
                                  AlpacaError::DriverException);
        }
#endif
    }

    void reader_loop() {
        TokenStreamParser parser;
        while (running_.load()) {
            char ch = 0;
            bool got = false;
#ifdef _WIN32
            HANDLE handle;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                handle = serial_handle_;
            }
            if (handle == INVALID_HANDLE_VALUE) break;
            DWORD bytes_read = 0;
            if (ReadFile(handle, &ch, 1, &bytes_read, nullptr)) {
                if (bytes_read == 1) {
                    got = true;
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
#else
            int fd;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                fd = serial_fd_;
            }
            if (fd < 0) break;
            ssize_t r = ::read(fd, &ch, 1);
            if (r == 1) {
                got = true;
            } else if (r < 0 && errno != EINTR) {
                // Persistent read error (device unplugged): VTIME rate-limits
                // the no-data path but not errors, so back off to avoid a CPU
                // spin until stop_reader() runs.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
#endif
            if (!got) {
                continue;  // VTIME timeout — loop and re-check running_
            }
            FilterWheelStatus updated;
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                updated = status_;
                if (!parser.feed(ch, updated)) {
                    continue;
                }
                status_ = updated;
                // Cache the firmware date once (YYYYMMDD int -> YYYY-MM-DD).
                if (firmware_date_.empty() && updated.valid && updated.firmware_version > 0) {
                    const int fw = updated.firmware_version;
                    const int year = fw / 10000;
                    const int month = (fw / 100) % 100;
                    const int day = fw % 100;
                    char buf[16];
                    if (month >= 1 && month <= 12 && day >= 1 && day <= 31) {
                        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
                    } else {
                        // Not a plausible YYYYMMDD — surface the raw integer
                        // rather than an invalid date.
                        std::snprintf(buf, sizeof(buf), "%d", fw);
                    }
                    firmware_date_ = buf;
                }
            }
        }
    }

    void stop_reader() {
        running_.store(false);
        if (reader_thread_.joinable()) {
            reader_thread_.join();
        }
    }

    // Common teardown for connect() failure paths: join the reader, drop any
    // cached status (a firmware-rejected connect has already received a valid
    // frame), then close the port under the lock. Leaves the wrapper fully
    // reset and reusable for a retry, matching the disconnect() contract that
    // get_status() is invalid whenever the wrapper is disconnected.
    void teardown_after_failed_connect() {
        stop_reader();
        {
            std::lock_guard<std::mutex> status_lock(status_mutex_);
            status_ = FilterWheelStatus{};
            firmware_date_.clear();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        close_serial();
    }

    void open_serial() {
#ifdef _WIN32
        std::string port_name = "\\\\.\\" + config_.serial_port;
        serial_handle_ =
            CreateFileA(port_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (serial_handle_ == INVALID_HANDLE_VALUE) {
            throw AlpacaException("Failed to open serial port: " + config_.serial_port, AlpacaError::NotConnected);
        }
        DCB dcb = {};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(serial_handle_, &dcb)) {
            CloseHandle(serial_handle_);
            serial_handle_ = INVALID_HANDLE_VALUE;
            throw AlpacaException("Failed to get serial port state", AlpacaError::DriverException);
        }
        dcb.BaudRate = static_cast<DWORD>(config_.baud_rate);
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        if (!SetCommState(serial_handle_, &dcb)) {
            CloseHandle(serial_handle_);
            serial_handle_ = INVALID_HANDLE_VALUE;
            throw AlpacaException("Failed to configure serial port", AlpacaError::DriverException);
        }
        COMMTIMEOUTS timeouts = {};
        timeouts.ReadIntervalTimeout = 50;
        timeouts.ReadTotalTimeoutConstant = 500;
        timeouts.ReadTotalTimeoutMultiplier = 0;
        SetCommTimeouts(serial_handle_, &timeouts);
        PurgeComm(serial_handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
#else
        // Claim the port in the in-use set BEFORE opening it, so a concurrent
        // auto-detect scan can't slip between its is_serial_port_in_use() check
        // and our open() and probe this same node. Store the canonical path so
        // the check matches what the enumerators compare against.
        std::error_code path_ec;
        std::string canonical_path = std::filesystem::canonical(config_.serial_port, path_ec).string();
        opened_port_ = path_ec ? config_.serial_port : canonical_path;
        mark_serial_port_open(opened_port_);

        serial_fd_ = open(config_.serial_port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (serial_fd_ < 0) {
            mark_serial_port_closed(opened_port_);
            opened_port_.clear();
            throw AlpacaException(
                "Failed to open serial port: " + config_.serial_port + " (" + std::strerror(errno) + ")",
                AlpacaError::NotConnected);
        }
        if (!configure_serial_fd(serial_fd_)) {
            close(serial_fd_);
            serial_fd_ = -1;
            mark_serial_port_closed(opened_port_);
            opened_port_.clear();
            throw AlpacaException("Failed to configure serial port", AlpacaError::DriverException);
        }
#endif
    }

    void close_serial() {
#ifdef _WIN32
        if (serial_handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(serial_handle_);
            serial_handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (serial_fd_ >= 0) {
            close(serial_fd_);
            serial_fd_ = -1;
        }
        mark_serial_port_closed(opened_port_);
        opened_port_.clear();
#endif
    }

    mutable std::mutex mutex_;         // guards serial handle + connected_/config_
    mutable std::mutex status_mutex_;  // guards the latest status frame
    FilterWheelConnectionConfig config_;
    bool connected_ = false;
    std::string opened_port_;  // path registered in the in-use set while open

    std::atomic<bool> running_{false};
    std::thread reader_thread_;
    FilterWheelStatus status_;
    // Firmware date (YYYY-MM-DD), captured once from the first valid frame and
    // cleared on disconnect; guarded by status_mutex_ alongside status_.
    std::string firmware_date_;

#ifdef _WIN32
    HANDLE serial_handle_ = INVALID_HANDLE_VALUE;
#else
    int serial_fd_ = -1;
#endif
};

// --- WandererFilterWheelProtocolWrapper public interface forwarding ---

WandererFilterWheelProtocolWrapper::WandererFilterWheelProtocolWrapper() : impl_(std::make_unique<Impl>()) {}

WandererFilterWheelProtocolWrapper::~WandererFilterWheelProtocolWrapper() = default;

std::string WandererFilterWheelProtocolWrapper::connect(const FilterWheelConnectionConfig& config) {
    return impl_->connect(config);
}

void WandererFilterWheelProtocolWrapper::disconnect() { impl_->disconnect(); }

bool WandererFilterWheelProtocolWrapper::is_connected() const { return impl_->is_connected(); }

FilterWheelStatus WandererFilterWheelProtocolWrapper::get_status() const { return impl_->get_status(); }

std::optional<std::string> WandererFilterWheelProtocolWrapper::get_firmware_date() const {
    return impl_->get_firmware_date();
}

void WandererFilterWheelProtocolWrapper::select_filter(int slot) { impl_->select_filter(slot); }

void WandererFilterWheelProtocolWrapper::calibrate() { impl_->calibrate(); }

}  // namespace alpacacore::vendor::wandererastro
