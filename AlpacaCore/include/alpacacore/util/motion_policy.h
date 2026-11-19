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

// open-astro#2405: what "the mount is still" means to the shared motion guards (util/motion_guards.h), for the
// verified stop and the landing gate alike. A status flag alone is never enough: the iOptron HAE29C reports
// "stopped" 1-2 s before the motion ends (#2396 rig evidence, one GoTo read stopped with Dec still 4.7 deg from its
// target). So two position reads at least kStillSampleSpacing apart must agree within kStillToleranceArcsec on each
// axis. 500 ms is two status polls, long enough for a decelerating axis to show its motion; 30 arcsec over 500 ms
// still passes an axis that drifts at the sidereal rate (about 7.5 arcsec in that time) with tracking off.
constexpr auto kStillSampleSpacing = std::chrono::milliseconds(500);
constexpr double kStillToleranceArcsec = 30.0;

// open-astro#2405: the verified stop. The stop is sent, then the mount is read every kStopVerifyPollInterval; a
// round that has not verified after kStopVerifyBudget / kStopResendRounds sends the stop again, and the stop fails
// after kStopResendRounds stops (one round each) have not verified it: a stop that does not take within 3 s is a
// mount the driver no longer controls, and the client must hear that rather than a success. Resending covers a stop
// lost on the wire. Three rounds within 3 s leave room for a deceleration the status flag reports early (1-2 s on
// the HAE29C, #2396) while a mount that ignores the stop is reported within seconds.
constexpr auto kStopVerifyPollInterval = std::chrono::milliseconds(250);
constexpr int kStopResendRounds = 3;
constexpr auto kStopVerifyBudget = std::chrono::seconds(3);

// open-astro#2405: the slew deadline. No slew a supported mount runs takes 300 s (a meridian-crossing GOTO on the
// slowest of them is about two minutes), so a slew still running then is a fault, and the guard stops it. Within
// that cap a slew whose distance and rate are known gets twice the time it should take plus 10 s for the start and
// the deceleration; a slew with no known rate gets the cap alone.
constexpr auto kMaxSlewDuration = std::chrono::seconds(300);
constexpr double kSlewDeadlineFactor = 2.0;
constexpr auto kSlewDeadlineMargin = std::chrono::seconds(10);

// open-astro#2405: the MoveAxis dead-man. A MoveAxis runs until the client stops it, so a client that crashed or
// lost its link leaves the axis moving. 60 s is longer than any manual centring move and far shorter than the time
// an axis needs to reach a hard stop; the operator may set 5 to 300 s per device. This cap does not replace the
// client-silence watchdog (kClientSilenceStopInterval) and fixes no silence interval of its own: a shorter silence
// interval while a MoveAxis runs (#2396 AB-9) stays possible.
constexpr auto kMaxMoveAxisDuration = std::chrono::seconds(60);
constexpr auto kMoveAxisDurationMin = std::chrono::seconds(5);
constexpr auto kMoveAxisDurationMax = std::chrono::seconds(300);

// open-astro#2405: the landing gate's far miss. A mount that has come to rest more than 600 arcsec (10 arcmin) from
// its target on either axis did not land: the GOTO was refused, stopped at a limit or never ran. Smaller residuals
// are pointing error a re-GOTO or a sync corrects, not a failure. The Celestron Park arrival check and the SynScan
// landing refine limit keep their own 600 arcsec copies until those drivers adopt the guards (#2409, #2412).
constexpr double kLandingFarMissArcsec = 600.0;

// open-astro#2405: the sync bound. A sync re-defines where the mount points, so a sync far from where the mount
// believes it is (a wrong target, a client that synced to the wrong object, a plate solve of the wrong field) would
// throw every later GOTO off by that much, with no motion to notice. 5 deg covers any real polar alignment or cone
// error; the operator may set 1 to 30 deg per device. Independently a sync may change RA by at most 20 minutes of
// time: the RA rule is one constant here and one function in util/motion_guards.h (sync_ra_change_exceeds_bound), so
// a rule that scales with cos(Dec) near the pole would be a change in one place.
constexpr double kMaxSyncOffsetDegrees = 5.0;
constexpr double kSyncOffsetMinDegrees = 1.0;
constexpr double kSyncOffsetMaxDegrees = 30.0;
constexpr double kMaxSyncRaChangeMinutes = 20.0;

}  // namespace alpacacore::util
