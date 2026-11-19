### Changed
- **Web UI: Sync Time, Restart and Shutdown move to a Server Actions card** (AlpacaHTTP web UI, issue #1764): the action bar below every tab keeps only Refresh and gains side padding, so its button no longer touches the screen edge. Sync Time, Restart Server and Shutdown Server sit in a new Server Actions card on the Server Info tab, with Shutdown in its own row below a rule, apart from Restart. Their confirm steps are unchanged. The clock-error message and `docs/troubleshooting.md` now send the operator to that card.

### Added (tests)
- **Server Actions placement** (issue #1764): four `server_actions.test.js` cases pin the bar, the card, the separated Shutdown row and the clock-error text.
