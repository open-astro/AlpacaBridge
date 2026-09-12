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

#pragma once

#include <alpacacore/util/host_clock.h>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace alpacahttp {

enum class LogLevel {
    DEBUG,
    INFO,
    WARNING,
    ERROR
};

class Config {
public:
    Config() = default;
    ~Config() = default;

    // Load configuration from file
    bool load(const std::string& config_path);
    bool load_default();

    // Getters
    std::uint16_t http_port() const { return http_port_; }
    bool discovery_enabled() const { return discovery_enabled_; }
    LogLevel log_level() const { return log_level_; }
    const std::string& server_name() const { return server_name_; }
    const std::string& manufacturer() const { return manufacturer_; }
    const std::string& location() const { return location_; }
    const std::string& profile_name() const { return profile_name_; }
    // open-astro#289: let a client's Telescope.UTCDate write step the host
    // clock when the kernel reports it undisciplined (no NTP/RTC). Default on.
    bool sync_system_clock_from_clients() const { return sync_system_clock_from_clients_; }
    std::size_t thread_pool_size() const { return thread_pool_size_; }
    std::size_t max_connections() const { return max_connections_; }
    int keep_alive_lifetime_seconds() const { return keep_alive_lifetime_seconds_; }
    // open-astro#314: how often the server's timer thread re-probes the
    // hardware RTC. Settable for the same reason the keep-alive cap is:
    // a test cannot wait 31 s.
    int rtc_probe_interval_seconds() const { return rtc_probe_interval_seconds_; }
    const std::string& log_directory() const { return log_directory_; }
    bool file_logging_enabled() const { return file_logging_enabled_; }
    int log_retention_days() const { return log_retention_days_; }
    const std::string& config_path() const { return config_path_; }

    // Device enable/disable
    bool is_device_enabled(const std::string& device_type, std::uint32_t device_number) const;

    // Set values (for testing/overrides)
    void set_http_port(std::uint16_t port) { http_port_ = port; }
    void set_discovery_enabled(bool enabled) { discovery_enabled_ = enabled; }
    void set_log_level(LogLevel level) { log_level_ = level; }
    void set_server_name(const std::string& name) { server_name_ = name; }
    void set_manufacturer(const std::string& mfg) { manufacturer_ = mfg; }
    void set_location(const std::string& loc) { location_ = loc; }
    void set_profile_name(const std::string& name) { profile_name_ = name; }
    void set_sync_system_clock_from_clients(bool enabled) { sync_system_clock_from_clients_ = enabled; }
    void set_thread_pool_size(std::size_t size) {
        if (size < 1) size = 1;
        if (size > 256) size = 256;
        thread_pool_size_ = size;
    }
    void set_max_connections(std::size_t count) {
        if (count < 1) count = 1;
        if (count > 4096) count = 4096;
        max_connections_ = count;
    }
    void set_keep_alive_lifetime_seconds(int seconds) {
        if (seconds < 1) seconds = 1;
        keep_alive_lifetime_seconds_ = seconds;
    }
    // Values at or below HostClock::kRtcProbeRateLimit are a TEST seam
    // (test_server_socket.cpp drives the thread at 1 s): the probe's own
    // limiter still swallows the extra passes, so in production such a value
    // buys nothing and the default below is what a deployment runs on.
    void set_rtc_probe_interval_seconds(int seconds) {
        if (seconds < 1) seconds = 1;
        rtc_probe_interval_seconds_ = seconds;
    }
    void set_log_directory(const std::string& dir) { log_directory_ = dir; }
    void set_file_logging_enabled(bool enabled) { file_logging_enabled_ = enabled; }
    void set_log_retention_days(int days) { log_retention_days_ = days; }

private:
    std::uint16_t http_port_ = 6800;
    bool discovery_enabled_ = true;
    LogLevel log_level_ = LogLevel::WARNING;
    std::string server_name_ = "AlpacaHTTP";
    std::string manufacturer_ = "OpenAstro.net";
    std::string location_ = "";
    std::string profile_name_ = "";
    bool sync_system_clock_from_clients_ = true;
    std::size_t thread_pool_size_ = 32;  // Default: 32 concurrent requests (supports multiple devices + clients)
    // Upper bound on open client connections across all owners (idle on the
    // reactor, queued, or being served). Idle keep-alive connections cost no
    // worker, so without this the only limit would be RLIMIT_NOFILE (1024 on
    // a typical systemd unit); 512 leaves the other half for device SDKs,
    // serial ports and log files. At the bound the accept loop pauses and new
    // clients wait in the listen backlog until an idle connection expires.
    std::size_t max_connections_ = 512;
    // How long one keep-alive connection may stay persistent, in seconds,
    // regardless of request count. The next response after this forces a
    // reconnect (cheap: one handshake every few minutes for a long-lived
    // client). Settable so the cap can be tested without waiting five
    // minutes.
    int keep_alive_lifetime_seconds_ = 300;
    // One second above the probe's own rate limit, deliberately not equal
    // to it: equal periods race, and a pass landing microseconds early is
    // silently swallowed, which would make the effective period 60 s (#314).
    // Derived from the limiter rather than a second literal, so moving one
    // moves the other (#406).
    static constexpr int kDefaultRtcProbeIntervalSeconds =
        static_cast<int>(alpacacore::util::HostClock::kRtcProbeRateLimit.count()) + 1;
    int rtc_probe_interval_seconds_ = kDefaultRtcProbeIntervalSeconds;
    std::string log_directory_ = "/var/log/AlpacaBridge";
    bool file_logging_enabled_ = true;
    int log_retention_days_ = 90;  // 0 = forever
    std::string config_path_;

    // Device enable/disable map: "devicetype:number" -> enabled
    std::unordered_map<std::string, bool> device_enable_map_;

    LogLevel parse_log_level(const std::string& level_str);
    void apply_environment_overrides();
    void load_config_from_yaml(const std::string& config_path);
};

} // namespace alpacahttp
