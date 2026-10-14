### Fixed

- **Web UI asks before a hotspot or WiFi change can drop your connection** (WiFi, issue #671): turning the hotspot or the WiFi radio off, and editing the name, password or band of a running hotspot, now show the same confirmation the hotspot-on toggle already had (unlike switching client networks, never skipped because the page is on the hotspot, since that browser is the one that loses the page); Cancel restores the toggle, field or band. Turning the radio on, or editing a hotspot that is off, still saves without asking. Cases in `AlpacaHTTP/tests/web/wifi-confirm.test.js`.
