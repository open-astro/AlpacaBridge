# CGX USB stop acknowledgement preceding completion

## Hardware finding

On 2026-10-13, after kernel-exclusive serial ownership resolved the competing
probe, an abort approximately 250 ms into a bounded one-degree Dec slew failed
with `Celestron CGX did not confirm that both motor axes stopped`. A zero-rate/
stop-only recovery confirmed both motors subsequently idle. A repeat with
individual status diagnostics identified the transient:

```text
Dec slew_done=0 home_done=1 guide_active=0
```

The next stop check succeeded. The firmware had acknowledged the stop but had not
yet reported completion while Dec decelerated. The old verifier took only one
snapshot and converted valid busy telemetry into a kept operation failure.

## Fix and coverage

The initial fix made shared `stop_axes_checked` poll both controllers' valid slew/home/guide
status against a TaskClock deadline of 500 ms, with a 20 ms sleep between busy
reads. A communication exception remains terminal, and a controller still busy
at the deadline throws DriverException and latches unverified motion. Only a
complete successful verification clears that latch. The final complete status
snapshot is evaluated before the deadline refusal.

Sibling set: AbortSlew, Unpark, disconnect/destructor, home/slew failures and
cancellations, and ambiguous-command cleanup all use this same verifier. No
nonzero motion command is replayed and no capability flag is changed.

The fake can script valid busy replies after a zero-rate stop ACK, without
claiming a full acceleration/encoder-motion simulation. Added regression cases:

- `Celestron CGX USB Telescope Driver - stop acknowledgement waits for completion`:
  fails against the old one-shot verifier; two delayed status reads must settle
  and return success rather than leave an unverified-motion error.
- `Celestron CGX USB Telescope Driver - stop completion has a bounded deadline`:
  rejects a controller that never settles, with elapsed-time and latched-state
  checks. The old code fails the minimum wait assertion. Raising the production
  deadline from 500 ms to 5000 ms falsifies the upper-bound assertion.

Native arm64 focused CGX suite after the fix: **60 cases / 713 assertions passed**,
including the kernel-ownership, guide-polarity and lifecycle stress coverage.

The deadline bounds busy polling, not an SDK transaction already in flight.
Transactions retain their separate bounded timeout. This record requires a fresh
hardware abort check before the original failing lab case is called a pass.

## Hardware repeat

With the completion wait deployed, the same early abort of a one-degree slew
returned in **0.4649 s**. Dec stopped at **+89.7465634 degrees**, approximately
0.2534 degrees from home, rather than reaching the +89 target. Dec remained
unchanged during the post-abort observation. Tracking restoration remained true,
with an approximately sidereal RA-axis encoder rate, then tracking was explicitly
disabled. FindHome returned the mount to Dec +90 in about four seconds.

Final state: AtHome true; Slewing, Tracking and IsPulseGuiding false. This validates
the bounded lab abort path, not every rate, payload or meridian-route abort.

## Full-speed RA abort and recovered failure state

The subsequent larger-movement session (2026-10-14 UTC, 2026-10-13 local) aborted
a GOTO towards HA +3 hours / Dec +65 degrees after two seconds. The cancelled
worker's first 500 ms verification still saw RA `slew_done=0`. AbortSlew's later
stop verified success, but `Slewing` continued throwing the earlier worker failure.

AbortSlew and Unpark now clear both joined operation slots' failures **only after**
their final stop verification succeeds. Disconnect and Connect already clear the
slots. A failed final verification still keeps unverified motion visible; no
deadline or busy-status assertion is weakened. This is recovery from a failed
earlier stop attempt, not a claim that full-speed deceleration completes in 500 ms.

`Celestron CGX USB Telescope Driver - verified stop replaces a cancelled worker failure`
reproduces the shape for AbortSlew and Unpark. The old code fails both post-stop
Slewing checks. Removing their `motion_slot_.clear_failure()` calls falsifies it.
The existing unresolved-guide case also checks that verified AbortSlew clears the
guide failure; the responsive-sibling stop case now requires truthful idle state
after the final verified stop, rather than a stale worker exception.

The deployed repeat returned in **1.2626 s**, stopped at Dec **+81.9407558 degrees**,
and reported Slewing false. After tracking-off, two seconds of observation showed
zero Dec drift and approximately one encoder count of RA-axis variation. Park
from the stopped position completed in approximately **16.1 s**, with a **20.5 ms**
initiator; Unpark left tracking off. The mount finished home and idle.

The historical **blocking** 1.2626 s AbortSlew is not a one-second initiator
timing pass. It is not a timing result for the later async stop worker below.
Guide cancellation's active-packet wait remains a separate completion bound.

## Async-stop follow-up and approved stage 1–2 retest

Stages 1–2 now dispatch AbortSlew/Unpark cleanup to a stop worker, with pending
completion state and no duplicate stop from cancelled motion bodies. The earlier
hardware measurements above remain evidence for the previous synchronous path.
The [stage 1–2 evidence](../celestron-cgx-usb-motion.md#stages-12-software-and-approved-rig-evidence)
records the tracking-publication race and its ACK-pair mutex fix, followed by
user-approved deployment and testing by the main rig operator.

The initial stage 1–2 build (reported binary hash prefix `c3c3d5e`) dispatched
AbortSlew in **4.335 ms**, pending true, but failed about **0.5868 s** later with
`RA slew_done=0 home_done=1 guide_active=0`. The explicit async worker still
inherited the default 500 ms budget. Cleanup initially saw the same failure;
FindHome subsequently restored home with tracking and slewing off. Evidence:
local `/tmp/opencode/cgx-stage12-hardware.log`.

Current source gives `run_stop_operation()` a **3 s** verification budget through
`stop_axes_checked(std::chrono::seconds(3))`. The **500 ms default remains** for
disconnect/destructor, failure/cancellation and ambiguous-command cleanup;
cancelled bodies skip their own stop when the explicit worker owns it. This
changes valid-busy polling only, not communication-failure handling, transaction
timeouts, command replay policy or active guide packet draining.

`Celestron CGX USB Telescope Driver - async stop verifies deceleration beyond the cleanup budget`
covers AbortSlew and Unpark with 60 valid busy replies (~1.2 s), prompt dispatch,
eventual idle and one stop command per axis. The operator reports that the old
native 500 ms baseline fails it. The never-settling regression now pins the
explicit worker's bounded 3 s failure; normal cleanup retains its shorter default.
At that checkpoint, focused normal, UBSan and TSan runs each passed **76 cases / 1507
assertions**, plus **2 stress cases / 5 assertions** each. The earlier **2115 Core
/ 16 HTTP** full-suite pass predates this final budget change and was not repeated.

The deployed retest binary SHA-256 was
`a381d1a5156d76ee4c5524793f3a0d3729a85a417f3987f9a27c429e0132b744`.
Local `/tmp/opencode/cgx-stage12-hardware-retest.log` records AbortSlew returning
in **4.014 ms**, pending true, then completion in **0.16175 s**, tracking true,
Dec +89.9983263 degrees. Unpark passed with a **3.588 ms** initiator; final cleanup
returned home with tracking and slewing off.

This abort started near the pole after FindHome and may have taken a home route.
It is bounded async-dispatch/completion evidence, **not a full-speed same-branch
RA abort pass** or proof of a 3 s physical deceleration interval. The subsequent
approved true same-branch case from Dec +85 is recorded below. No rig APIs or
deployment were used for this documentation update.

## Full-speed same-branch async-stop pass

The main operator's approved follow-up on the same `a381d1a5` binary is recorded
in local `/tmp/opencode/cgx-fullspeed-stop.log`:

| Operation | Initiator | Completion | Dec before → after |
| --- | --- | --- | --- |
| AbortSlew | 4.822 ms | 1.261968 s | +79.2139363 → +76.8061709 degrees |
| Unpark | 3.944 ms | 1.282701 s | +79.2142797 → +76.8065357 degrees |

Both returned with pending true, completed successfully and restored tracking.
Both **initiators pass the one-second response target** in this hardware check;
the ~1.26–1.28 s physical completion remains asynchronous. Do not confuse it
with the old 1.2626 s blocking call. Final cleanup recorded home true, tracking
false and slewing false. Full guiding/cancellation qualification remains
separate; the subsequent ConformU outcome and its scope are in the
[motion and guide evidence](../celestron-cgx-usb-motion.md).

The [latest session and software review](../celestron-cgx-usb-motion.md#latest-approved-session-and-software-review)
records the subsequent `c575880f` deployment, 2120/16 native full-suite results
before the final arrival patch, and the 80/1785 focused review checkpoint.
Run 3 passed extended guiding then stopped at return-to-start RA; run 4 on
`bc40f868` stopped at FAST-only correction exhaustion and recorded DeviceState
at 129 ms. The same motion evidence records the approved SLOW pilot and stationary
telemetry timing follow-up; run 5 on `9c353c87` completed cleanly with all timing
rows passing on the northern CGX. Its scope does not retroactively replace the
historical blocking-call results or qualify every payload/cancellation path.
