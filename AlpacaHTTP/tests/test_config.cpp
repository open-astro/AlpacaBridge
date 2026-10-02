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
#include <alpacacore/util/motion_policy.h>
#include <alpacahttp/config.h>
#include <alpacahttp/software_update.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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
        // open-astro#314: same shape for the RTC probe period. The default
        // is deliberately above the probe's own rate limit. The relation is
        // what matters (#406): a default at or below the limiter has passes
        // swallowed with nothing failing, so it is asserted against the
        // limiter itself, not against a literal (and not against the
        // definition's `+ 1`, which would only restate config.h).
        EXPECT(fresh.rtc_probe_interval_seconds() > alpacacore::util::HostClock::kRtcProbeRateLimit.count());
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

    // Software update (docs/software-update.md): update_packages_url under
    // [server]. The default is the arm64 Trixie index; the file and the
    // environment both override it, and an explicit empty value (which
    // disables the check) is kept rather than replaced by the default.
    {
        alpacahttp::Config fresh;
        EXPECT(fresh.update_packages_url() == std::string(alpacahttp::util::kDefaultPackagesUrl));
        EXPECT(fresh.update_packages_url().find("apt.openastro.net") != std::string::npos);
        fresh.set_update_packages_url("https://mirror.example/Packages");
        EXPECT(fresh.update_packages_url() == "https://mirror.example/Packages");

        char path_template[] = "/tmp/alpacahttp_test_update_url_XXXXXX";
        int fd = ::mkstemp(path_template);
        EXPECT(fd >= 0);
        const std::string path = path_template;
        {
            std::ofstream out(path);
            // The value carries a colon of its own; the parser must split on
            // the first one only.
            out << "server:\n"
                   "  update_packages_url: https://mirror.example/dists/trixie/main/binary-arm64/Packages  # note\n";
        }
        ::close(fd);

        ::unsetenv("ALPACAHTTP_UPDATE_PACKAGES_URL");
        alpacahttp::Config from_file;
        EXPECT(from_file.load(path));
        EXPECT(from_file.update_packages_url() == "https://mirror.example/dists/trixie/main/binary-arm64/Packages");

        {
            std::ofstream out(path);
            out << "server:\n"
                   "  update_packages_url: \"\"\n";
        }
        alpacahttp::Config from_file_empty;
        EXPECT(from_file_empty.load(path));
        EXPECT(from_file_empty.update_packages_url().empty());

        ::setenv("ALPACAHTTP_UPDATE_PACKAGES_URL", "https://env.example/Packages", 1);
        alpacahttp::Config from_env;
        EXPECT(from_env.load(path));
        EXPECT(from_env.update_packages_url() == "https://env.example/Packages");
        ::setenv("ALPACAHTTP_UPDATE_PACKAGES_URL", "", 1);
        alpacahttp::Config from_env_empty;
        EXPECT(from_env_empty.load(path));
        EXPECT(from_env_empty.update_packages_url().empty());
        ::unsetenv("ALPACAHTTP_UPDATE_PACKAGES_URL");

        // The release-notes and release-page templates follow the same rules.
        EXPECT(fresh.update_release_notes_url() == std::string(alpacahttp::util::kDefaultReleaseNotesUrl));
        EXPECT(fresh.update_release_url() == std::string(alpacahttp::util::kDefaultReleaseUrl));
        EXPECT(fresh.update_release_notes_url().find("{version}") != std::string::npos);
        {
            std::ofstream out(path);
            out << "server:\n"
                   "  update_release_notes_url: https://notes.example/{version}.md\n"
                   "  update_release_url: \"\"\n";
        }
        ::unsetenv("ALPACAHTTP_UPDATE_RELEASE_NOTES_URL");
        ::unsetenv("ALPACAHTTP_UPDATE_RELEASE_URL");
        alpacahttp::Config templates_from_file;
        EXPECT(templates_from_file.load(path));
        EXPECT(templates_from_file.update_release_notes_url() == "https://notes.example/{version}.md");
        EXPECT(templates_from_file.update_release_url().empty());
        ::setenv("ALPACAHTTP_UPDATE_RELEASE_NOTES_URL", "", 1);
        ::setenv("ALPACAHTTP_UPDATE_RELEASE_URL", "https://rel.example/v{version}", 1);
        alpacahttp::Config templates_from_env;
        EXPECT(templates_from_env.load(path));
        EXPECT(templates_from_env.update_release_notes_url().empty());
        EXPECT(templates_from_env.update_release_url() == "https://rel.example/v{version}");
        ::unsetenv("ALPACAHTTP_UPDATE_RELEASE_NOTES_URL");
        ::unsetenv("ALPACAHTTP_UPDATE_RELEASE_URL");

        ::unlink(path.c_str());
    }

    // open-astro#547: motion_watchdog_seconds under [server], next to
    // sync_system_clock_from_clients. Default matches AlpacaCore's
    // kClientSilenceStopInterval (util/motion_policy.h), which is the two
    // constants' shared home with open-astro#521's relink window. 0 = the
    // watchdog is disabled outright (no upper clamp: an operator with a very
    // slow polling client may want longer than 30 s).
    {
        alpacahttp::Config fresh;
        EXPECT(fresh.motion_watchdog_seconds() ==
               static_cast<int>(alpacacore::util::kClientSilenceStopInterval.count()));

        fresh.set_motion_watchdog_seconds(-3);
        EXPECT(fresh.motion_watchdog_seconds() == 0);
        fresh.set_motion_watchdog_seconds(0);
        EXPECT(fresh.motion_watchdog_seconds() == 0);
        fresh.set_motion_watchdog_seconds(45);
        EXPECT(fresh.motion_watchdog_seconds() == 45);

        char path_template[] = "/tmp/alpacahttp_test_watchdog_XXXXXX";
        int fd = ::mkstemp(path_template);
        EXPECT(fd >= 0);
        const std::string path = path_template;
        {
            std::ofstream out(path);
            out << "server:\n"
                   "  motion_watchdog_seconds: 5\n";
        }
        ::close(fd);

        ::unsetenv("ALPACAHTTP_MOTION_WATCHDOG_SECONDS");
        alpacahttp::Config from_file;
        EXPECT(from_file.load(path));
        EXPECT(from_file.motion_watchdog_seconds() == 5);

        {
            std::ofstream out(path);
            out << "server:\n"
                   "  motion_watchdog_seconds: 0\n";
        }
        alpacahttp::Config from_file_disabled;
        EXPECT(from_file_disabled.load(path));
        EXPECT(from_file_disabled.motion_watchdog_seconds() == 0);

        {
            std::ofstream out(path);
            out << "server:\n"
                   "  motion_watchdog_seconds: -7\n";
        }
        alpacahttp::Config from_file_negative;
        EXPECT(from_file_negative.load(path));
        EXPECT(from_file_negative.motion_watchdog_seconds() == 0);

        ::setenv("ALPACAHTTP_MOTION_WATCHDOG_SECONDS", "12", 1);
        alpacahttp::Config from_env;
        EXPECT(from_env.load(path));
        EXPECT(from_env.motion_watchdog_seconds() == 12);
        ::unsetenv("ALPACAHTTP_MOTION_WATCHDOG_SECONDS");

        {
            std::ofstream out(path);
            out << "server:\n"
                   "  motion_watchdog_seconds: banana\n";
        }
        alpacahttp::Config from_file_garbage;
        EXPECT(from_file_garbage.load(path));
        EXPECT(from_file_garbage.motion_watchdog_seconds() ==
               static_cast<int>(alpacacore::util::kClientSilenceStopInterval.count()));

        ::unlink(path.c_str());
    }

    // open-astro#392: http.allowed_hosts, one comma-separated string, and its
    // env override. Entries are trimmed and empty ones dropped; the router
    // normalizes them.
    {
        ::unsetenv("ALPACAHTTP_ALLOWED_HOSTS");
        alpacahttp::Config fresh;
        EXPECT(fresh.allowed_hosts().empty());

        char path_template[] = "/tmp/alpacahttp_test_allowed_hosts_XXXXXX";
        int fd = ::mkstemp(path_template);
        EXPECT(fd >= 0);
        const std::string path = path_template;
        {
            std::ofstream out(path);
            out << "http:\n"
                   "  port: 6800\n"
                   "  allowed_hosts: \".lan, astropi.home\"  # a comment\n"
                   "server:\n"
                   "  allowed_hosts: wrong.section\n";
        }
        ::close(fd);

        alpacahttp::Config from_file;
        EXPECT(from_file.load(path));
        EXPECT((from_file.allowed_hosts() == std::vector<std::string>{".lan", "astropi.home"}));

        {
            std::ofstream out(path);
            out << "http:\n"
                   "  allowed_hosts: a, ,b,\n";
        }
        alpacahttp::Config from_file_empty_entries;
        EXPECT(from_file_empty_entries.load(path));
        EXPECT((from_file_empty_entries.allowed_hosts() == std::vector<std::string>{"a", "b"}));

        ::setenv("ALPACAHTTP_ALLOWED_HOSTS", " .fritz.box ,, pi.lan ", 1);
        alpacahttp::Config from_env;
        EXPECT(from_env.load(path));
        EXPECT((from_env.allowed_hosts() == std::vector<std::string>{".fritz.box", "pi.lan"}));

        // An explicitly empty variable overrides the file with no entries.
        ::setenv("ALPACAHTTP_ALLOWED_HOSTS", "", 1);
        alpacahttp::Config from_env_empty;
        EXPECT(from_env_empty.load(path));
        EXPECT(from_env_empty.allowed_hosts().empty());
        ::unsetenv("ALPACAHTTP_ALLOWED_HOSTS");

        ::unlink(path.c_str());
    }

    // http.host_check_enabled: off unless set, and ALPACAHTTP_HOST_CHECK
    // overrides the file either way.
    {
        ::unsetenv("ALPACAHTTP_HOST_CHECK");
        alpacahttp::Config fresh;
        EXPECT(!fresh.host_check_enabled());

        char path_template[] = "/tmp/alpacahttp_test_host_check_XXXXXX";
        int fd = ::mkstemp(path_template);
        EXPECT(fd >= 0);
        const std::string path = path_template;
        ::close(fd);
        const auto write_file = [&path](const char* value) {
            std::ofstream out(path);
            out << "http:\n  port: 6800\n";
            if (*value != '\0') {
                out << "  host_check_enabled: " << value << "  # a comment\n";
            }
        };

        write_file("");
        alpacahttp::Config absent;
        EXPECT(absent.load(path));
        EXPECT(!absent.host_check_enabled());

        write_file("true");
        alpacahttp::Config file_on;
        EXPECT(file_on.load(path));
        EXPECT(file_on.host_check_enabled());

        write_file("false");
        ::setenv("ALPACAHTTP_HOST_CHECK", "true", 1);
        alpacahttp::Config env_on;
        EXPECT(env_on.load(path));
        EXPECT(env_on.host_check_enabled());

        write_file("true");
        ::setenv("ALPACAHTTP_HOST_CHECK", "false", 1);
        alpacahttp::Config env_off;
        EXPECT(env_off.load(path));
        EXPECT(!env_off.host_check_enabled());

        // An unparseable variable leaves the file's value.
        ::setenv("ALPACAHTTP_HOST_CHECK", "maybe", 1);
        alpacahttp::Config env_bad;
        EXPECT(env_bad.load(path));
        EXPECT(env_bad.host_check_enabled());
        ::unsetenv("ALPACAHTTP_HOST_CHECK");

        ::unlink(path.c_str());
    }

    // The web UI may change the two Host check settings unless the
    // environment fixes them. Config says which: ALPACAHTTP_ALLOWED_HOSTS
    // fixes the list whenever it is set, even empty; ALPACAHTTP_HOST_CHECK
    // fixes the flag whenever it is set; an unparseable value leaves the
    // file's value in force but still keeps the web UI from changing it.
    {
        ::unsetenv("ALPACAHTTP_HOST_CHECK");
        ::unsetenv("ALPACAHTTP_ALLOWED_HOSTS");
        alpacahttp::Config fresh;
        EXPECT(!fresh.host_check_env_fixed());
        EXPECT(!fresh.allowed_hosts_env_fixed());

        char path_template[] = "/tmp/alpacahttp_test_host_env_fixed_XXXXXX";
        int fd = ::mkstemp(path_template);
        EXPECT(fd >= 0);
        const std::string path = path_template;
        ::close(fd);
        {
            // The quoted form the description PUT writes.
            std::ofstream out(path);
            out << "http:\n"
                   "  host_check_enabled: \"true\"\n"
                   "  allowed_hosts: \".lan, astropi.home\"\n";
        }

        alpacahttp::Config from_file;
        EXPECT(from_file.load(path));
        EXPECT(from_file.host_check_enabled());
        EXPECT((from_file.allowed_hosts() == std::vector<std::string>{".lan", "astropi.home"}));
        EXPECT(!from_file.host_check_env_fixed());
        EXPECT(!from_file.allowed_hosts_env_fixed());

        ::setenv("ALPACAHTTP_HOST_CHECK", "false", 1);
        alpacahttp::Config flag_fixed;
        EXPECT(flag_fixed.load(path));
        EXPECT(!flag_fixed.host_check_enabled());
        EXPECT(flag_fixed.host_check_env_fixed());
        EXPECT(!flag_fixed.allowed_hosts_env_fixed());

        ::setenv("ALPACAHTTP_HOST_CHECK", "maybe", 1);
        alpacahttp::Config flag_unparseable;
        EXPECT(flag_unparseable.load(path));
        EXPECT(flag_unparseable.host_check_enabled());
        EXPECT(flag_unparseable.host_check_env_fixed());
        ::unsetenv("ALPACAHTTP_HOST_CHECK");

        ::setenv("ALPACAHTTP_ALLOWED_HOSTS", "", 1);
        alpacahttp::Config list_fixed;
        EXPECT(list_fixed.load(path));
        EXPECT(list_fixed.allowed_hosts().empty());
        EXPECT(list_fixed.allowed_hosts_env_fixed());
        EXPECT(!list_fixed.host_check_env_fixed());
        ::unsetenv("ALPACAHTTP_ALLOWED_HOSTS");

        ::unlink(path.c_str());
    }

    // open-astro#392: a '#' inside a double-quoted value is data; one outside
    // starts a comment.
    {
        ::unsetenv("ALPACAHTTP_HOST_CHECK");
        ::unsetenv("ALPACAHTTP_ALLOWED_HOSTS");
        char path_template[] = "/tmp/alpacahttp_test_hash_quoted_XXXXXX";
        int fd = ::mkstemp(path_template);
        EXPECT(fd >= 0);
        ::close(fd);
        const std::string path = path_template;
        {
            std::ofstream out(path);
            out << "http:\n"
                   "  allowed_hosts: \"a#b, .lan\" # trailing comment\n";
        }
        alpacahttp::Config quoted;
        EXPECT(quoted.load(path));
        EXPECT((quoted.allowed_hosts() == std::vector<std::string>{"a#b", ".lan"}));
        ::unlink(path.c_str());
    }

    std::cout << "All configuration tests passed!\n";
    return 0;
}
