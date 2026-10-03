### Fixed

- **A lone double quote in a plain config value no longer swallows its trailing comment** (issue #804). `location: 8" Dob  # note` now loads as `8" Dob`, and saving a setting over such a line keeps `  # note`. A double quote opens a quoted value only as the first non-space character of the value.
