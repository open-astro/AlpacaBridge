### Breaking changes
- **DeviceState no longer returns a lone TimeStamp when every operational read fails** (AlpacaCore, issue #1478): this affects all ten device types, including connected devices with unavailable telemetry. **After upgrading:** clients must accept an empty state list and tolerate the absence of TimeStamp, as required by ASCOM.

### Fixed
- **Omit TimeStamp when no operational values are available** (AlpacaCore, issue #1478): all ten DeviceState builders return an empty list when no operational reads succeed, while retaining successful values and their timestamp in partial responses; for client compatibility, see Breaking changes.
