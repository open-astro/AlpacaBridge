---
applyTo: "AlpacaCore/src/vendors/bisque/**,AlpacaCore/include/alpacacore/vendor/bisque/**,AlpacaCore/tests/*bisque*"
---

### Bisque (Paramount / TheSkyX)

Devices: Telescope.

Protocol documentation: `AlpacaCore/external/Bisque/` (INDI reference driver and TheSkyX scripting API docs). No external SDK required — communicates via JavaScript commands over TCP to TheSkyX Pro.

Connection types: TCP only. TheSkyX acts as middleware between the driver and the actual mount hardware.

- Communication is via JavaScript command snippets sent to the TheSkyX TCP scripting server (default port 3040).
- Commands are strings terminated with `#`. Responses are prefixed with `|No error. Error = 0.` on success, terminated with `#`.
- Special case: Handshake (`ConnectAndDoNotUnpark`/`IsConnected`) returns just `1` with no prefix.
- Slew is async: set `sky6RASCOMTele.Asynchronous = true`, call `SlewToRaDec`, poll `IsSlewComplete`.
- Pulse guiding uses `sky6DirectGuide.MoveTelescope(dRA, dDec)` with arcsecond displacement. `sky6DirectGuide.lAsynchronous=0` makes that call wait for completion; keep it on a joinable driver worker so `PulseGuide()` returns promptly while `IsPulseGuiding` remains observable.
- Open loop motion for MoveAxis uses `DoCommand(9, 'direction|rate')` and `DoCommand(10, '')`.
- Park uses `ParkAndDoNotDisconnect()` to keep TCP connection alive (not `Park()` which disconnects).
- Pier side is read-only via `DoCommand(11, 'Pier Side')` — returns 1 for west of pier, else east.
- Find Home uses `FindHome()` with a 60-second timeout.
- Slew speed presets: 9 rates (1x, 2x, 4x, 8x, 32x, 64x, 128x, 256x, 512x sidereal).
- Every TheSkyX exchange (connect and handshake, FindHome, Park and its polls, slews and their completion polls, sync, `SetTracking`, MoveAxis, Unpark, SetPark, the position samples at the end of sync, slew complete, FindHome and Park) runs with the driver `mutex_` free, under the protocol wrapper's per-link lock (the wrapper is one instance per process, so that lock is the I/O lock); `mutex_` stays the short state lock. MoveAxis, both slew forms and AbortSlew hold `initiator_mutex_` across gate, command and state write (a sync slew drops it before its wait); order `initiator_mutex_`, then the wrapper's lock, then `mutex_` (leaf). A control-path end takes the snapshot token under `mutex_`, samples without it and publishes under it, so a write since drops the publish (`publish_measurement()`). Connect runs under `transition_mutex_`; `set_connected(false)` clears state, releases `mutex_`, then closes the transport. Getters never wait behind the wire ([decision 0016](../../docs/decisions/0016-getters-never-wait-on-device-io.md)). Tests: the timing cases in `test_bisque_telescope_snapshot.cpp` (FindHome, SyncToCoordinates position read, `SetTracking`, the connect's IsParked query, Park's IsParked poll, a disconnect behind a PulseGuide reply, the sync slew's IsSlewComplete poll, the MoveAxis, SlewToCoordinatesAsync, Unpark and SetPark commands).
