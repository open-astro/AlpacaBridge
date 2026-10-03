### Fixed
- **Wi-Fi SSIDs preserve their exact bytes** (AlpacaHTTP, issue #793): expose a hex identity for SSIDs and use it for profile matching and joins, so non-UTF-8 network names remain selectable without breaking JSON responses.
