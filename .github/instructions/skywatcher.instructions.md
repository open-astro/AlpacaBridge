---
applyTo: "AlpacaCore/src/vendors/skywatcher/**,AlpacaCore/include/alpacacore/vendor/skywatcher/**,AlpacaCore/tests/*skywatcher*,AlpacaCore/conformu/SkyWatcher/**"
---

### SkyWatcher (Wave / direct motor controller)

Related decisions and failures — read when changing the behavior they explain:

- [Pointing-clock decision](../../docs/decisions/0001-skywatcher-pointing-clock.md)
- [EQMOD board-detection failures](../../docs/failures/0006-eqmod-board-detection.md)
- [EQMOD pointing-validation failures](../../docs/failures/0007-eqmod-pointing-validation.md)
- [EQMOD cross-axis motion failures](../../docs/failures/0008-eqmod-cross-axis-motion.md)


Devices: Telescope. Vendor key `skywatcher` — distinct from `synscan`, which speaks the
hand-controller protocol. This driver speaks the **Sky-Watcher motor controller command
set** (the `:` command / `=`|`!` reply protocol) directly to the mount's motor board.
Target hardware: Wave 100i/150i (also applicable to AZ-GTi-class mounts).

Protocol documentation: `AlpacaCore/external/SynScan/SkyWatcher_Motor_Controller_Command_Set.md`
(shared with the SynScan vendor directory). No external SDK required.

Connection types: Serial (the mount's own USB port or an EQDIR-class adapter, 8N1; the scan probes
9600 then 115200 per port, because an EQ board's built-in PL2303 port answers only at 115200,
so a silent Prolific/FTDI/CH340-class port costs at least about 3.3 s per scan: 1.5 s at 9600, the
300 ms SynScan echo guard, 1.5 s at 115200, and up to roughly 4.4 s because the read loops only
check their deadline between `VTIME` reads; multiplied by every such adapter on the rig; #403
records the measurement) and Network (built-in Wi-Fi module,
**UDP** port 11880 — one command per datagram, one reply per datagram; AP-mode address
192.168.4.1). The wrapper retransmits up to 3 times on UDP timeout and drains stale
datagrams before each send so replies cannot get off-by-one.

- **The serial probe asks for a SynScan handset echo first and skips the port if one answers** (`util/synscan_handset_probe.h`, 2026-09): a SynScan V4 hand controller (fw 04.40.00, built-in PL2303 `067b:23a3`) shares the Prolific adapter class this scan targets, and it stops answering serial ENTIRELY after receiving bytes at the wrong rate — one motor-controller probe at 115200 is enough — until it is power-cycled (unplugging the mount is not enough when the handset runs on USB power from the SBC). On the EQM-35 Pro rig this was the whole "hand-controller commands time out" report: the handset had been wedged by this probe at service start. The guard is at the top of `probe_skywatcher_port()`, gated on the caller's baud not being 9600 (the rate that is safe for a handset to receive). Since the EQ-class support landed, `kProbeBauds` includes 115200, so the guard fires on every scanned port that stays silent at 9600 and costs its 300 ms echo timeout there; it still covers every future non-9600 caller; the same hazard applies to any other scan that sends non-9600 traffic to Prolific-class ports (the iOptron iEAF/iAFS2/3 and iEFW handshakes at 115200 are the known ones — not yet guarded). The SynScan driver's serial connect additionally claims the port with `TIOCEXCL`, an independent layer that blocks a concurrent same-process open regardless of baud. Hardware-verified 2026-09-09 (EQM-35 Pro rig): the exact-echo path connects cleanly, a manual open of the port while connected fails with `EBUSY` and succeeds again immediately after disconnect (no lock leak), and an abrupt `systemctl restart` mid-poll still re-detects the handset on the very first probe after restart (the closest this rig can reproduce of the stale-reply race the tolerant read loop targets). **Not exercised by a hardware pass yet:** the baud gate firing at 115200 — the fix's own validation branch only ever called `probe_skywatcher_port()` at 9600; its trigger (`kProbeBauds` including 115200) is now on `main`, so the combined guard-plus-115200 path still needs its own hardware pass on the EQM-35 Pro rig. **Reverse direction (review on open-astro#242):** the guard sends the 9600 echo to a port that may be a Sky-Watcher motor board expecting 115200 — could a board be wedged by wrong-rate bytes the way the handset is? Empirically no, and the guard adds no new class of traffic: on `main` the probe has always sent `:e1` at 9600 to every candidate port, and `driver/skywatcher-eqm35`'s `kProbeBauds` tries 9600 first, so the real EQM-35 board received 9600 traffic before every successful 115200 detection during that branch's hardware bring-up (build a014531). The guard only runs on the non-9600 pass, i.e. after that same port has just been probed at 9600. Still, when the branches are combined, the combined pass should confirm both directions on the same rig: handset not wedged by the scan, board still detected at 115200 after the 9600 echo.
- **Coordinates are equinox of date, not J2000.** `EquatorialSystem` reports Topocentric and
  RA/Dec come from LST, so they are mean-equinox-of-date; plate solvers return J2000, and the
  two drift apart by ~50"/yr since 2000 (~22 arcmin in 2026, mostly RA). Alpaca clients read
  the flag and convert; a raw solver output held next to the driver's reported RA/Dec does not,
  and a `SyncToCoordinates` fed a J2000 position writes the whole offset into the mount.
  Precess before comparing or syncing, and rule this out before reading a ~20' goto error as
  a driver bug (open-astro#230, Wave 150i report).
- **All pointing math lives in the driver.** The MC protocol only counts steps: the driver
  owns RA/Dec <-> axis-angle conversion (CPR read at connect via `:a`, timer frequency
  `:b`, high-speed ratio `:g`), LST computation, pier-side selection, and tracking-rate
  step-period math (`T1 = TMR_Freq * 360 / rate / CPR`, times the high-speed ratio in
  fast mode). The mount stores **no site or time** — site lat/long/elevation come from
  the web UI config or the Alpaca setters. **Latitude and longitude are mandatory on this
  vendor** (#274): `configuredevice` rejects a skywatcher config without both, and
  `Connected = true` throws `InvalidOperation` unless each has been set explicitly, by
  config or by its setter. A config **already on disk** is registered anyway, with a WARN,
  and left for the connect-time guard to refuse: a device dropped at startup never enters
  the registry, so `configureddevices` cannot list it and the web UI offers no way to edit
  the entry that is at fault. That asymmetry is the rule for any new validation in
  `register_device_from_config` — reject `ConfigSource::Api`, warn on `ConfigSource::Persisted`.
  Since #380 that rule is not left to each branch to remember: `Router::reject_invalid_config()`
  takes the source and the reason and returns whether the caller must refuse, and
  `Router::normalize_persisted_connection_type()` does the same for an unrecognised
  `connectionType`, which has no value to carry forward — it returns `"serial"` for a persisted
  config, never `"auto"`, so the connect fails on the port path instead of auto-probing and
  attaching to whatever mount answers. Use them rather than an inline `return false`; the
  `portPath`, `host` and `connectionType` checks in every telescope branch do.
  Both coordinates are also **range-checked** (#398), inclusive of ±90/±180 since the poles and
  the antimeridian are real places, and rejecting NaN and the infinities: presence alone let a
  config carry latitude 200, which reads as northern to `hemisphere_south_locked()`, while the
  ASCOM setters have always refused exactly that at runtime — a validation a client cannot bypass
  but a config can is not a validation. The reads and the check live in one shared
  `read_site_coordinates()` used by all seven vendor branches that take a site, and on the
  persisted path the offending coordinate is **cleared** so the driver's unset handling covers it. `0.0` is a real coordinate, so the driver tracks whether each
  was ever set rather than testing for the value — an unset southern rig would otherwise
  run northern pointing math: the #432 sky frame (both the `a1` term and dec), the RA
  tracking direction (#250, restored by #432) and the Dec rate / pulse-guide sign (#253).
  **Not #261**, despite what this line said before #432 and what the `#274` CHANGELOG entry
  still says as history: the pier-side label is picked from the sky hour angle and is
  hemisphere-independent, which is one of #432's findings; the mechanical branch is
  `k * side` and mirrors with the hemisphere only on a board with a measured sense (#458).
  The driver comment on the connect-time guard says the same. Time comes from two functions: `utc_now_locked()`
  feeds every LST computation (pointing, `SiderealTime`, pier side, gotos) and applies the
  client-set `UTCDate` offset only while the host clock is undisciplined (no NTP): sampled at
  the write and, while such an offset is armed, re-sampled at most once per 30 s on the pointing
  path through `detail::host_synchronized_probe()` (one `adjtimex` read, no device I/O; #405), so
  a client's clock error never steers pointing on an NTP-good host and stops steering it within
  about 30 s of the host becoming disciplined by slewing (INFO log; the flag only moves
  undisciplined to disciplined, since ignoring the offset is the safe side);
  `client_utc_now_locked()` feeds the `UTCDate` readback and always honours the client's write,
  because that property is the client's to set and ConformU reads back what it wrote (#287,
  #351). On an NTP-less host the router also steps the system clock from that write (#289). The
  offset is not sticky: it is dropped (with an INFO log) as soon as the host clock is stepped
  underneath it (Sync Time, NTP taking over, `date`), detected as the system and steady clocks
  disagreeing by more than 1 s since the write, and re-armed by the next `UTCDate` write; the
  30 s re-sample above covers discipline gained without a step. Tests pin both branches through
  the probe seam (`ProbeGuard` in `test_skywatcher_async.cpp`) rather than the build host's own
  clock state (#395). **This split is Sky-Watcher-only.** A mount with its own clock (OnStep,
  Celestron, SynScan, iOptron, ZWO AM) has the ASCOM `UTCDate` setter write the MOUNT's time,
  and the driver's goto and sidereal logic then run on that clock (the mount itself may discard the write, as an aligned Celestron does), so those drivers keep aiming by the
  client's instant on purpose; what they share with #301 is the once-per-connection WARN when
  an NTP-disciplined host disagrees with the client by more than
  `HostClock::kClientDisagreementWarn`, through `alpacacore/util/client_utc_warning.h`
  (`ClientUtcWarning::warn_once()` after the write, flag re-armed on each real connect but
  NOT on a `Connected=true`-while-connected no-op, or a client that re-sends both would get a
  line per poll; probe seam `set_host_synchronized_probe()` for tests; #409). A new driver that caches a client-set time
  the same way calls it too.
- Pointing convention (#432): home = counterweight down, tube parallel to the polar axis
  pointing at the visible pole, counts offset `0x800000`, axis angles `a1`/`a2` in degrees
  from home in the increasing-count direction. **`HA = s * (a1/15) + k * (branch * 6 h)`
  and `dec = s * (90 - |a2|)`, with `s = +1` north and `-1` south**, where `branch` is the
  sign of `a2` away from the pole and, inside a two-count deadband of `a2 = 0` where the
  encoder cannot say, the branch the last goto or sync commanded
  (`branch_from_axis_locked()`, #459). The 6 h term is the
  counterweight-down home: the dec axis lies in the meridian plane there, so a dec-only
  rotation sweeps the HA = ±6 h circle and the meridian needs the bar horizontal
  (`a1 = ±90`); every reachable target keeps `|a1| <= 90`, which is the
  counterweight-never-above-horizontal rule falling out of the geometry. Its sign
  `k = s * eps` (`home_term_sign_locked()`) flips with the hemisphere like the `a1` term,
  because a mount facing the other pole is the same mount turned half a turn about the
  vertical, and with the board's dec-axis count sense `eps`, which is wiring, not
  latitude (#458). `eps` is **measured, per mount code** (`measured_dec_axis_sense()`):
  -1 for the EQM-35 Pro (0x32; south 2026-09-12 and a +37.2 latitude on the same rig
  2026-09-19), +1 for the Wave 150i (0x45; the #432 report, north), +1 for the
  EQ-AL55i Pro (0x09; a reporter's mount at about +40, 2026-09-20, #579). Every other
  board keeps `k = +1`, the shipped model, which is 12 h out wherever that board's
  `s * eps` is -1; adding a board takes one reading on it, not a derivation. Two boards
  read +1 and one reads -1, so `eps` is per board and not a family constant, and the
  classic Synta boards (EQ6, HEQ5, AZ-EQ6, EQ5 Pro) are all still unmeasured (#579).
  The Wave 150i and the EQ-AL55i Pro south of the equator follow from geometry and have
  not been measured; a board measured only in the north constrains nothing there, because
  `s * eps = +1` in the north is also the unmeasured default. Pier side is
  `k * branch > 0` -> pierEast, the same reader, since the goto picks the side from the
  sky hour angle; the Dec rate and guide signs read `branch` alone, because dec does not
  involve `eps`.
  Tracking, `RightAscensionRate` and East/West pulses go through `ra_axis_sign_locked()`
  (counts up north, down south); `MoveAxis`, goto deltas and AutoHome are mechanical and
  never apply it. With `eps = +1` this is `indi-eqmod`'s `EncodersToRADec()` in both
  hemispheres (asserted in `test_skywatcher_pointing.cpp`); with `k = +1` it matches
  indi-eqmod in the north only and differs by 12 h in the south, and on the EQM-35 Pro
  the hardware backs this model, not indi-eqmod's default.
  **Do not judge this model by the driver's own reported RA/Dec, ConformU included: the
  driver reports what it commands.** It was established by driving an EQM-35 Pro to known
  axis positions and reading the tube's real direction off the mount (2026-09-12); those
  rows are in the driver comment and asserted in `test_skywatcher_pointing.cpp`. Extend
  that file with a new hardware row for any change here.
- **Sync** uses the controller's own `:E` set-position command (motors must be fully
  stopped — the driver pauses tracking around the write), never a driver-side offset.
- **Pulse guiding**: RA pulses while tracking are done by changing the RA step period
  in-place (`:I` is legal during slow-mode motion), then restoring the sidereal preset —
  the axis never stops. Dec pulses (and RA while not tracking) are software-timed
  speed-mode nudges. Position override accumulation as per the SynScan lessons.
- **A live `:I` on a running axis is not always applied** (EQM-35 Pro, MC firmware 3.39,
  2026-09-06): the board stores the preset (`:i` reads it back) but the motor keeps its old
  rate. Every live in-place `:I` is therefore followed by a `:J` re-latch (INDI does the
  same), and the driver sample-verifies the rate over ~450 ms (`verify_live_rate_or_rekick`)
  and resends `:I`+`:J` if the axis did not change speed. Pulses ≥ 1.5 s verify inside the
  pulse task (the window is deducted from the pulse; shorter pulses rely on the kick alone);
  the `RightAscensionRate`/`TrackingRate` setters cannot wait 450 ms inside a property call,
  so they spawn a one-shot background task (`rate_verify_thread_`, open-astro #248). That
  task never takes `mutex_`, which is what lets every RA-taking path reap it WITH `mutex_`
  held (setters, Tracking off, `stop_axis_and_wait_locked`, pulse dispatch, AbortSlew,
  disconnect) — a lock-free reap would leave a window for a setter to spawn one between an
  initiator's reap and its lock, and the resend would land mid-pulse or on a stopped axis.
- `:f` status nibbles: char0 bit0 speed-mode/bit1 CCW/bit2 fast; char1 bit0 running/bit1
  blocked; char2 bit0 init-done/bit1 level switch. Slewing = running AND NOT speed-mode
  on either axis (a tracking axis is not slewing).
- Connect sequence: `:e` version, `:a`/`:b`/`:g` per axis, then `:F` init (with `:E` home
  stamp) ONLY when the status reports not-initialized — never re-stamp an aligned session.
- **Wave USB port is STM32 CDC-ACM** (`0483:5740`, `/dev/ttyACM*`, by-id name
  `usb-STMicroelectronics_STM32_Virtual_ComPort_...`), NOT a ttyUSB bridge chip. The
  auto-detect scan must include STM32/STMicroelectronics in the candidate filters and probe
  `/dev/ttyACM0-9` as well as `/dev/ttyUSB0-9`. Baud rate is irrelevant on CDC-ACM.
  Hardware-verified: `:e1` on the Wave 100i replies `=033A44` (MC firmware 3.58.68).
- **Park and MoveAxis(axis, 0) are asynchronous initiators** (ConformU 4.5 STANDARD timing,
  1 s target): a blocking park slew (19 s) and a blocking stop-and-wait in MoveAxis(0)
  (1.2 s deceleration ramp) both failed timing on real Wave 100i hardware. Park dispatches
  the slew in the background (AtPark turns true on completion); MoveAxis(0) issues the stop,
  keeps Slewing true via the manual flag, and a background task clears it and restores
  tracking once the axis reports stopped. This applies to every telescope driver.
- ConformU needs a real site. Since #274 a Sky-Watcher device with no site refuses
  `Connected` outright, so the run fails at connect. Since #358 the client is told why:
  the router reports the driver's own sentence naming the two fields as the
  `ErrorMessage` (the error number is still `NotConnected`), rather than a bare
  "Connection failed" with the reason left in the server log.
  Set the observing site in the web UI before validating. Before #274 the site collapsed
  to 0,0 instead and the CheckMethods slew tests aborted with "highest elevation
  available is below the horizon".
- Web UI: `skywatcher`-prefixed field names; network field is `udpPort` (NOT `tcpPort`).
- ConformU 4.5.0 validated on Wave 100i over **both transports** (Linux arm64): USB (dev PC)
  and Wi-Fi UDP (Raspberry Pi CM4 joined to the mount AP) — 0 errors, 0 issues, 0 timing
  violations each; slews within the ±10 arcsec tolerance.
- **Async-initiator self-deadlock trap (Park)**: making `Slewing` report true while
  `parking_` (so pollers never see the Slewing-false/AtPark-false gap) breaks the park
  task itself if its completion wait polls the same accessor — it can never observe
  "stopped" and times out (ConformU: "Failed to park within 300 seconds"). The internal
  wait must poll a hardware-only variant (`get_hardware_slewing_locked`).
- **PulseGuide is also an async initiator**: the axis dispatch (stop-and-wait on a
  ramping axis + possible UDP retries) took 1.79 s synchronously; it now runs inside the
  background pulse task with `IsPulseGuiding` already true at return.
- **AutoHome (home index sensors)**: the Wave reports feature bit 0x04 (":q" data
  0x000001 -> 0x100C) on both axes. FindHome ports the EQMod AutoHome procedure:
  arm the indexer (":W" data 0x000008), read it (":q" data 0x000000 -> 0 below /
  0xFFFFFF above / latched count), hunt the edge, approach from below, then
  `:E`-stamp kHomeCounts at the sensed mark. Count-frame home (goto 0,0) is the
  fallback for boards without the bit — and is NOT physically meaningful unless
  the mount was powered on at home (a killed ConformU run mid-sync can shift the
  frame; this is why AutoHome matters).
- **Tracking rates**: Lunar/Solar are just different step-period constants
  (live `:I` change while tracking). **RA/Dec rate OFFSETS are supported**
  (issue #214): RightAscensionRate is SUBTRACTED from the drive rate
  (RA = LST − HA), DeclinationRate flips sign on the east branch (a2 ≥ 0,
  dec = 90 − a2), sub-floor Dec rates duty-cycle floor-rate bursts on a 3 s
  period (~140 ms stop-landing compensation), reads hold the dead-reckoned
  model while offsets run, and offsets zero on a drive-rate change (setters
  throw InvalidOperation off Sidereal). The two "hardware anomalies" that
  originally deferred this (Dec undershoot 40→16 as/s; ~45 arcsec RA count
  jumps after in-place `:I` writes) were bench-DISPROVEN on 2026-08-23: direct
  UDP measurements show Dec tracks 5–320 as/s within 0.2% and zero `:j` count
  glitches across 120 reads interleaved with `:I` writes. Both symptoms were
  artifacts of the pre-#216 refinement-goto races, not the motor controller.
- **No read freezes — ever**: the old 10 s post-slew/post-sync/pulse position
  overrides masked a real GOTO landing error (~3 arcmin east: axis targets were
  computed with LST at dispatch, not arrival) and corrupted every ConformU 4.5
  endpoint measurement (rates, pulse displacement, sync return). Cures that
  replaced them: (1) gotos aim at the ARRIVAL-time LST and refine to an
  8 arcsec deadband; (2) reads dead-reckon `cached + commanded rate x elapsed`
  between hardware polls (kills the LST-vs-stale-cache sawtooth and count
  quantization); (3) LST uses sub-second time (whole-second truncation stepped
  RA in 15 arcsec jumps); (4) sync computes its frame AFTER the axes stop,
  aimed at the tracking-restart moment (a pre-stop frame is stale by the whole
  1-3 s pause -- ConformU saw a constant ~79 arcsec return error).
- ConformU 4.5 **physically measures pulse-guide displacement** (Dec moved,
  RA unchanged) -- a driver that freezes reads during the pulse fails with
  "The declination axis did not move".
- **Slewing must be a STATE FLAG spanning the whole goto + landing refinement**
  (`goto_in_progress_`, same pattern as `parking_`/`homing_`), never a timed
  hold: the 3 s `slew_force_until_` expired during a slow refine iteration
  (axis stop-waits take seconds), Slewing flickered false, ConformU started
  its pulse test, and the next refinement goto dragged the axes back to the
  slew target ("declination axis did not move", phantom RA drift, the
  constant ~79 arcsec sync-return error). Diagnosed by logging every motion
  frame (:G/:I/:J/:K) at WARN and killing ConformU at the first issue -- the
  trace showed three refinement gotos interleaved with the pulse.
- **Reap the pulse task at every motion boundary** (slews, park, home,
  moveaxis, sync, abort): ConformU's dual-axis pulse test leaves a live pulse
  timer that otherwise fires its stop/step-period restore into the middle of
  the next goto. A CANCELLED pulse task must not touch the hardware -- the
  canceller stops or re-commands the axes itself.
- **AbortSlew must cancel the async slew task** (set `slew_task_cancel_`,
  join later via reap) or the landing refinement re-slews after the abort;
  every slew entry point reaps first, which also resets the flag.
- Debug technique: a watchdog loop that `pkill`s ConformU at the FIRST logged
  issue preserves the exact journal window and stops the mount from grinding
  through a failed run.
- The shipped images log at WARNING: `ALPACA_LOG_INFO` never reaches
  journalctl on the test rigs -- temporary debug instrumentation must log at
  WARN or it silently vanishes.
- **CM4 `ondemand` CPU governor causes ~100 ms single-member FAST blips**
  early in ConformU runs (first request burst pays the clock ramp; even the
  I/O-free EquatorialSystem getter blipped). Three consecutive runs each had
  exactly one such blip until the governor was pinned to `performance` --
  then 0 timing violations. Same class as the RK3568's `interactive`
  governor (that image got `openastro-cpufreq.service`); consider the same
  for the CM4 image. Not Wi-Fi: power save was off and the BSSID pinned.
- **Wi-Fi UDP field lessons** (Wave AP + SBC): (1) a single-radio SBC running hotspot
  (`ap0`) + client (`wlan0`) dual-role flaps the link — disable the hotspot while the
  mount Wi-Fi is in use (and beware hotspot subnets clashing with the mount's
  192.168.4.x); (2) an AP rejoin can change the local address, invalidating a
  `connect()`ed datagram socket (`ENETUNREACH`) — the wrapper rebuilds the socket and
  resends once; (3) retransmit duplicates cause reply mis-pairing — defenses are
  drain-before-send, a settle drain after any timeout, and per-command expected reply
  length validation; (4) run ConformU on the SBC itself (localhost), not across the LAN —
  VM-to-SBC jitter alone produces FAST-target (0.1 s) violations.
- **Disconnect all stray Alpaca clients before a ConformU run**: the per-client Connected
  registry keeps the device physically connected for other ClientIDs, so leftover test
  sessions carry state (targets, tracking) into ConformU's "first time use" checks.
- Deploy note: the systemd service executes `/usr/bin/alpacabridge` — install the built
  `alpacahttp_server` there (NOT `/usr/local/bin/`), and verify with
  `md5sum /usr/bin/alpacabridge` after restart; a wedged park/slew thread can hang
  `systemctl stop` (use `systemctl kill -s SIGKILL`).

#### EQ-class Synta boards (EQM-35 Pro and relatives) — 2026-09-06

The `:` command set is identical on classic Synta EQ mounts, so the Wave driver drives
them unchanged. What differs is the transport and the identity, and both bit us:

- **Baud is NOT irrelevant off the Wave.** The Wave's USB port is STM32 CDC-ACM, where
  the baud setting is ignored. Synta EQ boards reached over the mount's own USB port or
  an EQDIR cable are real UART bridges: the **EQM-35 Pro's built-in port is a soldered
  Prolific PL2303 (067b:23a3, "ATEN Serial Bridge") at 115200**, and a 9600-only scan
  finds nothing at all. Enumeration probes 9600 then 115200; the probe's winning baud
  MUST be carried into `ConnectionInfo` (auto-detect used to drop it, so a board found
  at 115200 was reopened at 9600 and every command timed out).
- **`":e"` byte 3 is the MOUNT CODE, not a firmware patch level.** Layout is
  `<fw major><fw minor><mount code>`, matching INDI `skywatcherAPI.cpp`. The Wave's
  `=033A44` is firmware 3.58 + code 0x44 (WAVE_100I), never "3.58.68". EQM-35 Pro:
  `=032732` -> firmware 3.39, code **0x32**, a code in neither INDI's `MountType` enum
  nor Sky-Watcher's published SynScan model list. Cross-confirmed: the SynScan handset
  on the same mount reports model id 50 (= 0x32) from its own `m` command, so
  `synscan_model_id_to_name` gained `case 50` too.
- **Feature word tells you which mount you are on.** `":q"` with data 0x000001 succeeds
  on EQ boards — it does not throw — the home-index bit is simply absent. EQM-35 Pro
  returns **0x7000** (POLAR_LED | COMMON_SLEW_START | HALF_CURRENT_TRACKING); the Wave
  returns 0x100C (POLAR_LED | IS_AZEQ | HOME_INDEXER). Flags follow EQMod's set. Gate
  AutoHome on the 0x04 bit, never on `":q"` failing: an EQM-35 takes the count-frame
  `FindHome` fallback, and running the sensor hunt on a mount with no index sensors
  would drive the axes looking for an edge that never arrives.
- The three presets live in `FakeSkyWatcherMount` as `FakeMountProfile::wave_100i()` /
  `eqm35_pro()` / `eq_al55i()`, so loopback tests run against real captured geometry.
- **Hardware bring-up, EQM-35 Pro over the mount's built-in USB, 2026-09-06** (Raspberry
  Pi 3B, Debian 13 arm64, direct USB-A-to-B, no handset in the chain): auto-detect found
  it unaided -- `Found Sky-Watcher EQM-35 Pro on /dev/ttyUSB0 (MC firmware 3.39, 115200
  baud)` -- and `Name` reported "Sky-Watcher EQM-35 Pro", firmware "3.39". CCDciel
  connected over Alpaca with zero driver warnings. Further bring-up notes (pointing math,
  MoveAxis semantics, tracking-rate measurement, the southern-hemisphere fixes) are
  recorded against those fixes elsewhere in this section.
- **SynScan hand controller in "PC Direct Mode" reaches this driver unchanged, 2026-09-10**
  (open-astro#275; EQM-35 Pro, SynScan V4 handset, Raspberry Pi 3B). The handset's menu
  setting switches its own USB port from the SynScan command set to the raw motor-controller
  protocol, so a `skywatcher` serial device pointed at the *handset's* port (9600 baud — PC
  Direct Mode keeps the PC-facing rate) connects exactly like the board's own port: identity
  `EQM-35 Pro (mount code 50), firmware 3.39`, Declination bit-identical to the direct port
  (so `:a`/`:b`/`:g` geometry relays intact), `PulseGuide` N/S +11.25" and back to the same
  count. The #242 echo guard steps aside by itself — a handset in this mode no longer answers
  the SynScan echo — and the SynScan driver's scan then finds nothing on that port, which is
  correct. Caveat: command latency through the 9600-baud relay is higher and more variable
  than the board's own port; open-loop `MoveAxis` legs of ±2 deg/s for 1 s netted ~7 arcmin
  instead of ~2 arcsec. Driver-timed motion is unaffected. Prefer the mount's own USB port
  or an EQDIR cable where available; PC Direct Mode is a working no-extra-hardware fallback
  for classic mounts that have neither (the #230 audience). Docs line for
  `SUPPORTED-DRIVERS.md` lands with the post-ConformU direct-driver docs PR.
- **EQM-35 Pro over an EQDIR cable (FTDI FT232R), 2026-09-21** (Raspberry Pi 3 Model B, Debian 13
  arm64, no handset in the chain): the cable enumerates as `usb-FTDI_FT232R_USB_UART_<serial>-if00-port0`
  (`/dev/ttyUSB0`) and, unlike the mount's built-in Prolific port, answers at **9600 baud**. Auto-detect
  found it unaided in 27 ms -- `Found Sky-Watcher EQM-35 Pro on /dev/ttyUSB0 (MC firmware 3.39, 9600
  baud)` -- with the same identity as over the built-in port (`EQM-35 Pro (mount code 50), firmware
  3.39`). With an explicit `connectionType: serial`, `baudRate: 9600`: `FindHome`, a 30 s slew to
  RA 17.249 h / Dec +60 deg, and `FindHome` back returned both axes to the exact home count
  (`:j1`/`:j2` = `=000080` before and after), and one session of 7146 motor-controller transactions
  logged no serial timeout, checksum or EIO error. The TRACE log spaces back-to-back commands about
  16 ms apart at 9600 baud, against about 2 ms over the built-in port at 115200. Two limits on
  what this proves: the EQM-35 has no HOME_INDEXER bit (feature word 0x7000), so that `FindHome`
  exercised the count-frame goto fallback, not home-sensor AutoHome; and open-loop `MoveAxis`
  timing was not measured over EQDIR, so the PC Direct Mode latency caveat above is neither
  confirmed nor ruled out for this path.

#### Goto landing, tracking restart and the dev-VM clock (EQM-35 Pro) — 2026-09-12

Full ConformU on the EQM-35 Pro over USB, in a Lima Debian 13 arm64 VM on an Apple Silicon Mac
with the mount's USB-serial bridge passed through by VirtualHere. Five full runs; each finding
below was one of them.

- **The controller's stopped flag is not the end of a goto.** After a 6 h slew and three landing
  refinements, the last refinement's landing read 11 counts short of its `:S` target while `:f`
  already said stopped; every clean landing in the same log read exactly on target. The tracking
  restart (`:K1 :G111 :I1 :J1`, sidereal period written and read back correctly) sent 7 ms later
  left the RA axis running at ~2x sidereal for the rest of the session (raw `:j1`: 1251 counts in
  the 6 s of a Dec-only pulse, 9716 counts in the 47 s to FindHome). ConformU saw it as
  `PulseGuide +9.0 North` "East-West movement outside tolerance, RA change -5.68 s". Not
  reproducible on demand (five targeted attempts incl. the identical slew shape). Driver now: a
  slew is complete only when the axis reads stopped AND two `:j` reads 60 ms apart agree
  (`wait_axis_stationary_locked`, also between refinement gotos); `Slewing` stays true until
  tracking is restarted; the restart is rate-checked over 300 ms and redone once with a WARN
  ("Post-slew tracking restart: RA axis running at N counts/s") if off by >25%. The check is
  skipped, with an INFO naming the count, when the window cannot accumulate 4 counts: ":j" is
  whole counts and both reads truncate, so below that every possible reading lands outside the
  tolerance and the check would condemn a healthy axis (an effective RA rate near zero, e.g.
  RightAscensionRate ~0.9 nearly cancelling sidereal, is the way in). Grep for that WARN if a
  2x ever recurs, and for "rate check skipped" if a slew was never verified: **every exit that
  does not complete a measurement logs that phrase** -- the entry guards, the zero-rate and
  zero-interval guards, both `sleep_unlocked()` supersession exits, the three exits inside the
  attempt-0 recovery (the stop-wait losing the axis, tracking going off while it settled, and the
  restart itself throwing), and the two catch blocks (a position read throwing mid-window, and
  the caller's catch around the whole check, which fires when `stop_axis_and_wait_locked()`
  throws inside the recovery; a `check_connected()` throw out of the sample sleep lands in the
  position-read catch). Review of this branch found three of those silent, including one that fires
  with the RA axis already stopped by the check's own stop, and a second review found the three
  exception paths silent too. A check that RAN and found the rate correct logs
  nothing -- that is the ordinary case, once per goto, and the grep is for slews that were
  never verified, not for slews that passed. Power was a suspect (mount fed from an SVBONY SV241's 12 V rail; the
  event followed a 26 s full-speed slew) but was not proven.
- **A retry loop whose supersession test compares against a generation captured before the loop
  can only ever run once.** Review of the branch above: the rate check's second sample was
  unreachable, because the attempt-0 recovery is itself a motion command (its own
  `++motion_generation_`, and `start_speed_motion_locked()` bumps it again), so attempt 1's first
  re-lock read the check's OWN restart as another command's supersession and returned. The
  "restart did not correct it" WARN could never be emitted, and a restart that also latched wrong
  ran at the wrong rate in silence -- the exact failure the check exists to surface. Fixed by
  re-seeding the entry generation from the recovery's own restart. **Rule:** whenever a loop both
  issues a motion command and guards itself with "has the generation moved", the guard's baseline
  has to be re-established after each of the loop's own commands, or every iteration after the
  first is dead code. The tell is a `const` generation captured outside the loop. Pinned by a
  case arming two bad latches instead of one (`restart_tracking_at_wrong_rate(1, 2)`) and
  asserting the second-attempt WARN.
- **One flag cannot answer two questions, and "Slewing" is not "the axes are busy".** Holding
  `goto_in_progress_` across the post-slew restore was the right fix for the Slewing half (a
  client must not fire motion into the restart window) and a regression for the other: that
  same flag feeds `axes_busy_locked()`, which the rate setters read as "a goto owns the axes,
  its restore will re-apply this when it releases them". The restore had already run. A
  `RightAscensionRate` write landing in the window returned 200, read back the new value, and
  was never driven. Fixed with a second flag, `restoring_tracking_`, that
  `get_slewing_locked()` consults and `axes_busy_locked()` does not. **Rule:** before widening
  the span of a state flag, list every predicate that reads it and check each one still wants
  the wider span. Here `get_slewing_locked()` did, `axes_busy_locked()` did not, and the
  duty-cycle worker's start gate did -- so it names the new flag explicitly, because a burst
  in that window would bump `motion_generation_` under the rate check's supersession guard and
  make it skip. Same family as the per-axis `axes_busy_locked()` finding in #432: a predicate
  that bundles several questions eventually gets asked the one it answers wrongly.
- **A measured estimate needs a test that the estimate MOVES, not that it helps.** The
  constants-to-EMA change (`goto_overhead_seconds_`, `resume_latency_seconds_`) shipped with
  nothing pinning it: delete both update blocks, re-seed from the constants, suite still green.
  The estimates are private, so the observable is the thing they steer -- the RA landing
  residual, which is pure aim-ahead error since Dec has no time term and lands exactly on
  target every slew. Over five identical slews the residual spread is ~9.3 arcsec measured and
  ~0.13 arcsec frozen, stable to +/-0.1 across runs; the case asserts a 2 arcsec floor.
  **And the seam disagrees with the hardware about which is better**: on the loopback fake the
  frozen constants land at about -0.8 arcsec and the measured EMAs at -3 to -13, because the
  fake has no equivalent of the real MC's ~3 s floor on even a 350-count refinement goto --
  which is precisely the fact that made the constants wrong on an EQM-35. So the test pins that
  the aim-ahead is driven by something that moves, and says in its own comment that the
  evidence measuring HELPS is the hardware ConformU run, not the fake. **Rule:** when a fake
  cannot reproduce the quantity a change was made for, pin the mechanism and name the real
  evidence in the test, rather than asserting an improvement the fake will contradict.
- **Test seams have to model the failure, not a nearby one.** The landing-settle wait
  (`wait_axis_stationary_locked`) shipped with nothing in the suite failing without it, and the
  ramped-`:K` seam that looked like it should cover it could not: a ramped stop keeps `:f`
  RUNNING for the whole ramp, which the ordinary stop-wait already handles, so the stationary
  check had no window left to close. The real window is the one the hardware showed -- `:f`
  clearing while the last counts still arrive -- and it needed its own seam (`land_short_by()`:
  report the landing stopped N counts short, then creep the remainder in). Goto counts could not
  be the signal either (`refine_goto_landing()` burns all three iterations on this fake whether or
  not a landing coasts), nor wall-clock timing (the 3 s `slew_force_until_` window and the
  tracking restore both sit between the landing and `Slewing` clearing). What works: coast for
  longer than `kLandingSettleTimeout` and assert the check's own give-up WARN, a string nothing
  else emits. **Rule:** before claiming a change is covered, delete it and run the suite; if it
  stays green, the seam models the wrong failure.
- **Goto aim-ahead constants are rig-specific: measure them.** `kGotoRampSeconds` (2.5 s) and
  `kTrackingResumeSeconds` (0.7 s) were tuned on the Wave 100i. On the EQM-35 the landing-to-`:J1`
  restart takes ~0.2 s and even a 350-count refinement goto ~3.1 s (the MC's minimum goto time),
  so 2.5 + 0.7 happened to equal 3.1 + 0.2 for refinements (which is why they landed to 0.6 arcsec)
  while a 20 deg goto whose estimate ran 1.2 s long read as 6 arcsec off at the deadband check and
  resumed tracking 1.03 s ahead of the sky (`SyncToCoordinates` "15.4 arc seconds away", exactly
  1.03 s of RA). Fixing only the restart latency made every refinement land 0.6 s late
  (`SlewToCoordinates` "10.8 arc seconds away"). Both are now EMAs measured per goto
  (`goto_overhead_seconds_`, `resume_latency_seconds_`), seeded from the constants so the first
  goto of a session is unchanged on every mount; the refinement loop remains the safety net.
- **Run chrony on the machine running ConformU. A stepped clock is an RA error.** RA = LST - HA
  with LST from the host clock. Lima's host agent steps the guest clock by ~100 ms whenever the
  drift passes its threshold (every 2-3 min at the ~500 ppm a vz guest drifts; no knob in Lima
  2.2.0), and `systemd-timesyncd` does not correct frequency. Every 10 s rate-offset measurement
  or Dec pulse that spans a step fails by exactly 0.1 s of RA: `RightAscensionRate Write`
  -0.0136 vs -0.0033 s/s (twice, at different hour angles), `PulseGuide +3.0 South` 0.10 s
  east-west; the raw RA counts were exactly sidereal both times and the step timestamps in
  `~/.lima/<vm>/ha.stderr.log` ("guest clock adjusted") sat inside each measurement window.
  `apt install chrony` (fast poll: `minpoll 3 maxpoll 5`, one `chronyc makestep`) holds the drift
  at ~65 ms with no steps; the passing run had none. `/conformu` Step 2f2 now requires chrony.
- **ConformU's `-9.0 / +9.0 / -3.0 / +3.0` test labels are hour angles.** The extended
  rate-offset and pulse-guide tests slew to HA -9, +9, -3 and +3 h and repeat each measurement
  there; a failure at one label and not another is position/timing-dependent, not a sign flip.
- **VirtualHere for the USB pass-through** (Lima vz has none): the free server refuses `USE`
  from a client started with `-n` ("running as a service"); run `vhclientarm64` without `-n`.
  The client needs `vhci-hcd`, which Debian's `cloud` kernel lacks -- install `linux-image-arm64`.

#### Alignment with upstream issue #230 (EQMOD-style direct motor-controller support)

open-astro/AlpacaBridge#230, filed by the maintainer, asks for exactly the work in this
section: generalizing the Wave driver to classic Sky-Watcher/Orion EQ mounts (HEQ5, EQ6,
EQ6-R, AZ-EQ6, EQ5 Pro, etc.) via EQDIR cable, with no hand controller in the loop. Status
against its checklist, 2026-09-06:

- [x] Model/feature detection via `:e`/`:q` — done (mount-code table, feature-word gating).
- [ ] Board-capability gating for PPEC, dual-encoder, WiFi, and the polar-scope LED per the
  issue's list — only the home-index bit (`0x04`) is actually consulted so far.
- [x] CPR/high-speed-ratio/timer-freq read from the board, not hardcoded for Wave —
  confirmed: EQM-35 Pro geometry (CPR 9,216,000, timer 16 MHz) differs from the Wave
  (4,147,200 / 14 MHz) and the SAME driver code tracked correctly on it (0.99995x
  sidereal), so this was already correct, just unverified until now.
- [x] High/low speed mode switch threshold — already board-generic:
  `kFastModeThresholdDegPerSec = 128.0 * kSiderealDegPerSec`, derived from the MC
  protocol's universal 128x switchover, not a Wave-specific constant.
- [x] AutoHome/FindHome gracefully disabled without home-index sensors — hardware
  verified: the EQM-35's `0x7000` feature word has no `HOME_INDEXER` bit, and `FindHome`
  correctly takes the count-frame goto fallback rather than hunting a sensor that
  doesn't exist.
- [x] Naming/config: model auto-detected under the existing `vendor: skywatcher` key
  (no separate `eqmod` alias needed) — done, `get_name()` reports the real model.
- [x] **Auto-detect distinguishing an EQDIR cable from other vendors' PL2303/CH340/FTDI
  devices** — was a real gap: the enumeration scan and `connect_serial()` did not use
  `alpacacore/util/serial_port_registry.h` (the cross-vendor in-use registry originally
  built for WandererAstro, explicitly designed to generalize "across wrappers"). Fixed:
  both scan loops skip a port another connected device holds open, `probe_skywatcher_port`
  re-checks after `open()` for the TOCTOU window, and `connect_serial()` claims the port
  in the registry BEFORE opening it and releases it in `disconnect_locked()`. The gap is
  wider than this vendor: only WandererAstro (all four wrappers) and Gemini's PDH wrapper
  (`gemini_pdh_protocol_wrapper.cpp`) use the registry; synscan, ioptron, celestron, onstep
  and Gemini's focuser/flat-panel wrappers do not — only `skywatcher` was closed here, in
  scope for this issue. The claim/release in `connect_serial()` is covered by a pty-backed
  test in `test_skywatcher_serial.cpp`; the post-`open()` re-check in `probe_skywatcher_port`
  narrows the TOCTOU window but cannot close it (in-process best-effort set, not a file lock).
- [x] Pier side / meridian handling for GEMs in the southern hemisphere — open-astro#261.
  Audit (2026-09-09, no hardware): unlike the RA/Dec direction bugs above, the branch that
  drives `SideOfPier`/`DestinationSideOfPier` is chosen purely from the sign of hour angle
  in `ra_dec_to_axis_degrees_locked()`. Since #432 that function consults
  `hemisphere_south_locked()` twice -- `sky_sign` multiplies both `dec_mech` (the a2
  magnitude) and the `a1` term -- but still never for which branch is picked or which
  side it is labelled, which is the half this audit rests on. (Superseded in part by
  #458: the branch is now `k * side`, and `k = s * eps` does consult the hemisphere on a
  board with a measured sense; the side label is still the sign of HA alone, so the
  flip contract below is unchanged.)
  So the reported side already satisfies the ASCOM flip-with-HA contract (the same one the
  OnStep driver had to learn the hard way, see below) in both hemispheres by construction, and
  a loopback or ConformU check can only confirm that self-consistency — it cannot tell whether
  the "pierEast" branch is the true physical east side below the equator, because there is no
  internal contradiction to expose (whichever side the code calls pierEast, it consistently
  slews to and reports that side). Loopback regressions asserting the flip contract on the
  EQM-35 Pro and Wave profiles are in `test_skywatcher_async.cpp` ("Pier side across the
  meridian"). **Plate-solved on the rig 2026-09-24** (EQM-35 Pro at -37, TRACE log): gotos
  across the meridian and back, with the board's `:j` counts at every exposure. Every landing's
  dec branch was on the side of the meridian the solved hour angle puts it: `a2 < 0` for the
  east-side (HA < 0) targets IC 5148 and a Capricornus field, `a2 >= 0` for every west one, with
  `SideOfPier` reading 1 after the flip. A flip and flip back returned M7 to within 82 arcsec.
  Pinned as sky truth in `test_skywatcher_pointing.cpp` ("measured axes agree with the
  plate-solved sky across a flip, south").
- [x] `SyncToCoordinates` single-point offset sync model — exercised 2026-09-24 with plate
  solves: each sync was a pair of `:E` register writes with no motion, and later gotos on the
  sync's side of the meridian landed 0.2-0.7 deg from the sky. Across the meridian the error
  was 1.4-2.3 deg in Dec, which a single-point offset cannot remove: the mount's own dec zero
  and cone errors change sign with the pier side (see the absolute-pointing bullet below).
- [ ] Park/unpark weights-down convention — not specifically re-verified on a classic
  board this session (uses the same `kHomeCounts` convention as the Wave; untested here).
- [ ] ConformU 4.5.x on a classic mount — blocked on Pi 5 hardware availability; not the
  EQM-35 specifically, but the issue's ask applies equally.
- [x] Fake mount test double extended with a classic-board profile: `FakeMountProfile::eqm35_pro()`
  in `AlpacaCore/tests/fake_skywatcher_mount.h` is a REAL EQM-35 Pro capture (its built-in PL2303
  port answers only at 115200), used by the `[eqm35]`-tagged cases in
  `AlpacaCore/tests/test_skywatcher_async.cpp`: identity from the mount code, the count-frame
  FindHome fallback, the board's own sidereal period, and the `[hemisphere]` southern-hemisphere
  regressions that depend on its geometry and feature word (one `[eqm35]` case uses the Wave
  profile as the control). Counts are deliberately not stated here; grep the tag.
- [ ] A second classic-board profile (HEQ5 PRO / EQ6, 9600 baud over an EQDIR cable, older
  firmware string) — deliberately NOT added with invented numbers: fabricating a plausible
  profile without hardware to source it from would misrepresent guessed values as measured
  ones. HEQ5 PRO and EQ6 hardware is on hand via the `synscan` (hand-controller) driver
  validation (#7, #29); capture an actual reading from it over an EQDIR cable when available.
#### KNOWN BUG (FIXED): superseded MoveAxis stop task strands `Slewing` and kills tracking

Found on an EQM-35 Pro 2026-09-06, but **not hemisphere- or model-specific — the Wave
100i is equally affected.** Not caused by the southern-hemisphere RA fix; that change
only altered a rate sign and does not touch this machinery.

**Symptom.** After a sequence of `MoveAxis` presses, the driver reports `Slewing = true`
indefinitely while the axis is demonstrably stopped (`":f1"` running bit clear, `":j1"`
counts frozen), AND tracking is never restarted even though `Tracking` still reports
true. The mount sits motionless claiming to be both tracking and slewing. Reported RA
then drifts at 1.0x sidereal — the signature of a stationary mount — instead of holding.

This is the dangerous shape: a sequencer that waits for `Slewing` to clear before
exposing hangs forever, and one that does not wait images on an untracked mount.

**Mechanism.** `move_axis()` uses a SINGLE shared `stop_task_thread_` for both axes.
When a new stop supersedes a pending one, the old task is cancelled
(`stop_task_cancel_.store(true)`) and returns early from `task_wait_for()` — before
reaching `manual_axis_slewing_[axis] = false` and the restore-tracking tail. Its axis's
flag is stranded set, and `get_hardware_slewing_locked()` returns true forever because
it ORs both `manual_axis_slewing_` entries.

**Reproduction.** Drive MoveAxis on alternating axes with stops close together — CCDciel
issues MoveAxis pairs ~44 ms apart on button release (observed in the journal), which is
enough for the second stop to cancel the first axis's task. N, S, E, W in sequence
reproduced it reliably.

**Recovery (user-level).** `PUT moveaxis Axis=<n> Rate=0` on the stranded axis clears the
flag and restores tracking, because a stop on an axis whose flag is set spawns a fresh
task that runs to completion.

**Fix (done).** `stop_task_thread_` and `stop_task_cancel_` are now per-axis arrays;
`reap_stop_task(axis)` and the spawn/retry-join block only ever race with a prior task
for the SAME axis. A new loopback regression reproduces the exact scenario (RA stop
dispatched, Dec stop dispatched while RA's stop task is still mid-ramp) and asserts
`Slewing` clears promptly. The generation guard
(`motion_generation_ == stop_task_generation`) is unchanged and still gates the
tracking-restore tail — which is exactly what exposed the SECOND bug below.

#### KNOWN BUG (FIXED): cross-axis `motion_generation_` can block a same-axis tracking restore

Found while writing the regression test for the bug above, on the SAME night
(2026-09-06) — the per-axis stop-task fix is necessary but not sufficient. Not
hemisphere- or model-specific.

**Symptom.** With the per-axis fix in place, `Slewing` now clears correctly after
stopping both axes close together — but the RA axis can still fail to resume tracking
after a `MoveAxis(0, 0)` stop, even though `Tracking` reports true throughout. Caught by
a loopback test asserting `mount.axis_running(1)` becomes true again after the stop
settles: it does not, reliably, when a Dec-axis stop is dispatched while the RA stop
task is still polling.

**Mechanism.** The restore-tracking tail guards itself with
`motion_generation_ == stop_task_generation` — "only restore if nothing newer
superseded this stop." But `motion_generation_` is ONE counter bumped by every motion
command on EITHER axis (see its declaration: "bumped by every motion command"). A Dec
stop dispatched while RA's stop task is polling bumps the shared counter for a reason
that has nothing to do with RA, so the RA task's tail reads a mismatch and silently
skips restoring RA's tracking — even though nothing actually superseded the RA stop
itself (which is correctly detected via the now-per-axis `stop_task_cancel_[0]`, a
separate and correctly-scoped check).

The codebase already has the right idiom for this elsewhere: the duty-cycle worker
(`apply_ra_drive_locked`'s burst path, guarding sub-floor rate duty-cycling) computes a
`same_axis_owner` flag from `goto_in_progress_ || parking_ || homing_ || slewing_cached_
|| manual_axis_slewing_[i] || pulse_axis_active_[channel - 1]` before
trusting a generation mismatch as a real supersession, specifically BECAUSE "the global
generation cannot tell a same-axis supersession from an unrelated other-axis command"
(exact wording from that code's own comment). The MoveAxis stop-task tail does not apply
this idiom and should.

**Fix (done).** Applied option (a): the stop-task restore tail now computes a
channel-scoped `same_axis_owner` (`goto_in_progress_ || parking_ || homing_ ||
slewing_cached_ || manual_axis_slewing_[axis] ||
pulse_axis_active_[channel - 1]`), the exact idiom the duty-cycle worker already uses, and only
treats a `motion_generation_` mismatch as a real supersession when `same_axis_owner`
is true. The historical regression this guards against (PR #216 round-5:
`SetTracking(false)` racing the restore) is covered only for a COMPLETED
`SetTracking(false)`: that path publishes `tracking_ = false` under the SAME `mutex_`
this task also holds, so the `tracking_ &&`/`dec_rate_arcsec_per_sec_ != 0.0 &&` guards
ahead of the generation check see it. They do NOT cover one still IN FLIGHT.
`set_tracking_locked(false)` bumps `motion_generation_` first, then releases `mutex_`
inside `stop_axis_and_wait_locked()`'s poll loop, and assigns `tracking_` only after that
wait returns -- so a restore tail waking inside that window reads `tracking_ == true`,
restores the drive, and the caller throws `Tracking change superseded by a concurrent
motion command` with the mount left tracking. That is issue #535, still open; its
regression case is quarantined `[!mayfail]` in `test_skywatcher_async.cpp` (see #586 /
#587), so nothing gates this path until #535 lands.
Extended the regression test from the first bug to assert the RA axis actually resumes
running (not just that `Slewing` clears); confirmed it fails at exactly that assertion
with the fix reverted to the raw equality check, and passes with it restored.

#### KNOWN BUG (FIXED): `axes_busy_locked()` is the wrong question for a per-axis re-apply

Found in review of #432 (2026-09-12), the third instance of the same idiom in this
driver. Not hemisphere-specific in shape, only in trigger.

**Symptom.** `SiteLatitude` written across the equator while a North/South pulse or a
`MoveAxis` on the DECLINATION axis is in flight leaves the RA axis running the old
hemisphere's direction indefinitely -- stars trail at 2x, the #250 signature, with
nothing scheduled to correct it. Under an autoguider this is the common case, not the
corner: roughly half of a session's corrections are declination, and PHD2 holds
`pulse_axis_active_[i]` true for most of every guide cycle.

**Mechanism.** A setter that must re-command an axis skips when the axis is busy, on the
grounds that the busy operation's own restore path re-derives the value. That contract
holds per axis, but the guard asked `axes_busy_locked()`, which is true when EITHER axis
is busy. A declination pulse ends in `stop_axis()`'s final `else` branch -- it stops the
DEC axis and re-applies the DEC offset, and never touches RA. The `MoveAxis` stop task is
the same shape: its restore calls `set_tracking_locked()` only for `channel == kAxisRa`
and `apply_dec_rate_offset_locked()` only for `kAxisDec`. So both the setter and the
in-flight operation skipped RA, each expecting the other to do it.

**Fix (done).** `axis_busy_locked(channel)` is now the primitive -- the same
`goto_in_progress_ || parking_ || homing_ || slewing_cached_ || manual_axis_slewing_[i]
|| pulse_axis_active_[channel - 1]` idiom the duty-cycle worker and the
MoveAxis stop tail already use -- and `axes_busy_locked()` is defined as the OR of the
two, so every existing caller is unchanged. `set_site_latitude()` decides each half
separately: re-apply the RA drive unless RA is busy, re-apply the Dec offset unless Dec
is busy, and skip entirely only when both are. A sub-floor RA rate is pre-armed into
`ra_duty_rate_deg_s_` when RA is busy, the way `set_right_ascension_rate()`'s busy branch
already does, because the duty worker resumes from that stored rate and no restore path
re-derives it. Two loopback regressions (`test_skywatcher_async.cpp`) drive the
declination-pulse and declination-`MoveAxis` variants and were confirmed to fail on the
pre-fix setter with the RA axis still counting the old way.

**Rule for the next driver.** Any "skip while busy, the restore path will re-apply"
guard has to name the axis it is talking about, and the reviewer's question is always:
does the operation that owns the busy axis actually re-derive THIS value? A mount-wide
busy flag can only answer that when the operation owns every axis -- a goto, park, home
or slew does; a pulse or a manual nudge does not.

The audit found two more instances, both fixed with it, and it took three review rounds
to find all three -- each fix's own claim of completeness was what exposed the next one.
No hemisphere is involved in either:

- `set_right_ascension_rate()`: a declination operation in flight made the whole-mount
  predicate true and stranded the rate write with nothing scheduled to apply it, so a
  client's `RightAscensionRate` silently did nothing until the next re-apply.
- `set_declination_rate()`, which passes the predicate down as
  `apply_dec_rate_offset_locked(defer_motion=)`: an East/West pulse or an RA `MoveAxis`
  made it true while owning only the RA axis, and the pulse's `stop_axis()` restore
  rewrites the RA step period without ever calling `apply_dec_rate_offset_locked()`, so a
  continuous Dec offset was stranded the same way. Comet or satellite tracking while
  autoguiding is the way in. Only the continuous branch was affected: a sub-floor rate
  recovers on its own through the duty worker's start gate.

Every "skip while busy" guard in this driver now names its axis. When adding a new one,
grep for `axes_busy_locked()` and justify each remaining caller: the legitimate uses are
the ones asking "is the mount doing anything at all", such as the duty worker's
`connected_ && tracking_ && !axes_busy_locked()` start gate. **And do the grep before
writing the claim** -- this section asserted the sweep was complete twice before it was,
and each time the assertion itself was the review finding.

- **Hardware bring-up, EQM-35 Pro over the mount's built-in USB, 2026-09-06** (Raspberry
  Pi 3B, Debian 13 arm64, direct USB-A-to-B, no handset in the chain):
  - Pointing math at latitude -37.2: home points at the SOUTH celestial pole, so
    `dec = -90 + a2`. The session "verified" this against raw counts — reported HA
    matched axis 1 to 0.0004 deg, and alt/az recomputed from the reported RA/Dec matched
    to 4 decimal places. **That was self-consistency only**: the driver reports the model
    it commands, so those checks could not see that the RA-axis/hour-angle relation was
    missing its 6 h home offset and its southern sign (#432, found from a Wave 150i sky
    test in the north). The dec relation was right; the RA relation is now
    `HA = -(a1/15) ± 6` here. Treat every "reported coordinates matched" line in this
    section as a consistency check, not a sky check.
  - `MoveAxis` verified semantically in all four directions, not just for motion:
    each button was checked against the change in REPORTED RA/Dec. N: Dec +15.59
    deg, S: Dec -16.96 deg, E: RA +15.47 deg, W: RA -15.28 deg, zero cross-axis
    coupling in every case. **The two RA rows are stale as reported values**: they were
    read under the pre-#432 model, where `d(HA)/d(a1)` did not flip below the equator.
    The same mechanical button now moves reported RA the other way at this site. What
    the rows still establish is the mechanical fact, which way each button turns which
    axis; only the RA/Dec labels on them changed. `move_axis()` applies NO branch or hemisphere sign
    transform (the rate goes straight to `start_speed_motion_locked`), so this is
    also the hardware reference for which way a raw Dec-axis rate moves reported
    Dec below the equator -- the fact the DeclinationRate/PulseGuide fix below
    rests on. Reported coordinates come from the driver's own pointing model; the
    independent sky check (plate solve, 2026-09-24) is below. Do NOT "fix"
    MoveAxis to follow sky Dec: the ASCOM spec says the sign of the Rate parameter
    "is purposely left undefined" and the motion is about the MECHANICAL axis, so
    the no-transform behaviour is correct in both hemispheres (checked against
    ascom-standards.org/newdocs/telescope.html#Telescope.MoveAxis, 2026-09-06).
  - **Tracking rate measured at 0.99995x sidereal over 5 minutes** (-46 ppm,
    -2.5 arcsec/hour, against a +/-31 ppm encoder-quantisation floor), Dec drift
    exactly 0 counts. Ten consecutive 30 s intervals of -3214 counts, +/-1. The
    magnitude stands; the DIRECTION that session settled on (counts up, #250) was
    reversed by #432: below the equator the counts must go down, which is what the
    original code did and what indi-eqmod does. The "2.007x sidereal" that condemned
    it was the reported RA of the old model, not the sky.
  - **Technique worth reusing:** the protocol wrapper does NOT log individual
    commands, so do not plan to read step periods out of the journal. Sample
    `":j1"`/`":j2"` through the Alpaca `commandstring` passthrough instead and
    differentiate — that measures what the mount ACTUALLY does rather than what it
    was told, and needs no rebuild. Expected sidereal counts/s = `CPR * 360.98564736629
    / 86400 / 360` (106.959 on this mount). Make sure nothing else is driving the
    mount while sampling; a manual slew mid-run silently corrupts the result.
  - Note this validates driver -> board -> encoder counts. It validates counts -> SKY
    only if the gear ratio matches what the firmware's `":a"` assumes; a belt/pulley
    mod that changes the reduction would track perfectly in counts and still drift on
    sky. (Confirmed ratio-preserving on this unit.)
  - **Dec-axis direction of `DeclinationRate` / `PulseGuide` North-South below the
    equator: MEASURED on the mount 2026-09-06** (build `d29d650`, Pi-native arm64 build,
    daylight, OTA mounted). Method: `Connected=true`, tracking on at sidereal, Dec axis
    first offset +0.51 deg from home with `MoveAxis` so a2 > 0 -- **do not run this test
    from the home position: at a2 = 0 (Dec -90) reported Dec rises for EITHER mechanical
    direction, so the pass/fail signature is invisible there.** Reported Dec and raw
    `":j2"` counts (via `commandstring`) sampled around each command:
    PulseGuide North 5000 ms -> Dec +37.7" / +268 counts (expected +37.6" / +267 at the
    default 0.5x sidereal guide rate); South -> -37.7" / -268, net 0.
    `DeclinationRate` +5"/s for 60 s -> +302.9" / +2159 counts (expected +300" / +2133,
    the excess is the ~60.7 s wall time); -5"/s -> -299.7" / -2144; rate 0 -> 0 counts of
    drift in 30 s. Both call sites of the KNOWN BUG fix below are confirmed on the a2 > 0
    branch; the a2 < 0 branch rests on the loopback tests only -- see the PENDING BENCH
    TEST below, which reaches it WITHOUT a real meridian flip. Same session: reported RA
    held constant to 1e-5 h over ~90 s of tracking (a self-consistency result only: the
    "RA tracking-direction fix" it was read as confirming is the #250 removal that #432
    reversed),
    and `MoveAxis(Dec, +rate)` again moved reported Dec and the counts up. Mount returned
    to home, tracking off.
  - **Absolute pointing and the southern meridian flip: plate-solved 2026-09-24.** Same
    EQM-35 Pro at -37, 5 s frames solved by ASTAP and precessed to date, the service at TRACE
    so every `:S` goto target, `:j` read and `:E` sync is in the log. The driver sent exactly
    its formula's axes on the 12 gotos that have a solve (a2 to 0.000 deg, a1 within the
    seconds between computing and logging the frame), and the board landed on the commanded
    count (spot-checked in the log on two gotos). Against the sky, the plain model is up to 0.43 h and 4.6 deg off; a
    seven-term fit to 15 solved exposures over two power-ons leaves 8 arcmin rms with polar
    1.3 deg, cone 0.8 deg, a hand-homed dec zero 0.8 and 3.3 deg off and an RA zero per
    power-on. Those are this rig's errors; the model has no terms for them, so the practical
    fix is a sync on each side of the meridian. A 23-minute tracking run drifted Dec
    -11.2 and RA +9.9 arcsec/min against -8.8 and +12.1 predicted by the fitted polar error;
    they agree within about 2.5 arcsec/min, so no separate tracking-rate error is needed. Four same-Dec gotos turning only the RA axis
    lay on one circle to 8 arcsec. The rows are pinned in `test_skywatcher_pointing.cpp`.
  - STILL UNVALIDATED on EQ-class hardware: the
    `":g"` high-speed ratio under fast slews, and the Dec-axis direction of
    `DeclinationRate` / `PulseGuide` North-South below the equator on the a2 < 0 branch (fixed in code from
    the pointing model -- see the KNOWN BUG below -- but not yet measured on the mount;
    a short autoguiding session is the cheapest check).
  - **PENDING BENCH TEST (not yet run): `a2 < 0` Dec-direction sign coverage.** Closes the
    gap above. Key realization (2026-09-07): `a2` is the raw Dec-axis angle relative to
    home (see `compute_ra_dec_locked()`) and is NOT coupled to the RA axis at all, so the
    `a2 < 0` branch does not require an actual GOTO across the meridian -- the same
    `MoveAxis` bench technique already used for `a2 > 0` reaches it directly, mirrored:
    1. `Connected=true`, tracking on at sidereal, OTA mounted, daylight is fine (same
       setup as the `a2 > 0` session, 2026-09-06).
    2. `MoveAxis` the Dec axis to roughly **-0.5 deg from home** (the OPPOSITE direction
       from the `a2 > 0` session's +0.51 deg) so `a2 < 0`. Do NOT start from `a2 = 0`
       (Dec -90): reported Dec rises for either mechanical direction there, so the
       pass/fail signature is invisible right at home -- same caveat as the `a2 > 0` run.
    3. Sample reported `Declination` and raw `":j2"` counts via the `commandstring`
       passthrough around each command (same technique as the `a2 > 0` bring-up notes).
    4. `PulseGuide` North 5000 ms -> expect reported Dec to RISE; South -> back to
       baseline, net 0 counts.
    5. `DeclinationRate` +5"/s for ~60 s -> expect Dec rising roughly 300" (accounting for
       actual wall time as in the `a2 > 0` run); -5"/s -> back down; rate 0 -> no drift in
       30 s.
    6. Compare signs against the fix's table: on the `a2 < 0` (west) branch, southern
       sites should NEGATE (previously wrongly kept) and northern sites should KEEP
       (unchanged) -- the mirror image of the `a2 > 0` row already confirmed.
    This closes ONLY the sign-rule coverage gap. It does NOT validate `SideOfPier`
    reporting or automatic pier-flip behaviour during a real GOTO across the meridian --
    that was the separate plate-solved check above (2026-09-24), which needed an
    independent sky check to confirm a flip landed correctly.

#### KNOWN BUG (FIXED): DeclinationRate and PulseGuide North/South run backwards south of the equator

Found by static review on 2026-09-06 while auditing the hemisphere-conditional code
after the RA tracking-direction fix above -- NOT on hardware. Not model-specific: any
Sky-Watcher mount on this driver at a southern site was affected; northern sites never
were.

**Symptom.** Below the equator, a positive `DeclinationRate` drives the reported
Declination DOWN, and a `PulseGuide` North pushes the star further south. For an
autoguider this is the dangerous shape: every Dec correction lands on the wrong side, so
the guide loop diverges instead of converging. Magnitudes were always right, only the
direction was wrong -- exactly the signature of the RA bug above, which is why a
rate-only check never caught it.

**Mechanism.** Both `apply_dec_rate_offset_locked()` and the North/South branch of
`pulse_guide()` chose the axis direction with the plain rule "a2 >= 0 -> negate the
rate", derived from the northern pointing formula `dec = 90 - a2` (so d(dec)/d(a2) = -1
on that branch). But `compute_ra_dec_locked()` mirrors Dec below the equator
(`dec_sky = -(90 - a2) = a2 - 90` on the same branch), which flips the sign of that
derivative. Neither call site consulted `hemisphere_south_locked()`, so south of the
equator the rule was backwards on BOTH dec-axis branches. The full sign table:

| Site      | a2 >= 0 (east branch) | a2 < 0 (west branch) |
|-----------|-----------------------|----------------------|
| Northern  | negate (was correct)  | keep (was correct)   |
| Southern  | keep (was: negate)    | negate (was: keep)   |

**Why it survived.** The Wave 100i was ConformU-validated in the northern hemisphere,
where the rule is right, and the ConformU measured-rate tests that "confirmed" the sign
ran there. The southern-hemisphere hardware session (2026-09-06) exercised tracking and
`MoveAxis` -- and `move_axis()` applies no sign transform at all, so it was never
exposed to this rule.

**Fix (done).** Both call sites now negate when
`(branch_from_axis_locked(a2) > 0) != hemisphere_south_locked()` (XOR), which reproduces the
table above (the reader is the plain `a2 >= 0` sign outside the sub-arcsecond deadband at
the pole, see #459). Two loopback regressions on the EQM-35 Pro
profile at latitude -37.2 assert the ASCOM contract against the driver's own pointing
model -- reported Declination RISES under `+DeclinationRate` and after a North pulse --
and both were confirmed to fail before the fix (axis moved -19.97 arcsec and -11.25
arcsec respectively, the exact mirror of the passing northern-hemisphere cases). The
existing northern-hemisphere tests are untouched and still pass.

**Still open.** This is validated against the pointing model and the loopback
simulator, not measured on the mount. The cheapest hardware confirmation is a short
autoguiding session (PHD2 calibration reports the Dec direction directly) or a
plate-solved drift run with a non-zero `DeclinationRate`. Do this before ConformU: the
suite's offset-rate tests measure the Dec direction and will fail on the old code at a
southern site.

### Sky-Watcher serial recovery tests (PR #522)

Each direct-driver instance owns its protocol wrapper; never use the legacy singleton
from a driver operation or worker. A second configured mount must not replace the first
mount's transport. Both HTTP disconnect endpoints deliver last-client cleanup even when
`get_connected()` is already false: link health is not proof that runtime state was reset.
A cable-pull followed by an HTTP connect tests the first relink branch because the router
probes link health first. The late-loss branch needs a deterministic probe seam and an
outstanding fake-board task; assert that the old task never sends commands to the new link.
Zero-byte reads are not themselves proof of removal; back off within the response deadline
so a hung-up-but-present tty cannot busy-spin, while retaining the quiet-board timeout policy.
