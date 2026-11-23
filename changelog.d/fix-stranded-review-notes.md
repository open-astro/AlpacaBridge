### Fixed

- **A remotealpaca ObservingConditions link a getter saw lost no longer reads Connected true through the next Connect**, remotealpaca driver, issue #2496. When the frame aged past the loss bound while the poll waited its tick, a getter set the loss latch but `on_lost()` never ran; a Connect before the next tick then joined the poll and reset the latch with `connected_` still true, so `Connected` read true before the Connect had a frame. `connect_upstream()` now clears it after its joins. Covered by "a loss a getter latched reads disconnected through the next Connect" in `test_remotealpaca_observingconditions.cpp`. This and the entries below were pushed to #2546, #2548, #2549, #2551, #2555, #2556 and #2558 after those were queued, and the merge queue merged the approved heads without them.

### Added (tests)

- **Review-note follow-ups for the PulseGuide refusals and the host clock gate**, iOptron, OnStep and test suite, issues #2504 and #2480. A new iOptron case pins the settle-trim boundary (a pulse is refused before a trimmed GOTO is seen stopped, accepted after); the OnStep Park leg also refuses a pulse on an `I` frame with no GOTO, so the `is_parking` term has its own test; the owed-write case in `test_host_clock_write_gate.cpp` now holds a `HostClockTrustGuard`.

### Changed

- **Comments, a guard moved earlier and a compile-time check from the same review notes**, iOptron, Bisque, Sky-Watcher and ZWO, issues #2503, #2504 and #2415. iOptron comments say why the settle trim and the guide-rate sampler call the wire's `pulse_guide()` directly; Bisque builds `JogPendingGuard` before the MoveAxis gate, so every exit clears the flag, and the fake's `on_move_start` doc says a hook must not do wire I/O; `skywatcher.instructions.md` says a pulse is refused during the post-GOTO tracking restore; ZWO `static_assert`s that the MoveAxis stop epsilon sits just under 1e-9.
