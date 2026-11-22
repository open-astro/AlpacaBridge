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

// Router helpers that a per-type routes source (src/http/routes/<type>_routes.cpp, issue #2081) shares
// with router.cpp. Each is defined once, in router.cpp; this header only declares it, so no helper has two
// definitions. Internal to AlpacaHTTP: not installed, not under include/.

#include <alpacacore/telescope_driver.h>
#include <alpacahttp/request.h>
#include <alpacahttp/response.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

namespace alpacahttp {

// open-astro#1660: SlewToCoordinates and SlewToTarget are served by starting the async slew and waiting, so
// a driver that cannot slew is refused (MethodNotImplemented) before anything is parsed or written.
void require_can_slew(const alpacacore::TelescopeDriver& telescope);

// Waits while Slewing is true, up to `limit` (Router::sync_slew_wait_limit()). On expiry, or once `stopping`
// is set (open-astro#1705), it throws DriverException and the slew is NOT aborted. An exception from
// get_slewing() propagates as is.
void wait_for_slew_end(const alpacacore::TelescopeDriver& telescope, std::chrono::milliseconds limit,
                       const std::atomic<bool>& stopping);

// The CSRF guard of every state-changing management endpoint and of the four device setters with a side
// effect beyond the driver: UTCDate steps the host clock (open-astro#401) and the three site setters rewrite
// persisted config (#444). Returns the 403 to send, or std::nullopt when the request may proceed. Echoes
// the client's ClientTransactionID (open-astro#384). Decision 0007 rule 2.
std::optional<Response> reject_cross_origin_request(const Request& request, std::uint32_t client_tx_id,
                                                    std::uint32_t server_tx_id, const char* what);

}  // namespace alpacahttp
