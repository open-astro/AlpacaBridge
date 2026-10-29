### Fixed

- **A Disconnect queued against an in-flight Connect is no longer dropped when the link is lost meanwhile**, AsyncConnectable (every driver that uses it), issue #1289. `Connected` already read false, so the lost session was never reset; the connection task now always runs the queued Disconnect. Covered by `test_async_connectable.cpp`; [decision 0009](docs/decisions/0009-link-loss-and-relink-policy.md) point 4 and rule 4 now say so, and record a Disconnect that consumes the loss stamps as a rejected alternative.
