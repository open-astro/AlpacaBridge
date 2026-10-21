### Breaking changes
- **alpacabridge: a config path that cannot be read now stops the start** (AlpacaHTTP, issue #1015): `alpacabridge <path>` used to start on defaults when `<path>` was missing or unreadable, and now prints `cannot read config file` and exits 1. Pass a readable file, or no argument for the default configuration. A second argument, or an unknown option starting with `-`, now exits 2 with the usage text.

### Fixed
- **alpacabridge: `--help` printed no usage and started a full server** (AlpacaHTTP, issue #1015): `--help` and `-h` now print the usage text, including the `ALPACAHTTP_*` environment overrides, and exit 0 before any config, logging, Wi-Fi or socket work (`server_cli_help`).

### Added
- **alpacabridge `--version`** (AlpacaHTTP, issue #1015): prints the version and exits 0 without starting anything (`server_cli_version`).
