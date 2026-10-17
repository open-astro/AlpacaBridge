### Fixed

- **OnStep: queries no longer cost 10 ms per reply character** (OnStep, issue #1242): `read_response()` slept 10 ms after every pass of its read loop, including passes that had just read a character, so a 14-character `:GRa#` reply took about 0.14 s even from an instant fake mount; for a `#`-terminated reply it now sleeps only when a pass read nothing (the serial `read()` already waits up to `VTIME` when no data is pending), and `:GRa#`/`:GDe#` on a TeSeek HM-17 at 9600 baud drop from 157 ms to 25 ms through the protocol wrapper. Unterminated acks (`:Sr`, `:Sd`, `:MS#`, `:GXEE#`) keep the pause, which their 100 ms idle break relies on, and take the same time as before.

### Changed (docs)

- **OnStep instructions: the "cold start" timing notes are corrected** (issue #1242): a slow first `DeviceState` after `Connect()` (~0.65 s before this fix, about 0.11 s after) means another client kept the device open, so ConformU's reconnect did nothing and the caches had expired, not that connect-time warm-up cannot help; the other slow first FAST reads in old ConformU 4.5.0 runs match ConformU's own first-use cost (`AlignmentMode`, `EquatorialSystem`, `SideOfPier`) and the per-character sleep (`Declination`). The same corrections are made in the two driver comments that repeated them, and the `:GRa#`/`:GDe#` round-trip time and ConformU's PulseGuide tolerance unit (0.07 s, not 0.07″, also in `SUPPORTED-DRIVERS.md`) are corrected too.

### Added (tests)

- **OnStep long-reply timing case** (issue #1242): a 60-character reply from the loopback fake mount must arrive in under 200 ms, which a 10 ms pause per character (600 ms) cannot meet.
