### Fixed
- **Sky-Watcher telescope: a full lock table refuses the call** (AlpacaCore, issue #1730): when one thread already holds the locks of eight drivers, a Sky-Watcher member now throws `DriverException` before it takes any lock, as OnStep does, instead of logging and then holding the state lock across its serial or UDP exchanges, as decision 0016 Rule 6 requires. No known path fills the table. This replaces the Debug-build assert that the table is not full (#1600, `fix-1600-sky-watcher-1583-follow-ups.md`); the assert that `Wire::call` drops the state lock only on a thread that holds the operation lock stays.

### Changed
- **OnStep and Sky-Watcher telescopes share one lock gate** (AlpacaCore, issue #1729): the decision 0016 gate both drivers carried (the per-thread table of held locks, `StateLock` and its two modes, dropping the state lock around an exchange, OnStep's single read) is now `util::LockGate` in `lock_gate.h`. The two drivers now share one per-thread table of eight slots, where each had its own before. Behaviour is unchanged apart from the Sky-Watcher fix above; a single read that finds the table full now refuses too, instead of going on without its mark.

### Changed (docs)
- **Driver lifecycle, OnStep and Sky-Watcher instructions** (#1729): the full-table rule from #1766 now names `util::LockGate` as the shared tracker and drops the two deviations it listed, which this change removes; the OnStep and Sky-Watcher instructions point at the shared gate and decision 0016 Rule 6.

### Added (tests)
- **Lock gate** (#1729, #1730): `test_lock_gate.cpp` covers the full table, the locks given back after a throw, the single read's operation-lock mark and the lock order. Its full-table case is the full-table test for both drivers, whose `StateLock` is the shared one.
