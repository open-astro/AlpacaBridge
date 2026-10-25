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

// Tests for util::CaptureSession (issue #1605). Every case runs on a
// FakeTaskClock and bounds each blocking step in real time, so a defect fails
// the case instead of hanging. "Blocked in the SDK" means a body waiting on a
// test-owned Gate that ignores the session's cancel; the Harness opens every
// gate before it destroys the session.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/camera_driver.h>
#include <alpacacore/util/capture_session.h>
#include <alpacacore/util/error_handling.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "fake_task_clock.h"

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::AlpacaException;
using alpacacore::CameraState;
using alpacacore::ImageArray;
using alpacacore::test::FakeTaskClock;
using alpacacore::util::CaptureControl;
using alpacacore::util::CaptureSession;
using alpacacore::util::StopReason;
using namespace std::chrono_literals;

namespace {

constexpr auto kBound = 2s;
constexpr auto kMargin = 2s;  // the watchdog margin every case uses

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

// The error code `fn` throws, or -1 when it returns.
int error_of(const std::function<void()>& fn, std::string* message = nullptr) {
    try {
        fn();
    } catch (const AlpacaException& e) {
        if (message != nullptr) {
            *message = e.what();
        }
        return e.error_code();
    }
    return -1;
}

// A test-owned gate: a body blocked in wait() ignores the session's cancel.
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
    std::atomic<int> wait_result{-1};  // -1 until the body's last wait returned
    std::atomic<StopReason> reason{StopReason::None};
    std::atomic<int> accepted{-1};  // -1 until the body offered its frame
};

using ProbePtr = std::shared_ptr<Probe>;
using GatePtr = std::shared_ptr<Gate>;

std::shared_ptr<const ImageArray> frame(std::int32_t first_pixel) {
    auto image = std::make_shared<ImageArray>();
    image->width = 2;
    image->height = 1;
    image->rank = 2;
    image->data = {first_pixel, 0};
    return image;
}

// Starts the SDK exposure, then polls the clock every 50 ms until stopped,
// and offers a frame at the end the way a late SDK return would.
CaptureSession::Body polling_body(const ProbePtr& p) {
    return [p](CaptureControl& c) {
        c.exposure_started();
        p->entered = true;
        bool more = true;
        while (more) {
            more = c.wait_for(50ms);
        }
        p->wait_result = 0;
        p->reason = c.stop_reason();
        p->accepted = c.image_ready(frame(7)) ? 1 : 0;
        p->exited = true;
    };
}

// Starts the SDK exposure, blocks in the SDK until the gate opens, then
// offers its frame.
CaptureSession::Body sdk_blocked_body(const ProbePtr& p, const GatePtr& g, std::int32_t pixel = 9) {
    return [p, g, pixel](CaptureControl& c) {
        c.exposure_started();
        p->entered = true;
        g->wait();
        p->reason = c.stop_reason();
        p->accepted = c.image_ready(frame(pixel)) ? 1 : 0;
        p->exited = true;
    };
}

// Waits on the clock until told to stop, then throws, the way the Player One
// download throws once POAStopExposure has run (fake_playerone_sdk.h,
// get_image_data).
CaptureSession::Body throws_when_stopped(const ProbePtr& p) {
    return [p](CaptureControl& c) {
        c.exposure_started();
        p->entered = true;
        while (c.wait_for(50ms)) {
        }
        p->reason = c.stop_reason();
        p->exited = true;
        throw std::runtime_error("late SDK failure after the stop");
    };
}

// Delivers a frame at once.
CaptureSession::Body instant_body(const ProbePtr& p, std::int32_t pixel) {
    return [p, pixel](CaptureControl& c) {
        c.exposure_started();
        p->entered = true;
        p->accepted = c.image_ready(frame(pixel)) ? 1 : 0;
        p->exited = true;
    };
}

// Waits 1 s on the clock once, then offers its frame.
CaptureSession::Body one_wait_body(const ProbePtr& p, std::int32_t pixel) {
    return [p, pixel](CaptureControl& c) {
        c.exposure_started();
        p->entered = true;
        p->wait_result = c.wait_for(1s) ? 1 : 0;
        p->reason = c.stop_reason();
        p->accepted = c.image_ready(frame(pixel)) ? 1 : 0;
        p->exited = true;
    };
}

// Captured by a body that arms it as its last step, it holds that body's
// thread after the body has returned, while the body's captures are dropped,
// until its gate opens. Unarmed (the body never ran) it holds nothing.
struct HoldOnDestroy {
    explicit HoldOnDestroy(GatePtr g) : gate(std::move(g)) {}
    HoldOnDestroy(const HoldOnDestroy&) = delete;
    HoldOnDestroy& operator=(const HoldOnDestroy&) = delete;
    ~HoldOnDestroy() {
        if (armed) {
            gate->wait();
        }
    }
    GatePtr gate;
    std::atomic<bool> armed{false};
};

// One fake clock and one session on it. The destructor opens every gate and
// only then destroys the session, so a failed case still tears down.
struct Harness {
    FakeTaskClock clock;
    std::unique_ptr<CaptureSession> session = std::make_unique<CaptureSession>("Test camera", clock, kMargin);
    std::vector<GatePtr> gates;

    GatePtr gate() {
        gates.push_back(std::make_shared<Gate>());
        return gates.back();
    }

    ~Harness() {
        for (auto& g : gates) {
            g->open();
        }
        session.reset();
    }

    // Runs `fn` on a helper thread and waits kBound of real time for it. If it
    // has not returned, advances the clock until it does so the case can
    // report, and returns false.
    bool returns_without_advance(const std::function<void()>& fn) {
        auto work = std::async(std::launch::async, fn);
        if (work.wait_for(kBound) == std::future_status::ready) {
            work.get();
            return true;
        }
        while (work.wait_for(1ms) != std::future_status::ready) {
            clock.advance(1s);
        }
        work.get();
        return false;
    }
};

}  // namespace

TEST_CASE("CaptureSession - start publishes Exposing and PercentCompleted follows the task clock",
          "[util][capture-session]") {
    Harness h;
    auto p = std::make_shared<Probe>();
    h.session->start(10.0, polling_body(p));

    CHECK(h.session->camera_state() == CameraState::Exposing);
    CHECK(h.session->busy());
    CHECK_FALSE(h.session->image_ready());
    REQUIRE(eventually([&] { return p->entered.load(); }));
    REQUIRE(h.clock.wait_for_waiters(1, kBound));
    CHECK(h.session->percent_completed() == Catch::Approx(0.0));

    h.clock.advance(5s);
    CHECK(h.session->percent_completed() == Catch::Approx(50.0));
    REQUIRE(h.clock.wait_for_waiters(1, kBound));
    h.clock.advance(5s);
    CHECK(h.session->percent_completed() == Catch::Approx(100.0));
    // 10 s + the 2 s margin: still exposing, never past 100 %.
    REQUIRE(h.clock.wait_for_waiters(1, kBound));
    h.clock.advance(1s);
    CHECK(h.session->camera_state() == CameraState::Exposing);
    CHECK(h.session->percent_completed() == Catch::Approx(100.0));
    h.session->abort();
}

TEST_CASE("CaptureSession - image_ready publishes the frame and commits the last exposure", "[util][capture-session]") {
    Harness h;
    auto p = std::make_shared<Probe>();
    auto sdk = h.gate();
    auto download = h.gate();

    CHECK(error_of([&] { (void)h.session->last_exposure_duration(); }) == AlpacaError::InvalidOperation);
    CHECK(error_of([&] { (void)h.session->last_exposure_start_time(); }) == AlpacaError::InvalidOperation);
    CHECK(error_of([&] { (void)h.session->image(); }) == AlpacaError::InvalidOperation);
    try {
        (void)h.session->image();
        FAIL("Expected AlpacaException");
    } catch (const AlpacaException& e) {
        CHECK(e.is_asked_too_early());
    }

    const auto before = std::chrono::system_clock::now();
    h.session->start(10.0, [p, sdk, download](CaptureControl& c) {
        c.exposure_started();
        p->entered = true;
        sdk->wait();
        c.reading_out();
        download->wait();
        p->accepted = c.image_ready(frame(42)) ? 1 : 0;
        p->exited = true;
    });
    REQUIRE(eventually([&] { return p->entered.load(); }));
    CHECK(error_of([&] { (void)h.session->image(); }) == AlpacaError::InvalidOperation);

    sdk->open();
    CHECK(eventually([&] { return h.session->camera_state() == CameraState::Download; }));
    CHECK(h.session->busy());
    CHECK_FALSE(h.session->image_ready());

    download->open();
    REQUIRE(eventually([&] { return p->exited.load(); }));
    CHECK(p->accepted == 1);
    CHECK(h.session->image_ready());
    CHECK(h.session->camera_state() == CameraState::Idle);
    CHECK(eventually([&] { return !h.session->busy(); }));
    REQUIRE(h.session->image() != nullptr);
    CHECK(h.session->image()->data.front() == 42);
    CHECK(h.session->last_exposure_duration() == Catch::Approx(10.0));
    CHECK(h.session->last_exposure_start_time() >= before);
    CHECK(h.session->last_exposure_start_time() <= std::chrono::system_clock::now());
    CHECK_FALSE(h.session->failure().has_value());
}

TEST_CASE("CaptureSession - abort wakes the body, discards the frame and keeps the last good exposure",
          "[util][capture-session]") {
    Harness h;
    auto first = std::make_shared<Probe>();
    h.session->start(0.5, instant_body(first, 5));
    REQUIRE(eventually([&] { return h.session->image_ready(); }));
    const auto first_start = h.session->last_exposure_start_time();

    auto p = std::make_shared<Probe>();
    h.session->start(10.0, polling_body(p));
    CHECK_FALSE(h.session->image_ready());  // a new start drops the previous frame
    REQUIRE(eventually([&] { return p->entered.load(); }));
    REQUIRE(h.clock.wait_for_waiters(1, kBound));

    // No advance: abort must wake the body's clock wait.
    CHECK(h.returns_without_advance([&] { h.session->abort(); }));
    CHECK(p->exited);
    CHECK(p->reason == StopReason::Cancelled);
    CHECK(p->accepted == 0);  // the aborted body's frame is dropped
    CHECK(h.session->camera_state() == CameraState::Idle);
    CHECK_FALSE(h.session->busy());
    CHECK_FALSE(h.session->image_ready());
    CHECK(error_of([&] { (void)h.session->image(); }) == AlpacaError::InvalidOperation);
    CHECK_FALSE(h.session->failure().has_value());
    CHECK(h.session->last_exposure_duration() == Catch::Approx(0.5));
    CHECK(h.session->last_exposure_start_time() == first_start);

    h.session->abort();  // idle: a no-op
    CHECK(h.session->camera_state() == CameraState::Idle);
}

TEST_CASE("CaptureSession - a body that throws fails the exposure until the next start", "[util][capture-session]") {
    Harness h;
    h.session->start(1.0, [](CaptureControl& c) {
        c.exposure_started();
        throw std::runtime_error("scripted SDK failure");
    });
    REQUIRE(eventually([&] { return h.session->failure().has_value(); }));
    CHECK(*h.session->failure() == "scripted SDK failure");
    CHECK(eventually([&] { return !h.session->busy(); }));
    // ASCOM: the completion property raises the failure; the camera is ready
    // for the next exposure (open choice 1 in the plan).
    CHECK(h.session->camera_state() == CameraState::Idle);
    std::string message;
    CHECK(error_of([&] { (void)h.session->image_ready(); }, &message) == AlpacaError::DriverException);
    CHECK(message.find("scripted SDK failure") != std::string::npos);
    CHECK(error_of([&] { (void)h.session->image(); }) == AlpacaError::DriverException);
    CHECK(error_of([&] { (void)h.session->last_exposure_duration(); }) == AlpacaError::InvalidOperation);

    h.session->abort();  // not exposing: the failure stays
    CHECK(error_of([&] { (void)h.session->image_ready(); }) == AlpacaError::DriverException);

    auto p = std::make_shared<Probe>();
    h.session->start(1.0, polling_body(p));
    CHECK_FALSE(h.session->failure().has_value());
    CHECK(error_of([&] { (void)h.session->image_ready(); }) == -1);
    CHECK(h.session->camera_state() == CameraState::Exposing);
    h.session->abort();
}

TEST_CASE("CaptureSession - the watchdog fails an exposure stuck in the SDK and ignores its late frame",
          "[util][capture-session]") {
    Harness h;
    auto p = std::make_shared<Probe>();
    auto sdk = h.gate();
    h.session->start(1.0, sdk_blocked_body(p, sdk));
    REQUIRE(eventually([&] { return p->entered.load(); }));

    h.clock.advance(2999ms);  // 1 s exposure + 2 s margin, less 1 ms
    CHECK(h.session->camera_state() == CameraState::Exposing);
    CHECK_FALSE(h.session->failure().has_value());
    h.clock.advance(1ms);
    CHECK(h.session->camera_state() == CameraState::Idle);
    REQUIRE(h.session->failure().has_value());
    CHECK(error_of([&] { (void)h.session->image_ready(); }) == AlpacaError::DriverException);
    CHECK(h.session->busy());  // the body is still in the SDK

    sdk->open();
    REQUIRE(eventually([&] { return p->exited.load(); }));
    CHECK(p->accepted == 0);  // the late frame is not published
    CHECK(error_of([&] { (void)h.session->image_ready(); }) == AlpacaError::DriverException);
    CHECK(error_of([&] { (void)h.session->image(); }) == AlpacaError::DriverException);
    CHECK(eventually([&] { return !h.session->busy(); }));
    CHECK(error_of([&] { (void)h.session->last_exposure_duration(); }) == AlpacaError::InvalidOperation);
}

TEST_CASE("CaptureSession - the watchdog ends a body that waits on the clock", "[util][capture-session]") {
    Harness h;
    auto p = std::make_shared<Probe>();
    h.session->start(1.0, [p](CaptureControl& c) {
        c.exposure_started();
        p->entered = true;
        p->wait_result = c.wait_for(10s) ? 1 : 0;  // longer than the deadline
        p->reason = c.stop_reason();
        p->exited = true;
    });
    REQUIRE(eventually([&] { return p->entered.load(); }));
    REQUIRE(h.clock.wait_for_waiters(1, kBound));

    h.clock.advance(3s);  // the deadline; no getter is read first
    REQUIRE(eventually([&] { return p->exited.load(); }));
    CHECK(p->wait_result == 0);
    CHECK(p->reason == StopReason::Cancelled);
    CHECK(h.session->failure().has_value());
    CHECK(h.session->camera_state() == CameraState::Idle);
    CHECK(error_of([&] { (void)h.session->image_ready(); }) == AlpacaError::DriverException);
}

TEST_CASE("CaptureSession - start while exposing is refused and leaves the exposure running",
          "[util][capture-session]") {
    Harness h;
    auto p = std::make_shared<Probe>();
    h.session->start(10.0, polling_body(p));
    REQUIRE(eventually([&] { return p->entered.load(); }));

    auto second = std::make_shared<Probe>();
    CHECK(error_of([&] { h.session->start(1.0, instant_body(second, 1)); }) == AlpacaError::InvalidOperation);
    CHECK_FALSE(second->entered);
    CHECK(h.session->camera_state() == CameraState::Exposing);
    CHECK_FALSE(p->exited);
    h.session->abort();
    CHECK(p->reason == StopReason::Cancelled);
}

TEST_CASE("CaptureSession - start waits for a body still in the SDK and starts once it returns",
          "[util][capture-session]") {
    Harness h;
    auto p = std::make_shared<Probe>();
    auto sdk = h.gate();
    h.session->start(1.0, sdk_blocked_body(p, sdk));
    REQUIRE(eventually([&] { return p->entered.load(); }));
    h.clock.advance(3s);  // the watchdog fails it; the body stays in the SDK
    REQUIRE(h.session->failure().has_value());

    auto next = std::make_shared<Probe>();
    auto work = std::async(std::launch::async, [&] { h.session->start(1.0, instant_body(next, 3)); });
    REQUIRE(h.clock.wait_for_waiters(1, kBound));  // start's bounded wait
    CHECK_FALSE(next->entered);                    // never two bodies on one camera
    sdk->open();                                   // the old body returns
    REQUIRE(work.wait_for(kBound) == std::future_status::ready);
    work.get();
    REQUIRE(eventually([&] { return h.session->image_ready(); }));
    CHECK(h.session->image()->data.front() == 3);
    CHECK(p->accepted == 0);
}

TEST_CASE("CaptureSession - start refuses after kStartTimeout when the old body stays in the SDK",
          "[util][capture-session]") {
    Harness h;
    auto p = std::make_shared<Probe>();
    auto sdk = h.gate();
    h.session->start(1.0, sdk_blocked_body(p, sdk));
    REQUIRE(eventually([&] { return p->entered.load(); }));
    h.clock.advance(3s);
    REQUIRE(h.session->failure().has_value());
    const auto failure = *h.session->failure();

    auto next = std::make_shared<Probe>();
    auto work = std::async(std::launch::async, [&] { h.session->start(1.0, instant_body(next, 3)); });
    REQUIRE(h.clock.wait_for_waiters(1, kBound));
    h.clock.advance(CaptureSession::kStartTimeout);
    REQUIRE(work.wait_for(kBound) == std::future_status::ready);
    CHECK(error_of([&] { work.get(); }) == AlpacaError::InvalidOperation);
    // A refused start changes nothing.
    CHECK_FALSE(next->entered);
    REQUIRE(h.session->failure().has_value());
    CHECK(*h.session->failure() == failure);
    CHECK(h.session->busy());

    sdk->open();
    REQUIRE(eventually([&] { return !h.session->busy(); }));
    CHECK(error_of([&] { h.session->start(1.0, instant_body(next, 4)); }) == -1);
    CHECK(eventually([&] { return h.session->image_ready(); }));
}

TEST_CASE("CaptureSession - two starts over a body still in the SDK start one body and getters never wait",
          "[util][capture-session]") {
    Harness h;
    auto stale = std::make_shared<Probe>();
    auto sdk = h.gate();
    h.session->start(1.0, sdk_blocked_body(stale, sdk));
    REQUIRE(eventually([&] { return stale->entered.load(); }));
    h.clock.advance(3s);  // the watchdog fails it; the body stays in the SDK
    REQUIRE(h.session->failure().has_value());

    auto a = std::make_shared<Probe>();
    auto b = std::make_shared<Probe>();
    auto start_a =
        std::async(std::launch::async, [&] { return error_of([&] { h.session->start(10.0, polling_body(a)); }); });
    auto start_b =
        std::async(std::launch::async, [&] { return error_of([&] { h.session->start(10.0, polling_body(b)); }); });
    const auto ready = [](std::future<int>& f) { return f.wait_for(0s) == std::future_status::ready; };
    REQUIRE(h.clock.wait_for_waiters(1, kBound));
    // Both wait on the clock, or one was refused at once: either way the
    // session is settled before the getters are read.
    REQUIRE(eventually([&] { return h.clock.waiter_count() >= 2 || ready(start_a) || ready(start_b); }));

    // Getters while start() waits: none waits for it (decision 0016 rule 1),
    // and a start that has not started changes nothing.
    CameraState state = CameraState::Exposing;
    int ready_error = -1;
    bool busy = false;
    CHECK(h.returns_without_advance([&] {
        state = h.session->camera_state();
        ready_error = error_of([&] { (void)h.session->image_ready(); });
        busy = h.session->busy();
        (void)h.session->percent_completed();
        (void)h.session->failure();
    }));
    CHECK(state == CameraState::Idle);
    CHECK(ready_error == AlpacaError::DriverException);
    CHECK(busy);
    CHECK_FALSE(a->entered);
    CHECK_FALSE(b->entered);

    sdk->open();  // the stale body returns
    REQUIRE(start_a.wait_for(kBound) == std::future_status::ready);
    REQUIRE(start_b.wait_for(kBound) == std::future_status::ready);
    const int result_a = start_a.get();
    const int result_b = start_b.get();
    // Exactly one start succeeds; the other sees it exposing and is refused.
    CHECK(((result_a == -1 && result_b == AlpacaError::InvalidOperation) ||
           (result_b == -1 && result_a == AlpacaError::InvalidOperation)));
    CHECK(eventually([&] { return a->entered.load() || b->entered.load(); }));
    CHECK(a->entered.load() != b->entered.load());  // never two bodies on one camera
    CHECK(stale->accepted == 0);
    CHECK(h.session->camera_state() == CameraState::Exposing);
    h.session->abort();
}

TEST_CASE("CaptureSession - disconnect_and_join cancels the body and clears the session", "[util][capture-session]") {
    Harness h;
    auto first = std::make_shared<Probe>();
    h.session->start(0.5, instant_body(first, 5));
    REQUIRE(eventually([&] { return h.session->image_ready(); }));

    auto p = std::make_shared<Probe>();
    h.session->start(10.0, polling_body(p));
    REQUIRE(eventually([&] { return p->entered.load(); }));
    REQUIRE(h.clock.wait_for_waiters(1, kBound));

    CHECK(h.returns_without_advance([&] { h.session->disconnect_and_join(); }));
    CHECK(p->exited);
    CHECK(p->reason == StopReason::Cancelled);
    CHECK(p->accepted == 0);
    CHECK(h.session->camera_state() == CameraState::Idle);
    CHECK_FALSE(h.session->busy());
    CHECK_FALSE(h.session->image_ready());
    CHECK(error_of([&] { (void)h.session->image(); }) == AlpacaError::InvalidOperation);
    CHECK(error_of([&] { (void)h.session->last_exposure_duration(); }) == AlpacaError::InvalidOperation);

    SECTION("a kept failure is cleared too") {
        h.session->open();  // the next connect
        h.session->start(1.0, [](CaptureControl&) { throw std::runtime_error("scripted SDK failure"); });
        REQUIRE(eventually([&] { return h.session->failure().has_value(); }));
        h.session->disconnect_and_join();
        CHECK_FALSE(h.session->failure().has_value());
        CHECK(error_of([&] { (void)h.session->image_ready(); }) == -1);
    }
}

TEST_CASE("CaptureSession - discard_image drops a ready frame and keeps the last exposure", "[util][capture-session]") {
    Harness h;
    auto p = std::make_shared<Probe>();
    h.session->start(0.5, instant_body(p, 5));
    REQUIRE(eventually([&] { return h.session->image_ready(); }));
    REQUIRE(eventually([&] { return !h.session->busy(); }));

    h.session->discard_image();
    CHECK_FALSE(h.session->image_ready());
    CHECK(error_of([&] { (void)h.session->image(); }) == AlpacaError::InvalidOperation);
    CHECK(h.session->camera_state() == CameraState::Idle);
    CHECK(h.session->last_exposure_duration() == Catch::Approx(0.5));
}

TEST_CASE("CaptureSession - fail ends the exposure with the driver's failure and cancels the body",
          "[util][capture-session]") {
    Harness h;
    auto p = std::make_shared<Probe>();
    h.session->start(10.0, polling_body(p));
    REQUIRE(eventually([&] { return p->entered.load(); }));
    REQUIRE(h.clock.wait_for_waiters(1, kBound));

    h.session->fail("scripted stop failure");
    REQUIRE(eventually([&] { return p->exited.load(); }));  // no advance: the body is woken
    CHECK(p->reason == StopReason::Cancelled);
    CHECK(p->accepted == 0);
    CHECK(h.session->camera_state() == CameraState::Idle);
    REQUIRE(h.session->failure().has_value());
    CHECK(*h.session->failure() == "scripted stop failure");
    CHECK(error_of([&] { (void)h.session->image_ready(); }) == AlpacaError::DriverException);

    h.session->fail("a later failure");  // not exposing: a no-op
    CHECK(*h.session->failure() == "scripted stop failure");
}

TEST_CASE("CaptureSession - a start during an abort's join waits for it and its body is not cancelled",
          "[util][capture-session]") {
    Harness h;
    auto a = std::make_shared<Probe>();
    auto held = h.gate();
    auto hold = std::make_shared<HoldOnDestroy>(held);
    h.session->start(10.0, [a, hold](CaptureControl& c) {
        c.exposure_started();
        a->entered = true;
        while (c.wait_for(50ms)) {
        }
        a->reason = c.stop_reason();
        hold->armed = true;
        a->exited = true;
    });
    hold.reset();  // the body holds the only reference
    REQUIRE(eventually([&] { return a->entered.load(); }));
    REQUIRE(h.clock.wait_for_waiters(1, kBound));

    auto aborting = std::async(std::launch::async, [&] { h.session->abort(); });
    // The cancel wakes the body and it returns; its thread then waits while
    // its captures are dropped, so abort's join has not returned.
    REQUIRE(eventually([&] { return a->exited.load(); }));
    CHECK(a->reason == StopReason::Cancelled);
    CHECK(aborting.wait_for(0s) == std::future_status::timeout);

    auto b = std::make_shared<Probe>();
    auto starting =
        std::async(std::launch::async, [&] { return error_of([&] { h.session->start(10.0, one_wait_body(b, 11)); }); });
    // An abort counts as pending until its join returns: the start waits on
    // the clock instead of starting a body that join could still cancel.
    CHECK(h.clock.wait_for_waiters(1, kBound));
    CHECK_FALSE(b->entered);
    CHECK(h.session->camera_state() == CameraState::Idle);

    held->open();
    REQUIRE(aborting.wait_for(kBound) == std::future_status::ready);
    aborting.get();
    REQUIRE(starting.wait_for(kBound) == std::future_status::ready);
    CHECK(starting.get() == -1);
    REQUIRE(eventually([&] { return b->entered.load(); }));
    CHECK(h.session->camera_state() == CameraState::Exposing);
    REQUIRE(h.clock.wait_for_waiters(1, kBound));
    h.clock.advance(1s);
    REQUIRE(eventually([&] { return b->exited.load(); }));
    CHECK(b->wait_result == 1);
    CHECK(b->reason == StopReason::None);  // the earlier abort never reached it
    CHECK(b->accepted == 1);
    CHECK(h.session->image_ready());
    CHECK(h.session->image()->data.front() == 11);
}

TEST_CASE("CaptureSession - disconnect_and_join closes the session: start raises NotConnected until open",
          "[util][capture-session]") {
    Harness h;
    auto first = std::make_shared<Probe>();
    h.session->start(0.5, instant_body(first, 5));
    REQUIRE(eventually([&] { return h.session->image_ready(); }));
    REQUIRE(eventually([&] { return !h.session->busy(); }));
    h.session->disconnect_and_join();

    auto late = std::make_shared<Probe>();
    std::string message;
    CHECK(error_of([&] { h.session->start(1.0, instant_body(late, 6)); }, &message) == AlpacaError::NotConnected);
    CHECK_FALSE(message.empty());
    // A refused start publishes nothing and runs no body.
    CHECK(h.session->camera_state() == CameraState::Idle);
    CHECK_FALSE(h.session->busy());
    CHECK_FALSE(h.session->image_ready());
    CHECK_FALSE(late->entered);
    h.session->abort();                // closed and idle: a no-op
    h.session->disconnect_and_join();  // already closed: stays closed
    CHECK(error_of([&] { h.session->start(1.0, instant_body(late, 6)); }) == AlpacaError::NotConnected);
    CHECK_FALSE(late->entered);

    h.session->open();
    h.session->open();  // already open: a no-op
    CHECK(error_of([&] { h.session->start(1.0, instant_body(late, 6)); }) == -1);
    REQUIRE(eventually([&] { return late->exited.load(); }));
    CHECK(late->accepted == 1);
    CHECK(h.session->image_ready());
    CHECK(h.session->image()->data.front() == 6);
}

TEST_CASE("CaptureSession - disconnect_and_join during an abort's join returns only once that body has returned",
          "[util][capture-session]") {
    Harness h;
    auto a = std::make_shared<Probe>();
    auto after_stop = h.gate();  // the SDK call the body makes after the stop
    h.session->start(10.0, [a, after_stop](CaptureControl& c) {
        c.exposure_started();
        a->entered = true;
        while (c.wait_for(50ms)) {
        }
        a->reason = c.stop_reason();
        a->wait_result = 0;
        after_stop->wait();
        a->exited = true;
    });
    REQUIRE(eventually([&] { return a->entered.load(); }));
    REQUIRE(h.clock.wait_for_waiters(1, kBound));

    auto aborting = std::async(std::launch::async, [&] { h.session->abort(); });
    REQUIRE(eventually([&] { return a->wait_result.load() == 0; }));  // abort's join now waits for it
    auto disconnecting = std::async(std::launch::async, [&] { h.session->disconnect_and_join(); });
    // A disconnect that saw the slot already emptied by abort() must still
    // wait for that join: the driver closes the camera when it returns
    // (docs/failures/0011).
    CHECK(disconnecting.wait_for(100ms) == std::future_status::timeout);
    CHECK_FALSE(a->exited);

    after_stop->open();
    REQUIRE(aborting.wait_for(kBound) == std::future_status::ready);
    aborting.get();
    REQUIRE(disconnecting.wait_for(kBound) == std::future_status::ready);
    disconnecting.get();
    CHECK(a->exited);
    CHECK(a->reason == StopReason::Cancelled);
    CHECK(h.session->camera_state() == CameraState::Idle);
    CHECK_FALSE(h.session->busy());
    CHECK(error_of([&] { h.session->start(1.0, instant_body(a, 1)); }) == AlpacaError::NotConnected);
}

TEST_CASE("CaptureSession - a start waiting when disconnect_and_join begins is refused and never runs after the clear",
          "[util][capture-session]") {
    Harness h;
    auto stale = std::make_shared<Probe>();
    auto sdk = h.gate();
    h.session->start(1.0, sdk_blocked_body(stale, sdk));
    REQUIRE(eventually([&] { return stale->entered.load(); }));
    h.clock.advance(3s);  // the watchdog fails it; the body stays in the SDK
    REQUIRE(h.session->failure().has_value());

    auto next = std::make_shared<Probe>();
    auto starting =
        std::async(std::launch::async, [&] { return error_of([&] { h.session->start(10.0, polling_body(next)); }); });
    REQUIRE(h.clock.wait_for_waiters(1, kBound));  // start's bounded wait
    auto disconnecting = std::async(std::launch::async, [&] { h.session->disconnect_and_join(); });
    // The close ends that wait at once, while the old body is still in the
    // SDK and the disconnect still joins it.
    CHECK(starting.wait_for(kBound) == std::future_status::ready);
    CHECK(disconnecting.wait_for(0s) == std::future_status::timeout);
    // No reopen while that disconnect still joins: its clear would wipe the
    // new connection's exposure.
    CHECK(error_of([&] { h.session->open(); }) == AlpacaError::InvalidOperation);

    sdk->open();  // the old body returns; the join ends
    REQUIRE(disconnecting.wait_for(kBound) == std::future_status::ready);
    disconnecting.get();
    REQUIRE(starting.wait_for(kBound) == std::future_status::ready);
    CHECK(starting.get() == AlpacaError::NotConnected);
    CHECK_FALSE(next->entered);  // no body after the clear: the driver closes the camera next
    CHECK(stale->accepted == 0);
    CHECK(h.session->camera_state() == CameraState::Idle);
    CHECK_FALSE(h.session->busy());
    CHECK_FALSE(h.session->failure().has_value());
    CHECK(error_of([&] { h.session->start(1.0, instant_body(next, 2)); }) == AlpacaError::NotConnected);
}

TEST_CASE("CaptureSession - a stale body's late throw is never kept", "[util][capture-session]") {
    Harness h;
    auto p = std::make_shared<Probe>();

    SECTION("after abort") {
        h.session->start(10.0, throws_when_stopped(p));
        REQUIRE(eventually([&] { return p->entered.load(); }));
        REQUIRE(h.clock.wait_for_waiters(1, kBound));
        h.session->abort();  // joins: the throw has happened when it returns
        CHECK(p->exited);
        CHECK(p->reason == StopReason::Cancelled);
        CHECK_FALSE(h.session->failure().has_value());
        CHECK(error_of([&] { CHECK_FALSE(h.session->image_ready()); }) == -1);
        CHECK(h.session->camera_state() == CameraState::Idle);
    }

    SECTION("after fail") {
        h.session->start(10.0, throws_when_stopped(p));
        REQUIRE(eventually([&] { return p->entered.load(); }));
        REQUIRE(h.clock.wait_for_waiters(1, kBound));
        h.session->fail("scripted stop failure");
        REQUIRE(eventually([&] { return p->exited.load() && !h.session->busy(); }));
        REQUIRE(h.session->failure().has_value());
        CHECK(*h.session->failure() == "scripted stop failure");
        std::string message;
        CHECK(error_of([&] { (void)h.session->image_ready(); }, &message) == AlpacaError::DriverException);
        CHECK(message.find("late SDK failure") == std::string::npos);
    }

    SECTION("after the watchdog") {
        auto sdk = h.gate();
        h.session->start(1.0, [p, sdk](CaptureControl& c) {
            c.exposure_started();
            p->entered = true;
            sdk->wait();
            p->exited = true;
            throw std::runtime_error("late SDK failure after the stop");
        });
        REQUIRE(eventually([&] { return p->entered.load(); }));
        h.clock.advance(3s);  // 1 s exposure + 2 s margin
        REQUIRE(h.session->failure().has_value());
        const auto deadline_failure = *h.session->failure();
        CHECK(deadline_failure.find("late SDK failure") == std::string::npos);

        sdk->open();
        REQUIRE(eventually([&] { return p->exited.load() && !h.session->busy(); }));
        REQUIRE(h.session->failure().has_value());
        CHECK(*h.session->failure() == deadline_failure);
    }

    SECTION("after disconnect_and_join") {
        h.session->start(10.0, throws_when_stopped(p));
        REQUIRE(eventually([&] { return p->entered.load(); }));
        REQUIRE(h.clock.wait_for_waiters(1, kBound));
        h.session->disconnect_and_join();
        CHECK(p->exited);
        CHECK(p->reason == StopReason::Cancelled);
        CHECK_FALSE(h.session->failure().has_value());
        CHECK(error_of([&] { CHECK_FALSE(h.session->image_ready()); }) == -1);
    }
}

// Tagged [stress-guard]: the sanitizers-tsan job runs it under ThreadSanitizer.
TEST_CASE("CaptureSession - destruction joins a body blocked in the SDK once it returns",
          "[util][capture-session][stress-guard]") {
    FakeTaskClock clock;
    auto session = std::make_unique<CaptureSession>("Test camera", clock, kMargin);
    auto p = std::make_shared<Probe>();
    auto sdk = std::make_shared<Gate>();
    session->start(1.0, sdk_blocked_body(p, sdk));
    REQUIRE(eventually([&] { return p->entered.load(); }));

    auto work = std::async(std::launch::async, [&] { session.reset(); });
    CHECK(work.wait_for(100ms) == std::future_status::timeout);  // it joins, never detaches
    sdk->open();
    REQUIRE(work.wait_for(kBound) == std::future_status::ready);
    work.get();
    CHECK(p->exited);
}

// Tagged [stress-guard]: the sanitizers-tsan job runs it under ThreadSanitizer,
// which reports a data race, or a lock-order inversion between `driver` (a
// test mutex standing in for the driver's mutex_), the session mutex and the
// slot mutex. Getters, fail(), discard_image() and open() run under
// `driver`, and so does a body's publish, as the driver calls them; start(),
// abort() and disconnect_and_join() run without it (decision 0006 rule 4).
// disconnect_and_join() runs on its own thread, racing abort()'s join: when
// it returns, no body may still be inside (docs/failures/0011).
TEST_CASE("CaptureSession - lock-order storm: getters under the driver mutex race every session call",
          "[util][capture-session][stress-guard]") {
    struct Shared {
        std::mutex driver;
        std::atomic<bool> done{false};
        std::atomic<int> starts{0};
        std::atomic<int> bodies{0};
        std::atomic<int> inside{0};  // bodies that have entered and not returned
        std::atomic<int> inside_after_disconnect{0};
        std::atomic<long> seen{0};
    };
    struct Inside {
        explicit Inside(std::atomic<int>& n) : count(n) { ++count; }
        Inside(const Inside&) = delete;
        Inside& operator=(const Inside&) = delete;
        ~Inside() { --count; }
        std::atomic<int>& count;
    };
    Shared s;  // declared before the Harness, so it outlives every body
    Harness h;

    const auto body = [&s](int kind) -> CaptureSession::Body {
        return [&s, kind](CaptureControl& c) {
            const Inside inside(s.inside);
            c.exposure_started();
            ++s.bodies;
            if (kind == 0) {  // until abort, fail or the watchdog
                while (c.wait_for(20ms)) {
                }
                // The SDK call after the stop takes a while, so joins overlap.
                for (int i = 0; i < 200; ++i) {
                    std::this_thread::yield();
                }
                return;
            }
            (void)c.wait_for(20ms);
            if (kind == 1) {
                throw std::runtime_error("scripted storm failure");
            }
            std::lock_guard<std::mutex> lock(s.driver);
            c.reading_out();
            (void)c.image_ready(frame(kind));
        };
    };

    auto storm = std::async(std::launch::async, [&] {
        std::vector<std::thread> threads;
        for (int i = 0; i < 2; ++i) {
            threads.emplace_back([&] {
                // Every result feeds `seen`, so no read is optimised away.
                while (!s.done) {
                    std::lock_guard<std::mutex> lock(s.driver);
                    int seen = static_cast<int>(h.session->camera_state());
                    seen += h.session->busy() ? 1 : 0;
                    seen += error_of([&] { seen += h.session->image_ready() ? 1 : 0; });
                    seen += error_of([&] {
                        const auto image = h.session->image();
                        seen += image ? image->data.front() : 0;
                    });
                    seen += static_cast<int>(h.session->percent_completed());
                    seen += h.session->failure().has_value() ? 1 : 0;
                    seen += error_of([&] { seen += static_cast<int>(h.session->last_exposure_duration()); });
                    s.seen += seen;
                }
            });
        }
        threads.emplace_back([&] {
            for (int i = 0; !s.done; ++i) {
                if (i % 2 == 0) {
                    h.session->abort();
                } else {
                    std::lock_guard<std::mutex> lock(s.driver);
                    h.session->fail("scripted storm stop failure");
                    h.session->discard_image();
                }
                std::this_thread::yield();
            }
        });
        threads.emplace_back([&] {  // a disconnect, then the next connect, every few bodies
            int last = 0;
            while (!s.done) {
                if (s.bodies < last + 3) {
                    std::this_thread::yield();
                    continue;
                }
                last = s.bodies;
                h.session->disconnect_and_join();
                if (s.inside != 0) {
                    ++s.inside_after_disconnect;
                }
                std::lock_guard<std::mutex> lock(s.driver);
                h.session->open();
            }
        });
        threads.emplace_back([&] {  // moves the watchdog and start()'s bounded wait
            while (!s.done) {
                h.clock.advance(50ms);
                std::this_thread::yield();
            }
        });
        for (int i = 0; i < 20000 && s.bodies < 300; ++i) {
            if (error_of([&] { h.session->start(0.05, body(i % 3)); }) == -1) {
                ++s.starts;
            }
        }
        s.done = true;
        for (auto& t : threads) {
            t.join();
        }
    });
    if (storm.wait_for(30s) != std::future_status::ready) {
        // A deadlock: its threads still use the session, so the case cannot
        // unwind; end the binary instead of hanging it.
        std::fputs("CaptureSession lock-order storm deadlocked\n", stderr);
        std::abort();
    }
    storm.get();
    CHECK(s.starts > 0);
    CHECK(s.bodies > 0);
    CHECK(s.inside_after_disconnect == 0);

    h.session->disconnect_and_join();
    CHECK_FALSE(h.session->busy());
    h.session->open();
    auto p = std::make_shared<Probe>();
    h.session->start(10.0, polling_body(p));
    CHECK(h.session->camera_state() == CameraState::Exposing);
    h.session->abort();
}

namespace {
using Spawn = std::function<std::thread(std::function<void()>)>;
Spawn plain_spawn() {
    return [](std::function<void()> f) { return std::thread(std::move(f)); };
}
// Holds the slot thread on `g` before it runs `f`.
Spawn held_spawn(const GatePtr& g) {
    return [g](std::function<void()> f) {
        return std::thread([g, f = std::move(f)]() mutable {
            g->wait();
            f();
        });
    };
}
}  // namespace

TEST_CASE("CaptureSession - a start reserved before a close or an abort never runs its body",
          "[util][capture-session]") {
    Harness h;
    auto p = std::make_shared<Probe>();
    auto held = h.gate();
    h.session->set_spawn_for_testing(held_spawn(held));
    h.session->start(10.0, polling_body(p));  // reserved; its thread waits before run_body

    SECTION("disconnect_and_join") {
        auto closing = std::async(std::launch::async, [&] { h.session->disconnect_and_join(); });
        REQUIRE(eventually(
            [&] { return error_of([&] { h.session->start(1.0, polling_body(p)); }) == AlpacaError::NotConnected; }));
        held->open();
        REQUIRE(closing.wait_for(kBound) == std::future_status::ready);
        closing.get();
    }
    SECTION("abort") {
        auto aborting = std::async(std::launch::async, [&] { h.session->abort(); });
        REQUIRE(eventually([&] { return h.session->camera_state() == CameraState::Idle; }));
        held->open();
        REQUIRE(aborting.wait_for(kBound) == std::future_status::ready);
        aborting.get();
    }
    CHECK_FALSE(p->entered);
    CHECK(h.session->camera_state() == CameraState::Idle);
    CHECK_FALSE(h.session->busy());
    CHECK_FALSE(h.session->failure().has_value());
}

TEST_CASE("CaptureSession - a refused body thread rolls the start back and the next start runs",
          "[util][capture-session]") {
    Harness h;
    auto p = std::make_shared<Probe>();
    h.session->start(1.0, instant_body(p, 5));
    REQUIRE(eventually([&] { return h.session->image_ready(); }));
    auto earlier = h.session->image();
    REQUIRE(eventually([&] { return !h.session->busy(); }));

    SECTION("a refused thread") {
        h.session->set_spawn_for_testing([](std::function<void()>) -> std::thread {
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        });
        CHECK(error_of([&] { h.session->start(1.0, polling_body(p)); }) == AlpacaError::DriverException);
    }
    SECTION("bad_alloc") {
        h.session->set_spawn_for_testing([](std::function<void()>) -> std::thread { throw std::bad_alloc(); });
        CHECK_THROWS_AS(h.session->start(1.0, polling_body(p)), std::bad_alloc);
    }
    CHECK_FALSE(h.session->busy());
    CHECK(h.session->image_ready());  // the earlier frame, not Exposing
    CHECK(h.session->image() == earlier);
    CHECK(h.session->camera_state() == CameraState::Idle);

    h.session->set_spawn_for_testing(plain_spawn());
    auto q = std::make_shared<Probe>();
    h.session->start(1.0, instant_body(q, 6));
    REQUIRE(eventually([&] { return q->exited.load(); }));
}

TEST_CASE("CaptureSession - a refused body thread keeps the earlier failure", "[util][capture-session]") {
    Harness h;
    h.session->start(10.0, [](CaptureControl&) { throw std::runtime_error("earlier failure"); });
    REQUIRE(eventually([&] { return h.session->failure().has_value() && !h.session->busy(); }));
    h.session->set_spawn_for_testing([](std::function<void()>) -> std::thread {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    });
    CHECK(error_of([&] { h.session->start(1.0, [](CaptureControl&) {}); }) == AlpacaError::DriverException);
    REQUIRE(h.session->failure().has_value());
    CHECK(*h.session->failure() == "earlier failure");
    CHECK_FALSE(h.session->busy());
}

TEST_CASE("CaptureSession - a stale body's reading_out does not move the session", "[util][capture-session]") {
    Harness h;
    auto sdk = h.gate();
    auto p = std::make_shared<Probe>();
    auto body = [p, sdk](CaptureControl& c) {
        c.exposure_started();
        p->entered = true;
        sdk->wait();
        c.reading_out();
        p->exited = true;
    };

    SECTION("after fail") {
        h.session->start(10.0, body);
        REQUIRE(eventually([&] { return p->entered.load(); }));
        h.session->fail("x");
        sdk->open();
        REQUIRE(eventually([&] { return p->exited.load() && !h.session->busy(); }));
        REQUIRE(h.session->failure().has_value());
        CHECK(*h.session->failure() == "x");
    }
    SECTION("after abort") {
        h.session->start(10.0, body);
        REQUIRE(eventually([&] { return p->entered.load(); }));
        auto aborting = std::async(std::launch::async, [&] { h.session->abort(); });
        REQUIRE(eventually([&] { return h.session->camera_state() == CameraState::Idle; }));
        sdk->open();
        REQUIRE(aborting.wait_for(kBound) == std::future_status::ready);
        aborting.get();
        CHECK_FALSE(h.session->failure().has_value());
    }
    CHECK(h.session->camera_state() == CameraState::Idle);
}

TEST_CASE("CaptureSession - a refused body thread does not restore state over a close", "[util][capture-session]") {
    Harness h;
    auto p = std::make_shared<Probe>();
    h.session->start(1.0, instant_body(p, 5));
    REQUIRE(eventually([&] { return h.session->image_ready() && !h.session->busy(); }));

    std::future<void> closing;
    h.session->set_spawn_for_testing([&](std::function<void()>) -> std::thread {
        closing = std::async(std::launch::async, [&] { h.session->disconnect_and_join(); });
        REQUIRE(eventually([&] {
            return error_of([&] { h.session->start(1.0, [](CaptureControl&) {}); }) == AlpacaError::NotConnected;
        }));
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    });
    CHECK(error_of([&] { h.session->start(1.0, polling_body(p)); }) == AlpacaError::DriverException);
    REQUIRE(closing.wait_for(kBound) == std::future_status::ready);
    closing.get();
    CHECK_FALSE(h.session->image_ready());  // the closed session stays cleared
    CHECK_FALSE(h.session->busy());
}
