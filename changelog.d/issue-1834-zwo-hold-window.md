### Changed
- **ZWO AM mount: the Slewing and IsPulseGuiding windows are `util::HoldWindow`s** (AlpacaCore, issue #1834, decision 0022): the driver's two raw deadlines (`slew_force_until_`, `pulse_guiding_end_`) become `slew_hold_` and `pulse_hold_`, with the same arm, clear and read points and the same 2 s / 5 s / pulse-plus-500 ms holds. No behaviour change. The decision 0022 ZWO row is now `partial`: landing still reads the slew window, and four initiators still arm after their command. `util::HoldWindow` gains `until()` for deadline arithmetic (the Tracking-on wait and the link-staleness base).

### Added (tests)
- **ZWO hold-window pins** (issue #1834): an AbortSlew late in a long pulse pause keeps the link; Tracking-on returns at the pulse's exact end; `HoldWindow::until()`.
