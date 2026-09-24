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

namespace alpacacore::util {

// Two links, two timers -- read as one policy rather than two unrelated
// numbers. Both answer the same question ("motion is running and nobody who
// could still be watching it has been heard from -- how long before that
// stops being a glitch and starts being unattended?"), just for two
// different links:
//
// open-astro#521: how long a link may be gone before motion that survived it
// is treated as unattended rather than as a glitch to ride out. A USB
// re-enumeration or a briefly disturbed connector recovers in about one to
// three seconds, so five is comfortably clear of the common case while staying
// short enough that a runaway is stopped early. The asymmetry decides the
// value: stopping motion that was still wanted costs an aborted slew the
// client can re-issue, while preserving motion nobody is watching costs a
// mount driving into the tripod, so this rounds DOWN rather than up.
constexpr auto kRelinkMotionPreserveWindow = std::chrono::seconds(5);

// open-astro#547: the other link -- the one where the driver keeps command
// authority the whole time (the HTTP link never dropped) but no client has
// addressed the telescope while an axis was moving. A crashed client, a
// sleeping host or a dead network leaves a goto or a MoveAxis running with
// nobody watching it, and Alpaca is stateless HTTP: silence is observable
// only as "no request reached this device for N seconds while it was
// slewing". 30 s is long enough that ordinary polling gaps from NINA/PHD2/
// ConformU (roughly 1-10 s) never trip it, short enough to catch a runaway
// well before it does real damage, and configurable per operator (0
// disables it) because the right value depends on the client's polling
// cadence, not on anything this driver can know. Unlike the relink window,
// this one does NOT round toward stopping motion: the asymmetry here runs
// the other way -- a false trip on a long, deliberate goto costs a client an
// aborted slew it has to reissue, while the failure this exists to catch
// (nobody watching a live axis at all) has no other backstop.
constexpr auto kClientSilenceStopInterval = std::chrono::seconds(30);

}  // namespace alpacacore::util
