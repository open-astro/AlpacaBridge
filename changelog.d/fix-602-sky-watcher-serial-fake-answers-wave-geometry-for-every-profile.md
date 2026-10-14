### Fixed (tests)

- **Sky-Watcher serial fake no longer answers Wave 100i geometry for every board** - AlpacaCore tests, issue #602. `FakeSkyWatcherSerialBoard` takes a `FakeMountProfile` and answers `:a`, `:b`, `:g`, `:q` and `:s` from it as the UDP fake does, so an EQM-35 Pro over serial advertises no home indexer and tracks on its own step period (`SkyWatcher serial - an EQM-35 board's geometry and features reach the driver`).
