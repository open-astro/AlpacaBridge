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

// OnStep getters never wait on a serial exchange (decision 0016, issue #1449).
// The driver talks to a loopback FakeMountServer that holds every reply for
// kLatency, so a getter that waits for the driver's state lock while a command
// or a Park/FindHome body is on the wire pays at least one held reply. The
// driver runs on a FakeTaskClock that the cases never advance (except the
// MoveAxis re-send case), so its 2 s status and position caches stay fresh:
// every getter here answers without asking the mount. A getter whose cache is
// stale still asks, and waits for the wire, as decision 0016 allows.
// Decision 0005: the budget is wall time spent in a getter, not a driver
// timeout; no case waits on a task-clock deadline.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/onstep/onstep_protocol_wrapper.h>
#include <alpacacore/vendor/onstep/onstep_telescope_driver.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"

namespace onstep = alpacacore::vendor::onstep;

namespace {

using std::chrono::milliseconds;
using std::chrono::steady_clock;

// One held reply. A command that runs a few exchanges keeps the wire for
// several of these.
constexpr auto kLatency = milliseconds(300);
// Far under one held reply: a getter that waited for even one would miss it.
constexpr auto kGetterBudget = milliseconds(100);

// Made-up site, Sydney Opera House.
constexpr double kSiteLatitude = -33.856784;
constexpr double kSiteLongitude = 151.215297;

// An OnStepX that answers what the driver asks, holding each reply for
// `latency_ms`. Blind commands (no reply) are answered at once, so the driver's
// drain window never sees a late byte. While `moving`, :GU# shows a guide move
// ('g') until the first stop arrives.
struct HeldOnStep {
    std::atomic<int> latency_ms{0};
    std::atomic<bool> moving{false};
    std::mutex mutex;
    std::vector<std::string> log;
    std::string status;  // non-empty: :GU# answers this (under `mutex`)

    void set_status(const std::string& s) {
        std::lock_guard<std::mutex> lock(mutex);
        status = s;
    }

    std::size_t count(const std::string& needle) {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<std::size_t>(std::count_if(
            log.begin(), log.end(), [&](const std::string& c) { return c.find(needle) != std::string::npos; }));
    }
    std::size_t served() {
        std::lock_guard<std::mutex> lock(mutex);
        return log.size();
    }

    std::string answer(const std::string& chunk) {
        if (chunk.rfind(":GVP", 0) == 0) return "OnStep#";
        if (chunk.rfind(":GVN", 0) == 0) return "10.23a#";
        if (chunk.rfind(":GXEE", 0) == 0) return "1";
        if (chunk.rfind(":GU", 0) == 0) {
            std::lock_guard<std::mutex> lock(mutex);
            if (!status.empty()) return status;
            return moving.load() ? "NpgEW180#" : "NpEW180#";  // tracking, no GOTO
        }
        if (chunk.rfind(":GRa", 0) == 0) return "12:00:00.0000#";
        if (chunk.rfind(":GDe", 0) == 0) return "+45*00:00.000#";
        if (chunk.rfind(":GA", 0) == 0) return "+45*00:00#";
        if (chunk.rfind(":GZ", 0) == 0) return "180*00:00#";
        if (chunk.rfind(":Gh", 0) == 0) return "-10*#";
        if (chunk.rfind(":Go", 0) == 0) return "90*#";
        if (chunk.rfind(":GE", 0) == 0) return "00#";
        if (chunk.rfind(":CM", 0) == 0) return "N/A#";
        if (chunk.rfind(":MS", 0) == 0) return "0";  // GOTO accepted (bare digit)
        if (chunk.rfind(":Q", 0) == 0) {
            moving = false;
            return "";
        }
        if (chunk.rfind(":M", 0) == 0 || chunk.rfind(":R", 0) == 0 || chunk.rfind(":hC", 0) == 0) return "";
        if (chunk.rfind(":S", 0) == 0 || chunk.rfind(":h", 0) == 0) return "1";  // :ST, :Sr, :hP, :hQ, :hR ...
        return "0#";
    }

    std::string reply(const std::string& chunk) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            log.push_back(chunk);
        }
        std::string r = answer(chunk);
        if (!r.empty() && latency_ms.load() > 0) {
            std::this_thread::sleep_for(milliseconds(latency_ms.load()));
        }
        return r;
    }
};

struct Rig {
    std::shared_ptr<HeldOnStep> mount = std::make_shared<HeldOnStep>();
    alpacacore::test::FakeTaskClock clock;
    alpacacore::test::FakeMountServer server{[m = mount](const std::string& chunk) { return m->reply(chunk); }};
    std::unique_ptr<alpacacore::TelescopeDriver> driver;

    explicit Rig(bool moving = false) {
        mount->moving = moving;
        driver = onstep::create_onstep_telescope_with_site(0, endpoint(), kSiteLatitude, kSiteLongitude, std::nullopt,
                                                           true, clock);
    }
    ~Rig() {
        mount->latency_ms = 0;
        try {
            driver->set_connected(false);
        } catch (...) {  // NOLINT(bugprone-empty-catch): best-effort teardown
        }
        driver.reset();
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    onstep::ConnectionInfo endpoint() const {
        onstep::ConnectionInfo info;
        info.type = onstep::ConnectionType::Network;
        info.host = "127.0.0.1";
        info.tcp_port = server.port();
        info.response_timeout_ms = 3000;
        return info;
    }
};

struct Getter {
    const char* name;
    std::function<void(alpacacore::TelescopeDriver&)> read;
};

// Members that never ask the mount once connected with a known site and clock.
std::vector<Getter> stateless_getters() {
    return {
        {"Connected", [](auto& d) { (void)d.get_connected(); }},
        {"SiderealTime", [](auto& d) { (void)d.get_sidereal_time(); }},
        {"SiteLatitude", [](auto& d) { (void)d.get_site_latitude(); }},
        {"SiteLongitude", [](auto& d) { (void)d.get_site_longitude(); }},
        {"UTCDate", [](auto& d) { (void)d.get_utc_date(); }},
        {"IsPulseGuiding", [](auto& d) { (void)d.get_is_pulse_guiding(); }},
        {"DoesRefraction", [](auto& d) { (void)d.get_does_refraction(); }},
        {"DestinationSideOfPier", [](auto& d) { (void)d.get_destination_side_of_pier(2.0, 20.0); }},
        {"TargetRightAscension", [](auto& d) { (void)d.get_target_right_ascension(); }},
        {"TargetDeclination", [](auto& d) { (void)d.get_target_declination(); }},
        {"TrackingRate", [](auto& d) { (void)d.get_tracking_rate(); }},
    };
}

// Members served from the 2 s status and position caches while they are fresh.
std::vector<Getter> all_getters() {
    auto list = stateless_getters();
    const std::vector<Getter> cached = {
        {"Slewing", [](auto& d) { (void)d.get_slewing(); }},
        {"Tracking", [](auto& d) { (void)d.get_tracking(); }},
        {"AtPark", [](auto& d) { (void)d.get_at_park(); }},
        {"AtHome", [](auto& d) { (void)d.get_at_home(); }},
        {"RightAscension", [](auto& d) { (void)d.get_right_ascension(); }},
        {"Declination", [](auto& d) { (void)d.get_declination(); }},
        {"Altitude", [](auto& d) { (void)d.get_altitude(); }},
        {"Azimuth", [](auto& d) { (void)d.get_azimuth(); }},
        {"SideOfPier", [](auto& d) { (void)d.get_side_of_pier(); }},
        {"DeviceState", [](auto& d) { (void)d.get_device_state(); }},
    };
    list.insert(list.end(), cached.begin(), cached.end());
    return list;
}

// Reads every getter in rounds while `in_flight()` holds, each read on its own
// thread, and returns the slowest read of each. One thread per getter keeps a
// getter stuck behind the state lock from hiding the ones queued after it. A
// round counts only when the command was still in flight when it ended: once
// it is over, a cache it invalidated is refilled with a held reply, which is a
// live read, not a wait behind another member's exchange.
struct Sampled {
    std::vector<std::pair<std::string, milliseconds>> worst;
    int rounds = 0;
};

Sampled slowest_reads(alpacacore::TelescopeDriver& driver, const std::vector<Getter>& list,
                      const std::function<bool()>& in_flight) {
    Sampled result;
    for (const auto& g : list) {
        result.worst.emplace_back(g.name, milliseconds(0));
    }
    const auto give_up = steady_clock::now() + std::chrono::seconds(20);
    while (in_flight() && steady_clock::now() < give_up) {
        std::vector<milliseconds> spent(list.size(), milliseconds(0));
        std::vector<std::thread> readers;
        readers.reserve(list.size());
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
        if (!in_flight()) {
            break;
        }
        ++result.rounds;
        for (std::size_t i = 0; i < list.size(); ++i) {
            result.worst[i].second = std::max(result.worst[i].second, spent[i]);
        }
        std::this_thread::sleep_for(milliseconds(5));
    }
    return result;
}

void check_no_getter_waited(const Sampled& sampled) {
    CHECK(sampled.rounds > 0);  // the window overlapped the exchanges, or a pass proves nothing
    for (const auto& [getter, spent] : sampled.worst) {
        CAPTURE(getter);
        CHECK(spent < kGetterBudget);
    }
}

// Blocks until the fake has seen a command past `before`, the sign that the
// member under test is on the wire.
void wait_for_exchange(HeldOnStep& mount, std::size_t before) {
    for (int i = 0; i < 400 && mount.served() <= before; ++i) {
        std::this_thread::sleep_for(milliseconds(5));
    }
    REQUIRE(mount.served() > before);
}

struct Command {
    const char* name;
    std::function<void(alpacacore::TelescopeDriver&)> run;
};

// Members that reach the wire with a held reply, each with the driver's state
// lock held across it on 26d7e839.
std::vector<Command> commands() {
    return {
        {"SetPark", [](auto& d) { d.set_park(); }},
        {"Park", [](auto& d) { d.park(); }},
        {"FindHome", [](auto& d) { d.find_home(); }},
        {"Unpark", [](auto& d) { d.unpark(); }},
        {"SetTracking", [](auto& d) { d.set_tracking(true); }},
        {"SlewToCoordinatesAsync", [](auto& d) { d.slew_to_coordinates_async(2.0, 20.0); }},
        {"SyncToCoordinates", [](auto& d) { d.sync_to_coordinates(2.0, 20.0); }},
        {"UTCDate write", [](auto& d) { d.set_utc_date(std::chrono::system_clock::now()); }},
        {"SiteLatitude write", [](auto& d) { d.set_site_latitude(kSiteLatitude); }},
        // Five blind stops, each drained for 50 ms: no held reply, but 250 ms on the wire.
        {"AbortSlew", [](auto& d) { d.abort_slew(); }},
    };
}

}  // namespace

TEST_CASE("OnStep getters - none waits behind a command that is on the wire", "[onstep][getter-locks]") {
    for (const auto& command : commands()) {
        CAPTURE(command.name);
        Rig rig;
        REQUIRE(rig.server.ok());
        rig.driver->set_connected(true);
        REQUIRE(rig.driver->get_connected());
        rig.driver->set_target_right_ascension(2.0);
        rig.driver->set_target_declination(20.0);

        rig.mount->latency_ms = static_cast<int>(kLatency.count());
        const std::size_t before = rig.mount->served();
        std::atomic<bool> done{false};
        std::thread issuer([&] {
            try {
                command.run(*rig.driver);
            } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): only the getters are under test
            }
            done = true;
        });
        wait_for_exchange(*rig.mount, before);
        const auto sampled = slowest_reads(*rig.driver, all_getters(), [&] { return !done.load(); });
        rig.mount->latency_ms = 0;
        issuer.join();
        check_no_getter_waited(sampled);
    }
}

// Park and FindHome that first stop a running motion finish in the park_home_
// body (#1430): its :GU# settle poll and the :hP#, or the :hC# and :GE#, run
// there after the call has returned.
TEST_CASE("OnStep getters - none waits behind a Park or FindHome body on the wire", "[onstep][getter-locks]") {
    for (const bool home : {false, true}) {
        CAPTURE(home);
        Rig rig(true);
        REQUIRE(rig.server.ok());
        rig.driver->set_connected(true);
        REQUIRE(rig.driver->get_connected());

        rig.mount->latency_ms = static_cast<int>(kLatency.count());
        const char* const last = home ? ":GE" : ":hP";
        // The call reads :GU# once, sees the move and sends the stops; the body's first :GU# is the next.
        const std::size_t polls = rig.mount->count(":GU");
        if (home) {
            rig.driver->find_home();
        } else {
            rig.driver->park();
        }
        // The call has sent the stops and returned; the body is on the wire once it polls.
        for (int i = 0; i < 400 && rig.mount->count(":GU") < polls + 2; ++i) {
            std::this_thread::sleep_for(milliseconds(5));
        }
        REQUIRE(rig.mount->count(":GU") >= polls + 2);
        REQUIRE(rig.mount->count(":Q") > 0);
        const auto sampled = slowest_reads(*rig.driver, all_getters(), [&] { return rig.mount->count(last) == 0; });
        rig.mount->latency_ms = 0;
        check_no_getter_waited(sampled);
        CHECK(rig.mount->count(last) == 1);  // the body went on to the park or home
    }
}

// The #1378 re-send tick reads :GU#, the altitude and its limits, then
// re-sends the move (#1449 names it). The case advances the clock past the
// re-send interval, so the position caches are stale and their getters ask
// the mount; only the members that never ask are timed.
TEST_CASE("OnStep getters - none waits behind the MoveAxis re-send tick", "[onstep][getter-locks]") {
    Rig rig;
    REQUIRE(rig.server.ok());
    rig.driver->set_connected(true);
    REQUIRE(rig.driver->get_connected());
    rig.mount->moving = true;  // the mount shows the move ('g'), so the tick re-sends it

    const auto waits = rig.clock.wait_count();
    rig.driver->move_axis(0, 1.0);
    REQUIRE(rig.clock.wait_for_wait_count(waits + 1, std::chrono::seconds(5)));
    const std::size_t moves = rig.mount->count(":Me#");
    rig.mount->latency_ms = static_cast<int>(kLatency.count());
    const std::size_t before = rig.mount->served();
    rig.clock.advance(std::chrono::seconds(7));
    wait_for_exchange(*rig.mount, before);
    const auto sampled =
        slowest_reads(*rig.driver, stateless_getters(), [&] { return rig.mount->count(":Me#") == moves; });
    rig.mount->latency_ms = 0;
    check_no_getter_waited(sampled);
    CHECK(rig.mount->count(":Me#") == moves + 1);  // the tick re-sent the move
    rig.mount->moving = false;
    rig.driver->move_axis(0, 0.0);
}

// A getter whose cache is stale asks the mount, and that one read takes the
// operation lock: it never lands inside a command's sequence. :GE# answers for
// the command right before it, so a :GU# between :hC# and :GE# would make the
// home read the status read's code.
TEST_CASE("OnStep getters - a stale-cache read never lands between a command's exchanges", "[onstep][getter-locks]") {
    Rig rig;
    REQUIRE(rig.server.ok());
    rig.driver->set_connected(true);
    REQUIRE(rig.driver->get_connected());
    rig.mount->latency_ms = 20;
    for (int i = 0; i < 5; ++i) {
        rig.clock.advance(std::chrono::seconds(3));  // past the 2 s caches: every read below asks the mount
        std::atomic<bool> done{false};
        std::vector<std::thread> readers;
        for (int r = 0; r < 3; ++r) {
            readers.emplace_back([&] {
                while (!done.load()) {
                    try {
                        (void)rig.driver->get_right_ascension();
                        (void)rig.driver->get_tracking();
                        (void)rig.driver->get_altitude();
                    } catch (const alpacacore::AlpacaException&) {  // NOLINT(bugprone-empty-catch): order only
                    }
                }
            });
        }
        try {
            rig.driver->find_home();
        } catch (const alpacacore::AlpacaException&) {  // NOLINT(bugprone-empty-catch): order only
        }
        done = true;
        for (auto& t : readers) {
            t.join();
        }
    }
    rig.mount->latency_ms = 0;
    std::vector<std::string> log;
    {
        std::lock_guard<std::mutex> lock(rig.mount->mutex);
        log = rig.mount->log;
    }
    int homes = 0;
    for (std::size_t i = 0; i < log.size(); ++i) {
        if (log[i].rfind(":hC", 0) == 0) {
            ++homes;
            REQUIRE(i + 1 < log.size());
            CAPTURE(log[i + 1]);
            CHECK(log[i + 1].rfind(":GE", 0) == 0);
        }
    }
    CHECK(homes == 5);
}

// A Slewing read whose status cache is stale drops mutex_ for its :GU#. A slew
// sent by another client in that gap is not "the slew that just ended": the
// read must not clear its tracking-restore flag or restart tracking during it.
TEST_CASE("OnStep getters - a slew sent during a Slewing read's status wait is not taken as ended",
          "[onstep][getter-locks]") {
    Rig rig;
    REQUIRE(rig.server.ok());
    rig.driver->set_connected(true);
    REQUIRE(rig.driver->get_connected());
    rig.mount->set_status("pEW180#");  // a GOTO runs, tracking on
    rig.clock.advance(std::chrono::milliseconds(2100));
    REQUIRE(rig.driver->get_slewing());  // the driver has seen it running
    rig.mount->set_status("NpEW180#");   // it ended
    rig.clock.advance(std::chrono::milliseconds(2100));

    onstep::set_single_read_before_io_hook_for_testing(*rig.driver, [&] {
        // A second client's GOTO lands between this read's unlock and its :GU#.
        std::thread other([&] { rig.driver->slew_to_coordinates_async(2.0, 20.0); });
        other.join();
        rig.mount->set_status("nNpEW180#");  // the new GOTO not shown yet, tracking reads off
    });
    const bool slewing = rig.driver->get_slewing();
    onstep::set_single_read_before_io_hook_for_testing(*rig.driver, nullptr);

    REQUIRE(rig.mount->count(":MS#") == 1);
    CHECK(slewing);  // the new slew's grace
    // No tracking restore for the slew that "ended" (:ST60.164275#).
    CHECK(rig.mount->count(":ST") == 0);
}

// A getter whose cache is stale waits for the operation lock. A Disconnect and
// Connect that win that race open a new session; the getter must not read it,
// but answer NotConnected as check_connected() would, without touching the wire.
TEST_CASE("OnStep getters - a stale-cache read that waited out a reconnect reads nothing", "[onstep][getter-locks]") {
    Rig rig;
    REQUIRE(rig.server.ok());
    rig.driver->set_connected(true);
    REQUIRE(rig.driver->get_connected());
    rig.clock.advance(std::chrono::milliseconds(2100));  // past the position cache

    std::size_t served_after_reconnect = 0;
    onstep::set_single_read_before_io_hook_for_testing(*rig.driver, [&] {
        std::thread other([&] {
            rig.driver->set_connected(false);
            rig.driver->set_connected(true);
        });
        other.join();
        served_after_reconnect = rig.mount->served();
    });
    int error = 0;
    try {
        (void)rig.driver->get_right_ascension();
    } catch (const alpacacore::AlpacaException& e) {
        error = e.error_code();
    }
    onstep::set_single_read_before_io_hook_for_testing(*rig.driver, nullptr);

    REQUIRE(served_after_reconnect > 0);
    CHECK(error == alpacacore::AlpacaError::NotConnected);
    CHECK(rig.mount->served() == served_after_reconnect);  // nothing sent for the stale read
    CHECK(rig.driver->get_connected());                    // the new session stands
}

// Decision 0006 rule 3 (#1728 review): the re-send tick drops mutex_ for its
// :GU#, altitude and limit reads, so a cancel or a cleared re-send flag can land
// before the re-send. The seam ends the move in that gap; the tick must re-check
// and send nothing.
TEST_CASE("OnStep getters - a MoveAxis ended between the re-send tick's reads and its re-send is not re-sent",
          "[onstep][getter-locks]") {
    Rig rig;
    REQUIRE(rig.server.ok());
    rig.driver->set_connected(true);
    REQUIRE(rig.driver->get_connected());
    rig.mount->moving = true;  // the mount shows the move ('g'), so the tick would re-send it

    const auto waits = rig.clock.wait_count();
    rig.driver->move_axis(0, 1.0);
    REQUIRE(rig.clock.wait_for_wait_count(waits + 1, std::chrono::seconds(5)));
    const std::size_t moves = rig.mount->count(":Me#");
    std::atomic<bool> hook_ran{false};
    onstep::set_move_axis_resend_before_send_hook_for_testing(*rig.driver, [&] {
        hook_ran = true;
        return true;
    });
    rig.clock.advance(std::chrono::seconds(7));
    REQUIRE(rig.clock.wait_for_woken_settled(std::chrono::seconds(5)));
    onstep::set_move_axis_resend_before_send_hook_for_testing(*rig.driver, nullptr);

    REQUIRE(hook_ran);
    REQUIRE(rig.mount->count(":Go#") >= 1);    // the tick got through its reads
    CHECK(rig.mount->count(":Me#") == moves);  // and re-sent nothing
    rig.mount->moving = false;
}

TEST_CASE("OnStep getters - a connect on the wire holds up no getter and reads as not connected",
          "[onstep][getter-locks]") {
    Rig rig;
    REQUIRE(rig.server.ok());
    rig.mount->latency_ms = static_cast<int>(kLatency.count());
    const std::size_t before = rig.mount->served();
    std::atomic<bool> done{false};
    std::thread connector([&] {
        rig.driver->set_connected(true);
        done = true;
    });
    wait_for_exchange(*rig.mount, before);

    // The handshake is many held replies long: sample while it is still on the wire.
    int sampled = 0;
    while (!done.load() && sampled < 20) {
        const auto start = steady_clock::now();
        CHECK_FALSE(rig.driver->get_connected());
        CHECK_THROWS_AS(rig.driver->get_right_ascension(), alpacacore::AlpacaException);
        CHECK_THROWS_AS(rig.driver->get_slewing(), alpacacore::AlpacaException);
        CHECK_THROWS_AS(rig.driver->get_site_latitude(), alpacacore::AlpacaException);
        CHECK(rig.driver->get_device_state().empty());
        CHECK(std::chrono::duration_cast<milliseconds>(steady_clock::now() - start) < kGetterBudget);
        ++sampled;
        std::this_thread::sleep_for(milliseconds(20));
    }
    CHECK(sampled > 0);

    rig.mount->latency_ms = 0;
    connector.join();
    CHECK(rig.driver->get_connected());
    CHECK_NOTHROW(rig.driver->get_right_ascension());
}

TEST_CASE("OnStep getters - a getter during a clean disconnect's close reads as not connected",
          "[onstep][getter-locks]") {
    // Decision 0016, item 4: a disconnect clears Connected under the state lock before the close, which runs
    // with that lock released. A getter on another thread in that window answers NotConnected; it never serves
    // the session that is being closed.
    Rig rig;
    REQUIRE(rig.server.ok());
    rig.driver->set_connected(true);
    REQUIRE(rig.driver->get_connected());
    REQUIRE_NOTHROW(rig.driver->get_right_ascension());

    bool hook_ran = false;
    bool connected_in_close = true;
    int ra_error = 0;
    int slewing_error = 0;
    auto error_of = [](const std::function<void()>& read) {
        try {
            read();
        } catch (const alpacacore::AlpacaException& e) {
            return e.error_code();
        }
        return 0;
    };
    onstep::set_disconnect_before_close_hook_for_testing(*rig.driver, [&] {
        hook_ran = true;
        std::thread reader([&] {
            connected_in_close = rig.driver->get_connected();
            ra_error = error_of([&] { (void)rig.driver->get_right_ascension(); });
            slewing_error = error_of([&] { (void)rig.driver->get_slewing(); });
        });
        reader.join();
    });
    rig.driver->set_connected(false);
    onstep::set_disconnect_before_close_hook_for_testing(*rig.driver, nullptr);

    REQUIRE(hook_ran);
    CHECK_FALSE(connected_in_close);
    CHECK(ra_error == alpacacore::AlpacaError::NotConnected);
    CHECK(slewing_error == alpacacore::AlpacaError::NotConnected);
    CHECK_FALSE(rig.driver->get_connected());
}

#endif  // !_WIN32
