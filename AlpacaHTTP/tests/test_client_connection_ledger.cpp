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

// The per-client Connected bookkeeping on its own (issue #2521): a ledger over
// a test clock, no Router, no devices and no driver calls. The device keys are
// the addresses of local ints, as the Router's are the drivers' addresses.

#include <chrono>
#include <cstddef>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "http/client_connection_ledger.h"
#include "test_assert.h"

namespace {

using alpacahttp::ClientConnectionLedger;
using namespace std::chrono_literals;

struct TestClock {
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    ClientConnectionLedger::Clock fn() {
        return [this] { return now; };
    }
};

const std::function<bool()> kCurrent = [] { return true; };
const std::function<bool()> kRemoved = [] { return false; };

std::string key(const std::string& address, const std::string& id) {
    return ClientConnectionLedger::client_key(address, id);
}

}  // namespace

int main() {
    std::cout << "Testing the client-connection ledger...\n";
    {
        // case: the ledger counts clients per device and reports 0 only when the last client leaves
        TestClock clock;
        ClientConnectionLedger ledger(clock.fn());
        int mount = 0;
        int camera = 0;
        const auto imaging = key("10.0.0.2", "1");
        const auto guider = key("10.0.0.3", "1");
        EXPECT(ledger.register_client(&mount, imaging) == std::optional<std::size_t>(1));
        EXPECT(ledger.register_client(&mount, guider) == std::optional<std::size_t>(2));
        EXPECT(ledger.register_client(&mount, guider) == std::optional<std::size_t>(2));
        EXPECT(ledger.register_client(&camera, imaging) == std::optional<std::size_t>(1));
        EXPECT(ledger.unregister_client(&mount, imaging) == 1);
        EXPECT(!ledger.is_registered(&mount, imaging));
        EXPECT(ledger.is_registered(&mount, guider));
        EXPECT(ledger.unregister_client(&mount, guider) == 0);
        EXPECT(ledger.unregister_client(&mount, guider) == 0);
        EXPECT(ledger.is_registered(&camera, imaging));
    }
    {
        // case: ClientID-less callers share one slot per peer address and a ClientID cannot forge another key
        TestClock clock;
        ClientConnectionLedger ledger(clock.fn());
        int mount = 0;
        const auto anonymous = ClientConnectionLedger::client_key("10.0.0.2", std::nullopt);
        EXPECT(anonymous == "8#10.0.0.2#<anonymous>");
        EXPECT(anonymous == ClientConnectionLedger::client_key("10.0.0.2", std::nullopt));
        EXPECT(anonymous != ClientConnectionLedger::client_key("10.0.0.3", std::nullopt));
        EXPECT(ClientConnectionLedger::client_key("", std::string("5")) == "0##5");
        EXPECT(key("1", "2#3") != key("1#2", "3"));
        EXPECT(ledger.register_client(&mount, anonymous) == std::optional<std::size_t>(1));
        EXPECT(ledger.register_client(&mount, anonymous) == std::optional<std::size_t>(1));
        EXPECT(ledger.register_client(&mount, ClientConnectionLedger::client_key("10.0.0.3", std::nullopt)) ==
               std::optional<std::size_t>(2));
        EXPECT(ledger.unregister_client(&mount, anonymous) == 1);
    }
    {
        // case: a device refuses a new client past kMaxClientsPerDevice but not a holder or a freed slot
        TestClock clock;
        ClientConnectionLedger ledger(clock.fn());
        int mount = 0;
        for (std::size_t id = 1; id <= ClientConnectionLedger::kMaxClientsPerDevice; ++id) {
            EXPECT(ledger.register_client(&mount, key("10.0.0.2", std::to_string(id))) ==
                   std::optional<std::size_t>(id));
        }
        const auto late = key("10.0.0.2", "9999");
        EXPECT(!ledger.register_client(&mount, late).has_value());
        EXPECT(!ledger.is_registered(&mount, late));
        EXPECT(ledger.register_client(&mount, key("10.0.0.2", "1")) ==
               std::optional<std::size_t>(ClientConnectionLedger::kMaxClientsPerDevice));
        EXPECT(ledger.unregister_client(&mount, key("10.0.0.2", "2")) ==
               ClientConnectionLedger::kMaxClientsPerDevice - 1);
        EXPECT(ledger.register_client(&mount, late) ==
               std::optional<std::size_t>(ClientConnectionLedger::kMaxClientsPerDevice));
        // Every entry gone stale frees the device for a new client.
        clock.now += ClientConnectionLedger::kStaleAfter + 1s;
        EXPECT(ledger.register_client(&mount, key("10.0.0.2", "10000")) == std::optional<std::size_t>(1));
    }
    {
        // case: a client that stops polling expires after kStaleAfter and a lookup or touch keeps it alive
        TestClock clock;
        ClientConnectionLedger ledger(clock.fn());
        int mount = 0;
        const auto vanished = key("10.0.0.2", "1");
        const auto polling = key("10.0.0.3", "1");
        const auto touched = key("10.0.0.4", "1");
        EXPECT(ledger.register_client(&mount, vanished).has_value());
        EXPECT(ledger.register_client(&mount, polling).has_value());
        EXPECT(ledger.register_client(&mount, touched).has_value());
        clock.now += ClientConnectionLedger::kStaleAfter - 1min;
        EXPECT(ledger.is_registered(&mount, polling));
        ledger.touch(&mount, touched);
        ledger.touch(&mount, key("10.0.0.5", "1"));  // no registration: touch adds none
        clock.now += 2min;
        // The vanished client no longer pins the device: the other two leaving is last-out.
        EXPECT(ledger.unregister_client(&mount, polling) == 1);
        EXPECT(!ledger.is_registered(&mount, vanished));
        EXPECT(ledger.unregister_client(&mount, touched) == 0);
    }
    {
        // case: purge of a removed device erases its registrations and its op mutex
        TestClock clock;
        ClientConnectionLedger ledger(clock.fn());
        int mount = 0;
        const auto client = key("10.0.0.2", "1");
        EXPECT(ledger.register_client(&mount, client).has_value());
        const auto before = ledger.op_mutex(&mount, kCurrent);
        EXPECT(ledger.op_mutex(&mount, kCurrent) == before);
        ledger.purge(&mount);
        EXPECT(!ledger.is_registered(&mount, client));
        EXPECT(ledger.activity(&mount).active_clients == 0);
        // A straggler on the removed device gets a fresh mutex each time, never stored.
        const auto straggler = ledger.op_mutex(&mount, kRemoved);
        EXPECT(straggler != before);
        EXPECT(ledger.op_mutex(&mount, kRemoved) != straggler);
        // A new driver at the recycled address starts clean, with its own mutex.
        const auto recycled = ledger.op_mutex(&mount, kCurrent);
        EXPECT(recycled != before);
        EXPECT(recycled != straggler);
        EXPECT(ledger.op_mutex(&mount, kCurrent) == recycled);
    }
    {
        // case: clear while the device's op mutex is held drops the registrations and keeps the mutex
        TestClock clock;
        ClientConnectionLedger ledger(clock.fn());
        int mount = 0;
        const auto client = key("10.0.0.2", "1");
        EXPECT(ledger.register_client(&mount, client).has_value());
        const auto held = ledger.op_mutex(&mount, kCurrent);
        {
            std::lock_guard<std::mutex> op_lock(*held);
            ledger.clear(&mount);
            EXPECT(!ledger.is_registered(&mount, client));
            // A concurrent op must queue on the same mutex, not mint a new one.
            const auto concurrent = ledger.op_mutex(&mount, kCurrent);
            EXPECT(concurrent == held);
            EXPECT(!concurrent->try_lock());
        }
        EXPECT(ledger.op_mutex(&mount, kCurrent) == held);
    }
    {
        // case: activity reports the clients inside the stale window and the age of the newest request
        TestClock clock;
        ClientConnectionLedger ledger(clock.fn());
        int mount = 0;
        int idle = 0;
        const auto none = ledger.activity(&idle);
        EXPECT(none.active_clients == 0);
        EXPECT(!none.last_request_age.has_value());
        const auto first = key("10.0.0.2", "1");
        const auto second = key("10.0.0.3", "1");
        EXPECT(ledger.register_client(&mount, first).has_value());
        clock.now += 2s;
        EXPECT(ledger.register_client(&mount, second).has_value());
        clock.now += 1s;
        auto seen = ledger.activity(&mount);
        EXPECT(seen.active_clients == 2);
        EXPECT(seen.last_request_age == std::optional<std::chrono::milliseconds>(1000ms));
        // Past the window for the first client only: it is skipped, not counted.
        clock.now += ClientConnectionLedger::kStaleAfter - 2s;
        seen = ledger.activity(&mount);
        EXPECT(seen.active_clients == 1);
        EXPECT(seen.last_request_age ==
               std::optional<std::chrono::milliseconds>(ClientConnectionLedger::kStaleAfter - 1s));
        clock.now += 1min;
        seen = ledger.activity(&mount);
        EXPECT(seen.active_clients == 0);
        EXPECT(!seen.last_request_age.has_value());
        // Activity only reads: the stale entries are still there for the next unregister to expire.
        EXPECT(ledger.unregister_client(&mount, key("10.0.0.9", "1")) == 0);
    }
    std::cout << "All client-connection ledger tests passed.\n";
    return 0;
}
