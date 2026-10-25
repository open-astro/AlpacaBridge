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
`parking_` flag, then run the slew + completion poll + tracking stop as a body in the driver's
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
not count as off: a motion state the getter cannot tell apart from Tracking off (iOptron `:GLS` reports a GOTO,
guiding or a meridian flip in place of tracking, so its guard refuses only a stopped mount), and a retarget of the
driver's own slew still in flight (ZWO `:GU`, Bisque `IsTracking`: nothing says what they report during a GOTO).
Syncs and Alt/Az slews are exempt. A driver test that slews turns Tracking on first; do not leave it off and assert
only that the slew threw.

### ASCOM exception vocabulary (pick the right one — ConformU checks it)

| Throw | When |
|---|---|
| `InvalidValue` | Bad argument / out-of-range id or index — **even while disconnected** (precedes the connection check). |
| `NotConnected` | Any operational property/method called while disconnected. |
| `PropertyNotImplemented` | A property the hardware genuinely lacks (e.g. `Offsets` list, `SubExposureDuration`). |
| `MethodNotImplemented` | A method the hardware lacks (e.g. `PulseGuide` when `CanPulseGuide` is false). |
| `ActionNotImplemented` | `Action()` called with a name the driver does not list in `SupportedActions` (0x40C, ASCOM's `ActionNotImplementedException`). An unsupported `CommandBlind`/`CommandBool`/`CommandString` is `MethodNotImplemented`, not this. |
| `NotImplemented` | A generic unsupported action (e.g. `set_temp_comp(true)` with no temp-comp support) — never `DriverException` for "not supported". |
| `InvalidOperation` | Valid call, wrong state (e.g. changing readout mode/geometry mid-exposure). |
| `DriverException` | A genuine internal/driver failure only — not a stand-in for any of the above. |

All map to HTTP 200 with a non-zero `ErrorNumber` — clients read the body, not the status.

A camera's "no image yet" answer (`ImageArray` before any exposure, or while one is still running) is an `InvalidOperation` the contract requires, not a fault: throw it with `AlpacaException::asked_too_early(message)`, so the router logs it at DEBUG instead of ERROR (issue #1167). Never use it for an `InvalidOperation` that reports a failed, stopped or racing exposure. The tier-2 contract sweep checks the first case on every camera fake.

### Telescope refraction, site elevation and DestinationSideOfPier (all telescope drivers)

- `DoesRefraction` throws `PropertyNotImplemented` on read and write in a driver that neither knows nor computes refraction (ITelescopeV4). Never store the flag and read it back: it would claim a correction the driver does not apply. OnStep reads it from the firmware.
- `SiteElevation` throws `InvalidOperation` until the device config or a client sets it, never 0 m ([decision 0019](../../docs/decisions/0019-connect-without-site.md)).
- `DestinationSideOfPier` throws `InvalidValue` for RA outside [0, 24) or Dec outside [-90, 90], before the connection check.
- RA, Dec and site range checks, local sidereal time, the hour and degree wraps, the shortest RA delta and the destination pier-side rule go through `util/pointing.h` (`validate_ra`, `validate_dec`, `validate_ra_dec`, `validate_latitude`, `validate_longitude`, `local_sidereal_hours`, `wrap_hour_angle`, `pier_side_for_hour_angle`); a telescope driver never defines its own validator, sidereal-time, hour-angle-wrap or pier-side function, so the `InvalidValue` text is the same on every mount. Inline RA wraps and hand-written shortest-RA-delta code inside larger expressions (Celestron NexStar, iOptron, SynScan, ZWO) are not converted yet. Exceptions: CGX `cgx_local_sidereal_time_hours` builds on `gmst_degrees` and adds its own equation of equinoxes; CGX derives its pier side as the `CgxPointingSide` enum from the hour angle, not as an int; Bisque keeps a `detail::validate_ra_dec` forwarder to `util::validate_ra_dec` until its test call is removed.
- A `UTCDate` read the mount clock cannot answer throws `InvalidOperation`; the host clock does not stand in for the mount's (ITelescopeV4 `UTCDate`, the ASCOM time FAQ). Celestron and OnStep follow this; the other adopters are not converted yet.

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

### Action names are case-insensitive (all drivers with actions)

ASCOM treats `SelectWheel`, `selectwheel` and `SELECTWHEEL` as the same action: the
`ActionName` parameter of `Action()` is case-insensitive, and the names in
`SupportedActions` may use any casing. Match the requested name against the supported
list without regard to case, in `action()` and in `can_action()` alike, and throw
`ActionNotImplemented` only when no supported name matches. The two drivers with
actions already do this: the Player One camera (`to_lower_copy`), the Celestron
focuser (`same_action`) and the iOptron and Celestron mounts' `gps_status`
(`util::is_gps_status_action`). A new driver with actions does the same.

### Camera ROI alignment (all camera vendors)

Every camera SDK constrains ROI geometry, and the pattern is the same everywhere:
**keep the client-requested values for the Alpaca interface, align down for the SDK
call, and pad outputs if needed** — ConformU's read-back checks must see the requested
geometry. Per-SDK constraints (the only vendor-specific part): ZWO width%8 / height%2
after binning; SVBONY width%8 / height%2; Player One width%4 / height%2; ToupTek even
sensor-resolution width/height/offset AND an even binned output size (see the ToupTek
odd-bin-factor note for the 3×3 subtlety and the #977 odd-NumX rule).

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
- **DeviceState includes operational fields only** (e.g. `Position`); omit `Connected`
  for ConformU compatibility.

### GPIO power-switch / soft-PWM drivers (general rules)

All GPIO 12V power-port Switch drivers (ZWO ASIAIR Pro / Plus CM4 / Plus RK3568,
ToupTek StellaVita, iOptron iMate, and any future board) share these rules; the vendor
notes carry only the pin map and per-board deltas.

- **libgpiod v2 only** (`libgpiod-dev (>= 2.0)`, `libgpiod3` runtime): one
  `gpiod_line_request*` owns all lines together; values go through
  `gpiod_line_request_set_value`. Never port back to the v1 per-line API. The daemon
  user needs `gpio`-group access to the chip/char device via a udev rule.
- **Boot-high preserve**: these boards drive the DC ports HIGH at boot
  (`gpio=...=op,dh[,pu]`), so attached gear is powered before userspace runs. The
  wrapper requests lines with an initial value of high and defaults its cached state
  to "on" — connecting the driver must not glitch power.
- **Never power-cycle on disconnect**: `close()` releases the lines without driving a
  boolean line low; a PWM port first stops its worker and drives a defined steady
  level (duty > 0 ⇒ high). Users who want a port off must set it off in the client
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
- **Read-only pass-through ports** (e.g. iMate DC3): writes throw `NotImplemented`
  *before* the connection check, so the static capability holds while disconnected
  and is unit-testable without hardware.

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
