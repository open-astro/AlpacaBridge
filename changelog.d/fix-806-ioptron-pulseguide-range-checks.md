### Fixed
- **iOptron telescope `PulseGuide` now validates its arguments before the connection and park checks** (issue #806). A direction outside 0-3 or a duration outside 0-99999 ms answered `NotConnected` (0x407) on a disconnected mount and `InvalidWhileParked` on a parked one, and a generic driver error on a connected one; it now answers `InvalidValue` (0x401) in every state.
