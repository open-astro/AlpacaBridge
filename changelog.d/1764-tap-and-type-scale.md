### Changed
- **Web UI: 44 px touch targets and a 13/14/17 px type scale** (AlpacaHTTP web UI, issue #1764): a `--tap` token of 44 px sets the minimum height of tabs and buttons and gives the info "i" buttons, the Details pills and the switches an invisible 44 px hit area around their unchanged look. Text sizes come from three tokens (13, 14 and 17 px); the 25 declarations that were between 10 and 12.5 px now use 13 px, except three monospace badges (the version and build badges at 11.5 px and the WiFi signal glyph, raised from 10 to 11 px).

### Added (tests)
- **Touch target and type scale** (issue #1764): three `touch_type.test.js` cases read `style.css` for the `--tap` token on each control, the 13 px floor outside an explicit badge allow-list, and the scale tokens.
