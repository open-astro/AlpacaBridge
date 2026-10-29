### Fixed
- **Report Bisque UTCDate from the remote TheSkyX clock** (issue #1779): seed and refresh time through the status snapshot, advance it without getter I/O, preserve established time through transient query failures, clear it on manual-clock mode, refuse disconnected reads, retain unavailable/malformed-time diagnostics, and explicitly reject unsupported writes. Requires TheSkyX's Use Computer Clock mode; validated with fake equipment.

### Breaking changes
- **Bisque UTCDate now reports errors instead of invented success** (issue #1779): disconnected reads return NotConnected, manual/chart-time mode returns InvalidOperation, and writes return PropertyNotImplemented. Enable Use Computer Clock and set time on the TheSkyX computer; clients must tolerate unsupported UTCDate writes. The HTTP router's existing bridge-host clock synchronization policy still runs before the vendor setter.
