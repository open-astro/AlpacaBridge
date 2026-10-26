### Changed

- **Sky-Watcher lock hardening**, Sky-Watcher driver, issue #1600. Every disconnect path now clears `connected_` before it closes the transport, as [decision 0016](docs/decisions/0016-getters-never-wait-on-device-io.md), item 4, orders, so a getter that runs during the close reads `NotConnected`. Debug and test builds assert that `Wire::call` drops the state lock only on a thread that holds the operation lock, and that the per-thread lock table is not full. Decision 0016 no longer says that no driver family has converted.
