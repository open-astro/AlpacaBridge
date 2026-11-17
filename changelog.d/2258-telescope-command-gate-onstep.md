### Changed
- **Telescope command gate: one module holds the order a mount refuses a command in** (AlpacaCore), issue #2258. The new `util/telescope_command_gate.h` runs the argument checks against a driver's limits, then the driver's connected, parked, link-fault and Tracking checks in the order #2257 set, for PulseGuide, MoveAxis and the equatorial slews. The OnStep telescope is the first driver on it. No error code, message, limit or order changes for clients.

### Added (tests)
- **Telescope command gate table** (issue #2258): (member, state, argument) to the refusal a caller sees, the per-family parked-against-fault order, MoveAxis stops, and the no-limit defaults.
