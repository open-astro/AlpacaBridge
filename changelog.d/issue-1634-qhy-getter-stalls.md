### Breaking changes
- **QHY exposure limits report an in-progress mode change instead of waiting** (AlpacaCore, issue #1634): `ExposureMin` and `ExposureMax` return `DriverException` while ReadoutMode is reinitializing and its limits are unknown. **After upgrading:** clients polling these limits concurrently with a mode change must retry after the ReadoutMode write completes.

### Fixed
- **QHY camera cached getters stay responsive during SDK reinitialization** (AlpacaCore, issue #1634): separate state publication from camera configuration I/O while preserving exposure, cancellation and disconnect serialization; for exposure-limit polling during a mode change, see Breaking changes.
- **QHY cooling and temperature updates resume after a failed mode refresh**: invalid exposure limits still block acquisition, but no longer permanently pause cooler regulation or telemetry. Exposure cancellation also stays serialized against mode initialization after a completed frame.
- **QHY cooler-off requests survive disconnect races**: record accepted OFF requests before background cleanup so reconnect does not restart regulation. A queued OFF worker cannot overwrite a newer ON request; normal OFF still sends the physical PWM-zero command.
- **QHY PulseGuide stays responsive during camera configuration**: publish guiding state and start the guide worker without waiting on the foreground configuration I/O lock. The SDK wrapper retains physical-handle serialization.
