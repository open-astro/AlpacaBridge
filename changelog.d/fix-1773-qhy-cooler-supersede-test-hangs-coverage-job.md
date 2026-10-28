### Fixed (tests)

- **QHY cooler cases no longer hang the coverage job** (AlpacaCore tests, issue #1773): `set_cooler_on(true)` now steps the fake clock while the ON path reaps the stopped PID worker, and the `SdkGate` and `WorkerStartGate` waits time out at 30 s.
