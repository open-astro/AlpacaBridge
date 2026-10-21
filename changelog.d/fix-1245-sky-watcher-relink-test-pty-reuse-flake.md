### Fixed (tests)

- **Sky-Watcher relink tests own the old pty index**, Sky-Watcher serial tests, issue #1245. `relink_board()` and the `Connected=true on a stale link` case now build the replacement board before `sever_link()`, so devpts cannot hand back the severed board's path when the driver's reader closes its fd first; the `replugged->slave_path() != old_path` check in `AlpacaCore/tests/test_skywatcher_serial.cpp` no longer depends on driver timing.
