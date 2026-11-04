### Fixed
- **Server config** (AlpacaHTTP): `http.port` in the config file is checked like `ALPACAHTTP_PORT` and `discovery.port`: a value outside 1-65535 is ignored with a warning and the default 6800 is kept. Before, `port: 0` made the server listen on a random port no client could find, and a malformed value was dropped without a message (issue #1741).
