### Fixed

- **SynScan PulseGuide beside a MoveAxis on the other axis**, SynScan driver, issue #2467. A PulseGuide is accepted while a MoveAxis drives the other axis, as the #2263 motion-claim table says: each axis is its own motor command, and the pulse's end stops or hands back to tracking only its own axis, so the jog keeps running. A MoveAxis on the pulse's own axis still refuses it with `InvalidOperation` (`test_synscan_motion_owner.cpp`).
