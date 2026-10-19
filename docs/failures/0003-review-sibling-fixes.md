# PR #99 sibling fixes took extra review rounds

## Summary

A fix for one accessor or disconnect route left a matching route with the same defect, and the next review round found it.

## Root Cause

The initial fixes were local rather than a sweep of all sites sharing the invariant: `set_readout_mode` was fixed before `get_readout_mode`; synchronous disconnect during homing was fixed before the async disconnect entry point.

## Prevention

Before pushing a review fix, name the full sibling set in the commit body or PR comment. Check getter/setter, connect/disconnect, sync/async, and all reads and writes of any new lock-protected state in one change. Re-run the concurrency checklist over changed lines and siblings.

## Evidence

- Review history: [PR #99](https://github.com/open-astro/AlpacaBridge/pull/99); shared rule: `.github/instructions/driver-lifecycle.instructions.md` (“When fixing a review finding”).
- Regression tests for related lifecycle paths: `AlpacaCore/tests/test_touptek_concurrency_stress.cpp` AFW homing-window and camera racing-disconnect cases; `AlpacaCore/tests/test_touptek_fake_sdk.cpp` connect-path failure and AFW homing cases. The sibling-sweep process itself remains a review requirement, not a single executable test.
