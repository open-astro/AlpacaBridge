### Changed (docs)
- **Decision 0017 settles its two open questions** (issue #1440): `TIOCEXCL` stays best effort (a refusal logs a warning and the open goes on; a link opts in to `SerialExclusive::Required`, as the Celestron CGX does), and no raw `read`/`write` runs on a serial link's fd outside `util::tty_read()`/`util::tty_write_all()` and the Sky-Watcher and Celestron CGX `poll()`-bounded loops.

### Changed (tooling)
- **`scripts/check_serial_port_ownership.py` fails a raw `read()`/`write()` on a link's fd** (`x.fd()`, `x->fd()`, `serial_fd_`; issue #1440). The allowance is per rule and per file, so Sky-Watcher and the CGX wrappers are exempt from this rule only, and an allowance whose rule no longer matches fails as stale. The `--self-test` cases cover the new shapes.
