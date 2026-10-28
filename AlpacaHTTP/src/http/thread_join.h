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

// Internal seam (not part of the public AlpacaHTTP API -- lives under src/,
// not include/) that lets AlpacaHTTP tests observe and pin the fallback
// behavior of join_or_abandon() without touching Server itself. See issue
// #561: the fallback branches (join fails -> detach; both fail -> park in
// abandoned_threads()) previously lived in an anonymous namespace inside
// server.cpp where nothing could name them or make either call throw.

#include <cstddef>
#include <functional>
#include <thread>

namespace alpacahttp::detail {

// Join `thread`, falling back to detach() if join() throws, and parking the
// thread object in an internal leaked container if detach() also throws.
// This is the production entry point; all call sites in server.cpp use this
// one-arg form unchanged. In a test build it transparently consults a
// test-only override installed via ScopedJoinHooksForTest (see below); in a
// shipped build ALPACAHTTP_ENABLE_TEST_HOOKS is undefined, the override does
// not exist, and this calls real join()/detach() directly.
void join_or_abandon(std::thread& thread, const char* context);

// Test-visible overload: the caller supplies the join/detach implementations
// directly, so a unit test can force either one to throw without going
// through the process-wide hook override. The one-arg form above forwards to
// this with real join()/detach() as the two callables (or the installed
// override, if any).
void join_or_abandon(std::thread& thread, const char* context, const std::function<void(std::thread&)>& joiner,
                     const std::function<void(std::thread&)>& detacher);

// Number of std::thread objects currently parked because both join() and
// detach() failed on them. Test-only observation point; never touches the
// container itself.
std::size_t abandoned_thread_count();

#ifdef ALPACAHTTP_ENABLE_TEST_HOOKS
// RAII installer for a process-wide override of the one-arg join_or_abandon()
// form, used by tests that exercise real Server call sites (which only ever
// call the one-arg form and so cannot be reached by the two-callable
// overload alone). The override is a single pair of hooks shared by every
// production call site while installed; a `context` string lets a hook
// choose to act only on one call site (e.g. "join_server_thread") and defer
// to a real join()/detach() otherwise.
//
// Test-only. Null (no-op passthrough to real join()/detach()) in every
// ordinary run. Must be destroyed before any Server instance that may still
// be joining threads is torn down -- a hook that throws during ~Server's
// stop() turns the fallback into a detach of a still-live thread.
class ScopedJoinHooksForTest {
public:
    using Hook = std::function<void(std::thread&, const char* context)>;

    ScopedJoinHooksForTest(Hook joiner, Hook detacher);
    ~ScopedJoinHooksForTest();

    ScopedJoinHooksForTest(const ScopedJoinHooksForTest&) = delete;
    ScopedJoinHooksForTest& operator=(const ScopedJoinHooksForTest&) = delete;
};

// Test-only. Installs a process-wide hook that every Server worker thread
// calls as its first action, before it touches any Server state, so a test
// can hold new workers there (a stop() that lands before a worker has
// executed an instruction). Must be destroyed before any Server whose
// workers may still be starting.
class ScopedWorkerStartHookForTest {
public:
    explicit ScopedWorkerStartHookForTest(std::function<void()> hook);
    ~ScopedWorkerStartHookForTest();

    ScopedWorkerStartHookForTest(const ScopedWorkerStartHookForTest&) = delete;
    ScopedWorkerStartHookForTest& operator=(const ScopedWorkerStartHookForTest&) = delete;
};

// Test-only. Installs a process-wide hook that join_server_thread() calls
// after every wake from its condition wait, with server_thread_mutex_
// released, before it re-checks the generation. A test parks a woken waiter
// there while a restart installs the next thread. Must be destroyed before
// any Server whose waiters may still be woken.
class ScopedJoinWakeHookForTest {
public:
    explicit ScopedJoinWakeHookForTest(std::function<void()> hook);
    ~ScopedJoinWakeHookForTest();

    ScopedJoinWakeHookForTest(const ScopedJoinWakeHookForTest&) = delete;
    ScopedJoinWakeHookForTest& operator=(const ScopedJoinWakeHookForTest&) = delete;
};

// Runs the installed join-wake hook, if any. Called by join_server_thread().
void run_join_wake_hook();

// Runs the installed worker-start hook, if any. Called by Server's worker.
void run_worker_start_hook();
#endif  // ALPACAHTTP_ENABLE_TEST_HOOKS

}  // namespace alpacahttp::detail
