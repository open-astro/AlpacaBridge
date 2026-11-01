### Changed

- **Celestron CGX direct-USB ConformU re-qualification**, Celestron CGX driver, issue #1759. `AlpacaCore/conformu/Celestron/CGX/Linux-arm64-usb.txt` now holds a ConformU 4.5.3.56334 telescope pass from 2026-10-31 on native Linux arm64 (0 errors, 0 warnings, 0 issues, all 98 timing rows within target), run with both guide rates at 0.25x sidereal; site, site-derived and time values are redacted. Two attempts at the default 0.5x guide rate on the same binary stopped at a PulseGuide North ISSUE; `SUPPORTED-DRIVERS.md` records that limit.

### Fixed

- **Celestron CGX Tracking, AtHome and GuideRate no longer wait on a tracking, stop, home or slew exchange**, Celestron CGX driver, issue #1759. `Tracking` and `TrackingRate` writes, the AbortSlew and Unpark stops, FindHome, and the slew start, arrival and slow correction now run their AUX exchanges with the driver state lock released, so `Tracking`, `AtHome` and `GuideRate` answer within 300 ms while the mount holds one of those replies. During a stop, `Tracking` reports the state from before the stop instead of waiting for both stop acknowledgements. The five coordinate getters still wait for a motion start, a motion stage or a tracking write. Covered by `AlpacaCore/tests/test_celestron_cgx_getter_wait.cpp` and the stop-initiator case in `AlpacaCore/tests/test_celestron_cgx_telescope.cpp`.
