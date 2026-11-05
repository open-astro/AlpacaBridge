### Fixed
- **QHY CFW3: a bridge that stops taking data no longer parks a probe or a goto forever** (AlpacaCore, issue #2038): the probe and the connected exchange drained with a bare `tcdrain()`, the unbounded wait #2030 fixed on the Q-Focuser. Both now use `util::tty_drain_bounded()`; the probe treats a stall as "not this device" and a goto fails with a `DriverException` naming the replug, which `Position` raises.

### Changed
- **iOptron mount: the disconnect drain is the shared bounded helper** (AlpacaCore, issue #2039): the hand-rolled `TIOCOUTQ` loop is `util::tty_drain_bounded()` at the same 250 ms, followed by the same input flush.
- **`scripts/check_serial_port_ownership.py` fails on a bare `tcdrain()`** (tooling, issue #2038): drains go through `util::tty_drain_bounded()`.

### Added (tests)
- **Hardware-free stalled-endpoint coverage** (AlpacaCore, issue #2040): `util::tty_outq_probe_for_testing()` stands in for `TIOCOUTQ`, so the drain's deadline and flush path, the zero-timeout poll-once rule (now documented in the header), the Q-Focuser's "not taking data" refusal and probe skip (`probe_qfocuser_port()` is public for it), and the CFW3's failed goto are pinned on a pty.
