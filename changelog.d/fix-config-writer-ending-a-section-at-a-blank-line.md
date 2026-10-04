### Fixed
- **Saving a setting no longer duplicates a key when its section has a gap** (`AlpacaHTTP/src/http/router.cpp`): the config writer ended a section at a blank line or a column-0 comment, so a hand-edited `http:` block with a gap got a second copy of the key appended. A section now ends only at the next top-level key, and keys added to it land before its trailing blank and comment lines.
