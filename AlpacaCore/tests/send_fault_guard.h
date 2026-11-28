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

#include <atomic>
#include <functional>
#include <string>
#include <utility>

namespace alpacacore::test {

/// A send that fails with a chosen errno over TCP while the socket stays up (#2753), for a mount wrapper's
/// set_send_fault_for_testing(): the failure a send timeout or EHOSTUNREACH makes in production, which the fakes'
/// hangups cannot build (decision 0009 rule 2a). `Wrapper` is the vendor's protocol wrapper singleton.
///
/// Fails each command @p matches accepts with @p err while armed, and clears the seam when it dies, so a failing
/// REQUIRE cannot leave it set for the next case. `failed()` counts the sends it failed.
template <class Wrapper>
class SendFaultGuard {
public:
    SendFaultGuard(int err, std::function<bool(const std::string&)> matches) : err_(err), matches_(std::move(matches)) {
        Wrapper::instance().set_send_fault_for_testing([this](const std::string& command) {
            if (armed_.load() && matches_(command)) {
                failed_.fetch_add(1);
                return err_;
            }
            return 0;
        });
    }
    ~SendFaultGuard() { Wrapper::instance().set_send_fault_for_testing({}); }
    SendFaultGuard(const SendFaultGuard&) = delete;
    SendFaultGuard& operator=(const SendFaultGuard&) = delete;

    void arm() { armed_.store(true); }
    void disarm() { armed_.store(false); }
    int failed() const { return failed_.load(); }

private:
    const int err_;
    const std::function<bool(const std::string&)> matches_;
    std::atomic<bool> armed_{false};
    std::atomic<int> failed_{0};
};

}  // namespace alpacacore::test
