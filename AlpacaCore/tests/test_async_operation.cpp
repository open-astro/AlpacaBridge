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

// Tests for util::AsyncOperation, the async operation slot (issue #717,
// decision record docs/decisions/0006-async-operation-ownership.md).
//
// Every case but the storm runs on a FakeTaskClock: time moves only through
// advance(), and every rendezvous with a body in ctx.wait_for() goes through
// wait_for_waiters(n, 2s). Every blocking step on the test thread has a
// real-time bound (a future waited for 2 s, or a yield loop with a 2 s
// deadline), so a defect fails the case instead of hanging the binary.
//
// "Blocked in a fake transaction" means a body waiting on a test-owned Gate
// that ignores the slot's cancel, the shape of a serial call blocked until its
// own timeout. The Harness opens every gate before it destroys the slot.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/error_handling.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_task_clock.h"

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::AlpacaException;
using alpacacore::test::FakeTaskClock;
using alpacacore::test::StressCallGuard;
using alpacacore::util::AsyncOperation;
using alpacacore::util::OperationContext;
using alpacacore::util::OperationGeneration;
using alpacacore::util::StopReason;
using namespace std::chrono_literals;

// Case 9: the slot owns a mutex and threads that capture `this` (rule 11).
static_assert(!std::is_copy_constructible_v<AsyncOperation> && !std::is_move_constructible_v<AsyncOperation>,
              "AsyncOperation must be neither copyable nor movable");
static_assert(!std::is_copy_assignable_v<AsyncOperation> && !std::is_move_assignable_v<AsyncOperation>,
              "AsyncOperation must be neither copy- nor move-assignable");

namespace {

constexpr auto kBound = 2s;

// Spins (yielding, no sleep) until pred() is true or `bound` of real time
// passes; returns pred().
bool eventually(const std::function<bool()>& pred, std::chrono::milliseconds bound = kBound) {
    const auto give_up = std::chrono::steady_clock::now() + bound;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= give_up) {
            return pred();
        }
        std::this_thread::yield();
    }
    return true;
}

// A test-owned gate: a body blocked in wait() ignores the slot's cancel.
class Gate {
public:
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return open_; });
    }
    void open() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool open_ = false;
};

// What a body saw, readable from the test thread.
struct Probe {
    std::atomic<bool> entered{false};
    std::atomic<bool> exited{false};
    std::atomic<int> wait_result{-1};  // -1 until the body's wait returned
    std::atomic<StopReason> reason{StopReason::None};
    std::atomic<int> stops{0};  // "sends a stop": only when the reason is Cancelled
    std::atomic<std::uint64_t> token{0};
};

using ProbePtr = std::shared_ptr<Probe>;
using GatePtr = std::shared_ptr<Gate>;
using Body = std::function<void(OperationContext&)>;

// Records the reason and sends a stop only when it is Cancelled, as a
// driver's stop tail must (CONTEXT.md, "Cancelled vs superseded").
void finish(const ProbePtr& p, OperationContext& ctx) {
    const auto reason = ctx.stop_reason();
    p->reason = reason;
    if (reason == StopReason::Cancelled) {
        p->stops.fetch_add(1);
    }
    p->exited = true;
}

// Blocked in a fake transaction until the gate opens, then finishes.
Body gated_body(const ProbePtr& p, const GatePtr& g) {
    return [p, g](OperationContext& ctx) {
        p->token = ctx.generation();
        p->entered = true;
        g->wait();
        finish(p, ctx);
    };
}

// Runs its callback when the thread that last touched it exits. A body's
// thread exits only after the slot has published that the body returned, so
// this is the one point a test can wait for that publication; `exited` is
// set inside the body, before it.
struct ThreadExitHook {
    std::function<void()> on_exit;
    ThreadExitHook() = default;
    ThreadExitHook(const ThreadExitHook&) = delete;
    ThreadExitHook& operator=(const ThreadExitHook&) = delete;
    ~ThreadExitHook() {
        if (on_exit) {
            on_exit();
        }
    }
};

// gated_body() that also sets `thread_ended` when its thread has exited.
Body gated_body_signalling_exit(const ProbePtr& p, const GatePtr& g, std::shared_ptr<std::atomic<bool>> thread_ended) {
    return [inner = gated_body(p, g), thread_ended](OperationContext& ctx) {
        thread_local ThreadExitHook hook;
        hook.on_exit = [thread_ended] { thread_ended->store(true); };
        inner(ctx);
    };
}

// One wait of `d` on the slot's clock, then finishes.
Body waiting_body(const ProbePtr& p, std::chrono::nanoseconds d) {
    return [p, d](OperationContext& ctx) {
        p->token = ctx.generation();
        p->entered = true;
        p->wait_result = ctx.wait_for(d) ? 1 : 0;
        finish(p, ctx);
    };
}

// A stop-completion body: polls every 50 ms until stopped, then finishes.
Body polling_body(const ProbePtr& p) {
    return [p](OperationContext& ctx) {
        p->token = ctx.generation();
        p->entered = true;
        while (ctx.wait_for(50ms)) {
        }
        finish(p, ctx);
    };
}

// One fake clock, one generation and one slot on them. The destructor opens
// every gate, releases any wait on the clock, joins every helper thread and
// only then destroys the slot, so a failed case still tears down.
struct Harness {
    FakeTaskClock clock;
    OperationGeneration generation;
    std::unique_ptr<AsyncOperation> op;
    std::vector<GatePtr> gates;
    std::vector<std::thread> helpers;
    // Helpers whose function has returned; each helper counts itself last.
    std::shared_ptr<std::atomic<std::size_t>> helpers_done = std::make_shared<std::atomic<std::size_t>>(0);

    Harness() : op(std::make_unique<AsyncOperation>("test-slot", generation, clock)) {}
    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;
    ~Harness() {
        for (auto& g : gates) {
            g->open();
        }
        // A start() refused at the bound throws; nothing else waits this long.
        clock.advance(AsyncOperation::kStaleReapTimeout + 1h);
        // The helper joins are bounded: a helper still running after kBound
        // (a slot that no longer returns from start() or from its joins)
        // aborts the binary with a message instead of hanging it. A
        // destructor cannot fail a Catch2 case, and detaching a helper that
        // references this Harness would be a use-after-free.
        if (!eventually([&] { return helpers_done->load() == helpers.size(); })) {
            std::fprintf(stderr, "test_async_operation: %zu of %zu helper threads still running after %lld ms\n",
                         helpers.size() - helpers_done->load(), helpers.size(),
                         static_cast<long long>(std::chrono::milliseconds(kBound).count()));
            std::abort();
        }
        for (auto& t : helpers) {
            t.join();
        }
        // AsyncOperation's own destructor joins with no bound (rule 8); every
        // gate is open and every clock wait released, so it returns.
        op.reset();
    }

    GatePtr gate() {
        gates.push_back(std::make_shared<Gate>());
        return gates.back();
    }

    // Runs fn on a helper thread; the result arrives through the future.
    template <typename Fn>
    auto on_helper(Fn fn) -> std::future<decltype(fn())> {
        std::packaged_task<decltype(fn())()> task(std::move(fn));
        auto future = task.get_future();
        helpers.emplace_back([task = std::move(task), done = helpers_done]() mutable {
            task();
            done->fetch_add(1);
        });
        return future;
    }

    // start() on a helper thread; true when it returned within the bound.
    bool start_within(Body body) {
        auto f = on_helper([this, body = std::move(body)]() mutable { return op->start(std::move(body)); });
        if (f.wait_for(kBound) != std::future_status::ready) {
            return false;
        }
        f.get();
        return true;
    }
};

ProbePtr probe() { return std::make_shared<Probe>(); }

}  // namespace

// Case 1: the three task_wait_for bodies' contract, through the slot.
TEST_CASE("AsyncOperation - wait contract", "[util][async_operation][unit]") {
    Harness h;
    auto p = probe();

    SECTION("true after the full time passes") {
        REQUIRE(h.start_within(waiting_body(p, 100ms)));
        REQUIRE(h.clock.wait_for_waiters(1, kBound));
        h.clock.advance(99ms);
        CHECK(h.clock.waiter_count() == 1);  // not due, not woken
        h.clock.advance(1ms);
        REQUIRE(eventually([&] { return p->exited.load(); }));
        CHECK(p->wait_result == 1);
        CHECK(p->reason.load() == StopReason::None);
    }

    SECTION("false at once, with virtual time unchanged, on a cancel mid-wait") {
        REQUIRE(h.start_within(waiting_body(p, 100ms)));
        REQUIRE(h.clock.wait_for_waiters(1, kBound));
        const auto before = h.clock.now();
        h.op->cancel();
        REQUIRE(eventually([&] { return p->exited.load(); }));
        CHECK(p->wait_result == 0);
        CHECK(h.clock.now() == before);
        CHECK(p->reason.load() == StopReason::Cancelled);
    }

    SECTION("false when cancelled and advanced past the deadline (cancel wins)") {
        REQUIRE(h.start_within(waiting_body(p, 100ms)));
        REQUIRE(h.clock.wait_for_waiters(1, kBound));
        h.op->cancel();
        h.clock.advance(200ms);
        REQUIRE(eventually([&] { return p->exited.load(); }));
        CHECK(p->wait_result == 0);
    }

    SECTION("false without registering a waiter when cancelled before the call") {
        auto g = h.gate();
        auto registered = std::make_shared<std::atomic<std::size_t>>(99);
        REQUIRE(h.start_within([p, g, registered, &h](OperationContext& ctx) {
            p->entered = true;
            g->wait();
            // Never advanced: a wait that registered would block forever.
            p->wait_result = ctx.wait_for(100ms) ? 1 : 0;
            *registered = h.clock.waiter_count();
            p->exited = true;
        }));
        REQUIRE(eventually([&] { return p->entered.load(); }));
        h.op->cancel();
        g->open();
        REQUIRE(eventually([&] { return p->exited.load(); }));
        CHECK(p->wait_result == 0);
        CHECK(registered->load() == 0);
    }
}

// Case 2: start() does not wait for the body it replaces (rule 2).
TEST_CASE("AsyncOperation - start returns while the old body is blocked", "[util][async_operation][unit]") {
    Harness h;
    auto a = probe();
    auto ga = h.gate();
    auto a_thread_ended = std::make_shared<std::atomic<bool>>(false);
    REQUIRE(h.start_within(gated_body_signalling_exit(a, ga, a_thread_ended)));
    REQUIRE(eventually([&] { return a->entered.load(); }));

    auto b = probe();
    REQUIRE(h.start_within(waiting_body(b, 1h)));  // A's gate is still closed
    CHECK_FALSE(a->exited);
    CHECK(h.op->stale_count() == 1);

    ga->open();
    // Wait for A's thread to exit, not only for `exited`: the next start()
    // joins A only once the slot has published that A returned.
    REQUIRE(eventually([&] { return a_thread_ended->load(); }));
    CHECK(a->exited);
    CHECK(a->reason.load() == StopReason::Superseded);
    CHECK(a->stops == 0);
    CHECK(h.op->stale_count() == 1);  // returned, not joined yet

    SECTION("the next start() joins it") {
        auto c = probe();
        REQUIRE(h.start_within(waiting_body(c, 1h)));
        CHECK(h.op->stale_count() == 1);  // A joined; B is the one stale body now
    }

    SECTION("cancel_all_and_join() joins it") {
        h.op->cancel_all_and_join();
        CHECK(h.op->stale_count() == 0);
    }
}

// Case 3: at most kMaxStaleBodies replaced bodies still running (rule 3).
TEST_CASE("AsyncOperation - stale bound", "[util][async_operation][unit]") {
    STATIC_REQUIRE(AsyncOperation::kMaxStaleBodies == 3);
    STATIC_REQUIRE(AsyncOperation::kStaleReapTimeout == 2000ms);

    Harness h;
    std::vector<ProbePtr> p;
    std::vector<GatePtr> g;
    for (int i = 0; i < 5; ++i) {
        p.push_back(probe());
        g.push_back(h.gate());
    }
    for (int i = 0; i < 4; ++i) {
        REQUIRE(h.start_within(gated_body(p[i], g[i])));
        REQUIRE(eventually([&] { return p[i]->entered.load(); }));
    }
    CHECK(h.op->stale_count() == 3);
    const auto generation_before = h.generation.current();
    const auto failure_before = h.op->last_failure();

    // The refusal is caught on the helper thread and returned by value: an
    // exception carried through the future's exception_ptr is released on the
    // helper thread while the test thread reads it, which ThreadSanitizer
    // reports against the uninstrumented libstdc++ reference count.
    struct Outcome {
        std::uint64_t token = 0;
        int error_code = 0;
        std::string message;
    };
    auto fifth = h.on_helper([&h, body = gated_body(p[4], g[4])]() mutable {
        Outcome out;
        try {
            out.token = h.op->start(std::move(body));
        } catch (const AlpacaException& e) {
            out.error_code = e.error_code();
            out.message = e.what();
        }
        return out;
    });
    CHECK(h.clock.wait_for_waiters(1, kBound));  // the fifth start() waits on the clock
    CHECK(h.op->stale_count() == 3);
    CHECK(fifth.wait_for(0s) == std::future_status::timeout);
    CHECK_FALSE(p[4]->entered);

    SECTION("the oldest returns in time: start() joins it and continues") {
        g[0]->open();
        REQUIRE(fifth.wait_for(kBound) == std::future_status::ready);
        const auto out = fifth.get();
        CHECK(out.error_code == 0);
        CHECK(out.token == generation_before + 1);
        CHECK(p[0]->exited);
        CHECK(h.op->stale_count() == 3);
        REQUIRE(eventually([&] { return p[4]->entered.load(); }));
    }

    SECTION("the bound expires: start() refuses and changes nothing") {
        h.clock.advance(AsyncOperation::kStaleReapTimeout);
        REQUIRE(fifth.wait_for(kBound) == std::future_status::ready);
        const auto out = fifth.get();
        CHECK(out.error_code == AlpacaError::InvalidOperation);
        CHECK(out.message == "test-slot: the previous operations are still finishing; try again");
        CHECK(h.generation.current() == generation_before);
        CHECK(h.op->stale_count() == 3);
        CHECK(h.op->running());
        CHECK(h.op->last_failure() == failure_before);  // not cleared (case 6)
        CHECK_FALSE(p[4]->entered);

        // The current body was not superseded.
        g[3]->open();
        REQUIRE(eventually([&] { return p[3]->exited.load(); }));
        CHECK(p[3]->reason.load() == StopReason::None);
    }
}

// Case 4: a stop body on one slot sees Superseded when a slew on another slot,
// or a direct bump(), takes the shared generation (rules 4, 5, 6).
TEST_CASE("AsyncOperation - shared generation: a stop body sees superseded", "[util][async_operation][unit]") {
    Harness h;  // h.op is the stop slot S
    AsyncOperation slew("slew-slot", h.generation, h.clock);
    auto s = probe();

    const auto token = h.op->start(polling_body(s));
    CHECK(token == h.generation.current());
    REQUIRE(h.clock.wait_for_waiters(1, kBound));
    CHECK(s->token == token);

    SECTION("W.start() bumps the generation") {
        auto w = probe();
        const auto slew_token = slew.start(waiting_body(w, 1h));
        CHECK(slew_token == token + 1);
        REQUIRE(eventually([&] { return w->entered.load(); }));
    }

    SECTION("a direct bump()") { CHECK(h.generation.bump() == token + 1); }

    // No "S is still waiting" check here: the slot does not notify S on a
    // bump elsewhere (rule 6), but a condition variable may wake spuriously
    // (seen under ThreadSanitizer), and S then correctly sees the new
    // generation and ends early. Either way it ends Superseded.

    h.clock.advance(50ms);
    REQUIRE(eventually([&] { return s->exited.load(); }));
    CHECK(s->reason.load() == StopReason::Superseded);
    CHECK(s->stops == 0);
}

// Case 5: Superseded takes precedence over Cancelled (rule 5).
TEST_CASE("AsyncOperation - replaced sees superseded, aborted sees cancelled", "[util][async_operation][unit]") {
    Harness h;
    auto p = probe();

    SECTION("replaced by start() on its own slot: Superseded, no stop") {
        REQUIRE(h.start_within(polling_body(p)));
        REQUIRE(h.clock.wait_for_waiters(1, kBound));
        REQUIRE(h.start_within(waiting_body(probe(), 1h)));
        REQUIRE(eventually([&] { return p->exited.load(); }));  // woken at once
        CHECK(p->reason.load() == StopReason::Superseded);
        CHECK(p->stops == 0);
    }

    SECTION("stopped by cancel(): Cancelled, one stop") {
        REQUIRE(h.start_within(polling_body(p)));
        REQUIRE(h.clock.wait_for_waiters(1, kBound));
        h.op->cancel();
        REQUIRE(eventually([&] { return p->exited.load(); }));
        CHECK(p->reason.load() == StopReason::Cancelled);
        CHECK(p->stops == 1);
    }

    SECTION("cancelled, then replaced before it looks: Superseded, no stop") {
        auto g = h.gate();
        REQUIRE(h.start_within(gated_body(p, g)));
        REQUIRE(eventually([&] { return p->entered.load(); }));
        h.op->cancel();
        REQUIRE(h.start_within(waiting_body(probe(), 1h)));
        g->open();
        REQUIRE(eventually([&] { return p->exited.load(); }));
        CHECK(p->reason.load() == StopReason::Superseded);
        CHECK(p->stops == 0);
    }

    SECTION("superseded by the shared generation, then reached by cancel_all_and_join(): Superseded, no stop") {
        AsyncOperation slew("slew-slot", h.generation, h.clock);
        auto g = h.gate();
        REQUIRE(h.start_within(gated_body(p, g)));
        REQUIRE(eventually([&] { return p->entered.load(); }));
        slew.start(waiting_body(probe(), 1h));
        auto joined = h.on_helper([&h] { h.op->cancel_all_and_join(); });
        // cancel_all_and_join() has marked and taken the body once the slot
        // reports nothing running; it is now blocked joining the gated body.
        REQUIRE(eventually([&] { return !h.op->running(); }));
        CHECK(joined.wait_for(0s) == std::future_status::timeout);
        g->open();
        REQUIRE(joined.wait_for(kBound) == std::future_status::ready);
        CHECK(p->exited);
        CHECK(p->reason.load() == StopReason::Superseded);
        CHECK(p->stops == 0);
    }

    SECTION("replaced on its own slot, then reached by cancel_all_and_join(): Superseded, no stop") {
        auto g = h.gate();
        REQUIRE(h.start_within(gated_body(p, g)));
        REQUIRE(eventually([&] { return p->entered.load(); }));
        REQUIRE(h.start_within(waiting_body(probe(), 1h)));
        auto joined = h.on_helper([&h] { h.op->cancel_all_and_join(); });
        REQUIRE(eventually([&] { return h.op->stale_count() == 0; }));
        g->open();
        REQUIRE(joined.wait_for(kBound) == std::future_status::ready);
        CHECK(p->reason.load() == StopReason::Superseded);
        CHECK(p->stops == 0);
    }
}

// Case 6: a throw never reaches std::terminate; the current body's is kept
// (rule 9).
TEST_CASE("AsyncOperation - failure retention", "[util][async_operation][unit]") {
    Harness h;
    CHECK_FALSE(h.op->last_failure().has_value());

    SECTION("std::exception from the current body") {
        REQUIRE(h.start_within([](OperationContext&) { throw std::runtime_error("boom"); }));
        REQUIRE(eventually([&] { return !h.op->running(); }));
        CHECK(h.op->last_failure() == std::optional<std::string>("boom"));
        h.op->cancel();
        CHECK(h.op->last_failure() == std::optional<std::string>("boom"));

        auto next = probe();
        h.op->start(waiting_body(next, 1h));
        CHECK_FALSE(h.op->last_failure().has_value());  // cleared as soon as start() returns
    }

    SECTION("a non-std::exception value") {
        REQUIRE(h.start_within([](OperationContext&) { throw 42; }));
        REQUIRE(eventually([&] { return !h.op->running(); }));
        CHECK(h.op->last_failure() == std::optional<std::string>("unknown exception"));
    }

    SECTION("a stale body's throw is not kept") {
        auto g = h.gate();
        auto stale = probe();
        REQUIRE(h.start_within([g, stale](OperationContext&) {
            stale->entered = true;
            g->wait();
            stale->exited = true;
            throw std::runtime_error("stale");
        }));
        REQUIRE(eventually([&] { return stale->entered.load(); }));
        REQUIRE(h.start_within([](OperationContext&) { throw std::runtime_error("current"); }));
        REQUIRE(eventually([&] { return !h.op->running(); }));
        CHECK(h.op->last_failure() == std::optional<std::string>("current"));
        g->open();
        h.op->cancel_all_and_join();  // the stale body has returned after this
        CHECK(stale->exited);
        CHECK(h.op->last_failure() == std::optional<std::string>("current"));
    }

    SECTION("a body taken by cancel_all_and_join() does not overwrite a later start()") {
        auto g = h.gate();
        auto taken = probe();
        REQUIRE(h.start_within([g, taken](OperationContext&) {
            taken->entered = true;
            g->wait();
            taken->exited = true;
            throw std::runtime_error("taken");
        }));
        REQUIRE(eventually([&] { return taken->entered.load(); }));
        auto joined = h.on_helper([&h] { h.op->cancel_all_and_join(); });
        // cancel_all_and_join() has taken the body once nothing is running;
        // it is now blocked joining it.
        REQUIRE(eventually([&] { return !h.op->running(); }));
        auto next = probe();
        REQUIRE(h.start_within(waiting_body(next, 1h)));
        CHECK_FALSE(h.op->last_failure().has_value());
        g->open();
        REQUIRE(joined.wait_for(kBound) == std::future_status::ready);
        CHECK(taken->exited);
        CHECK_FALSE(h.op->last_failure().has_value());  // the next start()'s outcome, not the taken body's throw
    }

    SECTION("a body taken by cancel_all_and_join() with no later start() keeps its failure") {
        auto g = h.gate();
        REQUIRE(h.start_within([g](OperationContext&) {
            g->wait();
            throw std::runtime_error("disconnect");
        }));
        auto joined = h.on_helper([&h] { h.op->cancel_all_and_join(); });
        REQUIRE(eventually([&] { return !h.op->running(); }));
        g->open();
        REQUIRE(joined.wait_for(kBound) == std::future_status::ready);
        CHECK(h.op->last_failure() == std::optional<std::string>("disconnect"));
    }
}

// Case 7: the destructor and cancel_all_and_join() wake and join every body
// with no time bound (rule 8).
TEST_CASE("AsyncOperation - destructor joins every body", "[util][async_operation][unit]") {
    Harness h;
    auto s1 = probe();
    auto s2 = probe();
    auto cur = probe();
    auto g1 = h.gate();
    auto g2 = h.gate();
    REQUIRE(h.start_within(gated_body(s1, g1)));
    REQUIRE(eventually([&] { return s1->entered.load(); }));
    REQUIRE(h.start_within(gated_body(s2, g2)));
    REQUIRE(eventually([&] { return s2->entered.load(); }));
    REQUIRE(h.start_within(waiting_body(cur, 1h)));
    REQUIRE(h.clock.wait_for_waiters(1, kBound));
    CHECK(h.op->stale_count() == 2);

    std::future<void> done;
    SECTION("destructor") {
        done = h.on_helper([op = std::move(h.op)]() mutable { op.reset(); });
    }
    SECTION("cancel_all_and_join()") {
        done = h.on_helper([&h] { h.op->cancel_all_and_join(); });
    }

    REQUIRE(eventually([&] { return cur->exited.load(); }));  // the current body was woken
    CHECK(cur->reason.load() == StopReason::Cancelled);
    CHECK(cur->wait_result == 0);
    CHECK(done.wait_for(100ms) == std::future_status::timeout);  // still joining the gated bodies
    CHECK_FALSE(s1->exited);
    CHECK_FALSE(s2->exited);

    g1->open();
    g2->open();
    REQUIRE(done.wait_for(kBound) == std::future_status::ready);
    CHECK(s1->exited);
    CHECK(s2->exited);
    CHECK(s1->reason.load() == StopReason::Superseded);
    CHECK(s2->reason.load() == StopReason::Superseded);

    if (h.op) {  // the cancel_all_and_join() section: the slot is empty and usable
        CHECK(h.op->stale_count() == 0);
        CHECK_FALSE(h.op->running());
        auto again = probe();
        REQUIRE(h.start_within(waiting_body(again, 10ms)));
        REQUIRE(eventually([&] { return again->entered.load(); }));
        REQUIRE(h.clock.wait_for_waiters(1, kBound));
        h.clock.advance(10ms);
        REQUIRE(eventually([&] { return again->exited.load(); }));
        CHECK(again->wait_result == 1);
    }
}

// Case 8: check, spawn and assign under one mutex (rule 1), with every public
// member racing. On the real clock, so this is the one case that sleeps.
//
// Tagged [stress-guard], not [stress]: this file is in the unconditional
// TEST_SOURCES, and scripts/check_stress_registration.py rejects [stress]
// outside a *_concurrency_stress.cpp file (#396). The sanitizers-tsan job runs
// "[stress-guard]" under ThreadSanitizer.
TEST_CASE("AsyncOperation - double-start storm", "[async_operation][stress-guard]") {
    OperationGeneration generation;
    std::atomic<long long> starts{0};
    std::atomic<long long> entered{0};
    std::atomic<long long> exited{0};
    std::atomic<std::size_t> max_stale{0};
    StressCallGuard guard({AlpacaError::NotConnected, AlpacaError::InvalidOperation});

    {
        AsyncOperation op("storm-slot", generation);  // default_task_clock()
        std::atomic<bool> stop{false};
        std::vector<std::thread> threads;
        for (int i = 0; i < 3; ++i) {
            threads.emplace_back([&] {
                while (!stop.load()) {
                    guard([&] {
                        op.start([&](OperationContext& ctx) {
                            entered.fetch_add(1);
                            for (int n = 0; n < 3 && ctx.wait_for(1ms); ++n) {
                            }
                            exited.fetch_add(1);
                        });
                        starts.fetch_add(1);
                    });
                    std::this_thread::yield();
                }
            });
        }
        threads.emplace_back([&] {
            while (!stop.load()) {
                guard([&] { op.cancel(); });
                guard([&] { static_cast<void>(op.running()); });
                guard([&] { static_cast<void>(op.last_failure()); });
                std::this_thread::yield();
            }
        });
        threads.emplace_back([&] {
            while (!stop.load()) {
                guard([&] {
                    const auto n = op.stale_count();
                    auto seen = max_stale.load();
                    while (n > seen && !max_stale.compare_exchange_weak(seen, n)) {
                    }
                });
                std::this_thread::yield();
            }
        });
        std::this_thread::sleep_for(500ms);
        stop.store(true);
        for (auto& t : threads) {
            t.join();
        }
    }

    CHECK(starts.load() > 0);
    CHECK(entered.load() == starts.load());
    CHECK(exited.load() == entered.load());
    CHECK(max_stale.load() <= AsyncOperation::kMaxStaleBodies);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}
