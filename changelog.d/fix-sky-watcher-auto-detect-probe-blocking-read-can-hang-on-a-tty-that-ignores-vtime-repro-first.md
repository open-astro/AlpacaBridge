### Fixed
- **Sky-Watcher: the serial auto-detect probe no longer hangs on a silent port whose adapter ignores VTIME** (AlpacaCore): the `:e1` probe cleared O_NONBLOCK and relied on VTIME for its read timeout, so on a USB-serial adapter that does not honour VMIN/VTIME a candidate port that never answered parked the read forever and hung the connect and the auto-detect scan. The probe now keeps the fd non-blocking and bounds every read with `poll()` against its 1500 ms budget, as the connected link already does; the other serial vendors are unchanged.

### Added (tests)
- **Sky-Watcher probe read bound** (AlpacaCore): a pty case whose fake board rewrites the line to VMIN=1/VTIME=0 and stays silent, asserting the probe gives up within its budget.
