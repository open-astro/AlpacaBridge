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

#include <alpacacore/util/client_utc_warning.h>
#include <alpacacore/util/host_clock.h>
#include <alpacacore/util/logging.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"

using alpacacore::util::HostClock;
using Outcome = HostClock::Outcome;
using namespace std::chrono;

namespace {

struct Fake {
    bool synchronized = false;
    bool set_ok = true;
    bool rtc = false;  // the kernel loaded the clock from a plausible RTC at boot
    std::vector<system_clock::time_point> sets;

    HostClock clock() {
        return HostClock([this] { return synchronized; },
                         [this](system_clock::time_point tp, std::string& err) {
                             sets.push_back(tp);
                             if (!set_ok) {
                                 err = "EPERM";
                             }
                             return set_ok;
                         },
                         [this] { return rtc; });
    }
};

const system_clock::time_point kNow = system_clock::time_point(seconds(1786298276));  // 2026-08

}  // namespace

TEST_CASE("HostClock - undisciplined host takes the client's UTCDate", "[util][hostclock][unit]") {
    Fake f;
    auto c = f.clock();
    CHECK_FALSE(c.synchronized());
    CHECK(c.source() == "none");
    CHECK_FALSE(c.stepped_by_client());

    auto r = c.step_from_client(kNow + minutes(7), kNow);
    REQUIRE(r.outcome == Outcome::Stepped);
    CHECK(r.delta == minutes(7));
    REQUIRE(f.sets.size() == 1);
    CHECK(f.sets[0] == kNow + minutes(7));
    CHECK(c.stepped_by_client());
    CHECK(c.source() == "client");

    // A later, backwards correction is stepped too (the client is the only source).
    r = c.step_from_client(kNow - hours(3), kNow);
    CHECK(r.outcome == Outcome::Stepped);
    CHECK(r.delta == -hours(3));
    CHECK(f.sets.size() == 2);
}

TEST_CASE("HostClock - an NTP-disciplined host is never touched", "[util][hostclock][unit]") {
    Fake f;
    f.synchronized = true;
    auto c = f.clock();
    CHECK(c.synchronized());
    CHECK(c.source() == "ntp");
    auto r = c.step_from_client(kNow + hours(1), kNow);
    CHECK(r.outcome == Outcome::SkippedSynchronized);
    CHECK(r.delta == hours(1));  // the delta is still reported so the caller can log the disagreement
    CHECK(f.sets.empty());
    CHECK_FALSE(c.stepped_by_client());
}

TEST_CASE("HostClock - NTP taking over forgets an earlier client step", "[util][hostclock][unit]") {
    // Field sequence: no NTP, a client steps the clock; later NTP appears
    // (hotspot with internet), then disappears again. The clock is now
    // whatever NTP left plus drift, not the client's value: the state must
    // read "none" again and the connect-time warning must be armed again.
    Fake f;
    auto c = f.clock();
    REQUIRE(c.step_from_client(kNow + minutes(5), kNow).outcome == Outcome::Stepped);
    CHECK(c.source() == "client");
    CHECK(c.stepped_by_client());
    f.synchronized = true;
    CHECK(c.source() == "ntp");
    CHECK_FALSE(c.stepped_by_client());
    f.synchronized = false;
    CHECK(c.source() == "none");
    CHECK_FALSE(c.stepped_by_client());
    // and a fresh client step is accepted again
    CHECK(c.step_from_client(kNow + minutes(5), kNow).outcome == Outcome::Stepped);
    CHECK(c.source() == "client");
}

TEST_CASE("HostClock - opt-out disables the step but keeps the readout", "[util][hostclock][unit]") {
    Fake f;
    auto c = f.clock();
    CHECK(c.enabled());
    c.set_enabled(false);
    CHECK_FALSE(c.enabled());
    auto r = c.step_from_client(kNow + hours(1), kNow);
    CHECK(r.outcome == Outcome::SkippedDisabled);
    CHECK(f.sets.empty());
    CHECK(c.source() == "none");
    c.set_enabled(true);
    CHECK(c.step_from_client(kNow + hours(1), kNow).outcome == Outcome::Stepped);
}

TEST_CASE("HostClock - sanity window and small deltas", "[util][hostclock][unit]") {
    Fake f;
    auto c = f.clock();
    // 1999: below the window (a client with a dead CMOS battery)
    CHECK(c.step_from_client(system_clock::time_point(seconds(915148800)), kNow).outcome == Outcome::SkippedOutOfRange);
    // 2101: above the window
    CHECK(c.step_from_client(system_clock::time_point(seconds(4133980800)), kNow).outcome ==
          Outcome::SkippedOutOfRange);
    // Exactly the bounds are accepted
    CHECK(c.step_from_client(system_clock::time_point(seconds(HostClock::kMinEpoch)), kNow).outcome ==
          Outcome::Stepped);
    CHECK(c.step_from_client(system_clock::time_point(seconds(HostClock::kMaxEpoch)), kNow).outcome ==
          Outcome::Stepped);
    CHECK(f.sets.size() == 2);
    // Sub-second disagreement is HTTP jitter, not clock error
    CHECK(c.step_from_client(kNow + milliseconds(400), kNow).outcome == Outcome::SkippedSmall);
    CHECK(c.step_from_client(kNow - milliseconds(999), kNow).outcome == Outcome::SkippedSmall);
    CHECK(c.step_from_client(kNow + milliseconds(1000), kNow).outcome == Outcome::Stepped);
    CHECK(f.sets.size() == 3);
}

TEST_CASE("HostClock - a refused step latches, so callers know the clock is uncorrectable", "[util][hostclock][unit]") {
    // Without CAP_SYS_TIME every step is refused, so no client will ever fix
    // this clock. enabled() alone cannot tell a caller that (open-astro#292:
    // the connect-time line must stay a WARN in that state).
    Fake f;
    f.rtc = true;
    auto c = f.clock();
    CHECK_FALSE(c.step_ever_failed());
    f.set_ok = false;
    CHECK(c.step_from_client(kNow + seconds(40), kNow).outcome == Outcome::Failed);
    CHECK(c.step_ever_failed());
    CHECK(c.enabled());  // the policy is still on; the kernel is the problem
    // It stays latched even once the kernel starts accepting: the operator
    // needs to know the host was refused at least once this session.
    f.set_ok = true;
    CHECK(c.step_from_client(kNow + seconds(40), kNow).outcome == Outcome::Stepped);
    CHECK(c.step_ever_failed());
    // A skipped step never latches it.
    Fake g;
    auto d = g.clock();
    CHECK(d.step_from_client(kNow + milliseconds(200), kNow).outcome == Outcome::SkippedSmall);
    CHECK_FALSE(d.step_ever_failed());
    // A path that sets the clock itself (the synctime endpoint) reports its own
    // refusal the same way, so an operator who only presses Sync Time on a host
    // without CAP_SYS_TIME still trips the latch.
    CHECK_FALSE(d.stepped_by_client());
    d.mark_step_failed();
    CHECK(d.step_ever_failed());
    CHECK_FALSE(d.stepped_by_client());  // a refusal is not a step
}

TEST_CASE("HostClock - a refused clock_settime is reported, not thrown", "[util][hostclock][unit]") {
    Fake f;
    f.set_ok = false;
    auto c = f.clock();
    auto r = c.step_from_client(kNow + hours(1), kNow);
    CHECK(r.outcome == Outcome::Failed);
    CHECK(r.error == "EPERM");
    CHECK(r.delta == hours(1));
    CHECK_FALSE(c.stepped_by_client());
    CHECK(c.source() == "none");
    CHECK(f.sets.size() == 1);
}

TEST_CASE("HostClock - a hardware RTC is reported as the source, and stepping is unchanged (#292)",
          "[util][hostclock][unit]") {
    Fake f;
    f.rtc = true;
    auto c = f.clock();
    // Booted from the RTC: the kernel still reports the clock undisciplined
    // (loading an RTC is a plain clock set), but the state is not "none".
    CHECK_FALSE(c.synchronized());
    CHECK(c.has_rtc());
    CHECK(c.source() == "rtc");
    // The label is about provenance only: it never changes the stepping rule,
    // so a client more than a second off still corrects the clock, and the
    // source then becomes "client" -- that IS where the time came from.
    CHECK(c.step_from_client(kNow + milliseconds(300), kNow).outcome == Outcome::SkippedSmall);
    CHECK(c.source() == "rtc");
    CHECK(c.step_from_client(kNow + seconds(40), kNow).outcome == Outcome::Stepped);
    CHECK(f.sets.size() == 1);
    CHECK(c.source() == "client");
    // NTP outranks both.
    f.synchronized = true;
    CHECK(c.source() == "ntp");
    // No RTC, and NTP having taken over already forgot the client step (that
    // rule comes from the base branch), so this reads "none".
    f.synchronized = false;
    f.rtc = false;
    // has_rtc() is a cached read since open-astro#314, so the fake's new
    // answer is not visible until something off the request path re-probes.
    CHECK(c.has_rtc());
    c.refresh_rtc();
    CHECK_FALSE(c.has_rtc());
    CHECK(c.source() == "none");
    Fake g;
    CHECK(g.clock().source() == "none");
    // The two-argument constructor means "no RTC probe": never "rtc".
    HostClock no_probe([] { return false; }, [](system_clock::time_point, std::string&) { return true; });
    CHECK_FALSE(no_probe.has_rtc());
    CHECK(no_probe.source() == "none");
}

TEST_CASE("HostClock - the RTC probe is primed at construction and never re-run implicitly (#314)",
          "[util][hostclock][unit]") {
    int probes = 0;
    HostClock c([] { return false; }, [](system_clock::time_point, std::string&) { return true; },
                [&probes] {
                    ++probes;
                    return true;
                });
    // Construction is startup: the one place where a wedged I2C bus may cost
    // a second, because nothing is waiting on it.
    CHECK(probes == 1);

    // Every reader is a memory read. source() goes through has_rtc(), and on
    // an undisciplined, unstepped clock that is the branch that reports "rtc".
    for (int i = 0; i < 10; ++i) {
        CHECK(c.has_rtc());
        CHECK(c.source() == "rtc");
    }
    CHECK(probes == 1);

    // Only an explicit refresh re-probes. That is what the server's RTC probe
    // thread calls, and what #307 will call after this process writes the RTC.
    c.refresh_rtc();
    CHECK(probes == 2);
}

TEST_CASE("HostClock - an external clock set (synctime endpoint) is recorded as a client step",
          "[util][hostclock][unit]") {
    Fake f;
    f.rtc = true;
    auto c = f.clock();
    CHECK(c.source() == "rtc");
    CHECK_FALSE(c.stepped_by_client());
    c.mark_stepped();
    CHECK(c.stepped_by_client());
    CHECK(c.source() == "client");
    CHECK(f.sets.empty());  // nothing was set through this object
    // NTP taking over still clears it, and the RTC label returns underneath.
    f.synchronized = true;
    CHECK(c.source() == "ntp");
    f.synchronized = false;
    CHECK(c.source() == "rtc");
}

TEST_CASE("HostClock - the real kernel RTC probe is callable and self-consistent", "[util][hostclock][unit]") {
    HostClock real;
    const bool probe = HostClock::host_booted_from_rtc();
    CHECK(real.has_rtc() == probe);
    CHECK(HostClock::host_booted_from_rtc() == probe);  // stable across calls
    if (!real.synchronized() && !real.stepped_by_client()) {
        CHECK(real.source() == (probe ? "rtc" : "none"));
    }
}

TEST_CASE("HostClock - outcome names are stable log text", "[util][hostclock][unit]") {
    CHECK(std::string(HostClock::outcome_name(Outcome::Stepped)) == "stepped");
    CHECK(std::string(HostClock::outcome_name(Outcome::SkippedSynchronized)).find("NTP") != std::string::npos);
    CHECK(std::string(HostClock::outcome_name(Outcome::SkippedDisabled)).find("syncSystemClockFromClients") !=
          std::string::npos);
    CHECK(std::string(HostClock::outcome_name(Outcome::SkippedOutOfRange)).find("2000-2100") != std::string::npos);
    CHECK(std::string(HostClock::outcome_name(Outcome::SkippedSmall)).find("1 s") != std::string::npos);
    CHECK(std::string(HostClock::outcome_name(Outcome::Failed)) == "failed");
}

TEST_CASE("HostClock - default construction queries the real kernel without stepping", "[util][hostclock][unit]") {
    // The real adjtimex() path must be callable unprivileged; whether this
    // host is synchronised depends on the machine, so only the type is checked.
    HostClock real;
    const bool s = real.synchronized();
    CHECK((s == true || s == false));
    // "rtc" is what an NTP-less host with a plausible boot RTC reports, so the
    // readout is checked against the real probe, as the RTC case above does.
    const std::string src = real.source();
    if (s) {
        CHECK(src == "ntp");
    } else {
        CHECK(src == (HostClock::host_booted_from_rtc() ? "rtc" : "none"));
    }
    real.set_enabled(false);  // never call clock_settime from a unit test
    CHECK(real.step_from_client(kNow, kNow).outcome == Outcome::SkippedDisabled);
}

TEST_CASE("HostClock - a manual sync marks the clock client-stepped", "[util][hostclock][unit]") {
    // The web UI's Sync Time button sets the clock outside step_from_client;
    // clock_settime leaves STA_UNSYNC set, so the readout must be told.
    alpacacore::util::HostClock c([] { return false; },
                                  [](std::chrono::system_clock::time_point, std::string&) { return true; });
    CHECK(c.source() == "none");
    c.mark_stepped();
    CHECK(c.source() == "client");
    CHECK(c.stepped_by_client());
}

// open-astro#399: the test seam replaces the hooks in place, so a reader that
// is mid-call cannot be left holding a freed object.
TEST_CASE("HostClock - set_hooks replaces the probes without disturbing the state", "[util][hostclock][unit]") {
    alpacacore::util::HostClock c([] { return false; },
                                  [](std::chrono::system_clock::time_point, std::string&) { return true; });
    c.set_enabled(false);
    c.mark_stepped();
    CHECK(c.source() == "client");

    // The hooks are what changes; the enabled flag and the step latches
    // describe what has happened to the HOST clock, which swapping the probes
    // does not undo. The old seam had to save and restore the flag by hand
    // around building a replacement object; carrying over is now the default
    // because there is no replacement.
    c.set_hooks([] { return false; }, [](std::chrono::system_clock::time_point, std::string&) { return true; });
    CHECK(c.enabled() == false);
    CHECK(c.stepped_by_client());
    CHECK(c.source() == "client");

    // The new probe really is the one being called: flipping it to
    // "synchronized" makes source() read "ntp" and clears the client latch,
    // which the old probe never would have.
    c.set_hooks([] { return true; }, [](std::chrono::system_clock::time_point, std::string&) { return true; });
    CHECK(c.synchronized());
    CHECK(c.source() == "ntp");

    // has_rtc() is re-probed by set_hooks() rather than left on the previous
    // hooks' cached answer until the next timer tick.
    c.set_hooks([] { return false; }, [](std::chrono::system_clock::time_point, std::string&) { return true; },
                [] { return true; });
    CHECK(c.has_rtc());
    CHECK(c.source() == "rtc");
}

// [stress-guard] puts this case in the sanitizers-tsan job, which runs only
// "[stress]" and "[stress-guard]". Without the tag, the TSan cleanliness this
// case exists to demonstrate is a local, manual result and CI only proves "no
// crash, no deadlock". [stress] is reserved for vendor driver registrations
// (check_stress_registration.py), and [stress-guard] is the tag that gate
// documents for a core/harness self-test that needs TSan.
TEST_CASE("HostClock - readers in flight survive a concurrent set_hooks", "[util][hostclock][unit][stress-guard]") {
    // The hazard the issue is about: every host_clock_ dereference in
    // Router::route() runs on a request thread, and since #314 the server's
    // RTC probe thread is a second, non-request reader. The old seam destroyed
    // the whole object, so a reader mid-call was left holding a freed one --
    // a use-after-free, not a stale read, prevented only by a comment saying
    // to call the seam before Server::start().
    //
    // Under TSan/ASan this case is the one that would report it. Without a
    // sanitizer it still pins that the swap neither crashes nor deadlocks,
    // and that a blocking probe cannot park a concurrent set_hooks().
    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};
    // A hang here (set_hooks() starved on the mutex, a reader wedged) would
    // otherwise run to the CI job timeout; the deadline turns it into a red
    // CHECK instead.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    const auto expired = [&] { return std::chrono::steady_clock::now() >= deadline; };
    alpacacore::util::HostClock c([] { return false; },
                                  [](std::chrono::system_clock::time_point, std::string&) { return true; });

    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!stop.load() && !expired()) {
                // The three reads the router actually makes on a request path.
                static_cast<void>(c.source());
                static_cast<void>(c.enabled());
                static_cast<void>(c.has_rtc());
                reads.fetch_add(1);
            }
        });
    }
    // A stand-in for the server's RTC probe thread, which is not a request path.
    std::thread prober([&] {
        while (!stop.load() && !expired()) {
            c.refresh_rtc();
        }
    });

    for (int i = 0; i < 200 && !expired(); ++i) {
        const bool synced = (i % 2) == 0;
        c.set_hooks([synced] { return synced; },
                    [](std::chrono::system_clock::time_point, std::string&) { return true; },
                    [synced] { return !synced; });
    }

    stop.store(true);
    for (auto& t : readers) {
        t.join();
    }
    prober.join();
    CHECK_FALSE(expired());
    CHECK(reads.load() > 0);
}

// ── The client-clock disagreement warning for mounts with their own clock (#409) ──

TEST_CASE("ClientUtcWarning - the pure rule reports only a disciplined host and only past the threshold",
          "[util][hostclock][unit]") {
    using alpacacore::util::ClientUtcWarning;
    const auto over = HostClock::kClientDisagreementWarn + milliseconds(1);
    const auto under = HostClock::kClientDisagreementWarn - milliseconds(1);

    // Undisciplined host: the client's time is the best the host will see
    // (#289), whatever the delta. Never a warning.
    CHECK_FALSE(ClientUtcWarning::disagreement(minutes(30), false).has_value());
    CHECK_FALSE(ClientUtcWarning::disagreement(-minutes(30), false).has_value());

    // Disciplined host: the shared threshold, both signs, exclusive at the
    // boundary (the router's rule is "more than", not "at least").
    CHECK_FALSE(ClientUtcWarning::disagreement(under, true).has_value());
    CHECK_FALSE(ClientUtcWarning::disagreement(-under, true).has_value());
    CHECK_FALSE(ClientUtcWarning::disagreement(HostClock::kClientDisagreementWarn, true).has_value());
    CHECK_FALSE(ClientUtcWarning::disagreement(-HostClock::kClientDisagreementWarn, true).has_value());
    REQUIRE(ClientUtcWarning::disagreement(over, true).has_value());
    CHECK(*ClientUtcWarning::disagreement(over, true) == over);
    REQUIRE(ClientUtcWarning::disagreement(-over, true).has_value());
    CHECK(*ClientUtcWarning::disagreement(-over, true) == -over);
    CHECK(*ClientUtcWarning::disagreement(minutes(30), true) == minutes(30));
}

namespace {

struct ProbeGuard {
    explicit ProbeGuard(bool synchronized) {
        alpacacore::util::ClientUtcWarning::set_host_synchronized_probe([synchronized] { return synchronized; });
    }
    ~ProbeGuard() { alpacacore::util::ClientUtcWarning::set_host_synchronized_probe(nullptr); }
};

struct WarnCounter {
    std::atomic<int> warns{0};
    std::string last;
    alpacacore::logging::LogSink previous = alpacacore::logging::get_log_sink();
    WarnCounter() {
        alpacacore::logging::set_log_sink(
            [this](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
                if (level == alpacacore::logging::LogLevel::Warn &&
                    message.find("Client UTCDate disagrees") != std::string::npos) {
                    ++warns;
                    last = std::string(message);
                }
            });
    }
    ~WarnCounter() { alpacacore::logging::set_log_sink(previous); }
};

}  // namespace

TEST_CASE("ClientUtcWarning - warn_once logs once per flag on a disciplined host and re-arms with the flag",
          "[util][hostclock][unit]") {
    using alpacacore::util::ClientUtcWarning;
    ProbeGuard probe(true);
    WarnCounter counter;
    bool warned = false;
    const auto far = system_clock::now() + minutes(30);

    // A write that agrees does not consume the once-per-connection budget:
    // the flag is set only when a line is logged, so a client whose FIRST
    // write is fine and whose later write is not is still reported.
    CHECK_FALSE(ClientUtcWarning::warn_once("Test", system_clock::now(), warned));
    CHECK_FALSE(warned);
    CHECK(counter.warns.load() == 0);

    CHECK(ClientUtcWarning::warn_once("Test", far, warned));
    CHECK(warned);
    CHECK(counter.warns.load() == 1);
    CHECK(counter.last.find("the mount's clock and pointing now follow the client") != std::string::npos);
    CHECK(counter.last.find(" ms;") != std::string::npos);

    // Repeats within the same connection are silent.
    CHECK_FALSE(ClientUtcWarning::warn_once("Test", far + seconds(1), warned));
    CHECK_FALSE(ClientUtcWarning::warn_once("Test", far - hours(2), warned));
    CHECK(counter.warns.load() == 1);

    // The connect path resets the flag: the next disagreement is reported again.
    warned = false;
    CHECK(ClientUtcWarning::warn_once("Test", far, warned));
    CHECK(counter.warns.load() == 2);
}

TEST_CASE("ClientUtcWarning - warn_once is silent on an undisciplined host", "[util][hostclock][unit]") {
    using alpacacore::util::ClientUtcWarning;
    ProbeGuard probe(false);
    WarnCounter counter;
    bool warned = false;
    CHECK_FALSE(ClientUtcWarning::warn_once("Test", system_clock::now() + minutes(30), warned));
    CHECK_FALSE(warned);
    CHECK(counter.warns.load() == 0);
}

TEST_CASE("ClientUtcWarning - a null probe restores the kernel one", "[util][hostclock][unit]") {
    using alpacacore::util::ClientUtcWarning;
    ClientUtcWarning::set_host_synchronized_probe([] { return true; });
    CHECK(ClientUtcWarning::host_synchronized());
    ClientUtcWarning::set_host_synchronized_probe(nullptr);
    CHECK(ClientUtcWarning::host_synchronized() == HostClock::kernel_is_synchronized());
}
