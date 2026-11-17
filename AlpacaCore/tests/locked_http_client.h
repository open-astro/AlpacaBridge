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

#include <alpacacore/util/http_client.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace alpacacore::test {

/**
 * Thread-safe decorator over any util::HttpClient (decision 0015 rule 2; issue #1753): get and put_form forward
 * to the inner client under one mutex, the way a client with one shared connection would serialise its requests.
 * The [stress] runs put it over FakeHttpClient. The mutex is a leaf (the inner client never calls back into this
 * interface) and is held across a request, so never wrap a client whose request a test holds open.
 */
class LockedHttpClient final : public util::HttpClient {
public:
    explicit LockedHttpClient(std::shared_ptr<util::HttpClient> inner) : inner_(std::move(inner)) {}

    util::HttpResponse get(const std::string& url, std::chrono::milliseconds timeout) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return inner_->get(url, timeout);
    }

    util::HttpResponse put_form(const std::string& url, const std::string& form_body,
                                std::chrono::milliseconds timeout) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return inner_->put_form(url, form_body, timeout);
    }

private:
    std::shared_ptr<util::HttpClient> inner_;
    std::mutex mutex_;
};

}  // namespace alpacacore::test
