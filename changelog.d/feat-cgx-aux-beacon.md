### Added
- **Celestron CGX: SkyPortal WiFi adapter beacon listener, not yet reachable** (AlpacaCore, private, no issue): `listen_for_aux_beacons()` listens on UDP port 55555 for a bounded window and lists each SkyPortal adapter it hears (source address, SSID, module), for a later "Find adapter" button on the CGX form. No driver, config or web page uses it yet.

### Added (tests)
- **SkyPortal beacon cases** (private, no issue): eight cases over loopback broadcast senders (one adapter, two adapters, an empty window, garbage, a shared port, a held port, argument checks).
