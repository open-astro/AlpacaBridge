# CGX motion handoff racing disconnect

## Summary

The SkyPortal AUX work exposed an intermittent failure in the existing direct-USB
CGX stress test on the RPi4: `Slewing` reported a stored motion failure whose cause
was "Celestron AUX is not connected". The CGX driver had not been changed before
the failure. A deterministic guide/homing handoff test reproduced the teardown
race without physical hardware.

## Root Cause

`MotionTransition` holds `motion_start_mutex_`, cancels a guide or motion slot,
then releases that mutex while calling `AsyncOperation::cancel_all_and_join()`.
That call takes the body out of the slot before joining its thread. Disconnect
could take the same motion mutex during the join, reap an apparently empty slot,
and close the protocol while the first initiator was still draining its worker.
The worker's late `NotConnected` exception could then become a kept task failure.

An empty slot is not proof that no caller still owns a thread being joined.

## Prevention

The CGX `MotionTransition` now also owns the existing `transition_mutex_` for its
entire lifetime, before taking `motion_start_mutex_`. Connection teardown takes
that same lifecycle lock, so it cannot pass a handoff that is still joining.
Background bodies do not take it; they can finish while the initiator joins.

Sibling set covered by this one change: tracking and tracking-rate setters,
latitude/longitude setters, Sync, Unpark, AbortSlew, the shared FindHome/Park start,
and the shared coordinate/target GOTO start. PulseGuide holds the motion mutex
across its dispatch and does not perform this unlocked reaper handoff; it is N/A
to this particular shape. The stress guard was not broadened to accept the error.

The caller-serialization rule is recorded in `.github/instructions/driver-lifecycle.instructions.md` and the slot header;
the Celestron owner links here for the concrete driver fix.

## Evidence

- Native arm64 full CTest observed the original stress failure once; twenty
  immediate isolated repeats passed, demonstrating why a green rerun alone was
  insufficient.
- `Celestron CGX USB Telescope Driver - disconnect waits for an in-flight motion handoff`
  holds a cancelled guide packet through the injected clock while a FindHome
  initiator joins it, then races disconnect. Before the fix, disconnect returned
  early, FindHome threw the late link error, and disconnect could not verify the
  guide stopped. After the fix, disconnect waits and both calls complete cleanly.
- The new regression and the original CGX lifecycle stress test each passed
  twenty-five consecutive native arm64 runs after the fix.
- Hardware/ConformU was not run. This is fake-based lifecycle evidence, not motor
  or guide-timing qualification.
