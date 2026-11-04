### Fixed
- **Server config** (AlpacaHTTP): `http.port` in the config file is checked like `ALPACAHTTP_PORT` and `discovery.port`: a value outside 1-65535 is ignored with a warning and the default 6800 is kept. Before, `port: 0` made the server listen on a random port no client could find, and a value that did not parse was dropped without a message (issue #1741).

### Breaking changes
- **Server config** (AlpacaHTTP): an `http.port` value with characters after the digits, such as `6810.0` or `6810x`, used to load as its leading digits (6810). It is now rejected with a warning, and the server listens on the default 6800 (issue #1741). **After upgrading:** if the log shows `Ignoring http.port`, write the port as a plain number, such as `port: 6810`.
