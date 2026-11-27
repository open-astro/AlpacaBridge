# Link-loss and relink policy

Status: accepted

## Context

A driver's link can fail in two ways a client cannot tell apart from `Connected`. The transport can be gone (a pulled USB adapter, a closed socket), or the transport can be fine while the device stops answering (mount unpowered, cable pulled at the mount end). Until now the Sky-Watcher serial path dropped `Connected` for the first (open-astro#445) and latched a fault for the second (#505) while `Connected` stayed true for ever. The UDP path had only the latch. The #445 rig run showed clients never learn a mount is gone when `Connected` stays true, and a relink after an outage needs the outage length (#521), which only the serial loss paths recorded.

## Decision

1. **Two stages.** *Faulted* is today's latch (`StreamLinkHealth`, `PolledLinkHealth`, driver latches): `Connected` true, the cache refused with `DriverException` "communications compromised", the fault text from `get_link_fault()`, cleared by the next good frame or reply. *Lost*: `Connected` false.
2. **When a link is lost.** (a) At once on positive evidence the transport is gone: a serial or HID node removed or re-pointed, `EIO`/`ENXIO`/`ENODEV` on the fd, `EBADF` on a serial fd too (our own fd is unusable, a driver bug, logged as an error; `util::SerialLink` owns this verdict for serial links, adopted by every serial wrapper since #2262), a socket that is itself unusable (`EBADF`, `ENOTSOCK`, `ENOTCONN` on a UDP socket), a TCP connection the peer closed or reset (EOF, `ECONNRESET`, `EPIPE`: it never recovers without a client Connect, since point 4 forbids a reopen) or a refused connect. Other UDP send and receive errors (`EHOSTUNREACH`, `ECONNREFUSED`, `ENETUNREACH`, `EADDRNOTAVAIL`, `ENETDOWN`) report reachability, which a Wi-Fi drop or a board reboot restores: they count as a no-reply exchange for the latch and the staleness bound decides. (b) When a latched fault has stood without a break past the staleness bound, `util::kLinkStalenessBound` (30 s, `util/link_health.h`), measured on the task clock of [decision 0005](0005-task-clock.md). A driver may set a longer bound with a reason in its code, never one shorter than its own latch threshold.
3. **While lost.** Operational getters and setters throw `NotConnected` (a SafetyMonitor's `IsSafe` instead returns `false` and does not throw: a lost link never reads safe, and an observatory that polls it closes rather than stops on an error, #1753); `Connected` reads false; `DeviceState` returns the empty list with no `TimeStamp`; the transport handle is closed (best effort: when an exchange holds the link at that moment, the fd may stay open until the next Connect or Disconnect); `link_lost_at` is stamped once on the task clock for every transport (`util::LinkLostStamp`).
4. **No autonomous reconnect.** A driver never reopens on its own. Only the client's `Connected=true` / `Connect()` relinks. It never short-circuits on a lost link and re-runs the connect probe. Motion on relink keeps the #521 window, now fed by `link_lost_at` on every transport, for motion the driver cannot attribute. On Sky-Watcher, motion the driver started (a GoTo, Park, FindHome or MoveAxis under way at the loss, a stop that was not confirmed, any GOTO-mode axis found after a loss) is stopped on relink whatever the outage length (#2396 AB-9, #2407). OnStep stops any motion on the relink after a recorded loss, whatever the outage length, and keeps no window (#2406). A client's Disconnect queued against an in-flight Connect reaches the driver even though `Connected` already reads false (#1289).
5. **One fault text.** `get_link_fault()` stays the single source. After the link is lost it keeps the last fault text until the next Connect or Disconnect, so the management listing shows why. There is no second health field.
6. **open-astro#660 identity gap.** Closing it is a per-driver identity query, each a vendor-protocol change with its own rig check, so drivers close it one at a time. The risk is a different device appearing on the same path between a loss and the client's reconnect. The table below records what each connect probe checks (read from the source when the row was written; any rig evidence is in the driver's vendor instructions).
7. **Slice scope.** This record, the shared helper in `util/link_health.h`, Sky-Watcher as the reference driver for both transports, fake-only tests (`tests/test_skywatcher_link_loss.cpp`). Other drivers adopt through the [Adoption table](../adoption/0009-link-loss-and-relink-policy.md).

### Connect probe: does it reject a different device? (point 6)

| Driver | Probe at connect | Rejects a different unit of the same family? | Follow-up |
| --- | --- | --- | --- |
| Sky-Watcher | `:e1` firmware/board query, board check after recovery | No: a different Sky-Watcher board passes | none, reference driver |
| SynScan | `K`+byte echo | No: any handset speaking the protocol passes | planned follow-up |
| Celestron | NexStar version/model queries | No identity check found in the wrapper | planned follow-up |
| OnStep | `:GVP#` identity string | Rejects a non-OnStep device only | planned follow-up |
| iOptron | `:MountInfo#` / `:DeviceInfo#` model | Mount: no, any four-digit code passes. iEAF, iEFW, iFR: a code naming another product is refused, a second unit of one model passes | planned follow-up |
| Gemini | `>H#` identity string (hub, flat panels) | Hub: a reply naming another Gemini product is refused. Flat panel: any `*H` reply passes and a mismatch only warns. Neither rejects a second unit | planned follow-up |
| QHY | JSON version handshake (Q-Focuser), slot-count check (CFW3) | No per-unit identity | planned follow-up |
| ZWO AM | Fresh hash-terminated non-error `:GVP#` reply and normalized twelve-hex `:GMA#` serial | Yes: refuses a different mount serial until the driver is recreated ([ZWO](../../.github/instructions/zwo.instructions.md)) | done (#1581) |

## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. A faulted link keeps `Connected` true and makes reads and writes throw `DriverException` with the fault text from `get_link_fault()`; the next good reply clears it. Scope: `AlpacaCore/src/vendors/*/*_driver.cpp`. Checked by: `AlpacaCore/tests/test_skywatcher_link_loss.cpp`, review.
2. A link is lost at once on positive evidence that the transport is gone, and when a fault has stood past `util::kLinkStalenessBound`; a driver never sets a bound shorter than its own latch threshold. Scope: `AlpacaCore/src/vendors/*/*_driver.cpp`, `AlpacaCore/include/alpacacore/util/link_health.h`. Checked by: `AlpacaCore/tests/test_skywatcher_link_loss.cpp`, `AlpacaCore/tests/test_onstep_link_loss.cpp`, review.
3. While lost: `Connected` reads false, operations throw `NotConnected` and a SafetyMonitor's `IsSafe` returns `false` without throwing (a lost link never reads safe, #1753), `DeviceState` is the empty list, the transport is closed and the loss time is stamped once. Scope: `AlpacaCore/src/vendors/*/*_driver.cpp`. Checked by: review (trace each loss path through state clearing, best-effort close and the one-time loss stamp), `AlpacaCore/tests/test_remotealpaca_safetymonitor.cpp` (the `IsSafe` answer on a lost link).
4. A driver never reopens a lost link on its own; only the client's Connect relinks, and it re-runs the connect probe. A Disconnect queued against an in-flight Connect still reaches the driver when the link was lost meanwhile. Scope: `AlpacaCore/src/vendors/*/*_driver.cpp`, `AlpacaCore/include/alpacacore/async_connectable.h`, `AlpacaCore/include/alpacacore/util/power_port_switch_driver.h`. Checked by: review (no worker reopens a lost transport; the client connect path re-runs the connect probe), `AlpacaCore/tests/test_async_connectable.cpp` ("a disconnect queued against a connect reaches a driver whose link was lost").
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

## Links

- Adoption roster: [docs/adoption/0009-link-loss-and-relink-policy.md](../adoption/0009-link-loss-and-relink-policy.md).
- Upstream issue [#970](https://github.com/open-astro/AlpacaBridge/issues/970); open-astro#445, #505, #521, #660.
- [Decision 0005](0005-task-clock.md) (the clock); `AlpacaCore/include/alpacacore/util/link_health.h`.
