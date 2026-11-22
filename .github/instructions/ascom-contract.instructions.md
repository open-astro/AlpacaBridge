---
applyTo: "AlpacaCore/src/**,AlpacaCore/include/**,AlpacaCore/tests/**,AlpacaHTTP/src/**"
---

## ASCOM contract rules shared by drivers

Shared rules for every vendor, moved verbatim from `AGENTS.md`, which keeps the
core rules and routes here. A rule that applies to a second vendor belongs in
the matching shared file, not in a vendor file.

### Telescope Park / MoveAxis(axis, 0) are asynchronous initiators (all drivers)

ConformU 4.5 times `Park` (and every ITelescopeV4 initiator) against the 1 s STANDARD
target and completes it by polling `AtPark`/`Slewing`. Never block through a park slew.
The proven shape (SkyWatcher, then SynScan / Celestron in issue #208, the Celestron and SynScan flow now one `util::run_park_slew()`; Bisque pending on `fix/bisque-async-park`): snapshot the park target under the mutex, publish `slewing_cached_ = true` and a
`parking_` flag, then run the slew + completion poll + tracking stop + landing check (a mount more than 600" off the park target fails the park; SynScan repeats the GOTO once first) as a body in the driver's
operation slot (`util::AsyncOperation`, see Threads & shutdown in [driver-lifecycle](driver-lifecycle.instructions.md): the slot owns the thread, the reaping and the cancellable `ctx.wait_for()` between polls). `AtPark` and
`Slewing` flip in the same locked step (the public `Slewing` getter returns true while
`parking_`; the body polls the hardware through a separate `poll_hardware_slewing_locked`).
Park twice is a no-op; `Unpark`/`AbortSlew` during a park clear `parking_` (Unpark also
stops the axes; SynScan and Celestron supersede the body); disconnect and the destructor cancel + join through the slot. Keep any
pre-slew safety gate (Celestron alignment check) synchronous in `park()` so a refused park
still throws instead of silently never reaching `AtPark`. Drivers with native park commands
that return at once (iOptron `:MP1#`, OnStep `:hP#`, ZWO `:hP#`) need no task (OnStep has one for
a park or home that must first stop a running motion, or that the mount refused as in motion or
without a reason: OnStepX refuses it while an axis moves or brakes, so the call sends the stops or
the first try and returns, and a task waits for the mount to be still and parks or retries,
keeping a refusal for `Slewing`/`AtPark`/`AtHome`, #1430) — verified
on the iOptron HAE29C with ConformU 4.5.0 (Park 11.5 s of motion, initiator well under 1 s).
`MoveAxis(axis, 0)` must likewise send the stop and return; only add a background
stop-completion task (SkyWatcher) when the mount has a deceleration ramp longer than the
target. Hardware-free coverage lives in `tests/test_<vendor>_async_park.cpp` over
`FakeMountServer`: the fake must answer the alignment/position probes the driver gates on
(Celestron `J` → `1#`, SynScan/NexStar `e` → a parseable pair with Dec < 90°), or the park
is refused before the GOTO is ever sent.

### Sky-Watcher MoveAxis is sky-referenced (issue #297)

On the Sky-Watcher direct driver a positive `MoveAxis` rate on axis 0 moves the tube east (the reported RightAscension rises) and a positive rate on axis 1 moves it north (the reported Declination rises), in both hemispheres and on both pier sides, as `indi-eqmod` does. The hemisphere and Dec branch come from `sky_move_axis_rate_locked()`, which reuses `ra_axis_sign_locked()` and the pulse-guide Dec rule. A client needs no reverse toggle: NINA's "Primary Reversed" stays off. Goto, tracking and guiding are unchanged. Tests: `test_skywatcher_pointing.cpp`, `[moveaxis]`.

### iOptron MoveAxis signs (issue #2424)

The iOptron mount follows the same convention. A positive rate on axis 0 sends `:mw#`, which iOptron labels "RA+": the reported RightAscension rises and the tube turns east. A negative rate sends `:me#` ("RA-"). The letters mislead, but the sign is backed by three sources: iOptron's RS-232 v3.10 labels, the GOTO trim, where `:ZQ` ("RA-") lowers RA on the HAE16 and HAE29C, and INDI's finding that `:mw#` raises RA on a CEM120 (indilib/indi `91bda0d00`). A positive rate on axis 1 is meant to raise the reported Declination on both pier sides. The driver sends `:mn#` when the `:GEP#` pier digit is 0 and `:ms#` when it is 1, through the Dec pulse-guide polarity (`dec_guide_inverted_`, `should_flip_dec_for_pier_locked()`). The HAE29C rig check of #2424 (2026-11-20, over USB and over Wi-Fi) confirmed both signs on both pier sides: a positive rate raised the reported RightAscension (the tube turned east) and the reported Declination (toward the pole), sending `:ms#` at pier digit 1 and `:mn#` at pier digit 0. The southern hemisphere is untested on any rig. NINA's "Primary Reversed" stays off. Tests: `test_ioptron_telescope.cpp`, `[moveaxis]`; the fake steps its position on each arrow command (`fake_ioptron_mount.h`).

### TargetRightAscension and TargetDeclination are independent (all telescope drivers)

ASCOM treats the two target properties as separate: each getter must refuse a read
until **that property itself** has been written. The current Master Interfaces
documentation (revision 1.0.27) specifies `InvalidOperation`; this is a documentation
revision, not a new telescope interface version. The direct CGX backend follows it.
The existing Celestron NexStar, SynScan, iOptron, OnStep, ZWO, Bisque and SkyWatcher
backends and their tests still use `ValueNotSet`, a known migration gap not repaired
by the CGX PR. Do not copy that legacy error into new implementations.
Every driver therefore carries `target_ra_set_`
**and** `target_dec_set_`, each set only by its own setter. One flag for both is a
review-blocking regression — it was the original shape in all seven drivers and took two
passes to remove (#304, then #346).

While disconnected, a target read, `SlewToTarget`, `SlewToTargetAsync` and `SyncToTarget`
answer `NotConnected` before the unset-target error, in every backend (#2075).

Protect each target value and flag with the driver's mutex. The three target-consuming
operations snapshot both values and flags while holding it, then release it before calling
the coordinate operation that acquires the same non-recursive mutex.

The paths that legitimately define both coordinates at once set or clear both: the slew
and sync *coordinate* forms, the post-slew position-override and arrival reads, and the
connect/disconnect resets. A pulse-guide position estimate is internal state, not either
public target property (SynScan keeps it separately). `SlewToTarget`,
`SlewToTargetAsync` and `SyncToTarget` require the pair when supported. CGX reports
`InvalidOperation` for unset targets; the legacy backends retain `ValueNotSet` pending
migration — Celestron and SynScan were both missing that check on the
synchronous form, which one shared flag hid, since any target write made it pass.

Hardware-free coverage per driver: while connected, read each property before any write
and expect the backend's error above; write RA alone and check Dec still throws while RA reads
back, confirm supported `*ToTarget` calls refuse the half-set pair with the same error,
then write Dec and expect both. A driver whose setters write to the mount (iOptron) needs
the fake rather than a disconnected instance. Disconnected getters still return
`NotConnected` first.

### Equatorial slews require Tracking (all telescope drivers)

ITelescopeV4 raises `InvalidOperation` for `SlewToCoordinates`, `SlewToCoordinatesAsync`, `SlewToTarget` and
`SlewToTargetAsync` while `Tracking` is false (#775 SynScan, #1274 every other telescope
driver). The check runs after argument validation (`InvalidValue`) and the parked check, and before the driver cancels
or sends anything, so a refused slew leaves a park, goto or pulse guide in flight running. Read Tracking the way the
driver's own `Tracking` getter does, a commanded `Tracking=false` still in flight included, with two exceptions that do
not count as off: a motion state the getter cannot tell apart from Tracking off (iOptron `:GLS` reports a GOTO in
place of tracking, so its guard refuses only a stopped mount; guiding and a meridian flip read as Tracking true there
since #2186), and a retarget of the
driver's own slew still in flight (ZWO `:GU`, Bisque `IsTracking`: nothing says what they report during a GOTO).
Syncs and Alt/Az slews are exempt. A driver test of an equatorial slew turns Tracking on first; do not leave it off
and assert only that the slew threw. An Alt/Az slew test in a driver that refuses Alt/Az slews while Tracking is true
(ITelescopeV4; OnStep and the iOptron mount since #1377) turns Tracking off first instead.

### Telescope refusal order (all mount drivers)

A telescope member refuses in this order: argument (`InvalidValue`), connected, parked, link fault, tracking (#2257). Out of range always throws `InvalidValue`; no mount driver clamps a client argument (drivers that still drift are listed in `kProbeExemptions` until their fix lands). The parked-against-fault order is per driver (Sky-Watcher checks the fault first). The contract sweep probes PulseGuide, MoveAxis, SlewToCoordinates(Async), the guide-rate setters (#2257) and SideOfPier, TrackingRate, the target setters, SlewToAltAz(Async), SyncToCoordinates, SyncToAltAz, the rate-offset setters and DestinationSideOfPier (#2287) disconnected, connected and parked; `kProbeExemptions` in `test_contract_sweep.cpp` lists each known failure (deleted by the fix) and each member a driver does not implement. A PulseGuide `Duration` above the driver's bound is `InvalidValue`, on a camera too; a driver whose hardware sets no bound of its own uses `kDefaultMaxPulseGuideMs` (60 s, `alpaca_defs.h`, #2292), and none clamps it.

The order lives in one module, `util/telescope_command_gate.h` (#2258): `check_pulse_guide`, `check_move_axis`, `check_equatorial_slew`, `check_guide_rate` and `check_tracking_rate` (#2418), each also as an `_arguments` half and a `_state` half. The argument half takes the driver's `TelescopeCommandLimits` only: the protocol limits and the InvalidValue texts (each driver keeps its own words) are data, so a driver replaces its argument helpers with a limits value. The state half calls the driver's own connected, parked, fault and Tracking checks in order, through a checks object of the driver's, because those checks read the mount or its link (OnStep's `:GU#` read for an unread status, a fault check that closes a stale link); the parked-against-fault order per member family, the MoveAxis stop epsilon and whether a stop skips the fault check are limits too. The gate does no I/O and takes no lock. All eight mount drivers call it: OnStep, Sky-Watcher, NexStar, SynScan, iOptron, ZWO AM, Bisque and Celestron CGX; the six that implement a TrackingRate write call `check_tracking_rate` for it (Sky-Watcher and CGX its argument half only, NexStar and SynScan do not implement the write). Sync, Alt/Az slews, rate offsets, targets and DestinationSideOfPier are not in it yet; each driver still checks them itself.

The motion guards that make telescope motion fail closed live in one module, `util/motion_guards.h` (#2405), and every threshold they use lives in `util/motion_policy.h`, next to `kRelinkMotionPreserveWindow` and `kClientSilenceStopInterval`; a driver that adopts a guard keeps no copy of its own. The guards are the verified stop (`run_verified_stop`: status "no motion" alone never verifies, two position reads `kStillSampleSpacing` apart must agree within `kStillToleranceArcsec`, at most `kStopResendRounds` stops in `kStopVerifyBudget`, else `Slewing` raises `kStopNotVerifiedText`), the slew deadline (`slew_time_budget`) and the MoveAxis cap (`validate_move_axis_time_limit`), both enforced by `enforce_motion_deadline`, which runs the verified stop; the landing gate (`LandingGate`: a still mount more than `kLandingFarMissArcsec` off its target is an error, a moving one never is), the sync bound (`check_sync_bound`, InvalidValue) and the relink stop (`RelinkMotionStop`). Each is a clock-free value or a free function over the driver's callbacks: it takes no driver lock and does no I/O of its own (decision 0016), waits only through the slot context on the task clock (decision 0005) and sends nothing once superseded (decision 0006 rule 6). The Celestron CGX uses the sync bound and the stillness check under its AbortSlew and Unpark stop (#2411; it keeps its own stop rounds, see the Celestron file); iOptron adopts them in #2396 and the other mounts in #2406-#2412.

### ASCOM exception vocabulary (pick the right one — ConformU checks it)

| Throw | When |
|---|---|
| `InvalidValue` | Bad argument / out-of-range id or index — **even while disconnected** (precedes the connection check). |
| `NotConnected` | Any operational property/method called while disconnected. |
| `PropertyNotImplemented` | A property the hardware genuinely lacks (e.g. `Offsets` list, `SubExposureDuration`). |
| `MethodNotImplemented` | A method the hardware lacks (e.g. `PulseGuide` when `CanPulseGuide` is false). |
| `ActionNotImplemented` | `Action()` called with a name the driver does not list in `SupportedActions` (0x40C, ASCOM's `ActionNotImplementedException`). An unsupported `CommandBlind`/`CommandBool`/`CommandString` is `MethodNotImplemented`, not this. |
| `NotImplemented` | A generic unsupported action (e.g. `set_temp_comp(true)` with no temp-comp support) — never `DriverException` for "not supported". |
| `InvalidOperation` | Valid call, wrong state (e.g. changing readout mode/geometry mid-exposure), including a value the client can set and has not yet (the site, [decision 0019](../../docs/decisions/0019-connect-without-site.md); `UTCDate`). |
| `DriverException` | A genuine internal/driver failure, including a device that did not answer: a timeout, or a value only the device supplies (status, position, telemetry) that it has not answered since the connect (ASCOM: "Includes communication errors"; [decision 0010](../../docs/decisions/0010-snapshot-served-state.md) rule 8). Never `InvalidOperation` for a device that did not answer, and not a stand-in for any of the above. |

All map to HTTP 200 with a non-zero `ErrorNumber` — clients read the body, not the status.

**A Focuser `Move` outside `0..MaxStep` is the one out-of-range argument that is not an `InvalidValue`** (every focuser driver, #2028): ConformU's focuser tester moves below 0 and above `MaxStep` and expects the focuser to stop at that limit ("Move should fail gracefully by just moving to position MaxStep; it should not throw an exception"; an `InvalidValue` is logged as an Issue), so a focuser driver clamps the target to `0..MaxStep` after its connection check and sends the limit, and its `contract_sweep.h` entry is `with_clamping_focuser`. The iFR, iEAF, QHY, ToupTek and ZWO focusers threw `InvalidValue` until 2026-11-04 and passed anyway, because every ConformU release up to 4.6.0-rc.1 ran the range sweep (`Move - To 0`, `Below 0`, `To MidPoint`, `To MaxStep`, `Above MaxStep`, and since the fix `Halt While Moving`) only for a focuser with temperature compensation (ConformU `a226f298`, 2026-10-28, unreleased when the five were fixed); a focuser report made with 4.6.0-rc.1 or earlier on a focuser without `TempComp` proves one short move and `Halt`, nothing at the limits. The ZWO EAF still refuses every Move while the unit's stored max step is 0 and none is configured ([ZWO](zwo.instructions.md)).

A camera's "no image yet" answer (`ImageArray` before any exposure, or while one is still running) is an `InvalidOperation` the contract requires, not a fault: throw it with `AlpacaException::asked_too_early(message)`, so the router logs it at DEBUG instead of ERROR (issue #1167). Never use it for an `InvalidOperation` that reports a failed, stopped or racing exposure. The tier-2 contract sweep checks the first case on every camera fake.

### Telescope refraction, site elevation and DestinationSideOfPier (all telescope drivers)

- `DoesRefraction` throws `PropertyNotImplemented` on read and write in a driver that neither knows nor computes refraction (ITelescopeV4). Never store the flag and read it back: it would claim a correction the driver does not apply. OnStep reads it from the firmware.
- `SiteElevation` throws `InvalidOperation` until the device config or a client sets it, never 0 m ([decision 0019](../../docs/decisions/0019-connect-without-site.md)).
- `ApertureDiameter`, `ApertureArea`, `FocalLength` and `SlewSettleTime` (read and write) throw `NotConnected` while disconnected, after the `SlewSettleTime` write's `InvalidValue` check; they are the client's values, so a latched fault does not refuse them. `set_aperture_diameter()` and `set_focal_length()` are not ASCOM members (ASCOM has no setter for either): the catalog factories and `updatedevice` (#2057) inject the configured optics with them, connected or not (the CGX backend still refuses them while disconnected; its factory passes the optics in its connection info), and a test reads the injected values through a seam, never the ASCOM getter. The contract sweep probes all five members.
- `DestinationSideOfPier` throws `InvalidValue` for RA outside [0, 24) or Dec outside [-90, 90], before the connection check.
- RA, Dec and site range checks, local sidereal time, the hour and degree wraps, the shortest RA delta and the destination pier-side rule go through `util/pointing.h` (`validate_ra`, `validate_dec`, `validate_ra_dec`, `validate_latitude`, `validate_longitude`, `local_sidereal_hours`, `wrap_hour_angle`, `pier_side_for_hour_angle`, and for a German mount whose controller keeps its side past the meridian `gem_destination_pier_side` and `gem_pier_side_reaches` with the `GemPierModel`/`GemPierLimits`/`GemPointing` inputs, #2179); a telescope driver never defines its own validator, sidereal-time, hour-angle-wrap or pier-side function, so the `InvalidValue` text is the same on every mount. Inline RA wraps and hand-written shortest-RA-delta code inside larger expressions (Celestron NexStar, iOptron, SynScan, ZWO) are not converted yet. Exceptions: CGX `cgx_local_sidereal_time_hours` builds on `gmst_degrees` and adds its own equation of equinoxes; CGX derives its pier side as the `CgxPointingSide` enum from the hour angle, not as an int; Bisque keeps a `detail::validate_ra_dec` forwarder to `util::validate_ra_dec` until its test call is removed.
- A `UTCDate` read the mount clock cannot answer throws `InvalidOperation` until a supported successful time write establishes it; the host clock does not stand in for the mount's (ITelescopeV4 `UTCDate`, the ASCOM time FAQ). Internal pointing fallbacks never initialize public UTCDate. Sky-Watcher's direct host-clock backend and the Celestron CGX direct USB backend (#2166), whose controllers store no time, follow [decision 0001](../../docs/decisions/0001-skywatcher-pointing-clock.md) instead: `UTCDate` reads the host clock that `SiderealTime` and pointing use. Bisque samples the remote TheSkyX computer clock only in UseComputerClock mode (see [vendor notes](bisque.instructions.md)).
- A mount that keeps its own clock writes the host clock into it only while `util::HostClockTrust::trusted()` (`host_clock_trust.h`: the kernel reports NTP sync, or this process saw a client UTCDate step or confirm the clock, a Sync Time, or a realtime step; a hardware-RTC boot does not count), and re-syncs it from `TelescopeDriver::on_host_clock_changed()`, which the server's 1 s timer calls on a step or on the trust edge (`Router::check_host_clock()`). The hook runs on the timer thread with no lock held: it is `noexcept`, does no I/O, joins nothing, never waits for a driver lock (a held one leaves the change pending), and catches what `AsyncOperation::start()` throws. It is not wait-free: `start()` itself can wait up to `kStaleReapTimeout` (2 s) when three replaced bodies are still running, as on a blocked link (#2432). iOptron (#2419), ZWO AM (its connect and its GOTO setup's time write), OnStep, Celestron NexStar and SynScan follow it: an untrusted connect skips the time write with one WARN and keeps the site writes and the mount-clock seed. Each re-sync runs on its own operation slot, checks driver motion inside the write's own exchange and waits it out in `util::kHostClockResyncMotionPoll` steps for at most `kHostClockResyncMotionBound`, lets a client `UTCDate` write made after it was decided win (a write generation), and is drained by a disconnect. A hook that finds the transition lock held stores a pending flag that `set_connected()` drains after it unlocks, so a change is never lost. A hand controller that keeps a star alignment (Celestron NexStar, SynScan) is not re-synced while aligned, because a clock change shifts its pointing model: the re-sync asks `J` in its own write turn and sends no `H` when the hand controller is aligned or the query fails. Sky-Watcher direct and Celestron CGX keep no mount clock. `HostClockTrust` is separate from `HostClock`'s client-step flag, and an observed step never calls `HostClock::mark_stepped()`, which would write the unverified time into the hardware RTC.

### A failed async operation body keeps its error (all drivers)

The ASCOM async FAQ makes a failed async command (a goto, a slew, a cover move, an exposure) a failed
operation. The body keeps its error, and the device's completion property (`IsMoving`, `Position`,
`CoverState`/`CalibratorState`, `ImageReady`, `Slewing`, ...) raises it until the next command of that kind
or a reconnect clears it. Never log the failure and report done, and never answer the property with a live
read that invents a value. Clear the kept error in the connect/teardown path as well, and test each clear
site on its own. Precedent: the QHY CFW3 `Position` (`move_failure_`), the Gemini flat panel.

### UniqueID is the router's stored ID (all drivers)

A driver does not build its `UniqueID`. The router assigns each configured device the ID stored
in its config entry (`uniqueId`; never changed once stored) through `AlpacaDriver::assign_unique_id()` before it
registers the driver. The router keeps a stored or client-sent ID that `is_keepable_unique_id()` accepts
(a version 4 UUID, or 12 to 128 printable ASCII characters that are not a device-number ID such as
`SkyWatcher_0`; `ZWO_UID_<hex>` and `my-rig-focuser-A` pass) and that no other entry holds; only an entry
with none, a device-number ID or a duplicate gets a minted UUID, once (decision 0018). The base
class's non-virtual `get_unique_id()` returns the assigned ID. A new driver
implements only `fallback_unique_id()`, which answers for a driver a unit test builds directly; never
key persistent state on the fallback. A driver that keys state on its ID (the rotator Sync offset)
reads `get_unique_id()`, or `assigned_unique_id()` while holding its own mutex. State saved under an
older ID built from a device number or SDK index moves in an `adopt_legacy_state()` override, which
the router calls only at the start that gives the entry its first GUID, never at connect: a device
configured later on the same number must not inherit it. State under an older ID that names the
hardware (a serial) may move at connect. Both use `RotatorSyncOffsetStore::load_or_adopt()` or its
equivalent. The reasons are in [decision 0018](../../docs/decisions/0018-persisted-unique-id.md).

### Name is brand and model (all drivers)

`get_name()` is the device's brand followed by its model, nothing after it: `Sky-Watcher EQM-35 Pro`,
`ZWO EAFN`, `QHY CFW`. Before the model is read it is the brand followed by the product family
(`ZWO EAF`) or the ASCOM device type (`Sky-Watcher Telescope`; never `Mount`). A model string from an
SDK or a device goes through `util::branded_name(brand, model)` (`alpacacore/util/device_name.h`), which
adds the brand only when the string does not start with it, and an SDK's ` (...)` connection-mode qualifier
(libgphoto2's `(Control)`) is dropped with `util::without_qualifiers()`. A connection path, protocol, board revision,
calibration state or host camera goes in `get_description()` (no device I/O), which the web UI shows; two
drivers reaching one device (EQMOD and SynScan on one mount) give it the same name. A camera-attached dew
heater or thermal switch is the camera model plus `Dew Heater` or `Thermal Switch`. A new vendor adds its
brand to `name_brands_of()` in `test_contract_sweep.cpp`, whose two name cases check every driver. The
reasons are in [decision 0024](../../docs/decisions/0024-device-names.md).

### Action names are case-insensitive (all drivers with actions)

ASCOM treats `SelectWheel`, `selectwheel` and `SELECTWHEEL` as the same action: the
`ActionName` parameter of `Action()` is case-insensitive, and the names in
`SupportedActions` may use any casing. Match the requested name against the supported
list without regard to case, in `action()` and in `can_action()` alike, and throw
`ActionNotImplemented` only when no supported name matches. The drivers with
actions already do this: the Player One camera (`to_lower_copy`), the Celestron
focuser (`same_action`), the iOptron and Celestron mounts' `gps_status`
(`util::is_gps_status_action`) and the ZWO EAF, ToupTek AAF and iOptron iEAF
focusers' `SetZero` (`util::same_action_name`, `alpacacore/util/action_name.h`).
A new driver with actions does the same.

### The not-implemented answers are `AlpacaDriver`'s defaults (all drivers)

`AlpacaDriver` (`alpacadriver.h`) defines `get_supported_actions()` (empty), `can_action()` (false),
`action()` (`ActionNotImplemented` naming the action) and the three `command_*` (`MethodNotImplemented`,
0x400) inline, issue #1757. A driver overrides one of them only to give it real behaviour (a vendor action, a
`Command*` pass-through to its protocol), never to restate the default: a copied override was how the
message and the error spelling drifted across 40 files.

### Camera ROI alignment (all camera vendors)

Every camera SDK constrains ROI geometry, and the pattern is the same everywhere:
**keep the client-requested values for the Alpaca interface, align down for the SDK
call, and pad outputs if needed** — ConformU's read-back checks must see the requested
geometry. Per-SDK constraints (the only vendor-specific part): ZWO width%8 / height%2
after binning; SVBONY width%8 / height%2; Player One width%4 / height%2; ToupTek even
sensor-resolution width/height/offset AND an even binned output size (see the ToupTek
odd-bin-factor note for the 3×3 subtlety and the #977 odd-NumX rule).

### Camera argument order (all camera vendors)

A camera member refuses in one order on every brand (#2259), so a client writes one error handler:

- **`StartExposure(duration, light)`**: (1) a NaN, negative or infinite duration, or one too large to convert to
  microseconds, is `InvalidValue` (a static cap such as GPhoto's 3600 s belongs here too); (2) `NotConnected`; (3) a
  duration below `ExposureMin`, 0 included, runs at `ExposureMin`, and one above `ExposureMax` is `InvalidValue`; (4) a
  frame (NumX, NumY, StartX, StartY, bin) that does not fit the sensor is `InvalidValue`; (5) an exposure running or
  starting is `InvalidOperation`.
- **`BinX`, `BinY`, `NumX`, `NumY`, `StartX`, `StartY` setters**: (1) a bin, NumX or NumY of 0 or less, or a StartX or
  StartY below 0, is `InvalidValue`; (2) `NotConnected`; (3) a bin the camera does not support is `InvalidValue`; (4) an
  exposure running or starting is `InvalidOperation`. The setters do not check the sensor size: StartExposure does
  (ConformU sets a NumX beyond the sensor and expects StartExposure to refuse it).
- **`SetCCDTemperature(temperature)`** (#2487): (1) a NaN or infinite setpoint, or one outside a static bound, is
  `InvalidValue`; (2) `NotConnected`; (3) a range only the connected device knows is `InvalidValue`; never clamp. A
  range is enforced only where the SDK or device reports it: Player One `caps_.target_temp_min/max` and the ZWO and
  SVBONY control caps (`MinValue`/`MaxValue` of the target-temperature control) at step 3, QHY's static bounds
  (above -273.15, at most 60) at step 1. Never invent a number for a camera that reports none. A camera with no
  cooler (GPhoto, the iGuider) does not implement the member.

The contract sweep probes these disconnected, connected and while an exposure runs (started through each camera's
tier-3 recipe). `kProbeExemptions` in `test_contract_sweep.cpp` lists each camera that still drifts, and
`kExposingRefusalExemptions` each camera that takes a good write or a second StartExposure while exposing; the fix
deletes its rows.

The value checks of that order live in one module, `util/camera_frame_request.h` (#2261): `check_exposure_duration`
(StartExposure step 1), `exposure_microseconds` (step 3), `check_frame_on_sensor` (step 4; `frame_sensor_misfit` is
the same check as a value, for a driver that answers one text or only skips an SDK write, as ZWO does), and for the
setters `check_bin`, `check_frame_size` and `check_frame_start` (step 1) and `check_bin_supported` (step 3), and for
SetCCDTemperature `check_ccd_setpoint` (step 1; the cooled drivers ZWO, SVBONY, Player One with iCAM, ToupTek with
Altair and QHY call it, QHY with its static `CcdSetpointBounds`). A camera driver calls them at those points and never writes its own copy. It supplies the connection and exposing checks, the
device lookups (ExposureMin and ExposureMax, the supported bins, the sensor size at the current bin) and its own
InvalidValue texts (`CameraRequestTexts`; the codes are the same on every camera). Only the SDK alignment above and
vendor limits (GPhoto's 3600 s cap and nearest shutter speed, the iGuider exposure steps) stay in the driver. Every
camera driver calls it: ZWO, SVBONY, Player One (with iCAM), ToupTek (with Altair), QHY, GPhoto and the iGuider. Three
keep a check for a vendor reason, named at the call: QHY and GPhoto their ExposureMax check in seconds (QHY's range
may be unreported, GPhoto maps a duration to a shutter speed) and QHY its per-bin SDK query; the iGuider its subframe
value checks, whose text names the value.

### Camera PercentCompleted and cooler members (all camera vendors)

- **AlpacaBridge supports `PercentCompleted` only while `CameraState` is `Exposing`.** ICameraV4 optionally permits
  Waiting, Reading and Downloading too, and requires `InvalidOperation` in every other state. The project's
  exposing-only policy lives once, in `CameraDriver::get_percent_completed_checked()`;
  the GET route and the DeviceState builder both call it, never the virtual `get_percent_completed()` (the raw
  driver value, 0..100). It reads `CameraState` and the raw value first, so a link fault or `NotConnected` still
  surfaces before the `InvalidOperation` (ADR 0006, 0009). DeviceState therefore omits `PercentCompleted` while the
  camera is not Exposing, on purpose, and carries it as an integer while Exposing. A new camera driver adds
  nothing for this; it only keeps `get_percent_completed()` honest while Exposing.
- **`CoolerOn` (get and set) and `CoolerPower` raise `PropertyNotImplemented` on a camera without a cooler**
  (ICameraV4: "if not supported"), gated on the capability the driver already uses (ZWO and SVBONY: the SDK
  control, Player One and QHY: the camera's cooler flag, GPhoto: none). Never answer `false` or `0.0` for a
  member the camera does not have. With the cooler, `CoolerPower` is `0` when `CoolerOn` is false.
- **An SDK failure reading `CoolerPower` is the driver's normal error, never `0.0`.**

### Camera LastExposureDuration and LastExposureStartTime (all camera vendors)

- **Both raise `InvalidOperation` until an image has been successfully acquired** (ICameraV4: "if no image has yet
  been successfully acquired"), never `ValueNotSet`. A failed exposure publishes nothing and never erases the
  values of an earlier success; only a new success replaces them. Keep the exposure in flight in pending fields
  (`PercentCompleted` reads them) and publish under the state lock when the image is acquired
  (`util::LastExposure`, `util/last_exposure.h`, for supported pairs); clear them on disconnect. While disconnected both raise `NotConnected` first
  (`ensure_connected()`); a GPhoto abort during the decode re-checks `exposure_active_` under `mutex_` and publishes nothing.
- In cameras that hold the state lock across initialization, fast-fail disconnected reads before taking that
  lock, then recheck connection under it. The first check avoids waiting behind connect I/O; the second closes
  the disconnect race. iGuider's connect I/O already runs outside its leaf state lock.
- **The duration is the actual one** (ICameraV4: "may differ from the requested duration"): report the applied
  register or shutter value after rounding/clamping, or monotonic shutter-open elapsed time for a driver-timed
  exposure. Configuration, mirror settling, readout, download and decode do not count as integration.
- **The start time is when the camera started to collect photons**, not the `StartExposure` request. Capture
  candidate UTC at the acquisition boundary after deferred configuration, and refresh it for a retried attempt;
  use `TaskClock` for elapsed time and progress, never as UTC. Host SDK-call timestamps still need rig evidence
  for sensor-start accuracy. If no trustworthy source exists, the optional property may raise
  `PropertyNotImplemented` rather than invent a timestamp; document that vendor's limitation.

### Camera values the SDK cannot give (all camera vendors)

- **`ElectronsPerADU` and `FullWellCapacity` raise `PropertyNotImplemented` when the SDK has no figure that follows the
  current gain and mode** (ICameraV4: "if not available"); never `1.0`, `0.0`, the ADU saturation (MaxADU) or a static
  figure. Once connected they raise it (`NotConnected` first while disconnected). Today: QHY, ToupTek (and Altair),
  SVBONY, GPhoto and ZWO (the SDK's `ElecPerADU` is one static number) raise it for both; Player One raises it for
  `FullWellCapacity` and returns the real per-gain `POA_EGAIN` for `ElectronsPerADU`, raising it only when the camera
  lacks `POA_EGAIN` or the read fails. A client such as Ara stores whatever it is told, so a missing value beats a false
  one. Restore a return only when a per-gain source turns up.
- **`SensorName` is never a placeholder**: ICameraV4 makes it mandatory and says to "return an empty string if the name
  is not known". It never raises PropertyNotImplemented, and a literal such as `"<vendor> Sensor"` is a made-up
  name. Return `""`.

### Mount `gps_status` action (iOptron, Celestron)

`Action("gps_status")` returns the JSON string `{"fix":<bool>,"latitude":<deg|null>,"longitude":<deg|null>,
"elevation":<m|null>}` so a client can tell that the site came from the mount's own GPS receiver, not from the bridge
config. `latitude` and `longitude` are the receiver's reading and are `null` unless `fix` is true; `elevation` is `null`
where the mount does not report one (iOptron and Celestron never do). It is listed in `SupportedActions` only while
connected, and on Celestron only when the bus probe found the GPS module; `NotConnected` otherwise. The format lives in
`util::format_gps_status` (`util/gps_status.h`); iOptron reads the `:GLS#` GPS digit and site, Celestron the AUX 0xB0
GPS module (see the vendor files). OnStep does not offer it yet.

### Telescope `findhomeissensorsearch` action (every telescope driver)

`Action("findhomeissensorsearch")` returns the JSON string `{"sensor_search":<bool>,"method":"<word>","model":"<model>"}`
so a client (Ara) can tell whether a completed FindHome puts the mount at its true home whatever position it believed
before. `sensor_search` is true only when FindHome is a search on the mount's own home or zero sensors; a slew to a
stored or power-on home is false, since a lost or wrong position lands it somewhere else. The action reads only state
the connect cached, never the wire. It is listed in `SupportedActions` and `CanAction` is true only while connected;
`NotConnected` otherwise. The format and the method words live in `util/findhome_sensor_search.h`; `model` is the
mount model or code the answer depends on, else `""`. A client treats a missing action or a reply it cannot parse as
false, and even when true trusts AtHome only after a FindHome it sent in this session that ended with no error.

| Driver | `sensor_search` | `method` | Why |
| --- | --- | --- | --- |
| iOptron | true only for the HAE29C EQ (`0036`, `zero_search_active()`) | `zero_sensor_search` / `goto_stored_home` | The HAE29C runs the `:MSH#` zero sensor search (#2420); every other model's `:MH#` slews to the stored home. |
| Celestron NexStar | true only for a home-switch model (CGE Pro 13, CGX 23, CGX-L 24) whose RA and Dec home switch devices (AUX 0x30, 0x31) also answered at connect | `home_switch_search` / `no_home_sensor` | FindHome is `MC_LEVEL_START` on both axes, which drives each to its switch. The model is required too because a hand controller may answer the probe for a device that is not there (an AVX, for example). |
| Celestron CGX USB | true | `home_switch_search` | Connect refuses a CGX whose home switches do not answer. |
| Sky-Watcher direct | true when both axes report the home index feature (bit 0x04, the Wave) | `home_index_search` / `goto_stored_home` | AutoHome hunts the index sensors; other boards' FindHome is the count-frame goto to the power-on index. |
| Bisque / TheSkyX | false | `external_find_home` | TheSkyX's Find Home on a Paramount is likely a homing-sensor search, but the driver cannot tell which mount TheSkyX drives and no Paramount run has verified it. |
| OnStep | false | `goto_stored_home` | `:hC#` slews to the stored home; optional OnStepX home sensors are a build option the driver cannot read. |
| ZWO AM | false | `goto_stored_home` | `:hC#` slews to the stored home. |
| SynScan | false | `not_supported` | The handset has no home command (CanFindHome false). |

Tests: the `[findhome-sensor]` cases, one per driver.

### Camera-attached thermal switches pick their camera (ZWO, ToupTek, Player One)

A Switch that drives a camera's dew heater or fan (ZWO Dew Heater, ToupTek thermal,
Player One thermal) binds through `util::choose_thermal_camera()`
(`alpacacore/util/thermal_camera_choice.h`), never a bare position in the
enumeration. A bare index bound the 5.0.0~beta4 rig's dew heater to an ASI290MM Mini
(no heater) at index 0 beside the ASI2600MM Pro at index 1, and the refusal named
neither camera. The rules:

- **`cameraIndex` -1 is Auto**: the one attached camera with the controls; none or
  several is a refusal that lists them. A `serialNumber` (ZWO, Player One) binds that
  camera only and never falls back to the index. A pinned index binds that camera
  whatever the listing says, so the driver's own probe decides (the ToupTek tail light
  has no flag), and an empty probe throws `util::thermal_capability_refusal()`, which
  names the camera to pick. Saved switches keep their index.
- **One listing per vendor**, `list_<vendor>_*_cameras()`, used by the driver's connect
  AND by `GET /management/v1/thermalcameras`, through
  `DeviceCatalog::add_thermal_camera_lister()` so the router includes no vendor header
  for it. A listing that must open cameras (ZWO controls and serial, Player One
  heater/fan) opens through the vendor's ref-counted wrapper and closes again.
- **The web form is the `createThermalCameraPicker()` dropdown** (model in `format.js`,
  `thermalCameraOptions`) and it always asks: nothing is preselected for a new switch,
  cameras without the controls are disabled, the submit refuses until one is picked,
  and Auto is not offered (a rig can carry a 2600MC, a 6200 and a 2400, all with
  heaters, and a guess that follows a camera swap is the bug this replaced). Its select
  has no `name`, so it never reaches `FormData`; the submit reads `picker.read()`. A new
  camera-attached switch for another vendor reuses all three pieces.

### FilterWheel semantics (all vendors)

- **`Position == -1` IS the ASCOM "moving" sentinel.** SDKs that report `-1` while in
  motion (ToupTek AFW) or a distinct moving state (Player One `PW_ERROR_IS_MOVING`)
  map directly onto it — pass it through; don't invent a separate is-moving flag, and
  never translate the SDK's moving-read into an exception on the read path.
- **Names must be non-empty** — default `"Filter 1..N"`; names and focus offsets are
  settable while disconnected.
- **Slot data lives in `util::FilterWheelSlots`** (`filterwheel_slots.h`, issue #1991): the
  slot count is known from a successful connect until the next Disconnect (WandererAstro:
  always, its lineup is 8-slot). While it is known, names and offsets must have one entry per
  slot and a lone shorthand token expands; while it is not, they are taken as given and
  resized, with a warning, at the next connect. A lost link keeps the count until the Disconnect.
- **DeviceState includes operational fields only** (e.g. `Position`); omit `Connected`
  for ConformU compatibility.

### GPIO power-switch / soft-PWM drivers (general rules)

All GPIO 12V power-port Switch drivers (ZWO ASIAIR Pro / Plus CM4 / Plus RK3568,
ToupTek StellaVita, iOptron iMate, and any future board) share these rules; the vendor
notes carry only the pin map and per-board deltas.

- **One driver for every board above** (issue #2264):
  `util::PowerPortSwitchDriver<Wrapper>` (`alpacacore/util/power_port_switch_driver.h`) holds
  the connect and disconnect gates, the power-on state, the value checks and the error order,
  so a fix to the rules below lands there once. Each vendor's factory builds a plain-data
  board profile (strings, per-port metadata, the power-on state) and passes its own port
  wrapper's constructor arguments; the wrapper stays the vendor's. A future board adds a
  profile and a wrapper, not a driver.
- **libgpiod v2 only** (`libgpiod-dev (>= 2.0)`, `libgpiod3` runtime): one
  `gpiod_line_request*` owns all lines together; values go through
  `gpiod_line_request_set_value`. Never port back to the v1 per-line API. The daemon
  user needs `gpio`-group access to the chip/char device via a udev rule.
- **A connect reads the ports and changes none** (issue #300): these boards drive
  the DC ports high at boot (`gpio=...=op,dh[,pu]`), but a user may have changed
  that or a previous session left a port off. A libgpiod wrapper claims its lines
  through `util::claim_outputs_at_current_level()` (request as-is, read, reconfigure
  to outputs at the read levels) and its cache follows the line through
  `util::reconcile_cached_value()`; never request a line with an initial value. The
  RK3568 cannot read its pads (`GET_LEVEL` lies), so it keeps its "on" cache.
- **Power-on state is applied once per server start, never at connect**
  ([decision 0021](../../docs/decisions/0021-power-port-power-on-state.md)):
  `powerOnState` = `leave` (default) / `restore` / `off`, applied by
  `apply_power_on_state()` from the router's start; `off` only on the first start of a
  boot. Every successful write saves through `util::PowerPortStateStore`. Lines a start
  drove stay claimed with `Connected` false; a driver's `get_connected()` is its own
  flag, never the wrapper's `is_open()`. A new GPIO power-port driver implements all of it.
- **Never power-cycle on disconnect**: `close()` releases the lines without driving a
  boolean line low; a PWM port first stops its worker and drives a defined steady
  level (duty > 0 ⇒ high); a port the worker never drove is left alone. Users who want a port off must set it off in the client
  before disconnecting. Do NOT add a drive-low-on-close path without making it opt-in
  config — it would silently flip the policy for everyone who upgrades. Documented
  user-facing in [`AlpacaCore/PowerPorts.md`](../../AlpacaCore/PowerPorts.md).
- **Userspace soft-PWM, per-port worker threads** (`sleep_until` bit-bang): hardware
  or DMA PWM is board-specific and unavailable/unreachable on every board we ship, so
  userspace toggling is the standard mechanism. Steady-state 0%/100% skips the
  per-period syscall. Every line write checks the return code — on failure, log at
  ERROR and set the per-port stop flag so the thread exits cleanly instead of looping
  while the ASCOM API reports success. Two-phase shutdown: signal stop under the
  mutex, join outside it, release/close back under it (the classic
  join-under-the-same-mutex deadlock otherwise).
  These per-port periodic workers are not operation-slot bodies; a cancellable one-shot
  or polling body added to a GPIO driver uses the slot (Threads & shutdown in [driver-lifecycle](driver-lifecycle.instructions.md)).
- **PWM frequency is the lever, not load type** — a flat panel's internal LED driver
  smooths a too-fast chop into plain on/off (a panel that "won't dim" at 1 kHz dims
  fine at 50 Hz); resistive dew heaters dim at any frequency; regulated gear
  (cameras/mounts) stays on/off regardless. The default is **per-driver**, verified
  against the stock firmware's actual value or on real hardware — never re-derived
  from bench psychoacoustics: ASIAIR Pro/CM4 1 kHz, ASIAIR Plus RK3568 50 Hz, iMate
  50 Hz, StellaVita 100 Hz.
- **`SetSwitchValue` checks the id, a finite value and the port's range before the
  connection** (issue #2264): Min/MaxSwitchValue are static on these boards, so a
  non-finite or out-of-range value answers `InvalidValue` while disconnected, and a valid
  one `NotConnected`. The order is id, finite value, range, connection, writability.
  Tests: the "value range is InvalidValue while disconnected" case of each board.
- **Read-only pass-through ports** (e.g. iMate DC3): a write with a valid value checks the
  connection first (a disconnected write answers `NotConnected`), then throws
  `NotImplemented`; the capability is unit-tested on a connected fake.

### Platform 7 InterfaceVersion + DeviceState

- Drivers advertise ASCOM Platform 7 interface versions: Camera 4 (ICameraV4),
  Telescope 4, Focuser 4, Rotator 4, FilterWheel 3, Switch 3, CoverCalibrator 2 (ICoverCalibratorV2),
  ObservingConditions 2.
  Keep `get_interface_version()` and its unit-test assertion in sync when adding a driver.
- **Do not** write a per-vendor `get_device_state()`. Each device base class
  (`CameraDriver`, `TelescopeDriver`, …) implements it once, inline, building the
  operational-property list by calling that device's own property getters inside a
  `try { … } catch (const std::exception&) {}` (a getter that throws — `AlpacaException`
  or any unwrapped vendor error — is omitted, never propagated) and
  appending a `TimeStamp` via the inline `device_state_timestamp()` helper only when at least
  one operational value is available. If every operational read fails, return the empty list
  with no `TimeStamp`, even while connected (the ASCOM read-all FAQ). A disconnected driver
  also returns the empty list (each base class checks `get_connected()` first). Using the
  same getters as the GET endpoints guarantees DeviceState ↔ GET consistency, which is
  what ConformU checks. A new vendor driver inherits the compliant DeviceState for free.
- DeviceState is **not an atomic snapshot**: each getter locks the driver mutex
  separately, so e.g. `RightAscension` and `Slewing` can straddle a state change, and a
  device dropping mid-call yields a partially populated response. The old per-vendor
  overrides read everything under one mutex. ASCOM doesn't require atomicity and
  ConformU only checks DeviceState ↔ GET consistency, so don't "fix" this by adding a
  snapshot lock — but don't build features that assume mutual consistency within one
  DeviceState response either.
- The base `get_device_state()` and `device_state_timestamp()` are **inline in the
  headers on purpose**: an out-of-line virtual would make the device class's vtable a
  "key function" emitted only in the core library, and the per-vendor static libraries
  (linked before it) would fail to resolve `vtable for XDriver`. Keep them inline.
- **Value types must match the ASCOM member type**: ConformU coerces each DeviceState
  value to the member's declared type and treats a failure as "property not included"
  (INFO, not an issue, so it hides in a passing log). `PercentCompleted` is a `short`; the
  camera base emitted it as a double (`0.0`) for a year and every camera log carried the
  INFO line. Emit integers for integer members (`CameraState`, `PercentCompleted`,
  `Position`, …) and doubles only for double members.
- ConformU is lenient about DeviceState contents (it does not require a fixed property
  set or even a TimeStamp — the iOptron switch passed at ISwitchV3 with neither), but it
  does flag values inconsistent with the individual GETs. The getter-based pattern above
  satisfies it. Still, bumping any InterfaceVersion **requires a fresh ConformU V4 run on
  real hardware** before release, since it switches ConformU to the stricter test suite.
