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

// Self-test for StressCallGuard (issue #322) — the per-call guard every
// [stress] registration is meant to share, replacing the per-file `call()`
// copies whose catch type has flip-flopped across review rounds. These
// assertions ARE the harness-level decision: swallow an expected
// AlpacaError code silently, count everything else, and never intercept a
// non-std::exception throw.

#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"

using alpacacore::AlpacaException;
using alpacacore::test::StressCallGuard;
namespace AlpacaError = alpacacore::AlpacaError;

TEST_CASE("StressCallGuard - a call that does not throw is not counted", "[unit]") {
    StressCallGuard guard;
    guard([] {});
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.report().empty());
}

TEST_CASE("StressCallGuard - the default expected code (NotConnected) is swallowed silently", "[unit]") {
    StressCallGuard guard;
    guard([] { throw AlpacaException("racing a disconnect", AlpacaError::NotConnected); });
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.report().empty());
}

TEST_CASE("StressCallGuard - an AlpacaException with an unexpected code is counted", "[unit]") {
    StressCallGuard guard;
    guard([] { throw AlpacaException("bug", AlpacaError::DriverException); });
    CHECK(guard.unexpected_count() == 1);
    CHECK(guard.report().find("AlpacaException") != std::string::npos);
    CHECK(guard.report().find("bug") != std::string::npos);
    CHECK(guard.report().find(std::to_string(AlpacaError::DriverException)) != std::string::npos);
}

TEST_CASE("StressCallGuard - any other std::exception is counted, not silently dropped", "[unit]") {
    // The exact failure mode #322 exists to fix: a non-Alpaca throw escaping
    // teardown (e.g. std::system_error from serial/curl) must not be
    // indistinguishable from an expected NotConnected.
    StressCallGuard guard;
    guard([] { throw std::out_of_range("index out of range"); });
    CHECK(guard.unexpected_count() == 1);
    CHECK(guard.report().find("index out of range") != std::string::npos);
}

TEST_CASE("StressCallGuard - a caller-specified expected code is swallowed", "[unit]") {
    // e.g. WeeWX's PropertyNotImplemented getters that answer without a
    // connection — a widened set is an explicit per-file opt-in, not a
    // silent default.
    StressCallGuard guard({AlpacaError::NotConnected, AlpacaError::PropertyNotImplemented});
    guard([] { throw AlpacaException("no such property", AlpacaError::PropertyNotImplemented); });
    CHECK(guard.unexpected_count() == 0);
}

TEST_CASE("StressCallGuard - a caller-specified set REPLACES the default, not extends it", "[unit]") {
    // The exact footgun the header comment and AGENTS.md warn about in bold:
    // this must NOT still treat NotConnected as expected just because it's
    // the default. Passing only {InvalidValue} means NotConnected is now
    // unexpected too -- without this case, an implementation that quietly
    // unioned the caller's set with the default would pass every other test
    // in this file identically.
    StressCallGuard guard({AlpacaError::InvalidValue});
    guard([] { throw AlpacaException("racing a disconnect", AlpacaError::NotConnected); });
    CHECK(guard.unexpected_count() == 1);
}

TEST_CASE("StressCallGuard - a call after an unexpected throw still keeps isolation", "[unit]") {
    // The whole point: the FIRST throw must not skip the calls after it, the
    // way run_lifecycle_stress's outer catch would.
    StressCallGuard guard;
    int reached = 0;
    guard([] { throw std::runtime_error("first call fails"); });
    guard([&] { ++reached; });
    guard([] { throw AlpacaException("third call fails", AlpacaError::DriverException); });
    CHECK(reached == 1);
    CHECK(guard.unexpected_count() == 2);
}

TEST_CASE("StressCallGuard - a non-std::exception throw is not intercepted", "[unit]") {
    // Must reach the caller exactly as it does today -- and from there,
    // std::terminate on a raw thread (run_lifecycle_stress's own catch
    // around the operate call is also catch(const std::exception&), so this
    // never reaches that either).
    StressCallGuard guard;
    CHECK_THROWS_AS(guard([] { throw 42; }), int);
    CHECK(guard.unexpected_count() == 0);
}

TEST_CASE("StressCallGuard - report() caps the number of DISTINCT failure modes", "[unit]") {
    StressCallGuard guard;
    for (int i = 0; i < 20; ++i) {
        guard([i] { throw std::runtime_error("failure " + std::to_string(i)); });
    }
    CHECK(guard.unexpected_count() == 20);
    const std::string report = guard.report();
    CHECK(report.find("failure 0") != std::string::npos);
    // Assert the exact overflow line, not just the word "more": a bare
    // substring search would also be satisfied by an exception message that
    // happened to contain it. 20 distinct modes recorded, kMaxSamples kept,
    // the remaining 12 summarised by count.
    CHECK(report.find("... and 12 more distinct failure mode(s), 12 occurrence(s)") != std::string::npos);
    // The cap held: the 9th-onward modes are absent.
    CHECK(report.find("failure 8") == std::string::npos);
}

TEST_CASE("StressCallGuard - a repeated failure mode is one sample with a count", "[unit]") {
    // Issue #377: the cap used to be first-N EVENTS, so one thread faulting in
    // a tight loop filled all eight slots with copies of the same message
    // before any other thread recorded once, and every later distinct mode
    // vanished into a bare "... and N more". Nothing was hidden from the CHECK
    // -- unexpected_count() was and is exact -- but the text a human reads
    // while diagnosing a CI-only failure showed one problem where there were
    // several, biased toward whichever mode fired first.
    StressCallGuard guard;
    for (int i = 0; i < 50; ++i) {
        guard([] { throw std::runtime_error("the noisy one"); });
    }
    guard([] { throw std::out_of_range("the interesting one"); });

    CHECK(guard.unexpected_count() == 51);
    const std::string report = guard.report();
    // The repeat collapses to a single labelled line carrying its count...
    CHECK(report.find("the noisy one (x50)") != std::string::npos);
    // ...which leaves room for the mode that actually mattered. Under the old
    // first-N-events cap this line was the one that got summarised away.
    CHECK(report.find("the interesting one (x1)") != std::string::npos);
    // Two modes, both sampled, so no overflow tail at all.
    CHECK(report.find("... and") == std::string::npos);
}

TEST_CASE("StressCallGuard - same code with different messages are distinct modes", "[unit]") {
    // Keyed on the message as well as the code: in a connected registration a
    // live driver throws InvalidValue from several unrelated calls, and the
    // message is what names which one. Collapsing on the code alone would hide
    // exactly the distinction the reader is looking for.
    StressCallGuard guard;
    guard([] { throw AlpacaException("slew below the horizon", AlpacaError::InvalidValue); });
    guard([] { throw AlpacaException("slew below the horizon", AlpacaError::InvalidValue); });
    guard([] { throw AlpacaException("brightness out of range", AlpacaError::InvalidValue); });

    CHECK(guard.unexpected_count() == 3);
    const std::string report = guard.report();
    CHECK(report.find("slew below the horizon (x2)") != std::string::npos);
    CHECK(report.find("brightness out of range (x1)") != std::string::npos);
}

TEST_CASE("StressCallGuard - the overflow tail counts modes and occurrences separately", "[unit]") {
    // Nine distinct modes, the ninth repeated: eight are sampled, and the tail
    // has to say both how many modes it dropped and how many events they
    // account for, or the reader cannot tell a rare unlabelled mode from a
    // flood of one.
    StressCallGuard guard;
    for (int i = 0; i < 8; ++i) {
        guard([i] { throw std::runtime_error("mode " + std::to_string(i)); });
    }
    for (int i = 0; i < 5; ++i) {
        guard([] { throw std::runtime_error("the ninth"); });
    }
    CHECK(guard.unexpected_count() == 13);
    const std::string report = guard.report();
    CHECK(report.find("mode 0 (x1)") != std::string::npos);
    CHECK(report.find("the ninth") == std::string::npos);
    CHECK(report.find("... and 1 more distinct failure mode(s), 5 occurrence(s)") != std::string::npos);
}

TEST_CASE("StressCallGuard - the unsampled-mode key list is itself bounded", "[unit]") {
    // The tail counts distinct unsampled modes by keeping their keys, which a
    // storm whose every message is unique would otherwise grow without bound
    // in a test helper that runs under TSan. Past the bound the count carries
    // a "+" rather than silently understating.
    StressCallGuard guard;
    const int total = 8 + 64 + 10;
    for (int i = 0; i < total; ++i) {
        guard([i] { throw std::runtime_error("mode " + std::to_string(i)); });
    }
    CHECK(guard.unexpected_count() == total);
    const std::string report = guard.report();
    CHECK(report.find("... and 64+ more distinct failure mode(s), 74 occurrence(s)") != std::string::npos);
}

TEST_CASE("StressCallGuard - concurrent hits from many threads count exactly", "[stress-guard][unit]") {
    // Tagged [stress-guard], NOT [stress] -- this needs TSan too (a lost-update
    // bug in count_/samples_ would otherwise only show as an occasional flaky
    // count under ASan), but [stress] is reserved for vendor driver
    // registrations: the CI/ci_preflight zero-coverage guard runs
    // `alpacacore_tests "[stress]"` and fails the job if that filter matches
    // zero tests, specifically so an environment change that silently drops
    // every vendor stress target can't green-light with no concurrency
    // coverage. This file is unconditional in TEST_SOURCES (no vendor target
    // guard), so tagging this case [stress] would let it alone satisfy that
    // filter even with every vendor target absent -- defeating the exact
    // check it exists to be. [stress-guard] gets its own separate TSan
    // invocation instead (see ci.yml / ci_preflight.sh), decoupled from the
    // vendor-registration count.
    //
    // That was not hypothetical: review found the guard already vacuous,
    // because test_async_connectable.cpp's lifecycle case carried [stress]
    // from the same unconditional block. It is [stress-guard] now too. If you
    // are about to tag something [stress] that is not a vendor driver
    // registration, this is the comment telling you not to.
    // op_threads hits one guard concurrently in real registrations.
    StressCallGuard guard;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 200;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&guard] {
            for (int i = 0; i < kPerThread; ++i) {
                guard([i] {
                    if (i % 2 == 0) {
                        throw AlpacaException("expected", AlpacaError::NotConnected);
                    }
                    throw AlpacaException("unexpected", AlpacaError::DriverException);
                });
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    CHECK(guard.unexpected_count() == kThreads * kPerThread / 2);
}

TEST_CASE("StressCallGuard - a non-Alpaca throw is labelled with the raw typeid name", "[unit]") {
    // report()'s type label for a non-Alpaca throw is typeid(ex).name() passed
    // through verbatim, never demangled. On the Itanium ABI (libstdc++/libc++)
    // that is "St12out_of_range" rather than "std::out_of_range", which the
    // header documents so a reader doesn't take the prefix for garbage.
    //
    // Compare against typeid(...).name() rather than that literal: the property
    // worth guarding is "whatever the ABI calls this type, unmodified", and
    // hardcoding the Itanium spelling would fail under MSVC ("class
    // std::out_of_range") for a reason that has nothing to do with the guard.
    // Demangling still fails this on the platforms where demangling is a
    // visible change, which is the regression it exists to catch.
    StressCallGuard guard;
    guard([] { throw std::out_of_range("boom"); });
    const std::string report = guard.report();
    INFO(report);
    CHECK(report.find("out_of_range") != std::string::npos);
    CHECK(report.find(typeid(std::out_of_range).name()) != std::string::npos);
}
