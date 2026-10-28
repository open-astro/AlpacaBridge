### Breaking changes
- **ZWO AM replacement mount admission** (AlpacaCore, issue #1581): a driver that has admitted one mount now refuses a different mount serial on later connects, including after Disconnect. **After upgrading:** restore the original mount at the configured endpoint, or save the device again in the web UI (or restart the service) to admit an intentionally replaced mount. No serial is persisted and the configured ASCOM UniqueID is unchanged.

### Fixed
- **ZWO AM reconnect identity** (AlpacaCore, issue #1581): compare each fresh mount serial with the first admitted serial before enabling configuration or motion writes; retain the pin across link loss and failed reconnects. For intentional replacement, see Breaking changes.
- **ZWO AM expected Auto refusal logging** (AlpacaCore, issue #1619): log the no-usable-candidate discovery summary at DEBUG instead of WARN, without changing connection results or transport-fault diagnostics.
