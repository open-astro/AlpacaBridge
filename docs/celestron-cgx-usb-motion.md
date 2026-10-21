# CGX direct-USB larger-movement lab evidence

## Rig and scope

Session: 2026-10-13 local / 2026-10-14 UTC. Bare northern CGX, both motors
7.17.25 / model 0x1788, direct 115200-baud USB, native arm64 RPi4. The operator
authorized larger motion and meridian flips after the earlier bounded tests.
Targets stayed at Dec +60 to +85 degrees and HA +/-1 or +/-3 hours; each completed
sequence returned home with tracking/guiding off. These are encoder-derived
coordinates and API state, not optical pointing or southern/CGX-L qualification.

Build provenance at this historical checkpoint was `driver/celestron-cgx-usb`,
local committed identity checkpoint `5452f384` plus then-pending fixes below.
The Pi checkout's git metadata predates copied source updates; do not identify
the run from that metadata alone.
The final installed/server-build SHA-256 for that rig session matched:
`b1ea1bf0f4524f4af7319a834a1fce8134d27fd561e383ed6b2c2f7ee5007606`.

## Completed rig checks

| Check | Result |
| --- | --- |
| Home to HA +3 / Dec +75 | Nominal 45-degree RA / 15-degree Dec travel; arrival ~14.5 s; projected RA error 0.01355 degrees; Dec error 0.0000072 degrees |
| Home to HA +1 / Dec +85 | Nominal 75-degree RA / 5-degree Dec travel; arrival ~20.7 s; side 0; projected RA error ~0.00730 degrees |
| HA +1 to -1 / Dec +85 | Hardware-home route, then BeyondPole target; ~52.7 s; side 1; tracking restored |
| HA -1 to +1 / Dec +85 | Reverse home route; ~50.8 s; side 0; tracking restored |
| BeyondPole North, North, South, South (1 s each) | +8.41999, +7.57027, -9.73320, -7.49302 arcsec; expected +/-7.52055; follow-ups within one encoder count, reversal excess retained |
| BeyondPole East / West (3 s each) | -22.25581 / +22.56916 arcsec relative to sampled tracking baseline; expected +/-22.56165; segmented pulses completed |
| HA +3, +1, -1, -3 / Dec +60 grid | All four arrivals and branch checks passed; projected RA error 0.00102, 0.01830, 0.00100, 0.01835 degrees; Dec error 0.0000072 degrees |
| Larger immediate Sync followed by both flip directions | Passed after post-label settling fix; see [Sync evidence](failures/0015-cgx-sync-settling.md) |
| Abort after 2 s of a large slew, then Park/Unpark | Verified idle after recovery fix; AbortSlew 1.2626 s; Park ~16.1 s; see [stop evidence](failures/0014-cgx-stop-completion.md) |
| Abort 2 s into a flip's home route | AbortSlew returned and Slewing read false, but coordinates remained unknown; cleanup stop/tracking-off succeeded and FindHome restored the reference in ~32 s |

The +60-degree grid exercised 30-degree Dec travel, same-branch RA movement,
the opposite-branch home route, and RA wrap through zero hours. The existing
arrival verifier corrected larger projected RA undershoot when necessary; no
tolerance or model offset was changed to obtain these results.

BeyondPole Dec polarity now has northern rig evidence. Its first/reversed pulse
excess remains unresolved; these figures must not be summarized as exact guide
accuracy. RA comparisons use the measured approximately 14.9607 arcsec/s baseline,
not an independent sky measurement.

The home-route abort was not a reference-preservation pass: its first subsequent
RA read correctly refused with `encoder reference is unknown; run FindHome`.
No unknown coordinate was substituted. A successful stop alone does not prove
the reference survived every aborted home-route/error path. Recovery by fresh
FindHome was verified; reference preservation on that path remains unqualified.

## Software verification

- Native arm64 Release all-vendor Core: **2102 distinct CTest cases passed**.
  The completed 1226-case prefix was retained; the 876-case remainder ran at
  the repository's documented eight-way test parallelism. Combined case IDs
  cover 1 through 2102 with no omissions or failures.
- Native arm64 Release all-vendor HTTP: **16/16 CTest cases passed**.
- Focused CGX ordinary and UBSan runs: **64 cases / 753 assertions passed** each.
  UBSan instrumented the Core and Celestron libraries and the test/fake sources.
- Native arm64 TSan: both CGX lifecycle/destruction cases passed, **2 cases /
  5 assertions**, with the same libraries and test/fake sources instrumented and
  no vendor-blob suppression. The earlier `[cgx][stress]` invocation matched no
  cases; the actual verified run used `[celestron][stress]`.
- clang-format 18, cppcheck 2.17.1, native clang-tidy on both changed production
  files, Unicode, changelog, documentation drift, layering, connect-error,
  stress-registration and contract-sweep checks passed. The analyzer's blocking-
  read warning was a false positive: the owned fd is opened O_NONBLOCK and its
  mode is checked before use; a narrowly explained NOLINT covers that read.
- This is not a full `ci_preflight.sh` pass or an all-vendor sanitizer pass.
  The focused sanitizer coverage and the ASan host limitation remain distinct.

Final live reads verified Connected and AtHome true; AtPark, Slewing, Tracking
and IsPulseGuiding false; Dec +90, side 0. The original site and 50% Dec guide
setting survived the tests; WARNING logging was restored and chrony remained
synchronized. At the time of this checkpoint, source/evidence changes were
uncommitted and unpushed; they have since been committed to this PR.

## ConformU first-issue run

With operator approval, chrony 4.6.1 replaced systemd-timesyncd. Before the run,
chrony reported Normal leap state, nonzero update interval and approximately
0.58 ms system offset. ConformU was **4.5.1, build 54507.3929333**, running locally
against telescope 0. The broader compiler job was paused during this run.

The first-issue watchdog stopped the run at:

```text
Altitude ISSUE Unexpected error: Celestron CGX encoder reference is unknown; run FindHome
```

Azimuth, Declination, RightAscension and SideOfPier produced the same refusal
within the watchdog's 200 ms polling window. Tracking Write also refused:
`Run FindHome before enabling Celestron CGX tracking`.

ConformU's connection tests reset the reference before its property tests. The
driver used for this run intentionally constructed/connected without homing or assuming
retained encoder labels, so a pre-run FindHome does not survive that reconnect.
This run is **not a conformance pass**. It established the need for an explicit
reference-lifecycle design before qualification; do not silence these errors,
return guessed coordinates, or introduce unannounced physical homing on Connect.
Failed/partial output remains outside the committed conformance-report directory.

Cleanup verified home, tracking off and restoration of the original WARNING log
level. ConformU did not reach its large-method choreography in this attempt.

## Stages 1–2: software and approved rig evidence

After user-approved deployment, the main rig operator ran stages 1–2 on the
same northern CGX. The previous session used SHA-256
`b1ea1bf0f4524f4af7319a834a1fce8134d27fd561e383ed6b2c2f7ee5007606`;
the initial stages 1–2 deployment was reported with hash prefix `c3c3d5e`.
The stop-budget fix and retest used the then-deployed binary SHA-256
`a381d1a5156d76ee4c5524793f3a0d3729a85a417f3987f9a27c429e0132b744`.
These are binary hashes, not proof from the Pi checkout's stale git metadata.
This documentation update uses the operator's stage 1–2 and retest logs, current
source/tests and reported software results, plus the full-speed stop and stage 3
guide logs below; it performed no rig access or deployment. Unless linked to a
published report, log-derived results here are operator-reported summaries;
their raw logs are not publicly attached.
The latest section records four stopped ConformU attempts followed by the
completed run 5 pass, with its exact hardware/build scope and remaining limits.

- **Stage 1 — logical reference continuity:** retain the exclusive fd across
  successful logical disconnects; revalidate identity/firmware and stopped,
  settled encoder checkpoints within two counts on the same session. Transport
  errors/recovery invalidate calibration. Connect does not home or write labels.
  Idle site coordinates remain mutable; FindHome captures the mechanical
  hemisphere independently of later SiteLatitude writes.
- **Stage 2 — asynchronous stop ownership:** AbortSlew/Unpark dispatch a stop
  worker that drains motion/guiding and verifies completion. Slewing stays true
  while pending, AtPark is cleared only on successful Unpark completion, and
  failures remain visible. Cancelled workers skip duplicate stops owned by the
  explicit stop worker; tracking restoration requires a valid reference. Only
  the explicit async stop worker now uses a **3 s** valid-busy verification
  budget; other cleanup callers retain the default **500 ms**. Transaction
  timeouts and active-guide-packet draining are separate bounds.
- **Reported native arm64 software results:** `[cgx]` passed **75 cases / 1485
  assertions**. Normal and TSan stress each passed **six repeats**, each repeat
  **2 cases / 5 assertions**, with no unexpected exceptions. These exercise the
  production driver over the PTY fake, not physical motors.
- Before the tracking race fix, stress failed in the actual `get_tracking()`
  path because it observed the temporary unknown state between stop writes.
  The driver mutex now covers the complete stop ACK pair and tracking-state
  publication; the expected-exception set was not widened to hide that failure.
- **Earlier native software verification:** normal, UBSan and TSan each passed
  **75 cases / 1486 assertions**, plus **2 stress cases / 5 assertions**. The
  complete native Core CTest suite passed **2115/2115** and HTTP passed **16/16**
  before the final optional-access cleanup and hard-link-loss release refinement;
  the focused checks above were repeated after those refinements. This full-suite
  result also predates the final async-stop budget change and was not repeated
  after that change. No sanitizer
  diagnostics were emitted. ASan remains unavailable on this Pi kernel.
- Changed-line clang-format 18 and clang-tidy 18, production cppcheck 2.17.1,
  Unicode, documentation drift, layering, stress registration, connect-error,
  contract-sweep registration and changelog validation pass. This is not a full
  pre-flight pass; cppcheck's test-file scan could not parse the Catch2 macros.
- **Mutation evidence:** five isolated native UBSan mutants were killed by the
  intended regression: dropping the retained reference, ignoring the transport
  generation, deriving mechanics from mutable latitude, dispatching duplicate
  worker stops, and making the stop initiator wait. Each unmodified baseline
  passed first. This is fake-backed evidence, not a physical continuity proof.

### Stage 1–2 hardware sequence and stop-budget retest

The initial log records home-reference reconnect, Sync-reference reconnect via
both synchronous and asynchronous paths, site round-trip and both flip arrivals
before the abort failure. AbortSlew returned in **4.335 ms**, pending true, but
`Slewing` failed approximately **0.5868 s** later with
`RA slew_done=0 home_done=1 guide_active=0` (ErrorNumber 1280). The explicit
worker still used the 500 ms cleanup budget. The main operator recovered with
FindHome and recorded cleanup complete: home true, tracking false, slewing false.

Current source gives only `run_stop_operation()` a 3 s verification budget. The
new regression, `Celestron CGX USB Telescope Driver - async stop verifies deceleration beyond the cleanup budget`,
scripts about 1.2 s of valid busy status for both AbortSlew and Unpark, requiring
prompt dispatch and exactly one stop per axis. The operator reports that the old
500 ms native baseline falsified it. After the change, reported native normal,
UBSan and TSan focused runs each passed **76 cases / 1507 assertions**, plus
**2 stress cases / 5 assertions** each. These are software results, not rig runs.

The retest log and operator report record:

| Check | Result |
| --- | --- |
| Home reference across logical reconnect | Passed; initial FindHome ~2.278 s |
| Sync reference across sync and async reconnect | Both passed; Dec +85.0999904 degrees |
| Exclusive retained link | Operator reports competing non-root open refused with EBUSY across logical reconnect |
| Idle site changes | Latitude-sign change and longitude round-trip passed; not southern hardware qualification |
| Both meridian flip directions after Sync | Passed; arrivals ~52.327 s and ~49.908 s; projected RA errors ~0.00735 and ~0.00727 degrees; Dec error ~-0.0000024 degrees |
| AbortSlew dispatch and completion | **4.014 ms** call, pending true; logged completion **0.16175 s**, tracking true, Dec +89.9983263 degrees |
| Unpark | Passed; initiator **3.588 ms** |
| Final cleanup | Home true, tracking false, slewing false |

**Abort scope remains limited:** this retest began near the pole after FindHome
and may still have taken the flip's home route. It does not establish a
full-speed same-branch RA abort pass or demonstrate 3 s of physical deceleration.
The subsequent true same-branch case starting at Dec +85 is recorded below.
The local near-pole retest's `PASS` applies to its sequence, not to the full
guiding suite or ConformU.

### Full-speed same-branch stop repeat

The main operator recorded an approved follow-up on the `a381d1a5` binary.
Both operations passed during
full-speed same-branch motion from the Dec +85 setup:

| Operation | Initiator | Completion | Pending at return | Dec before → after | Tracking after stop |
| --- | --- | --- | --- | --- | --- |
| AbortSlew | **4.822 ms** | **1.261968 s** | true | +79.2139363 → +76.8061709 degrees | true |
| Unpark | **3.944 ms** | **1.282701 s** | true | +79.2142797 → +76.8065357 degrees | true |

These initiators pass the one-second response target in this rig check;
physical stop completion is asynchronous and takes about 1.26–1.28 s. The older
**1.2626 s blocking AbortSlew** result remains historical evidence for the
synchronous path, not a timing failure of these new initiators. Final cleanup
recorded home true, tracking false and slewing false. This is not a ConformU
timing verdict or a guarantee for every motion/load or guide-cancellation path.

The [continuity decision](decisions/0011-cgx-logical-reference-continuity.md)
defines the source limits: no reset-generation counter means matching firmware,
status and encoder labels do not prove reset-safe continuity or HC/AUX
coexistence. These bounded rig reconnect checks do not prove reset-safe
continuity, persistence across restart, or HC/AUX coexistence, and do not turn
the earlier first-issue ConformU run into a pass.

## Stage 3: northern Normal-branch Dec guide evidence

The approved operator sequence at Dec +85 / side 0 used 50% and 100% sidereal
Dec guide rates, each in order **N1, N1, S1, S1, N5, S5** (seconds). The operator
recorded JSON samples and a raw serial trace. Both blocks measured
zero Dec drift over their two-second baseline. The following values are parsed
from the JSON, in arcseconds; error is **measured minus signed expected**.
Immediate means the first inactive sample; settled means the final tail value.

| Rate | Pulse (sequence order) | Expected | Immediate | Immediate error | Settled | Settled error |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 50% | N1 first | +7.52055 | +8.26550 | +0.74495 | +8.18825 | +0.66770 |
| 50% | N1 repeated | +7.52055 | +7.49302 | -0.02753 | +7.49302 | -0.02753 |
| 50% | S1 reversal | -7.52055 | -10.35118 | -2.83063 | -10.35118 | -2.83063 |
| 50% | S1 repeated | -7.52055 | -7.49302 | +0.02753 | -7.41577 | +0.10478 |
| 50% | N5 | +37.60275 | +38.31482 | +0.71207 | +38.23757 | +0.63482 |
| 50% | S5 | -37.60275 | -38.16032 | -0.55757 | -38.08308 | -0.48033 |
| 100% | N1 first | +15.04110 | +15.91301 | +0.87191 | +15.75851 | +0.71741 |
| 100% | N1 repeated | +15.04110 | +15.06329 | +0.02219 | +14.90879 | -0.13231 |
| 100% | S1 reversal | -15.04110 | -15.44952 | -0.40842 | -15.37228 | -0.33118 |
| 100% | S1 repeated | -15.04110 | -15.06329 | -0.02219 | -14.98604 | +0.05506 |
| 100% | N5 | +75.20550 | +75.77991 | +0.57441 | +75.62542 | +0.41992 |
| 100% | S5 | -75.20550 | -76.78413 | -1.57863 | -76.70689 | -1.50139 |

The trace verifies all **16** Dec `MC_AUX_GUIDE` (`0x26`) packets and **16 empty
ACKs** (`3b 03 11 00 26 c6`). Northern Normal North uses signed -50/-100
(bytes `ce`/`9c`), South +50/+100 (`32`/`64`). One-second duration is 100
centiseconds (`64`); each five-second pulse is **255 + 245** (`ff` + `f5`).
There is no incorrect percent byte or duration chunk evident in this capture.

After the first inactive sample, the largest sampled tail displacement was
**0.154495 arcsec** (two encoder counts). All tails reached their final sampled
value by **0.116042 s** after inactive and stayed there through the ~2 s
observation; this is a sampled bound, not continuous-time metrology. Repeated
same-direction one-second pulses were close to nominal. The 50% five-second
settled errors (+0.63482 / -0.48033 arcsec) were within one arcsecond, but the
50% S1 reversal (-2.83063) and 100% S5 (-1.50139) retain excess movement.

The excess appears early rather than as a large post-inactive tail. Its root
mechanism remains unresolved: this is not proof of backlash, a rate-scaling
defect, optical sky displacement or universal guide accuracy. No compensation
or rate correction was added. Cleanup recorded home true, tracking false and
the original 50% guide rate restored. This bounded Dec sequence does not
complete full RA/both-branch guiding or cancellation qualification.

## Latest approved session and software review

The session-review and run 3 server binary SHA-256 was
`c575880f33602ba5f4ad37cb18829466dc01c473f9de068575ae3819bf28a6f8`.
Earlier hashes and results above remain their own checkpoints; the stopped
ConformU attempts below are not retroactively attributed to this build.
The main operator reported **native Core 2120/2120 and HTTP 16/16 CTest cases
passed** at this checkpoint. That full-suite result predates the final arrival
patch below and is **not final-build full-suite qualification**. Earlier hardware
results retain their own provenance and limits.

The operator's review report records final native normal, UBSan and TSan
focused runs, each **80 cases / 1785 assertions**, plus **2 lifecycle/
destruction stress cases / 5 assertions** each, without sanitizer diagnostics.
The review rebuilt the Celestron archive and directly linked the four focused
CGX test files; source/test hashes matched the workspace throughout those
checks. This is fake-backed review evidence, not a ConformU pass.

The three review regressions cover the common enabled-tracking reference gate,
known-fault/closed-link cached state and recovered provenance, and home/flip
publication racing a transport fault. **Six defect-shape falsifiers** compiled
and failed the intended native case after each unmodified baseline passed:
disable the common enabled-write gate; disable the unhealthy-link refusal;
disable true Home provenance; disable true Park provenance; capture the home
generation after label evidence; and capture the flip generation at publication.
A preliminary rejection-only flip mutant survived because its retained old
token still invalidated calibration; the corrected generation-adoption mutant
was killed. Stress now exercises TrackingRate and Home/Park without widening
expected errors. See [current Celestron rules](../.github/instructions/celestron.instructions.md)
and [the continuity decision](decisions/0011-cgx-logical-reference-continuity.md)
for the owning invariants.

### ConformU stage 1–2 attempts

Both local reports use **ConformU 4.5.1.54507**, native arm64 on localhost,
Telescope 0. Their watchdogs stopped at the first finding (exit -15), rather
than producing a full-suite verdict:

- **Run 1:** The operator's report and done record show connection/reference checks reached
  property and Park tests, but parked `PulseGuide(East, 0)` returned
  `InvalidValue` instead of `InvalidWhileParked`. Zero milliseconds is valid;
  current source checks connection/ownership/operational state before returning
  a no-op and sends **no guide packet**. It is not coerced to a 10 ms pulse.
  Invalid direction or negative/out-of-range duration still fails validation.
- **Run 2:** The operator's report and done record show parked zero-duration handling,
  Unpark, AbortSlew, FindHome and basic East/North/overlap guide checks passed.
  The first extended guide at **HA -9 / North 5 s / 50%** failed: reported Dec
  movement **40.2 arcsec**, expected **37.6**, difference **+2.6**, against a
  **1 arcsec** tolerance. The cross-axis RA check passed. This is a guide finding,
  not a clean conformance result.

Both done records verify cleanup home true, tracking false, restoration of
site/elevation, guide rates, tracking rate and SlewSettleTime, and the original
**WARNING** logging level.

### First post-GOTO BeyondPole guide diagnostic

The operator's post-GOTO guide log records North 5 s at 50%, after
GOTO to **HA -9 / Dec +85 / BeyondPole (side 1)**, with 0, 2 and 10 s delays.
Expected movement was +37.60275 arcsec. Errors are measured minus expected:

| Delay after GOTO | Immediate movement | Settled movement | Settled error |
| --- | ---: | ---: | ---: |
| 0 s | +39.55078 | +39.39629 | +1.79354 |
| 2 s | +39.70528 | +39.70528 | +2.10253 |
| 10 s | +39.85977 | +39.78252 | +2.17977 |

In the delayed baselines, Dec changed by one encoder count (-0.0772476 arcsec)
then stayed flat before guiding. Waiting did not remove the excess; these
samples do not support leftover GOTO drift or merely an incorrect steady guide
rate as a sufficient explanation. The mechanism is still unresolved, and the
diagnostic's `PASS` denotes its completed sequence, not guide accuracy. No
coordinate compensation or arrival sleep was added. Cleanup recorded home true
and tracking false. Encoder measurements still do not establish optical accuracy.

### Run 3: extended guides passed, return-to-start RA failed

The operator's report and done record describe the `c575880f` run with raw
strace capture. All **16 extended guide checks** passed: North/South/East/West
at each HA **-9, +9, -3, +3**. The logged North/South differences were
0.1–0.8 arcsec against the 1 arcsec tolerance; East/West differences were
0.02 seconds of RA time against 0.07 seconds, with cross-axis checks passing.
This successful sequence does **not** establish that the earlier intermittent
guide excess is fixed; its mechanism and broader repeatability remain unresolved.

The watchdog then stopped run 3 at **SyncToCoordinates return-to-start**:
reported RA was **18.3 arcsec** from target, outside **±10 arcsec**. The initial
slew and Sync had passed; return-to-start Dec also passed. The done record
reports exit -15, home true, tracking false, restored site/elevation, guide rates,
tracking rate and SlewSettleTime, and restoration of **WARNING** logging.
This is a stopped conformance attempt, not a fresh full pass.

### Arrival correction follow-up and run 4

The direct driver's former **0.02-degree (72 arcsec) projected arrival gate**
could accept this RA miss. Current `kSlewArrivalToleranceDegrees` is
**3/3600 degrees**, checked separately against **unprojected wrapped RA angle**
(`abs(remainder(actualRA - targetRA, 24)) * 15`) and Dec. Do not multiply the
RA error by cos(Dec): at Dec +85, an 18.3 arcsec RA error projects to only about
1.6 arcsec. Correction commands are sent only to axes outside tolerance;
already-arrived axes are not restarted. The existing **three correction rounds,
one-degree maximum error and shared 180 s operation budget** remain unchanged.
This uses real encoder reads and current-time correction targets, not fabricated
coordinates or an arrival sleep.

The elapsed-motion PTY regression,
`Celestron CGX USB Telescope Driver - elapsed arrival corrects raw RA without restarting arrived Dec`,
models about 18.3 arcsec of RA error after 1.22 s, at Dec **+14.04 and +85** on
both normal and flip paths. It requires an RA-only correction and actual final
RA/Dec within 3 arcsec. The operator reports the old arrival gate was falsified
by this regression. After the patch, reported native normal, UBSan and TSan
focused runs each passed **81 cases / 1848 assertions**, plus **2 stress cases /
5 assertions** each. The earlier 2120/16 full-suite pass was not repeated after
this arrival patch and must not be claimed for the final source.

The run 4 deployed binary SHA-256 was
`bc40f868f1e17437b2106d690eabfb2ec614b3b260199c617cbbeec9a8f62f26`.
**Run 4 stopped** at the first extended-test GOTO: all three FAST correction
rounds exhausted the 3-arcsecond arrival gate. It also recorded DeviceState at
0.129 s against the 0.1 s FAST target. Cleanup verified home, tracking off,
restored settings and WARNING logging. No completed pass is claimed.

### Resumed approved SLOW pilot — 14 October 2026

After the pause, the main operator obtained renewed user permission through an
explicit question confirming the mount was **bare and clear**, then deployed
SLOW candidate SHA-256
`2aaff73f6033f24bc045168baffa740ff566603b7f93e95c1d1f08f8bfa23a4a`.
The pause/undeployed status is historical, not a current operating instruction.

Current source uses `MC_GOTO_SLOW` (**0x17**) for RA-only/Dec-only corrections as
needed, with **20 ms** corrective completion polling and actual-error diagnostics.
Initial FAST completion polling stays at 200 ms; the unprojected 3-arcsecond
gate, three correction rounds, one-degree limit and shared 180 s budget remain
unchanged. The helper/codec and corrective-command path have native coverage.
At this SLOW checkpoint, normal, UBSan and TSan each passed **82 cases / 1922
assertions**, plus **2 stress cases / 5 assertions**; the prior FAST-only
implementation was falsified by the updated corrective-command regression.

The operator's pilot log records the approved hardware sequence:

| Target/check | Arrival time | RA error (arcsec) | Dec error (arcsec) | Side | Tracking |
| --- | ---: | ---: | ---: | --- | --- |
| HA +1 / Dec +85 | 22.962 s | 2.17123 | -0.00858 | Normal | restored |
| HA +3 / Dec +85 | 9.125 s | 2.21399 | -0.00858 | Normal | restored |
| HA -9 / Dec +85 (flip route) | 37.737 s | 2.36731 | -0.00858 | BeyondPole | restored |
| Return after Sync | 0.658 s | 2.24339 | -0.07725 | BeyondPole | restored |

The pilot recorded `PASS`, then home true, tracking/slewing false and **WARNING**
logging restored. These are encoder-derived arrivals within the 3-arcsecond
gate, not optical pointing or guide-accuracy qualification. Earlier guide
diagnostics remain separate evidence; the pilot does not resolve guide onset
excess or qualify southern/CGX-L behavior.

### Stationary telemetry and DeviceState timing

Only public coordinate telemetry may reuse a raw encoder pair, when **AtHome or
AtPark**, tracking is **known off**, the link/reference are healthy and valid,
and no operation, transition or retained operation failure is present. The
**30 ms TTL is fixed from refill**, never extended by hits. It stores raw RA/Dec
counts, not sky coordinates or future/projected positions; each read transforms
the counts with current time/site. Moving reads and control callers remain fresh
by default.

Cache keys include reference generation/epoch and motion/pulse/stop generations.
Connection changes, home-label publication and failed/partial reads invalidate
the frame; a known fault refuses even a warm frame, and neither partial refill
nor recovery falls back to stale labels. Owning constraints are in
[current Celestron rules](../.github/instructions/celestron.instructions.md).

The main operator's local DeviceState timing logs each contain **25** hardware
calls:

| Build | Median | Maximum | Calls >100 ms |
| --- | ---: | ---: | ---: |
| `2aaff73f` | 115.146 ms | 121.709 ms | 25/25 |
| `9c353c87` | 34.763 ms | 39.586 ms | 0/25 |

This verifies stationary timing improvement on this rig, not a moving-state
timing guarantee or a completed ConformU verdict.

At this telemetry checkpoint, reported native normal, UBSan and TSan focused
runs each passed
**89 cases / 2024 assertions**, including **7 telemetry cases / 102 assertions**,
plus **2 stress cases / 5 assertions** per configuration. The new
`AlpacaCore/tests/test_celestron_cgx_telemetry.cpp` is registered in the Celestron
block of `AlpacaCore/tests/CMakeLists.txt`. Cases cover bounded reuse, fresh
control/moving/GOTO reads, rehome/reconnect invalidation, failed refill, pending
stop/generation changes and known/unobserved link loss. The previous **2120 Core /
16 HTTP** full-suite pass predates these arrival/telemetry changes and is not a
current full-suite claim.

The latest deployed binary SHA-256 is
`9c353c8721c122561539928708b437b35aa7f28962df9fe423f4345108b096c9`.
The main operator completed **ConformU run 5, untraced**, on that binary; the
qualification and final software checks follow. This documentation update used
local logs/source/tests only; the main operator remains the sole hardware operator.

### Run 5: passing qualification and final software verification

Following explicit user approval to save qualification, the published report is
[Linux-arm64-usb.txt](../AlpacaCore/conformu/Celestron/CGX/Linux-arm64-usb.txt).
It is a new report; the CGX-L hand-controller report remains unchanged. The raw
382-line operator log remains private and unchanged. The publication applies
only the approved site/elevation and
latitude-derived Sync-coordinate privacy substitutions, keeping the 0.1-arcsec
residuals, timestamps, units, pass results and timing rows, followed by an
explicitly labelled publication note without an invented ASCOM timestamp.

**ConformU 4.5.1.54507 on native Linux arm64 passed with 0 errors, 0 warnings,
0 issues and all 98 timing rows within target.** Both the validation and timing
success statements are present. DeviceState took **0.047 s**; AbortSlew **0.001 s**,
Park **0.039 s**, and SyncToCoordinates **0.337/0.306 s**. The run passed all 16
extended guides, slew/Sync return checks and SideOfPier model tests.

Qualified hardware is the **bare northern CGX**, both motors **7.17.25 / model
0x1788**, direct USB AUX, native RPi4/Debian 13 arm64, on 2026-10-14. The deployed
and final rebuilt server SHA-256 is exactly
`9c353c8721c122561539928708b437b35aa7f28962df9fe423f4345108b096c9`.
This is not direct-USB CGX-L, southern-site or optical-accuracy qualification.
Native guide-wire behavior was unchanged; the earlier intermittent guide-onset
excess remains unresolved despite this single complete successful run.

The private supervisor done record reports **exit 0**, cleanup home true and
tracking false, original site/elevation, tracking rate and SlewSettleTime restored,
both guide rates restored to **0.0020890416666666666 degrees/s**, and logging
restored to **WARNING**. The main operator reports the persisted config hash
unchanged: `4a86a99a4fb4a01a6eda5672a8e2c328fed7f8260a8f2138e4f97f952c5a6714`.
The supervisor JSON is not published in the ConformU directory.

The local daemon log was inspected only within **2026-10-14 13:43:49.807–
13:53:58.724 UTC** (09:43:49.807–09:53:58.724 local, UTC-4). Its **11 ERROR
records** were controlled negative tests (unset targets, parked operations and
guide overlap); its **one WARN** was the deliberate UTCDate +1-hour test against
an NTP-disciplined host. No unexpected transport failure occurred in that window.
These daemon log levels are distinct from the ConformU report's zero findings.

The operator's post-run software check recorded an all-vendor build refresh and
**full native Core 2129/2129 plus HTTP 16/16 passed**,
without failures/timeouts. All nine local/native source/build files matched
before and after execution, and the rebuilt server hash matches the run 5 binary.
This is the full-suite result for the run 5 source snapshot, replacing the earlier
pre-arrival/telemetry 2120-case checkpoint for that snapshot. Later PR follow-up
checks are recorded in the [review follow-up](celestron-cgx-cgxl-usb-review.md#pr-1148-review-follow-up--2026-10-14).
Focused normal/UBSan/TSan **89 cases / 2024 assertions**, telemetry **7/102**,
and stress **2/5** per
configuration remain separately recorded checks, not additional post-run runs.
The main operator reports final static checks passed. At that checkpoint,
documentation/report work was uncommitted and unpushed; it has since been
committed to this PR. The documenting agent made no source edits or rig accesses.

## Remaining qualification

- Optical alignment/pointing accuracy and real southern-hemisphere behavior need
  suitable hardware/site evidence; changing SiteLatitude in the northern lab is
  not southern qualification.
- Modern CGX-L identity and motion require an actual CGX-L.
- Full-speed same-branch AbortSlew/Unpark initiator timing now has the approved
  rig pass above; broader motion/load and guide-cancellation coverage remains
  distinct. The active <=2.55 s guide packet drain is separate from stop dispatch.
- Run 5 qualifies the guide checks in this completed ConformU sequence; broader
  guiding/cancellation repeatability and the earlier intermittent excess remain
  distinct. Stage 3 and the post-GOTO diagnostic are not retroactively fixed by
  one passing run or a guide-wire change (none was made).
- Run 3 stopped at return-to-start RA; run 4 on `bc40f868` exhausted tightened
  FAST-only corrections and recorded DeviceState at 129 ms. The approved SLOW
  pilot and stationary telemetry timing checks now have bounded rig evidence.
  Run 5 on `9c353c87` completed cleanly; earlier stopped attempts retain their
  historical findings and provenance.
- The Pi 4's 39-bit-VA kernel cannot run ASan; see
  [the host limitation](failures/0010-rpi4-asan-39-bit-va.md).
