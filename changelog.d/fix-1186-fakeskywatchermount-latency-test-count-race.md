### Added (tests)

- **Sky-Watcher UDP fake counts a reply before sending it**, test fake, issue #1186. `FakeSkyWatcherMount::transactions_served()` now includes a transaction before its reply reaches the client, so a count read right after a transaction returns is final; `FakeSkyWatcherMount - a steady reply latency is paid by every transaction and counted` asserts the delta directly instead of polling for it, and no longer fails on a slow runner.
