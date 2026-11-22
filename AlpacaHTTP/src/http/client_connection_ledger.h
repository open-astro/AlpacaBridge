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

#pragma once

// Internal seam (lives under src/, not include/): the per-client Connected
// bookkeeping (issue #2521, moved out of the Router). Alpaca lets several
// clients share one device (imaging app + guider on the same mount), so the
// upstream link drops only when the LAST client leaves (issue #160). This
// ledger records which clients hold each device and when each was last seen,
// and owns the per-device connection-op mutex that orders a connect against
// another client's last-out teardown. Keyed by the driver's address: a later
// driver at a recycled address starts clean once purge() has run (issue #162).
// It never calls a driver; the Router decides when to connect or disconnect.

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace alpacahttp {

class ClientConnectionLedger {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    // A registration not refreshed within this window is abandoned (the
    // client crashed or vanished without PUT connected=false) and no longer
    // blocks the last-client-out disconnect. Any request from the client to
    // the device refreshes it, so ordinary polling keeps it alive.
    static constexpr std::chrono::minutes kStaleAfter{10};

    // open-astro#2292 (ADR 0007 rules 4 and 10): the most (address, ClientID)
    // registrations one device holds. A uint32 ClientID per request (#1738)
    // still let one host add an entry per request for the whole stale window;
    // past this a new client's Connected=true is refused until an entry
    // leaves or goes stale. A rig has a handful of clients per device.
    static constexpr std::size_t kMaxClientsPerDevice = 256;

    // `clock` stamps and ages the registrations; empty means
    // steady_clock::now(). A test clock must outlive the ledger.
    explicit ClientConnectionLedger(Clock clock = {});

    // The registration key of a caller (issue #163): its ClientID, or one
    // anonymous slot per peer address when it sent none, qualified by the
    // length-prefixed peer address. The prefix keeps a client-supplied
    // ClientID from forging another (address, ClientID) pair; an empty
    // address (unit tests, getpeername failure) gives "0##<id>", a
    // ClientID-only identity no address-qualified key collides with.
    static std::string client_key(std::string_view remote_address, std::optional<std::string_view> client_id);

    // Registers (or refreshes) `client` on `device` after expiring its stale
    // entries. Returns the device's client count, or nullopt when it already
    // holds kMaxClientsPerDevice other clients (#2292).
    std::optional<std::size_t> register_client(const void* device, const std::string& client);
    // Drops `client` and expires stale entries; returns the clients left. 0
    // means the caller was the last one out and tears the link down.
    std::size_t unregister_client(const void* device, const std::string& client);
    // Whether `client` holds `device`. A lookup is client activity: it
    // refreshes the registration, so steady polling never goes stale.
    bool is_registered(const void* device, const std::string& client);
    // Refreshes `client`'s registration on `device` if it has one.
    void touch(const void* device, const std::string& client);

    // Issue #1764: what configureddevices reports per device, read from the
    // ledger only, never from the driver. `active_clients` counts
    // registrations refreshed inside the stale window; `last_request_age` is
    // the age of the newest of them, empty when there is none.
    struct Activity {
        std::size_t active_clients = 0;
        std::optional<std::chrono::milliseconds> last_request_age;
    };
    Activity activity(const void* device) const;

    // Drops every registration of `device` (a dead link, or a device being
    // removed) and keeps its op mutex: this runs while that mutex is held,
    // and erasing it would let a concurrent op mint a fresh mutex and bypass
    // the serialization. Only purge() erases an op mutex.
    void clear(const void* device);
    // Erases both the registrations and the op mutex of a removed device
    // (issue #162). Call it after releasing the op mutex and after the device
    // has left the DeviceRegistry.
    void purge(const void* device);

    // The mutex that serializes the connect/disconnect decision and driver
    // call per device (review of #160). Held across the blocking connect and
    // disconnect waits, so same-device connection ops queue; everything else
    // is unaffected. `device_is_current` is read under the ledger's lock: a
    // device it rejects (a straggler on a removed device, after purge) gets an
    // ephemeral mutex rather than an entry nobody would reap (PR #164 review).
    std::shared_ptr<std::mutex> op_mutex(const void* device, const std::function<bool()>& device_is_current);

private:
    using Clients = std::unordered_map<std::string, std::chrono::steady_clock::time_point>;
    void expire_stale(Clients& clients, std::chrono::steady_clock::time_point now) const;

    const Clock clock_;
    // Guards both maps; never held across a driver call.
    mutable std::mutex mutex_;
    std::unordered_map<const void*, Clients> clients_;
    std::unordered_map<const void*, std::shared_ptr<std::mutex>> op_mutexes_;
};

}  // namespace alpacahttp
