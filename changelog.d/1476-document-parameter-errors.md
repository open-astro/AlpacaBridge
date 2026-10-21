### Changed (docs)

- **The AlpacaHTTP conformance instructions say how a missing or malformed parameter is answered** (docs, issue #1476): HTTP 200 with `InvalidValue`, raised by the router's parse helpers before the driver is called, with the reason the schema's 400 text is not applied to it. Docs only, no behaviour change.
