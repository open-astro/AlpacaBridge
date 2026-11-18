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

#pragma once

// A scripted OnStepX controller (the telescope, its focusers, its rotator and its auxiliary features) on a loopback
// FakeMountServer, the Network transport, for the focuser, rotator, switch and shared-link cases (#2223, #2221,
// #2222): test_onstep_focuser.cpp, test_onstep_rotator.cpp, test_onstep_switch.cpp and the [stress] registrations in
// test_onstep_concurrency_stress.cpp. Replies follow OnStepX Focuser.command.cpp, Rotator.command.cpp and
// Features.command.cpp at af1d26bf.

#ifndef _WIN32

#include <alpacacore/util/error_handling.h>

#include "catch2_compat.h"
#include "onstep_controller_model.h"

namespace alpacacore::test::onstep_controller {

namespace onstep = alpacacore::vendor::onstep;
using std::chrono::milliseconds;

// The controller and its server; the devices a case builds on it are declared after it, so they go first.
struct Controller {
    std::shared_ptr<OnStepXController> state = std::make_shared<OnStepXController>();
    alpacacore::test::FakeMountServer server{[s = state](const std::string& chunk) { return s->reply(chunk); }};

    Controller() { REQUIRE(server.ok()); }
    onstep::ConnectionInfo info() const { return endpoint(server.port()); }
    std::unique_ptr<alpacacore::FocuserDriver> focuser(int n = 1) {
        return onstep::create_onstep_focuser(0, info(), n);
    }
    std::unique_ptr<alpacacore::TelescopeDriver> telescope() { return onstep::create_onstep_telescope(0, info()); }
    std::unique_ptr<alpacacore::RotatorDriver> rotator() { return onstep::create_onstep_rotator(0, info()); }
    std::unique_ptr<alpacacore::SwitchDriver> aux_switch() { return onstep::create_onstep_switch(0, info()); }
};

// Runs `action` `tries` times on this thread. Before each exchange it makes, the BeforeExchange hook lets another
// thread ask the shared link for the product name (:GVP#) and waits up to 50 ms for it. A refused command and its
// :GE# then stay together only if the command holds the link's Sequence: without it, the :GVP# lands between the
// two every time. Returns the command log for the caller to check the pairs.
template <typename Action>
std::vector<std::string> run_with_a_sibling_at_every_exchange(Controller& controller, int tries, Action&& action) {
    const auto link = onstep::detail::registered_link_for_testing(controller.info());
    REQUIRE(link);
    std::mutex m;
    std::condition_variable cv;
    bool go = false;
    bool done = false;
    bool stop = false;
    std::atomic<std::thread::id> mover{std::this_thread::get_id()};
    std::thread sibling([&] {
        std::unique_lock<std::mutex> lock(m);
        for (;;) {
            cv.wait(lock, [&] { return go || stop; });
            if (stop) return;
            go = false;
            lock.unlock();
            (void)link->get_product_name();  // waits for the wire, and for a Sequence another device holds
            lock.lock();
            done = true;
            cv.notify_all();
        }
    });
    onstep::detail::set_link_loss_hook([&](onstep::detail::LinkLossHookPoint point) {
        if (point != onstep::detail::LinkLossHookPoint::BeforeExchange || std::this_thread::get_id() != mover.load()) {
            return;
        }
        std::unique_lock<std::mutex> lock(m);
        done = false;
        go = true;
        cv.notify_all();
        cv.wait_for(lock, std::chrono::milliseconds(50), [&] { return done; });  // done only when nothing held it
    });
    for (int i = 0; i < tries; ++i) {
        action();
    }
    onstep::detail::set_link_loss_hook({});
    mover.store(std::thread::id());
    {
        std::lock_guard<std::mutex> lock(m);
        stop = true;
    }
    cv.notify_all();
    sibling.join();
    return controller.state->commands();
}

// Runs `member` on this thread with a Disconnect of `driver` landed right after the member's connection check passed
// (the AfterConnectionCheck hook, once, on this thread only): the window a TSan storm found (#2222). Returns the
// error code the member threw, or -1 when it returned.
template <typename Member>
int code_with_disconnect_after_check(alpacacore::AlpacaDriver& driver, Member&& member) {
    const auto me = std::this_thread::get_id();
    std::atomic<bool> fired{false};
    onstep::detail::set_link_loss_hook([&](onstep::detail::LinkLossHookPoint point) {
        if (point != onstep::detail::LinkLossHookPoint::AfterConnectionCheck || std::this_thread::get_id() != me ||
            fired.exchange(true)) {
            return;
        }
        driver.set_connected(false);
    });
    int code = -1;
    try {
        member();
    } catch (const alpacacore::AlpacaException& e) {
        code = e.error_code();
    }
    onstep::detail::set_link_loss_hook({});
    CHECK(fired.load());
    return code;
}

// Checks that every :GE# in `log` follows a command starting with `prefix`, and counts them.
inline int ge_pairs_after(const std::vector<std::string>& log, const std::string& prefix) {
    int pairs = 0;
    for (std::size_t i = 1; i < log.size(); ++i) {
        if (log[i] == ":GE#") {
            ++pairs;
            CAPTURE(i);
            CHECK(log[i - 1].rfind(prefix, 0) == 0);  // never another exchange between the command and its :GE#
        }
    }
    return pairs;
}

}  // namespace alpacacore::test::onstep_controller

#endif  // !_WIN32
