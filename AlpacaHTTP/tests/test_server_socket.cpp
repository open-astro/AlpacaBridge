// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

// Socket-level tests for the Server request-read path (read_request) and the
// keep-alive connection loop, which the Router-level test_routing.cpp cannot
// reach because it feeds Router::route directly. Covers the header-size (431)
// boundary fixed in #128 (the cap must be enforced on the recv chunk that
// contains the \r\n\r\n terminator, not only on earlier chunks; see issue
// #129), HTTP/1.1 persistence and its bounds, the framing gate that decides
// whether a connection may stay open, and the graceful close path.

#include <alpacacore/device_registry.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/logging.h>
#include <alpacahttp/config.h>
#include <alpacahttp/server.h>
#include <alpacahttp/util/logging_adapter.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "http/thread_join.h"
#include "test_assert.h"

namespace {

// Must match kMaxHeaderBytes in AlpacaHTTP/src/http/server.cpp (not exported).
constexpr std::size_t kMaxHeaderBytes = 64 * 1024;

// Must match kMaxRequestsPerConnection in AlpacaHTTP/src/http/server.cpp (not exported).
constexpr std::uint64_t kMaxRequestsPerConnection = 1000;

// Aborts the process if not disarmed within `budget` -- turns a regression
// that HANGS (a join that never returns) into a failed test with a clear
// message instead of a test binary that never exits and just times out the
// CI job. Construct at the top of a scope that must complete within budget;
// destructor disarms it.
class Watchdog {
public:
    explicit Watchdog(std::chrono::milliseconds budget, std::string label)
        : label_(std::move(label)), thread_([this, budget] {
              std::unique_lock<std::mutex> lock(mutex_);
              if (!cv_.wait_for(lock, budget, [this] { return disarmed_; })) {
                  std::fprintf(stderr, "WATCHDOG TIMEOUT: %s did not complete within budget\n", label_.c_str());
                  std::abort();
              }
          }) {}

    ~Watchdog() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            disarmed_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

private:
    std::string label_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool disarmed_ = false;
    std::thread thread_;
};

// Send `data` to 127.0.0.1:port in two writes — the terminator-bearing tail
// goes in the second write so we exercise the fixed path (the chunk that finds
// \r\n\r\n must itself be size-checked). Returns the first line of the response,
// or "" if the connection produced nothing.
std::string send_split_request(std::uint16_t port, const std::string& data, std::size_t first_chunk) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT(fd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return "";
    }

    first_chunk = std::min(first_chunk, data.size());
    ::send(fd, data.data(), first_chunk, 0);
    // Brief gap so the two writes tend to arrive as separate recvs on the server.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ::send(fd, data.data() + first_chunk, data.size() - first_chunk, 0);

    std::string response;
    char buf[2048];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        response.append(buf, static_cast<std::size_t>(n));
        if (response.find("\r\n") != std::string::npos) {
            break;  // have the status line
        }
    }
    ::close(fd);

    auto eol = response.find("\r\n");
    return eol == std::string::npos ? response : response.substr(0, eol);
}

// Build a request whose header block (bytes before the terminating \r\n\r\n) is
// exactly `header_bytes` long, padding a single X-Pad header to hit the target.
std::string make_request_with_header_size(std::size_t header_bytes) {
    const std::string prefix = "GET / HTTP/1.1\r\nX-Pad: ";
    EXPECT(header_bytes >= prefix.size());
    std::string req = prefix;
    req.append(header_bytes - prefix.size(), 'a');
    req.append("\r\n\r\n");  // ends X-Pad line + empty line => \r\n\r\n terminator
    return req;
}

// --- keep-alive helpers ------------------------------------------------------

// Read back the ephemeral port a just-started Server bound, retrying
// briefly: is_running() can go true before the listener is actually created
// (see the restart-case comment below for why), so bound_port() may answer 0
// for a few milliseconds after start_async() returns. Returns 0 if the port
// never showed up within the budget.
std::uint16_t wait_for_bound_port(alpacahttp::Server& server, int budget_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    std::uint16_t port = 0;
    while (port == 0 && std::chrono::steady_clock::now() < deadline) {
        port = server.bound_port();
        if (port == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    return port;
}

int connect_local(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// MSG_NOSIGNAL matters here: several cases below deliberately make the server
// close first (the request cap, the malformed request, stop() under load), so
// a send can land on an already-closed socket. Without it the test process
// takes SIGPIPE and dies instead of failing an EXPECT with a usable message.
void send_all(int fd, const std::string& data) {
    EXPECT(::send(fd, data.data(), data.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(data.size()));
}

// Read exactly one HTTP response (status line + headers + Content-Length body)
// from `fd`. `carry` holds bytes already received past the previous response
// (pipelined replies) and is updated for the next call. Returns "" if the
// peer closed before a full header block arrived.
std::string read_one_response(int fd, std::string& carry) {
    char tmp[4096];
    std::size_t header_end = carry.find("\r\n\r\n");
    while (header_end == std::string::npos) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            return "";
        }
        carry.append(tmp, static_cast<std::size_t>(n));
        header_end = carry.find("\r\n\r\n");
    }
    std::string headers = carry.substr(0, header_end);
    std::transform(headers.begin(), headers.end(), headers.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::size_t content_length = 0;
    auto pos = headers.find("content-length:");
    if (pos != std::string::npos) {
        content_length = std::stoul(headers.substr(pos + std::strlen("content-length:")));
    }
    const std::size_t total = header_end + 4 + content_length;
    while (carry.size() < total) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            break;
        }
        carry.append(tmp, static_cast<std::size_t>(n));
    }
    std::string response = carry.substr(0, total);
    carry.erase(0, total);
    return response;
}

// open-astro#547: minimal telescope stub for the end-to-end watchdog-timer
// wiring test below. Always connected and always "slewing" so a single
// routed request is enough to arm the watchdog; abort_slew() counts its own
// calls instead of touching any mount state.
class WatchdogStubTelescope final : public alpacacore::TelescopeDriver {
public:
    explicit WatchdogStubTelescope(int number) : number_(number) {}

    int get_device_number() const override { return number_; }
    std::string get_name() const override { return "Watchdog Stub"; }
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::Telescope; }
    std::string get_unique_id() const override { return "watchdog-stub-" + std::to_string(number_); }
    std::string get_description() const override { return "fake telescope"; }
    std::string get_driver_info() const override { return "fake driver"; }
    std::string get_driver_version() const override { return "0.0.1"; }
    int get_interface_version() const override { return 4; }
    bool get_connected() const override { return true; }
    void set_connected(bool) override {}
    std::vector<std::string> get_supported_actions() const override { return {}; }
    std::string action(std::string_view, std::string_view) override { return ""; }
    bool can_action(std::string_view) const override { return false; }
    std::string command_blind(std::string_view, bool) override { return ""; }
    bool command_bool(std::string_view, bool) override { return false; }
    std::string command_string(std::string_view, bool) override { return ""; }
    std::chrono::system_clock::time_point get_utc_date() const override { return {}; }
    void set_utc_date(std::chrono::system_clock::time_point) override {}
    alpacacore::AlignmentMode get_alignment_mode() const override { return alpacacore::AlignmentMode::GermanPolar; }
    double get_altitude() const override { return 0.0; }
    double get_aperture_diameter() const override { return 0.0; }
    void set_aperture_diameter(double) override {}
    double get_aperture_area() const override { return 0.0; }
    bool get_at_home() const override { return false; }
    bool get_at_park() const override { return false; }
    double get_azimuth() const override { return 0.0; }
    bool get_can_find_home() const override { return false; }
    bool get_can_park() const override { return false; }
    bool get_can_pulse_guide() const override { return false; }
    bool get_is_pulse_guiding() const override { return false; }
    bool get_can_set_declination_rate() const override { return false; }
    bool get_can_set_guide_rates() const override { return false; }
    bool get_can_set_park() const override { return false; }
    bool get_can_set_pier_side() const override { return false; }
    bool get_can_set_right_ascension_rate() const override { return false; }
    bool get_can_set_tracking() const override { return false; }
    bool get_can_slew_alt_az() const override { return false; }
    bool get_can_slew_alt_az_async() const override { return false; }
    bool get_can_sync_alt_az() const override { return false; }
    bool get_can_slew() const override { return false; }
    bool get_can_slew_async() const override { return false; }
    bool get_can_sync() const override { return false; }
    bool get_can_unpark() const override { return false; }
    double get_declination() const override { return 0.0; }
    double get_declination_rate() const override { return 0.0; }
    void set_declination_rate(double) override {}
    bool get_tracking() const override { return true; }
    void set_tracking(bool) override {}
    double get_focal_length() const override { return 0.0; }
    void set_focal_length(double) override {}
    alpacacore::GuideRate get_guide_rate() const override { return alpacacore::GuideRate{}; }
    void set_guide_rate(const alpacacore::GuideRate&) override {}
    double get_right_ascension() const override { return 0.0; }
    double get_right_ascension_rate() const override { return 0.0; }
    void set_right_ascension_rate(double) override {}
    int get_side_of_pier() const override { return 0; }
    void set_side_of_pier(int) override {}
    int get_destination_side_of_pier(double, double) const override { return 0; }
    alpacacore::EquatorialSystem get_equatorial_system() const override {
        return alpacacore::EquatorialSystem::Topocentric;
    }
    bool get_does_refraction() const override { return false; }
    void set_does_refraction(bool) override {}
    int get_slew_settle_time() const override { return 0; }
    void set_slew_settle_time(int) override {}
    double get_sidereal_time() const override { return 0.0; }
    double get_site_elevation() const override { return 0.0; }
    void set_site_elevation(double) override {}
    double get_site_latitude() const override { return 0.0; }
    void set_site_latitude(double) override {}
    double get_site_longitude() const override { return 0.0; }
    void set_site_longitude(double) override {}
    // Always slewing: this stub's only job is to prove the timer thread
    // reaches stop_motion_if_client_silent() and that it can stop a
    // telescope, not to model a real motion state machine.
    bool get_slewing() const override { return true; }
    double get_target_declination() const override { return 0.0; }
    void set_target_declination(double) override {}
    double get_target_right_ascension() const override { return 0.0; }
    void set_target_right_ascension(double) override {}
    int get_tracking_rate() const override { return 0; }
    void set_tracking_rate(int) override {}
    std::vector<int> get_tracking_rates() const override { return {}; }
    void find_home() override {}
    void park() override {}
    void pulse_guide(int, int) override {}
    void set_park() override {}
    // open-astro#547 review finding: a synchronous SlewToCoordinates can
    // block the HTTP worker for as long as the goto takes. slew_sleep_ms
    // lets a test hold this call in flight past the watchdog interval, to
    // prove the client's own request never gets aborted out from under it.
    std::atomic<int> slew_sleep_ms{0};
    void slew_to_coordinates(double, double) override {
        const int ms = slew_sleep_ms.load();
        if (ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
    }
    void slew_to_coordinates_async(double, double) override {}
    void slew_to_target() override {}
    void slew_to_target_async() override {}
    void sync_to_coordinates(double, double) override {}
    void sync_to_target() override {}
    void unpark() override {}
    bool get_can_move_axis(int) const override { return false; }
    void move_axis(int, double) override {}
    std::pair<double, double> get_axis_rate_range(int) const override { return {0.0, 0.0}; }
    void abort_slew() override { ++aborts; }
    void slew_to_alt_az(double, double) override {}
    void slew_to_alt_az_async(double, double) override {}
    void sync_to_alt_az(double, double) override {}

    std::atomic<int> aborts{0};

private:
    int number_;
};

// True if the server has closed the connection (EOF within `ms`); false if it
// is still open (the peek times out).
//
// The previous receive timeout is saved and restored. Without that, every
// read_one_response() after a peer_closed() check inherits this function's
// short budget (a few hundred ms), and a response that merely arrives slowly
// on a loaded CI machine comes back as "" -- which the following EXPECT then
// reports as "server closed", sending the reader after a bug that isn't there.
bool peer_closed(int fd, int ms) {
    struct timeval previous {};
    socklen_t previous_len = sizeof(previous);
    const bool saved = ::getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &previous, &previous_len) == 0;

    struct timeval tv {};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char c = 0;
    const bool closed = ::recv(fd, &c, 1, MSG_PEEK) == 0;

    if (saved) {
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &previous, previous_len);
    }
    return closed;
}

}  // namespace

int main() {
    std::cout << "Testing Server socket read path and keep-alive...\n";

    alpacahttp::Config config;
    config.set_http_port(0);  // ephemeral: let the OS pick, so parallel test runs never collide
    config.set_discovery_enabled(false);
    config.set_server_name("TestServer");

    alpacahttp::Server server(config);
    server.start_async();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // is_running() can go true before the listener is actually bound (see
    // wait_for_bound_port()'s comment), so the port readiness check -- not
    // just is_running() -- decides whether this run is skipped. Folding both
    // into one wait keeps a slow-but-eventually-successful bind on a loaded
    // runner from hitting a hard EXPECT abort instead of the intended skip.
    const std::uint16_t port = server.is_running() ? wait_for_bound_port(server, 2000) : 0;
    if (port == 0) {
        std::cerr << "Server socket test skipped: unable to bind an ephemeral port.\n";
        return 0;  // tolerate a busy/unavailable port, like the discovery test
    }

    // Over the cap by one byte, terminator in the second write => 431.
    {
        std::string req = make_request_with_header_size(kMaxHeaderBytes + 1);
        std::string status = send_split_request(port, req, req.size() - 8);
        EXPECT(status.find(" 431") != std::string::npos);
    }

    // Exactly at the cap, same split => accepted (parsed and routed, NOT 431).
    {
        std::string req = make_request_with_header_size(kMaxHeaderBytes);
        std::string status = send_split_request(port, req, req.size() - 8);
        EXPECT(!status.empty());
        EXPECT(status.find(" 431") == std::string::npos);
    }

    // A normal small request still gets a well-formed response (sanity).
    {
        std::string req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
        std::string status = send_split_request(port, req, req.size() - 4);
        EXPECT(status.rfind("HTTP/1.1 ", 0) == 0);
        EXPECT(status.find(" 431") == std::string::npos);
    }

    // --- Keep-alive ---------------------------------------------------------
    const std::string kGet11 = "GET /management/apiversions HTTP/1.1\r\nHost: localhost\r\n\r\n";

    // HTTP/1.1 is persistent by default: two requests on one connection, each
    // answered with "Connection: keep-alive", then "Connection: close" ends it.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        send_all(fd, kGet11);
        std::string r1 = read_one_response(fd, carry);
        EXPECT(r1.rfind("HTTP/1.1 ", 0) == 0);
        EXPECT(r1.find("Connection: keep-alive\r\n") != std::string::npos);
        EXPECT(!peer_closed(fd, 200));
        send_all(fd, kGet11);
        std::string r2 = read_one_response(fd, carry);
        EXPECT(r2.rfind("HTTP/1.1 ", 0) == 0);
        EXPECT(r2.find("Connection: keep-alive\r\n") != std::string::npos);
        send_all(fd, "GET /management/apiversions HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        std::string r3 = read_one_response(fd, carry);
        EXPECT(r3.find("Connection: close\r\n") != std::string::npos);
        EXPECT(peer_closed(fd, 2000));
        ::close(fd);
    }

    // HTTP/1.0 closes by default...
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        send_all(fd, "GET /management/apiversions HTTP/1.0\r\nHost: localhost\r\n\r\n");
        std::string r = read_one_response(fd, carry);
        EXPECT(r.find("Connection: close\r\n") != std::string::npos);
        EXPECT(peer_closed(fd, 2000));
        ::close(fd);
    }

    // ...unless the client asks for keep-alive.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        const std::string get10_ka =
            "GET /management/apiversions HTTP/1.0\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
        send_all(fd, get10_ka);
        std::string r1 = read_one_response(fd, carry);
        EXPECT(r1.find("Connection: keep-alive\r\n") != std::string::npos);
        EXPECT(!peer_closed(fd, 200));
        send_all(fd, get10_ka);
        std::string r2 = read_one_response(fd, carry);
        EXPECT(r2.find("Connection: keep-alive\r\n") != std::string::npos);
        ::close(fd);
    }

    // Pipelined: two requests in a single write are answered in order on the
    // same connection (the bytes past the first request are carried over).
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        send_all(fd, kGet11 + kGet11);
        std::string r1 = read_one_response(fd, carry);
        std::string r2 = read_one_response(fd, carry);
        EXPECT(r1.rfind("HTTP/1.1 ", 0) == 0);
        EXPECT(r2.rfind("HTTP/1.1 ", 0) == 0);
        EXPECT(r2.find("Connection: keep-alive\r\n") != std::string::npos);
        EXPECT(!peer_closed(fd, 200));
        ::close(fd);
    }

    // A connection is force-closed after kMaxRequestsPerConnection requests,
    // even though every one of them individually asked to keep the
    // connection alive -- the worker-pinning mitigation added in response to
    // the PR #2 review must actually fire, not just exist as an unused cap.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        for (std::uint64_t i = 1; i < kMaxRequestsPerConnection; ++i) {
            send_all(fd, kGet11);
            std::string r = read_one_response(fd, carry);
            EXPECT(r.find("Connection: keep-alive\r\n") != std::string::npos);
        }
        EXPECT(!peer_closed(fd, 200));
        send_all(fd, kGet11);
        std::string last = read_one_response(fd, carry);
        EXPECT(last.find("Connection: close\r\n") != std::string::npos);
        EXPECT(peer_closed(fd, 2000));
        ::close(fd);
    }

    // A malformed request on a persistent connection gets 400 and a close.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        send_all(fd, kGet11);
        std::string r1 = read_one_response(fd, carry);
        EXPECT(r1.find("Connection: keep-alive\r\n") != std::string::npos);
        send_all(fd, "GARBAGE\r\n\r\n");
        std::string r2 = read_one_response(fd, carry);
        EXPECT(r2.find(" 400 ") != std::string::npos);
        EXPECT(r2.find("Connection: close\r\n") != std::string::npos);
        EXPECT(peer_closed(fd, 2000));
        ::close(fd);
    }

    // A slow-but-legitimate request body on a keep-alive connection's SECOND
    // request must not be held to the short idle-wait bound. Before the fix,
    // SO_RCVTIMEO was set to kKeepAliveIdleSeconds (15s) for the whole of
    // request 2+ and never restored once the peer started sending, so a body
    // arriving in two writes >15s apart -- fine on request 1, which gets the
    // full 30s kSocketTimeoutSeconds per recv -- would time out and drop the
    // connection purely because it happened to be request 2. The timeout is
    // now restored the moment the peer's first byte of the new request
    // arrives, so only the true gap BETWEEN requests is 15s-limited.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        send_all(fd, kGet11);
        std::string r1 = read_one_response(fd, carry);
        EXPECT(r1.find("Connection: keep-alive\r\n") != std::string::npos);

        const std::string body = "{}";
        std::string headers =
            "POST /nonexistent HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\n\r\n";
        send_all(fd, headers + body.substr(0, 1));
        // Longer than the 15s idle bound, shorter than the 30s per-request one.
        std::this_thread::sleep_for(std::chrono::seconds(16));
        send_all(fd, body.substr(1));
        std::string r2 = read_one_response(fd, carry);
        EXPECT(!r2.empty());
        ::close(fd);
    }

    // Pre-carried headers must not leave the body under the idle timeout. A
    // pipelining client can deliver request B's headers in the same write as
    // request A; read_request then finds B's terminator in the carried bytes
    // and does no recv in its header loop -- and the idle-timeout restore
    // used to run only on a recv, so it was skipped and B's first body recv
    // ran under the 15s idle bound instead of the 30s per-request one. Same
    // bug as the slow-body case above, reached through carry-over instead.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        const std::string body = "{}";
        std::string b_headers =
            "POST /nonexistent HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\n\r\n";
        // A complete, plus B's headers only, in ONE write.
        send_all(fd, kGet11 + b_headers);
        std::string ra = read_one_response(fd, carry);
        EXPECT(ra.find("Connection: keep-alive\r\n") != std::string::npos);
        // Longer than the 15s idle bound, shorter than the 30s per-request one.
        std::this_thread::sleep_for(std::chrono::seconds(16));
        send_all(fd, body);
        std::string rb = read_one_response(fd, carry);
        EXPECT(!rb.empty());
        ::close(fd);
    }

    // A chunked request body must not be treated as a zero-length one. This
    // server frames bodies from Content-Length only, so before the 501 the
    // chunk framing stayed on the wire and -- now that the connection
    // survives a request -- was parsed as the NEXT request: the client got
    // its response followed by a spurious 400, and behind an intermediary
    // that does understand chunked this is a smuggling-shaped desync.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        send_all(fd,
                 "PUT /api/v1/telescope/0/connected HTTP/1.1\r\nHost: localhost\r\n"
                 "Transfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
        std::string r = read_one_response(fd, carry);
        EXPECT(r.rfind("HTTP/1.1 501 ", 0) == 0);
        EXPECT(peer_closed(fd, 2000));
        ::close(fd);
    }

    // HEAD is not in parse_method, so it routes as UNKNOWN and is answered
    // with a normal BODIED error. Sending a body to a HEAD client is already
    // wrong (RFC 7231 4.3.2), but on a persistent connection it desyncs: the
    // client discards headers, expects no body, and reads ours as the head of
    // its next response. Browsers, uptime monitors and reverse-proxy health
    // checks all send HEAD at the web UI, so the connection must close.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        send_all(fd, "HEAD /management/apiversions HTTP/1.1\r\nHost: localhost\r\n\r\n");
        std::string r = read_one_response(fd, carry);
        EXPECT(!r.empty());
        EXPECT(r.find("Connection: close\r\n") != std::string::npos);
        EXPECT(peer_closed(fd, 2000));
        ::close(fd);
    }

    // A stray empty line before the request line must be skipped, not 400'd
    // (RFC 7230 3.5). Several client stacks leave one on the wire after a
    // body; before keep-alive those bytes died with the connection, now they
    // would drop a session the client believes is still good.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        send_all(fd, kGet11);
        EXPECT(!read_one_response(fd, carry).empty());
        send_all(fd, "\r\n" + kGet11);
        std::string r = read_one_response(fd, carry);
        EXPECT(r.rfind("HTTP/1.1 200 ", 0) == 0);
        // Same thing pipelined: the CRLF arrives in the bytes carried over
        // from the previous request, so read_request must strip it before
        // its first terminator search, not only after a recv.
        send_all(fd, kGet11 + "\r\n" + kGet11);
        std::string r3 = read_one_response(fd, carry);
        std::string r4 = read_one_response(fd, carry);
        EXPECT(r3.rfind("HTTP/1.1 200 ", 0) == 0);
        EXPECT(r4.rfind("HTTP/1.1 200 ", 0) == 0);
        ::close(fd);
    }

    // Idle keep-alive connections cost no worker: they are parked on the
    // reactor's poll set, so thread_pool_size bounds concurrent REQUESTS
    // (what config.h documents) and not connections. Before the reactor a
    // parked connection held a worker in recv, and a stopgap reserve had to
    // force Connection: close as the pool filled. Own server on its own
    // port: the shared one above has the default pool of 32.
    {
        alpacahttp::Config small_config;
        small_config.set_http_port(0);
        small_config.set_discovery_enabled(false);
        small_config.set_server_name("TestServerSmallPool");
        small_config.set_thread_pool_size(2);
        alpacahttp::Server small_server(small_config);
        small_server.start_async();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        if (const std::uint16_t small_port = small_server.is_running() ? wait_for_bound_port(small_server, 2000) : 0;
            small_port != 0) {
            // Twice as many idle keep-alive connections as workers, every
            // one of them kept alive (no reserve forcing a close).
            constexpr int kParked = 4;
            int parked[kParked];
            std::string parked_carry[kParked];
            for (int i = 0; i < kParked; ++i) {
                parked[i] = connect_local(small_port);
                EXPECT(parked[i] >= 0);
                send_all(parked[i], kGet11);
                std::string r = read_one_response(parked[i], parked_carry[i]);
                EXPECT(r.find("Connection: keep-alive\r\n") != std::string::npos);
            }

            // Clients that connect and never send cost no worker either:
            // before the reactor, three of these would have held both
            // workers in recv for the 30 s slowloris bound.
            int silent[3];
            for (int& fd : silent) {
                fd = connect_local(small_port);
                EXPECT(fd >= 0);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // A fresh client must still be served promptly. The recv timeout
            // makes a regression fail in seconds instead of hanging until
            // ctest's timeout.
            int late = connect_local(small_port);
            EXPECT(late >= 0);
            struct timeval tv {};
            tv.tv_sec = 5;
            ::setsockopt(late, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            std::string carry;
            send_all(late, kGet11);
            std::string r = read_one_response(late, carry);
            EXPECT(r.rfind("HTTP/1.1 200 ", 0) == 0);

            // And every parked connection is still live: its next request
            // is picked up off the poll set and served.
            for (int i = 0; i < kParked; ++i) {
                ::setsockopt(parked[i], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                send_all(parked[i], kGet11);
                std::string again = read_one_response(parked[i], parked_carry[i]);
                EXPECT(again.find("Connection: keep-alive\r\n") != std::string::npos);
            }

            ::close(late);
            for (int fd : silent) {
                ::close(fd);
            }
            for (int fd : parked) {
                ::close(fd);
            }
            small_server.stop();
        } else {
            std::cout << "  (skipped reactor pool case: could not bind an ephemeral port)\n";
        }
    }

    // The connection lifetime cap, injectable through Config so it can be
    // exercised without waiting five minutes. The cap is enforced only on a
    // RESPONSE (Connection: close on the first one past it), never by the
    // reactor closing an idle socket at the cap: that would race a polling
    // client's next request, which would meet EOF instead of an answer. So
    // an active connection sees the close header, and an idle one is still
    // open past the cap and gets the header on its next request.
    {
        alpacahttp::Config cap_config;
        cap_config.set_http_port(0);
        cap_config.set_discovery_enabled(false);
        cap_config.set_server_name("TestServerLifetime");
        cap_config.set_keep_alive_lifetime_seconds(2);
        alpacahttp::Server cap_server(cap_config);
        cap_server.start_async();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        if (const std::uint16_t cap_port = cap_server.is_running() ? wait_for_bound_port(cap_server, 2000) : 0;
            cap_port != 0) {
            // Idle connection: one request, then silence.
            int idle_fd = connect_local(cap_port);
            EXPECT(idle_fd >= 0);
            std::string idle_carry;
            send_all(idle_fd, kGet11);
            EXPECT(read_one_response(idle_fd, idle_carry).find("Connection: keep-alive\r\n") != std::string::npos);

            // Active connection: a request every 250 ms until the cap fires.
            int active_fd = connect_local(cap_port);
            EXPECT(active_fd >= 0);
            std::string active_carry;
            bool got_close = false;
            int served = 0;
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 20 && !got_close; ++i) {
                send_all(active_fd, kGet11);
                std::string r = read_one_response(active_fd, active_carry);
                EXPECT(!r.empty());
                ++served;
                got_close = r.find("Connection: close\r\n") != std::string::npos;
                if (!got_close) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
            }
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            EXPECT(got_close);
            EXPECT(served >= 4);         // kept alive through most of the 2 s
            EXPECT(elapsed_ms >= 1900);  // not closed early
            EXPECT(elapsed_ms < 4000);   // not the 15 s idle gap or the 300 s default
            EXPECT(peer_closed(active_fd, 2000));

            // The idle one is past the cap too but still open (only the idle
            // gap closes a parked socket); its next request gets the close.
            EXPECT(!peer_closed(idle_fd, 200));
            send_all(idle_fd, kGet11);
            std::string idle_r = read_one_response(idle_fd, idle_carry);
            EXPECT(idle_r.rfind("HTTP/1.1 200 ", 0) == 0);
            EXPECT(idle_r.find("Connection: close\r\n") != std::string::npos);
            EXPECT(peer_closed(idle_fd, 2000));

            ::close(active_fd);
            ::close(idle_fd);
            cap_server.stop();
        } else {
            std::cout << "  (skipped lifetime-cap case: could not bind an ephemeral port)\n";
        }
    }

    // The connection bound (Config::max_connections). Idle connections cost
    // no worker, so without a bound the only limit on parked clients would
    // be the process's descriptor limit. At the bound the accept loop
    // pauses: a new client's handshake completes in the listen backlog and
    // its request waits, unanswered, until a connection is released.
    {
        alpacahttp::Config bound_config;
        bound_config.set_http_port(0);
        bound_config.set_discovery_enabled(false);
        bound_config.set_server_name("TestServerBound");
        bound_config.set_max_connections(2);
        alpacahttp::Server bound_server(bound_config);
        bound_server.start_async();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        if (const std::uint16_t bound_port = bound_server.is_running() ? wait_for_bound_port(bound_server, 2000) : 0;
            bound_port != 0) {
            int held[2];
            for (int& fd : held) {
                fd = connect_local(bound_port);
                EXPECT(fd >= 0);
                std::string carry;
                send_all(fd, kGet11);
                EXPECT(read_one_response(fd, carry).find("Connection: keep-alive\r\n") != std::string::npos);
            }

            // Third client: connects (kernel backlog) but is not accepted.
            int third = connect_local(bound_port);
            EXPECT(third >= 0);
            struct timeval tv {};
            tv.tv_usec = 700 * 1000;
            ::setsockopt(third, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            std::string carry;
            send_all(third, kGet11);
            EXPECT(read_one_response(third, carry).empty());  // nothing within 700 ms

            // Release one held connection; the third is accepted and served.
            ::close(held[0]);
            tv.tv_sec = 5;
            tv.tv_usec = 0;
            ::setsockopt(third, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            std::string r = read_one_response(third, carry);
            EXPECT(r.rfind("HTTP/1.1 200 ", 0) == 0);

            ::close(third);
            ::close(held[1]);
            bound_server.stop();
        } else {
            std::cout << "  (skipped max-connections case: could not bind an ephemeral port)\n";
        }
    }

    // The management restart endpoint tears the server down and brings it
    // back on a detached thread while the requesting client's connection is
    // still alive. Connection accounting (live_connections_, which gates
    // accept()) must survive that: a counter reset on start, or a queue
    // cleared without closing what it held, would let a connection that
    // straddles the restart drive the count below zero and block every
    // later accept. Two restarts back to back, each followed by a fresh
    // client that must be served, and a keep-alive client that lived
    // through the restart and is closed rather than leaked.
    {
        // Counter declared before the Server, so the Router that owns the
        // capturing lambda is destroyed first (open-astro#314 review).
        std::atomic<int> restart_probes{0};
        alpacahttp::Config restart_config;
        restart_config.set_http_port(0);
        restart_config.set_discovery_enabled(false);
        restart_config.set_server_name("TestServerRestart");
        restart_config.set_max_connections(3);
        restart_config.set_rtc_probe_interval_seconds(1);
        alpacahttp::Server restart_server(restart_config);
        // open-astro#314: the probe thread must come back with the new
        // generation. run_server() clears rtc_probe_stop_ before respawning;
        // deleting that line leaves the thread dead after the first restart,
        // which every other assertion here would happily ignore.
        restart_server.router_for_test().set_host_clock_hooks(
            [] { return true; }, [](std::chrono::system_clock::time_point, std::string&) { return true; },
            [&restart_probes] {
                ++restart_probes;
                return false;
            });
        restart_server.start_async();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        if (std::uint16_t restart_port = restart_server.is_running() ? wait_for_bound_port(restart_server, 2000) : 0;
            restart_port != 0) {
            const std::string restart_request =
                "PUT /management/restart HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n";
            struct timeval tv {};
            tv.tv_sec = 5;

            for (int round = 0; round < 2; ++round) {
                // A bystander parked on the reactor across the restart.
                int bystander = connect_local(restart_port);
                EXPECT(bystander >= 0);
                std::string bystander_carry;
                send_all(bystander, kGet11);
                EXPECT(read_one_response(bystander, bystander_carry).find("Connection: keep-alive\r\n") !=
                       std::string::npos);

                int fd = connect_local(restart_port);
                EXPECT(fd >= 0);
                ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                std::string carry;
                send_all(fd, restart_request);
                std::string r = read_one_response(fd, carry);
                EXPECT(r.rfind("HTTP/1.1 200 ", 0) == 0);
                ::close(fd);

                // The restart runs on a detached thread 100 ms after the
                // response, so polling is_running() here would race it (a
                // true seen before stop() begins is the OLD generation, and
                // a client connecting then lands in a listener about to be
                // closed and gets a reset). Wait for proof the restart
                // happened instead: stop() closes the parked bystander.
                EXPECT(peer_closed(bystander, 5000));
                ::close(bystander);
                // Then for the new generation to be up. is_running() goes
                // true before the new listener is bound, so retry connect.
                bool back = false;
                for (int i = 0; i < 50 && !back; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    back = restart_server.is_running();
                }
                EXPECT(back);
                // The new generation's listener is a fresh ephemeral port
                // (config_.http_port() is still 0), not necessarily the one
                // from before this restart -- re-read it before using
                // restart_port again, here and on the next round.
                restart_port = wait_for_bound_port(restart_server, 2000);
                EXPECT(restart_port != 0);

                // New generation accepts and serves, as many times as the
                // bound allows: a drifted counter would refuse all of them.
                for (int i = 0; i < 3; ++i) {
                    int after = -1;
                    for (int attempt = 0; attempt < 50 && after < 0; ++attempt) {
                        after = connect_local(restart_port);
                        if (after < 0) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        }
                    }
                    EXPECT(after >= 0);
                    ::setsockopt(after, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                    std::string after_carry;
                    send_all(after, kGet11);
                    std::string ra = read_one_response(after, after_carry);
                    EXPECT(ra.rfind("HTTP/1.1 200 ", 0) == 0);
                    ::close(after);
                }

                // The probe timer survived this restart: wait for a pass on
                // the new generation rather than a fixed sleep.
                const int probes_before = restart_probes.load();
                const auto probe_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (restart_probes.load() == probes_before && std::chrono::steady_clock::now() < probe_deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                EXPECT(restart_probes.load() > probes_before);
            }
            restart_server.stop();
        } else {
            std::cout << "  (skipped restart case: could not bind an ephemeral port)\n";
        }
    }

    // stop() straight after start_async(), with no settle time, repeatedly.
    // The spawn phase and stop() are serialized by a lifecycle mutex and
    // workers are counted at spawn, so a stop() that lands before a new
    // worker has executed an instruction still releases its wake permit
    // and the join completes. Before that, stop() could undercount and hang
    // on the uncounted thread. Ends with a normal start and a served request
    // to prove the object is still usable.
    {
        alpacahttp::Config churn_config;
        churn_config.set_http_port(0);
        churn_config.set_discovery_enabled(false);
        churn_config.set_server_name("TestServerChurn");
        churn_config.set_thread_pool_size(4);
        alpacahttp::Server churn_server(churn_config);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; ++i) {
            churn_server.start_async();
            churn_server.stop();
            EXPECT(!churn_server.is_running());
        }
        const auto churn_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        EXPECT(churn_ms < 10000);  // a hung join would sit here for good

        churn_server.start_async();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        if (const std::uint16_t churn_port = churn_server.is_running() ? wait_for_bound_port(churn_server, 2000) : 0;
            churn_port != 0) {
            int fd = connect_local(churn_port);
            EXPECT(fd >= 0);
            std::string carry;
            send_all(fd, kGet11);
            EXPECT(read_one_response(fd, carry).rfind("HTTP/1.1 200 ", 0) == 0);
            ::close(fd);
            churn_server.stop();
        } else {
            std::cout << "  (skipped churn case's final request: could not bind an ephemeral port)\n";
        }
    }

    // Closing must not destroy a response the client has not read yet. On
    // Linux, close() on a socket with unread bytes in its receive queue sends
    // RST instead of FIN, and the peer's stack then discards its own receive
    // buffer -- including the response we just sent. With keep-alive that is
    // ordinary: at the caps and on the stop() path the client usually has its
    // next request already on the wire. The server now shuts down its write
    // side and drains before close(). Here the "next request" is 10 KB of
    // trailing bytes in the same write as a Connection: close request: the
    // server's 8 KB recv leaves the tail queued in the kernel when it decides
    // to close, and the client deliberately waits before reading so the close
    // (FIN or RST) has arrived before it looks at the response.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        const std::string close_request =
            "GET /management/apiversions HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        send_all(fd, close_request + std::string(10 * 1024, 'x'));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::string r = read_one_response(fd, carry);
        EXPECT(r.rfind("HTTP/1.1 200 ", 0) == 0);
        EXPECT(r.find("Connection: close\r\n") != std::string::npos);
        // A clean EOF, not ECONNRESET: peer_closed() is true only on recv == 0.
        EXPECT(peer_closed(fd, 2000));
        ::close(fd);
    }

    // --- RTC probe thread (open-astro#314) ----------------------------------
    // The probe was moved off the request path, then off the reactor, onto its
    // own timer thread. Both #314 unit cases drive Router::refresh_rtc_probe()
    // directly, so deleting the thread's spawn left every one of them green --
    // and the bug the issue is about is precisely a host where nothing
    // re-probes: has_rtc() no longer probes, so with the thread gone the
    // startup answer is pinned for the life of the process.
    //
    // Drive it end to end instead: a real Server, a counting has_rtc hook, a
    // 1 s interval from Config (the default, one second past the probe's rate limit, is unwaitable), and an
    // assertion that the count rises on its own.
    {
        alpacahttp::Config rtc_config;
        rtc_config.set_http_port(0);
        rtc_config.set_discovery_enabled(false);
        rtc_config.set_server_name("TestServerRtcProbe");
        rtc_config.set_rtc_probe_interval_seconds(1);

        // Counter first, Server second: the Router owns the lambda that
        // captures &probes, so the Server must be destroyed first. The
        // explicit stop() below joins the thread anyway, but that is a
        // property of this case rather than of the declaration order.
        std::atomic<int> probes{0};
        alpacahttp::Server rtc_server(rtc_config);
        rtc_server.router_for_test().set_host_clock_hooks(
            [] { return true; }, [](std::chrono::system_clock::time_point, std::string&) { return true; },
            [&probes] {
                ++probes;
                return false;
            });
        // Installing the hooks primes the probe exactly once -- that is
        // #314's other half. Since open-astro#399 the single call comes from
        // the refresh_rtc() at the end of HostClock::set_hooks() rather than
        // from a fresh HostClock's constructor, because the seam no longer
        // builds a clock. The count is what matters either way: one probe at
        // install, and none on a request path.
        const int primed = probes.load();
        EXPECT(primed == 1);

        rtc_server.start_async();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        if (rtc_server.is_running()) {
            // No requests are sent: the whole point is that the refresh does
            // not depend on one arriving. Poll rather than sleeping one
            // interval and asserting: on a loaded runner thread start plus a
            // 1 s period can exceed any fixed margin, and waiting up to 5 s
            // for something that normally takes 1 s costs nothing when it
            // works.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (probes.load() == primed && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            EXPECT(probes.load() > primed);
        }
        rtc_server.stop();
        EXPECT(!rtc_server.is_running());
        // And the thread stops when the server does: no further passes after
        // the join returns.
        const int after_stop = probes.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        EXPECT(probes.load() == after_stop);
    }

    // --- Client-silence motion watchdog timer wiring (open-astro#547) ------
    // Mirrors the RTC probe case above end to end: a real Server, the same
    // rtc_probe_thread_ (retasked by #547 to also tick the watchdog every
    // second), and an assertion that a telescope left "slewing" with no
    // further client activity gets stopped on its own -- no request ever
    // asks it to.
    {
        alpacahttp::Config watchdog_config;
        watchdog_config.set_http_port(0);
        watchdog_config.set_discovery_enabled(false);
        watchdog_config.set_server_name("TestServerMotionWatchdog");
        watchdog_config.set_motion_watchdog_seconds(1);

        auto stub = std::make_shared<WatchdogStubTelescope>(9547);
        EXPECT(alpacacore::management::DeviceRegistry::instance().register_device(stub));

        alpacahttp::Server watchdog_server(watchdog_config);
        watchdog_server.start_async();
        const std::uint16_t port = watchdog_server.is_running() ? wait_for_bound_port(watchdog_server, 2000) : 0;
        EXPECT(port != 0);
        if (port != 0) {
            // One real routed request arms the watchdog (note_client_activity
            // at the router's device-dispatch choke point) -- after this, NO
            // further request is sent, so the only way `aborts` can rise is
            // the timer thread finding the device on its own.
            int fd = connect_local(port);
            EXPECT(fd >= 0);
            std::string carry;
            send_all(fd, "GET /api/v1/telescope/9547/connected HTTP/1.1\r\nHost: localhost\r\n\r\n");
            const std::string resp = read_one_response(fd, carry);
            EXPECT(resp.rfind("HTTP/1.1 200 ", 0) == 0);
            ::close(fd);

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (stub->aborts.load() == 0 && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            EXPECT(stub->aborts.load() > 0);
        }
        watchdog_server.stop();
        EXPECT(!watchdog_server.is_running());
        alpacacore::management::DeviceRegistry::instance().unregister_device(alpacacore::DeviceType::Telescope, 9547);
    }

    // --- Watchdog must not trip on the client's OWN in-flight synchronous
    // request (open-astro#547 review finding) -------------------------------
    // A synchronous SlewToCoordinates blocks the HTTP worker for the length
    // of the goto. note_client_activity() only stamps once, at intake, so
    // without begin_client_request()/end_client_request() bracketing the
    // dispatch, the timer thread finds the interval elapsed while the
    // client is still on the wire waiting on its own response -- and aborts
    // the very slew that client just issued.
    {
        alpacahttp::Config watchdog_config;
        watchdog_config.set_http_port(0);
        watchdog_config.set_discovery_enabled(false);
        watchdog_config.set_server_name("TestServerMotionWatchdogInFlight");
        watchdog_config.set_motion_watchdog_seconds(1);

        auto stub = std::make_shared<WatchdogStubTelescope>(9548);
        stub->slew_sleep_ms.store(3000);  // 3 s in-flight, 3x the 1 s interval
        EXPECT(alpacacore::management::DeviceRegistry::instance().register_device(stub));

        alpacahttp::Server watchdog_server(watchdog_config);
        watchdog_server.start_async();
        const std::uint16_t port = watchdog_server.is_running() ? wait_for_bound_port(watchdog_server, 2000) : 0;
        EXPECT(port != 0);
        if (port != 0) {
            std::thread client([&] {
                int fd = connect_local(port);
                if (fd < 0) {
                    return;
                }
                std::string carry;
                send_all(fd,
                         "PUT /api/v1/telescope/9548/slewtocoordinates?RightAscension=5&Declination=10"
                         " HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n");
                const std::string resp = read_one_response(fd, carry);
                EXPECT(resp.rfind("HTTP/1.1 200 ", 0) == 0);
                ::close(fd);
            });

            // The request is now blocking inside slew_to_coordinates() for
            // 3 s. Give the 1 s-interval timer thread two full ticks to find
            // it "silent" if the in-flight guard is missing.
            std::this_thread::sleep_for(std::chrono::milliseconds(2200));
            EXPECT(stub->aborts.load() == 0);

            // No abort check after the join: once the request ends, the stub
            // still reports Slewing (it is a stub) and its timestamp is up to
            // one interval old, so a tick landing right here legitimately
            // aborts. The in-flight window above is what this case pins.
            client.join();
        }
        watchdog_server.stop();
        EXPECT(!watchdog_server.is_running());
        alpacacore::management::DeviceRegistry::instance().unregister_device(alpacacore::DeviceType::Telescope, 9548);
    }

    // stop() must not wait out an ACTIVE keep-alive client. Before the
    // running_ check a client that kept sending (NINA/PHD2 polling) held its
    // worker, and therefore stop(), until the 300s lifetime cap; systemd
    // would SIGKILL the service at its 90s TimeoutStopSec first. Measured
    // 26s of stop() latency behind a client sending every 2s. Now a request
    // in flight when stop() begins is answered with "Connection: close", and
    // a connection idle on the reactor at that moment gets FIN, a shared
    // 100 ms window and a drain (its next request meets EOF), so the client
    // sees one or the other and stop() never waits on it. This must be the
    // last test: it stops the server.
    //
    // Also: idle connections parked on the reactor are closed by stop()
    // immediately. Before the reactor each one held a worker in recv, and
    // stop() had to wait out the idle gap for every one of them.
    {
        int idle_parked[2];
        for (int& fd : idle_parked) {
            fd = connect_local(port);
            EXPECT(fd >= 0);
            std::string carry;
            send_all(fd, kGet11);
            EXPECT(read_one_response(fd, carry).find("Connection: keep-alive\r\n") != std::string::npos);
        }

        std::atomic<bool> got_close{false};
        std::atomic<bool> got_eof{false};
        std::atomic<int> served{0};
        std::thread client([&] {
            int fd = connect_local(port);
            EXPECT(fd >= 0);
            std::string carry;
            for (int i = 0; i < 40; ++i) {  // up to ~20s of activity, every 500 ms
                send_all(fd, kGet11);
                std::string r = read_one_response(fd, carry);
                if (r.empty()) {
                    got_eof = true;  // server closed the socket while idle
                    break;
                }
                ++served;
                if (r.find("Connection: close\r\n") != std::string::npos) {
                    got_close = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            ::close(fd);
        });
        // Let the client get a couple of keep-alive responses in first.
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        const int served_before_stop = served.load();
        EXPECT(served_before_stop >= 2);

        const auto t0 = std::chrono::steady_clock::now();
        server.stop();
        const auto stop_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        client.join();

        EXPECT(!server.is_running());
        // One in-flight idle gap (<= 500 ms here) plus scheduling slack, not
        // the ~19 s the client was prepared to keep going.
        EXPECT(stop_ms < 5000);
        // Either a last answer marked close (request was in flight) or a
        // clean EOF (idle on the reactor); never a stall.
        EXPECT(got_close.load() || got_eof.load());
        // stop() answered at most one more request after being called.
        EXPECT(served.load() <= served_before_stop + 1);
        // The idle ones are gone too, and stop() did not wait on them.
        for (int fd : idle_parked) {
            EXPECT(peer_closed(fd, 2000));
            ::close(fd);
        }
    }

    // Issue #402: a run_server() that fails early must not leave a joinable
    // std::thread behind.
    {
        // bind() fails when the port is already in use. run_server() logs,
        // sets running_ = false and returns -- on the thread start_async()
        // already created and stored. stop() then early-returned on
        // `if (!running_)` without joining, and ~Server() destroyed a still
        // joinable std::thread, which calls std::terminate(). So a port
        // conflict became an abort at destruction rather than a clean failure
        // the caller could report, and the caller's own is_running() check
        // did not help: it correctly returned false and the crash came later.
        //
        // This case is the shape an embedder actually writes -- construct,
        // start_async(), see is_running() == false, destroy, try another port
        // -- so if the fix regresses, this binary aborts rather than failing
        // an assertion.
        alpacahttp::Config holder_config;
        holder_config.set_http_port(0);
        holder_config.set_discovery_enabled(false);
        holder_config.set_server_name("TestServerPortHolder");
        alpacahttp::Server holder(holder_config);
        holder.start_async();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        // The specific port doesn't matter here -- what matters is that
        // conflict_config and retry_config below target the SAME one holder
        // is already listening on, to force a genuine bind() collision
        // (#402). Reading it back keeps this test off a fixed literal
        // without weakening that collision. Gated on the port itself (not
        // just is_running(), which can go true before the listener is
        // actually bound) so a slow-but-eventually-successful bind on a
        // loaded runner takes the skip path below instead of a hard abort.
        const std::uint16_t holder_port = holder.is_running() ? wait_for_bound_port(holder, 2000) : 0;
        if (holder_port == 0) {
            // Say so. The whole #402 block hangs off this, and a silent skip
            // turns the flagship regression case into a green no-op on a
            // runner where no ephemeral port could be bound at all.
            std::cerr << "WARNING: port-conflict cases SKIPPED -- could not bind an ephemeral port\n";
        }
        if (holder_port != 0) {
            {
                alpacahttp::Config conflict_config;
                conflict_config.set_http_port(holder_port);  // already held
                conflict_config.set_discovery_enabled(false);
                conflict_config.set_server_name("TestServerPortConflict");
                alpacahttp::Server conflicted(conflict_config);
                conflicted.start_async();
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                EXPECT(!conflicted.is_running());
                // An explicit stop() before the destructor is the other order
                // an embedder writes; it must also be a no-crash no-op.
                conflicted.stop();
                EXPECT(!conflicted.is_running());
                // ...and the destructor runs here, on a Server whose thread
                // stop() has already reaped.
            }

            {
                // Retrying on the SAME Server after the failure. This is the
                // shape that exercises start_async()'s own join: the second
                // call assigns over server_thread_, and assigning over a
                // joinable std::thread is std::terminate(). It has to be one
                // object -- a fresh Server gets a fresh server_thread_ and
                // proves nothing about that path (which is exactly how this
                // test read before review: it built a second Server, so
                // deleting the join in start_async() left the suite green).
                alpacahttp::Config retry_config;
                retry_config.set_http_port(holder_port);
                retry_config.set_discovery_enabled(false);
                retry_config.set_server_name("TestServerPortRetry");
                alpacahttp::Server retried(retry_config);
                retried.start_async();
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                EXPECT(!retried.is_running());

                // No stop() in between: the embedder sees is_running() false
                // and simply tries another port on the same object. Without
                // the join in start_async() this aborts the binary.
                retried.start_async();
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                // Still the held port, so still not running -- the point is
                // that we got here at all.
                EXPECT(!retried.is_running());

                retry_config.set_http_port(0);  // any free port; does not need to collide
                alpacahttp::Server second(retry_config);
                second.start_async();
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                // The retry really came up, so the first failure left nothing
                // broken behind it.
                EXPECT(second.is_running());
                second.stop();
                EXPECT(!second.is_running());
            }
        }

        holder.stop();
        EXPECT(!holder.is_running());
    }

    {
        // Two threads calling stop() on the SAME running Server, which is the
        // shipped shutdown path, not a contrived one: PUT
        // /management/v1/shutdown spawns a detached thread that runs the
        // shutdown callback, and the example server's callback clears the flag
        // its own main loop polls -- so that loop calls stop() too, while the
        // detached thread is inside stop(). Both reach join_server_thread().
        //
        // Concurrent join() on one std::thread is UB; in practice the second
        // pthread_join throws std::system_error, which nothing catches, so the
        // process terminates. Like the port-conflict case above, a regression
        // here ABORTS this binary rather than failing an assertion.
        alpacahttp::Config concurrent_config;
        concurrent_config.set_http_port(0);
        concurrent_config.set_discovery_enabled(false);
        concurrent_config.set_server_name("TestServerConcurrentStop");
        alpacahttp::Server concurrent(concurrent_config);
        concurrent.start_async();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        if (!concurrent.is_running()) {
            std::cerr << "WARNING: concurrent-stop case SKIPPED -- could not bind an ephemeral port\n";
        }
        if (concurrent.is_running()) {
            // Released together so both land in stop() at once, which is what
            // makes them race for the join rather than queueing behind it.
            std::atomic<bool> go{false};
            std::atomic<int> finished{0};
            std::vector<std::thread> stoppers;
            for (int i = 0; i < 2; ++i) {
                stoppers.emplace_back([&]() {
                    while (!go.load()) {
                        std::this_thread::yield();
                    }
                    concurrent.stop();
                    finished.fetch_add(1);
                });
            }
            go.store(true);
            for (auto& t : stoppers) {
                t.join();
            }
            EXPECT(finished.load() == 2);
            EXPECT(!concurrent.is_running());
            // A third stop() after the fact is still a no-op, not a second
            // join of an already-reaped thread.
            concurrent.stop();
            EXPECT(!concurrent.is_running());
        }
    }

    {
        // The loser of the ownership race must not return from stop() early.
        // ~Server() runs straight after stop() and destroys the wake pipe, the
        // config and the connection maps that run_server() still reads, so a
        // stop() that returns while the accept loop is unwinding is a
        // use-after-free -- which is exactly what "return if another caller
        // took the thread" does.
        //
        // Both stoppers are joined before the Server is destroyed: a stopper
        // still inside stop() when the object dies is a *different* hazard
        // (the caller must outlive the callee) and not what this PR claims to
        // fix, so racing it here would only make the test unsound. What this
        // does exercise, many times over, is the interleaving itself -- one
        // caller in the !running_ branch winning the thread while the other
        // runs the full phases -- and the ASan and TSan pre-flight gates are
        // what turn a surviving run_server() into a report.
        //
        // The early-return regression was confirmed against this loop under
        // ASan by deleting the condition-variable wait in
        // join_server_thread(): heap-use-after-free in run_server() reading
        // the destroyed Server's running_ flag, on every run.
        for (int round = 0; round < 25; ++round) {
            alpacahttp::Config teardown_config;
            teardown_config.set_http_port(0);
            teardown_config.set_discovery_enabled(false);
            teardown_config.set_server_name("TestServerStopThenDestroy");

            auto server = std::make_unique<alpacahttp::Server>(teardown_config);
            server->start_async();
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            if (!server->is_running()) {
                std::cerr << "WARNING: stop-then-destroy loop SKIPPED at round " << round
                          << " -- could not bind an ephemeral port\n";
                break;
            }

            alpacahttp::Server* raw = server.get();
            std::atomic<bool> go{false};
            std::atomic<int> returned{0};
            std::thread other([&]() {
                while (!go.load()) {
                    std::this_thread::yield();
                }
                raw->stop();
                returned.fetch_add(1);
            });

            go.store(true);
            raw->stop();
            returned.fetch_add(1);

            other.join();
            EXPECT(returned.load() == 2);
            EXPECT(!raw->is_running());

            // Destroyed only once both stop() calls have returned. With the
            // wait in place that means the server thread is already reaped;
            // without it, run_server() can still be live here.
            server.reset();
        }
    }

    {
        // Regression test for #428's join_server_thread() fix, specifically
        // for wait() as one of the two racing callers -- issue #507 asked for
        // this because the block above only proves it for two stop() callers,
        // and a fix scoped to "the OTHER caller of stop()" rather than "any
        // other caller of join_server_thread()" would still let a wait() that
        // loses the ownership race return before the thread is truly gone.
        // wait() and stop() share join_server_thread(), so the same
        // early-return hazard applies: whichever call does not win the move
        // must still block until the winner's join() completes, not merely
        // find server_thread_ empty and return.
        //
        // stop() always runs its own teardown phases (closing the listener,
        // stopping the reactor and workers) before it ever reaches
        // join_server_thread(), regardless of which of the two calls wins
        // ownership of the actual join -- so this cannot deadlock even when
        // wait() wins the race and calls owned.join() first: stop()'s
        // concurrent phase 3 still closes the listener and unblocks the
        // accept loop that join() is waiting on.
        //
        // Same reasoning as the block above for why destruction is not
        // raced here: a caller still inside join_server_thread() when the
        // Server dies is a different hazard (the caller must outlive the
        // callee), not what this proves. Both calls are joined before the
        // Server is destroyed; what this exercises, many times over, is the
        // interleaving itself -- stop() and wait() reaching
        // join_server_thread() at the same moment, one of them losing the
        // move -- for the ASan/TSan pre-flight gates to turn a surviving
        // run_server() (reading a destroyed Server's members) into a report.
        for (int round = 0; round < 25; ++round) {
            alpacahttp::Config wait_config;
            wait_config.set_http_port(0);
            wait_config.set_discovery_enabled(false);
            wait_config.set_server_name("TestServerStopWaitRace");

            auto server = std::make_unique<alpacahttp::Server>(wait_config);
            server->start_async();
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            if (!server->is_running()) {
                std::cerr << "WARNING: stop/wait race loop SKIPPED at round " << round
                          << " -- could not bind an ephemeral port\n";
                break;
            }

            alpacahttp::Server* raw = server.get();
            std::atomic<bool> go{false};
            std::atomic<int> returned{0};
            std::thread waiter([&]() {
                while (!go.load()) {
                    std::this_thread::yield();
                }
                raw->wait();
                returned.fetch_add(1);
            });

            go.store(true);
            raw->stop();
            returned.fetch_add(1);

            waiter.join();
            EXPECT(returned.load() == 2);
            EXPECT(!raw->is_running());

            // Destroyed only once both stop() and wait() have returned. With
            // the fix that means the server thread is already reaped;
            // without it (a fix scoped to stop()-vs-stop() only rather than
            // every join_server_thread() caller), wait() could have returned
            // while run_server() was still unwinding.
            server.reset();
        }
    }

    {
        // The restart shape: one caller stops and immediately starts again
        // while another is still parked inside stop() waiting for the join.
        // Without a generation counter the waiter wakes after the restart has
        // installed a NEW server_thread_, adopts it and joins a server that is
        // still running -- stop() never returns. A regression HANGS here
        // rather than failing an assertion, which the watchdog below turns
        // into a reported failure.
        //
        // Repeated, because the window is the winner's join: the waiter has to
        // park on the condition variable while the winner is inside it, and
        // the winner has to finish and restart before the waiter re-acquires
        // the mutex.
        //
        // HONEST LIMIT: 40 restarts did NOT reach that window on this machine
        // -- the generation check was removed and this loop still passed, three
        // runs out of three. So treat it as an exerciser of the restart shape
        // (and material for the ASan/TSan gates), not as the regression test
        // for the adopt-the-next-generation bug. That one is argued in
        // join_server_thread()'s comment and would need a test seam inside the
        // join to pin properly.
        alpacahttp::Config restart_config;
        restart_config.set_http_port(0);
        restart_config.set_discovery_enabled(false);
        restart_config.set_server_name("TestServerRestartRace");

        alpacahttp::Server restarting(restart_config);
        restarting.start_async();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        if (!restarting.is_running()) {
            std::cerr << "WARNING: restart-race case SKIPPED -- could not bind an ephemeral port\n";
        } else {
            std::atomic<bool> done{false};
            std::atomic<long> waiter_returns{0};
            // The embedder loop: every time it sees the server go down it
            // calls stop() too, which is the caller that ends up parked.
            std::thread waiter([&]() {
                while (!done.load()) {
                    if (!restarting.is_running()) {
                        restarting.stop();
                        waiter_returns.fetch_add(1);
                    }
                    std::this_thread::yield();
                }
            });

            for (int round = 0; round < 40; ++round) {
                restarting.stop();
                restarting.start_async();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }

            done.store(true);
            waiter.join();
            // Reaching here at all is the assertion: a waiter that adopted a
            // restarted thread would still be inside stop() and this join
            // would never return -- when the window is actually hit.
            EXPECT(waiter_returns.load() >= 0);

            restarting.stop();
            EXPECT(!restarting.is_running());
        }
    }

    {
        // (issue #561, case a2) join_or_abandon()'s fallback is unreachable
        // from a real Server through ordinary testing -- nothing can make
        // join_server_thread()'s join() throw. ScopedJoinHooksForTest makes
        // the ONE production call site named "join_server_thread" throw,
        // while every other call site (worker/reactor/rtc-probe joins) still
        // really joins, so this proves the fallback fires end-to-end and
        // stop() still returns rather than propagating or hanging.
        std::mutex captured_mutex;
        std::vector<std::string> captured;
        std::mutex stopped_mutex;
        std::condition_variable stopped_cv;
        bool stopped_seen = false;
        // Default Config{} logs at WARNING, which would filter out the INFO
        // "Server stopped" line below before it ever reaches the sink (see
        // logging.cpp's level gate) -- need INFO to observe it.
        alpacahttp::Config logging_config;
        logging_config.set_log_level(alpacahttp::LogLevel::INFO);
        alpacahttp::util::init_logging(logging_config);
        alpacahttp::util::set_external_log_sink(
            [&](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
                if (message == "Server stopped") {
                    // run_server()'s very last statement before it returns. Once
                    // this fires, the (possibly detached, see below) thread
                    // running it is done touching the Server object and it is
                    // safe to let `server` go out of scope.
                    std::lock_guard<std::mutex> lock(stopped_mutex);
                    stopped_seen = true;
                    stopped_cv.notify_all();
                }
                if (level != alpacacore::logging::LogLevel::Error) {
                    return;
                }
                std::lock_guard<std::mutex> lock(captured_mutex);
                captured.emplace_back(message);
            });

        alpacahttp::Config config;
        config.set_http_port(0);
        config.set_discovery_enabled(false);
        config.set_server_name("TestServerJoinFallback");

        // `server` is declared OUTSIDE the hooks/watchdog scope below so that
        // its destructor runs strictly AFTER ScopedJoinHooksForTest's, per
        // thread_join.h's documented contract ("must be destroyed before any
        // Server instance that may still be joining threads is torn down").
        // With hooks still installed during ~Server(), the SKIPPED branch's
        // own teardown (join_server_thread() reaping the early-returned
        // thread) would run under the injected throw too, masking a bind
        // failure as a spurious fallback-error match.
        alpacahttp::Server server(config);
        bool ran = false;
        {
            Watchdog watchdog(std::chrono::seconds(10), "case a2 (join_server_thread fallback)");

            alpacahttp::detail::ScopedJoinHooksForTest hooks(
                [](std::thread& t, const char* context) {
                    if (std::strcmp(context, "join_server_thread") == 0) {
                        throw std::system_error(EDEADLK, std::generic_category());
                    }
                    t.join();
                },
                [](std::thread& t, const char*) { t.detach(); });

            server.start_async();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!server.is_running()) {
                std::cerr << "WARNING: join-fallback case SKIPPED -- could not bind an ephemeral port\n";
            } else {
                ran = true;
                server.stop();  // Must return despite the injected join() throw.
                server.wait();  // Must also return -- both funnel through join_server_thread().
                EXPECT(!server.is_running());
                server.stop();  // Second stop() is a no-op; must not re-throw or hang.

                // The injected join() throw makes join_or_abandon() detach
                // the still-running server thread instead of joining it --
                // case a2's whole point. stop()/wait() return as soon as
                // that detach happens, but the now-detached OS thread is
                // still inside run_server(), which keeps touching `this`
                // (server_fd_, then the "Server stopped" log) right up to
                // its last statement. Without waiting for that thread to
                // actually finish, `server`'s destructor below can run
                // concurrently with those last touches -- a real
                // use-after-free window, not a theoretical one.
                std::unique_lock<std::mutex> lock(stopped_mutex);
                stopped_cv.wait(lock, [&stopped_seen] { return stopped_seen; });
            }
            // hooks (and its process-wide override) ARE destroyed here, at
            // the end of this scope -- strictly before `server` goes out of
            // scope below.
        }

        alpacahttp::util::set_external_log_sink(nullptr);
        if (ran) {
            std::lock_guard<std::mutex> lock(captured_mutex);
            bool saw_fallback_error = false;
            for (const auto& line : captured) {
                if (line.find("join_server_thread") != std::string::npos &&
                    line.find("pthread_join failed") != std::string::npos) {
                    saw_fallback_error = true;
                    break;
                }
            }
            EXPECT(saw_fallback_error);
        }
    }

    std::cout << "All server socket tests passed!\n";
    return 0;
}
