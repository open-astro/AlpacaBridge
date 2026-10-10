# Link-loss and relink policy

Status: accepted

## Context

A driver's link can fail in two ways a client cannot tell apart from `Connected`. The transport can be gone (a pulled USB adapter, a closed socket), or the transport can be fine while the device stops answering (mount unpowered, cable pulled at the mount end). Until now the Sky-Watcher serial path dropped `Connected` for the first (open-astro#445) and latched a fault for the second (#505) while `Connected` stayed true for ever. The UDP path had only the latch. The #445 rig run showed clients never learn a mount is gone when `Connected` stays true, and a relink after an outage needs the outage length (#521), which only the serial loss paths recorded.

## Decision

1. **Two stages.** *Faulted* is today's latch (`StreamLinkHealth`, `PolledLinkHealth`, driver latches): `Connected` true, the cache refused with `DriverException` "communications compromised", the fault text from `get_link_fault()`, cleared by the next good frame or reply. *Lost*: `Connected` false.
2. **When a link is lost.** (a) At once on positive evidence the transport is gone: a serial or HID node removed or re-pointed, `EIO`/`ENXIO`/`ENODEV` on the fd, a socket that is itself unusable (`EBADF`, `ENOTSOCK`, `ENOTCONN` on a UDP socket) or a refused connect. Other UDP send and receive errors (`EHOSTUNREACH`, `ECONNREFUSED`, `ENETUNREACH`, `EADDRNOTAVAIL`, `ENETDOWN`) report reachability, which a Wi-Fi drop or a board reboot restores: they count as a no-reply exchange for the latch and the staleness bound decides. (b) When a latched fault has stood without a break past the staleness bound, `util::kLinkStalenessBound` (30 s, `util/link_health.h`), measured on the task clock of [decision 0005](0005-task-clock.md). A driver may set a longer bound with a reason in its code, never one shorter than its own latch threshold.
3. **While lost.** Operational getters and setters throw `NotConnected`; `Connected` reads false; `DeviceState` returns the empty list with no `TimeStamp`; the transport handle is closed (best effort: when an exchange holds the link at that moment, the fd may stay open until the next Connect or Disconnect); `link_lost_at` is stamped once on the task clock for every transport (`util::LinkLostStamp`).
4. **No autonomous reconnect.** A driver never reopens on its own. Only the client's `Connected=true` / `Connect()` relinks. It never short-circuits on a lost link and re-runs the connect probe. Motion on relink keeps the #521 window, now fed by `link_lost_at` on every transport.
5. **One fault text.** `get_link_fault()` stays the single source. After the link is lost it keeps the last fault text until the next Connect or Disconnect, so the management listing shows why. There is no second health field.
6. **open-astro#660 identity gap, accepted for this slice.** Closing it is a per-driver identity query on five drivers, each a vendor-protocol change with its own rig check. The risk is a different device appearing on the same path between a loss and the client's reconnect. The table below records what each connect probe checks today (read from the source at the time of writing, not rig-verified).
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

## Alternatives rejected

- **A: `Connected` stays true for ever and the client decides** (the old latch doc comment). The #445 rig evidence shows clients never learn the mount is gone.
- **B: drop `Connected` on the first failed exchange.** It flaps on a short USB or Wi-Fi drop and ends sessions mid-exposure.
- **C: autonomous reconnect.** A mount relinked without its client can move unsupervised (#521), and ASCOM leaves connection to the client.
- **D: an identity gate on all five drivers in this slice.** Too large (point 6).

## How to reverse

Revert the PR. The bound is one constant, so "never lost by silence" is a one-line change back to the previous behaviour.

## Adoption

| Driver family | Behaviour today | Owner |
| --- | --- | --- |
| Sky-Watcher (serial, UDP) | Decision applied | this record |
| WandererAstro (cover, filter wheel, box, rotator) | Stream latch; `Connected` stays true | planned follow-up |
| Gemini PDH, flat panel, focuser | Polled latch; `Connected` stays true | planned follow-up |
| iOptron mount, iEAF, iEFW, iMate | `device_faulted_`; `Connected` stays true | planned follow-up |
| QHY Q-Focuser | `link_lost_` latch on the next transaction (#527) | planned follow-up |
| SynScan, Celestron, OnStep, Bisque | No latch; transport errors only | planned follow-up |
| Camera, filter wheel and focuser SDK drivers (ZWO, QHY, SVBONY, ToupTek, Player One, Astroasis, GPhoto) | SDK error on the next call | planned follow-up |
| WeeWX | HTTP fetch staleness | planned follow-up |

Drivers adopt this record in their own later changes; the telescope state-snapshot slices apply point 3 when they land.

## Links

- Upstream issue [#970](https://github.com/open-astro/AlpacaBridge/issues/970); open-astro#445, #505, #521, #660.
- [Decision 0005](0005-task-clock.md) (the clock); `AlpacaCore/include/alpacacore/util/link_health.h`.
