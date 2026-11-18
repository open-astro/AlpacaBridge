### Changed

- **The serial link owns the lost-link verdict**, `util::SerialLink`, issue #2262. One errno rule (`util::is_serial_loss_errno()`: `EIO`, `ENXIO`, `ENODEV`, `EBADF`) now decides when a serial link is lost, in one place. The link's `read()`, `write_all()` and `read_until(SerialLink&, ...)` set a lock-free `lost()` latch and keep the text that set it ("Serial link lost (errno N)") across the port release until the next Connect or Disconnect (decision 0009 rule 5). An `EBADF` logs one error line, because an unusable fd is our bug and not a pulled cable. The Gemini focuser and the QHY Q-Focuser move to it first; the other serial wrappers follow one vendor per PR.

### Fixed

- **Gemini focuser drops Connected when its fd is unusable**, Gemini focuser, issue #2262. `EBADF` on a command write used to fail with `DriverException` "Write failed" while `Connected` stayed true over the dead fd; it is now a lost link like a pulled cable, with "Serial link lost (errno 9)" kept for `get_link_fault()`.
- **QHY Q-Focuser clears its lost-link latch on Disconnect**, QHY Q-Focuser, issue #2262. Only the next Connect cleared it before, so the wrapper still reported the link dead after the client had disconnected.
