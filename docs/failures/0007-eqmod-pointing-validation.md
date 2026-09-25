# EQMOD-style direct support: self-consistent pointing hid wrong sky motion

## Summary

EQM-35 Pro bring-up measurements were initially treated as proof of correct southern
tracking and pointing. They compared outputs from the driver's own model and could
not expose that model's missing RA home offset and hemisphere sign. The later #432
correction reversed the tracking-direction conclusion associated with #250.

## Root Cause

Reported RA/Dec and derived altitude/azimuth shared the same incorrect model.
Agreement between them was circular evidence. Raw encoder rates established motion
magnitude, but did not independently establish the direction on the sky. Separately,
DeclinationRate and North/South PulseGuide retained a northern-only direction rule;
MoveAxis testing did not exercise that transform.

## Prevention

Separate encoder-rate measurements, model self-consistency, and independent sky
validation. Use plate solving for absolute pointing and physical pier-side claims.
Cover both hemispheres and both Dec-axis branches for rate and pulse corrections.
Do not change mechanical MoveAxis signs to imitate sky-coordinate rate semantics.
Retain corrected historical conclusions so the old tracking-direction change is not
reintroduced from stale bring-up notes.

## Evidence

- [Canonical SkyWatcher history](../../.github/instructions/skywatcher.instructions.md),
  corrected 2026-09-06 hardware notes and “DeclinationRate and PulseGuide North/South
  run backwards south of the equator”; includes the #250/#432 history.
- Regression coverage: `AlpacaCore/tests/test_skywatcher_pointing.cpp` and
  `AlpacaCore/tests/test_skywatcher_async.cpp`.
- The historical notes distinguish measured positive-Dec-axis-branch behavior from
  loopback-only negative-branch coverage. Southern pier-side handling was
  plate-solved on 2026-09-24 (see the instruction file above). This record does not
  promote the still-pending negative-branch checks to completed ones.
- [Related pointing-clock rationale](../decisions/0001-skywatcher-pointing-clock.md).
