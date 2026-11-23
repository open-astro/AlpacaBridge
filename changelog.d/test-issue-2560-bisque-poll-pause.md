### Fixed (tests)

- **Bisque test poll pause holds across a reconnect** (tests, issue #2560): the Bisque status-poll test pause now also pauses the `PolledState` module, so the poll thread a later `set_connected(true)` starts waits its ticks but probes nothing. The "Bisque UTCDate - bad remote replies retain errors and a good poll recovers" case reconnects in every leg and raced that fresh poll under load. Test hook only; behaviour for clients is unchanged.
