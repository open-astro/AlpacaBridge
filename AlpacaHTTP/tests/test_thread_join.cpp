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

// Unit tests for the join_or_abandon() fallback seam (issue #561). Before
// this seam existed, the fallback branches lived in an anonymous namespace
// inside server.cpp and were unreachable from any test: nothing outside
// server.cpp could name join_or_abandon(), and nothing could make
// thread.join()/thread.detach() throw. See http/thread_join.h.

#include <alpacacore/util/logging.h>
#include <alpacahttp/config.h>
#include <alpacahttp/util/logging_adapter.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "http/thread_join.h"
#include "test_assert.h"

namespace {

std::mutex g_captured_mutex;
std::vector<std::string> g_captured;

void capture_sink(alpacacore::logging::LogLevel level, std::string_view component, std::string_view message) {
    if (level != alpacacore::logging::LogLevel::Error) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_captured_mutex);
    g_captured.emplace_back(message);
}

std::size_t captured_count() {
    std::lock_guard<std::mutex> guard(g_captured_mutex);
    return g_captured.size();
}

std::string captured_at(std::size_t index) {
    std::lock_guard<std::mutex> guard(g_captured_mutex);
    return g_captured.at(index);
}

void reset_capture() {
    std::lock_guard<std::mutex> guard(g_captured_mutex);
    g_captured.clear();
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Case (a1): join() throws, detach() succeeds -> one ERROR line, detach
// called exactly once, the thread ends up non-joinable, and nothing is
// parked in abandoned_thread_count().
void test_join_fails_detach_succeeds() {
    reset_capture();
    const std::size_t before = alpacahttp::detail::abandoned_thread_count();

    std::thread real([] {});
    int detach_calls = 0;

    alpacahttp::detail::join_or_abandon(
        real, "test_join_fails_detach_succeeds",
        [](std::thread&) { throw std::system_error(ESRCH, std::generic_category()); },
        [&detach_calls](std::thread& t) {
            ++detach_calls;
            t.detach();
        });

    EXPECT(detach_calls == 1);
    EXPECT(!real.joinable());
    EXPECT(alpacahttp::detail::abandoned_thread_count() == before);
    EXPECT(captured_count() == 1);
    EXPECT(contains(captured_at(0), "pthread_join failed"));
    EXPECT(contains(captured_at(0), "test_join_fails_detach_succeeds"));
}

// Case (b): both join() and detach() throw -> the thread object is parked
// (abandoned_thread_count() goes up by one), two ERROR lines are logged, and
// -- the actual regression being pinned -- the process does not terminate.
// A joinable std::thread reaching this point normally means its destructor
// calls std::terminate(); the whole point of parking it in a
// never-destroyed container is to avoid that. Reaching the end of this
// function (and this whole binary exiting 0 under ctest) is the evidence
// that std::terminate() was not called. Note: the real OS thread behind
// `real` has already returned by the time this runs (its body is empty and
// it's given time to finish); only the std::thread object itself is
// deliberately leaked, by design (see thread_join.cpp's abandoned_threads()
// comment).
void test_join_and_detach_both_fail() {
    reset_capture();
    const std::size_t before = alpacahttp::detail::abandoned_thread_count();

    std::thread real([] {});
    real.join();  // Let the real thread finish normally first.
    // Re-create a joinable std::thread object to feed the seam without a
    // live OS thread backing it in an awkward state; the seam only cares
    // that `thread` is a std::thread lvalue it can move out of.
    std::thread stand_in([] {});

    alpacahttp::detail::join_or_abandon(
        stand_in, "test_join_and_detach_both_fail",
        [](std::thread&) { throw std::system_error(EDEADLK, std::generic_category()); },
        [](std::thread&) { throw std::system_error(ESRCH, std::generic_category()); });

    EXPECT(alpacahttp::detail::abandoned_thread_count() == before + 1);
    EXPECT(!stand_in.joinable());  // Moved-from: ownership went into abandoned_threads().
    EXPECT(captured_count() == 2);
    EXPECT(contains(captured_at(0), "pthread_join failed"));
    EXPECT(contains(captured_at(1), "detach also failed"));
}

// Guard the happy path: the seam must not change ordinary success behavior.
void test_happy_path_unaffected() {
    reset_capture();
    const std::size_t before = alpacahttp::detail::abandoned_thread_count();

    // A detaching joiner would also leave the thread non-joinable, so the body
    // must be observable: the flag is set only if the join really waited.
    std::atomic<bool> finished{false};
    std::thread real([&finished] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        finished.store(true);
    });
    alpacahttp::detail::join_or_abandon(real, "test_happy_path_unaffected");

    EXPECT(finished.load());
    EXPECT(!real.joinable());
    EXPECT(captured_count() == 0);
    EXPECT(alpacahttp::detail::abandoned_thread_count() == before);
}

}  // namespace

int main() {
    // The internal log_sink() only forwards to the external sink once
    // alpacacore::logging::set_log_sink has been pointed at it; init_logging
    // does that. Without this call, log_error()'s output goes to the
    // alpacacore default sink and set_external_log_sink has no effect.
    alpacahttp::Config config;
    alpacahttp::util::init_logging(config);
    alpacahttp::util::set_external_log_sink(capture_sink);

    test_happy_path_unaffected();
    test_join_fails_detach_succeeds();
    test_join_and_detach_both_fail();

    alpacahttp::util::set_external_log_sink(nullptr);

    std::cout << "All thread_join tests passed!\n";
    return 0;
}
