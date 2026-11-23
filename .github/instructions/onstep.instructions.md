---
applyTo: "AlpacaCore/src/vendors/onstep/**,AlpacaCore/include/alpacacore/vendor/onstep/**,AlpacaCore/tests/*onstep*,AlpacaCore/conformu/OnStep/**"
---

### OnStep

Devices: Telescope, Focuser (OnStepX focusers 1-6, #2223), Rotator (OnStepX, #2221), Switch (OnStepX auxiliary
features, #2222). Every device on one controller shares one link (see "Focuser and the shared link", "Rotator" and
"Switch" below).

No vendor SDK — OnStep is open-source LX200-protocol firmware (Arduino Mega/Due, Teensy,
ESP32, STM32) for DIY/retrofit mounts. The protocol reference is the firmware's own OnStepX
command reference, kept unmodified in [`AlpacaCore/external/OnStep/`](../../AlpacaCore/external/OnStep/README.md)
(GPL-3.0, with its license; upstream
[at the same commit](https://github.com/hjd1964/OnStepX/blob/66a4b3220371661d40d3df9f6e5e1203068842e4/docs/COMMAND_REFERENCE.md),
the one read for the table below), then the firmware source itself
([OnStepX](https://github.com/hjd1964/OnStepX), and [OnStep](https://github.com/hjd1964/OnStep)
`Command.ino` for the older firmware). Check a reply format there before a driver change or a
test fake relies on it. The driver was first written from INDI's `lx200_OnStep` driver
(`indilib/indi`, `drivers/telescope/lx200_OnStep.cpp` + the shared `lx200telescope.cpp`/`lx200driver.cpp`
base it inherits from); several of INDI's readings turned out wrong (see the `:GU#` polarity and
the tracking commands below), so INDI is history, not the reference.

#### Commands this driver sends

Replies are from the command reference unless noted. "Bare digit" means one character and no
`#` (the driver reads it with `require_hash_terminator=false` and `kSetAckTimeoutMs`); "blind"
means the driver sends it and drains whatever arrives within 50 ms without reading it.

| Command | Used for | Reply | How the driver reads it |
| --- | --- | --- | --- |
| `:GVP#` / `:GVN#` | identity probe, firmware version | `name#` / `M.mm...#` | `#`-terminated; product must be `OnStep` or `On-Step` |
| `:GVC#` | the device name (OnStepX product description, decision 0024) | `text#`; no reply at all from a firmware without it | `#`-terminated; sent by firmware version, below |
| `:GXEE#` | coordinate mode (`DoesRefraction`, #1216) | `0#`, `1#`, ... | read without waiting for `#` |
| `:GU#` | status (tracking, slewing, park, home, guide, mount type, pier side) | packed status `#` | `#`-terminated; see the status table below |
| `:GX96#` | preferred pier side (OnStepX, #2179) | `E`/`W`/`B`/`A` `#` (`A` on main only; `E` with the flips off) | `#`-terminated; one of the four letters, else unknown |
| `:GX94#` | meridian flips switch (OnStepX, #2179) | `<n>#` flips on, `<n> N#` off (n 0 none, 1 east, 2 west) | `#`-terminated; strict, else unknown |
| `:GXE9#` / `:GXEA#` | meridian limit east / west (OnStepX, #2179) | whole minutes past the meridian `#`, may be 0 or negative | `#`-terminated; whole number, else unknown |
| `:GXEe#` / `:GXEw#` | RA-axis limit min / max (OnStepX, #2179) | whole degrees `#` | `#`-terminated; whole number, else unknown |
| `:GXEF#` / `:GXEG#` | tangent arm / sector gear (FindHome, #2296; 4.x answers `:GXEF#` only, and is not asked `:GXEG#` once its version reads) | `1`/`0` bare digit | read at the first FindHome of a session that is not a no-op, also when the home is then refused; an unknown command answers `0`; a reply that is neither digit is read again at the next |
| `:GX93#` / `:GXE4#` | base slew period in us/step / axis 1 steps per degree (park and home bound, #2296) | `n.nnn#` / `n#` | `#`-terminated, at connect; a reply that does not parse, or `0`, keeps 20 minutes |
| `:MNe#` / `:MNw#` | GOTO the current RA/Dec on the east / west side of the pier (the flip, #2179; OnStepX) | one digit, codes as `:MS#` | bare digit, no `#` (suppressFrame, like `:MS#`); `0` accepted; with the flips off the firmware answers the unknown-command `0`, so only a `CanSetPierSide` mount sends it |
| `:GRa#` / `:GDe#` | RA / Dec | `HH:MM:SS.SSSS#` / `sDD*MM:SS.SSS#` | `#`-terminated. Not in the reference: aliases of `:GRH#`/`:GDH#` (OnStepX `Mount.command.cpp`, the `'a'`/`'e'` parameter) |
| `:GA#` / `:GZ#` | altitude / azimuth | `sDD*MM#` or `sDD*MM'SS#` / `DDD*MM#` or `DDD*MM'SS#` | `#`-terminated |
| `:Gh#` / `:Go#` | horizon / overhead limit (MoveAxis gate, #1378) | `sDD*#` / `DD*#` | `#`-terminated |
| `:GS#` | sidereal time | `HH:MM:SS#` | `#`-terminated |
| `:Gt#` / `:Gg#` | latitude / longitude (west-positive on the wire) | `sDD*MM#` / `sDDD*MM#` | `#`-terminated |
| `:GL#` / `:GC#` / `:GG#` | local time / date / UTC offset (`UTCDate` read) | `HH:MM:SS#` / `MM/DD/YY#` / `sHH:MM#` | `#`-terminated |
| `:GE#` | last command error (after a refused `:hP#`/`:hC#`, #1430, or `:ST`/`:hQ#`/`:hR#`, #1379, or a refused focuser `:F<n>s`/`:F<n>c0`/`:F<n>c1`, #2223) | 2-digit code `#` | `#`-terminated; codes below; read inside the link's sequence lock, right after its command (#2223) |
| `:SrHH:MM:SS#` / `:SdsDD*MM:SS#` | GOTO target | `0`/`1` | bare digit, `1` accepted |
| `:StsDD*MM#` / `:SgsDDD*MM#` | site latitude / longitude | `0`/`1` | bare digit |
| `:SLHH:MM:SS#` / `:SCMM/DD/YY#` / `:SGsHH[:MM]#` | time sync (UTC as local, offset 0) | `0`/`1` | bare digit |
| `:MS#` | GOTO | one digit `0`..`9` | bare digit; `0` accepted, the rest below |
| `:CM#` | sync | `N/A#`, or `E1#`..`E9#` on failure (the `:MS#` digits) | `#`-terminated; `E<n>` throws with the digit's reason (#1379) |
| `:Q#` | stop all slews, abort GOTO | none | blind |
| `:Qn#` / `:Qs#` / `:Qe#` / `:Qw#` | stop one axis direction | none | blind |
| `:Mn#` / `:Ms#` / `:Me#` / `:Mw#` | MoveAxis at the custom guide rate | none | blind; re-sent every 7 s (#1378) |
| `:RAn.n#` / `:REn.n#` | MoveAxis rate, deg/s per axis | none | blind |
| `:RS#` | slew-rate preset before a move | none | blind |
| `:Mgdnnnn#` | pulse guide, `d` = `n`/`s`/`e`/`w`, 4-digit ms | none | blind |
| `:ST60.164275#` / `:ST0#` | tracking on (OnStep 4.x) / off (both) | `0`/`1` | bare digit for the client's `Tracking` write (`0` throws with the `:GE#` reason, #1379); blind for the driver's own tracking restores |
| `:Te#` | tracking on (OnStepX; `:ST` sets the rate back to sidereal there) | `0`/`1` | as `:ST` above; `0` is `CE_PARKED` |
| `:TQ#` / `:TL#` / `:TS#` / `:TK#` | tracking rate sidereal / lunar / solar / King (`TrackingRate` write, OnStepX, #2214) | none | blind; changes neither tracking on/off nor the rate offsets |
| `:GT#` | tracking rate in Hz (`60.16427` sidereal, `57.90000` lunar, `60.00000` solar, `60.13600` King), `0` when not tracking (OnStepX, #2214) | `n.nnnnn#` | `#`-terminated; read at connect and on the slow tick while compensation hides the `:GU#` letter and the mount tracks; matched within 0.01 Hz, else the last rate stays |
| `:SXTR,n.n#` / `:SXTD,n.n#` | RA / Dec rate offset, arcseconds per sidereal second (`RightAscensionRate` x 15, `DeclinationRate` x 0.9972695663; OnStepX, #2214) | `0`/`1` | bare digit; `0` throws with the `:GE#` reason; the firmware clamps silently at +/-1800, so the driver refuses a larger request first |
| `:GXTR#` / `:GXTD#` | RA / Dec rate offset read back (OnStepX, #2214) | `n.nnnnnnnn#` | `#`-terminated; needs a decimal point (an unknown command answers `0`), else the previous value stays |
| `:GX90#` | pulse-guide rate as a multiple of sidereal (`GuideRate`, both firmwares, #2214) | `n.nn#` | `#`-terminated; a positive decimal, else `GuideRate` keeps its informational value |
| `:hP#` | park | `0`/`1` | bare digit; then `:GE#` for the reason |
| `:hC#` | find home | none | blind; then `:GE#` for the reason |
| `:hR#` | unpark | `0`/`1` | bare digit; `0` throws with the `:GE#` reason, except `11` (not parked), ASCOM's no-op |
| `:hQ#` | set park position | `0`/`1` | bare digit; `0` throws with the `:GE#` reason |
| `:F<n>T#` | focuser n (1-6) status: the connect's check, and every focuser poll (#2223) | `M` moving or `S` stopped, then the goto-rate digit, `#` | read without waiting for the `#`: a controller without focuser n answers the unknown-command `0`, a bare digit; anything but `M`/`S` and a digit fails Connect with "OnStepX reports no focuser n" |
| `:F<n>g#` | focuser position in raw steps (poll) | `n#` (`%ld`, less the TCF offset) | `#`-terminated; `Position` is it less `:F<n>i#` |
| `:F<n>i#` / `:F<n>m#` | focuser minimum / maximum position in raw steps (connect) | `n#` | `#`-terminated; `MaxStep` and `MaxIncrement` are maximum less minimum; either one that does not parse fails Connect |
| `:F<n>u#` | focuser microns per step (`StepSize`, connect) | `n.nnnnn#` | `#`-terminated; a reply that is not a number above 0 makes `StepSize` `PropertyNotImplemented` |
| `:F<n>t#` | focuser temperature in degrees C (connect, every fourth poll) | `n.n#`; `nan#` with no probe (the sensor reads NAN) | `#`-terminated; no number at connect makes `Temperature` `PropertyNotImplemented` and `TempCompAvailable` false |
| `:F<n>c#` | focuser temperature compensation on/off (connect, every fourth poll) | `1`/`0` bare digit | `TempCompAvailable` needs it to answer and a temperature at connect |
| `:F<n>c0#` / `:F<n>c1#` | `TempComp` write | `0`/`1` bare digit | `0` throws `InvalidOperation` with the `:GE#` reason |
| `:F<n>s<steps>#` | focuser `Move`, absolute, raw steps (the client's position plus the minimum) | `0`/`1` bare digit | `0` throws `InvalidOperation` with the `:GE#` reason (`09`: the focuser is parked) |
| `:F<n>Q#` | focuser `Halt` | none | blind |
| `:GX98#` | rotator capability (the rotator's connect check, #2221) | `R#` rotate only, `D#` rotate and derotate, `N#` none; a firmware without it answers the unknown-command `0` | read without waiting for the `#`; anything but `R`/`D` fails Connect with "OnStepX reports no rotator" |
| `:rT#` | rotator status (every rotator poll, and right before each move, #2221) | `M` moving, or `S` stopped then `D` (derotation on) and `R` (derotator reversed), then the goto-rate digit, `#` | read without waiting for the `#`; `D` refuses a move with `InvalidOperation` |
| `:rG#` | rotator angle (every poll; Sync) | `sDDD*MM#`, signed, inside the travel | `#`-terminated; the sign and `*` are required; `MechanicalPosition` is it wrapped into 0-360 |
| `:rI#` / `:rM#` | rotator travel minimum / maximum, whole degrees (connect) | `n#` | `#`-terminated; either one that does not parse, or a maximum not above the minimum, fails Connect |
| `:rD#` | rotator degrees per step (`StepSize`, connect) | `n.nnnnn#` | `#`-terminated; a reply that is not a number above 0 makes `StepSize` `PropertyNotImplemented` |
| `:rSsDDD*MM:SS#` | rotator move to an absolute angle inside the travel | `0`/`1` bare digit | `0` throws `InvalidOperation` with the `:GE#` reason (`09`: the rotator is parked) |
| `:rQ#` | rotator `Halt` | none | blind |
| `:GXY0#` | auxiliary feature slots (the switch's connect, #2222) | eight `0`/`1`, slot 1 first, `#`; a firmware without the features answers the unknown-command `0` | read without waiting for the `#`; a reply that is not `0`/`1` fails Connect |
| `:GXY<n>#` | slot n's name and purpose (connect, for each slot the bitmap marks) | `name,purpose#` (name cut to 10 characters; a momentary or cover switch reports purpose 1); `0` for a slot that is off | read without waiting for the `#`; the purpose follows the last `,` |
| `:GXX<n>#` | slot n's value (every switch poll) | `value#` for a switch or PWM output, `enabled,zero,span,deltaT#` for a dew heater, each with `;volts,amps,flags` appended in a power-monitor build | `#`-terminated; the first field before `,` or `;` is the value |
| `:SXX<n>,V<v>#` | switch write | `0`/`1` bare digit | `0` throws `InvalidOperation` with the `:GE#` reason |

`:MS#` refusal digits (OnStepX `Goto.command.cpp`, OnStep `Command.ino`; the same digits follow
`E` in a `:CM#` failure): `1` below the horizon limit, `2` above the overhead limit, `3` controller
in standby, `4` parked, `5` GOTO in progress (OnStepX also stops the running GOTO), `6` outside
limits, `7` hardware fault, `8` already in motion, `9` unspecified.

`:GE#` codes the driver acts on (the full list is in the command reference): `00` none and `25`
true are success; `22` in motion makes Park/FindHome wait for the mount to be still and retry for
3 s; any other code is a refusal the driver keeps and reports with its number (#1430). The `:MS#`
digits are these codes 15 to 23 less 14.

#### Focuser and the shared link (#2223)

The telescope and every OnStep focuser on one controller share ONE link, the iOptron iFR precedent
([decision 0017](../../docs/decisions/0017-serial-port-ownership.md) adoption row).
`OnStepProtocolWrapper::acquire(info)` returns the live link to the same controller, keyed by the serial
port's canonical path or by `host:port`, or opens one (`util::SerialLink` under the owner name `OnStep`, or the
TCP channel); the registry lock is held across the open, so two connects at once open it once. A joiner takes the
link as the first device opened it (line settings and response timeout; a different baud is a WARN). Each driver
holds a `shared_ptr` in an `std::atomic<std::shared_ptr>` (so `Connected` stays lock-free), and the link closes when
the last holder lets go: a device's Disconnect only lets go, and the other device keeps the link and its poll.
There is no process-wide wrapper any more (`instance()` is gone); a test that drives the wire directly builds a
standalone `OnStepProtocolWrapper` and calls `connect()`/`disconnect()` on it, and
`detail::registered_link_for_testing(info)` returns the registered link, if any.

- **Wire turns**: every exchange holds the link's wire lock for its command and reply, as before. A command whose
  error code is read next with `:GE#` (the telescope's `:hP#`/`:hC#`, `:ST`/`:Te#`, `:hQ#`, `:hR#`, `:SXTR`/`:SXTD`;
  the focuser's `:F<n>s` and `:F<n>c0`/`c1`; the rotator's `:rT#` check and `:rS`, #2221; the switch's `:SXX`, #2222) holds the link's sequence lock (`hold_sequence()`, a recursive mutex
  above the wire lock) across both, because OnStep keeps one code per channel and a sibling's command in between
  would replace it. The telescope takes it through `Wire::hold_sequence()`, with `mutex_` released. Nothing else
  holds it, so the telescope's poll, MoveAxis re-send and pulse-guide timing keep their cadence; a focuser exchange
  delays a telescope exchange by one reply at a time (`test_onstep_focuser.cpp`, the pulse-guide `[shared-link]` case).
- **Link loss** (decision 0009): a loss found by any holder is the link's, so every device on it reads `Connected`
  false at once and each relinks on its own Connect; `acquire()` replaces a lost link on the same key with a fresh
  one (closing the lost one's port, keeping its loss reason for the holders that have not let go, rule 5). A
  device's stale fault declares the link lost for all of them (`declare_link_lost()`), as the iFR does: one
  controller, one link. The telescope's #521 stamps: the telescope sets the link's clock when it takes it and, when
  it lets go (a Disconnect, a relink or a failed connect), moves the link's loss and last-alive stamps into its own
  state before it releases the clock, so its next connect times the outage whichever link it then joins, as the
  process-wide stamps did. A driver object rebuilt during an outage (an edited device) starts with no stamps.
- **Focuser** (`onstep_focuser_driver.cpp`, OnStepX only): the config takes a focuser number 1-6 (default 1); the
  API field's 1-6 range refuses any other (the catalog clamps a saved value into it). Connect reads `:GVP#` (an OnStep) and `:GVN#` (refused below
  version 10: "The OnStep Focuser requires OnStepX firmware"), then `:F<n>T#` (else "OnStepX reports no focuser
  n"), the travel, the step size, the temperature and the compensation flag. Only the addressed `:F<n>...#` forms
  are sent: never `:FA<n>#` (the handset and the OnStepX web page use the active focuser) and never the bare rate
  preset `:F<d>#`. Getters serve a `util::PolledState` frame (500 ms, three failed polls latch a fault, a fault or
  frame standing past `util::kLinkStalenessBound` loses the link for every device; decisions 0009, 0010, 0016):
  `:F<n>T#` and `:F<n>g#` every poll, `:F<n>t#` and `:F<n>c#` every fourth. A stale frame is served as it is
  (decision 0010 rule 6): Position and IsMoving keep the last poll's value, TempComp the last `:F<n>c#` answer
  and Temperature the last `:F<n>t#` reading; only a fault (DriverException) or a lost link (NotConnected)
  refuses them. `Move` clamps to `0..MaxStep`
  (the shared #2028 rule, not the `InvalidValue` #2214 first wrote) and sends `:F<n>s<p + min>#` on the caller's
  thread: one exchange answered at once, so a refusal throws from `Move` itself, and the motion is followed by the
  poll (whose slot is the `AsyncOperation`); IsMoving is written through. `Halt` sends `:F<n>Q#` and writes
  not-moving through; it is not refused for a fault. `TempCompAvailable` needs a temperature at connect as well as
  a `:F<n>c#` answer: OnStepX turns a compensation request into off while the sensor reads NAN
  (`Focuser.cpp` `setTcfEnable()`, af1d26bf), so a probe-less focuser would accept `TempComp=true` and read false.
  The name is `OnStep Focuser` (decision 0024: no model string), the description names the focuser number.
  Auto-detect is the telescope's scan with the same `mountIndex`, at connect; a port a connected OnStep device
  holds keeps its place without a probe, so the focuser lands on the telescope's link. Fake evidence only: no
  OnStepX focuser on a rig; the ConformU Focuser check is tracked in the OnStep test plan, #2126. Tests:
  `test_onstep_focuser.cpp` (`[shared-link]`), the `[onstep][focuser][stress]` cases in
  `test_onstep_concurrency_stress.cpp` (over `onstep_controller.h`, whose controller model is `onstep_controller_model.h`, also the fake ConformU host of the focuser, #2078), the contract sweep entry `onstep_focuser`.

#### Rotator (#2221)

The OnStepX rotator joins the controller's link the way a focuser does (`acquire()`, the same connect, refusals,
poll and loss rules, `onstep_rotator_driver.cpp`), and is OnStepX only. Commands are from OnStepX
`Rotator.command.cpp` at `af1d26bf`.

- **Connect** reads `:GVP#`, `:GVN#` (below version 10: "The OnStep Rotator requires OnStepX firmware"), `:GX98#`
  (`R` or `D`, else "OnStepX reports no rotator"; `D` is a derotator build, which OnStepX reports on an alt-az mount),
  the travel `:rI#`/`:rM#` and the step size `:rD#`, then loads the Sync offset ([decision 0008](../../docs/decisions/0008-rotator-sync-offset-persistence.md)).
- **State poll** (`util::PolledState`, 500 ms): `:rT#` then `:rG#`, so an angle read after a stopped status is the end
  of the move. `IsMoving`, `MechanicalPosition` and `Position` serve the frame, stale or not; a fault throws
  `DriverException`, a lost link `NotConnected`, and a poll unanswered for `util::kLinkStalenessBound` declares the link
  lost for every device on it.
- **Angles**: `:rG#` is the firmware's coordinate, signed and inside the travel (whole arcminutes: `Convert::doubleToDms`
  at `PM_LOW`). `MechanicalPosition` is it wrapped into 0-360, and `Position` is `MechanicalPosition` plus the Sync
  offset. A move (`Move`, `MoveAbsolute`, `MoveMechanical`) turns its target into a mechanical angle, then into the
  firmware coordinate inside `:rI#`..`:rM#` that reaches it nearest the current one
  (`detail::rotator_angle_within_travel()`); none inside is `InvalidValue` with nothing sent, so a move never asks the
  firmware to leave its travel or wrap a cable. Then, holding the link's sequence lock, it reads `:rT#`: `D`
  (derotation on) is `InvalidOperation` ("turn derotation off on the controller"), since a goto there sets the axis
  rate the derotator then fights; otherwise `:rS<sDDD*MM:SS>#` (an optional sign, then `Convert::dmsToDouble`'s
  `DDD*MM:SS`), whose `0` reads `:GE#` in the same sequence. `IsMoving` is written through.
- **Never sent**: `:r+#`, `:r-#` (derotation on and off), `:rP#` (to the parallactic angle), `:rR#` (derotator
  reverse), the rate presets, the continuous moves and the zero or home commands. The derotator is the firmware's.
- **`Reverse`** is the driver's own: IRotatorV4 says `CanReverse` "must always return True" and `Reverse` must be
  implemented, and OnStepX has no reverse for a goto (`:rR#` reverses only the derotator). While it is set the driver
  mirrors the firmware angle (`MechanicalPosition` is the negated angle wrapped into 0-360) and each move's target, so
  `Position` still equals `MechanicalPosition` without a Sync. It changes nothing on the wire and lasts for the driver
  object (a reconnect keeps it, a service restart or a device edit clears it). The issue asked for `CanReverse` false;
  the interface does not allow it.
- **`Sync`** reads `:rG#` live, sets the offset (Position - MechanicalPosition) and saves it through
  `util::RotatorSyncOffsetStore` under the stored UniqueID; a save that fails throws `DriverException` and keeps the old
  offset. `Halt` sends `:rQ#` and writes not-moving through, a lost link refusing it only.
- Fake evidence only: no OnStepX rotator on a rig, and `:rG#` reads whole arcminutes, so ConformU's position checks,
  which use `StepSize` as their tolerance, may need a rotator whose step is at least one arcminute. The ConformU Rotator
  check is tracked in the OnStep test plan, #2126. Tests: `test_onstep_rotator.cpp`, the `[onstep][rotator][stress]`
  cases in `test_onstep_concurrency_stress.cpp`, the contract sweep entry `onstep_rotator`.

#### Switch (#2222)

The OnStepX auxiliary features (up to eight slots: power switches, PWM outputs, dew heaters, intervalometers) are one
Switch device on the controller's link, joined the way the focuser and the rotator join it
(`onstep_switch_driver.cpp`), OnStepX only. Commands are from OnStepX `Features.command.cpp` and the purpose codes
from `Constants.h`, at `af1d26bf`.

- **Connect** reads `:GVP#`, `:GVN#` (below version 10: "The OnStep Switch requires OnStepX firmware"), then the slot
  bitmap `:GXY0#` and `:GXY<n>#` for each slot it marks. The slot set is fixed until the next connect: switch ids
  0..N-1 are the included slots in slot order, so a feature reconfigured in the firmware shows at the next Connect.
- **Purposes** (`detail::aux_switch_kind()`): 1 (switch; also the momentary and cover switches, which `:GXY` reports as
  1) a 0/1 switch, 2 (analog output) 0-255 (`:SXX<n>,V` takes 0-255 for 0-100 % power), 3 (dew heater) a 0/1 enable of
  its regulation; all step 1. 4 (intervalometer) and 6 (hidden switch) are left out, and any other code is left out
  with an INFO line naming the slot. A controller with none of the three is refused ("OnStepX reports no auxiliary
  feature a Switch can drive").
- **Values** come from a `util::PolledState` poll (1 s: one `:GXX<n>#` per included slot, the first field, so a dew
  heater's zero, span and delta-T and any power telemetry are not read), served stale or not, so a change made in the
  OnStepX web page, or a momentary switch turning itself off, shows at the next poll. `GetSwitch` is the value above 0.
- **Writes**: an id outside 0-7 (OnStepX has eight slots) or a value that is not a finite number is `InvalidValue`
  before the connection check; once connected, an id past `MaxSwitch` or a value outside the slot's range is
  `InvalidValue` with nothing sent. The value is rounded to the step of 1, as the firmware's `lroundf` would, and
  sent as `:SXX<n>,V<v>#` holding the link's sequence lock, with `:GE#` after a `0` (`InvalidOperation` with the
  reason); the value is written through. `CanWrite` is true for every switch; `CanAsync` is false (the firmware applies
  a write at once) and the async members throw `MethodNotImplemented`, as does `SetSwitchName` (the names are the
  firmware's configuration). `MaxSwitch` and every per-id member answer `NotConnected` while disconnected, after the
  0-7 id check, since the slots are read at connect.
- **Descriptions** carry only what the connect read: "OnStepX auxiliary feature <n>: <purpose>". Power telemetry
  (volts, amps, fault flags), dew heater zero and span and the intervalometer settings are out of scope (#2214).
- Fake evidence only: no OnStepX with auxiliary features on a rig; the ConformU Switch check is tracked in the OnStep
  test plan, #2126. Tests: `test_onstep_switch.cpp`, the `[onstep][switch][stress]` cases in
  `test_onstep_concurrency_stress.cpp`, the contract sweep entry `onstep_switch`.

#### OnStep 4.x and OnStepX

OnStep (the older firmware, 4.x, last release branch `release-4.24`) and OnStepX (10.x, a
rewrite) speak the same LX200 dialect for every command in the table, with the differences below
(read from OnStep `Command.ino`/`Goto.ino` at `3b73bffcfb` and the OnStepX sources). Only OnStepX
is tested: the TeSeek HM-17 runs OnStepX 10.23a, and no OnStep 4.x mount has been run, so the 4.x
column is from the source, not from hardware.

- **Telling them apart**: OnStepX numbers its releases from 10 (`:GVN#` `10.23a`), OnStep 4.x and
  older below that (`is_onstepx_version()`, version 10 or later). The driver uses this only to
  decide whether to ask `:GXEE#`.
- **`:GVC#`** (the product description, the device name, decision 0024): OnStepX only, and an unknown `:GV`
  command gets no reply at all (`CE_CMD_UNKNOWN`, `numericReply=false`), not the `0` of other unknown
  commands, so a mount without it makes the read wait out the response timeout (6.0 s against about 0.9 s for a
  connect to the HM-17, #2232). OnStepX history: `1b6a020f` (2024-05-22) is 10.23a with no `'C'` case in the `:GV`
  branch; `d9de0b8d` (2024-06-10 06:41:53) adds `:GVC#` and is still 10.23a, as is `675cd048` (06:44:33); `6cf2e575`
  (06:44:38, "Upped patch level") makes it 10.23b, and every later build answers; 10.24a starts at `d09b11af`
  (2024-09-18). So only two unreleased 10.23a commits from a three-minute window answer. The connect therefore
  reads `:GVN#` (`firmware_answers_product_description()`) and asks, with the normal response timeout, for a major
  above 10, for 10.24 and later, and for 10.23 with a patch letter after `a` (`b` to `z`); it does not ask for
  10.23a, 10.23 with no letter, anything older, or a version that does not parse. The name stays
  `OnStep Telescope` when it is not asked or there is no answer; a test fake that stands for a firmware without
  the command must stay silent, not answer `0`.
- **`:GXEE#`** (coordinate mode, `DoesRefraction`, #1216): OnStepX only. OnStep 4.x answers any
  unknown command with `0`, so the driver does not ask it there and keeps the settable local flag.
- **`:GE#`**: both, with the same codes up to 23; OnStep 4.x has no `25` (`CE_1`) and keeps the
  code per serial channel (see the Park and FindHome bullet).
- **`:GU#`**: the letters the driver reads are the same, except that OnStep 4.x has no `h`
  (homing), so a `FindHome` there is never taken as one already running. OnStep 4.x adds letters
  the driver does not read (`R`, `S`, `r`, `s`, `t`, `u`, `z`, `a` and a PEC character). Both send `w` while a GOTO
  waits at home for the user (the meridian-flip pause-at-home setting); the driver reads it for the park and home
  bound (#2296).
  Both send `g` while a `:Mn#`-style move runs and `G` while a pulse guide runs. Both end it with the general
  error code and write `F` for a failed park, but OnStepX computes the code from the limits as they stand and OnStep
  4.x latches it until `:Te#`, an unpark, a home reset or a reboot (`initGeneralError()`), so a stop code at the end
  of a slew is judged by where the mount is on both (#2296, the stop bullet under the slews). `:Te#` would clear the
  latch on a tracking 4.x mount with no other effect (`Command.ino`), but the driver does not send it: the end position
  already decides, and an Alt/Az slew or a park starts with tracking off, where `:Te#` would start it. OnStep 4.x has
  no `h`; its home with switches shows `g`.
- **`:GG#`** (UTC offset): OnStep 4.x answers whole hours (`sHH#`), OnStepX `sHH:MM#`; the
  sexagesimal parser reads both.
- **`:GRa#` / `:GDe#`**: both take `a`/`e` as the high-precision form, like `H`.
- **`:ST`** (tracking rate): both reply `0`/`1`; OnStep 4.x refuses it while slewing
  (`CE_MOUNT_IN_MOTION`); OnStepX does not start tracking while parked (no error, `Mount.command.cpp`).
  The client's `Tracking` write reads the reply, so a 4.x refusal during a GOTO throws (#1379).
- **Tracking rates, rate offsets and the guide rate** (#2214): OnStepX only for `:TQ#`/`:TL#`/`:TS#`/`:TK#`, `:Te#`, `:GT#`,
  `:SXTR`/`:SXTD` and `:GXTR#`/`:GXTD#` (OnStepX `Mount.command.cpp` at `af1d26bf`; OnStep 4.x `Command.ino` not read for
  them, so it keeps `TrackingRates` sidereal, `CanSet*Rate` false and `:ST60.164275#`). `:GX90#` is on both
  (OnStepX `Guide.command.cpp`, OnStep 4.x `Command.ino:922`), so the guide rate is not gated on the firmware.
- **A GOTO during a GOTO** (#1379): both stop the running one and refuse the new one (OnStepX
  `Goto.cpp` `CE_SLEW_IN_SLEW`; OnStep 4.x `Goto.ino` `CE_GOTO_ERR_GOTO`, unless built with
  `CE_GOTO_ERR_GOTO_OFF`), with `:MS#` reply `5`.
- **The guide time limit that ends a `MoveAxis`** (`GUIDE_TIME_LIMIT`, #1378) is a build setting
  in both: 1-120 s or `0` for none (OnStep 4.x `Config.h` ships `0`; OnStepX defaults to 10 s, which
  the HM-17 runs). The re-send keeps a move going under a limit and changes nothing without one.

Connection types: **Auto-Detect** (the USB serial ports only), **Serial** (a named port, default
9600 baud, 8N1) and **Network** (#2220): a Wi-Fi or Ethernet OnStepX (ESP32, or the Smart Web Server
add-on) by `host` and `tcpPort`, the same standard LX200 `#`-terminated ASCII commands on a TCP
command channel. Nothing scans the network; the host is typed in.

- **The default port is 9998**, a persistent channel. In OnStepX the standard channel, 9999, closes a
  client a fixed 2 s after it is accepted, while the persistent channels, 9996-9998, extend that timeout
  on every byte read (`Serial_IP_Wifi.cpp`, `IPSerial::available()` and `read()`, at `af1d26bf`); the
  1 s status poll keeps the link open. A user who picks 9999 gets a link that drops every 2 s, which the
  driver then sees as a hangup.
- **Config** (`onstep_fields.h`, `onstep_schema.cpp`): `connectionType` `network` with `host`, `tcpPort`
  (default 9998) and the shared `responseTimeoutMs`. From the API an empty host is refused with "Host IP
  address is required" and a port outside 1-65535 with "tcpPort is out of range (min 1) (max 65535)". A
  saved entry with the same faults is normalized with that warning, stays registered and editable, and
  keeps its value: `tcpPort` has no per-field min/max, which would drop a saved port to the default and
  connect there. The factory passes a saved port outside 1-65535 as 0 (range-checked before the cast,
  decision 0007 rule 4), and the wrapper's connect refuses an empty host or a port outside 1-65535 with
  the field in the message ("... (host)", "... (tcpPort)"), opening nothing.
- **Connect** (`connect_network()`): a numeric IPv4 address or a host name (`getaddrinfo`, IPv4; the
  lookup is not bounded by the timeout), a non-blocking connect that waits at most the response timeout,
  then `TCP_NODELAY` (one small write per command). An unreachable controller fails Connect within the
  response timeout ("could not connect to <host>:<port>: no answer within <n> ms"), a refusal at once
  ("... Connection refused"); both are the connect error the client and `LastConnectError` see.
- **Link loss** follows decision 0009 as the serial link does: a TCP hangup (the controller closed or
  reset the connection) is a lost link at once, the way a removed device node is for serial (rule 2a).
  `link_alive()` polls the socket with no wait (`POLLRDHUP`, `POLLHUP`, `POLLERR`), so `Connected` drops
  with no client I/O; a hangup on any reply read, in a blind command's drain or before a command, and a
  send that fails with `EPIPE`/`ECONNRESET`/`ENOTCONN`, lose it too (`NotConnected`, socket closed, loss
  stamped). A refused reconnect fails at once and keeps the loss stamps, so the reconnect that gets
  through times the whole outage for the #521 stops, and the #1289 rule (no stops while `:GU#` shows `I`)
  applies unchanged. A controller that drops off Wi-Fi without closing the connection is silence: the
  fault latch and the 30 s staleness bound decide, as for a silent mount on serial.
- **Bounded replies** (decisions 0007 and 0023): every reply read is bounded by the response timeout and
  by `kMaxReplyLen` (1024 bytes); a reply that reaches it without its terminator fails its read at once
  with `DriverException` "OnStep response too long ...". It is one failed exchange, as a silent reply is:
  it counts toward the decision 0009 fault latch (three in a row latch it), and is not a lost link. Before each
  command the bytes already waiting are read with no wait and discarded (at most `kMaxDrainLen`, 64 KiB),
  as `tcflush()` does over serial, so a late or overlong reply never answers the next command.
- **Rig check**: not yet run on an OnStepX over Wi-Fi or Ethernet; it is tracked in the OnStep test plan,
  #2126. Tests: the `OnStep network -` cases in `test_onstep_link_loss.cpp`, the TCP hangup cases in
  `test_onstep_telescope.cpp` and `test_mount_read_until_hangup.cpp`, and the catalog cases in
  `test_catalog_builtins.cpp`. The `[stress]` test and most of the telescope tests run over this link
  against a loopback `FakeMountServer` (`hang_up_on()` hangs up mid-exchange, `drop_connections()` at
  once).

- **Auto-detection**: probes `/dev/serial/by-id/` (Prolific/FTDI/CP210x/Silicon Labs/CH340/
  CH341 substrings, plus `Arduino`/`Teensy`) falling back to `/dev/ttyUSB0-9` **and**
  `/dev/ttyACM0-9` — OnStep boards commonly enumerate as ttyACM (native USB-CDC) rather than
  ttyUSB, unlike the RS-232-based mounts (iOptron/Celestron) this project supported first.
- **Identity probe**: `:GVP#` (get product name) must return `"OnStep"` or `"On-Step"` (both
  firmwares); `:GVN#` is read for the firmware string only. Which firmware it is matters in one
  place, see "OnStep 4.x and OnStepX" below.
- **DTR/HUPCL Arduino-reset quirk**: like the Gemini focuser wrapper, CH340/CH341/native-USB-CDC
  adapters reset the MCU on a DTR edge at port open. The probe path (`probe_onstep_port()`)
  clears `HUPCL` before *and* after probing, and the production `connect_serial()` path also
  clears `HUPCL`, so the driver's real connect (which follows a successful probe) never
  re-triggers the reset.
- **Pulse guiding is hardware-timed**: OnStep supports native duration-bearing guide commands
  (`:Mgn####`/`:Mgs####`/`:Mge####`/`:Mgw####`, 4-digit zero-padded milliseconds) — fire-and-forget,
  the mount stops the pulse itself. `IsPulseGuiding` is tracked from the known requested
  duration, not polled from the mount: each axis has its own `util::HoldWindow` (`pulse_hold_`,
  [decision 0022](../../docs/decisions/0022-hold-window.md)), armed for the duration on the task clock
  before the `:Mg` command goes out and cleared if the send fails, and `IsPulseGuiding` is true while
  either axis's pulse runs (OnStepX times each axis on its own, so a shorter pulse on one axis does not
  end the other's; before #1835 one deadline served both). It ends early on a stop that ends the pulse: `AbortSlew`
  and the same stops a `Park` or `FindHome` sends before a moving mount parks or homes (`:Q#` ends
  every guide, OnStepX `Guide::stop()`), and a `MoveAxis(axis, 0)` on the pulse's axis
  (`:Qe#`/`:Qw#` or `:Qn#`/`:Qs#` end that axis's guides), and on connect and disconnect. Past its end time a pulse still reads
  as running until the status poll publishes a frame sampled `kPulseGuideSettle` (50 ms) or more after
  that end, at most `kPulseMeasureBound` (2 s) longer (#1783): a `PulseGuide` starts one extra tick for
  then (unless the other axis's pulse ends later, whose tick stays), in a slot of its own (`pulse_measurement_`; the 1 s ticks run on through the pulse), so a position read right after `IsPulseGuiding` goes false is not one
  measured mid-pulse (ConformU's PulseGuide checks compare the RA and Dec around it). Unlike SynScan (no native pulse guide), this driver does
  **not** need a background stop-thread. A `Duration` over 9999 ms does not fit the 4-digit field
  and is refused with `InvalidValue` rather than clamped (#1478). Direction and Duration are
  checked before the connected, link-fault and parked checks, as are `SyncToCoordinates`' and
  `DestinationSideOfPier`'s RA and Dec. After those checks a pulse is refused with `InvalidOperation`, before
  anything is sent, while a slew, park or home runs (#2504, `refuse_pulse_during_slew_locked()`): the slew grace, a
  `Park` or `FindHome` still to be sent after its stops, or a frame showing a GOTO (written through at its dispatch
  and when the mount takes a park or home), `I` or `h`. It reads the frame, not `:GU#`, so a pulse costs no extra
  exchange; a handset GOTO, park or home refuses it too, once the poll sees it. A pulse on the axis a non-zero
  `MoveAxis` drives is refused the same way and the jog runs on (#2503, the #2467 rule); the other axis is accepted.
- **SideOfPier is the mount's own side, from the `:GU#` letter** (#2179). On an OnStepX German mount (`E` in
  `:GU#`) the polled frame's pier-side letter (`T` east, `W` west, `o` none) is served as it is; a fork or alt-az
  mount and OnStep 4.x, not checked on a rig, keep the hour-angle rule. OnStepX's default preferred pier side, Best, keeps the mount on
  the side it is on past the meridian, up to the meridian limit (60 minutes on the HM-17), "BEST stays on current
  side if possible" (`Goto.cpp` `setTarget`), so the hour-angle sign names the wrong side there (rig: East at HA
  +20 min with the mount still on the west), and NINA, which skips a flip when SideOfPier already reads the side
  expected after the meridian, logs "No flip is required" and lets the mount run into its limit. The
  hour-angle rule is used there and when the frame has no side: `o` (parked or at home) or firmware that sends no letter,
  and then it needs the site (`SideOfPier needs the site`, #1893). The old reason for the hour-angle rule, ConformU
  reading `EE` at HA -9 and +9, was the #1217 misparse: `E` is the mount-type letter (GEM), not the pier side, and
  `parse_status` no longer reads it as one. `get_side_of_pier()` takes the frame and never reads the mount (a live
  `:GRa#`/`:GDe#` pair there cost ConformU's 0.1 s FAST target, #1242). Any change to a pier-side source needs a
  ConformU SideofPier/DestinationSideofPier run on the bare mount.
- **AlignmentMode comes from the `:GU#` mount-type letter** (#2214), kept in driver state (`mount_type_seen_`) by
  the poll and the control-path `:GU#` reads, a mount setting and not frame telemetry (decision 0010 OnStep row), with no I/O: `E` GermanPolar, `K` (fork) Polar, `A` (alt-az) and `L` (alt-alt) AltAz, since ASCOM has
  no alt-alt value and what clients decide from it matches alt-az. OnStepX and OnStep 4.x both send the letter (4.x
  `Command.ino`: `E`, `K`, `A`, no `L`). A disconnected driver answers GermanPolar. A connected driver refuses like the other getters: a status with no
  type letter throws `DriverException` (OnStepX always writes one of E/K/A/L and 4.x one of E/K/A, so none is a
  supported answer), a latched fault
  throws the fault, a lost link `NotConnected`, and no `:GU#` status since the connect throws `DriverException`
  (decisions 0009 rules 1 and 3, 0010 rule 8), never a guessed GermanPolar. `set_connected(false)` clears the state and
  the getter also checks `published_connected_`. The letter does not change while connected, so a stale frame
  keeps the last letter seen (decision 0010 rule 6). Not hardware-validated on a fork or alt-az mount.
- **DestinationSideOfPier models the firmware's choice, SideOfPier can be set** (#2179). The settings the choice
  depends on are read on OnStepX (`is_onstepx_version()`) at connect and every tenth poll tick, and cached under
  `mutex_` (`PierSettings`), so the getters never wait on the wire: the preferred side `:GX96#`, the flips switch
  `:GX94#`, the meridian limits `:GXE9#`/`:GXEA#` and the RA-axis limits `:GXEe#`/`:GXEw#`. The read stops after
  `:GX96#` when it is not a letter, so a firmware or fake that does not know the commands costs one exchange; a read
  that fails leaves the settings unknown (connect) or as they were (poll). `DestinationSideOfPier` and
  `CanSetPierSide` read the frame's pier-side letter, mount type and RA/Dec stale or not (decision 0010 rule 6);
  with no frame they fall back to the hour-angle rule or false. The model is `util::gem_destination_pier_side()`
  in `util/pointing.h` (OnStepX `Goto.cpp` `setTarget` and `validateTarget`, read at 10.23a and 10.29c): the current
  side, the preference (Best stays unless the other side needs strictly less axis travel; East/West/Auto start from
  their side; at home the side follows the target's hour angle), and reach inside the meridian and axis limits
  (axis 1 may shift by 360 degrees). With the settings, the polled side and a GEM (`MountStatus::is_gem`) it answers
  `DestinationSideOfPier`; with any of them unknown, or when no side reaches the target (the firmware would refuse
  the GOTO), the hour-angle rule answers, as before. Best, E, W and A are the preferred-side letters (10.23a has
  E/W/B; main adds A). With the flips off (`!transform.meridianFlips`) OnStepX forces the east side only, `:GX96#`
  reads `E`, `:GX94#` reads `"<n> N"` and `:MNe#`/`:MNw#` get the unknown-command `0` (read as accepted: the `CanSetPierSide` gate keeps them from being sent); the model then reaches East only. OnStepX turns the flips on for every GEM (`Transform.cpp`), so this is a fork or alt-az case.
- **`CanSetPierSide` and the flip** (#2179). `CanSetPierSide` is true only on OnStepX, on a German mount (`E` in
  `:GU#`), with the flips on and a read preferred-side letter; it answers from the driver's state and is false
  while disconnected. `SideOfPier = n` is a command (operation lock) with the order: value 0 or 1 (`InvalidValue`,
  before the connection check), connected, `CanSetPierSide` (else `PropertyNotImplemented`), parked, link fault,
  Tracking on, no GOTO running (the slews' refusals, reading `:GU#` now), a mount that reports no side (`o`, at
  home) is `InvalidOperation` (OnStepX's `setTarget` overrides the requested side with the target's hour angle at
  home, so the flip would not go where asked), a mount already on that side
  (nothing sent), then the live position: a side that cannot reach it inside the RA-axis and meridian limits is
  `InvalidOperation`. Then `:MNe#`/`:MNw#` (OnStepX `Goto.command.cpp`: GOTO the current RA/Dec on that side of the
  pier; one reply digit with no `#`, codes as `:MS#`; only sent while `CanSetPierSide` holds, see the flips-off note above), sent through the same
  dispatch as the coordinate slews (`dispatch_goto_locked()`: re-checks, arms the slew hold, raises the digit as
  the `:MS#` refusals do, and writes `Slewing` through). It sends no `:Sr`/`:Sd`/`:MS#`, leaves
  `TargetRightAscension`/`TargetDeclination` alone, does not touch the preferred-side setting, and writes nothing
  to the mount's non-volatile memory. The call returns once the GOTO is accepted. Notes: with the meridian-flip
  pause-at-home setting the flip waits at home for the user; the GOTO is refused by the mount's own limits like any
  other (`:MS#` reply 6). Fork, alt-az and alt-alt mounts (`K`, `A`, `L`) and OnStep 4.x keep `CanSetPierSide` false.
- **`:GU#` status-character table**, confirmed against real hardware traces (`nNpHrsEo180` etc.
  observed live): `n`/`N` = **NOT** tracking / **NOT** slewing respectively (`== npos` check, i.e.
  the character's *absence* means the state is active — this is the opposite polarity of what
  the INDI driver's comments implied, and was the single biggest early bug in this driver).
  `H` = at home, `P`/`p` = parked/not parked, `I` = parking in progress, `F` = park failed,
  `h` = homing, `g` = a guide move is running (`guide.active()`: a `:Mn#/:Ms#/:Me#/:Mw#` move, so a `MoveAxis`, and also a home or spiral guide; not a pulse guide),
  `G` = a pulse guide is running. `(` lunar, `O` solar, `k` King: the tracking rate (sidereal: no letter), written
  only while tracking compensation is off (`mount.settings.rc == RC_NONE`, OnStepX `Status.command.cpp` at `af1d26bf`);
  with compensation on the frame has `r` (refraction) or `t` (OnTrack) instead and no rate letter, and the letters
  do not depend on Tracking being on (#2214). `N` covers a GOTO only: a `MoveAxis` never clears it. The mount type follows (`E` GEM, `K` fork, `A` alt-az, `L` alt-alt), then the
  pier side: `T` east, `W` west, `o` none (OnStepX `Status.command.cpp`; HM-17 traces `nNpEW180`
  = pier west, `nNpET1:0` = pier east, `nNPHEo180` = parked at home). `e` means "sync to
  encoders only": unpark, home and sync set it and the next GOTO clears it, and it matters only
  with external encoders. The last three characters are `'0' + n` for the pulse-guide rate, the
  guide rate and the general error code (`:` is 10, `<` is 12); the driver reads the first of them (third from the end) to follow
  `:GX90#` (#2214), and the last, when all three are code characters (`0` to `?`; otherwise no error), to tell a GOTO,
  park or home the mount stopped (#2296, the stop bullet below). The codes (OnStepX `Limits.cpp` `errorCode()`, OnStep
  4.x `Globals.h` `GeneralErrors`): `0` none, `1` motor fault, `2` below the horizon limit, `3` limit switch, `4` Dec
  axis limit, `5` azimuth axis limit, `6` RA axis limit (under the pole), `7` meridian limit, `12` above the overhead
  limit (these are the stop codes), `8` and `9` obsolete, `10` goto sync, `11` unspecified, `13` to `15` weather, site
  and NV init. OnStepX computes the code from the limits as they stand (`Limits::poll()`, every 100 ms); OnStep 4.x
  latches it until `:Te#`, an unpark, a home reset or a reboot (`initGeneralError()`). `F` in place of `p`/`I`/`P` is a
  failed park (OnStepX's park-sense check, OnStep 4.x `ParkFailed`). Error `7` is `ERR_MERIDIAN`: after it,
  OnStepX acknowledges GOTO, park and home commands but does not move (the driver reports them failed, #2296), and a
  manual move back inside the limit clears it (the HM-17's limits are ±60 min, `:GXE9#`/`:GXEA#`). Any other
  character is ignored (forward-compatible with firmware revisions); only a total non-response
  is a fault.
- **RA/Dec position reads use the high-precision variants**: `:GRa#`/`:GDe#` (fractional
  seconds/arcseconds: `HH:MM:SS.SSSS#` / `sDD*MM:SS.SSSS#`), not `:GR#`/`:GD#` (whole-second/
  whole-arcsecond resolution). The whole-second RA resolution (~15″ at the equator) is far too
  coarse for ConformU's 0.07 s PulseGuide tolerance on short (2-5s) guide pulses — confirmed via
  ConformU issue count dropping from 8 to 0-2 after switching. `parse_sexagesimal()` handles the
  fractional last field transparently (`std::stod` on a run of digits+`.`), no parser change
  needed.
- **MoveAxis uses a true arbitrary continuous rate, not a discrete preset**: `:RA[n.n]#`
  (East/West, RA axis) / `:RE[n.n]#` (North/South, Dec axis) set a real custom rate in
  degrees/second — confirmed against `Command.ino`/`Guide.ino`: setting one flags
  `currentGuideRate=-1`, and `:Mn#/:Ms#/:Me#/:Mw#` then resolve through `enableGuideRate(-1)`,
  which uses this custom rate instead of a discrete preset. **This was the second-biggest bug**:
  an earlier version of this driver snapped the requested rate to the nearest of OnStep's 4
  discrete named presets (`:RG#`/`:RC#`/`:RM#`/`:R9#` — Guide/Center/Find/Max, the classic
  hand-paddle rate set) and reported `AxisRates` as 4 single-point ranges to match. This looked
  ASCOM-conventional but broke real-world use: N.I.N.A.'s manual slew pad (native Alpaca
  connection) disabled both the direction buttons and the rate stepper entirely when given
  single-point (Minimum==Maximum) ranges — confirmed by a user testing against live hardware
  ("rien ne bouge" / nothing moves, rate stepper non-interactive). `AxisRates` must report ONE
  continuous range `[0, kMaxMoveAxisRateDegPerSec]`; `move_axis_start()` sends the exact
  requested rate via `:RA#`/`:RE#` rather than snapping to a tier. Do not reintroduce the
  discrete-preset approach even though it matches some other drivers' hand-paddle-only UX —
  OnStep's firmware supports true arbitrary rates and ASCOM clients expect to use them.
- **Continuous-motion rate is per-move, not fixed at connect**: `select_max_slew_rate()` (`:RS#`,
  half-max) is still called once at connect as a harmless safe default, but the real rate for
  every `MoveAxis` call is set immediately before each `:Mn#/:Ms#/:Me#/:Mw#` via the mechanism
  above.
- **A running `MoveAxis` is re-sent every 7 s** (#1378): OnStepX ends a `:Mn#/:Ms#/:Me#/:Mw#` move
  after `GUIDE_TIME_LIMIT` (default 10 s; 1-120 s, or 0 for none, set at firmware build), and every
  repeat restarts that timer (`Guide::startAxis1`). A background task (`move_axis_resend_`, an
  `AsyncOperation`) re-sends the rate and direction of each running axis until `MoveAxis(axis, 0)`,
  `AbortSlew`, a GOTO, `Park`, `FindHome`, disconnect, a lost link, or the mount ending the move (below); a stop request ends the re-send even when the stop
  itself fails, and a `MoveAxis` whose command cannot be sent leaves the other axis's re-send
  running (#1459; since #2220 a dropped TCP link is a lost link, which ends every re-send, so the
  stop half is tested with a hangup mid-exchange and the other-axis half is reviewed: no fake fails a
  send with the link still alive). If the slot refuses the new body after the move went out (no thread,
  or replaced bodies still finishing), `MoveAxis` stops that axis (both directions) and throws
  `DriverException` (if those stops fail too, the message says so and `Slewing` stays true; a
  newer `MoveAxis`, or an `AbortSlew`, GOTO, `Park`, `FindHome` or disconnect that ended the move
  meanwhile, owns the motion, so the refusal is reported as it came and nothing is stopped); the other axis keeps moving without a re-send until the next `MoveAxis`
  (#1458). The test seam is `set_move_axis_resend_spawn_for_testing()`. Each axis is due 7 s after its `MoveAxis`, then 7 s after the end of each tick
  that was due for it, whether that tick sent or skipped (OnStepX times each axis on its own), so a
  `MoveAxis` on the other axis, which replaces the body, never delays it, and `mutex_` is free
  between ticks. A skipped tick (a failed read, `G`, or near a limit) therefore puts the next
  attempt about 14 s after the last send, past the default 10 s limit: the move ends there. The re-send needs a `GUIDE_TIME_LIMIT` of 8 s or more; with less the move lapses
  between re-sends and reads as ended. Seen on the HM-17 before the fix: `MoveAxis(0, 0.5)` held
  for 20 s moved for about 10 s.
  - **On the task clock (decision 0005).** The driver takes a `util::TaskClock&` (factories
    `create_onstep_telescope()` / `create_onstep_telescope_with_site()`, default the real clock),
    the re-send slot waits through it, and the other deadlines read it too: the status poll's period
    and the age of its frame, the slew grace and the pulse-guide end (`util::HoldWindow`s, decision 0022), the synchronous-slew poll and settle
    sleeps, the elapsed time since a UTCDate write (a `util::UtcOffset`, decision 0014, cleared on
    connect and on disconnect), and the #521 relink outage:
    the driver hands its clock to the link it takes at connect
    (`OnStepProtocolWrapper::set_task_clock()`, put back to the default by `release_task_clock()`
    when the driver lets go of the link, #2223), so the link's loss and last-alive stamps and the
    outage share one clock, and it keeps the stamps of the link it let go of for its next connect. A
    change of clock forgets the link's stamps: one taken on another driver's clock is not timed
    as an outage (#1460; production keeps one clock). The re-send, tracking-restore, snapshot and
    relink-window tests inject a `FakeTaskClock` and `advance()` it (7 s per re-send tick, 1 s per
    poll tick, 250 ms steps through a synchronous slew, 2 s past the relink window, 60 s past a
    UTCDate write); a case that pins exchanges of its own pauses the poll
    (`set_status_poll_paused_for_testing()`) and drives ticks with `poll_status_now_for_testing()`.
  - **Only while the mount shows the move.** Each tick reads `:GU#` first and re-sends only when
    that read shows `g` (a guide-style move runs, either axis). A move the firmware let lapse stays
    ended: its time limit, or the 5 s `GUIDE_TIME_NO_LIMITS` cap it applies while its soft limits
    are off (`Guide::finalizeTimeLimit`), which asks for a fresh command on purpose. `G` (a pulse
    guide runs) hides `g`; that tick sends nothing and ends nothing. OnStepX keeps `G` until both
    axes are idle (`Guide::poll`), so after a pulse guide during a move the move ends at the
    firmware's time limit. Once the firmware has shown
    `g` since this connect, a `:GU#` read taken after the move started with no `g` (and no `G`)
    means the move ended at the mount (a hand controller or another app sent a stop, or a limit):
    the re-send ends and `Slewing` reads false. Firmware that never sends `g` gets no re-send, and
    `Slewing` stays true until the client stops the move, as before. On such firmware, or for a
    move that lapsed before the first tick, the body ends after the first `:GU#` read that shows
    neither `g` nor `G` while no `g` has been seen since connect, instead of reading `:GU#` every
    7 s (#1461).
  - **Braking (HM-17, OnStepX 10.23a, 2026-10-20):** after a stop `g` stays set while the axis
    brakes (`GA_BREAK`; about 0.4 s at 0.5 deg/s), and a move command in that time restarts the
    move at full rate (`Axis::autoSlew` "resum"). A stop at the mount is therefore undone when a
    tick lands in the braking; the 7 s interval keeps that rare. On an equatorial mount, with both
    axes moving, a stop of one axis alone keeps `g` set and that axis is re-sent.
  - **Altitude limits:** on an equatorial mount OnStepX stops at the horizon and overhead limits
    only on the crossing, and accepts a custom-rate move past them afterwards. The re-send
    therefore reads `:GA#`, `:Gh#` and `:Go#` and sends nothing unless the altitude is at least
    1 degree inside both limits (90 overhead means none); a move started below the horizon limit
    gets none either. `:GA#` strips refraction, so it reads at or below the altitude the firmware
    tests; the margin also covers whole-degree limits and a small pointing model. A failed or
    out-of-range read sends nothing. The firmware then ends the move after its time limit. The meridian, axis and switch
    limits need no check: OnStepX enforces them on every poll and refuses a move further out.
  - A client that goes silent is stopped by the server's 30 s motion watchdog instead of the
    firmware's 10 s.
- **Tracking on/off**: on OnStepX the start is `:Te#` (#2214; `Mount.command.cpp` `af1d26bf`): `:ST60.164275#` sets the
  tracking rate to sidereal there, so using it would cancel a lunar, solar or King `TrackingRate` (ASCOM: turning
  Tracking on uses the last selected rate). Both the client's `Tracking` write and the driver's own restores (after a
  GOTO, #1230) send `:Te#` on OnStepX; OnStep 4.x keeps `:ST60.164275#`, and stopping is `:ST0#` on both. The `:ST` pair was
  confirmed on real hardware (HM-17) to turn tracking on and off: `:ST60.164275#` (start — sets tracking
  rate to exact sidereal, `60.164275 = 60.0 × 1.00273790935`) / `:ST0#` (stop — any rate below
  0.1 Hz reads as `TrackingNone`). `:To#`/`:Tn#` (this driver's original guess, inferred from the
  INDI driver's tracking-*compensation* switches) do **not** toggle tracking — they instead
  control `rateCompensation` (refraction/full-model RA-rate compensation, `:Tr#`/`:Tn#`/`:To#`),
  a completely different feature (confirmed via firmware source `Command.ino` and an A/B test on
  real hardware). Do not reuse `:Tr#`/`:Tn#`/`:To#` for tracking on/off.
- **Longitude is West-positive on the wire**, despite ASCOM's East-positive convention and
  despite the lenient parser accepting either sign silently. Confirmed empirically on real
  hardware via a GMST/LST cross-check (mount's own `:GS#` vs. independently-computed sidereal
  time at matching instants): longitude=0 gave a near-exact match; the correct (site) longitude
  with the ASCOM sign gave a large, systematic LST error; negating it gave an exact match to the
  second. `format_longitude()` negates on write, `get_site_info()` negates on read.
- **Local time is sent as UTC directly, with offset=0.0**: `:SL#`/`:SC#`/`:SG#` accept a "local
  time + UTC offset" pair, but combining a nonzero offset with local time produced a further,
  not-cleanly-explained ~85-minute LST error on real hardware (not a clean multiple of the
  offset). Fixed pragmatically — an established pattern for LX200-family mounts — by always
  computing UTC via `gmtime_r()` and sending it as "local" time with `offset_hours=0.0`,
  sidestepping the firmware's own timezone/DST handling entirely. Verified via hardware:
  offset=0 + longitude=0 gave a GMST match within 6 seconds.
- **`:SG#` (UTC offset) parser is strict integer-hour ± optional fixed minutes**: accepts
  `sHH#` or `sHH:MM#` with `MM` restricted to `{00,30,45}` (`atoi2()`-based parser) — a decimal
  form like `"+01.0"` is rejected outright. `set_time()` builds whole-hour + nearest-of-{00,30,45}
  minutes rather than a naive `%+05.1f` format.
- **Bare-ack, no-`#`-terminator quirk on "set" commands**: `:Sr#`/`:Sd#`/`:MS#`/`:St#`/`:Sg#`/
  `:SL#`/`:SC#`/`:SG#` all reply with a bare `"0"`/`"1"` digit and **no trailing `#`** (firmware's
  `supress_frame=true`/`boolReply=false`), unlike the vast majority of LX200 commands. Every one
  of these call sites must use `send_command(cmd, /*require_hash_terminator=*/false,
  kSetAckTimeoutMs)` — using `require_hash_terminator=true` here (the initial, wrong
  implementation) hung every slew/site/time write until timeout and was the root cause of the
  first several ConformU failures (`SlewToCoordinatesAsync`, site/time writes).
  `kSetAckTimeoutMs = 1000` bounds the failure case only; real acks arrive in well under 100ms.
- **Replies are read with `util::read_until`** (`util/serial_read.h`, the shared
  [serial rule](serial-autodetect.instructions.md#serial--socket-io-always-use-the-shared-helpers-utilserial_ioh)):
  `read_response()` waits in `poll()` for the `#`, or for an unterminated ack the `#` or 100 ms of
  silence after its first byte, with no per-byte pacing sleep; when the timeout runs out the byte in
  the next read slice still settles the reply (a late `#` ends it; `ReadSpec::late_byte_grace` set to
  `read_slice()`, whose TCP value is `kSocketTimeoutMs`, the socket's `SO_RCVTIMEO`). The probe's `:GVP#`/`:GVN#`
  reads and `drain_stale()` are `read_until` calls too. A hangup on the tty loses the link
  ("read failed: <errno text>", below), a read that ends on silence checks the node, and over TCP
  a hangup loses the link as well ("the controller at <host>:<port> closed the connection", #2220)
  instead of timing out (`test_mount_read_until_hangup.cpp`, the `OnStep mount - ...` cases).
- **The serial read slice must be 100 ms, not higher** (`kSerialReadSliceMs`; it was `VTIME=1` on
  a blocking fd before #1202): an earlier version copied Gemini's one-time
  probe `VTIME=20` (2s) into the production `connect_serial()` path, causing every read
  to cost up to 2 real seconds — most visibly a ~2s stall on `PulseGuide` and pushing `AbortSlew`
  (5 sequential blind commands) toward ConformU's 1.0s STANDARD timeout. `send_command_blind()`'s
  stale-input drain window is `50ms` (half a read window) for the same reason — `120ms` (2+ windows)
  pushed `AbortSlew` to 1.04s, just over budget. The drain still waits at least one read slice for
  a first byte and ends after a quiet slice, as the byte-at-a-time loop did, and it ends without an error
  at `kDrainCap` (2 s, first-byte wait included) on a line that never goes quiet: it holds `io_mutex_`, so
  every command waits behind it (#1449), and before the cap a byte inside every slice could hold it for hours. After an aborted GOTO, `AbortSlew` also reads
  `:GU#` once to decide the tracking restore (#1230): 0.59s in total on the TeSeek HM-17. The fd is
  non-blocking (`util::SerialLink`), so a port another program retunes to `VMIN=1` can no longer park
  the drain after a blind command (the TeSeek HM-17 hang reported on #1202).
- **A pulled adapter is a lost link, not a stuck one** (#1241, decision 0009): the wrapper records
  the node the configured path reached at connect, and `link_alive()` (no I/O) reports the link
  lost when that path is gone or points elsewhere, or when a read or write on the fd fails with
  `EIO`/`ENXIO`/`ENODEV`/`EBADF` (the `util::SerialLink` verdict since #2262: the reads go through
  `read_until(link_, ...)` and the writes through `link_.write_all()`, so the wrapper keeps no serial errno list of its
  own; an `EBADF` logs one ERROR line, since our own fd is unusable; a successful connect and a Disconnect clear the
  link's verdict with the wrapper's latch). Lost means `Connected` false, `NotConnected` instead of a
  generic "Failed to send command to mount" while `Connected` still read true (a reply cut off
  mid-read no longer waits out the response timeout either), the fd closed (so the replugged
  CH340 can come back as `ttyUSB0`), and `get_link_fault()` naming the reason. A `Connected=true`
  then reconnects. OnStepX keeps running a GOTO while the cable is out, and a `MoveAxis` for up to
  about 10 s, after which this firmware ends it by itself (HM-17: 5 degrees at 0.5 deg/s in #1098,
  2.5 degrees at 0.25 deg/s in #1241; it moves from the home position too, HM-17 2026-10-19), so the reconnect after an outage of `util::kRelinkMotionPreserveWindow` (5 s) or more
  sends AbortSlew's stops (`:Q#`, `:Qn/s/e/w#`); a shorter outage, or a connect with no recorded
  loss, stops nothing (#521). A park still running is left to finish (#1289 item 5): the reconnect reads `:GU#` first and sends no stops while it shows `I` (parking), since `:Q#` would abort the park and leave the mount unparked where it stopped (OnStepX `Park::requestAborted()`); a status it cannot read stops anyway, after waiting out that read's reply timeout. A GOTO, park or home the driver started that the reconnect leaves running stays watched (#2296, the stop bullet).
  They leave tracking as it was: `:Q#` stops a GOTO through `goTo.abort()` and the guide stop and
  never calls `mount.tracking(false)` (HM-17 2026-10-24: `:GU#` `pEW160` before, `NpEW160` after a
  `:Q#` sent mid-GOTO), so nothing has to restore tracking after them.
  The outage runs from the last moment the link was seen alive (connect, a live `link_alive()`,
  the check before each exchange) to the end of the reconnect, so the reconnect's own time counts,
  an auto-detect re-scan included. It is not timed from when the loss was noticed: the status poll
  checks the link once a second, but the router reads `Connected` before every Connect, so the
  first to notice can be the reconnect itself. Tracking is left alone. Over TCP the same rules hold with a hangup as
  the evidence (the Network bullet under "Connection types"). `link_alive()` runs without `mutex_`, so it records a loss only while the node it checked is
  still the current link (a generation counter under `link_id_mutex_`): a Disconnect or a relink that
  lands between its `stat()` and the record leaves no loss behind and does not close the new link, and
  a Disconnect bumps the generation before it closes the port; the close that follows a recorded loss
  re-checks under `mutex_` that the link is still connected and still lost (#1289).
- **A mount that stops answering with the adapter still plugged in latches a fault, then a loss**
  (#1414, decision 0009): mount unpowered, or the cable pulled at the mount end, so the node and the
  fd stay healthy and nothing answers. Each failed poll tick (below; it ends at its first failed
  exchange) and each failed control-path `:GU#` read counts in `util::PolledLinkHealth`: the first two
  in a row are logged WARN and the frame is still served, the third latches a fault (ERROR): the
  getters throw `DriverException` "OnStep mount communications compromised" while `Connected` stays
  true and `get_link_fault()` reads "Mount not answering (...)". The poll keeps asking every second,
  and one good tick clears the fault (INFO). A failed `:GA#`/`:GZ#` read on a slow tick is logged and
  keeps the last Alt/Az; it does not count. Altitude and Azimuth can therefore be older than
  `DeviceState`'s `TimeStamp` when `:GA#`/`:GZ#` reads fail while `:GU#` and RA/Dec answer, because they keep
  their last values. The reads that answer from the driver's own state,
  `SiteLatitude`, `SiteLongitude`, `UTCDate`, `SiderealTime`, `DestinationSideOfPier`, `GuideRate` (the mount's `:GX90#` value),
  `TargetRightAscension`/`TargetDeclination` (#2075),
  `DoesRefraction`, `IsPulseGuiding`, `Slewing` (also while a `Park` or `FindHome` is pending or within
  the slew grace) and the rate getters (`DeclinationRate`, `RightAscensionRate`, `TrackingRate`, which
  answer from the driver's rate state, below), throw the fault too (decision 0009 rule 1, as in
  Sky-Watcher, open-astro#882). The client's own values answer: `SlewSettleTime`, `SiteElevation` (the device config) and the `Can*` constants. A getter asks the mount nothing while faulted, so a faulted
  `DeviceState` costs no reply timeout (the 2 s back-off of #1626 is gone with the getter reads). Writes
  throw the same `DriverException` (`refuse_while_faulted_locked()`; the motion and state commands and
  the site and `UTCDate` writes (#1626), not the raw `command_blind`/`command_bool` pass-through): the
  commands are blind, so on a healthy fd they cannot fail and would otherwise report success to a mount
  that is not listening. `AbortSlew` and `MoveAxis(axis, 0)` send their stops first and then throw, so
  the stop is still tried and the client learns it may not have arrived (`AbortSlew` gives up its
  #1230 tracking restore without asking the mount); a new `MoveAxis` move is refused before it ends a
  waiting `Park` or `FindHome`. A fault that has stood for `util::kLinkStalenessBound` (30 s, on the
  driver's task clock) is a lost link: the next failed poll tick, or a refused write, calls
  `OnStepProtocolWrapper::declare_link_lost()`, which records the reason and the loss time and closes
  the port, as for a pulled adapter, so `Connected` reads false, calls throw `NotConnected`,
  `DeviceState` is empty, the poll body ends, and the next Connect relinks (and runs the #521 stops
  after a long outage). A getter, which takes no operation lock, only throws the fault; the poll finds
  the bound within a tick. That outage runs from the loss, not from the last good reply: the failed
  reads still found the fd alive, so a Connect right after the loss stops nothing, and one after the
  preserve window sends the stops. A silent mount latches in about three poll periods plus three
  reply timeouts.
- **An equatorial slew needs Tracking** (#1274, the [ascom-contract rule](ascom-contract.instructions.md#equatorial-slews-require-tracking-all-telescope-drivers)): `SlewToCoordinates(Async)` and
  `SlewToTarget(Async)` throw `InvalidOperation` ("<member> requires Tracking to be true") while
  `Tracking` reads false, after the coordinate validation (which comes before the connection check)
  and the parked check, and before any target or GOTO command is sent (`check_equatorial_slew_locked()`). Tracking is read as the getter reads it, from the polled frame,
  so a `Tracking=false` just written counts as off. `SlewToAltAz(Async)` skips this check and has the opposite one
  (refused while Tracking is true, #1377, see below), then converts to RA/Dec and calls
  `start_slew_locked()`. OnStepX turns tracking on at a GOTO only on the first GOTO after a
  home reset (`firstGoto`) or one that starts from home (`Goto.cpp`; an HM-17 run on 2026-10-18 read `Tracking`
  true after a GOTO, which fits one of these), otherwise it leaves tracking as it was, so the
  refusal is the interface contract. `:GU#` keeps the tracking flag through a whole GOTO.
- **A slew while a GOTO runs is refused** (#1379): OnStepX answers a second `:MS#` by stopping the
  running GOTO and refusing the new one (`Goto.cpp` `request()`: `CE_SLEW_IN_SLEW` calls `abort()`,
  reply `5`; seen on the HM-17 2026-10-18, the first GOTO stopped about 1 degree short). So every slew
  (`SlewToCoordinates(Async)`, `SlewToTarget(Async)`, `SlewToAltAz(Async)`) throws
  `InvalidOperation` ("<member> is not allowed while a GOTO is running") before it cancels or sends
  anything while `:GU#` shows a GOTO, the driver's own within the 2 s slew grace included
  (`refuse_while_goto_running_locked()`). That covers a park or home already moving and a handset GOTO
  too, which the second `:MS#` would also have stopped. A home with home sensors runs as a guide
  move, not a GOTO (`:GU#` shows `h` with `N`), so it is refused separately ("<member> is not allowed
  while the mount is homing"), the syncs included: before, the slew ended the waiting `FindHome` and
  the mount refused it as already moving (reply `8`). Outside the slew grace the check reads `:GU#`
  on each slew and sync, never the polled frame alone, so a GOTO or home started from the handset is
  seen at once (before #1783 it read the 2 s status cache); inside the grace it refuses without a read.
  The grace is `slew_hold_`, a `util::HoldWindow` ([decision 0022](../../docs/decisions/0022-hold-window.md)):
  armed before the `:MS#` goes out and cleared when the `:MS#` is refused or fails, by AbortSlew and the
  Park/FindHome pre-stops, on connect and disconnect, and at the GOTO's end. Landing never reads the
  window: it is a reading that shows no GOTO, from the poll or a control path, and it clears the window,
  so a short GOTO reports landed when the mount stops, not at 2 s. Landing relies on `:MS#` setting the
  GOTO state before it replies, so the first `:GU#` after it shows the GOTO. From the source: OnStepX
  `Goto::request()` sets `state = GS_GOTO` (`Goto.cpp`) and `:GU#` writes `N` only for `GS_NONE`
  (`Status.command.cpp`), read at OnStepX 10.18a (`c3d28e101b`) and 10.29a (`main`, `0420f16f1b`); OnStep 4.x
  `goTo()` sets `trackingState=TrackingMoveTo` (`Goto.ino:382`) and `:GU#` writes `N` only otherwise
  (`Command.ino:714`), read at OnStep 4.24q (`3b73bffcfb`) and the same in the final 4.24s (`release-4.24`). On hardware, `:GU#` showed the GOTO 1 ms after `:MS#`
  answered (HM-17 OnStepX 10.23a, 2026-10-29). So the window only keeps `Slewing` true until the first
  reading (#1887). A stale reading cannot end the slew or send the tracking restore: a reading and the `:MS#` share
  the operation lock, and the dispatch write-through shows the GOTO until a later reading replaces it (#1835).
  `AbortSlew` ends the grace, so the next slew goes out once `:GU#` shows the GOTO ended. OnStepX keeps the
  GOTO state while it brakes after `:Q#` (HM-17 2026-10-24: about 1.6 s), and a slew in that time
  is refused, as the firmware would refuse it (reply `5`). A `:MS#` the mount refuses reports its reply digit and reason (the digits are
  listed under "Commands this driver sends"; 4 parked is `InvalidWhileParked`), not "below horizon
  or limit" for every refusal.
- **A GOTO, park or home that the mount stops or abandons fails** (#2296): OnStepX stops a GOTO that crosses a limit
  (`Limits::poll()`: `stop()`/`stopAxis1()`/`stopAxis2()` abort it and turn tracking off; a park then ends unparked,
  `Park::requestAborted()`), and only the `:GU#` general error says why. The driver watches each GOTO it sends (the
  coordinate and Alt/Az slews, the flip) and each park or home the mount takes (`note_motion_end_locked()`), never a
  motion it did not start, until a status shows it ended. A failure is kept as a `Park`/`FindHome` refusal is (the
  Park and FindHome bullets below) and raised by `Slewing`, and by `AtPark` (a park's) or `AtHome` (a home's), as
  `DriverException` "<member> failed: ...", until the next motion command or a reconnect; a synchronous slew throws it.
  The log line is WARN the first time each reason (or a landing) appears with a stop code, whichever member started
  the motion, and INFO for each repeat of one already logged, until a status shows another code (decision 0007 rule 9: a client can repeat
  a GOTO into a limit that stands, such as the meridian limit).
  Every status read is checked (the poll's before its restore decision, the control paths' through
  `note_status_read_locked()`), so `Slewing` never reads an end before it is judged. From the firmware sources (OnStepX
  `83857454`, OnStep 4.x `3b73bffcfb` and the `release-4.24` tip), not seen on a rig. Tests: the `OnStep limit stop -`,
  `OnStep park watch -` and `OnStep home watch -` cases.
  - **A slew** ends with no GOTO. With no stop code it landed, as before. With one, where the mount is decides, the
    same way on both firmwares, since the code alone is not evidence: OnStepX writes the limits as they stand, so a
    GOTO that lands close to a limit (a target may lie right at it: `Limits::validateTarget()` refuses only one beyond)
    and then tracks across it shows the code in its first end frame, and OnStep 4.x latches the code, so a GOTO can
    start with an older stop's code. Within 1 degree of the target it landed and the log names the code (at WARN the
    first time, as above); away from it, or with a position that cannot be read (a stop code is never read as a landing
    without the position that shows it), the mount stopped short: "<member> failed: OnStep stopped the GOTO at the
    horizon limit (:GU# general error 2)". A GOTO started with tracking off (the Alt/Az slews) holds its target's
    hour angle on both firmwares (OnStepX `Goto.cpp` moves the target only while tracking; OnStep 4.x likewise), so its target RA is advanced by the sidereal
    time since the command before the comparison: a slow Alt/Az slew that sits at the requested Alt/Az lands. A flip
    is judged against the position it slews back to and, when the frame reports a side, the requested pier side; the
    side alone is not enough, since OnStepX derives it from axis2 and a flip stopped part-way can already show it. 1 degree is generous on purpose (the driver had no
    landing tolerance; other drivers here use 3 to 600 arcseconds, and it is 4 minutes of a stopped mount's RA drift),
    so a landing is never read as a stop; a stop that ends inside it reads as a landing. It is not tightened, for the
    same reasons a GOTO that ends with no stop code is not judged by its position (below), and the log names the code.
    The frame's position decides on the poll; a control-path read without one reads `:GRa#`/`:GDe#` at once (the only
    new exchange, and only then), and if that fails the next status decides. No #1230 tracking restore follows a stop code on OnStepX, landed or not:
    it stops tracking at the horizon and overhead limits only on the crossing (`!lastError.altitude.min &&
    error.altitude.min`) and does not limit-check `:Te#`, so a restore would track on past them. On OnStep 4.x a landing
    keeps its restore, since 4.x re-checks its limits on every pass and stops tracking again past one (`OnStep.ino`
    safety checks); a slew that stopped short gets none. `AbortSlew` keeps the watched slew through its own supersede
    until its restore decision, since its `:GU#` read may be the first to see the stop, then drops the watch and the
    failure (it is a new motion command), also when a stop fails: a `:Q#` that fails may still have reached the mount
    (the wrapper writes it, then drains, and a hangup in the drain fails it), so a kept watch could report the client's
    own abort as a limit stop, on OnStep 4.x from an older held code. A link lost before the call fails it with
    `NotConnected` before the watch is touched, and a relink inside the preserve window still judges that GOTO. The
    abort logs what its read decided at INFO only. On OnStepX it also sends no restore when its read shows a stop code,
    GOTO flag or not: a limit stop runs `goTo.abort()`, which keeps `GS_GOTO` while the mount brakes, and has already
    turned tracking off. On OnStep 4.x the abort's read is judged like any end: with an older stop code still latched
    (it is held until an unpark, a home reset, a reboot or an accepted `:Te#`; the driver starts 4.x tracking with
    `:ST60.164275#`, which does not clear it, so it can be held long after the stop), a slew aborted short of its
    target reads as stopped short and also gets no restore. A GOTO that ends away from its target with no stop code (a
    handset abort) still reads as landed: with no code the end position would be the only evidence, and a healthy GOTO can end away
    from the target the driver sent. OnStepX takes a `Tracking` write during a GOTO (`:Te#`/`:Td#` have no GOTO
    refusal), which changes whether the firmware holds the target's RA or its hour angle; a tracking OnStepX GOTO
    follows the RA and Dec rate offsets on its final approach (`Goto.cpp` `poll()`); and a relink judges a GOTO the user
    may have moved on from with the handset during the outage.
  - **A park** ends with no GOTO and no `I`; both firmwares go from parking to parked or failed in one step (OnStepX
    `Goto.cpp` `poll()` then `Park::requestDone()`, OnStep 4.x `MoveTo.ino`), so no healthy park shows that frame
    unparked. Parked succeeded; `F`, a stop code, or (once a status showed the park running) an end without either, a
    handset abort or a park that gave up, fails it: "Park failed: the mount ended the park without parking".
  - **A home** ends with no GOTO, no `h` and no `g`. At home (`H`) succeeded. A home that ran as a GOTO (no switches:
    OnStepX `Home::request()` sets `HS_HOMING` and starts the GOTO before `:hC#` returns; OnStep 4.x `goHome()`) and
    ends elsewhere fails: "FindHome failed: the mount ended the home without reaching home", as does one whose end
    frame carries a stop code before any status showed it running. A home with switches (`h` or `g` with no GOTO) is
    not judged from such a frame: after the switches OnStepX moves on to the sense offset (up to
    180 degrees, `Home.command.cpp` `:hC1`/`:hC2`) as an axis move with nothing in `:GU#` (`Home.cpp` `guideDone()`),
    and OnStep 4.x is idle between its fast and slow search (`Home.ino` `FH_IDLE`). Neither is a home not yet seen
    running. Those are left to the bound. A switch home that fails is not always seen. An OnStepX search that a
    handset or ST4 stop ends, or that reaches its 600 s `GUIDE_HOME_TIME_LIMIT`, re-zeroes the axes at the home position
    where they stopped (`Home.cpp` `guideDone(false)` calls `reset()`). `H` is a match of the axis coordinates with home
    (`Mount.h` `isHome()`), so the end frame shows `H` and the driver reports the home reached. OnStepX keeps the
    failure (`home.failed`) out of `:GU#` and every `:GX` reply. A mount with absolute encoders, or one whose reset the
    sync threshold refuses, shows no `H`, so the bound fails it. OnStep 4.x latches error `3` on a timed-out search and
    does not set `atHome` (`Home.ino` `checkHome()`), so the bound fails it. On a mount built with a tangent arm or a
    sector gear (`:GXEF#`/`:GXEG#` answer `1`), FindHome centres that axis and neither firmware then reports `H`
    (OnStep 4.x `MoveTo.ino` sets `atHome` only without a tangent arm; OnStepX `Home::request()` moves the axis with no
    home state). So that home is not watched: `AtHome` stays false and no failure is kept.
  - **The bound**: a park or home not ended within its bound after the command fails ("the mount did not finish the
    park within N minutes"), checked on every status read and by the getters, on the task clock (decision 0005). The
    bound is 2160 s divided by the base slew rate in deg/s (`:GX93#` us/step and `:GXE4#` steps per degree, read at
    connect), never under 20 minutes and never over 24 hours. Both firmwares move a park or home at no less than a quarter of the base: the
    slowest preset halves it (`Goto.cpp`, 4.x `Initialize.ino`), and the 4.x switch search halves it again
    (`Home.ino`). A move is at most 180 degrees twice (1440 s at a quarter of 1 deg/s), with half again for margin;
    the quarter also covers an OnStepX `AXIS2_SLEW_RATE_PERCENT` down to 50 at the slowest preset. A mount whose rate does not read keeps 20 minutes, which is less than the 36 minutes the derivation gives a 1 deg/s base (it covers that base at half rate, as before #2296). The bound is fixed when the
    command is taken, and a relink keeps it. A status showing `w` (a GOTO waiting at home for the user) restarts it.
    It only reports: nothing is sent, as for the driver's other timed-out waits, and the client can `AbortSlew`.
    INDI's `lx200_OnStep` uses 300 s from `:hP#`.
  - **Across a relink** (#521): a relink that leaves the motion running, inside the preserve window, or a park past it
    (#1289), keeps the watch, and the relink's first `:GU#` (the connect's seed frame, with its position) judges it at
    once by the rules above, so a stop during the outage still fails it. When that frame has no position the check
    reads `:GRa#`/`:GDe#` itself; a link lost there fails the Connect through the connect's own lost-link check, as any
    lost seed read does. A relink that sends the stops drops it; a
    kept failure is cleared as by any reconnect. The park and home bound runs on from the command: the outage is part
    of the park's time. No baseline code is kept: the end position decides a slew, so a 4.x code that changed or was
    cleared during the outage changes nothing.
- **Motion fails closed: bounds and checked stops** (#2406, under the "motion fails closed" rule that #2395 and #2396 set for every telescope driver). A GOTO the mount still shows `kGotoTimeout` (120 s) after its `:MS#` is stopped with AbortSlew's stops: the status poll checks it each tick for an async slew (`check_motion_bounds_locked()`), and the synchronous slew's wait (whose 120 s runs from the start of the wait, a few exchanges after the `:MS#`), which used to throw with no stop, sends them before it throws. If the AbortSlew check then fails too, its text is appended to the bound's. A `:MS#` with no digit before its reply timeout is followed by `:Q#`, since the mount may have started it. A GOTO's bound is cleared when it lands (the poll, or the synchronous wait's own `:GU#`) and survives a MoveAxis started during it. A `MoveAxis` runs at most `kMaxMoveAxisDuration` (60 s): the re-send body wakes at that deadline too and stops the axis as `MoveAxis(axis, 0)` does (`stop_capped_move_locked()`); firmware without `g`, whose re-send body ends at once (#1461), relies on the firmware's guide time limit as before. After AbortSlew, the status poll checks the stop: while `:GU#` still shows a GOTO, a home search or a guide move (not a pulse guide), it sends the stops again every `kAbortStopRound` (2 s), `kAbortStopRounds` (3) rounds in all, the AbortSlew included; motion after that stores a failure. Each of these failures is `motion_error_` (the MoveAxis cap is logged at INFO, since a client causes it, decision 0007 rule 9; a stop that failed at ERROR), which Slewing raises as DriverException (and the synchronous wait throws) until the next motion command (slew, MoveAxis, AbortSlew, Park, FindHome) or a connect. `SiteLatitude`, `SiteLongitude` and `UTCDate` writes are refused with InvalidOperation while a motion of this driver runs: a GOTO until a frame shows it landed, a MoveAxis, or a Park or FindHome being started (`refuse_while_moving_locked()`, driver state only, so a `:GU#` it cannot parse refuses nothing and a hand-controller motion is not seen); a pulse guide does not count. `MoveAxis(axis, 0)` is allowed while parked (#2435): its `:Qe#`/`:Qw#` or `:Qn#`/`:Qs#` change nothing on a parked mount, and a non-zero rate there stays InvalidWhileParked. The 120 s GOTO bound and the about 6 s AbortSlew check are fake-only so far: a slow build flipping 180 degrees, or a high slew rate's braking, may need more, which the #2406 hardware test plan checks. Not done here: the relink stop after a short outage (`kRelinkMotionPreserveWindow`), a landing gate for a far miss, a sync bound and a setting for the MoveAxis cap. Tests: the `[goto-safety]` cases in `test_onstep_telescope.cpp`.
- **Refusals are read, not drained** (#1379): `Tracking` writes, `SetPark`, `Unpark` and the syncs
  used to send their command blind or ignore the reply, so a mount that refused them read as
  success. They now read it: a bare `0` from `:ST`/`:hQ#`/`:hR#` throws `InvalidOperation`
  ("<member> refused by the mount (<reason>)") with the `:GE#` reason, unless `:GE#` then says `00`
  or `25`, which is taken as accepted, as for `:hP#`. With no `:GE#` code (firmware without `:GE#`), the `:GU#` the driver has just re-read decides: an `Unpark` whose mount reads not parked, or a `Tracking` write the mount now shows, is taken; `SetPark` stays a refusal ("no reason given"), since no `:GU#` letter shows the park position; `Unpark` on a mount that is not parked (`11`)
  stays ASCOM's no-op. A `:CM#` `E<n>` throws with the `:MS#` digit's reason, and a sync while a
  GOTO runs is refused before anything is sent, as the slews are (OnStepX refuses it with reply
  `5`). `SyncToTarget` reports under its own name. The driver's own tracking restores after a
  slew stay blind, so AbortSlew's timing is unchanged.
- **An Alt/Az slew is refused while Tracking is true** (#1377, ITelescopeV4 `SlewToAltAz`): both
  forms throw `InvalidOperation` ("<member> is not allowed while Tracking is true") after the
  Alt/Az validation and the parked check, before anything is sent, under their own names
  (`util::check_alt_az_slew()`, the command gate). With Tracking off the driver converts to RA/Dec and runs the same
  GOTO as an equatorial slew; it does not use OnStepX's `:MA#`.
- **`Connected` is read lock-free** (#910): `set_connected(true)` holds `transition_mutex_` and the
  operation lock for the whole connect (port open, handshake, site and time sync, the first frame, an
  auto-detect scan with no explicit port), so `get_connected()` reads the atomic `published_connected_`
  instead, plus `link_alive()`, which takes no lock a connect holds. The flag is raised only after the warm-up and dropped before any disconnect,
  so `Connected` stays false while `Connecting` is true (the tier-2 contract sweep checks it). The
  internal `connected_` goes true right after the port opens, before the warm-up, because the warm-up's
  own helpers check it; a disconnect clears it, with `published_connected_`, before the close.
- **Getters and `DeviceState` are served from a polled snapshot, never from the mount** (#1783,
  [decision 0010](../../docs/decisions/0010-snapshot-served-state.md), the SynScan and Celestron shape).
  One status poll per connection runs in an `AsyncOperation` slot (`status_poll_`), started at the end
  of a connect (its first tick 1 s later: the connect seeds the frame; a poll that does not start, or a link
  lost during the connect's reads, fails the connect, which is undone as a Disconnect is, #1895) and joined, with the pulse-end
  tick's slot, before a disconnect or a relink closes the port and at the start of every connect, with
  no driver lock held. Every `kStatusPollPeriod` (1 s, start to start on
  the task clock, missed ticks skipped) it reads `:GU#` then `:GRa#`/`:GDe#`, and every
  `kSlowPollEvery`-th (2nd) tick `:GA#`/`:GZ#` and, while the site is not known, `:Gt#`/`:Gg#` (every
  `kSiteInfoRetryDelay` at most), into `util::StateSnapshot<PolledFrame>`. A `PulseGuide` adds one
  tick, in a slot of its own, that measures the pulse's end (see the pulse guiding bullet). The tick holds
  the operation lock for its exchanges, so it never lands inside a command's sequence, drops `mutex_`
  for each, and publishes under `mutex_` alone after `begin_poll()`. Publishes are ordered by sample
  (`PolledFrame::sample_seq`, numbered under the operation lock): a tick, the pulse-end tick or the
  synchronous slew's end read whose sample is older than the held frame's is skipped, and a newer one in
  flight is left to publish (before #1835 a publish dropped every other sample in flight, newer ones
  included, so `IsPulseGuiding` could stay true about 1 s longer and Alt/Az be 2 s older). A tick ends at its first failed
  exchange of `:GU#` or the position, so a Disconnect waits for one reply timeout. Served from the
  frame, stale or not (`kSnapshotMaxAge` 3 s is the freshness bound; stale never refuses, decision 0010
  rule 6): `RightAscension`, `Declination` (through the rate-offset model while an offset runs, see "Rates"),
  `Altitude`, `Azimuth`, `Tracking`, `AtPark`, `AtHome`,
  `Slewing` (with the driver's own MoveAxis flags, slew grace and pending Park/FindHome), `SideOfPier`
  (on an OnStepX German mount the frame's `:GU#` pier-side letter; otherwise, or when there is none, the hour-angle
  rule from the frame's RA, #2179) and `DeviceState`, whose `TimeStamp` is the frame's `measured_at`
  (`device_state_age()`). A field the mount has not answered yet throws `DriverException` "not
  available"; a latched fault and a lost link refuse as above. `AlignmentMode` and `TrackingRate` are not
  frame telemetry but driver state the poll keeps current from the `:GU#` letters (decision 0010 OnStep row, #2214):
  a stale frame means they keep the last letter seen, and they refuse like the other getters: the fault,
  `NotConnected`, or `DriverException` with no status since the connect. While the frame has no status (every `:GU#`
  since the connect failed), a command that checks for a park or writes the status through (`PulseGuide`, a
  moving `MoveAxis`, `FindHome`, `Tracking=true`, `Unpark`, the slews and syncs) reads `:GU#` first and throws
  `DriverException` ("the mount has not answered :GU# since the connect") when that read fails too, rather than take
  the unread status for "not parked"; `AbortSlew`, `MoveAxis(axis, 0)` and `Tracking=false` are stops and are
  never refused over an unread status (`Tracking=false` tries the read, sends `:ST0#` if it fails, and its
  write-through then does not land; a lost link and a latched fault still throw) (#1895). Client commands write through
  (`snapshot_.write()`): a sync's position, `Tracking`, `Unpark`, a slew start (`Slewing` true), a park
  or home the mount took, and a pulse, a MoveAxis start and AbortSlew drop the tick in flight, so a tick
  that sampled before the command cannot publish over it. Control paths keep their direct reads and
  write them through (`refresh_status_locked()`, only a status that differs from the frame's, so a loop
  that re-reads `:GU#` does not drop every tick): the slews' and syncs' GOTO check (their Tracking
  check reads the frame, as the getter does), Park and FindHome and their
  body, the synchronous slew's wait (which publishes a full measurement where the slew ends; a failed one
  keeps the last values and counts toward the fault, as a failed tick), AbortSlew's
  tracking decision and the MoveAxis re-send tick. The tracking restore a coordinate slew owes (#1230)
  is sent by the tick that first reads the GOTO ended in a frame sampled after the `:MS#` answer (not during a MoveAxis or
  while a Park or FindHome is pending), under the same operation lock as its `:GU#`. Test seams:
  `set_status_poll_paused_for_testing()`, `poll_status_now_for_testing()`,
  `set_status_poll_after_sample_hook_for_testing()`, `status_poll_running_for_testing()`,
  `pulse_measurement_running_for_testing()`, `set_pulse_guide_gap_hook_for_testing()`. Tests: `test_onstep_telescope_snapshot.cpp`.
- **Locks: getters never wait on the wire** ([decision 0016](../../docs/decisions/0016-getters-never-wait-on-device-io.md),
  #1449, the shape of Sky-Watcher's #1530). Order: `transition_mutex_` (`set_connected()` against
  `set_connected()`; no getter takes it) -> `io_mutex_` (the operation lock) -> the shared link's sequence
  lock (#2223, taken with `mutex_` released, through `Wire`) -> `mutex_` (the state lock, a leaf, retaken
  inside a held sequence after each exchange); the link's wire lock is taken under the sequence lock and never
  under `mutex_`; the `StateSnapshot` and slot mutexes are leaves under `mutex_`.
  Every command, and each poll tick, holds `io_mutex_` for the whole command through a `StateLock`
  (Mode::Io, the default), which also holds `mutex_`; its `unlock()` releases both, so the places that let
  other commands in (a task-clock wait, a slot start) are unchanged. Every exchange goes through `Wire`
  (`wire()`), which drops `mutex_` for the call and takes it back after (the shared `util::LockGate`, #1729:
  its held-lock table and `StateLock`, which refuse on a full table, decision 0016 Rule 6), so `mutex_` is
  never held across serial I/O, and sequences that must stay together (`:hP#` or `:hC#` then `:GE#`,
  `:Sr#`/`:Sd#` then `:MS#` or `:CM#`, the re-send tick's reads and move, a poll tick's reads and its
  tracking restore) stay together under `io_mutex_`. `Wire` refuses an exchange from a thread without the
  operation lock (`DriverException`). A getter takes `mutex_` alone (`StateLock::Mode::State`) and answers
  from the frame and the driver's state; it never reaches the wire (the 2 s caches and their single read
  under `io_mutex_` (`LockGate::single_read()`), with its session re-check and the
  `slew_epoch_`/`tracking_epoch_` epochs, went with #1783). While a connect runs a getter reads `NotConnected` (`check_connected()`: `published_connected_`
  unless the thread holds `io_mutex_`), and a disconnect clears `connected_` before the close. The stops
  (AbortSlew, `MoveAxis(axis, 0)`, Park/FindHome's pre-stops, a refused re-send, the #521 relink) all go
  through `send_stops_locked()`. A getter changes state between a command's exchanges only from what it
  already holds (the `Slewing` read that finds a MoveAxis ended in the frame). A new command takes a
  default `StateLock` and calls `wire()`, never the held link directly, for an exchange; a new
  getter takes `Mode::State`, and so does a setter that sends nothing (`TargetRightAscension`,
  `TargetDeclination`, `DoesRefraction`): a target written while a slew or sync runs, its checks' reads
  included, stands (each entry point notes the target writes before any check, and the command sets only
  a target nobody wrote since). Tests: `test_onstep_getter_locks.cpp`, and the Park/FindHome storm in
  `test_onstep_concurrency_stress.cpp`.
- **`Park()`/`FindHome()` must be fire-and-forget, not blocking**: send the command and return
  without waiting for the park or home to finish (a running motion is stopped first, #1430, below;
  optimistically marking `is_parked=false`/`is_at_home=false` and `is_slewing=true`,
  letting the next status poll pick up real completion), matching iOptron's pattern. A first
  version blocked synchronously until the mount physically finished (`wait_for_condition_locked`)
  — functionally correct, but ConformU classifies `Park`/`FindHome` under the strict STANDARD
  (1.0s) timing target when the call itself blocks for the whole operation, vs. the lenient
  EXTENDED (600s) target when the call returns fast and the *client* polls `AtPark`/`AtHome`
  afterward (confirmed by comparing timing-summary entries against iOptron's own passing
  ConformU log, where `Park`/`FindHome` also take ~30s in wall-clock terms but show `0.001s ✓
  (EXTENDED)` in the timing table). Real GEM mounts physically cannot park/home in 1 second;
  fire-and-forget is the only way to pass this target with genuine hardware.
- **Park and FindHome stop any motion first and read the answer** (#1430): OnStepX refuses a park
  or a home while a guide move (a `MoveAxis`, or its braking), a pulse guide, a GOTO or a home runs
  (`Park::request`, `Home::request`: `CE_SLEW_IN_MOTION`), and the driver used to send `:hP#` and
  `:hC#` blind, so a `Park` right after `MoveAxis(axis, 0)` returned success and never parked
  (HM-17, 2026-10-20). Both stay asynchronous initiators (the fire-and-forget rule above, the
  contract sweep's 1 s bound):
  - **On a still mount** the call sends `:hP#` (reads its bare `1`/`0`) or `:hC#` (no reply), then
    `:GE#` right after a refused `:hP#` and after every `:hC#` (any later command replaces the code;
    checked on the HM-17: `22` during braking, `25` after an accepted park, `00` after an accepted
    home), and returns. A park answered `9` (already parked) is ASCOM's no-op and a home answered
    `18` (mount parked) is `InvalidWhileParked`, as when the call's `:GU#` read shows the mount parked:
    OnStep 4.x gives them (`Park.ino` `park()`, `Home.ino` `goHome()`) when it parked after that read;
    OnStepX takes a park of a parked mount as done (`Park::request()`); a refusal a retry cannot change (no park position set, outside limits)
    throws `InvalidOperation` with the reason from the call (HM-17: 0.15 s).
  - **When the mount moves** (the driver's `MoveAxis` flags, or `:GU#` showing `g`, `G`, a GOTO or
    `h`) the call sends AbortSlew's stops and returns (a stop that cannot be sent throws
    `DriverException` and ends the `MoveAxis` re-send, as `AbortSlew` does). An `AsyncOperation`
    (`park_home_`) then polls `:GU#` every 100 ms until the mount is still (at most 6 s) and sends
    the park or home. `Slewing` reads true meanwhile.
  - **Code 22 is retried** every 200 ms for 3 s in that task, also when the first try on a still mount
    met it. A refusal the task meets, or a park or home the mount takes and then stops, abandons or does not finish
    within its bound (#2296, the stop bullet above), is kept and raised by `Slewing`, and by `AtPark` (a park's) or
    `AtHome` (a home's), until the next motion command (`MoveAxis`, `AbortSlew`, a GOTO, `Unpark`,
    `Park`, `FindHome`; a `Park` or `FindHome` that is a no-op because the mount is already parked,
    parking, home or homing clears it too) or a reconnect clears it; those also end a task still running, without
    sending anything (the shared rule "A failed async operation body keeps its error"). A GOTO
    does so only when it is sent: a slew is refused while `:GU#` shows a GOTO (#1379), which covers
    the braking after the stops of a GOTO the park interrupted and the park itself once it moves,
    so such a slew leaves the task and its kept refusal as they were. A
    `PulseGuide` does not. The call claims the task under `mutex_` before it releases it
    (`park_home_claim_`, decision 0006 rule 3), so a newer command that lands before the task starts
    still ends it, and `Slewing` reads true from the call until the task ends. The task runs the
    request current when it starts, and a task already stopped when it starts takes none, so a
    newer Park or FindHome is never cleared by an older task that started late.
  - **`:GE#` details:** OnStep 4.x has `:GE#` too, per channel, with the same codes up to 23
    (OnStep `Globals.h` `CommandErrors`; no 25), and its `:hC#` stops the mount when it refuses for
    motion. A reply that is not a two-digit code (firmware without `:GE#`, 1 s timeout) leaves the
    reason unknown: a refused park is retried, then reported as "no reason given", and a home
    counts as started, as before. A `:hP#` answered `0` or garbled is taken as accepted when `:GE#`
    then says 0 or 25. The code is kept per serial channel; a Smart Web Server page that shares
    the channel can replace it between `:hP#` and `:GE#` (rare). A link lost between `:hC#` and
    `:GE#` fails the read (`get_last_command_error()` rethrows). A `:GE#` with no code is followed
    by a `:GU#` read (#1414): firmware without `:GE#` answers it and the park or home goes on as
    above; a mount that stops answering on a link that stays up does not, so the read counts toward
    the link fault and `Park`/`FindHome` throw `DriverException` instead of reading as started.
    OnStep 4.x `:GU#` (`Command.ino`) has `I` (parking) but no `h`, so a `FindHome` there is never
    taken as one already running.
  - **Locks and clock:** the task waits on the driver's task clock (decision 0005) with both driver
    locks released, so getters, `AbortSlew` and Disconnect answer meanwhile. The stops, the `:GU#`
    polls, `:hP#` and `:GE#` (up to 1 s each without a reply; on firmware without `:GE#` a `FindHome`
    takes about 1 s longer) and `:hC#` hold the operation lock, so nothing lands between `:hP#` and
    its `:GE#`, and run with `mutex_` free (decision 0016, #1449; see "Locks" above). A `Park` while one
    runs (`I`), or a `FindHome` while one runs (`h`), is a no-op.
- **The first post-Connect `DeviceState` reads no serial port** (#1242, #1783). The connect seeds
  the frame (`:GU#`, `:GRa#`/`:GDe#`, the site when the config did not set it, `:GA#`/`:GZ#`), and
  from then on the getters and `DeviceState` answer from memory, so a `DeviceState` never waits on the
  mount, whether ConformU's preamble reconnected or another client (the web UI, a script, NINA) kept
  the device open. Before #1783 the second case refreshed the five reads, ~0.11 s on the HM-17 (~0.65 s
  while each reply character cost 10 ms, fixed in #1242).
- **Other slow first FAST reads in old ConformU 4.5.0 runs were not a cold-start window
  either.** Two runs showed `AlignmentMode` (~0.135s), `Declination` (~0.318s),
  `EquatorialSystem` (~0.136s) and `SideOfPier Read` (~0.140s) outside the 0.1s FAST target.
  `AlignmentMode` (driver state) and `EquatorialSystem` (a constant) do no serial I/O, so that
  time is spent in ConformU, not the driver; these two and `SideOfPier` are the first reads of
  an enum type, which matches ConformU's first-use JIT cost on enum-typed members
  (`.github/instructions/ioptron.instructions.md`; worse in the arm64 4.5.0 build, see "ConformU version" in `.github/instructions/testing.instructions.md`).
  `Declination` was then a live position refresh, a `:GRa#`/`:GDe#` pair, and its ~0.318s is consistent
  with ~0.16s each, what that pair measured on the HM-17 while the per-character sleep fixed in
  #1242 was in place. The HM-17 runs on ConformU 4.5.2-rc.1 and 4.5.3, with nothing else connected, have
  every member within its timing target.
- **`PulseGuide` residual ConformU issues: North/South at HA ±9 is refraction, the rest is
  noise.** The `-9.0`/`-3.0`/`+3.0`/`+9.0` in ConformU's PulseGuide member names are the **hour
  angles** of the test position (`TestPulseGuide(ha)` in `TelescopeTester.cs`), not guide rates.
  - *North/South at HA ±9* (`East-West movement was outside test tolerance` on a North or South
    pulse, RA change 0.07–0.12 s): ConformU picks the test Dec with the highest elevation under
    65° on a 10° grid, which from latitude +40.5° (the HM-17 site) is Dec +85 at HA ±9 and Dec +55
    at HA ±3; other latitudes give other positions. OnStepX removes refraction from the RA/Dec it
    reports (topocentric coordinates, `:GXEE#` = `1`), and at that position a 37.6″
    Dec pulse shifts the reported RA by ~0.08 s, while ConformU's RA tolerance is 0.07 s (0.09″ on
    the sky at Dec +85) and does not allow for refraction. Proven on the TeSeek HM-17 (2026-10-16)
    by repeating the pulses outside ConformU at three firmware pressures (`:SX9B,n#`, a diagnostic
    only): the RA change matched OnStepX's own refraction formula (measured = 1.00 × predicted),
    fell to the noise level at 100 hPa, and the raw RA axis angle (`:GX42#`) moved only at the
    sidereal rate. The sign flips between North and South and between HA −9 and +9; HA ±3 is
    clean. Upstream: ASCOMInitiative/ConformU#35 (allow for refraction when `DoesRefraction` is
    true, #1216). The maintainer fixed it in ConformU 4.5.2-rc.1 (build 56111) by keeping the test
    declinations within ±65°. A run with an older build still shows these 4 issues; note them in
    the saved report instead of changing the tolerance.
  - *Other residuals* (East/West at the 0.07 s boundary): treated as hardware noise after (1) an
    A/B test with refraction rate compensation (`:Tr#`/`:Tn#`) on vs. off, no correlation; (2) mount
    unloaded vs. loaded, no difference; (3) testing far from the pole was blocked by ConformU's own
    `Park()`/`FindHome()` returning near the pole. The earlier reading that all four labels drive
    one repeated experiment was wrong (the labels are hour angles, see above).
  - Do not "fix" either case by loosening tolerances or by re-adding rate-compensation toggling.
- **The driver is built for equatorial mounts** (a fork reports `Polar`, a German mount `GermanPolar`, and
  an alt-az mount `AltAz` from the `:GU#` type letter, #2214; only the German mount is validated on a rig),
  like every other telescope driver in this project (iOptron, SynScan, Celestron). `SlewToAltAzAsync`
  is supported via a local Alt/Az→RA/Dec coordinate transform followed
  by an ordinary GOTO, refused while Tracking is true (#1377) rather than needing it (#1274) (the router refuses synchronous `SlewToAltAz` with
  `MethodNotImplemented`, #775) — this works regardless of the physical mount's mechanical
  alignment, so it does not require native AltAz mount support. `SyncToAltAz` is implemented the same way
  (`CanSyncAltAz=true`, #2214): the same Alt/Az to RA/Dec conversion, then the `SyncToCoordinates` path
  (`:Sr#`/`:Sd#`/`:CM#`, the refusal handling and the write-through; OnStep has no native Alt/Az sync command). Its
  order is `SlewToAltAz`'s: the arguments (`InvalidValue`, also while disconnected), connected, parked, a latched
  fault, the site, then a GOTO running (`InvalidOperation`). It does not refuse while Tracking is true: ITelescopeV4
  only says it "may", and plate-solve syncs run on a tracking mount.
- **Rates** (#2214). All of it is driver state under `mutex_`, filled by the connect and the poll (never by a getter), so
  no rate getter reaches the wire; a stale frame keeps the last letter seen (decision 0010 rule 6, stale never refuses) and a
  latched fault throws (decision 0009 rule 1):
  - **`TrackingRate` / `TrackingRates`**: OnStepX offers sidereal, lunar, solar and King (0-3); OnStep 4.x, and a
    driver that is not connected, `{0}`. The getter serves driver state (`tracking_rate_known_`), a mount setting kept
    current by every `:GU#` status the driver sees from the rate letter (none is sidereal) while tracking
    compensation is off. The letters are not written with compensation on (`r`/`t` in the frame, see the
    status table), so then the last known rate answers: the driver's own writes, the letter while it last
    showed one, and `:GT#` (sidereal until one of those answers; read at connect, and on the slow tick only while compensation is on and the mount
    tracks; `0`, or a rate not within 0.01 Hz of one of the four, keeps the last one). On OnStepX, with no `:GU#` answer
    since the connect, `TrackingRate` (and the two offset getters built on it) throws `DriverException` as `Tracking` does
    (decision 0010 rule 8), never sidereal. Known limitation: with compensation on, after a home reset (it sets the rate
    back to the default) or a connect to a compensated mount that is not tracking, the driver serves its last known or
    assumed rate until the mount tracks and a slow tick reads `:GT#` (it answers `0` while not tracking). OnStep 4.x always
    reads sidereal. The
    setter is `InvalidValue` for a value outside 0-3 (before the connection check), then connected, a latched fault,
    and `InvalidValue` for a rate the firmware does not offer (1-3 on 4.x; sidereal is accepted and sends nothing there).
    On OnStepX it sends `:TQ#`/`:TL#`/`:TS#`/`:TK#`, writes the rate through (the next read sees it before the next
    poll) and reads the offsets again. It does not change Tracking. A change of rate while either stored offset is
    non-zero first sends `:SXTR,0.000000#` and `:SXTD,0.000000#` (OnStepX keeps applying the offsets on a lunar, solar or
    King rate while the getters read 0 and writes are refused; as the Sky-Watcher driver, ITelescopeV4 does not require
    it); a refusal throws with the `:GE#` reason.
  - **`RightAscensionRate` / `DeclinationRate`** (OnStepX; `CanSet*` false on 4.x and while disconnected, where the
    setters throw `PropertyNotImplemented` after the connection check): the firmware's offsets in arcseconds per
    sidereal second, read at connect, after each write, after a `TrackingRate` write, and by the poll when the frame's
    rate, compensation, tracking, parked or at-home flags differ from the previous frame's (OnStepX zeroes both on every
    home reset: a find home, the boot, the unpark path; `Home.cpp` `reset()`). A read that does not parse keeps the
    previous values and does not fail the tick. The getters divide back (RA by 15, Dec by 0.9972695663) and return
    0 unless `TrackingRate` (as served) is sidereal (ITelescopeV4). A write is `InvalidValue` when the value in the
    firmware's units is beyond +/-1800 (the firmware clamps there silently), before the connection check, then
    connected, a latched fault, `InvalidOperation` when `TrackingRate` is not sidereal, then `:SXTR,n.n#`/`:SXTD,n.n#`
    (six decimals) with the reply read: `0` throws `InvalidOperation` with the `:GE#` reason (#1379).
  - **`RightAscension` / `Declination` while an offset runs** (#2241, decision 0010 OnStep row). The frame is up to
    one poll period old, so at 40 arcsec/s it lagged the mount by up to 40 arcsec, and at 0.05 arcsec/s the HM-17's
    0.5 arcsec steps (`:GXA1#`/`:GXA2#` 7182.222 steps per degree) land about every 10 s, so a 10 s window saw 0 or
    0.5 arcsec by luck: ConformU's extended `RightAscensionRate`/`DeclinationRate` tests, which read the position twice
    10 s apart, reported ISSUEs. The getters, and `DeviceState` with them, now serve a commanded model while an offset
    runs: an anchor (position, task-clock time) plus the offsets times the sidereal seconds since it
    (`offset_model_position()`: SI seconds / 0.9972695663; RA offsets in RA-axis arcseconds, 54000 per hour), RA
    wrapped into [0, 24) and Dec clamped to +/-90. No traffic: the model is driver state under `mutex_` built from
    the snapshot and the stored offsets. It runs only on OnStepX, with a non-zero offset at `TrackingRate` sidereal,
    while the frame says tracking and not slewing (`N` absent), pulse guiding (`G`), moving an axis (`g`), homing
    (`h`), parking (`I`) or parked (`P`), and no MoveAxis, slew grace, pending Park/FindHome or pulse of the driver's
    own runs (`offset_model_allowed_locked()`); otherwise the frame is served as read. Each published frame is the
    guard (`adopt_offset_model_frame_locked()`): the first frame in such a state anchors the model (not one sampled
    before a pulse's end plus 50 ms), one within 2 arcsec of the model at its `:GRa#`/`:GDe#` read
    (`PolledFrame::position_at`) on both axes keeps the anchor, one beyond either re-anchors on the measurement, and
    one 30 minutes (`kOffsetModelHold`) or more after the anchor re-anchors on it anyway. A stalled axis therefore
    shows within one poll at 40 arcsec/s and within about 40 s at 0.05 arcsec/s. 2 arcsec is four HM-17 steps; a
    mount with larger steps needs at least four of its own (reading `:GXA1#`/`:GXA2#` for it is a possible
    follow-up). Before any stored offset changes, the model re-anchors under the old offsets
    (`reanchor_offset_model_locked()`): a rate write before its send, each of the `TrackingRate` change's zeroing
    writes, and an offset change the poll reads (`set_rate_offsets_locked()`); with both offsets zero the last anchor
    holds until a frame measured after the change publishes, so the position does not jump back to an older frame.
    A sync anchors at the synced position; a GOTO, park, home, MoveAxis start, a MoveAxis stop that ends a move or a
    pulse, a pulse guide, the stops and a `Tracking` write drop the anchor. A frame whose `:GRa#`/`:GDe#` read is
    older than the model's last driver-side change (a re-anchor, a sync or a drop, `offset_model_floor_`) changes
    nothing: a tick that sampled before the change and publishes after it (the poll publishes under `mutex_` alone,
    so it can land inside a command's exchange) neither ends that hold nor anchors the model.
    Extrapolation stops `kSnapshotMaxAge` (3 s) after the last publish: the stale frame is served as it is (decision
    0010 rule 6), never refused. `DeviceState`'s `TimeStamp` stays the frame's `measured_at`. An offset changed at
    the handset while the poll sees no flag change is not read again; the guard re-anchors on the measurement each
    time the model drifts 2 arcsec from it. Tests: the `[offset-model]` cases in `test_onstep_telescope_snapshot.cpp`,
    whose fake moves `:GRa#`/`:GDe#` with the virtual clock in 0.5 arcsec steps at the `:SXTR`/`:SXTD` offsets
    (`SnapshotMount::offsets_move`). Fake evidence only; the bare-mount ConformU run on the HM-17 is still owed.
  - **`GuideRate`** is the mount's pulse-guide rate: `:GX90#` (a multiple of sidereal) times 15.0410686 arcseconds per
    second, in degrees per second, on both axes. It is read at connect (any firmware) and again when the pulse-guide
    digit of `:GU#` changes. A mount that does not answer it, or answers something that is not a positive decimal,
    keeps the informational half-sidereal value. `CanSetGuideRates` stays `false`: `:R0#`-`:R2#` write the controller's
    non-volatile memory and take effect only in builds with `GUIDE_SEPARATE_PULSE_RATE`, so a client write could
    persist on the mount and not be detectable.
  - Test seam: none beyond the poll's (`set_status_poll_paused_for_testing()`, `poll_status_now_for_testing()`); the
    cases are in `test_onstep_telescope_snapshot.cpp` (`[rates]`).
- `CanSetPierSide` is true only on an OnStepX
  German mount with the flips on (#2179, the flip bullet above); the flip is `:MNe#`/`:MNw#`, a GOTO to the
  current position on the other side.
- **`UTCDate` is the mount's clock or a client's write, never the host's** (#1478): the connect
  reads the mount clock once (`:GL#`/`:GC#`/`:GG#`, `seed_utc_from_mount_locked()`, after any
  `syncTimeOnConnect` write, which makes the read unneeded), so `UTCDate`, and so `DeviceState`,
  performs no serial exchange of its own, and takes only the state lock, so it never queues behind
  another member's exchange (#1449). `:GG#` is the hours to add to local time to get UTC (OnStepX
  `Site.command.cpp`), so UTC is local time plus the offset (#1626; it read minus, which only shows
  on a mount whose offset was set elsewhere, since AlpacaBridge writes 0). A date or offset reply
  that does not parse fails the read instead of storing 2000-01-01 or offset 0 (#1626), as does a
  local time outside 0-24 h or a month or day outside 1-12 and 1-31 (checked before the calendar
  check, since `std::chrono::month` and `day` keep only 0-255). If that read fails, `UTCDate` throws `InvalidOperation` with
  the reason, with no further read, until a `UTCDate` write or the next connect. The LST
  computations still fall back to the host clock, without storing it. The `syncTimeOnConnect`
  write (off by default) goes out only while `util::HostClockTrust::trusted()`; otherwise one WARN,
  and the site writes and the seed are unchanged. `on_host_clock_changed()` re-syncs on the
  `clock_resync_` slot: the body sends only `:SL#`/`:SC#`/`:SG#`, never the site, and checks motion
  inside the write's own exchange under `io_mutex_` (ascom-contract host clock rule). Tests:
  `test_onstep_clock_trust.cpp`. `SiteElevation` throws
  `InvalidOperation` until the device config or a client sets it: the protocol has no elevation
  command, so it is client-side only. `SiteLatitude` and `SiteLongitude` throw `InvalidOperation`
  (with the reason the last `:Gt#`/`:Gg#` read failed) until the coordinate is known: written from
  the device config at connect, by a client, or read from the mount at connect or by the status poll
  (#1783; they answered a default 0.0 before, decision 0019). `DestinationSideOfPier`, the `SideOfPier` write,
  `SiderealTime`, both `SlewToAltAz` forms, and `SideOfPier` when the mount reports no side (`o`; a `T`/`W`
  letter needs no site, #2179) throw `InvalidOperation` too ("<member> needs the site", with the
  same reason) until both coordinates are known (#1893; they used latitude and longitude 0 before): decision 0019
  rule 2, followed for consistency, since its rule 3 covers controllers that store no site. An Alt/Az slew retries
  the site read first (at most every `kSiteInfoRetryDelay`) and sends nothing when it still fails. With a known
  site, the normal case and ConformU's, nothing changes.
- **No `gps_status` action: OnStep cannot report a GPS fix** (#1653). iOptron and Celestron answer
  `gps_status` (#1574) so a client lets the mount's site win only on a fix; OnStep has no reply that
  says the site or time came from GPS. OnStepX (`097cc579`, `Site.cpp` `gpsCheck()`) puts a GPS
  site in RAM only, named "GPS"; `:GM#`-`:GP#` read the saved site names, so they never show it,
  and `:GX89#` says only that the date and time are set, from any source. OnStep 4.x
  (`release-4.24`, `3b73bffcfb`, `OnStep.ino`) saves a GPS site as site 1 under the name "GPS",
  so `:GM#` reads "GPS" from then on, across power cycles and whatever the GPS does later, and a
  user can give a site that name too: it says the site was set from GPS once, not that there is a
  fix. A client keeps its own site for OnStep.
- **`DoesRefraction` comes from the firmware, not a local flag** (#1216): OnStepX applies
  refraction to every RA/Dec it reports unless it was built for observed-place coordinates, and
  `:GXEE#` says which: it replies `MOUNT_COORDS - 1` as one bare digit with no `#` (`0` observed
  place, `1` topocentric, `2` topocentric strict). The driver asks at connect, on firmware 10 or
  later only, because legacy OnStep answers any unknown command with `0` too. It reports `True`
  for `1`/`2`, and a write of the other value throws `PropertyNotImplemented`, since the mode is
  fixed when the firmware is built. Legacy firmware, or no answer, keeps the settable local flag.
  The TeSeek HM-17 (OnStepX 10.23a) answers `1`.
- **ConformU-validated on real hardware**: the TeSeek HM-17 (OnStepX 10.23a) on Linux arm64,
  bare mount, ConformU 4.5.3 (build 56334), 2026-10-25: 0 errors, 0 warnings, 0 issues, all members
  within their timing targets (`AlpacaCore/conformu/OnStep/TeSeek HM-17/Linux-arm64.txt`; its
  earlier 4.5.2-rc.1 run was clean too).
  Older ConformU 4.5.0 runs had slow first FAST reads (see the notes above; one came from
  `get_side_of_pier()`'s live position round trip, since fixed), which is why they are documented.
  The HM-17 report is the OnStep ConformU validation.
  See `AlpacaCore/conformu/OnStep/TeSeek HM-17/Linux-arm64.txt`
  and `SUPPORTED-DRIVERS.md`.
- **`findhomeissensorsearch` action**: false (`goto_stored_home`); optional OnStepX home sensors are a build option the driver cannot read. Rule in [ascom-contract](ascom-contract.instructions.md#telescope-findhomeissensorsearch-action-every-telescope-driver).
