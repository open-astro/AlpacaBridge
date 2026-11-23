### Fixed (docs)

- **Motion owner comments name the #2503 refusal** (AlpacaCore, issue #2583): the `motion_owner.h` header and the `test_motion_owner.cpp` table row for a PulseGuide on a MoveAxis axis now say that every mount driver refuses that pulse before it claims (#2503), so no driver reaches the owner's supersede outcome. Comments only; no behaviour change.
