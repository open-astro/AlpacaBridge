### Fixed

- **ZWO telescope reads the snapshot frame, not per-field cache stamps** (ZWO, issue #2492): the `kFast*Ttl` constants and the tracking, equatorial and park stamps are gone. A repeated `Tracking` write is skipped only while the frame's mandatory sample is 2 s old or less, so a client write no longer renews it and an older sample sends the write (test "a repeated Tracking write is sent once the mandatory sample is over 2 s old (2492)"); `ensure_not_parked` reads the frame's parked flag on the same bound, and `Unpark` and `FindHome` publish it. ConformU not re-run.
