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
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "fake_skywatcher_mount.h"
#include "fake_task_clock.h"

namespace sw = alpacacore::vendor::skywatcher;
using alpacacore::test::FakeMountProfile;
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

TEST_CASE("SkyWatcher getters - a getter during a clean disconnect's close reads as not connected",
          "[skywatcher][getter-locks]") {
    // Decision 0016, item 4: a disconnect clears Connected under the state lock before the close, which runs with
    // that lock released. A getter on another thread in that window answers NotConnected; it never serves the
    // session that is being closed.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    REQUIRE(driver->get_connected());
    REQUIRE_NOTHROW(driver->get_right_ascension());

    bool hook_ran = false;
    bool connected_in_close = true;
    int ra_error = 0;
    int slewing_error = 0;
    auto error_of = [&](const std::function<void()>& read) {
        try {
            read();
        } catch (const alpacacore::AlpacaException& e) {
            return e.error_code();
        }
        return 0;
    };
    sw::set_disconnect_before_close_hook_for_testing(*driver, [&] {
        hook_ran = true;
        std::thread reader([&] {
            connected_in_close = driver->get_connected();
            ra_error = error_of([&] { (void)driver->get_right_ascension(); });
            slewing_error = error_of([&] { (void)driver->get_slewing(); });
        });
        reader.join();
    });
    driver->set_connected(false);
    sw::set_disconnect_before_close_hook_for_testing(*driver, nullptr);

    REQUIRE(hook_ran);
    CHECK_FALSE(connected_in_close);
    CHECK(ra_error == alpacacore::AlpacaError::NotConnected);
    CHECK(slewing_error == alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(driver->get_connected());
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
    // nothing else. The two setters are separate calls, so no value check can
    // tell a torn snapshot from a legal pairing: the case guards the
    // client_clock_mutex_ locking, and only the TSan run (`[stress-guard]`)
    // fails when a reader drops that lock.
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

TEST_CASE("SkyWatcher getters - a reconnect does not race the rate-applied check reading the axis parameters",
          "[skywatcher][getter-locks][stress-guard]") {
    // A rate change starts its background rate-applied check after releasing the driver locks. In that gap a
    // disconnect joins an empty slot, and the next connect rewrites the axis parameters under the state lock while
    // the newly started body reads them. The values are equal on every connect, so no assertion can see the race:
    // only the TSan run (`[stress-guard]`) fails when the check reads axis_params_ without the state lock. The
    // hook runs the disconnect and the reconnect on a second thread; the setter waits for the disconnect only, so
    // the reconnect write and the body read have no ordering.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    auto driver = connected_driver(mount);
    std::vector<std::thread> reconnects;
    for (int i = 0; i < 15; ++i) {
        driver->set_tracking(true);
        std::atomic<bool> disconnected{false};
        sw::set_rate_verify_before_start_hook_for_testing(*driver, [&] {
            reconnects.emplace_back([&] {
                driver->set_connected(false);
                disconnected = true;
                driver->set_connected(true);
            });
            while (!disconnected) {
                std::this_thread::yield();
            }
        });
        try {
            driver->set_right_ascension_rate(i % 2 == 0 ? 0.5 : -0.5);
        } catch (const alpacacore::AlpacaException&) {  // NOLINT(bugprone-empty-catch)
            // A rate change refused mid-transition is not the subject here.
        }
        sw::set_rate_verify_before_start_hook_for_testing(*driver, nullptr);
        for (auto& t : reconnects) {
            t.join();
        }
        reconnects.clear();
        if (!driver->get_connected()) {
            driver->set_connected(true);
        }
    }
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher getters - an auto-detect scan on the connect holds up no state getter",
          "[skywatcher][getter-locks]") {
    // The resolver of an auto-detect mount scans serial ports and probes UDP hosts. It runs inside the connect,
    // so the state lock must be free while it runs: Slewing, Tracking and AtPark read it and must answer
    // NotConnected at once instead of waiting out the scan.
    FakeSkyWatcherMount mount;
    REQUIRE(mount.ok());
    const sw::ConnectionInfo info = endpoint(mount);
    std::atomic<bool> scanning{false};
    auto driver = sw::create_skywatcher_telescope_deferred(
        0,
        [&]() {
            scanning = true;
            std::this_thread::sleep_for(milliseconds(1000));
            return info;
        },
        kSiteLatitude, kSiteLongitude, kSiteElevation);
    std::thread connecting([&] { driver->set_connected(true); });
    while (!scanning) {
        std::this_thread::yield();
    }
    const std::vector<Getter> state_getters = {
        {"Slewing", [](auto& d) { (void)d.get_slewing(); }},
        {"Tracking", [](auto& d) { (void)d.get_tracking(); }},
        {"AtPark", [](auto& d) { (void)d.get_at_park(); }},
    };
    for (const auto& g : state_getters) {
        const auto start = steady_clock::now();
        CHECK_THROWS_AS(g.read(*driver), alpacacore::AlpacaException);
        const auto spent = std::chrono::duration_cast<milliseconds>(steady_clock::now() - start);
        INFO(g.name << " took " << spent.count() << " ms during the scan");
        CHECK(spent < kGetterBudget);
    }
    connecting.join();
    CHECK(driver->get_connected());
    driver->set_connected(false);
}

namespace {
// Reports the link as lost an hour ago on the driver's task clock, so a reconnect takes the stop branch.
class LongOutageWrapper : public sw::SkyWatcherProtocolWrapper {
public:
    explicit LongOutageWrapper(const alpacacore::test::FakeTaskClock& clock) : clock_(clock) {}
    std::optional<std::chrono::steady_clock::time_point> consume_link_lost_at() override {
        return clock_.now() - std::chrono::hours(1);
    }

private:
    const alpacacore::test::FakeTaskClock& clock_;
};
}  // namespace

TEST_CASE("SkyWatcher getters - the connect's stop-confirm wait holds up no state getter",
          "[skywatcher][getter-locks]") {
    // A reconnect after a long outage finds RA still running, stops it and sleeps 50 ms on the task clock between
    // polls of the axis. The clock is held, so that sleep lasts 300 ms of wall time: the state lock must be free
    // for all of it, and Slewing, Tracking and AtPark (which read it) answer NotConnected at once.
    alpacacore::test::FakeTaskClock clock;
    FakeSkyWatcherMount mount(FakeMountProfile::wave_100i(), clock);
    REQUIRE(mount.ok());
    auto first = sw::create_skywatcher_telescope(0, endpoint(mount), kSiteLatitude, kSiteLongitude, kSiteElevation, {},
                                                 {}, clock);
    first->set_connected(true);
    first->set_tracking(true);
    for (int i = 0; i < 300 && !mount.axis_running(1); ++i) {
        std::this_thread::sleep_for(milliseconds(10));
    }
    REQUIRE(mount.axis_running(1));
    mount.set_stop_ramp_ms(60000);  // the axis never reports at rest within the connect's budget

    auto second = sw::create_skywatcher_telescope(0, endpoint(mount), kSiteLatitude, kSiteLongitude, kSiteElevation,
                                                  std::make_unique<LongOutageWrapper>(clock), {}, clock);
    std::atomic<bool> done{false};
    std::thread connecting([&] {
        second->set_connected(true);
        done = true;
    });
    // `first`'s status poller is one waiter; the connect's sleep is the second.
    REQUIRE(clock.wait_for_waiters(2, milliseconds(5000)));
    // Release the clock 300 ms from now, so a getter stuck behind the sleeping connect pays that much.
    std::thread ticker([&] {
        std::this_thread::sleep_for(milliseconds(300));
        while (!done) {
            clock.advance(milliseconds(50));
            std::this_thread::sleep_for(milliseconds(2));
        }
    });
    // A client mutator arriving in the gap must wait for the connect: only the state lock is free there, the
    // operation lock is not. Admitted early it would command the board while Connected reads false, and the
    // connect's own rate reset would then overwrite what it set.
    std::atomic<long long> mover_ms{-1};
    const auto mover_start = steady_clock::now();
    std::thread mover([&] {
        try {
            second->move_axis(1, 1.0);
        } catch (const alpacacore::AlpacaException&) {  // NOLINT(bugprone-empty-catch): only the wait is timed
        }
        mover_ms = std::chrono::duration_cast<milliseconds>(steady_clock::now() - mover_start).count();
    });
    const std::vector<Getter> state_getters = {
        {"Slewing", [](auto& d) { (void)d.get_slewing(); }},
        {"Tracking", [](auto& d) { (void)d.get_tracking(); }},
        {"AtPark", [](auto& d) { (void)d.get_at_park(); }},
    };
    for (const auto& g : state_getters) {
        const auto start = steady_clock::now();
        try {
            g.read(*second);
        } catch (const alpacacore::AlpacaException&) {  // NOLINT(bugprone-empty-catch): only the wait is timed
        }
        const auto spent = std::chrono::duration_cast<milliseconds>(steady_clock::now() - start);
        INFO(g.name << " took " << spent.count() << " ms during the stop-confirm sleep");
        CHECK(spent < kGetterBudget);
    }
    mover.join();
    connecting.join();
    ticker.join();
    INFO("MoveAxis returned after " << mover_ms.load() << " ms; the clock was held for 300 ms");
    CHECK(mover_ms.load() >= 250);
    second->set_connected(false);
    first->set_connected(false);
}

#endif  // !_WIN32
