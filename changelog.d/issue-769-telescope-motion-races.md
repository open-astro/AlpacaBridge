### Fixed
- **Celestron, SynScan and Sky-Watcher telescope motion ownership** (AlpacaCore, issue #769): aborts and replacement motions now fence stale tasks, SynScan pulse guides own independent axis timers, and Sky-Watcher rejects parked initiators without cancelling Park or reusing a prior connection's stop-wait.
