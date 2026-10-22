### Fixed
- **Celestron CGX: use the injected clock for reference continuity** (AlpacaCore, issue #1404): the shared 500 ms checkpoint deadline and 20 ms settle waits now use the driver's task clock. Deterministic tests cover disconnect, reconnect, retained calibration and fake-clock cleanup. The default production clock and transport timeouts are unchanged.
