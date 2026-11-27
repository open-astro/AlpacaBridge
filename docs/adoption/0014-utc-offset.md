# Mount UTC offset: adoption

The roster of [decision 0014](../decisions/0014-utc-offset.md). It is kept out of the record so a PR that
adds or changes a row needs no `ADR authorized` comment; `scripts/check_docs_drift.py` check 23
compares it with the code.

## Adoption

`scripts/check_docs_drift.py` check 23 holds this roster to the code.

Scan: `AlpacaCore/src/vendors/*/*_driver.cpp`
Symbol: `UtcOffset`

| Driver | Source | State | Issue | Notes |
| --- | --- | --- | --- | --- |
| Celestron telescope | `AlpacaCore/src/vendors/celestron/celestron_telescope_driver.cpp` | adopted | #1385 | pilot |
| SynScan telescope | `AlpacaCore/src/vendors/synscan/synscan_telescope_driver.cpp` | adopted | #1409 | TaskClock elapsed time; clears on connect and now on disconnect too |
| iOptron telescope | `AlpacaCore/src/vendors/ioptron/ioptron_telescope_driver.cpp` | adopted | #1590 | `utc_offset_` set and read with `clock_.now()` |
| OnStep telescope | `AlpacaCore/src/vendors/onstep/onstep_telescope_driver.cpp` | adopted | #1378 | with its clock ticket; clears on disconnect too |
| ZWO telescope | `AlpacaCore/src/vendors/zwo/zwo_telescope_driver.cpp` | adopted | #1522 | TaskClock elapsed time; clears on fresh connect, disconnect and failed initialization |
| Bisque telescope | `AlpacaCore/src/vendors/bisque/bisque_telescope_driver.cpp` | adopted | #1779 | Remote TheSkyX computer-clock samples in the session snapshot; TaskClock elapsed time; snapshot reset clears on connect and disconnect |
| remotealpaca telescope | `AlpacaCore/src/vendors/remotealpaca/remotealpaca_telescope_driver.cpp` | adopted | #2710 | the upstream's UTCDate in the polled frame, anchored where the poll read it; a UTCDate write anchored when it is written; TaskClock elapsed time; the frame (and its offset) is dropped on connect and disconnect |
