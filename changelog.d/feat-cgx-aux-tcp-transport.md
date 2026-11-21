### Added
- **Celestron CGX: SkyPortal WiFi (AUX over TCP) transport in the AUX link, not yet selectable** (AlpacaCore, private, no issue): `CelestronCgxProtocol::connect_tcp()` talks to the SkyPortal adapter from source 0x20. It re-sends a read-only motor query once when its reply is lost (300 ms, within the caller's deadline) and never re-sends a motion or write command. It resyncs past collision debris and bad checksums, discards the adapter's startup backlog and treats the adapter closing the connection as a lost link. No driver, config or UI uses it yet; the USB path is unchanged.

### Added (tests)
- **CGX AUX over TCP cases** (private, no issue): a loopback fake SkyPortal adapter with per-request fault scripting, 13 transport cases and one USB guard case.
