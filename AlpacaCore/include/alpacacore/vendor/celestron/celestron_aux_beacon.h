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

#include <chrono>
#include <string>
#include <vector>

namespace alpacacore::vendor::celestron {

// The SkyPortal WiFi adapter (Gen4, Telit WE310G4 module) broadcasts a UDP datagram every 2 s to port 55555 of its
// subnet's broadcast address, from source port 55555. Captured payload (2026-11-20, adapter in hotspot mode):
//   { "manufacturer":"Celestron" , "ssid":"Celestron-AFFC" , "ip":"1.2.3.4" , "module":"Telit WE310G4" , "Count":2016 }
// Count increments by one per beacon. A beacon goes missing now and then (4 s and 6 s gaps in that capture, about
// 1 interval in 20), so a 3 s window misses an adapter about 2.5% of the time and a 5 s window about 0.1%.
inline constexpr int kAuxBeaconPort = 55555;
inline constexpr std::chrono::milliseconds kAuxBeaconMaxWindow{10000};

struct AuxBeacon {
    std::string ip;      // numeric IPv4 source address of the datagram (not the payload's "ip" field)
    std::string ssid;    // the payload's "ssid", empty when absent
    std::string module;  // the payload's "module", empty when absent
};

// Listens on UDP `port` (all IPv4 interfaces, SO_REUSEADDR, so another listener on the port is no obstacle) for the
// whole `window` (1 ms to kAuxBeaconMaxWindow) and returns each adapter once, in the order first heard. A datagram is
// a beacon when its payload is a JSON object whose "manufacturer" is "Celestron"; anything else is ignored. Throws
// AlpacaException InvalidValue for a bad window or port, DriverException when the socket cannot be opened or bound.
// Blocks the caller for the window; no thread is started.
std::vector<AuxBeacon> listen_for_aux_beacons(std::chrono::milliseconds window, int port = kAuxBeaconPort);

}  // namespace alpacacore::vendor::celestron
