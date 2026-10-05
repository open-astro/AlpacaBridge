### Fixed

- **Client text is escaped before it reaches the log** (AlpacaHTTP): control bytes, DEL and invalid UTF-8 in a request path, a `moveaxis` body or a camera `Accept` header become `\xNN`, and the 256-byte cut no longer splits a multi-byte character.
