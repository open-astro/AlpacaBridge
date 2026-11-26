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

#include "client_connection_ledger.h"

#include <algorithm>
#include <utility>

namespace alpacahttp {

namespace {
// The slot every ClientID-less caller from one peer address shares (issue
// #163), so such clients still take part in refcounting rather than tearing
// the device link down for everyone.
constexpr std::string_view kAnonymousClientKey = "<anonymous>";
}  // namespace

ClientConnectionLedger::ClientConnectionLedger(Clock clock)
    : clock_(clock ? std::move(clock) : Clock([] { return std::chrono::steady_clock::now(); })) {}

std::string ClientConnectionLedger::client_key(std::string_view remote_address,
                                               std::optional<std::string_view> client_id) {
    std::string key = std::to_string(remote_address.size());
    key += '#';
    key += remote_address;
    key += '#';
    key += client_id ? *client_id : kAnonymousClientKey;
    return key;
}

void ClientConnectionLedger::expire_stale(Clients& clients, std::chrono::steady_clock::time_point now) const {
    const auto cutoff = now - kStaleAfter;
    for (auto it = clients.begin(); it != clients.end();) {
        if (it->second < cutoff) {
            it = clients.erase(it);
        } else {
            ++it;
        }
    }
}

std::optional<std::size_t> ClientConnectionLedger::register_client(const void* device, const std::string& client) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& clients = clients_[device];
    const auto now = clock_();
    expire_stale(clients, now);
    if (clients.size() >= kMaxClientsPerDevice && clients.find(client) == clients.end()) {
        return std::nullopt;
    }
    clients[client] = now;
    return clients.size();
}

std::size_t ClientConnectionLedger::unregister_client(const void* device, const std::string& client) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = clients_.find(device);
    if (it == clients_.end()) {
        return 0;
    }
    it->second.erase(client);
    expire_stale(it->second, clock_());
    if (it->second.empty()) {
        clients_.erase(it);
        return 0;
    }
    return it->second.size();
}

bool ClientConnectionLedger::is_registered(const void* device, const std::string& client) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = clients_.find(device);
    if (it == clients_.end()) {
        return false;
    }
    auto client_it = it->second.find(client);
    if (client_it == it->second.end()) {
        return false;
    }
    client_it->second = clock_();
    return true;
}

void ClientConnectionLedger::touch(const void* device, const std::string& client) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = clients_.find(device);
    if (it == clients_.end()) {
        return;
    }
    auto client_it = it->second.find(client);
    if (client_it != it->second.end()) {
        client_it->second = clock_();
    }
}

ClientConnectionLedger::Activity ClientConnectionLedger::activity(const void* device) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Activity activity;
    auto it = clients_.find(device);
    if (it == clients_.end()) {
        return activity;
    }
    // Read-only: an entry past the stale window is skipped here and left for
    // the next register/unregister to expire, as the disconnect rule does.
    const auto now = clock_();
    const auto cutoff = now - kStaleAfter;
    std::optional<std::chrono::steady_clock::time_point> newest;
    for (const auto& entry : it->second) {
        const auto seen = entry.second;
        if (seen < cutoff) {
            continue;
        }
        ++activity.active_clients;
        if (!newest || seen > *newest) {
            newest = seen;
        }
    }
    if (newest) {
        activity.last_request_age = std::max(std::chrono::milliseconds{0},
                                             std::chrono::duration_cast<std::chrono::milliseconds>(now - *newest));
    }
    return activity;
}

void ClientConnectionLedger::clear(const void* device) {
    std::lock_guard<std::mutex> lock(mutex_);
    clients_.erase(device);
    // op_mutexes_ is deliberately NOT erased (see the header). The map is
    // bounded by registered-device count; a recycled pointer sharing an old
    // mutex only means harmless extra serialization.
}

void ClientConnectionLedger::purge(const void* device) {
    std::lock_guard<std::mutex> lock(mutex_);
    clients_.erase(device);
    op_mutexes_.erase(device);
}

std::shared_ptr<std::mutex> ClientConnectionLedger::op_mutex(const void* device,
                                                             const std::function<bool()>& device_is_current) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = op_mutexes_.find(device);
    if (it != op_mutexes_.end()) {
        return it->second;
    }
    // Lock order: this ledger's mutex -> whatever device_is_current takes (the
    // Router passes a DeviceRegistry lookup; the registry never calls back).
    if (!device_is_current()) {
        return std::make_shared<std::mutex>();
    }
    auto mutex = std::make_shared<std::mutex>();
    op_mutexes_[device] = mutex;
    return mutex;
}

}  // namespace alpacahttp
