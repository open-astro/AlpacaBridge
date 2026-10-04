### Fixed

- **A single-quoted config value containing `#` is no longer cut at the hash.** `location: 'Obs #2'` now loads as `Obs #2`, and saving a setting over such a line keeps its trailing comment. A single quote protects `#` only when it is the first non-space character of the value, as a double quote does; `location: Bob's #2` is still cut at the `#`.
