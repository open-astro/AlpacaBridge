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

// Tests for util::CaptureSession, one camera's exposure state, time, watchdog
// and kept failure over one util::AsyncOperation slot (issue #1605).
//
// Every case runs on a FakeTaskClock: time moves only through advance(), and
// every blocking step on the test thread has a real-time bound (a future
// waited for 2 s, or a yield loop with a 2 s deadline), so a defect fails the
// case instead of hanging the binary.
//
// "Blocked in the SDK" means a body waiting on a test-owned Gate that ignores
// the session's cancel, the shape of a vendor call blocked until its own
// timeout. The Harness opens every gate before it destroys the session.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/camera_driver.h>
#include <alpacacore/util/capture_session.h>
#include <alpacacore/util/error_handling.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
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

// Delivers a frame at once.
CaptureSession::Body instant_body(const ProbePtr& p, std::int32_t pixel) {
    return [p, pixel](CaptureControl& c) {
        c.exposure_started();
        p->entered = true;
        p->accepted = c.image_ready(frame(pixel)) ? 1 : 0;
        p->exited = true;
    };
}

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
