---
applyTo: "AlpacaCore/src/vendors/onstep/**,AlpacaCore/include/alpacacore/vendor/onstep/**,AlpacaCore/tests/*onstep*,AlpacaCore/conformu/OnStep/**"
---

### OnStep

Devices: Telescope.

No vendor SDK — OnStep is open-source LX200-protocol firmware (Arduino Mega/Due, Teensy,
ESP32, STM32) for DIY/retrofit mounts. The protocol reference is the firmware's own OnStepX
command reference, kept unmodified in [`AlpacaCore/external/OnStep/`](../../AlpacaCore/external/OnStep/README.md)
(GPL-3.0, with its license; upstream
[at the same commit](https://github.com/hjd1964/OnStepX/blob/097cc5797b4708e3787dd0b8bbbf81df66fdb5ff/docs/COMMAND_REFERENCE.md),
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
| `:GXEE#` | coordinate mode (`DoesRefraction`, #1216) | `0#`, `1#`, ... | read without waiting for `#` |
| `:GU#` | status (tracking, slewing, park, home, guide, pier side) | packed status `#` | `#`-terminated; see the status table below |
| `:GRa#` / `:GDe#` | RA / Dec | `HH:MM:SS.SSSS#` / `sDD*MM:SS.SSS#` | `#`-terminated. Not in the reference: aliases of `:GRH#`/`:GDH#` (OnStepX `Mount.command.cpp`, the `'a'`/`'e'` parameter) |
| `:GA#` / `:GZ#` | altitude / azimuth | `sDD*MM#` or `sDD*MM'SS#` / `DDD*MM#` or `DDD*MM'SS#` | `#`-terminated |
| `:Gh#` / `:Go#` | horizon / overhead limit (MoveAxis gate, #1378) | `sDD*#` / `DD*#` | `#`-terminated |
| `:GS#` | sidereal time | `HH:MM:SS#` | `#`-terminated |
| `:Gt#` / `:Gg#` | latitude / longitude (west-positive on the wire) | `sDD*MM#` / `sDDD*MM#` | `#`-terminated |
| `:GL#` / `:GC#` / `:GG#` | local time / date / UTC offset (`UTCDate` read) | `HH:MM:SS#` / `MM/DD/YY#` / `sHH:MM#` | `#`-terminated |
| `:GE#` | last command error (after a refused `:hP#`/`:hC#`, #1430, or `:ST`/`:hQ#`/`:hR#`, #1379) | 2-digit code `#` | `#`-terminated; codes below |
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
| `:ST60.164275#` / `:ST0#` | tracking on / off | `0`/`1` | bare digit for the client's `Tracking` write (`0` throws with the `:GE#` reason, #1379); blind for the driver's own tracking restores |
| `:hP#` | park | `0`/`1` | bare digit; then `:GE#` for the reason |
| `:hC#` | find home | none | blind; then `:GE#` for the reason |
| `:hR#` | unpark | `0`/`1` | bare digit; `0` throws with the `:GE#` reason, except `11` (not parked), ASCOM's no-op |
| `:hQ#` | set park position | `0`/`1` | bare digit; `0` throws with the `:GE#` reason |

`:MS#` refusal digits (OnStepX `Goto.command.cpp`, OnStep `Command.ino`; the same digits follow
`E` in a `:CM#` failure): `1` below the horizon limit, `2` above the overhead limit, `3` controller
in standby, `4` parked, `5` GOTO in progress (OnStepX also stops the running GOTO), `6` outside
limits, `7` hardware fault, `8` already in motion, `9` unspecified.

`:GE#` codes the driver acts on (the full list is in the command reference): `00` none and `25`
true are success; `22` in motion makes Park/FindHome wait for the mount to be still and retry for
3 s; any other code is a refusal the driver keeps and reports with its number (#1430). The `:MS#`
digits are these codes 15 to 23 less 14.

#### OnStep 4.x and OnStepX

OnStep (the older firmware, 4.x, last release branch `release-4.24`) and OnStepX (10.x, a
rewrite) speak the same LX200 dialect for every command in the table, with the differences below
(read from OnStep `Command.ino`/`Goto.ino` at `3b73bffcfb` and the OnStepX sources). Only OnStepX
is tested: the TeSeek HM-17 runs OnStepX 10.23a, and no OnStep 4.x mount has been run, so the 4.x
column is from the source, not from hardware.

- **Telling them apart**: OnStepX numbers its releases from 10 (`:GVN#` `10.23a`), OnStep 4.x and
  older below that (`is_onstepx_version()`, version 10 or later). The driver uses this only to
  decide whether to ask `:GXEE#`.
- **`:GXEE#`** (coordinate mode, `DoesRefraction`, #1216): OnStepX only. OnStep 4.x answers any
  unknown command with `0`, so the driver does not ask it there and keeps the settable local flag.
- **`:GE#`**: both, with the same codes up to 23; OnStep 4.x has no `25` (`CE_1`) and keeps the
  code per serial channel (see the Park and FindHome bullet).
- **`:GU#`**: the letters the driver reads are the same, except that OnStep 4.x has no `h`
  (homing), so a `FindHome` there is never taken as one already running. OnStep 4.x adds letters
  the driver does not read (`R`, `S`, `r`, `s`, `t`, `w`, `u`, `z`, `a` and a PEC character).
  Both send `g` while a `:Mn#`-style move runs and `G` while a pulse guide runs.
- **`:GG#`** (UTC offset): OnStep 4.x answers whole hours (`sHH#`), OnStepX `sHH:MM#`; the
  sexagesimal parser reads both.
- **`:GRa#` / `:GDe#`**: both take `a`/`e` as the high-precision form, like `H`.
- **`:ST`** (tracking rate): both reply `0`/`1`; OnStep 4.x refuses it while slewing
  (`CE_MOUNT_IN_MOTION`); OnStepX does not start tracking while parked (no error, `Mount.command.cpp`).
  The client's `Tracking` write reads the reply, so a 4.x refusal during a GOTO throws (#1379).
- **A GOTO during a GOTO** (#1379): both stop the running one and refuse the new one (OnStepX
  `Goto.cpp` `CE_SLEW_IN_SLEW`; OnStep 4.x `Goto.ino` `CE_GOTO_ERR_GOTO`, unless built with
  `CE_GOTO_ERR_GOTO_OFF`), with `:MS#` reply `5`.
- **The guide time limit that ends a `MoveAxis`** (`GUIDE_TIME_LIMIT`, #1378) is a build setting
  in both: 1-120 s or `0` for none (OnStep 4.x `Config.h` ships `0`; OnStepX defaults to 10 s, which
  the HM-17 runs). The re-send keeps a move going under a limit and changes nothing without one.

Connection types: **Serial (USB) only** in this project — no Wi-Fi/network config is exposed to
users, unlike iOptron/Celestron. Default 9600 baud, 8N1, standard LX200 `#`-terminated ASCII
commands. `onstep_protocol_wrapper`'s `ConnectionType::Network` branch still exists internally
(mirrors the SynScan wrapper) purely as a test seam so the mandatory `[stress]` concurrency test
can drive the driver to CONNECTED against a loopback `FakeMountServer` without real hardware —
it is never reachable through `router.cpp` or the web UI.

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
  duration, not polled from the mount, and ends early on a stop that ends the pulse: `AbortSlew`
  and the same stops a `Park` or `FindHome` sends before a moving mount parks or homes (`:Q#` ends
  every guide, OnStepX `Guide::stop()`), and a `MoveAxis(axis, 0)` on the pulse's axis
  (`:Qe#`/`:Qw#` or `:Qn#`/`:Qs#` end that axis's guides). Past its end time a pulse still reads
  as running until the status poll publishes a frame sampled `kPulseGuideSettle` (50 ms) or more after
  that end, at most `kPulseMeasureBound` (2 s) longer (#1783): a `PulseGuide` starts one extra tick for
  then, in a slot of its own (`pulse_measurement_`; the 1 s ticks run on through the pulse), so a position read right after `IsPulseGuiding` goes false is not one
  measured mid-pulse (ConformU's PulseGuide checks compare the RA and Dec around it). Unlike SynScan (no native pulse guide), this driver does
  **not** need a background stop-thread. A `Duration` over 9999 ms does not fit the 4-digit field
  and is refused with `InvalidValue` rather than clamped (#1478). Direction and Duration are
  checked before the connected, link-fault and parked checks, as are `SyncToCoordinates`' and
  `DestinationSideOfPier`'s RA and Dec.
- **SideOfPier must be computed from hour angle, not read from `:GU#`'s pier-side letter** — the same
  lesson iOptron already learned from its `:GEP#` raw pier value (see the [iOptron notes](ioptron.instructions.md)):
  a mount's raw physical-pier-side report does not match the ASCOM convention once tracking past
  the meridian. An earlier version of this driver preferred the `:GU#` flag over the hour-angle
  computation and failed ConformU's SideofPier check on real hardware: `Reported SideofPier at
  HA -9, +9: EE` instead of the required `WE` (flips with HA sign — ASCOM's pointing-state
  contract). That `EE` was not the physical pier side: the driver read `E`, which OnStepX writes
  for the mount type (GEM) on every reply, as "pier east" (#1217). The pier side is `T`/`W`/`o`
  (table below). `get_side_of_pier()` now always computes from hour angle (`LST − synced RA`);
  `:GU#`'s pier side is parsed into `MountStatus` but intentionally unused for this property. Do not
  reintroduce a firmware-flag-preferred branch here without re-validating against ConformU's
  SideofPier/DestinationSideofPier checks. `get_side_of_pier()` takes the RA from the polled frame
  (below) and never reads the mount: a live `:GRa#`/`:GDe#` pair there (~25 ms each on the HM-17 at
  9600 baud; ~0.16s each while `read_response()` slept 10 ms after every reply character, #1242)
  cost ConformU's 0.1 s FAST target, and the hour-angle *sign* tolerates the frame's second of RA
  staleness.
- **`:GU#` status-character table**, confirmed against real hardware traces (`nNpHrsEo180` etc.
  observed live): `n`/`N` = **NOT** tracking / **NOT** slewing respectively (`== npos` check, i.e.
  the character's *absence* means the state is active — this is the opposite polarity of what
  the INDI driver's comments implied, and was the single biggest early bug in this driver).
  `H` = at home, `P`/`p` = parked/not parked, `I` = parking in progress, `F` = park failed,
  `h` = homing, `g` = a guide move is running (`guide.active()`: a `:Mn#/:Ms#/:Me#/:Mw#` move, so a `MoveAxis`, and also a home or spiral guide; not a pulse guide),
  `G` = a pulse guide is running. `N` covers a GOTO only: a `MoveAxis` never clears it. The mount type follows (`E` GEM, `K` fork, `A` alt-az, `L` alt-alt), then the
  pier side: `T` east, `W` west, `o` none (OnStepX `Status.command.cpp`; HM-17 traces `nNpEW180`
  = pier west, `nNpET1:0` = pier east, `nNPHEo180` = parked at home). `e` means "sync to
  encoders only": unpark, home and sync set it and the next GOTO clears it, and it matters only
  with external encoders. The last three characters are `'0' + n` for the pulse-guide rate, the
  guide rate and the general error code (`:` is 10). Error `7` is `ERR_MERIDIAN`: after it,
  OnStepX acknowledges GOTO, park and home commands but does not move, and a manual move back
  inside the limit clears it (the HM-17's limits are ±60 min, `:GXE9#`/`:GXEA#`). Any other
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
  running (both tested over a dropped TCP link, which fails the write while the wrapper still reads
  the link as alive, #1459). If the slot refuses the new body after the move went out (no thread,
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
    and the age of its frame, the slew grace, the pulse-guide end, the synchronous-slew poll and settle
    sleeps, the elapsed time since a UTCDate write (a `util::UtcOffset`, decision 0014, cleared on
    connect and on disconnect), and the #521 relink outage:
    the driver hands its clock to the process-wide wrapper at connect
    (`OnStepProtocolWrapper::set_task_clock()`, put back to the default by `release_task_clock()`
    when the driver goes), so the wrapper's loss and last-alive stamps and the outage share one
    clock. A change of clock forgets those stamps: one taken on another driver's clock is not timed
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
- **Tracking on/off**: confirmed on real hardware to be `:ST60.164275#` (start — sets tracking
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
  computing UTC via `std::gmtime()` and sending it as "local" time with `offset_hours=0.0`,
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
  ("read failed: <errno text>", below), a read that ends on silence checks the node, and over the
  TCP test seam a hangup throws `DriverException` "OnStep network link lost (hangup): <errno text>"
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
  `EIO`/`ENXIO`/`ENODEV`/`EBADF`. Lost means `Connected` false, `NotConnected` instead of a
  generic "Failed to send command to mount" while `Connected` still read true (a reply cut off
  mid-read no longer waits out the response timeout either), the fd closed (so the replugged
  CH340 can come back as `ttyUSB0`), and `get_link_fault()` naming the reason. A `Connected=true`
  then reconnects. OnStepX keeps running a GOTO while the cable is out, and a `MoveAxis` for up to
  about 10 s, after which this firmware ends it by itself (HM-17: 5 degrees at 0.5 deg/s in #1098,
  2.5 degrees at 0.25 deg/s in #1241; it moves from the home position too, HM-17 2026-10-19), so the reconnect after an outage of `util::kRelinkMotionPreserveWindow` (5 s) or more
  sends AbortSlew's stops (`:Q#`, `:Qn/s/e/w#`); a shorter outage, or a connect with no recorded
  loss, stops nothing (#521). A park still running is left to finish (#1289 item 5): the reconnect reads `:GU#` first and sends no stops while it shows `I` (parking), since `:Q#` would abort the park and leave the mount unparked where it stopped (OnStepX `Park::requestAborted()`); a status it cannot read stops anyway, after waiting out that read's reply timeout.
  They leave tracking as it was: `:Q#` stops a GOTO through `goTo.abort()` and the guide stop and
  never calls `mount.tracking(false)` (HM-17 2026-10-24: `:GU#` `pEW160` before, `NpEW160` after a
  `:Q#` sent mid-GOTO), so nothing has to restore tracking after them.
  The outage runs from the last moment the link was seen alive (connect, a live `link_alive()`,
  the check before each exchange) to the end of the reconnect, so the reconnect's own time counts,
  an auto-detect re-scan included. It is not timed from when the loss was noticed: the status poll
  checks the link once a second, but the router reads `Connected` before every Connect, so the
  first to notice can be the reconnect itself. Tracking is left alone. The TCP test seam has no node and is never
  lost. `link_alive()` runs without `mutex_`, so it records a loss only while the node it checked is
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
  `SiteLatitude`, `SiteLongitude`, `UTCDate`, `SiderealTime`, `DestinationSideOfPier`,
  `DoesRefraction`, `IsPulseGuiding`, `Slewing` (also while a `Park` or `FindHome` is pending or within
  the slew grace) and the rate getters (`DeclinationRate`, `RightAscensionRate`, `TrackingRate`, which
  read lock-free from the latched fault text), throw the fault too (decision 0009 rule 1, as in
  Sky-Watcher, open-astro#882). The client's own values answer: `TargetRightAscension`/
  `TargetDeclination`, `SlewSettleTime`, `SiteElevation` (the device config), `GuideRate`
  (informational) and the `Can*` constants. A getter asks the mount nothing while faulted, so a faulted
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
  `AbortSlew` ends the grace, so the next slew goes out once `:GU#` shows the GOTO ended. OnStepX keeps the
  GOTO state while it brakes after `:Q#` (HM-17 2026-10-24: about 1.6 s), and a slew in that time
  is refused, as the firmware would refuse it (reply `5`). A `:MS#` the mount refuses reports its reply digit and reason (the digits are
  listed under "Commands this driver sends"; 4 parked is `InvalidWhileParked`), not "below horizon
  or limit" for every refusal.
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
  (`check_alt_az_slew_locked()`). With Tracking off the driver converts to RA/Dec and runs the same
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
  of a connect (its first tick 1 s later: the connect seeds the frame) and joined before a disconnect or
  a relink closes the port, with no driver lock held. Every `kStatusPollPeriod` (1 s, start to start on
  the task clock, missed ticks skipped) it reads `:GU#` then `:GRa#`/`:GDe#`, and every
  `kSlowPollEvery`-th (2nd) tick `:GA#`/`:GZ#` and, while the site is not known, `:Gt#`/`:Gg#` (every
  `kSiteInfoRetryDelay` at most), into `util::StateSnapshot<PolledFrame>`. A `PulseGuide` adds one
  tick, in a slot of its own, that measures the pulse's end (see the pulse guiding bullet). The tick holds
  the operation lock for its exchanges, so it never lands inside a command's sequence, drops `mutex_`
  for each, and publishes under `mutex_` alone after `begin_poll()`. A tick ends at its first failed
  exchange of `:GU#` or the position, so a Disconnect waits for one reply timeout. Served from the
  frame, stale or not (`kSnapshotMaxAge` 3 s is the freshness bound; stale never refuses, decision 0010
  rule 6): `RightAscension`, `Declination`, `Altitude`, `Azimuth`, `Tracking`, `AtPark`, `AtHome`,
  `Slewing` (with the driver's own MoveAxis flags, slew grace and pending Park/FindHome), `SideOfPier`
  (from the frame's RA) and `DeviceState`, whose `TimeStamp` is the frame's `measured_at`
  (`device_state_age()`). A field the mount has not answered yet throws `DriverException` "not
  available"; a latched fault and a lost link refuse as above. Client commands write through
  (`snapshot_.write()`): a sync's position, `Tracking`, `Unpark`, a slew start (`Slewing` true), a park
  or home the mount took, and a pulse, a MoveAxis start and AbortSlew drop the tick in flight, so a tick
  that sampled before the command cannot publish over it. Control paths keep their direct reads and
  write them through (`refresh_status_locked()`, only a status that differs from the frame's, so a loop
  that re-reads `:GU#` does not drop every tick): the slews' and syncs' GOTO check (their Tracking
  check reads the frame, as the getter does), Park and FindHome and their
  body, the synchronous slew's wait (which publishes a full measurement where the slew ends; a failed one
  keeps the last values and counts toward the fault, as a failed tick), AbortSlew's
  tracking decision and the MoveAxis re-send tick. The tracking restore a coordinate slew owes (#1230)
  is sent by the tick that first reads the GOTO ended past the slew grace (not during a MoveAxis or
  while a Park or FindHome is pending), under the same operation lock as its `:GU#`. Test seams:
  `set_status_poll_paused_for_testing()`, `poll_status_now_for_testing()`,
  `set_status_poll_after_sample_hook_for_testing()`, `status_poll_running_for_testing()`,
  `pulse_measurement_running_for_testing()`, `set_pulse_guide_gap_hook_for_testing()`. Tests: `test_onstep_telescope_snapshot.cpp`.
- **Locks: getters never wait on the wire** ([decision 0016](../../docs/decisions/0016-getters-never-wait-on-device-io.md),
  #1449, the shape of Sky-Watcher's #1530). Order: `transition_mutex_` (`set_connected()` against
  `set_connected()`; no getter takes it) -> `io_mutex_` (the operation lock) -> `mutex_` (the state lock, a
  leaf) -> the protocol wrapper's own lock; the `StateSnapshot` and slot mutexes are leaves under `mutex_`.
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
  default `StateLock` and calls `wire()`, never `OnStepProtocolWrapper::instance()` for an exchange; a new
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
    met it. A refusal the task meets is kept and raised by `Slewing`, and by `AtPark` (a park's) or
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
  `AlignmentMode` and `EquatorialSystem` are compile-time constants with no serial I/O, so that
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
- **v1 scope is equatorial mounts only** (`AlignmentMode::GermanPolar`), matching every other
  telescope driver in this project (iOptron, SynScan, Celestron). `SlewToAltAzAsync`
  is supported via a local Alt/Az→RA/Dec coordinate transform followed
  by an ordinary GOTO, refused while Tracking is true (#1377) rather than needing it (#1274) (the router refuses synchronous `SlewToAltAz` with
  `MethodNotImplemented`, #775) — this works regardless of the physical mount's mechanical
  alignment, so it does not require native AltAz mount support. `SyncToAltAz` is not implemented
  (`CanSyncAltAz=false`) since OnStep has no native Alt/Az sync command.
- `CanSetGuideRates=false` (guide rate is not queryable/settable over this protocol — the
  `GuideRate` property returns an informational fixed value only) and `CanSetPierSide=false`
  (no distinct "set pier side" command). `CanSetDeclinationRate`/`CanSetRightAscensionRate` are
  also `false` — custom tracking-rate commands were not confirmed against real OnStep firmware
  during this implementation, so the setters throw `PropertyNotImplemented` rather than
  advertise support that might silently no-op. The getters read 0, and throw `NotConnected` while
  disconnected (read lock-free from `Connected`, #1478).
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
  computations still fall back to the host clock, without storing it. `SiteElevation` throws
  `InvalidOperation` until the device config or a client sets it: the protocol has no elevation
  command, so it is client-side only. `SiteLatitude` and `SiteLongitude` throw `InvalidOperation`
  (with the reason the last `:Gt#`/`:Gg#` read failed) until the coordinate is known: written from
  the device config at connect, by a client, or read from the mount at connect or by the status poll
  (#1783; they answered a default 0.0 before, decision 0019). `SiderealTime`, `SideOfPier`,
  `DestinationSideOfPier` and `SlewToAltAz` still use longitude 0 until it is known.
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
