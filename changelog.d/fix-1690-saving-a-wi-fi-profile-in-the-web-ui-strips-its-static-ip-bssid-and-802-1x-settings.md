### Fixed

- **Wi-Fi profile save keeps stored settings**, web UI Wi-Fi manager, issue #1690. Saving an existing client profile now reads it back from NetworkManager and changes only autoconnect, priority, the interface pin and a given passphrase, so static IP, BSSID lock, 802.1x and the stored `key-mgmt` (for example `sae`) survive. Covered by `AlpacaHTTP/tests/test_wifi_manager.cpp` ("keeps every stored setting", "sets a new passphrase on an existing profile").
