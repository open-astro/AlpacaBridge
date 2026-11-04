### Fixed
- **ToupTek / Altair camera** (AlpacaCore): an AbortExposure or StopExposure that arrives while a StartExposure is still starting (it can wait up to 2 s behind an exposure the watchdog ended) is no longer dropped. It reported success and the new exposure then ran to its end; the driver now records it and aborts the exposure as soon as the start has begun it (issue #1996).
