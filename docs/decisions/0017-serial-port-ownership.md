# Serial port ownership

Status: accepted

## Context

Many vendors ship the same CH340 adapter, so one vendor's auto-detect probe matches every other vendor's device. Until #1202 each wrapper opened its own tty by hand: an `O_NOCTTY` open, its own `tcsetattr`, a blocking fd with `VMIN`/`VTIME`, and a claim in the in-process registry that only some wrappers made and that never refused. Three failures followed. A Gemini flat panel scan opened the connected Gemini PDH's port at its own baud rate, and the hub stayed faulted until a restart. A port that another program retuned to `VMIN=1` parked a WandererRotator monitor and an OnStep drain in `n_tty_read`, so Disconnect hung. And a probe could open a port that a connected device of another kind was using. Sky-Watcher had already avoided the hang on its connected link with a non-blocking, `poll()`-bounded fd, as a recorded exception to the shared rule; the other drivers kept the blocking shape.

[#1435](https://github.com/open-astro/AlpacaBridge/pull/1435) fixed #1202 on `stable/5.0` with one helper, `util::SerialLink` (`util/serial_link.h`), over an RAII registry claim (`util::SerialPortClaim` in `util/serial_port_registry.h`). `main` reworked the same wrappers in parallel (link loss under [decision 0009](0009-link-loss-and-relink-policy.md), the iOptron exclusive hold, the Celestron CGX backend), so the port to `main` was its own change, [#1448](https://github.com/open-astro/AlpacaBridge/issues/1448). This record holds the rule both branches are checked against.

## Decision

1. **One way to open a serial port.** A wrapper opens a tty only through `SerialLink::open(path, kind, owner, configure, &error)`, which claims the port in the registry, opens it `O_RDWR | O_NOCTTY | O_NONBLOCK`, takes `TIOCEXCL`, applies the caller's line settings and records what the kernel kept. `close()` releases all of it. A link that must not share its tty passes `SerialExclusive::Required`, so a refused `TIOCEXCL` fails the open instead of logging a warning (the Celestron CGX, [decision 0011](0011-cgx-logical-reference-continuity.md)).
2. **Two claim kinds.** The connected link holds a `SerialClaimKind::Link` claim for as long as it is open. A probe holds a `SerialClaimKind::Probe` claim for exactly the length of the probe. A port held by a link is refused at once, with a message that names the holder, and the refusal is the connect error. A port held by a probe is waited for, bounded by `util::kSerialProbeClaimWait`; colliding probes take turns and never skip each other.
3. **Scans classify before they open.** Before probing, an `enumerate_*_ports()` scan asks `util::serial_candidate_use(path, owner)`: a port held by a connected device of another kind is skipped without being opened, and a port held by a connected device of the same kind stays in the results at its scan position without being probed, so an auto-detect index still names the same device while its sibling is connected.
4. **One owner name per wrapper**, shared by its probe and its link (`"Gemini focuser"`). It is what the sibling test compares and what a refused operator reads.
5. **The fd stays non-blocking.** Reads go through `util::tty_read()` and writes through `util::tty_write_all()`, both bounded with `poll()`; Sky-Watcher and the Celestron CGX keep their own `poll()`-bounded loops on the same non-blocking fd, which are the path through their test seams. `VMIN`/`VTIME` have no effect on a non-blocking fd, so a retune cannot park a reader. The Sky-Watcher exception becomes the rule.
6. **A retune is repaired, not reopened.** When a read times out, `tty_read()` compares the line settings with the ones `SerialLink` recorded and puts back any another program changed. A streaming reader whose link-health latch trips calls `util::restore_serial_line_settings()`. Neither closes or reopens the port, so this does not conflict with [decision 0009](0009-link-loss-and-relink-policy.md) rule 4.
7. **A held port is a stale endpoint.** After a replug or a change of plug order, the remembered port name can belong to another device. A connect that finds its remembered port held by another connected device throws `util::StaleEndpoint` (serial drivers check with `util::serial_endpoint_stale()`), so `util::connect_resolved()` re-scans. A CH340 carries no serial number, so the connect handshake, not the by-id name, proves which device is on a port.

Two questions were left open on [#1440](https://github.com/open-astro/AlpacaBridge/issues/1440) and are settled here. **(a) `TIOCEXCL` is best effort.** `SerialLink` takes `TIOCEXCL` on every open. A refused `TIOCEXCL` logs a warning and the open goes on. A link that must not share its tty opts in to `SerialExclusive::Required` (the CGX, [decision 0011](0011-cgx-logical-reference-continuity.md)). The reason: no refusal was seen on the rig (5.0.0 logs, 2026-10-08 to 2026-10-23), so making it fatal fixes no observed failure. The ground for reversal: a refusal seen on a real adapter, or another program shown to share a bridge port. **(b) No raw `read`/`write` on a link's fd.** Reads and writes go through `util::tty_read()`/`util::tty_write_all()`. The exceptions are the Sky-Watcher and Celestron CGX `poll()`-bounded loops behind their test seams (Decision 5), and `scripts/check_serial_port_ownership.py` enforces this. A `util::SerialPortLease` design proposed for #1202 (registry private to the helper, I/O owned by the helper object, a `link_lost()` method) is not adopted by this record either; adopting it later adds rules and rewrites the Decision text.

The Rules below name behaviour, not functions: `SerialLink`, `serial_candidate_use()`, `tty_read()` and `tty_write_all()` are today's implementation of them, so a later helper that keeps the behaviour (the `SerialPortLease` design) changes the Decision text and the Adoption symbol, not the Rules.

The how-to stays in the [serial and auto-detect instructions](../../.github/instructions/serial-autodetect.instructions.md).

## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. A wrapper opens, configures and locks a serial port only through the one shared open helper (the registry claim, the open, `TIOCEXCL` and the line settings in one place); no raw `O_NOCTTY` open, `tcsetattr`, `TIOCEXCL`/`TIOCNXCL` or `c_cc[VMIN]`/`c_cc[VTIME]` assignment appears outside that helper and the registry. Scope: `AlpacaCore/src/vendors/**`, `AlpacaCore/include/alpacacore/vendor/**`, `AlpacaCore/include/alpacacore/util/*.h`. Checked by: `scripts/check_serial_port_ownership.py` (CI `serial-port-ownership` job), review.
2. A connected link holds an exclusive registry claim on its port while it is open and a probe holds a probe claim only while it runs; a port held by a connected link is refused with a message that names the holder, and colliding probes wait for each other, never skip. Scope: `AlpacaCore/src/vendors/**`, `AlpacaCore/include/alpacacore/util/*.h`. Checked by: review.
3. An auto-detect scan checks each candidate's holder in the registry before it opens one: it skips a port held by a connected device of another kind, waits for a port under another probe's claim (rule 2), and keeps a port held by a connected device of its own kind at its scan position without probing it. Scope: `AlpacaCore/src/vendors/**`, `AlpacaCore/include/alpacacore/util/*.h`. Checked by: review.
4. A wrapper uses one owner name for its probe and its link. Scope: `AlpacaCore/src/vendors/**`. Checked by: review.
5. A serial fd stays non-blocking, and every read and write on it is bounded by `poll()`, so no read or write can park on the tty. Scope: `AlpacaCore/src/vendors/**`, `AlpacaCore/include/alpacacore/util/*.h`. Checked by: review.
6. Restoring retuned line settings never closes or reopens the port. Scope: `AlpacaCore/src/vendors/**`, `AlpacaCore/include/alpacacore/util/*.h`. Checked by: review.
7. A connect whose remembered serial port is gone or held by another connected device reports a stale endpoint, so the connect re-scans instead of failing. Scope: `AlpacaCore/src/vendors/**`. Checked by: review.
8. A serial driver confirms which device it reached from its connect handshake reply before it reports `Connected`; a port name, by-id or not, never identifies the device on its own. Scope: `AlpacaCore/src/vendors/**`. Checked by: review.
9. No raw `read`/`write` on a serial link's fd outside the shared read and write helpers and the named `poll()`-bounded loops (Sky-Watcher and the Celestron CGX, behind their test seams). Scope: `AlpacaCore/src/vendors/**`, `AlpacaCore/include/alpacacore/util/*.h`. Checked by: `scripts/check_serial_port_ownership.py` (CI `serial-port-ownership` job), review.
10. The shared open helper takes `TIOCEXCL` on every open; a refusal logs a warning and the open continues, except on a link that opts in to a required lock (the Celestron CGX), where the open fails. Scope: `AlpacaCore/include/alpacacore/util/*.h`. Checked by: review. <!-- allowed -->

## Adoption

`scripts/check_docs_drift.py` check 23 holds this roster to the code: an `adopted` row fails when none of its sources names `SerialLink`, and a scanned wrapper that names it without a row fails too. [#1448](https://github.com/open-astro/AlpacaBridge/issues/1448) ported the `stable/5.0` wrappers to `main` and converted the `main`-only Celestron CGX wrapper. The SynScan handset probe has its own row because it sits outside the scan.

Scan: `AlpacaCore/src/vendors/*/*_protocol_wrapper.cpp`
Symbol: `SerialLink`

| Driver | Source | State | Issue | Notes |
| --- | --- | --- | --- | --- |
| Celestron NexStar | `AlpacaCore/src/vendors/celestron/celestron_protocol_wrapper.cpp` | adopted | #1448 | Ported from #1435 |
| Celestron CGX mount, AUX focuser | `AlpacaCore/src/vendors/celestron/celestron_cgx_protocol_wrapper.cpp` | adopted | #1448 | `main` only; one owner name per device; `SerialExclusive::Required`; keeps its link across a logical disconnect ([decision 0011](0011-cgx-logical-reference-continuity.md) rule 1) |
| Gemini focuser, flat panel, PDH | `AlpacaCore/src/vendors/gemini/gemini_protocol_wrapper.cpp`, `AlpacaCore/src/vendors/gemini/gemini_flatpanel_protocol_wrapper.cpp`, `AlpacaCore/src/vendors/gemini/gemini_pdh_protocol_wrapper.cpp` | adopted | #1448 | Ported from #1435 |
| iOptron mount, iEAF, iEFW | `AlpacaCore/src/vendors/ioptron/ioptron_protocol_wrapper.cpp`, `AlpacaCore/src/vendors/ioptron/ioptron_ieaf_protocol_wrapper.cpp`, `AlpacaCore/src/vendors/ioptron/ioptron_iefw_protocol_wrapper.cpp` | adopted | #1448 | Ported from #1435; replaces the #995 exclusive hold |
| OnStep | `AlpacaCore/src/vendors/onstep/onstep_protocol_wrapper.cpp` | adopted | #1448 | Ported from #1435; keeps `main`'s 0009 link loss (#1241, #1289) |
| QHY CFW3, Q-Focuser | `AlpacaCore/src/vendors/qhy/qhy_cfw3_protocol_wrapper.cpp`, `AlpacaCore/src/vendors/qhy/qhy_qfocuser_protocol_wrapper.cpp` | adopted | #1448 | Ported from #1435 |
| Sky-Watcher | `AlpacaCore/src/vendors/skywatcher/skywatcher_protocol_wrapper.cpp` | adopted | #1448 | Ported from #1435; keeps `main`'s 0009 link loss (#981) |
| SynScan | `AlpacaCore/src/vendors/synscan/synscan_protocol_wrapper.cpp` | adopted | #1448 | Ported from #1435 |
| SynScan handset probe | `AlpacaCore/include/alpacacore/util/synscan_handset_probe.h` | adopted | #1448 | Ported from #1435; outside the scan, so listed on its own |
| WandererAstro cover, box, filter wheel, rotator | `AlpacaCore/src/vendors/wandererastro/wandererastro_protocol_wrapper.cpp`, `AlpacaCore/src/vendors/wandererastro/wandererastro_box_protocol_wrapper.cpp`, `AlpacaCore/src/vendors/wandererastro/wandererastro_filterwheel_protocol_wrapper.cpp`, `AlpacaCore/src/vendors/wandererastro/wandererastro_rotator_protocol_wrapper.cpp` | adopted | #1448 | Ported from #1435 |
| ZWO mount | `AlpacaCore/src/vendors/zwo/zwo_mount_protocol_wrapper.cpp` | adopted | #1448 | Ported from #1435 |

## Alternatives rejected

- **The per-wrapper registry calls** (`try_mark_serial_port_open`, `mark_serial_port_open`). Only some wrappers made them, `mark_serial_port_open` never refused, and a check followed by a separate open left a window another probe could use. That gap is how the PDH fault arose. An RAII claim taken inside the one open path closes it.
- **Blocking fds with `VTIME`**, the old shared rule. Some USB CDC-ACM ports ignore `VTIME`, and any program can retune `VMIN` on a shared port, which is what parked the WandererRotator and OnStep readers.
- **A production `ByteChannel`** between the wrappers and their fds. Already rejected by [decision 0013](0013-designs-to-avoid.md) section 7: `SerialLink` is a concrete, move-only owner of the fd, not an abstract interface, and the wrappers are still tested through pty fakes.
- **`TIOCEXCL` required on every link.** No refusal was seen on the rig, so a fatal refusal fixes no observed failure and would fail a connect that works today. Best effort is the default; a link that must not share its tty opts in to `SerialExclusive::Required`.
- **Leaving each wrapper to take `TIOCEXCL` on its own.** Before #1202, the Celestron CGX wrapper required it, Sky-Watcher, SynScan and iOptron took it best effort, and the other serial wrappers did not take it; one owner takes it the same way everywhere.

## Consequences

- Every serial wrapper on `stable/5.0` and `main` follows the Rules.
- A connect can wait up to `util::kSerialProbeClaimWait` (20 s, sized for the CFW3 probe) for another device's probe on its port, which can outlast the Platform 7 five-second `Connecting` window.
- The registry holds one process-wide line-settings lock, held only to copy or update a record; no terminal call (`tcgetattr`/`tcsetattr`) runs under it ([#1439](https://github.com/open-astro/AlpacaBridge/issues/1439)).

## Links

- Issues [#1202](https://github.com/open-astro/AlpacaBridge/issues/1202) (the failures), [#1440](https://github.com/open-astro/AlpacaBridge/issues/1440) (this record, and the two questions it settled), [#1448](https://github.com/open-astro/AlpacaBridge/issues/1448) (the port to `main`), [#1439](https://github.com/open-astro/AlpacaBridge/issues/1439) (the line-settings lock); PR [#1435](https://github.com/open-astro/AlpacaBridge/pull/1435).
- Rules: [serial and auto-detect instructions](../../.github/instructions/serial-autodetect.instructions.md).
- Related: [decision 0009](0009-link-loss-and-relink-policy.md) (link loss and relink), [decision 0011](0011-cgx-logical-reference-continuity.md) (CGX logical-disconnect continuity), [decision 0013](0013-designs-to-avoid.md) section 7 (no production `ByteChannel`).
