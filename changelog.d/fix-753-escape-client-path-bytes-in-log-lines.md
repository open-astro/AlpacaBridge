### Fixed

- **Client request paths are escaped before they reach the log** (AlpacaHTTP): control bytes, DEL and invalid UTF-8 in a path become `\xNN`, and the 256-byte cut no longer splits a multi-byte character.
