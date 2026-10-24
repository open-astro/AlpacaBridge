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

// Sky-Watcher getters never wait on a serial or UDP exchange (decision 0016,
// issue #1530). The driver runs on the real task clock against the loopback
// FakeSkyWatcherMount, and every reply of the fake is held for kLatency, so a
// getter that waits for the driver's state lock while a SetPark, a goto, a
// Park or a FindHome is on the wire pays the exchanges of the whole command.
// Decision 0005: the budget is wall time spent in a getter, not a driver
// timeout; no case here waits on a task-clock deadline.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "fake_skywatcher_mount.h"

namespace sw = alpacacore::vendor::skywatcher;
using alpacacore::test::FakeSkyWatcherMount;

namespace {

using std::chrono::milliseconds;
using std::chrono::steady_clock;

// One reply of the held fake. A command that runs a few exchanges under the
// driver's state lock keeps it for several of these.
constexpr auto kLatency = milliseconds(300);
// Far under one exchange: a getter that waited for even one reply would miss it.
constexpr auto kGetterBudget = milliseconds(100);
constexpr auto kSampleWindow = milliseconds(1500);

// Made-up site, Sydney Opera House.
constexpr double kSiteLatitude = -33.856784;
constexpr double kSiteLongitude = 151.215297;
constexpr double kSiteElevation = 42.0;

sw::ConnectionInfo endpoint(const FakeSkyWatcherMount& mount) {
    sw::ConnectionInfo info;
    info.type = sw::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.udp_port = mount.port();
    info.response_timeout_ms = 2000;
    return info;
}

std::unique_ptr<alpacacore::TelescopeDriver> connected_driver(const FakeSkyWatcherMount& mount) {
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), kSiteLatitude, kSiteLongitude, kSiteElevation);
    driver->set_connected(true);
    return driver;
}

struct Getter {
    const char* name;
    std::function<void(alpacacore::TelescopeDriver&)> read;
};

// The five members the issue names, then a sample of the others that read the
// same state lock (the class converts in one change, decision 0016 rule 2).
std::vector<Getter> getters() {
    return {
        {"SiderealTime", [](auto& d) { (void)d.get_sidereal_time(); }},
        {"SiteLatitude", [](auto& d) { (void)d.get_site_latitude(); }},
        {"SiteLongitude", [](auto& d) { (void)d.get_site_longitude(); }},
        {"UTCDate", [](auto& d) { (void)d.get_utc_date(); }},
        {"Connected", [](auto& d) { (void)d.get_connected(); }},
        {"Slewing", [](auto& d) { (void)d.get_slewing(); }},
        {"Tracking", [](auto& d) { (void)d.get_tracking(); }},
        {"TrackingRate", [](auto& d) { (void)d.get_tracking_rate(); }},
        {"AtPark", [](auto& d) { (void)d.get_at_park(); }},
        {"RightAscension", [](auto& d) { (void)d.get_right_ascension(); }},
        {"IsPulseGuiding", [](auto& d) { (void)d.get_is_pulse_guiding(); }},
    };
}

// Reads every getter in rounds for `window`, each read on its own thread, and
// returns the slowest read of each. One thread per getter keeps a getter that
// is stuck behind the state lock from hiding the ones queued after it.
std::vector<std::pair<std::string, milliseconds>> slowest_reads(alpacacore::TelescopeDriver& driver,
                                                                std::chrono::milliseconds window) {
    const auto list = getters();
    std::vector<std::pair<std::string, milliseconds>> worst;
    for (const auto& g : list) {
        worst.emplace_back(g.name, milliseconds(0));
    }
    const auto end = steady_clock::now() + window;
    while (steady_clock::now() < end) {
        std::vector<milliseconds> spent(list.size(), milliseconds(0));
        std::vector<std::thread> readers;
        for (std::size_t i = 0; i < list.size(); ++i) {
            readers.emplace_back([&, i] {
                const auto start = steady_clock::now();
                try {
                    list[i].read(driver);
                } catch (const alpacacore::AlpacaException&) {  // NOLINT(bugprone-empty-catch): only the wait is timed
                }
                spent[i] = std::chrono::duration_cast<milliseconds>(steady_clock::now() - start);
            });
        }
        for (auto& t : readers) {
            t.join();
        }
        for (std::size_t i = 0; i < list.size(); ++i) {
            worst[i].second = std::max(worst[i].second, spent[i]);
        }
        std::this_thread::sleep_for(milliseconds(5));
    }
    return worst;
}

struct Command {
    const char* name;
    std::function<void(alpacacore::TelescopeDriver&)> run;
};

// Commands that reach the wire with the driver's state lock held on main.
std::vector<Command> commands() {
    return {
        // set_park() reads both axes under the lock (line 2134 on d907c3f).
        {"SetPark", [](auto& d) { d.set_park(); }},
        {"Park", [](auto& d) { d.park(); }},
        {"FindHome", [](auto& d) { d.find_home(); }},
        {"SlewToCoordinatesAsync", [](auto& d) { d.slew_to_coordinates_async(2.0, 20.0); }},
        {"SyncToCoordinates", [](auto& d) { d.sync_to_coordinates(2.0, 20.0); }},
        {"SetTracking", [](auto& d) { d.set_tracking(true); }},
        {"Unpark", [](auto& d) { d.unpark(); }},
    };
}

}  // namespace

TEST_CASE("SkyWatcher getters - none waits behind a command that is on the wire", "[skywatcher][getter-locks]") {
    for (const auto& command : commands()) {
        CAPTURE(command.name);
        FakeSkyWatcherMount mount;
        REQUIRE(mount.ok());
        auto driver = connected_driver(mount);
        REQUIRE(driver->get_connected());

        mount.set_reply_latency(kLatency);
        const int before = mount.transactions_served();
        std::atomic<bool> done{false};
        std::thread issuer([&] {
            try {
                command.run(*driver);
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): only the getters are under test
            }
            done = true;
        });
        // The command is on the wire once the fake has seen its first frame.
        for (int i = 0; i < 200 && mount.transactions_served() == before; ++i) {
            std::this_thread::sleep_for(milliseconds(5));
        }
        REQUIRE(mount.transactions_served() > before);

        const auto worst = slowest_reads(*driver, kSampleWindow);
        for (const auto& [getter, spent] : worst) {
            CAPTURE(getter);
            CHECK(spent < kGetterBudget);
        }
        // The window must have overlapped the command, or a pass proves nothing:
        // the command's own exchanges are still being answered after it.
        CHECK(mount.transactions_served() > before + 1);

        mount.set_reply_latency(milliseconds(0));
        issuer.join();
        driver->set_connected(false);
    }
}

TEST_CASE("SkyWatcher getters - a connect on the wire holds up no getter and reads as not connected",
          "[skywatcher][getter-locks]") {
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), kSiteLatitude, kSiteLongitude, kSiteElevation);

    mount.set_reply_latency(kLatency);
    const int before = mount.transactions_served();
    std::atomic<bool> done{false};
    std::thread connector([&] {
        driver->set_connected(true);
        done = true;
    });
    for (int i = 0; i < 200 && mount.transactions_served() == before; ++i) {
        std::this_thread::sleep_for(milliseconds(5));
    }
    REQUIRE(mount.transactions_served() > before);

    // The handshake is several replies long: sample while it is still on the wire.
    int sampled = 0;
    while (!done.load() && sampled < 20) {
        const auto start = steady_clock::now();
        CHECK_FALSE(driver->get_connected());
        CHECK_THROWS_AS(driver->get_right_ascension(), alpacacore::AlpacaException);
        CHECK_THROWS_AS(driver->get_slewing(), alpacacore::AlpacaException);
        CHECK(driver->get_site_latitude() == kSiteLatitude);
        CHECK(driver->get_site_longitude() == kSiteLongitude);
        CHECK(std::chrono::duration_cast<milliseconds>(steady_clock::now() - start) < kGetterBudget);
        ++sampled;
        std::this_thread::sleep_for(milliseconds(20));
    }
    CHECK(sampled > 0);

    mount.set_reply_latency(milliseconds(0));
    connector.join();
    CHECK(driver->get_connected());
    CHECK_NOTHROW(driver->get_right_ascension());
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher getters - SiderealTime and UTCDate read one client-clock state while it is written",
          "[skywatcher][getter-locks][stress-guard]") {
    struct ProbeGuard {
        ProbeGuard() {
            sw::detail::set_host_synchronized_probe([] { return false; });
        }
        ~ProbeGuard() { sw::detail::set_host_synchronized_probe(nullptr); }
    } probe_guard;

    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);

    // Two writers' worth of states: the UTCDate offset is 0 or +1 h, the
    // longitude 151.215297 or -0.0015 degrees. A reader may see any pairing of
    // one offset with one longitude (the two setters are separate calls), and
    // nothing else.
    constexpr double kOtherLongitude = -0.001500;
    constexpr auto kOffset = std::chrono::hours(1);
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        bool flip = false;
        while (!stop.load()) {
            flip = !flip;
            driver->set_site_longitude(flip ? kOtherLongitude : kSiteLongitude);
            driver->set_utc_date(std::chrono::system_clock::now() + (flip ? kOffset : std::chrono::hours(0)));
        }
    });

    const auto end = steady_clock::now() + milliseconds(1500);
    int reads = 0;
    int bad = 0;
    while (steady_clock::now() < end) {
        const auto before = std::chrono::system_clock::now();
        const auto utc = driver->get_utc_date();
        const auto after = std::chrono::system_clock::now();
        // UTCDate is the host clock plus the offset in force: 0 or +1 h.
        const auto slack = std::chrono::seconds(2);
        const bool plain = utc >= before - slack && utc <= after + slack;
        const bool shifted = utc >= before + kOffset - slack && utc <= after + kOffset + slack;
        if (!plain && !shifted) {
            ++bad;
        }
        const double lst = driver->get_sidereal_time();
        if (!std::isfinite(lst) || lst < 0.0 || lst >= 24.0) {
            ++bad;
        }
        ++reads;
    }
    stop = true;
    writer.join();
    CHECK(reads > 0);
    CHECK(bad == 0);
    driver->set_connected(false);
}

#endif  // !_WIN32
