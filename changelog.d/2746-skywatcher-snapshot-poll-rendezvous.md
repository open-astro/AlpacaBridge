### Fixed

- **Sky-Watcher snapshot link-loss tests wait for the status poll again**, tests, issue #2746. Since the MoveAxis cap body became a clock waiter (#2729), the "UDP link pulled past the window" and "stale-frame lost link past the window" cases stepped the fake clock before the status poll had parked, and the UDP case failed most runs on arm64. Both cases now name the poll thread before the MoveAxis and step only once that thread waits on the clock. No driver change.
