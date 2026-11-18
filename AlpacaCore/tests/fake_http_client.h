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

// Scripted util::HttpClient for tests (decision 0015; issue #1753). Every request answers with the scripted
// status and the body the body source returns at call time, unless a failure is scripted. Thread-safe, because a
// driver calls it from its connect thread and its poll thread. It never blocks unless a test sets a delay, so a
// driver that joins its threads always gets them back (decision 0015 rule 5). The fake does not apply the libcurl
// bounds: those are tested against a loopback server in test_http_client.cpp.

#include <alpacacore/util/http_client.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace alpacacore::test {

class FakeHttpClient final : public util::HttpClient {
public:
    struct Request {
        std::string method;  // "GET" or "PUT"
        std::string url;
        std::string form_body;
        std::chrono::milliseconds timeout{0};
    };

    FakeHttpClient() = default;
    explicit FakeHttpClient(std::function<std::string()> body_source) : body_source_(std::move(body_source)) {}

    util::HttpResponse get(const std::string& url, std::chrono::milliseconds timeout) override {
        return answer({"GET", url, {}, timeout});
    }

    util::HttpResponse put_form(const std::string& url, const std::string& form_body,
                                std::chrono::milliseconds timeout) override {
        return answer({"PUT", url, form_body, timeout});
    }

    // A non-owning handle for a factory that takes a shared_ptr: the fake must outlive every user.
    std::shared_ptr<util::HttpClient> client() {
        return std::shared_ptr<util::HttpClient>(std::shared_ptr<void>{}, this);
    }

    // The next `n` requests throw instead of answering, as a refused or timed-out request does.
    void fail_next(int n) { failures_.store(n); }

    // Every request sleeps this long before answering (an explicit hook; no delay by default).
    void set_delay(std::chrono::milliseconds delay) { delay_ms_.store(delay.count()); }

    void set_status(long status) { status_.store(status); }

    void set_body_source(std::function<std::string()> body_source) {
        std::lock_guard<std::mutex> lock(mutex_);
        body_source_ = std::move(body_source);
    }

    int calls() const { return calls_.load(); }

    // Requests executing now; zero once every caller has returned.
    int in_flight() const { return in_flight_.load(); }

    // The most requests that were executing at once.
    int max_in_flight() const { return max_in_flight_.load(); }

    std::vector<Request> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

private:
    util::HttpResponse answer(Request request) {
        calls_.fetch_add(1);
        InFlight guard(in_flight_, max_in_flight_);
        std::function<std::string()> source;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(std::move(request));
            source = body_source_;
        }
        const auto delay = delay_ms_.load();
        if (delay > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        int pending = failures_.load();
        while (pending > 0) {
            if (failures_.compare_exchange_weak(pending, pending - 1)) {
                throw std::runtime_error("scripted HTTP request failure");
            }
        }
        return util::HttpResponse{status_.load(), source ? source() : std::string{}};
    }

    struct InFlight {
        InFlight(std::atomic<int>& n, std::atomic<int>& peak) : n_(n) {
            const int now = n_.fetch_add(1) + 1;
            int seen = peak.load();
            while (now > seen && !peak.compare_exchange_weak(seen, now)) {
            }
        }
        ~InFlight() { n_.fetch_sub(1); }
        InFlight(const InFlight&) = delete;
        InFlight& operator=(const InFlight&) = delete;
        std::atomic<int>& n_;
    };

    mutable std::mutex mutex_;
    std::function<std::string()> body_source_;
    std::vector<Request> requests_;
    std::atomic<int> in_flight_{0};
    std::atomic<int> max_in_flight_{0};
    std::atomic<int> calls_{0};
    std::atomic<int> failures_{0};
    std::atomic<long long> delay_ms_{0};
    std::atomic<long> status_{200};
};

}  // namespace alpacacore::test
