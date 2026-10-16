# Celestron CGX/CGX-L direct-USB review

Static review of the CGX/CGX-L direct-AUX telescope backend at commit `1c23cefa`,
against ASCOM Master Interfaces 1.0.27 and the repository's Celestron AUX notes.
No source changes or hardware operations were performed during this review.
The working plan, including the complete repair checklist, is maintained at
`/home/pauleyj/src/open-astro/celestron-cgx-cgxl-usb-dirver-plan.md`.

## Follow-up implementation status — 2026-10-12

The follow-up fixes are committed on `driver/celestron-cgx-usb` at `82bafe45bea009cf68aa7afbb13a8a67036cf8db` and pushed to the fork.

The `driver/celestron-cgx-usb` branch now addresses the software-side findings:

- Motion starts treat acknowledgements as ambiguous, attempt both axis stops, reconnect/flush before retrying idempotent stop commands, and keep `Slewing`/coordinate reads in an error state when stopping cannot be verified. Home/GOTO dispatches and flip rebase/second-leg commands are covered by ownership fences.
- Motion initiators drain the active guide packet before taking motion ownership; pulse teardown is bounded to that packet. `Unpark` cancels and joins a pending Park, and `SlewSettleTime` delays completion through the injected task clock.
- The RA encoder transform and RA guide polarity now mirror the southern tracking direction; apparent sidereal time includes the equation of equinoxes. Sync stops tracking during encoder writes/readback, verifies within encoder tolerance, and restores the previous tracking state. The current follow-up preserves Sync's encoder-label correction across the flip re-home and adds post-flip sky-coordinate/tracking assertions.
- Async GOTO completion verifies measured RA/Dec against the target at current pointing time. Partial tracking writes attempt rollback; unverified rollback makes Tracking readback fail. Failed tracking restoration after a verified stop now clears the logical slew state before reporting the restore error. Firmware cache ordering and motion-slot failure reset were corrected.
- AUX exchanges enforce the absolute deadline and poison the stream after timeout until reconnect; the reconnect path flushes queued input. Modern identity requires four-byte RA/Dec motor versions and model `0x1788` on both motors, without separate switch-address probes. The two-byte-version/switch-probe route remains a legacy fake-backed profile. See [USB identity evidence](celestron-cgx-usb-identity.md). Target read-before-set uses `InvalidOperation` as specified in Master Interfaces documentation revision 1.0.27.
- Adversarial tests cover lost home/GOTO acknowledgements, CGX identity rejection, live tracking drift during Sync, off-target completion, Sync-preserving flip re-home, failed pre-GOTO tracking shutdown, unresolved guide-status recovery, Park cancellation, bounded guide teardown, timeout recovery, and settle-time completion. The direct factory rejects invalid site/optical ranges, and the Celestron stress guard no longer accepts broad `DriverException` failures.

Local C++20 syntax checks, `clang-format --dry-run`, `git diff --check`, all 64 web JavaScript tests, docs/instruction/changelog validation, layering, contract-sweep registration, stress registration, and connect-error-hook checks pass.

Native arm64 workflow run [38197443789](https://github.com/pauleyj/AlpacaBridge/actions/runs/38197443789) passed on the commit above: vendor-neutral and all-vendor CTest, ASan/UBSan, TSan, and the all-vendor coverage run/report. The docs-drift, layering, contract-sweep, stress-registration, connect-error-hook, JavaScript, Unicode, ShellCheck, and zizmor jobs also passed. This was a `workflow_dispatch`; PR-only clang-format/clang-tidy/cppcheck/ConformU-report jobs were skipped. Local formatting and syntax checks passed; no new ConformU hardware report was claimed.

The qualification gaps below describe the historical review baseline, not the latest hardware status. Later [identity evidence](celestron-cgx-usb-identity.md) and [run 5 qualification](celestron-cgx-usb-motion.md#run-5-passing-qualification-and-final-software-verification) cover the bare northern CGX with motors 7.17.25 / model `0x1788`; they do not qualify modern CGX-L identity, southern operation, or optical accuracy. The repository's older `ValueNotSet` target tests and implementations outside the CGX direct backend remain a separate migration to the documented `InvalidOperation` rule; revision 1.0.27 is not evidence of when that requirement was introduced.

### PR #1148 review follow-up — 2026-10-14

The follow-up committed as `bddbdc38` moves reconnect and disconnect continuity checkpoint I/O outside the coarse state mutex while retaining lifecycle ownership and rechecking the original reference generation before publication. Disconnect clears its transition flag on both successful and failed-stop completion; motion handoff checks connection again under its lifecycle locks before transition errors. A held-checkpoint fake case covers cached guide-rate responsiveness, queued tracking, failed checkpoint cleanup and calibration retention/refusal on reconnect. The initial native arm64 baseline passed that case (20 assertions), the focused CGX suite (90 cases / 2044 assertions), and all 442 Core CTest cases with only the Celestron vendor enabled. Final-source UBSan and TSan each passed 90 CGX cases / 2046 assertions and two lifecycle stress cases / five assertions. The final ordinary native rerun passed 90 CGX cases / 2045 assertions and both lifecycle stress cases / five assertions. ASan remains unvalidated for this follow-up: the supplied Pi 4 runs a 39-bit-VA kernel, the known [ASan host limitation](failures/0010-rpi4-asan-39-bit-va.md). No new hardware/ConformU result is claimed.

Mutation verification falsified all 95 native candidates and all three web candidates, with passing baseline/restored runs. Of these, 94 native candidates and the three web candidates produced ordinary named-case failures. The destruction-during-connect mutation instead causes SIGABRT when `shutdown_connection()` is omitted: the process abort is a failing outcome that falsifies the lifecycle contract, not an ordinary assertion failure. Candidate compilation failures, survivors and timeouts were not counted. The concrete edits are published in [PR #1148's Falsified by section](https://github.com/open-astro/AlpacaBridge/pull/1148); the counts here summarize operator-observed runs, not publicly attached raw logs.

Idle `IsPulseGuiding` hardware polling is intentionally unchanged. It queries RA and, if RA is inactive, Dec when the pulse slot is idle, preserving current status and fault reporting without another cache. Optimize only after a measured bus-load problem.

Sibling sweep: the shared `MotionTransition` check covers tracking/tracking-rate, Sync, Home/Park and coordinate/target GOTO starts. PulseGuide now also rechecks connection under `motion_start_mutex_`; AbortSlew/Unpark already do so in their shared stop initiator, and both site setters check under the motion/state locks. Connection teardown retains lifecycle ownership across joins; worker stage guards still refuse disconnected/transitioning state. Both reconnect and disconnect checkpoint callers release only the state mutex, retaining their existing lifecycle protection. Neither async connection forwarding nor the worker lock order was changed.

Local follow-up checks: C++20 syntax-only checks for the changed driver/test, clang-format 18 dry-run, changed-line clang-tidy 18, production-scoped cppcheck 2.17.1, all 81 web tests, docs-drift, instruction-structure, changelog, layering, contract/stress registration, connect-error-hook and whitespace checks passed. These local checks are not native C++ runtime or full-preflight evidence; native results are listed above. The corrected PR-body mutation list has 98 concrete entries, including the new regression, with the destructor exception explicitly identified. The latest review notes retain the intentional coordinate-read wait behind the disconnect's motion-start lock; cached-only reads need not take that lock. Idle guide-status polling remains unchanged without measured load evidence.

The committed fake-based suites model applied/reply-lost commands, partial-axis
failures and an explicit per-read drift hook. They do not establish real motor
dynamics, rate units or physical calibration. Hardware qualification remains separate.

### Issue #1156 contract follow-up

The follow-up to merged PR #1148 accepts AbortSlew and Unpark while an initiator
has released the motion mutex to drain a guide packet. The existing stop worker
waits for lifecycle ownership; the interrupted initiator rechecks the pending
stop before any dispatch and returns `InvalidOperation`. It leaves the transition
gate owned by that stop, and stop-start failure restores the previous gate.
Repeated requests still coalesce; stop completion and failure remain observable.

Sibling set: Tracking/TrackingRate, Sync, FindHome/Park and coordinate/target slew
share `MotionTransition`; PulseGuide and worker stages retain the transition
gate. AbortSlew/Unpark share the stop initiator. Disconnect/destruction retain
lifecycle ownership across all reaps. No transport, routing, configuration,
capability or NexStar-backend change is needed.

Issue #1156 also requests removing all four target-accessor parked guards.
The canonical [TargetRightAscension](https://ascom-standards.org/newdocs/telescope.html#Telescope.TargetRightAscension)
and [TargetDeclination](https://ascom-standards.org/newdocs/telescope.html#Telescope.TargetDeclination)
entries in Master Interfaces 1.0.27 explicitly list `ParkedException` (rechecked
for this follow-up). The guards remain pending authoritative clarification;
this portion of the issue is **not claimed resolved**. Tests can independently
exercise SlewToTargetAsync and SyncToTarget while parked by setting targets before
Park, avoiding the setter refusal that masked both method checks in the old
ConformU report (lines 156 and 158).

Native arm64 fake-based validation passed all 445 configured Core CTest cases
with only Celestron enabled, 93 focused CGX cases / 3060 assertions, and five
Celestron lifecycle stress cases / 11 assertions (including the two CGX cases).
The three new cases passed 1013 assertions; the two new concurrency cases passed
977 assertions on each of ten repeated runs, including guide-timeout recovery. On the unchanged baseline, the
FindHome handoff section and queued-stop disconnect each fail with the original
ownership rejection. The parked-method coverage case already passes its 36
assertions on the baseline. UBSan and TSan with halt-on-error each passed all 93 CGX
cases / 3059 assertions and five Celestron stress cases / 11 assertions. Counts
are reported as observed; all three configurations ran the same 93 CGX cases.
Six production mutations each failed their named test by assertions (exit 42):
missing post-join refusal, guide-failure priority over stop, lost transition gate,
restored stop rejection, and the slew and Sync parked guards independently. The
restored-source three-case run passed all 1013 assertions. No timeout, compile
failure or no-test run is counted as falsification. ASan and full pre-flight remain pending on a compatible
arm64 host; the available Pi has the documented 39-bit-VA ASan limitation.

After explicit bare/clear approval, the all-vendors Release build was deployed
with installed/running SHA-256
`df854e9ed9ddf98ec5ac15227f10824318344f1925c436178f654a65a4f35a56`.
It is an uncommitted snapshot based on `782294de`; the frozen Celestron production
manifest matches the tested driver. No driver version bump is claimed.

The second bounded northern CGX hardware run passed all six combinations of
AbortSlew/Unpark interrupting FindHome/Park/coordinate slew during a native guide.
Stop initiation took 2.13–3.97 ms; the interrupted initiator returned
`InvalidOperation`, and only the small guide displacement remained. Both parked
target methods were called directly using targets set before Park and returned
`InvalidWhileParked`. Cleanup confirmed home, tracking/slewing/guiding off.
The first run timed out waiting for one Park HTTP response despite its expected
server-side interruption error. Its cleanup was safe; the successful repeat does
not explain that timeout. It remains a qualification limitation, not a fixed bug.

Both fresh supervised ConformU 4.5.1.54507.3929333 runs failed at extended
`PulseGuide -9.0 North`: +39.3 arcsec measured versus +37.6 expected, a +1.7
arcsec difference against 1.0 arcsec tolerance, identically in both runs. The
second run was explicitly approved with unchanged code and settings. The watchdog stopped each run;
cleanup confirmed home and no motion/tracking/guiding, restored settings and
WARNING logging. Ara was restored after each session. This resembles the previously documented
intermittent guide excess but does not establish its cause. No guide compensation
or relaxed tolerance was applied. The failed raw reports stay outside the repo;
fresh qualification is **not complete**. Historical passing run 5 is unchanged.
Both new-run parked target-method rows still stop at their setter, so the separate
direct hardware method checks above remain essential evidence.

A separately approved two-pulse diagnostic captured the first pulse's settled
encoder movement as +39.3963 arcsec against +37.60275 expected. Its excess was
already present near onset, before the segment boundary; this is not proof of
optical movement or a particular controller mechanism. The second pulse suffered
a communication fault and has no valid final measurement. Software could not
verify final stop/home/settings restoration. The operator visually confirmed the
mount stationary. A subsequent explicitly approved stop-only recovery verified
slewing/tracking/guiding off, without home or guide commands, and Ara was restored.
Home/reference and prior settings restoration remain unverified. No extra pulses or
compensation were applied. Root-cause and safe-recovery work remain open.

The subsequent stop-only recovery verified slewing, tracking and guiding off;
no homing was attempted. The operator explicitly confirmed the mount is physically
not home. Ara was restored. A source-only diagnostic follow-up
logs the original AUX transaction exception with command/address and elapsed/
budget timing before generic health failures obscure it. It changes no guide
rate, deadline, retry or exception semantics and is not a guide-accuracy fix.
Its fake-based test failed against the old code and passed 60 assertions after
the change. Native Core CTest passed 446/446; ordinary, UBSan and TSan each passed
94 CGX cases / 3119 assertions and five Celestron stress cases / 11 assertions.
Downgrading the error log to DEBUG falsified the new case by assertions (exit 42),
and restored source passed. The diagnostics-only Release variant was subsequently
deployed with installed/running SHA-256
`2a072924af36ed22fdd470e34641007ebef5901ac5606a597c80815dacda7c93`.
Deployment preserved config, restored Ara and WARNING logging, and sent no device
commands. At that deployment checkpoint CGX was disconnected and physically not
home. This improves fault observability only; guide accuracy and hardware
qualification remain unresolved.

With subsequent operator approval, an untraced two-pulse repeat measured
38.314819 and 37.465096 arcsec against 37.60275 expected: errors +0.712069 and
-0.137654 arcsec, both within the unchanged 1 arcsec tolerance. No API fault was
observed. Cleanup verified home, slewing/tracking/guiding off, original settings
and config unchanged, WARNING logging and Ara restored. This demonstrates
variability, not a guide fix or a passing ConformU run.

A later non-moving status-load comparison completed 315/617/312 successful GETs
in 10 s untraced / 20 s traced / 10 s untraced phases. The traced phase contained
1,234 matched, checksum-valid AUX guide-status exchanges, maximum captured
write-entry to reply-read completion 10.064 ms. No communication fault reproduced;
this does not identify the earlier fault or exonerate tracing. Cleanup verified
slewing/tracking/guiding off, settings/config unchanged and Ara restored. The
capture's fd filter cannot establish absence of every descriptor replacement.

A forensic recheck of the earlier failed capture found matching valid replies
for all 828 recorded requests before its first API fault. The last guide-status
reply still indicated active; roughly 865 ms then elapsed without another
captured serial read/write before the worker exited. The failed Declination
call had no captured position request. That capture omitted poll/open/close and
syscall durations, so this gap does not identify the cause. Its later cleanup
also contains an unanswered request despite the decoder's empty `unmatched` list;
that summary is not proof of complete exchanges or safe cleanup.

Comparison with the NexStar backend found the same native `0x26` signed-percent /
centisecond guide payload, sent through HC `P` pass-through instead of direct AUX.
Its getters can substitute expected guide displacement for measured coordinates
(`celestron_telescope_driver.cpp`, `get_declination` / `get_right_ascension`), so
its passing guide result is not independent evidence of motor accuracy. Its
5 s default response budget and read-failure handling also differ from the direct
backend's bounded, fail-closed exchanges. Neither difference proves the present
fault's cause; no coordinate correction, retry or timeout increase was copied.

A third fresh ConformU run, on diagnostic binary `2a072924` and the unchanged
settings, passed HA -9 North (+38.2 arcsec vs +37.6, difference 0.6), then stopped
at HA -9 South (-40.9 vs -37.6, difference 3.3 against 1.0 tolerance). Its observed
prefix has one issue, no errors/warnings and no timing violations; the final
timing table was not reached. Neither daily-file nor journal capture contains an
AUX transaction failure or compromised-link error. The watchdog's
`health_fault=true` is its conservative any-finding policy, not a transport-fault
diagnosis. Child termination preceded one verified stop-only cleanup. Settings,
config and WARNING were restored/unchanged, Ara restored active. Home was skipped;
no post-run home/reference verification is claimed. Fresh qualification and the
intermittent communication fault remain unresolved; no fourth run is scheduled.

A subsequent fixed-order comparison used one matched North/South five-second
pair per binary. Yesterday's passing `9c353c87` measured errors +0.48033 and
-0.40308 arcsec; current `2a072924` measured +1.56179 and -0.48033. Only the
current build's North pulse exceeded 1 arcsec tolerance; its excess appeared
early, with flat baselines and near-nominal middle slopes. This keeps regression
in scope: one fixed A→B order cannot separate build effects from controller
history/time. No communication faults occurred. Current binary restored,
Home true and slewing/tracking/guiding false verified, settings/config/WARNING
unchanged, Ara active. No alternative guide commands or compensation added.

The single counterbalanced B→A comparison then measured current B North/South
errors +0.71207 / -0.24858 arcsec, and old A +1.94803 / -0.48033. The old passing
binary therefore reproduced excess beyond tolerance; the larger North error
followed second-phase placement in both orders. This weakens a build-only
explanation but does not establish an order/controller-history mechanism or
exclude an intermittent regression. Both baselines were flat and middle slopes
near nominal; temperature/history were not measured. Exactly four pulses, no
communication faults. Current B restored with verified hash, explicitly homed at
04:01:13.502 UTC on 2026-10-15, AtHome true and all motion/tracking/guiding false;
settings/config/dependencies/WARNING unchanged, Ara active. These bounded encoder
comparisons are not optical qualification or a replacement ConformU pass.

An alternate absolute-rate command experiment was not implemented: unlike native
guides those commands have no hardware expiry, and the existing wrapper cannot
guarantee priority zero-stop dispatch after blocked telemetry or lost ACK. Its
fail-closed transport behavior was not weakened for a diagnostic experiment.

Celestron's [official firmware history](https://www.celestron.com/pages/firmware-update-history),
checked 2026-10-15, lists CGX 7.18.2214 fixing rapid slew-button taps opposite
tracking that cause quarter-degree bumps. The rig reports 7.17.25. This is a
related manufacturer motion-transient lead, not evidence that the update fixes
our much smaller native Dec pulse-guide excess. The CGX-L 7.18.5020 performance
entry is not a CGX claim. No firmware update/reset or mechanical adjustment was
performed; guide and communication root causes remain unresolved.

#### Operator-approved firmware update — 2026-10-15

The operator subsequently authorized installing Java and updating the mount.
Official CFM 3.0.6196's universal JAR was checked against its published SHA-256
and run on the Pi with OpenJDK 21 and an isolated virtual display. AlpacaBridge
and Ara were stopped before CFM used the mount; no custom flashing code was used.

The standard update wrote and CRC-verified CGX boot 3.1.9120 and motor application
7.18.2214. USB re-enumeration changed ttyACM0 to ttyACM2; CFM's stale-port attempts
then failed before updating the switch modules. This partial failure was retained.
CFM re-discovery verified the new application. Its normal device-selection UI
then excluded the completed motor update while both remaining switch applications
were successfully programmed and CRC-verified as 1.0.7060. Final CFM re-discovery
confirmed the motor and both switch versions.

After CFM exited, AlpacaBridge independently reported **RA 7.18.2214, Dec
7.18.2214**. Verified stop/tracking-off preceded an explicit successful FindHome;
AtHome true and AtPark/slewing/tracking/guiding false were verified. Configured
API settings, persisted config and WARNING logging were preserved/restored;
Ara restored active. No claim is made about every undocumented EEPROM setting.
The server remains binary `2a072924`; no driver change or deployment accompanied
the firmware update. Java/display tools remain installed; CFM and the virtual
display are closed. Private evidence retains package hashes, both update attempts,
CFM logs/screenshots, kernel USB events and post-update checks.

The subsequent single supervised ConformU run on 7.18.2214 failed HA -9 North:
+39.2 arcsec measured versus +37.6 expected, difference 1.6 against tolerance 1.0,
at 2026-10-15 04:28:25.621 UTC. Basic East/North guides and overlap rejection
passed; the cross-axis RA check passed. The observed prefix has one issue and
no errors/warnings; remaining extended guides and final timing table were not
reached. Neither daily-file nor journal records an AUX transaction failure or
compromised-link error. **This firmware update did not eliminate the guide
finding in this attempt.**

The child was reaped before one stop-only cleanup; independent checks verified
slewing/tracking/guiding false, API settings/config/WARNING preserved/restored,
Ara active. Home was skipped after the finding: the earlier post-update Home is
not a current position/reference verification. The watchdog's health-fault flag
is conservative policy, not a communication diagnosis. No further repeat is
scheduled without a new discriminator. Neither underlying fault is claimed fixed;
At that checkpoint, the published passing report covered the original 7.17.25
environment. It has since been replaced by the post-update report described in
the [DeviceState follow-up](#devicestate-follow-up--15-october-2026).

#### Short native-guide polling comparison — 2026-10-15

The operator permitted short guides but prohibited larger positioning while away,
and explicitly reported the mount not home. A reviewed private helper used the
existing wrapper as sole port owner, after stopping Ara and AlpacaBridge. No
driver source/deployment, Home/GOTO, Sync/labels or tracking enable was involved.
Both conditions used stopped RA at the existing pose, not the tracking-on
ConformU geometry. Offline prerequisites passed 21 ordinary and 21 UBSan cases;
the reviewed cadence queried immediately after ACK then waited 20 ms after each
reply, rather than a fixed dispatch grid.

Exactly four positive/negative Dec motor pairs were sent in polled/quiet/quiet/
polled order. Each packet was native `0x26`, signed 50% and 100 centiseconds;
there was no pilot, retry or warm-up pulse. Settled count deltas were:

| Pair | Mode | Positive | Negative |
| --- | --- | ---: | ---: |
| 1 | Polled | +124 | -104 |
| 2 | Quiet | +100 | -110 |
| 3 | Quiet | +99 | -105 |
| 4 | Polled | +102 | -103 |

Nominal magnitude is approximately 97.3564 counts (7.52055 arcsec). The first
positive pulse's magnitude excess was 2.058155 arcsec; final polled positive
excess was 0.358707. Matched negative reversals had magnitude excesses
0.513202/0.976688/0.590450/0.435955 arcsec respectively. Quiet was not consistently
better; this does not establish polling as cause or justify a guide correction.
It also does not universally exclude a polling effect under other conditions.

Quiet windows contained no requests until about 1.200 s after dispatch; endpoint
timing matched within 675 microseconds. RA encoder deltas were zero and all
endpoints passed stationary checks. The helper exited successfully and was reaped
before service restoration; no recovery stops or extra guide pulses were needed.
The restarted server independently verified slewing/tracking/guiding off, with
API settings/config/WARNING and binary unchanged, Ara active. The mount remains
physically not home and no reference was established after restart. Larger motion
is paused for the operator's return; no further comparison scheduled today.
These are relative motor-encoder observations, not optical or ConformU evidence.

## DeviceState follow-up — 15 October 2026

The operator subsequently authorized a complete bare-mount ConformU run that
continued past conformance findings while retaining communication and motion
safety watchdogs. ConformU 4.5.1.54507 completed on the unchanged `2a072924`
binary and motor firmware 7.18.2214: zero errors, configuration alerts or issues,
all 16 extended guides passed, and 97 of 98 response-timing rows passed.
DeviceState took 123 ms against the 100 ms FAST target. Functional validation
passed; the separate response-time target was missed. This does not establish
that the earlier intermittent guide excess is fixed. Cleanup verified Home,
tracking/guiding/slewing off, original settings/config/WARNING and Ara restored.
The optional throughput benchmark remained off; the normal timing table was on.

Follow-up stationary measurements identified an avoidable server-side cost:
live guide-status queries between the coordinate getters can consume the shared
30 ms raw-position-pair lifetime. Isolated traced responses then required six AUX
queries instead of four, adding approximately 26–29 ms. The historical 123 ms call
was not traced; these measurements do not account for its entire delay or exclude
client/host contention. Instrumented measurements are kept distinct from the
untraced baseline.

The minimal shared-base change groups coordinate reads before live guide status.
It adds no cache, changes no freshness limit, and retains all getters and health
checks. A deterministic CGX regression covers Home and Park with the guide reply
held across 32 ms of fake-clock time: the original order fails four read-count
assertions, while the reordered private overlay passes eight telemetry cases /
141 assertions normally and under UBSan on arm64.

The same full report revealed that UTCDate was omitted even though the getter
works. The shared telescope DeviceState now includes UTCDate when known, using
the existing UTC formatter also used by its individual GET endpoint. Getter
failure still omits the field; disconnected state stays empty. Those private
overlay results preceded combined native validation and deployment; they were
not themselves deployed timing evidence.

Combined validation subsequently passed all 2168 all-vendor Release Core tests
and all 16 HTTP tests, with the unchanged HTTP suite run in network isolation to
exclude live discovery responders. The integrated Release binary
`f11acde940431dbe2d275af9c112a921ca4bb07383654e7d272dafbbcd7783e8` was deployed.
The [current published ConformU report](../AlpacaCore/conformu/Celestron/CGX/Linux-arm64-usb.txt)
records a complete pass on 2026-10-15 with both motors 7.18.2214 / model 0x1788:
zero errors, issues or configuration alerts, all 16 extended guide tests and all
98 response-timing rows passing. DeviceState included UTCDate and took 62 ms.
Cleanup verified Home with slewing, tracking and guiding off; original settings,
config and WARNING logging were preserved. Ara remained stopped at the operator's
request. This report qualifies the tested bare northern CGX/backend, not CGX-L,
southern operation or optical accuracy, and does not establish a fix for the
intermittent guide excess or AUX faults.

## Hardware-free follow-up — 2026-10-13

This section records a historical checkpoint, not the current validation status.
Subsequent native runtime and mutation results are in the 2026-10-14 follow-up
above. A full local pre-flight is still not claimed.

The follow-up based on `ff836c44` adds explicit elapsed-time encoder motion,
fractional signed motion and wrap handling, physical/encoder-label separation,
a fixed simulated home origin, and a held-fragment rendezvous that teardown wakes.
The physical-relabel test now observes physical position, rather than just reading
back a label. Both instant and explicit Home reset the same simulated origin;
manual positive/negative commands and tracking use the configured test rate.
These are fake assumptions, not verified controller velocity-register semantics.

New/extended cases cover both hemispheres and pointing branches, guide-duration
rounding around 2550 ms, segmented packet totals, independent driver instances,
reconnect calibration refusal, and the existing 180-second GOTO deadline. The
deadline test auto-completes Home before holding GOTO, so its setup cannot park
on an unadvanced fake clock.

Checks completed before committing this follow-up: C++20 syntax-only checks for both CGX
production translation units, the protocol/geometry/telescope tests, CGX stress
and the contract sweep; clang-format **18.1.8** changed-line check against the PR
base; clang-tidy **18.1.3** on the CGX driver with a source-only line filter;
docs-drift, layering, contract/stress registration, connect-error hook,
changelog validation and whitespace checks. Source-scoped tidy does not certify
unchanged shared-header warnings. At that checkpoint no runtime tests had run:
the host was x86_64, and the existing ARM container could not execute. Native
arm64 CTest, sanitizers, runtime mutation checks and full pre-flight were pending.
ConformU was not re-run in that continuation. Those source-only checks did not
qualify the hardware or replace native runtime validation.

### Mutation evidence

The initial proposed mutations from this checkpoint are superseded by the tested
98-entry list in [PR #1148](https://github.com/open-astro/AlpacaBridge/pull/1148).
The 2026-10-14 follow-up above records the observed outcomes and their limits.

## P1 — fix before hardware qualification

These are findings at the original `1c23cefa` review baseline, not open defects
at the current PR head. Resolution and remaining qualification are recorded above.

1. **Ambiguous first-axis command failure leaves unowned motion.** In
   `start_home_operation()` and `start_slew_operation()`, the first RA command
   is outside the cleanup `try`. A command applied by the mount whose reply is
   lost can leave an axis moving without a monitor or stop. Cover every dispatch
   stage and treat lost acknowledgements as possibly applied.
2. **Abort can be followed by a stale flip GOTO.** In
   `run_flip_slew_operation()`, cancellation is checked before home polling,
   but not revalidated between encoder rebasing and dispatching the target
   GOTO. Fence each motion-stage transition against AbortSlew/disconnect.
3. **Stop failures are swallowed and can become false completion.**
   `stop_axes_no_throw()` discards both errors; a cancelled body can then return
   normally and make `Slewing` false without evidence that the mount stopped.
   Attempt both stops but retain/report failures and verify stopping before
   publishing completion.
4. **Motion initiators do not take ownership from pulse guiding.** Slew, Park,
   FindHome, Sync, tracking changes, and AbortSlew do not cancel/drain
   `pulse_slot_`. A long guide can send a later pulse segment after another
   operation starts. Serialize guide ownership across the initiator family.
5. **Disconnect can wait for the full requested guide duration.** A cancelled
   pulse worker waits for the active bit to clear, with a deadline based on the
   original duration (up to 600 seconds), not the current ≤2.55-second packet.
   Bound cancellation to the active segment and a short timeout.
6. **Southern tracking sign contradicts the RA encoder transform.** The
   transform retains the northern RA orientation while tracking switches to
   negative command `0x07` in the south. Under these equations, reported RA
   drifts at about two sidereal rates (≈1,805 arcseconds/minute). Derive the
   transform, tracking, branch selection and guide polarity together and add
   dynamic fixtures.
7. **Sync readback requires an exact encoder count while tracking can move it.**
   Sync writes both axes then requires exact equality with the precomputed RA
   count. A 24-bit RA count advances about every 5.14 ms at sidereal rate, so
   normal transaction latency can fail Sync and invalidate calibration. Stop/
   restore tracking or use a time-adjusted comparison with encoder tolerance.
8. **Flip re-home discards Sync's encoder correction.** Sync relabels the
   current counters, but a later flip re-home writes fixed quarter/half-turn
   values as the new reference. Preserve the calibrated home-to-sky transform
   across re-home, or invalidate it and require Sync again.
9. **GOTO completion is not verified against the target sky position.** The
   worker trusts `MC_SLEW_DONE`, without reading arrival position or accounting
   for elapsed pointing time. The repository's CGX-L 7.18 notes say tracking
   stops during GOTO, giving roughly 15 arcseconds/second of RA undershoot if a
   fixed target is used. Verify arrival before reporting `Slewing=false`.
10. **Partial tracking writes can leave cached state false to hardware.** The
    helper attempts both axis commands and throws if either fails; the setter
    restores only the previous boolean even if one hardware command succeeded.
    Restore/verify hardware state or latch an error that makes `Tracking`
    unreliable.

## P2 — resolve before calling the interface complete

- `Unpark` only clears `parked_`; an active Park body can set it true again.
- `SlewSettleTime` is stored but ignored when deciding when `Slewing` becomes
  false.
- `AtHome` remains true after tracking starts, although ASCOM defines it as
  stopped in the Home position.
- Motion-slot failure state survives disconnect/reconnect, so a fresh session
  can report the preceding session's motion failure.
- LST uses mean sidereal time; ASCOM `SiderealTime` requires local apparent
  sidereal time.
- The AUX exchange loop does not enforce its absolute deadline if the fd stays
  continuously readable with unrelated frames.
- A delayed response with the same source/destination/command can satisfy a
  later identical request; timeout recovery needs resynchronization.
- Target/site/optical/rate/refraction properties do not consistently enforce
  disconnected state. The official Master page also says target read-before-set
  is `InvalidOperationException`, conflicting with repository
  `ValueNotSetException` guidance/tests; resolve the conflict explicitly.
- Firmware is published before `connected_` becomes true and cleared after it
  becomes false, allowing a management poll to expose firmware while the
  driver reports disconnected.
- TSan stress accepts broad `DriverException` failures with a healthy fake,
  potentially masking protocol regressions.
- Connect checks generic motor-controller version replies but has no CGX/CGX-L
  identity gate; the direct AUX application source address is also unverified.

## Repair order

1. Fix ambiguous dispatch failures, cancellation/ownership fencing, stop
   verification and bounded pulse teardown.
2. Fix hemisphere-aware transforms, Sync tolerance/reference persistence and
   GOTO arrival verification.
3. Correct ASCOM state/completion semantics and narrow stress expectations.
4. Add adversarial fake cases, rerun all-vendor arm64 CTest/TSan, then perform
   bare-mount and southern-hemisphere hardware qualification.
