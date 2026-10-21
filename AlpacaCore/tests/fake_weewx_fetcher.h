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

// Scripted WeeWxFetcher for tests: canned JSON, a scripted failure, and a slow
// response. Thread-safe, because the driver calls it from the connect thread and
// the poll thread. Never blocks beyond the configured delay, so a driver that
// joins its poll thread always gets it back.

#include <alpacacore/vendor/weewx/weewx_observingconditions_driver.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <stdexcept>
#include <string>
#include <thread>

namespace alpacacore::test {

class FakeWeeWxFetcher {
public:
    // Temperature (50 F = 10 C) and humidity, observed now.
    static std::string canned_payload() {
        const long long now = static_cast<long long>(std::time(nullptr));
        return R"({"lcd_datasheet":{"current":{"outTemp":{"value":50.0},"outHumidity":{"value":40.0},"dateTime":{"value":)" +
               std::to_string(now) + "}}}}";
    }

    // The next `n` fetches throw instead of answering.
    void fail_next(int n) { failures_.store(n); }

    // Every fetch sleeps this long before answering.
    void set_delay(std::chrono::milliseconds delay) { delay_ms_.store(delay.count()); }

    int calls() const { return calls_.load(); }

    // Fetches currently executing; zero once every caller has returned.
    int in_flight() const { return in_flight_.load(); }

    // The fetcher to hand to create_weewx_observingconditions. Captures `this`:
    // the fake must outlive every driver built on it.
    vendor::weewx::WeeWxFetcher fetcher() {
        return [this](const std::string&, std::chrono::milliseconds) { return fetch(); };
    }

private:
    std::string fetch() {
        calls_.fetch_add(1);
        InFlight guard(in_flight_);
        const auto delay = delay_ms_.load();
        if (delay > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        int pending = failures_.load();
        while (pending > 0) {
            if (failures_.compare_exchange_weak(pending, pending - 1)) {
                throw std::runtime_error("scripted WeeWX fetch failure");
            }
        }
        return canned_payload();
    }

    struct InFlight {
        explicit InFlight(std::atomic<int>& n) : n_(n) { n_.fetch_add(1); }
        ~InFlight() { n_.fetch_sub(1); }
        std::atomic<int>& n_;
    };

    std::atomic<int> in_flight_{0};
    std::atomic<int> calls_{0};
    std::atomic<int> failures_{0};
    std::atomic<long long> delay_ms_{0};
};

}  // namespace alpacacore::test
