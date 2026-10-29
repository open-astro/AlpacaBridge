### Added
- **ZWO AM bounded TCP probe wait** (AlpacaCore, issue #1620): explicit connections wait up to 1.5 seconds for a sibling discovery probe, avoiding a manual retry when admission succeeds before the deadline. Auto-owned endpoints still refuse immediately; explicit-pair sharing and socket-close ownership remain intact. The wrapper wait observes `cancel_pending_io()`, but public Disconnect during Connect remains deferred and does not trigger this cancellation. The deadline bounds claim admission, not the rest of connection initialization.

### Fixed
- **HTTP restart worker wakeup** (AlpacaHTTP, issue #1620 validation): a worker from the previous server generation returns its wake permit before exiting, so it cannot strand a new-generation request. The restart regression uses a two-worker pool, and ZWO identity tests wait for the fake's initial poll before reusing its serial stream.
