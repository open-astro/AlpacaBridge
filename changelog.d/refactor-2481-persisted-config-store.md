### Changed

- **Persisted config store owns `config/registered_devices.json`** (AlpacaHTTP, issue #2481): the persisted device list, its two locks, the unique-ID reservation and the entry cap move out of `router.cpp` into `PersistedConfigStore` (`AlpacaHTTP/src/http/persisted_config_store.h`). It loads the file (setting a corrupt one aside, sanitizing each entry and giving it its decision 0018 uniqueId, saving the IDs back), saves, adds, replaces and removes entries, and names today's three match rules (`matches_type_and_number`, `matches_vendor_type_and_number`, `matches_vendor_when_given`). The rule that a save never runs under the list lock is now kept by the store's interface: only the store takes that lock, and its edit calls save after releasing it. Registering devices, pruning the rotator-offset and power-port stores, legacy adoption and the power-on state stay in the Router. No behavior change.

### Added (tests)

- **Persisted config store cases** (issue #2481): `AlpacaHTTP/tests/test_persisted_config_store.cpp` drives the store over a temp directory with no Router or devices: load, corrupt-file quarantine, save and reload, the three match rules, remove, edit, concurrent add and remove, uniqueId reservation and the entry cap.
