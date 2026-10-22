### Fixed

- **A stalled serial adapter no longer blocks close, open or line restore on other ports** (SerialLink, issue #1439). `restore_serial_line_settings()` copies the recorded line settings out of the shared registry and releases its lock before calling `tcgetattr`/`tcsetattr`, so a port parked in the kernel cannot hold up `SerialLink::close()`, `open()` or the restore on another port; covered by "Serial link - a stalled line restore on one port does not block close or open on another".
