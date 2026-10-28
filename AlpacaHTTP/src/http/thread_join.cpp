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

#include "thread_join.h"

#include <alpacahttp/util/logging_adapter.h>

#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace alpacahttp::detail {

namespace {

// Threads that failed both join() AND detach() (should not be reachable in
// practice -- see join_or_abandon() below) are moved here instead of a bare
// `new std::thread(...)` with the pointer discarded. Two requirements this
// container has to satisfy at once, which is why it looks the way it does:
// (a) it must stay REACHABLE for the life of the process, so the `sanitizers`
// CI job's LeakSanitizer does not report the parked thread's allocation as an
// indistinguishable ordinary leak; (b) the parked std::thread objects must
// NEVER be destroyed, because they are still joinable() (both join() and
// detach() throw without clearing libstdc++'s internal id) and a joinable
// thread's destructor calls std::terminate(). A plain function-local or
// namespace-scope `static std::vector<std::thread>` satisfies (a) but not
// (b): it has a non-trivial destructor registered with `__cxa_atexit`, which
// runs at normal process exit and would abort there instead of never. A
// heap-allocated container reached through a static POINTER that is never
// deleted satisfies both: `new` keeps it reachable (satisfying LSan), and
// nothing ever runs its destructor (satisfying the no-terminate() guarantee).
std::mutex g_abandoned_threads_mutex;
std::vector<std::thread>& abandoned_threads() {
    static auto* threads = new std::vector<std::thread>();
    return *threads;
}

#ifdef ALPACAHTTP_ENABLE_TEST_HOOKS
// Process-wide test-only override of the one-arg join_or_abandon() form. Null
// in every ordinary run, and absent entirely from a shipped build: this block
// and its cost on the production join path compile only when tests are built.
std::mutex g_hook_mutex;
ScopedJoinHooksForTest::Hook g_test_joiner;
ScopedJoinHooksForTest::Hook g_test_detacher;
std::function<void()> g_worker_start_hook;
std::function<void()> g_join_wake_hook;
#endif

}  // namespace

std::size_t abandoned_thread_count() {
    std::lock_guard<std::mutex> guard(g_abandoned_threads_mutex);
    return abandoned_threads().size();
}

// Join `thread`, and if pthread_join fails (EDEADLK/ESRCH/EINVAL) fall back to
// detach() instead of a second join() attempt. A second join() is not safe
// here: libstdc++ only clears a thread's id on a SUCCESSFUL join, so the
// object is still joinable() after the exception, and a caller running under
// lifecycle_mutex_ (join_orphaned_threads()'s callers) that retried join()
// could block on it -- if the thread is not actually gone but merely blocked
// waiting for that same mutex (run_server()'s spawn phase takes it), the
// retry would deadlock instead of throwing. detach() makes the destructor a
// no-op at the cost of never confirming the thread has exited; if detach()
// also throws (both calls failing on the same OS handle is not reachable in
// practice), the thread object is parked in abandoned_threads() rather than
// left to destruct joinable, which would call std::terminate().
void join_or_abandon(std::thread& thread, const char* context, const std::function<void(std::thread&)>& joiner,
                     const std::function<void(std::thread&)>& detacher) {
    try {
        joiner(thread);
        return;
    } catch (const std::system_error& e) {
        util::log_error(std::string(context) + ": pthread_join failed (" + e.code().message() +
                        "), detaching thread instead of reaping it");
    }
    try {
        detacher(thread);
    } catch (const std::system_error& e) {
        util::log_error(std::string(context) + ": detach also failed (" + e.code().message() +
                        ") after a failed join; leaking the thread object rather than terminating");
        std::lock_guard<std::mutex> guard(g_abandoned_threads_mutex);
        abandoned_threads().push_back(std::move(thread));
    }
}

void join_or_abandon(std::thread& thread, const char* context) {
#ifdef ALPACAHTTP_ENABLE_TEST_HOOKS
    ScopedJoinHooksForTest::Hook joiner_hook;
    ScopedJoinHooksForTest::Hook detacher_hook;
    {
        std::lock_guard<std::mutex> guard(g_hook_mutex);
        joiner_hook = g_test_joiner;
        detacher_hook = g_test_detacher;
    }
    if (joiner_hook && detacher_hook) {
        join_or_abandon(
            thread, context, [&joiner_hook, context](std::thread& t) { joiner_hook(t, context); },
            [&detacher_hook, context](std::thread& t) { detacher_hook(t, context); });
        return;
    }
#endif
    join_or_abandon(thread, context, [](std::thread& t) { t.join(); }, [](std::thread& t) { t.detach(); });
}

#ifdef ALPACAHTTP_ENABLE_TEST_HOOKS
ScopedJoinHooksForTest::ScopedJoinHooksForTest(Hook joiner, Hook detacher) {
    std::lock_guard<std::mutex> guard(g_hook_mutex);
    g_test_joiner = std::move(joiner);
    g_test_detacher = std::move(detacher);
}

ScopedJoinHooksForTest::~ScopedJoinHooksForTest() {
    std::lock_guard<std::mutex> guard(g_hook_mutex);
    g_test_joiner = nullptr;
    g_test_detacher = nullptr;
}

ScopedWorkerStartHookForTest::ScopedWorkerStartHookForTest(std::function<void()> hook) {
    std::lock_guard<std::mutex> guard(g_hook_mutex);
    g_worker_start_hook = std::move(hook);
}

ScopedWorkerStartHookForTest::~ScopedWorkerStartHookForTest() {
    std::lock_guard<std::mutex> guard(g_hook_mutex);
    g_worker_start_hook = nullptr;
}

ScopedJoinWakeHookForTest::ScopedJoinWakeHookForTest(std::function<void()> hook) {
    std::lock_guard<std::mutex> guard(g_hook_mutex);
    g_join_wake_hook = std::move(hook);
}

ScopedJoinWakeHookForTest::~ScopedJoinWakeHookForTest() {
    std::lock_guard<std::mutex> guard(g_hook_mutex);
    g_join_wake_hook = nullptr;
}

void run_join_wake_hook() {
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> guard(g_hook_mutex);
        hook = g_join_wake_hook;
    }
    if (hook) {
        hook();
    }
}

void run_worker_start_hook() {
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> guard(g_hook_mutex);
        hook = g_worker_start_hook;
    }
    if (hook) {
        hook();
    }
}
#endif  // ALPACAHTTP_ENABLE_TEST_HOOKS

}  // namespace alpacahttp::detail
