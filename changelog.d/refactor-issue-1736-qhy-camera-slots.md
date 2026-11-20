### Changed

- **QHY camera temperature, telemetry, cooler-off and pulse-guide work runs in owned background slots, never detached** (AlpacaCore, issue #1736): `CoolerOn=true` no longer waits up to 2 s for a stopped regulation call to return (#1735), and an older pulse that ends late no longer clears a newer pulse's `IsPulseGuiding`. Disconnect still waits at most 2 s for these and then goes on; removing the camera or stopping the server now waits for a call stuck inside the QHY SDK instead of leaving it running. The exposure keeps its current thread until a later change.
