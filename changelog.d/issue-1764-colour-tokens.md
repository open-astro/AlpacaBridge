### Changed
- **Web UI: every colour is a token** (AlpacaHTTP web UI, issue #1764): the colour literals in `style.css` outside its `:root` block (the status-dot colours, the hover, danger and focus-ring shades, the inset panels, the shadows and the dialog backdrop), the inline hex colours of the vendor forms' port tables and notes in `index.html`, and the WiFi error colour that `app.js` set become `var(--token)` references to new `:root` tokens with the same values. The page looks the same. A night theme can now remap the tokens in one block.

### Added (tests)
- **Colour tokens** (issue #1764): three `colour_tokens.test.js` cases fail on a colour literal in `style.css` outside its token blocks, on an inline colour literal in `index.html` (and on a stale `style.css` or `app.js` cache-bust version), and on a colour string set from the page scripts.
