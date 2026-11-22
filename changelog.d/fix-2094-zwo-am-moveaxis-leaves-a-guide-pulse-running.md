### Fixed

- **ZWO AM MoveAxis cancels queued and running guide pulses** (ZWO, issue #2094): `move_axis()` now calls `reset_position_offsets()` like the other motion initiators, so after `MoveAxis(axis, 0)` or a jog no further `:Mg` is sent and `IsPulseGuiding` reads false; covered by "ZWO telescope clock - MoveAxis stop cancels a running guide pulse (2094)".
