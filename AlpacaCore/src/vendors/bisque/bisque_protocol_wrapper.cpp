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
#include <alpacacore/vendor/bisque/bisque_protocol_wrapper.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace alpacacore::vendor::bisque {

namespace {

constexpr const char* kErrorPrefix = "|No error. Error = 0.";
constexpr int kErrorPrefixLen = 21; // strlen("|No error. Error = 0.")

// Socket read timeout granularity (ms). This controls how often we poll
// the socket for data via recv(). A shorter value means we detect the '#'
// terminator sooner but spend more CPU in the poll loop.
constexpr int kSocketPollTimeoutMs = 200;

} // namespace

class BisqueProtocolWrapper::Impl {
public:
    Impl() = default;

    ~Impl() {
        disconnect();
    }

    bool connect(const ConnectionInfo& info) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connected_) {
            disconnect_locked();
        }
        connection_info_ = info;

        socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd_ < 0) {
            ALPACA_LOG_ERROR("Bisque", "Failed to create socket");
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<unsigned short>(info.tcp_port));

        if (inet_pton(AF_INET, info.host.c_str(), &addr.sin_addr) <= 0) {
            hostent* host_entry = gethostbyname(info.host.c_str());
            if (!host_entry) {
                ALPACA_LOG_ERROR("Bisque", "Failed to resolve host: " + info.host);
                close(socket_fd_);
                socket_fd_ = -1;
                return false;
            }
            addr.sin_addr = *reinterpret_cast<in_addr*>(host_entry->h_addr);
        }

        if (::connect(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ALPACA_LOG_ERROR("Bisque", "Failed to connect to " + info.host +
                             ":" + std::to_string(info.tcp_port));
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }

        configure_timeouts();
        connected_ = true;
        ALPACA_LOG_INFO("Bisque", "Connected to TheSkyX at " + info.host +
                        ":" + std::to_string(info.tcp_port));
        return true;
    }

    void disconnect() {
        std::lock_guard<std::mutex> lock(mutex_);
        disconnect_locked();
    }

    bool is_connected() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connected_;
    }

    bool handshake() {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected_locked();

        std::string cmd =
            "/* Java Script */"
            "var Out;"
            "sky6RASCOMTele.ConnectAndDoNotUnpark();"
            "Out = sky6RASCOMTele.IsConnected + '#';";

        write_locked(cmd);
        std::string response = read_until_hash_locked(connection_info_.response_timeout_ms);

        // Handshake is special: response is just "1#" (no error prefix).
        if (response == "1") {
            ALPACA_LOG_INFO("Bisque", "TheSkyX handshake successful");
            return true;
        }

        ALPACA_LOG_ERROR("Bisque", "TheSkyX handshake failed: " + response);
        return false;
    }

    Position get_ra_dec() {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected_locked();

        std::string cmd =
            "/* Java Script */"
            "var Out;"
            "sky6RASCOMTele.GetRaDec();"
            "Out = String(sky6RASCOMTele.dRa) + ',' + String(sky6RASCOMTele.dDec) + '#';";

        write_locked(cmd);
        std::string response = read_until_hash_locked(connection_info_.response_timeout_ms);
        std::string value = strip_error_prefix(response);

        Position pos;
        if (std::sscanf(value.c_str(), "%lf,%lf", &pos.ra_hours, &pos.dec_degrees) != 2) {
            throw AlpacaException("Failed to parse RA/Dec from TheSkyX: " + value);
        }
        return pos;
    }

    AltAz get_alt_az() {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected_locked();

        std::string cmd =
            "/* Java Script */"
            "var Out;"
            "sky6RASCOMTele.GetAzAlt();"
            "Out = String(sky6RASCOMTele.dAz) + ',' + String(sky6RASCOMTele.dAlt) + '#';";

        write_locked(cmd);
        std::string response = read_until_hash_locked(connection_info_.response_timeout_ms);
        std::string value = strip_error_prefix(response);

        AltAz altaz;
        if (std::sscanf(value.c_str(), "%lf,%lf", &altaz.azimuth_degrees, &altaz.altitude_degrees) != 2) {
            throw AlpacaException("Failed to parse Az/Alt from TheSkyX: " + value);
        }
        return altaz;
    }

    void slew_to_ra_dec(double ra_hours, double dec_degrees) {
        char body[256];
        std::snprintf(body, sizeof(body),
                      "sky6RASCOMTele.Asynchronous = true;"
                      "sky6RASCOMTele.SlewToRaDec(%g, %g,'');",
                      ra_hours, dec_degrees);
        send_ok_command_internal(body, 0);
    }

    bool is_slew_complete() {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected_locked();

        std::string cmd =
            "/* Java Script */"
            "var Out;"
            "Out = sky6RASCOMTele.IsSlewComplete + '#';";

        write_locked(cmd);
        std::string response = read_until_hash_locked(connection_info_.response_timeout_ms);
        std::string value = strip_error_prefix(response);

        int complete = 0;
        if (std::sscanf(value.c_str(), "%d", &complete) != 1) {
            throw AlpacaException("Failed to parse IsSlewComplete from TheSkyX: " + value);
        }
        return complete == 1;
    }

    void sync_to_coordinates(double ra_hours, double dec_degrees) {
        char body[256];
        std::snprintf(body, sizeof(body),
                      "sky6RASCOMTele.Sync(%g, %g,'');",
                      ra_hours, dec_degrees);
        send_ok_command_internal(body, 0);
    }

    void abort() {
        send_ok_command_internal("sky6RASCOMTele.Abort();", 0);
    }

    void park() {
        send_ok_command_internal(
            "sky6RASCOMTele.Asynchronous = true;"
            "sky6RASCOMTele.ParkAndDoNotDisconnect();", 0);
    }

    void unpark() {
        send_ok_command_internal("sky6RASCOMTele.Unpark();", 0);
    }

    bool is_parked() {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected_locked();

        std::string cmd =
            "/* Java Script */"
            "var Out;"
            "Out = sky6RASCOMTele.IsParked() + '#';";

        write_locked(cmd);
        std::string response = read_until_hash_locked(connection_info_.response_timeout_ms);
        std::string value = strip_error_prefix(response);

        if (value == "true") return true;
        if (value == "false") return false;

        throw AlpacaException("Failed to parse IsParked from TheSkyX: " + value);
    }

    void set_park_position() {
        send_ok_command_internal("sky6RASCOMTele.SetParkPosition();", 0);
    }

    void set_tracking(bool on, bool ignore_rates, double ra_rate, double dec_rate) {
        char body[256];
        std::snprintf(body, sizeof(body),
                      "sky6RASCOMTele.SetTracking(%d, %d, %g, %g);",
                      on ? 1 : 0, ignore_rates ? 1 : 0, ra_rate, dec_rate);
        send_ok_command_internal(body, 0);
    }

    bool is_tracking() {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected_locked();

        std::string cmd =
            "/* Java Script */"
            "var Out;"
            "Out = sky6RASCOMTele.IsTracking + '#';";

        write_locked(cmd);
        std::string response = read_until_hash_locked(connection_info_.response_timeout_ms);
        std::string value = strip_error_prefix(response);

        double rate = 0.0;
        if (std::sscanf(value.c_str(), "%lf", &rate) != 1) {
            throw AlpacaException("Failed to parse IsTracking from TheSkyX: " + value);
        }
        return rate > 0.0;
    }

    void find_home() {
        // FindHome with loop-wait (up to 60 seconds).
        send_ok_command_internal(
            "sky6RASCOMTele.FindHome();"
            "while(!sky6RASCOMTele.IsSlewComplete) {"
            "sky6Web.Sleep(1000);}", 60);
    }

    int get_pier_side() {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected_locked();

        std::string cmd =
            "/* Java Script */"
            "var Out;"
            "sky6RASCOMTele.DoCommand(11, \"Pier Side\");"
            "Out = sky6RASCOMTele.DoCommandOutput + '#';";

        write_locked(cmd);
        std::string response = read_until_hash_locked(connection_info_.response_timeout_ms);
        std::string value = strip_error_prefix(response);

        int pier_side = 0;
        if (std::sscanf(value.c_str(), "%d", &pier_side) != 1) {
            throw AlpacaException("Failed to parse pier side from TheSkyX: " + value);
        }
        return pier_side;
    }

    void start_open_loop_motion(int direction, int rate) {
        char body[128];
        std::snprintf(body, sizeof(body),
                      "sky6RASCOMTele.DoCommand(9,'%d|%d');",
                      direction, rate);
        send_ok_command_internal(body, 0);
    }

    void stop_open_loop_motion() {
        send_ok_command_internal("sky6RASCOMTele.DoCommand(10,'');", 0);
    }

    void guide(double ra_arcsec, double dec_arcsec) {
        char body[256];
        std::snprintf(body, sizeof(body),
                      "sky6RASCOMTele.Asynchronous = true;"
                      "sky6DirectGuide.MoveTelescope(%g, %g);",
                      ra_arcsec, dec_arcsec);
        send_ok_command_internal(body, 0);
    }

    std::string send_command(const std::string& js_body, int timeout_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected_locked();

        std::string cmd =
            "/* Java Script */"
            "var Out;" + js_body;

        int effective_timeout = timeout_ms > 0 ? timeout_ms : connection_info_.response_timeout_ms;

        write_locked(cmd);
        std::string response = read_until_hash_locked(effective_timeout);
        return strip_error_prefix(response);
    }

    void send_ok_command(const std::string& js_body, int timeout_ms) {
        send_ok_command_internal(js_body, timeout_ms);
    }

private:
    void disconnect_locked() {
        if (socket_fd_ >= 0) {
            close(socket_fd_);
            socket_fd_ = -1;
        }
        connected_ = false;
    }

    void check_connected_locked() const {
        if (!connected_) {
            throw AlpacaException("Not connected to TheSkyX",
                                  AlpacaError::NotConnected);
        }
    }

    void configure_timeouts() {
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = kSocketPollTimeoutMs * 1000;
        setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(socket_fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    }

    void write_locked(const std::string& data) {
        // send() may transmit only part of the payload (or be interrupted);
        // loop until the whole command is on the wire so a trailing '#'
        // terminator is never silently dropped.
        std::size_t total = 0;
        while (total < data.length()) {
            ssize_t sent = send(socket_fd_, data.c_str() + total, data.length() - total, MSG_NOSIGNAL);
            if (sent < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw AlpacaException("Failed to send command to TheSkyX");
            }
            total += static_cast<std::size_t>(sent);
        }
    }

    std::string read_until_hash_locked(int timeout_ms) {
        std::string response;
        auto start = std::chrono::steady_clock::now();

        while (true) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed > timeout_ms) {
                throw AlpacaException("Timeout reading response from TheSkyX");
            }

            char ch = 0;
            ssize_t bytes_received = recv(socket_fd_, &ch, 1, 0);
            if (bytes_received == 1) {
                if (ch == '#') {
                    return response;
                }
                response += ch;
            } else if (bytes_received == 0) {
                throw AlpacaException("TheSkyX connection closed");
            }
            // bytes_received < 0: EAGAIN/EWOULDBLOCK from socket timeout, loop and retry.
        }
    }

    std::string strip_error_prefix(const std::string& response) {
        if (response.size() >= static_cast<size_t>(kErrorPrefixLen) &&
            response.compare(0, kErrorPrefixLen, kErrorPrefix) == 0) {
            return response.substr(kErrorPrefixLen);
        }
        throw AlpacaException("TheSkyX error: " + response);
    }

    void send_ok_command_internal(const std::string& js_body, int timeout_seconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        check_connected_locked();

        std::string cmd =
            "/* Java Script */"
            "var Out;"
            "try {"
            + js_body +
            "Out  = 'OK#'; }"
            "catch (err) {Out = err; }";

        int effective_timeout = timeout_seconds > 0
            ? timeout_seconds * 1000
            : connection_info_.response_timeout_ms;

        write_locked(cmd);
        std::string response = read_until_hash_locked(effective_timeout);

        if (response != "|No error. Error = 0.OK") {
            throw AlpacaException("TheSkyX command failed: " + response);
        }
    }

    mutable std::mutex mutex_;
    int socket_fd_ = -1;
    bool connected_ = false;
    ConnectionInfo connection_info_;
};

// Singleton
BisqueProtocolWrapper& BisqueProtocolWrapper::instance() {
    static BisqueProtocolWrapper wrapper;
    return wrapper;
}

BisqueProtocolWrapper::BisqueProtocolWrapper() : pimpl_(std::make_unique<Impl>()) {}
BisqueProtocolWrapper::~BisqueProtocolWrapper() = default;

bool BisqueProtocolWrapper::connect(const ConnectionInfo& info) { return pimpl_->connect(info); }
void BisqueProtocolWrapper::disconnect() { pimpl_->disconnect(); }
bool BisqueProtocolWrapper::is_connected() const { return pimpl_->is_connected(); }
bool BisqueProtocolWrapper::handshake() { return pimpl_->handshake(); }
Position BisqueProtocolWrapper::get_ra_dec() { return pimpl_->get_ra_dec(); }
AltAz BisqueProtocolWrapper::get_alt_az() { return pimpl_->get_alt_az(); }
void BisqueProtocolWrapper::slew_to_ra_dec(double ra, double dec) { pimpl_->slew_to_ra_dec(ra, dec); }
bool BisqueProtocolWrapper::is_slew_complete() { return pimpl_->is_slew_complete(); }
void BisqueProtocolWrapper::sync_to_coordinates(double ra, double dec) { pimpl_->sync_to_coordinates(ra, dec); }
void BisqueProtocolWrapper::abort() { pimpl_->abort(); }
void BisqueProtocolWrapper::park() { pimpl_->park(); }
void BisqueProtocolWrapper::unpark() { pimpl_->unpark(); }
bool BisqueProtocolWrapper::is_parked() { return pimpl_->is_parked(); }
void BisqueProtocolWrapper::set_park_position() { pimpl_->set_park_position(); }
void BisqueProtocolWrapper::set_tracking(bool on, bool ign, double ra, double dec) { pimpl_->set_tracking(on, ign, ra, dec); }
bool BisqueProtocolWrapper::is_tracking() { return pimpl_->is_tracking(); }
void BisqueProtocolWrapper::find_home() { pimpl_->find_home(); }
int BisqueProtocolWrapper::get_pier_side() { return pimpl_->get_pier_side(); }
void BisqueProtocolWrapper::start_open_loop_motion(int dir, int rate) { pimpl_->start_open_loop_motion(dir, rate); }
void BisqueProtocolWrapper::stop_open_loop_motion() { pimpl_->stop_open_loop_motion(); }
void BisqueProtocolWrapper::guide(double ra, double dec) { pimpl_->guide(ra, dec); }
std::string BisqueProtocolWrapper::send_command(const std::string& js, int t) { return pimpl_->send_command(js, t); }
void BisqueProtocolWrapper::send_ok_command(const std::string& js, int t) { pimpl_->send_ok_command(js, t); }

} // namespace alpacacore::vendor::bisque
