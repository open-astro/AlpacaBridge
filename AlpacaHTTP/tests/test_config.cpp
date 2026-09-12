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

#include <alpacacore/util/host_clock.h>
#include <alpacahttp/config.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iostream>

#include "test_assert.h"

int main() {
    std::cout << "Testing configuration...\n";

    alpacahttp::Config config;

    // Test default values
    EXPECT(config.http_port() == 6800);
    EXPECT(config.discovery_enabled() == true);
    EXPECT(config.log_level() == alpacahttp::LogLevel::WARNING);

    // Test setters
    config.set_http_port(8080);
    EXPECT(config.http_port() == 8080);

    config.set_discovery_enabled(false);
    EXPECT(config.discovery_enabled() == false);

    config.set_log_level(alpacahttp::LogLevel::DEBUG);
    EXPECT(config.log_level() == alpacahttp::LogLevel::DEBUG);

    config.set_server_name("MyServer");
    EXPECT(config.server_name() == "MyServer");

    config.set_manufacturer("MyManufacturer");
    EXPECT(config.manufacturer() == "MyManufacturer");

    config.set_location("MyLocation");
    EXPECT(config.location() == "MyLocation");

    // Test device enable/disable (default should be enabled)
    EXPECT(config.is_device_enabled("camera", 0) == true);

    // Connection bound and keep-alive lifetime cap: defaults, clamping
    // setters, config-file keys under [http], and environment overrides
    // (which take precedence over the file, like the other overrides).
    {
        alpacahttp::Config fresh;
        EXPECT(fresh.max_connections() == 512);
        EXPECT(fresh.keep_alive_lifetime_seconds() == 300);

        fresh.set_max_connections(0);
        EXPECT(fresh.max_connections() == 1);
        fresh.set_max_connections(100000);
        EXPECT(fresh.max_connections() == 4096);
        fresh.set_keep_alive_lifetime_seconds(0);
        EXPECT(fresh.keep_alive_lifetime_seconds() == 1);
        fresh.set_keep_alive_lifetime_seconds(42);
        EXPECT(fresh.keep_alive_lifetime_seconds() == 42);
        // open-astro#314: same shape for the RTC probe period. 31 s by
        // default, deliberately not the probe's own 30 s rate limit. The
        // relation is what matters (#406): a default at or below the limiter
        // has every other pass swallowed with nothing failing, so it is
        // asserted against the limiter itself, not against a literal.
        EXPECT(fresh.rtc_probe_interval_seconds() > alpacacore::util::HostClock::kRtcProbeRateLimit.count());
        EXPECT(fresh.rtc_probe_interval_seconds() == alpacacore::util::HostClock::kRtcProbeRateLimit.count() + 1);
        fresh.set_rtc_probe_interval_seconds(0);
        EXPECT(fresh.rtc_probe_interval_seconds() == 1);
        fresh.set_rtc_probe_interval_seconds(7);
        EXPECT(fresh.rtc_probe_interval_seconds() == 7);

        char path_template[] = "/tmp/alpacahttp_test_config_XXXXXX";
        int fd = ::mkstemp(path_template);
        EXPECT(fd >= 0);
        const std::string path = path_template;
        {
            std::ofstream out(path);
            out << "http:\n"
                   "  port: 6810\n"
                   "  max_connections: 7\n"
                   "  keep_alive_lifetime_seconds: 45\n";
        }
        ::close(fd);

        ::unsetenv("ALPACAHTTP_MAX_CONNECTIONS");
        ::unsetenv("ALPACAHTTP_KEEP_ALIVE_LIFETIME_SECONDS");
        alpacahttp::Config from_file;
        EXPECT(from_file.load(path));
        EXPECT(from_file.http_port() == 6810);
        EXPECT(from_file.max_connections() == 7);
        EXPECT(from_file.keep_alive_lifetime_seconds() == 45);

        ::setenv("ALPACAHTTP_MAX_CONNECTIONS", "9", 1);
        ::setenv("ALPACAHTTP_KEEP_ALIVE_LIFETIME_SECONDS", "0", 1);  // clamps to 1
        alpacahttp::Config from_env;
        EXPECT(from_env.load(path));
        EXPECT(from_env.max_connections() == 9);
        EXPECT(from_env.keep_alive_lifetime_seconds() == 1);
        ::unsetenv("ALPACAHTTP_MAX_CONNECTIONS");
        ::unsetenv("ALPACAHTTP_KEEP_ALIVE_LIFETIME_SECONDS");
        ::unlink(path.c_str());
    }

    // open-astro#289: sync_system_clock_from_clients under [server]. Default
    // on; the file can turn it off; unparseable values keep the default.
    {
        alpacahttp::Config fresh;
        EXPECT(fresh.sync_system_clock_from_clients() == true);
        fresh.set_sync_system_clock_from_clients(false);
        EXPECT(fresh.sync_system_clock_from_clients() == false);

        char path_template[] = "/tmp/alpacahttp_test_clock_XXXXXX";
        int fd = ::mkstemp(path_template);
        EXPECT(fd >= 0);
        const std::string path = path_template;
        {
            std::ofstream out(path);
            out << "server:\n"
                   "  profile_name: \"Field Rig\"\n"
                   "  sync_system_clock_from_clients: \"false\"\n";
        }
        ::close(fd);
        alpacahttp::Config from_file;
        EXPECT(from_file.load(path));
        EXPECT(from_file.profile_name() == "Field Rig");
        EXPECT(from_file.sync_system_clock_from_clients() == false);
        {
            std::ofstream out(path);
            out << "server:\n"
                   "  sync_system_clock_from_clients: maybe\n";
        }
        alpacahttp::Config bad_value;
        EXPECT(bad_value.load(path));
        EXPECT(bad_value.sync_system_clock_from_clients() == true);
        ::unlink(path.c_str());
    }

    std::cout << "All configuration tests passed!\n";
    return 0;
}
