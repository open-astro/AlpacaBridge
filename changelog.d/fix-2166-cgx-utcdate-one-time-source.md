### Fixed

- **Celestron CGX direct USB: UTCDate answers from a fresh connect** (AlpacaCore, issue #2166): the CGX/CGX-L direct USB telescope refused a `UTCDate` read with `InvalidOperation` until a client wrote one, so every ConformU run from a fresh connect stopped at its first check, and `SiderealTime` (already on the host clock) had no `UTCDate` to explain it. `UTCDate`, `SiderealTime` and pointing now read one time source, as the Sky-Watcher direct driver does ([decision 0001](docs/decisions/0001-skywatcher-pointing-clock.md)): the host clock, plus a client's `UTCDate` offset only while the host is not NTP-disciplined. On a disciplined host a `UTCDate` write throws `PropertyNotImplemented` and changes nothing. The CGX backend is new in 6.0, so no stable release had the old behaviour.

### Changed (validation)

- **Celestron CGX direct USB ConformU report refreshed** (issue #2119): ConformU 4.6.0-rc.1 passes with 0 errors, 0 warnings, 0 issues and no timing misses on the bare northern CGX, on `stable/6.0` with the #2166 fix, without a pre-run `UTCDate` write. `AlpacaCore/conformu/Celestron/CGX/Linux-arm64-usb.txt`.
