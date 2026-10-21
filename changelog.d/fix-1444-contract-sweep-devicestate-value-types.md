### Added (tests)

- **Contract sweep DeviceState value types**, contract sweep, issue #1444. A new tier-2 case, "DeviceState value types match the ASCOM members", runs on every fake-connectable host: each connected `DeviceState` entry must hold the ASCOM member's type (integer, double, bool or string `TimeStamp`), and a member name the table in `AlpacaCore/tests/test_contract_sweep.cpp` does not list for the device type fails, so a new member needs a type entry. No fake emitted a wrong type, so no driver changed.
