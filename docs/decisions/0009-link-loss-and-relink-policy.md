# Link-loss and relink policy

Status: accepted

## Context

A driver's link can fail in two ways a client cannot tell apart from `Connected`. The transport can be gone (a pulled USB adapter, a closed socket), or the transport can be fine while the device stops answering (mount unpowered, cable pulled at the mount end). Until now the Sky-Watcher serial path dropped `Connected` for the first (open-astro#445) and latched a fault for the second (#505) while `Connected` stayed true for ever. The UDP path had only the latch. The #445 rig run showed clients never learn a mount is gone when `Connected` stays true, and a relink after an outage needs the outage length (#521), which only the serial loss paths recorded.

## Decision

1. **Two stages.** *Faulted* is today's latch (`StreamLinkHealth`, `PolledLinkHealth`, driver latches): `Connected` true, the cache refused with `DriverException` "communications compromised", the fault text from `get_link_fault()`, cleared by the next good frame or reply. *Lost*: `Connected` false.
2. **When a link is lost.** (a) At once on positive evidence the transport is gone: a serial or HID node removed or re-pointed, `EIO`/`ENXIO`/`ENODEV` on the fd, a socket that is itself unusable (`EBADF`, `ENOTSOCK`, `ENOTCONN` on a UDP socket) or a refused connect. Other UDP send and receive errors (`EHOSTUNREACH`, `ECONNREFUSED`, `ENETUNREACH`, `EADDRNOTAVAIL`, `ENETDOWN`) report reachability, which a Wi-Fi drop or a board reboot restores: they count as a no-reply exchange for the latch and the staleness bound decides. (b) When a latched fault has stood without a break past the staleness bound, `util::kLinkStalenessBound` (30 s, `util/link_health.h`), measured on the task clock of [decision 0005](0005-task-clock.md). A driver may set a longer bound with a reason in its code, never one shorter than its own latch threshold.
3. **While lost.** Operational getters and setters throw `NotConnected`; `Connected` reads false; `DeviceState` returns the empty list with no `TimeStamp`; the transport handle is closed (best effort: when an exchange holds the link at that moment, the fd may stay open until the next Connect or Disconnect); `link_lost_at` is stamped once on the task clock for every transport (`util::LinkLostStamp`).
4. **No autonomous reconnect.** A driver never reopens on its own. Only the client's `Connected=true` / `Connect()` relinks. It never short-circuits on a lost link and re-runs the connect probe. Motion on relink keeps the #521 window, now fed by `link_lost_at` on every transport (OnStep times it from the last time the link was seen alive instead; see Adoption, #1241). A client's Disconnect queued against an in-flight Connect reaches the driver even though `Connected` already reads false (#1289).
5. **One fault text.** `get_link_fault()` stays the single source. After the link is lost it keeps the last fault text until the next Connect or Disconnect, so the management listing shows why. There is no second health field.
6. **open-astro#660 identity gap.** Closing it is a per-driver identity query, each a vendor-protocol change with its own rig check, so drivers close it one at a time. The risk is a different device appearing on the same path between a loss and the client's reconnect. The table below records what each connect probe checks (read from the source when the row was written; any rig evidence is in the driver's vendor instructions).
7. **Slice scope.** This record, the shared helper in `util/link_health.h`, Sky-Watcher as the reference driver for both transports, fake-only tests (`tests/test_skywatcher_link_loss.cpp`). Other drivers adopt through the table below.

### Connect probe: does it reject a different device? (point 6)

| Driver | Probe at connect | Rejects a different unit of the same family? | Follow-up |
| --- | --- | --- | --- |
| Sky-Watcher | `:e1` firmware/board query, board check after recovery | No: a different Sky-Watcher board passes | none, reference driver |
| SynScan | `K`+byte echo | No: any handset speaking the protocol passes | planned follow-up |
| Celestron | NexStar version/model queries | No identity check found in the wrapper | planned follow-up |
| OnStep | `:GVP#` identity string | Rejects a non-OnStep device only | planned follow-up |
| iOptron | `:MountInfo#` / `:DeviceInfo#` model | Rejects a different model, not a second unit of the same one | planned follow-up |
| Gemini | `>H#` identity string (exact compare) | Rejects a different Gemini product, not a second unit | planned follow-up |
| QHY | JSON version handshake (Q-Focuser), slot-count check (CFW3) | No per-unit identity | planned follow-up |
| ZWO AM | Fresh hash-terminated supported `:GVP#` model and normalized twelve-hex `:GMA#` serial | Yes: refuses a different mount serial until the driver is recreated ([ZWO](../../.github/instructions/zwo.instructions.md)) | done (#1581) |

## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. A faulted link keeps `Connected` true and makes reads and writes throw `DriverException` with the fault text from `get_link_fault()`; the next good reply clears it. Scope: `AlpacaCore/src/vendors/*/*_driver.cpp`. Checked by: `AlpacaCore/tests/test_skywatcher_link_loss.cpp`, review.
2. A link is lost at once on positive evidence that the transport is gone, and when a fault has stood past `util::kLinkStalenessBound`; a driver never sets a bound shorter than its own latch threshold. Scope: `AlpacaCore/src/vendors/*/*_driver.cpp`, `AlpacaCore/include/alpacacore/util/link_health.h`. Checked by: `AlpacaCore/tests/test_skywatcher_link_loss.cpp`, `AlpacaCore/tests/test_onstep_link_loss.cpp`, review.
3. While lost: `Connected` reads false, operations throw `NotConnected`, `DeviceState` is the empty list, the transport is closed and the loss time is stamped once. Scope: `AlpacaCore/src/vendors/*/*_driver.cpp`. Checked by: review (trace each loss path through state clearing, best-effort close and the one-time loss stamp).
4. A driver never reopens a lost link on its own; only the client's Connect relinks, and it re-runs the connect probe. A Disconnect queued against an in-flight Connect still reaches the driver when the link was lost meanwhile. Scope: `AlpacaCore/src/vendors/*/*_driver.cpp`, `AlpacaCore/include/alpacacore/async_connectable.h`. Checked by: review (no worker reopens a lost transport; the client connect path re-runs the connect probe), `AlpacaCore/tests/test_async_connectable.cpp` ("a disconnect queued against a connect reaches a driver whose link was lost").
5. `get_link_fault()` is the one fault text, and it keeps the last text after a loss until the next Connect or Disconnect. Scope: `AlpacaCore/src/vendors/*/*_driver.cpp`. Checked by: review (loss preserves the fault text, while Connect and Disconnect reset it). <!-- allowed -->

## Alternatives rejected

- **A: `Connected` stays true for ever and the client decides** (the old latch doc comment). The #445 rig evidence shows clients never learn the mount is gone.
- **B: drop `Connected` on the first failed exchange.** It flaps on a short USB or Wi-Fi drop and ends sessions mid-exposure.
- **C: autonomous reconnect.** A mount relinked without its client can move unsupervised (#521), and ASCOM leaves connection to the client.
- **D: an identity gate on all five drivers in this slice.** Too large (point 6).
- **E: a client Disconnect consumes the loss stamps (#1289 item 1).** Clients such as NINA send Disconnect themselves when a reconnect fails, so the next Connect would leave motion running unattended (#521). An unwanted stop of a handset slew is the safe failure.

## Consequences

- On an adopted driver a client sees `Connected` false within `util::kLinkStalenessBound` of a device going silent, and reconnects itself; a drop shorter than the bound only faults.
- A driver not yet adopted keeps `Connected` true while faulted (Adoption table).
- A reconnect after a loss re-runs the connect probe. The identity table in point 6 states which drivers distinguish two units of one model; a driver that pins a unit says in its vendor instructions how a deliberate replacement is admitted.

## How to reverse

Revert the PR. The bound is one constant, so "never lost by silence" is a one-line change back to the previous behaviour.

## Adoption

`scripts/check_docs_drift.py` check 23 holds this roster to the code: a driver that names `kLinkStalenessBound` needs a row.

Scan: `AlpacaCore/src/vendors/*/*_driver.cpp`
Symbol: `kLinkStalenessBound`

| Driver | Symbol | Source | State | Issue | Notes |
| --- | --- | --- | --- | --- | --- |
| Sky-Watcher (serial, UDP) |  | `AlpacaCore/src/vendors/skywatcher/skywatcher_telescope_driver.cpp` | adopted | #970 | Decision applied |
| WandererAstro (cover, filter wheel, box, rotator) |  | — | planned | — | Stream latch; `Connected` stays true |
| Gemini PDH |  | — | planned | — | Polled latch; `Connected` stays true |
| Gemini flat panel | `link_alive` | `AlpacaCore/src/vendors/gemini/gemini_flatpanel_driver.cpp` | adopted | #1723 | A hangup on a read or `EIO`/`ENXIO`/`ENODEV` on a write is a lost link at once (`Connected` false, operations `NotConnected`, `DeviceState` empty, port released); no latch or staleness bound (request/response, no poll); no `LinkLostStamp` (no #521 consumer); `get_link_fault()` keeps the loss reason until the next Connect or Disconnect |
| Gemini focuser | `PolledState` | `AlpacaCore/src/vendors/gemini/gemini_focuser_driver.cpp` | adopted | #1229, #1643, #1723 | Applied with the state snapshot (#1643): three failed polls latch a fault, a fault standing past `util::kLinkStalenessBound` is a lost link (`Connected` false, operations `NotConnected`, `DeviceState` empty, transport released); a hangup on a read or `EIO`/`ENXIO`/`ENODEV` on a write is a lost link at once (#1723: port released, the poll ends); `get_link_fault()` keeps the loss reason until the next Connect or Disconnect; no `LinkLostStamp` (no #521 consumer) |
| Celestron focus motor | `PolledState` | `AlpacaCore/src/vendors/celestron/celestron_focuser_driver.cpp` | adopted | #1320, #1643 | Applied with the state snapshot (#1643): three failed polls latch a fault, a fault standing past `util::kLinkStalenessBound` is a lost link (`Connected` false, operations `NotConnected`, `DeviceState` empty, transport released); no `LinkLostStamp` (no #521 consumer) |
| iOptron mount, iEFW, iMate |  | — | planned | — | `device_faulted_`; `Connected` stays true |
| iOptron iEAF | `link_alive` | `AlpacaCore/src/vendors/ioptron/ioptron_ieaf_focuser_driver.cpp` | adopted | #1723 | A hangup on a read (a move, halt or zero ack included) or `EIO`/`ENXIO`/`ENODEV` on a write is a lost link at once (`Connected` false, operations `NotConnected`, the status cache not served, `DeviceState` empty, port released); no latch or staleness bound (request/response, no poll); no `LinkLostStamp` (no #521 consumer); `get_link_fault()` keeps the loss reason until the next Connect or Disconnect |
| QHY Q-Focuser | `PolledState` | `AlpacaCore/src/vendors/qhy/qhy_focuser_driver.cpp` | adopted | #1310 | `link_lost_` latch on the next transaction (#527); applied with the state snapshot (#1310): a poll that has not been answered for `util::kLinkStalenessBound` is a lost link (`Connected` false, transport released), three failed polls are a fault |
| OnStep (serial) |  | `AlpacaCore/src/vendors/onstep/onstep_telescope_driver.cpp` | adopted | #1241, #1414, #1626, #1783 | Applied: node check, fd errors, relink (#1241); the #521 window is timed from the last time the link was seen alive (the fd, not a reply, so after a staleness loss it runs from the loss) to the end of the reconnect, and a park still running is left to finish (see the OnStep instructions); three failed status-poll ticks or control-path `:GU#` reads in a row (the `:GU#` read after a `:GE#` with no code counts too) latch a fault (`util::PolledLinkHealth`; reads and writes throw, the site, `UTCDate`, `SiderealTime`, `DoesRefraction`, `IsPulseGuiding`, `Slewing` and rate reads and the site and `UTCDate` writes included; `Connected` stays true; the getters serve the state snapshot (decision 0010, #1783) and ask the mount nothing; AbortSlew and `MoveAxis(axis, 0)` still send their stops first; the raw `command_blind`/`command_bool` pass-through is not refused), and a fault standing past `util::kLinkStalenessBound` is a lost link through the same path as a pulled adapter (#1414), found by the next failed poll tick or refused write |
| Celestron telescope (NexStar) |  | `AlpacaCore/src/vendors/celestron/celestron_telescope_driver.cpp` | adopted | #1337 | Applied with the state snapshot (#1337): three failed status polls are a fault, a fault standing past `util::kLinkStalenessBound` is a lost link (`Connected` false, getters `NotConnected`, `DeviceState` empty); no `LinkLostStamp` (no #521 consumer) |
| SynScan telescope |  | `AlpacaCore/src/vendors/synscan/synscan_telescope_driver.cpp` | adopted | #1247 | Applied with the state snapshot (#1247): three failed status polls are a fault, a fault standing past `util::kLinkStalenessBound` is a lost link (`Connected` false, getters and setters `NotConnected`, `DeviceState` empty, transport closed); no `LinkLostStamp` (no #521 consumer) |
| Celestron CGX |  | — | planned | — | No latch; transport errors only |
| ZWO telescope |  | `AlpacaCore/src/vendors/zwo/zwo_telescope_driver.cpp` | adopted | #1432, #1433 | Three failed mandatory GU+RA/Dec polls latch a fault; complete mandatory samples restore health. Fault age or a stalled accepted sample is bounded by 30 s; TCP close/reset and fatal fd errors are immediate loss. Explicit client reconnect only; loss stamped once. |
| Bisque |  | `AlpacaCore/src/vendors/bisque/bisque_telescope_driver.cpp` | adopted | #1367 | Applied with the state snapshot (#1367): three failed status ticks latch a fault (`Connected` stays true, the getters refuse), a fault that has stood for `util::kLinkStalenessBound` is a lost link (`Connected` false, transport released); a tick that waits behind a guide or FindHome holding the socket is not a failed tick (the wait on the wrapper has no timeout of its own; only a failed exchange counts) |
| Astroasis focuser | `PolledState` | `AlpacaCore/src/vendors/astroasis/astroasis_focuser_driver.cpp` | adopted | #1327, #1756 | `link_alive()` latch on the next transaction (#1756): a HID write or read error (not a read timeout) is a lost link at once (`Connected` false, getters `NotConnected`), cleared by the next connect; applied with the state snapshot (#1327): a poll that has not been answered for `util::kLinkStalenessBound` is a lost link (`Connected` false, transport released), three failed polls are a fault |
| iOptron iGuider camera |  | `AlpacaCore/src/vendors/ioptron/ioptron_iguider_camera_driver.cpp` | adopted | #1464 | A 10 s stall or a V4L2 error is a fault (`Connected` stays true, the image, Gain and subframe members refuse); `EIO`/`ENXIO`/`ENODEV`/`EBADF` on the fd (an unplug) is a lost link at once, and a fault standing past `util::kLinkStalenessBound` is one too (`Connected` false, members `NotConnected`, `DeviceState` empty, camera closed); no `LinkLostStamp` (no #521 consumer) |
| Camera, filter wheel and focuser SDK drivers (ZWO, QHY, SVBONY, ToupTek, Player One) |  | — | planned | — | SDK error on the next call |
| GPhoto camera |  | — | planned | — | A failed exposure is reported by `ImageReady`/`ImageArray` (`CameraState` stays Idle); a camera gone from the USB bus mid-exposure reports `Connected` false; no automatic reopen (#1064, #1204) |
| WeeWX |  | — | planned | — | HTTP fetch staleness |

Drivers adopt this record in their own later changes; the telescope state-snapshot slices apply point 3 when they land.

## Links

- Upstream issue [#970](https://github.com/open-astro/AlpacaBridge/issues/970); open-astro#445, #505, #521, #660.
- [Decision 0005](0005-task-clock.md) (the clock); `AlpacaCore/include/alpacacore/util/link_health.h`.
