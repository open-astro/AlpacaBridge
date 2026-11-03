# ToupTek thermal poller join raced reconnect

## Summary

A reconnect stress run could hang in `join()` or corrupt the poller's thread handle.

## Root Cause

Disconnect joined the poller while a concurrent connect could assign a new `std::thread` to the same member. Joining while holding the driver mutex was also unsafe because each poll tick needs that mutex.

## Prevention

Serialize poller start and stop with `transition_mutex_`, which spans both `set_connected()` directions (the poller runs in an `AsyncOperation` slot, decision 0006), and stop it outside the driver mutex. Preserve the poller on a redundant connect. Skip SDK thermal reads during exposure; the SDK can return `E_UNEXPECTED` when those control transfers overlap a pending image wait.

## Evidence

- Behavior and hardware finding: `.github/instructions/touptek.instructions.md` (thermal reads).
- Implementation: `AlpacaCore/src/vendors/touptek/touptek_camera_driver.cpp`.
- Regression tests: `AlpacaCore/tests/test_touptek_concurrency_stress.cpp` camera connect/disconnect/operate storm; `AlpacaCore/tests/test_touptek_fake_sdk.cpp` thermal-poller cache and exposure case.
