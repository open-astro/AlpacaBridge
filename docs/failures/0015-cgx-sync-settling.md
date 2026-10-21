# CGX USB Sync encoder settling before and after label writes

## Hardware finding

On 2026-10-13, the northern CGX 7.17.25 was homed and positioned at Dec +89
degrees. Tracking was switched off, then a small coordinate-label Sync (RA
+0.001 hours, Dec +0.01 degrees) immediately failed the driver's strict
post-write encoder-reference check. The fallback confirmed both motors stopped.

Repeating the same workflow with a one-second caller pause after Tracking=false
passed: target RA 13.6058931469 / Dec 89.0100081062, readback RA 13.6059100612 /
Dec 89.0100073814. FindHome restored the home labels afterwards. This supports
settling after the rate-off acknowledgement as the failure mechanism; it is not
evidence for a different position encoding or permission to loosen readback.

## Fix

Sync now samples both raw encoders before writing labels, using the shared
`util::ConsecutiveSettle` decision: three consecutive stable comparisons, a
15-poll budget and a 500 ms TaskClock deadline, with 20 ms between polls.
Stable means each adjacent encoder delta is within two counts. The wait applies
even if Tracking was already false when Sync was called. Communication failures
remain errors. The existing two-count post-write check is unchanged.

An unsettled pre-write refusal preserves the known coordinate reference. Once
the first label write is attempted, even an ambiguous ACK can mean the reference
changed, so a later failure invalidates calibration as before. Coordinate and
target Sync forms share this path; both axes are checked together. Concurrent
Sync is added to the existing lifecycle stress callback.

## Regression evidence

The fake scripts four-count-per-read RA drift after a tracking-off command. It
models the fault shape, not the motor's full dynamics or a physical rate.

- `Celestron CGX USB Telescope Driver - Sync waits for encoder stability after tracking stops`:
  eight drifting reads must settle before relabelling; the old code fails with
  `RA delta=4 Dec delta=0 counts`. Removing the settling loop falsifies it.
- `Celestron CGX USB Telescope Driver - Sync refuses unsettled encoders before writing labels`:
  continuous drift must refuse without another RA or Dec MC_SET_POSITION;
  the old code writes both labels before failing. Bypassing the pre-write
  settling decision falsifies the write-count checks.

Native arm64 focused CGX suite after the fix: **62 cases / 725 assertions passed**,
including lifecycle stress with Sync and the prior ownership/stop/polarity cases.

This is coordinate-label and driver-state validation in a lab, not astronomical
alignment or pointing-accuracy qualification. A fresh immediate-Sync rig repeat
without the caller pause is required before calling the original case a pass.

## Hardware repeat without a caller delay

After deployment, the original immediate workflow passed without the one-second
caller pause. Sync target RA **13.9408262350 hours** / Dec **89.0100295639 degrees**
read back as RA **13.9408431345** / Dec **89.0100288391**. Both target properties
retained the exact written values, tracking stayed off, and no slew was reported.
FindHome restored the unsynced home reference in approximately 6.55 seconds.
Final state was AtHome true, AtPark/Slewing/Tracking/IsPulseGuiding false and Dec
approximately +90 within encoder noise.

An adjacent bounded Park/Unpark test also passed: Park completed at the existing
home location, tracking/guiding while parked were refused with InvalidWhileParked
(1032), repeated Park stayed idle, and Unpark left the mount at home without a
slew or tracking start. This is not a power-cycle park-recovery qualification.

## Post-write transient found during larger-movement testing

On 2026-10-14 UTC (2026-10-13 local), a larger Sync at Dec +85 degrees, offset
RA +0.01 hours / Dec +0.1 degrees, failed the first readback by RA -3 / Dec -2
counts. A one-second caller pause before Sync still failed (RA -2 / Dec -7), and
an explicit verified AbortSlew before Sync also failed (RA +3 / Dec -8). Waiting
only before the writes therefore does not address this second phase.

A syscall trace showed unchanged encoder samples before writing, then:

| Axis | MC_SET_POSITION payload | First MC_GET_POSITION payload | Delta |
| --- | --- | --- | --- |
| RA | `0a 90 2f` | `0a 90 2d` | -2 counts |
| Dec | `83 7c 05` | `83 7b fe` | -7 counts |

With the daemon stopped and the diagnostic holding kernel exclusivity, stop/zero
rate commands were sent to both motors. Label-only probes then requested offsets
0, 1, 2, 4, 8, 16, 32, 64, 1024, +4660 and -4660 counts relative to the starting
labels, sampling five reads 30 ms apart after each write. They sent no GOTO,
guide, home or nonzero-rate commands and restored the starting labels afterwards.
Even rewriting an unchanged label produced a transient: RA `[4,3,1,0,0]`, Dec
`[-8,-7,0,1,1]` counts relative to the request. Subsequent offset probes showed
the same brief pattern, ending within two counts. This establishes a post-write
readback transient, not its internal controller mechanism or optical movement.

Sync now uses another bounded `util::ConsecutiveSettle(3,15)` phase **after** the
writes: both readbacks must remain within the unchanged two-count target
tolerance for three consecutive comparisons. The 500 ms deadline and 20 ms
cadence remain; a persistent offset or I/O failure still invalidates calibration.
No label is rewritten to chase the transient. Coordinate and target Sync share
the path, and both axes are verified together.

`Celestron CGX USB Telescope Driver - Sync verifies settled label readback without widening tolerance`
scripts the captured shape and a persistent eight-count offset. The old immediate
check fails three assertions on the settling branch; widening the production
tolerance to eight counts falsifies the refusal branch. The fake models reply
offsets, not a controller servo or optical position.

After deployment the larger **immediate** Sync passed without a caller pause or
preceding AbortSlew. Readback was RA **20.0594086923 hours**, Dec **85.0999689102
degrees**. The following BeyondPole and return Normal slews both completed within
the existing arrival tolerance (approximately 52.7 s and 50.8 s). FindHome then
restored the unsynced home frame. The full Sync/flip workflow is rig evidence;
independent optical calibration preservation is not established by this lab run.

Native arm64 focused suite after both recovery and post-write changes:
**64 cases / 753 assertions passed**, including lifecycle/destruction stress.
