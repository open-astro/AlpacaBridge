### Changed

- **iOptron telescope: the per-axis motion owner replaces the hand-rolled slew generation** (AlpacaCore, issue #2522): `slew_operation_generation_` is gone. A slew, Park or FindHome claims both axes and a moving MoveAxis its own axis through `util::MotionOwner`, and MoveAxis(axis, 0), AbortSlew, Unpark, a connect and a disconnect cancel. The async dispatch body, the synchronous slew's stop, the refused-start undo, the motion bound watchdog's slew bound and the async landing gate each hold their GOTO's claim and test it before they touch the mount. Every slew claim owns both axes, so each of these stands down for exactly the commands that bumped the old counter. No error code, message or order changes for clients.

### Added (tests)

- **iOptron motion owner pins** (issue #2522): a MoveAxis on one axis leaves a guide pulse on the other axis running on the motion model fake, and a MoveAxis on either axis, a jog or a stop, ends an async GOTO body that has not dispatched.
