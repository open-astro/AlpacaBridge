# CGX logical-disconnect reference continuity

Status: accepted

## Context

The earlier direct-USB ConformU attempt lost its FindHome reference during
connection tests. Closing the tty also opens an ownership gap for competing
serial probes. Motor identity and encoder labels provide limited continuity
evidence, but AUX exposes no reset-generation counter.

## Decision

Keep the exclusive fd across successful logical disconnects. Retain calibration
only in the same transport session, with matching identity/firmware and bounded,
read-only stopped/settled encoder checkpoints within two counts. Session errors
invalidate calibration even when recovery reuses the fd. Fresh construction or
failed continuity requires explicit FindHome; Connect never homes or relabels.

Idle site changes update reported coordinates without reversing the mechanical
hemisphere established by FindHome. AbortSlew/Unpark transfer cleanup to a joined
stop worker; completion and failures remain observable rather than being declared
idle at dispatch.

## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. The CGX driver keeps its exclusive fd across a successful logical disconnect, and retains calibration only in the same transport session with matching identity and firmware and settled encoder checkpoints within two counts. Scope: `AlpacaCore/src/vendors/celestron/celestron_cgx_telescope_driver.cpp`. Checked by: review.
2. A session error invalidates calibration, even when recovery reuses the fd. Scope: `AlpacaCore/src/vendors/celestron/celestron_cgx_telescope_driver.cpp`. Checked by: review.
3. Connect never homes or relabels; a fresh driver or a failed continuity check needs an explicit FindHome. Scope: `AlpacaCore/src/vendors/celestron/celestron_cgx_telescope_driver.cpp`. Checked by: review.
4. An idle site change never reverses the mechanical hemisphere set by FindHome. Scope: `AlpacaCore/src/vendors/celestron/celestron_cgx_telescope_driver.cpp`. Checked by: `AlpacaCore/tests/test_celestron_cgx_telescope.cpp` ("site writes after Sync preserve mechanical hemisphere"), review.
5. AbortSlew and Unpark hand cleanup to a joined stop worker, and completion or failure stays observable instead of reading idle at dispatch. Scope: `AlpacaCore/src/vendors/celestron/celestron_cgx_telescope_driver.cpp`. Checked by: review.

## Alternatives rejected

- Always discard a reference on logical disconnect: breaks the pre-homed
  reconnect workflow even when ownership and measured continuity remain intact.
- Assume matching firmware/labels prove reset-free continuity: no available
  reset-generation source supports that inference.
- Silently FindHome on Connect: introduces unrequested physical motion.

## Consequences

This is process-local, exclusively owned-session continuity, not persistence
across service restart, reset-safe calibration or HC/AUX coexistence. Stages 1–2
now have approved bounded northern rig evidence for home/Sync logical reconnect,
retained exclusivity, site round-trip and both flips; see the motion evidence.
The later full-speed same-branch AbortSlew/Unpark repeat passed initiator timing
with asynchronous completion. Bounded northern Normal Dec guiding has additional
evidence, with unresolved excess movement; full guiding remains pending.
Two later ConformU attempts stopped at zero-duration parked guiding (fixed) and
extended North guide excess (unresolved). Run 3 on `c575880f` passed all 16
extended guides, then stopped at a return-to-start RA miss; that sequence does
not resolve the earlier intermittent guide excess. Run 4 on `bc40f868` stopped
at tightened FAST-only arrival exhaustion and recorded DeviceState at 129 ms.
Renewed bare/clear permission preceded the bounded SLOW pilot and stationary
telemetry timing checks. Run 5 on binary `9c353c87` completed a clean ConformU
pass on the bare northern CGX / firmware 7.17.25; it does not extend continuity
to resets, restart or HC/AUX coexistence, or claim intermittent guide excess fixed. The
[latest session and review evidence](../celestron-cgx-usb-motion.md#latest-approved-session-and-software-review)
records the native checks and fault-generation falsifiers; current guard and
publication rules remain owned by the Celestron instruction file.
Further deployment and hardware tests
require a fresh user prompt and approval.

## Links

- [Current Celestron rules](../../.github/instructions/celestron.instructions.md)
- [Earlier ConformU attempt and stage 1–2 software/rig evidence](../celestron-cgx-usb-motion.md)
- [Stop completion incident](../failures/0014-cgx-stop-completion.md)
- [Serial-probe interference](../failures/0013-cgx-serial-probe-interference.md)
