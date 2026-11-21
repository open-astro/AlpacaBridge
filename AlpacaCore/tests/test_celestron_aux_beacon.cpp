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

// The SkyPortal adapter beacon listener against loopback senders. Senders broadcast to 127.255.255.255 (the
// loopback broadcast route), so every socket bound to the port gets a copy, as on the adapter's subnet, and nothing
// leaves the host. Each case takes a free port of its own, so parallel test processes never hear each other. The
// window is wire timing (decision 0005 rule 6, steady_clock), so the cases wait real, bounded time.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/vendor/celestron/celestron_aux_beacon.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"

namespace {

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::test::require_alpaca_error;
using alpacacore::vendor::celestron::AuxBeacon;
using alpacacore::vendor::celestron::listen_for_aux_beacons;
using std::chrono::milliseconds;

// The payload the Gen4 adapter sent on 2026-11-20, byte for byte.
const std::string kCapturedBeacon =
    R"({ "manufacturer":"Celestron" , "ssid":"Celestron-AFFC" , "ip":"1.2.3.4" , "module":"Telit WE310G4" , "Count":2016 })";

std::string beacon_from(const std::string& ip, const std::string& ssid) {
    return R"({ "manufacturer":"Celestron" , "ssid":")" + ssid + R"(" , "ip":")" + ip +
           R"(" , "module":"Telit WE310G4" , "Count":7 })";
}

class Socket {
public:
    Socket() : fd_(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) {}
    ~Socket() {
        if (fd_ >= 0) ::close(fd_);
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    int fd() const { return fd_; }

private:
    int fd_;
};

bool bind_to(int fd, const char* address, int port) {
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(static_cast<std::uint16_t>(port));
    ::inet_pton(AF_INET, address, &local.sin_addr);
    return ::bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == 0;
}

int bound_port(int fd) {
    sockaddr_in local{};
    socklen_t size = sizeof(local);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &size) != 0) return -1;
    return ntohs(local.sin_port);
}

// A free UDP port, held by `holder` with SO_REUSEADDR (as another beacon listener would hold it).
int hold_free_port(Socket& holder) {
    const int on = 1;
    ::setsockopt(holder.fd(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (!bind_to(holder.fd(), "0.0.0.0", 0)) return -1;
    return bound_port(holder.fd());
}

int free_port() {
    Socket probe;
    return hold_free_port(probe);
}

// Broadcasts its datagrams from `source` (a 127/8 address) every 50 ms until destroyed, so a datagram lands after
// the listener has bound whatever the scheduling, as the adapter's 2 s beacon does.
class BeaconSender {
public:
    BeaconSender(std::string source, int port, std::vector<std::string> datagrams)
        : source_(std::move(source)), port_(port), datagrams_(std::move(datagrams)) {
        const int on = 1;
        ::setsockopt(socket_.fd(), SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
        ok_ = bind_to(socket_.fd(), source_.c_str(), 0);
        thread_ = std::thread([this] { run(); });
    }
    ~BeaconSender() {
        stop_.store(true);
        thread_.join();
    }
    BeaconSender(const BeaconSender&) = delete;
    BeaconSender& operator=(const BeaconSender&) = delete;
    bool ok() const { return ok_; }
    int sent() const { return sent_.load(); }

private:
    void run() {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(static_cast<std::uint16_t>(port_));
        ::inet_pton(AF_INET, "127.255.255.255", &to.sin_addr);
        while (!stop_.load()) {
            for (const auto& datagram : datagrams_) {
                if (::sendto(socket_.fd(), datagram.data(), datagram.size(), 0, reinterpret_cast<const sockaddr*>(&to),
                             sizeof(to)) == static_cast<ssize_t>(datagram.size())) {
                    sent_.fetch_add(1);
                }
            }
            // Real time: this paces a loopback sender, it waits for no driver timer.
            std::this_thread::sleep_for(milliseconds(50));
        }
    }

    Socket socket_;
    std::string source_;
    int port_;
    std::vector<std::string> datagrams_;
    bool ok_ = false;
    std::atomic<bool> stop_{false};
    std::atomic<int> sent_{0};
    std::thread thread_;
};

std::vector<std::string> ips_of(const std::vector<AuxBeacon>& beacons) {
    std::vector<std::string> ips;
    for (const auto& beacon : beacons) ips.push_back(beacon.ip);
    return ips;
}

}  // namespace

TEST_CASE("Celestron SkyPortal beacon - finds one adapter and parses its beacon", "[celestron][beacon]") {
    const int port = free_port();
    REQUIRE(port > 0);
    BeaconSender adapter("127.0.0.2", port, {kCapturedBeacon});
    REQUIRE(adapter.ok());
    const auto started = std::chrono::steady_clock::now();

    const auto found = listen_for_aux_beacons(milliseconds(400), port);

    // A steady stream of beacons does not stretch the window.
    CHECK(std::chrono::steady_clock::now() - started < milliseconds(900));
    REQUIRE(found.size() == 1);  // heard several times, listed once
    CHECK(found[0].ip == "127.0.0.2");
    CHECK(found[0].ssid == "Celestron-AFFC");
    CHECK(found[0].module == "Telit WE310G4");
    CHECK(adapter.sent() > 1);
}

TEST_CASE("Celestron SkyPortal beacon - the address is the datagram's source, not the payload's ip field",
          "[celestron][beacon]") {
    const int port = free_port();
    REQUIRE(port > 0);
    BeaconSender adapter("127.0.0.3", port, {beacon_from("10.9.8.7", "Celestron-0001")});
    REQUIRE(adapter.ok());

    const auto found = listen_for_aux_beacons(milliseconds(400), port);

    CHECK(ips_of(found) == std::vector<std::string>{"127.0.0.3"});
}

TEST_CASE("Celestron SkyPortal beacon - an empty window returns nothing within its bound", "[celestron][beacon]") {
    const int port = free_port();
    REQUIRE(port > 0);
    const auto started = std::chrono::steady_clock::now();

    const auto found = listen_for_aux_beacons(milliseconds(300), port);

    const auto elapsed = std::chrono::duration_cast<milliseconds>(std::chrono::steady_clock::now() - started);
    CHECK(found.empty());
    CHECK(elapsed >= milliseconds(300));
    CHECK(elapsed < milliseconds(800));
}

TEST_CASE("Celestron SkyPortal beacon - two adapters are both listed", "[celestron][beacon]") {
    const int port = free_port();
    REQUIRE(port > 0);
    // Both factory-default adapters claim 1.2.3.4 in their payload; only the source address tells them apart.
    BeaconSender first("127.0.0.4", port, {beacon_from("1.2.3.4", "Celestron-AAAA")});
    BeaconSender second("127.0.0.5", port, {beacon_from("1.2.3.4", "Celestron-BBBB")});
    REQUIRE(first.ok());
    REQUIRE(second.ok());

    const auto found = listen_for_aux_beacons(milliseconds(400), port);

    REQUIRE(found.size() == 2);
    CHECK((ips_of(found) == std::vector<std::string>{"127.0.0.4", "127.0.0.5"} ||
           ips_of(found) == std::vector<std::string>{"127.0.0.5", "127.0.0.4"}));
    for (const auto& beacon : found) {
        CHECK(beacon.ssid == (beacon.ip == "127.0.0.4" ? "Celestron-AAAA" : "Celestron-BBBB"));
    }
}

TEST_CASE("Celestron SkyPortal beacon - garbage datagrams are ignored", "[celestron][beacon]") {
    const int port = free_port();
    REQUIRE(port > 0);
    BeaconSender noise("127.0.0.6", port,
                       {
                           std::string(),                                           // empty datagram
                           std::string("\x3b\x03\x20\x11\x01\xcb", 6),              // an AUX frame
                           "not json at all",                                       //
                           R"({ "manufacturer":"Sky-Watcher" , "ip":"1.2.3.4" })",  // another vendor
                           R"({ "manufacturer":"Celest)",                           // truncated
                           R"({ "manufacturer" "Celestron" })",                     // no colon
                           R"({ "ssid":"Celestron-AFFC" })",                        // no manufacturer
                           R"({ "vendor":"x", "note":"manufacturer:Celestron" })",  // the word, not the field
                           std::string(2000, 'C'),                                  // oversize
                       });
    REQUIRE(noise.ok());
    BeaconSender adapter("127.0.0.7", port, {kCapturedBeacon});
    REQUIRE(adapter.ok());

    const auto found = listen_for_aux_beacons(milliseconds(400), port);

    CHECK(ips_of(found) == std::vector<std::string>{"127.0.0.7"});
    CHECK(noise.sent() > 9);
}

TEST_CASE("Celestron SkyPortal beacon - another listener on the port is no obstacle", "[celestron][beacon]") {
    Socket other_listener;
    const int port = hold_free_port(other_listener);
    REQUIRE(port > 0);
    BeaconSender adapter("127.0.0.8", port, {kCapturedBeacon});
    REQUIRE(adapter.ok());

    const auto found = listen_for_aux_beacons(milliseconds(400), port);

    CHECK(ips_of(found) == std::vector<std::string>{"127.0.0.8"});
}

TEST_CASE("Celestron SkyPortal beacon - a port held without SO_REUSEADDR is a driver error naming it",
          "[celestron][beacon]") {
    Socket exclusive;
    REQUIRE(bind_to(exclusive.fd(), "0.0.0.0", 0));
    const int port = bound_port(exclusive.fd());
    REQUIRE(port > 0);

    try {
        (void)listen_for_aux_beacons(milliseconds(100), port);
        FAIL("listening on a held port must throw");
    } catch (const alpacacore::AlpacaException& error) {
        CHECK(error.error_code() == AlpacaError::DriverException);
        CHECK(std::string(error.what()).find(std::to_string(port)) != std::string::npos);
    }
}

TEST_CASE("Celestron SkyPortal beacon - a bad window or port is an invalid value", "[celestron][beacon]") {
    require_alpaca_error([] { (void)listen_for_aux_beacons(milliseconds(0)); }, AlpacaError::InvalidValue);
    require_alpaca_error([] { (void)listen_for_aux_beacons(milliseconds(10001)); }, AlpacaError::InvalidValue);
    require_alpaca_error([] { (void)listen_for_aux_beacons(milliseconds(100), 0); }, AlpacaError::InvalidValue);
    require_alpaca_error([] { (void)listen_for_aux_beacons(milliseconds(100), 65536); }, AlpacaError::InvalidValue);
}
