### Fixed (tests)
- **SkyWatcher limits start-outside test no longer flakes** (AlpacaCore, issue #991): the "motion that starts outside a limit is not stopped" case drops the driver's position cache before starting tracking and waits on the guard's position reads instead of a fixed sleep.
