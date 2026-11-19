### Changed

- **OnStep judges a lost serial link with the shared `util::SerialLink` verdict** (issue #2262 PR 7, the last slice, decision 0009 point 2a). Over USB serial the controller link's reads go through `util::read_until(link_, ...)` and its writes through `link_.write_all()`, the auto-detect probe's too, and the wrapper's own serial errno list is gone: `EIO`, `ENXIO`, `ENODEV` and `EBADF` on the port's fd stay a lost link at once for every device on the controller (`Connected` false, operations `NotConnected`, the loss stamped once, only the client's Connect relinks). OnStep already counted `EBADF`; an `EBADF` now also logs one ERROR line naming the port, since it means a driver bug. The removed-node check, the stale-fault loss and the network link are unchanged. Every serial wrapper now reads its verdict from the link.

### Added (tests)

- **OnStep serial verdict cases** (issue #2262): in `AlpacaCore/tests/test_onstep_link_loss.cpp`, a hangup mid-reply is a lost link until the next connect, and `EBADF` on the controller's fd, on the command's write and on its reply read, is a lost link until the next connect. `replace_fd_on_tty()` moves from the ZWO mount test into `fake_pty_write.h`, which both use.
