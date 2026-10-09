### Changed

- **Sky-Watcher test fakes gain a steady reply latency and a transaction counter, and the UDP fake a go-silent mode** (SkyWatcher, issue #937): `set_reply_latency()` and `transactions_served()` on both fakes, `set_silent()` on the UDP fake, with cases in `test_skywatcher_async.cpp` and `test_skywatcher_serial.cpp` that drive the silent-timeout and frame-seen outcomes; test-only, no production change.
