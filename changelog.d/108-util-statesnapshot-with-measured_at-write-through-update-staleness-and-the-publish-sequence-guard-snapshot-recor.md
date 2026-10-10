### Added
- **StateSnapshot utility** (AlpacaCore, issue #998): `util::StateSnapshot` serves a polled device frame with its `measured_at`, a stale flag, write-through updates and a publish-sequence guard, with decision record 0010 (proposed). No driver uses it yet. Covered by `tests/test_state_snapshot.cpp`.
