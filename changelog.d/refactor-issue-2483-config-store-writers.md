### Changed

- **One writer for a stored device's fields** (AlpacaHTTP, issue #2483): the client-site write-through (`persist_client_site`) and `updatedevice`'s optics write now both call `PersistedConfigStore::update_fields`, which sets the given fields on the stored entry, skips a value that is unchanged and saves only a change. The `learnSiteFromClient` opt-out stays with the site write. The "could NOT be written" log text comes from the store, which names its own file. The six management handlers no longer call `load_persisted_devices()`: the constructor's load is the only one. No behavior change.

### Added (tests)

- **Fresh-Router handler case and `update_fields` case** (issue #2483): `test_routing.cpp` runs configureddevices, faileddevices, configuredevice, removedevice and updatedevice each as the first request of a new Router and checks that each sees the saved list; `test_persisted_config_store.cpp` drives `update_fields` directly.
