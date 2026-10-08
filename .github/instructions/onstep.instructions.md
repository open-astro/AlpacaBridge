---
applyTo: "AlpacaCore/src/vendors/onstep/**,AlpacaCore/include/alpacacore/vendor/onstep/**,AlpacaCore/tests/*onstep*,AlpacaCore/conformu/OnStep/**"
---

### OnStep

Devices: Telescope.

No vendor SDK — OnStep is open-source LX200-protocol firmware (Arduino Mega/Due, Teensy,
ESP32, STM32) for DIY/retrofit mounts. No manufacturer protocol PDF exists; the reference for
this driver is INDI's `lx200_OnStep` driver (`indilib/indi`, `drivers/telescope/lx200_OnStep.cpp`
+ the shared `lx200telescope.cpp`/`lx200driver.cpp` base it inherits from) — there was no INDI
driver naming collision to resolve, "OnStep"/"On-Step" appears verbatim in both.

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
- **Identity probe**: `:GVP#` (get product name) must return `"OnStep"` or `"On-Step"`;
  cross-checked with `:GVN#`'s leading version digit to distinguish legacy OnStep from OnStepX
  per the INDI driver's own detection logic.
- **DTR/HUPCL Arduino-reset quirk**: like the Gemini focuser wrapper, CH340/CH341/native-USB-CDC
  adapters reset the MCU on a DTR edge at port open. The probe path (`probe_onstep_port()`)
  clears `HUPCL` before *and* after probing, and the production `connect_serial()` path also
  clears `HUPCL`, so the driver's real connect (which follows a successful probe) never
  re-triggers the reset.
- **Pulse guiding is hardware-timed**: OnStep supports native duration-bearing guide commands
  (`:Mgn####`/`:Mgs####`/`:Mge####`/`:Mgw####`, 4-digit zero-padded milliseconds) — fire-and-forget,
  the mount stops the pulse itself. `IsPulseGuiding` is tracked from the known requested
  duration, not polled from the mount. Unlike SynScan (no native pulse guide), this driver does
  **not** need a background stop-thread.
- **SideOfPier must be computed from hour angle, not read from `:GU#`'s `E`/`W` flag** — the same
  lesson iOptron already learned from its `:GEP#` raw pier value (see the [iOptron notes](ioptron.instructions.md)):
  a mount's raw physical-pier-side report does not match the ASCOM convention once tracking past
  the meridian. `:GU#` does encode pier side directly (confirmed on real hardware, firmware
  "On-Step" v10.23a, characters `E`/`W`), and an earlier version of this driver preferred that
  flag over the hour-angle computation, falling back to hour angle only when neither character
  was present. This parsed correctly (verified against the exact character table below) but
  failed ConformU's SideofPier check on real hardware: `Reported SideofPier at HA -9, +9: EE`
  (constant, reflecting the physical side, which only changes on an actual mechanical meridian
  flip) instead of the required `WE` (flips with HA sign — ASCOM's pointing-state contract).
  `get_side_of_pier()` now always computes from hour angle (`LST − synced RA`); `:GU#`'s `E`/`W`
  is parsed and available in `MountStatus` but intentionally unused for this property. Do not
  reintroduce a firmware-flag-preferred branch here without re-validating against ConformU's
  SideofPier/DestinationSideofPier checks. Also avoid forcing a live position refresh inside
  `get_side_of_pier()` (a `:GRa#`/`:GDe#` round trip, ~0.13s each on this serial link, can push
  the read over ConformU's 0.1s FAST target) — hour-angle *sign* tolerates a couple of seconds of
  RA staleness, so reuse whatever is already cached and only force a fetch if never yet populated.
- **`:GU#` status-character table**, confirmed against real hardware traces (`nNpHrsEo180` etc.
  observed live): `n`/`N` = **NOT** tracking / **NOT** slewing respectively (`== npos` check, i.e.
  the character's *absence* means the state is active — this is the opposite polarity of what
  the INDI driver's comments implied, and was the single biggest early bug in this driver).
  `H` = at home, `P`/`p` = parked/not parked, `E`/`W` = pier side east/west. Any other character
  is ignored (forward-compatible with firmware revisions); only a total non-response is a fault.
- **RA/Dec position reads use the high-precision variants**: `:GRa#`/`:GDe#` (fractional
  seconds/arcseconds: `HH:MM:SS.SSSS#` / `sDD*MM:SS.SSSS#`), not `:GR#`/`:GD#` (whole-second/
  whole-arcsecond resolution). The whole-second RA resolution (~15″ at the equator) is far too
  coarse for ConformU's 0.07″ PulseGuide tolerance on short (2-5s) guide pulses — confirmed via
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
- **Serial `VTIME` must be 1 (100ms), not higher**: an earlier version copied Gemini's one-time
  probe `VTIME=20` (2s) into the production `connect_serial()` path, causing every blocking read
  to cost up to 2 real seconds — most visibly a ~2s stall on `PulseGuide` and pushing `AbortSlew`
  (5 sequential blind commands) toward ConformU's 1.0s STANDARD timeout. `send_command_blind()`'s
  stale-input drain window is `50ms` (one VTIME cycle) for the same reason — `120ms` (2+ cycles)
  pushed `AbortSlew` to 1.04s, just over budget.
- **`Park()`/`FindHome()` must be fire-and-forget, not blocking**: send the command and return
  immediately (optimistically marking `is_parked=false`/`is_at_home=false` and `is_slewing=true`,
  letting the next status poll pick up real completion), matching iOptron's pattern. A first
  version blocked synchronously until the mount physically finished (`wait_for_condition_locked`)
  — functionally correct, but ConformU classifies `Park`/`FindHome` under the strict STANDARD
  (1.0s) timing target when the call itself blocks for the whole operation, vs. the lenient
  EXTENDED (600s) target when the call returns fast and the *client* polls `AtPark`/`AtHome`
  afterward (confirmed by comparing timing-summary entries against iOptron's own passing
  ConformU log, where `Park`/`FindHome` also take ~30s in wall-clock terms but show `0.001s ✓
  (EXTENDED)` in the timing table). Real GEM mounts physically cannot park/home in 1 second;
  fire-and-forget is the only way to pass this target with genuine hardware.
- **`DeviceState`'s first post-Connect call routinely exceeds the 0.1s FAST target (~0.65s),
  and this is NOT fixable by warming caches during `connect()`**: two different warm-up
  strategies (locked, then a mutex-released ToupTek-AFW-style version) were tried against real
  hardware and neither moved the measured time at all — ConformU's Connect()/Connected preamble
  (`Connected=True` → `Connected=False` → `Connect()`) calls `DeviceState` fast enough after the
  final reconnect that the live `:GA#/:GZ#/:GU#/:GRa#/:GDe#` round trips always land inside its
  measurement window regardless of what connect-time work has or hasn't finished. Treat this as
  an accepted, inherent one-time cold-start cost, not a bug to keep chasing.
- **The same connect-adjacent cold-start window can land on other FAST members too, not just
  `DeviceState`** (re-confirmed on real hardware, ConformU 4.5.0, after the driver-branch update
  that ported this driver's auto-detect to the shared `serial_by_id_scan.h` helper): two
  consecutive runs both showed `AlignmentMode` (~0.135s), `Declination` (~0.318s),
  `EquatorialSystem` (~0.136s), and `SideOfPier Read` (~0.140s) OUTSIDE the 0.1s FAST target,
  every time, at nearly identical values (±0.003s) — including `AlignmentMode`/`EquatorialSystem`,
  which are compile-time constants with no wire I/O at all. This is the same phenomenon as the
  `DeviceState` note above, not a regression from that auto-detect change (which only touches
  `enumerate_onstep_ports()`, never called on this code path): manually repeating the same calls
  a few seconds after `Connect()` (well outside ConformU's rapid-fire post-Connect property sweep)
  answers in ~0.003s every time, confirming it's a transient connect-adjacent window, not a
  permanent per-call cost. Which members land inside that window depends on ConformU's exact call
  order/pacing for a given version, so don't be surprised if it isn't `DeviceState` specifically.
  Same conclusion applies: treat as accepted, not a bug to chase-fix via caching.
- **`PulseGuide` East/West shows 1-2 residual ConformU issues per run, right at the 0.07″
  tolerance boundary, and this is inherent hardware noise, not a driver bug**: root-caused via
  four independent tests, all on real hardware — (1) A/B test with OnStep's refraction
  rate-compensation feature (`:Tr#`/`:Tn#`) on vs. off: no correlation, disabling it made the
  residual *worse*; (2) mount unloaded vs. with the real telescope mounted and balanced: no
  measurable difference; (3) attempted testing far from the celestial pole: blocked by ConformU's
  own `Park()`/`FindHome()` always returning the mount near the pole regardless of test
  settings; (4) the same residual reappears at a different member (sometimes `-9.0`, sometimes
  `+9.0`, North or South) across repeated runs, and all 4 rate labels (`-9.0`/`-3.0`/`+3.0`/
  `+9.0`) drive the *same* physical pulse (`GuideRateDeclination`/`RightAscension` are fixed,
  `CanSetGuideRates=false`) — i.e. these are 4 repetitions of one experiment, and a noise floor
  straddling a tight tolerance will intermittently cross it. Do not attempt to "fix" this by
  loosening tolerances or by re-adding rate-compensation toggling.
- **v1 scope is equatorial mounts only** (`AlignmentMode::GermanPolar`), matching every other
  telescope driver in this project (iOptron, SynScan, Celestron). `SlewToAltAzAsync`
  is supported via a local Alt/Az→RA/Dec coordinate transform followed
  by an ordinary equatorial slew (the router refuses synchronous `SlewToAltAz` with
  `MethodNotImplemented`, #775) — this works regardless of the physical mount's mechanical
  alignment, so it does not require native AltAz mount support. `SyncToAltAz` is not implemented
  (`CanSyncAltAz=false`) since OnStep has no native Alt/Az sync command.
- `CanSetGuideRates=false` (guide rate is not queryable/settable over this protocol — the
  `GuideRate` property returns an informational fixed value only) and `CanSetPierSide=false`
  (no distinct "set pier side" command). `CanSetDeclinationRate`/`CanSetRightAscensionRate` are
  also `false` — custom tracking-rate commands were not confirmed against real OnStep firmware
  during this implementation, so the setters throw `PropertyNotImplemented` rather than
  advertise support that might silently no-op.
- **ConformU-validated on real hardware** (Generic OnStep DIY harmonic-drive mount, firmware
  "On-Step" v10.23a, Linux arm64, ConformU 4.4.0): 0 errors, 0 timing violations in the saved
  run. The only remaining findings are the 2 PulseGuide East/West residual issues documented
  above as an accepted, inherent hardware-noise limitation rather than a driver bug — earlier
  runs during this same validation session also hit the DeviceState-cold-start-style FAST
  timing risk described above (via `get_side_of_pier()`'s live position round trip, since
  fixed), which is why that failure mode is documented even though the saved log is clean of it.
  See `AlpacaCore/conformu/OnStep/Generic OnStep/Linux-arm64.txt` and `SUPPORTED-DRIVERS.md`.
