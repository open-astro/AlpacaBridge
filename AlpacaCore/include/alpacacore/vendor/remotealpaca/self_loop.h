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

// Decision 0023 rule 4, issue #1753: a proxied device never points at this server. An upstream whose host and port
// resolve to one of this server's own listening addresses is refused when it is configured and again at every
// Connect, since the host may resolve differently by then. AlpacaCore has no view of the server's bind addresses, so
// AlpacaHTTP hands them to the factory (catalog::BindContext::listen_addresses) and the driver asks again at Connect.

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace alpacacore::vendor::remotealpaca {

// One address this server listens on: the bound host and its TCP port. A wildcard host ("0.0.0.0", "::" or empty)
// counts every local interface address and every loopback address as this server.
struct ServerAddress {
    std::string host;
    int port = 0;
};

// This server's listening addresses at the time of the call. An empty function means no check: a driver a unit test
// builds directly, with no server.
using ServerAddressSource = std::function<std::vector<ServerAddress>()>;

// The refusal naming the loop when `host`:`port` resolves to one of `self`, else nullopt. A host that does not resolve
// is not refused here: no address of it can be this server, and the Connect reports the failure. Resolving a name
// may wait on the system resolver.
std::optional<std::string> self_loop_refusal(const std::string& host, int port, const std::vector<ServerAddress>& self);

}  // namespace alpacacore::vendor::remotealpaca
