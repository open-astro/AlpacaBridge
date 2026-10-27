### Fixed
- **QHY camera: report a mode change overtaken by disconnect** (AlpacaCore, issue #1735): ReadoutMode now raises NotConnected instead of returning success without publishing the mode, consistent with Gain and Offset.
- **QHY camera: contain all cooler cleanup exceptions** (AlpacaCore, issue #1735): non-standard worker-hook and PWM-action exceptions no longer terminate the server; cleanup releases its retained handle and permits a later retry.
- **QHY camera: share the disconnect worker-reap budget** (AlpacaCore, issue #1735): stop temperature and telemetry together and spend one two-second deadline across their joins and existing or late cooler OFF cleanup, instead of stacking up to six seconds of waits. Retained OFF work still finishes before physical handle close when the SDK returns.

### Changed
- **QHY camera: warn once per connection when PWM zero fails** (AlpacaCore, issue #1735): the first failed MANULPWM=0 action logs WARN and later failures log INFO instead of repeated ERROR messages. The driver still attempts physical cooler OFF.
