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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>

#include <exception>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <utility>

namespace alpacacore::util {

/**
 * Connect-time endpoint resolution for auto-detected devices.
 *
 * An auto-detect factory (`create_<vendor>_<device>_auto`, `_auto_network`,
 * `_by_index`) used to run its serial or network scan at construction, which
 * for a persisted device is server start-up. A device that is not there yet
 * at boot (a mount still joining the access point, a USB adapter powered
 * after the SBC) then threw out of the factory, the router logged "Failed to
 * load persisted device" and the entry sat as "(failed to load)" until the
 * service was restarted; no later connect could revive it.
 *
 * The factory now hands the driver a `ConnectionResolver<Info>` instead: a
 * callable that runs the scan and returns the endpoint, or throws the
 * operator-facing refusal ("No iOptron mount found on the local network ...").
 * The driver calls `connect_resolved()` from its connect path, so construction
 * never touches the bus and the scan runs when a client asks for the device.
 *
 * Policy (`connect_resolved`):
 * - No resolver: `try_connect(info)` once; the endpoint is fixed.
 * - Resolver, nothing resolved yet: resolve, then `try_connect` the result.
 * - Resolver, a previous connect resolved an endpoint: `try_connect` that
 *   endpoint first and re-resolve only if it throws `StaleEndpoint`. ConformU
 *   and NINA connect and disconnect many times per session; paying the full
 *   scan (several seconds for a serial probe ladder or a subnet sweep) on
 *   every connect would push a Platform 7 `Connect()` past its 5 s client
 *   budget, while a stale endpoint (re-enumerated USB port, new DHCP lease)
 *   still falls through to a fresh scan.
 *
 * `try_connect(const Info&)` must THROW on failure (the drivers' existing
 * `if (!protocol.connect(info)) throw AlpacaException(...)` shape, moved into
 * the lambda). Re-scanning is OPT-IN: the retry of a previously resolved
 * endpoint falls through to the resolver only when the lambda throws
 * `StaleEndpoint`, which it does for failures that mean the endpoint is gone
 * or is not this device any more: the serial or HID node no longer exists
 * (`device_node_missing()`), a network connect was refused, or the vendor's
 * identity gate failed (SynScan's echo test). Every other exception
 * propagates unchanged, because a scan is not free: the QHYCFW3 and Gemini
 * probes open, and DTR-reset, every CP210x or CH340 device on the box, so a
 * wheel that is still homing ("try again in a few seconds") or a handshake
 * that missed once must NOT trigger one (review of #660). An exception from
 * the resolver, or from the connect that follows it, propagates unchanged,
 * so the client sees the scan's own message as the connect refusal (#358).
 *
 * The caller holds whatever lock its connect path already holds; this helper
 * takes none. `info` and `resolved` are the driver's own members so the
 * endpoint that answered survives across connects and is what the driver's
 * log lines and getters report.
 */
template <typename Info>
using ConnectionResolver = std::function<Info()>;

/// Thrown by a `try_connect` lambda when the resolved endpoint is gone or is
/// not this device any more; the only exception `connect_resolved()` answers
/// with a fresh scan.
class StaleEndpoint : public AlpacaException {
public:
    using AlpacaException::AlpacaException;
};

/// True when a serial or HID device node that was resolved earlier no longer
/// exists (unplugged, or re-enumerated under another name). An empty path is
/// never "missing", and neither is a node whose status cannot be read for a
/// reason other than absence (EACCES on a parent directory, ELOOP, a path too
/// long): only ENOENT / ENOTDIR say the node is gone, and any other error
/// must NOT route a Gemini or QHYCFW3 reconnect into the DTR-resetting probe
/// the rest of the policy exists to avoid (review of #660).
inline bool device_node_missing(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (status.type() == std::filesystem::file_type::not_found) return true;
    return ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory;
}

template <typename Info, typename TryConnect>
void connect_resolved(Info& info, bool& resolved, const ConnectionResolver<Info>& resolver, TryConnect&& try_connect,
                      const char* log_tag = "AutoDetect") {
    if (!resolver) {
        try_connect(static_cast<const Info&>(info));
        return;
    }
    if (resolved) {
        try {
            try_connect(static_cast<const Info&>(info));
            return;
        } catch (const StaleEndpoint& e) {
            // The endpoint a previous connect found is gone: scan again. Any
            // other failure propagated out of try_connect above.
            ALPACA_LOG_INFO(log_tag,
                            "Previously resolved endpoint is stale (" + std::string(e.what()) + "); scanning again");
        }
    }
    Info fresh = resolver();
    info = std::move(fresh);
    resolved = true;
    try_connect(static_cast<const Info&>(info));
}

}  // namespace alpacacore::util
