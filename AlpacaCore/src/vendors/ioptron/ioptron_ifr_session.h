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

// The connect half both iFR drivers share: resolve the port (auto-detect at connect, #659), join or open the
// unit's IfrLink session, and hold it until the next Connect or Disconnect. Private to the iFR drivers.

#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/vendor/ioptron/ioptron_ifr_protocol_wrapper.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>

namespace alpacacore::vendor::ioptron::detail {

class IfrSessionHolder {
public:
    IfrSessionHolder(IfrConnectionConfig config, util::ConnectionResolver<IfrConnectionConfig> resolver)
        : config_(std::move(config)), resolver_(std::move(resolver)) {}

    /// Caller holds its driver's transition_mutex_. Drops any session it held (a lost one included), then
    /// resolves and acquires. Throws the operator-facing refusal.
    std::shared_ptr<IfrLink> open() {
        close();
        std::shared_ptr<IfrLink> link;
        util::connect_resolved(
            config_, resolved_, resolver_,
            [&link](const IfrConnectionConfig& cfg) {
                try {
                    link = IfrLink::acquire(cfg);
                } catch (const AlpacaException& e) {
                    // Only a vanished node, or a port another kind of device now holds (#1202), is stale; a
                    // handshake miss on a present port is not, and must not start a scan.
                    if (util::serial_endpoint_stale(cfg.serial_port))
                        throw util::StaleEndpoint(e.what(), e.error_code());
                    throw;
                }
            },
            "iOptron");
        return link;
    }

    /// Publish the session once the driver's own connect checks have passed.
    void publish(std::shared_ptr<IfrLink> link) { link_.store(std::move(link)); }

    /// Drop this driver's hold; the port closes when the other device holds none either. The pointer is
    /// released here, never under the driver's state lock: the last release closes the port under the
    /// session's I/O lock.
    void close() {
        std::shared_ptr<IfrLink> old = link_.exchange(nullptr);
        old.reset();
    }

    /// The held session, possibly lost. Never waits on the wire.
    std::shared_ptr<IfrLink> current() const { return link_.load(); }

    /// Held and not lost.
    bool alive() const {
        const auto link = link_.load();
        return link && link->link_alive();
    }

    /// Decision 0009 rule 5: a lost session keeps its reason until the next Connect or Disconnect drops it.
    std::string link_fault() const {
        const auto link = link_.load();
        return link ? link->link_fault() : std::string();
    }

private:
    IfrConnectionConfig config_;
    util::ConnectionResolver<IfrConnectionConfig> resolver_;
    bool resolved_ = false;  // under the driver's transition_mutex_
    std::atomic<std::shared_ptr<IfrLink>> link_;
};

}  // namespace alpacacore::vendor::ioptron::detail
