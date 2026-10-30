### Changed

- **Driver test rig rendezvous helpers**, AlpacaCore tests, issue #1897. `tick()` and `connect_and_park()` in `AlpacaCore/tests/driver_rig.h` replace the per-file fake-clock rendezvous in the Astroasis, Celestron, Gemini and QHY focuser tests, the Sky-Watcher motion-limit tests and the PolledState tests. Test code only; no driver behaviour changes. Covered by `AlpacaCore/tests/test_driver_rig.cpp`.
