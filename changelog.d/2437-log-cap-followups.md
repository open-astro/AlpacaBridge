### Removed

- **Unused `Config::set_log_max_total_size_mb()` setter** (AlpacaHTTP, issue #2437): nothing called it, and it turned a negative value into `0` (no total cap) while yaml and env keep the current value for a negative one. `logging.max_total_size_mb` is set from yaml or `ALPACAHTTP_LOG_MAX_TOTAL_SIZE_MB` only, as before.

### Fixed (docs)

- **Log size cap notes and docs agree** (AlpacaHTTP, issue #2437): the issue #1695 changelog entry no longer says a day keeps 19 parts and 100 MiB (it is 999 parts under the 1 GiB total cap of issue #2422). The worst case in the logging rules and the issue #2422 entry now say that the file being written is never deleted: with a file cap disk use can pass the total cap by up to one file, and with `max_file_size_mb: 0` today's file can grow past the total cap until midnight. `docs/troubleshooting.md` now points to the logging rules for the defaults and the deletion order instead of repeating them.

### Added (tests)

- **Start-up warning for a total cap below the file cap** (AlpacaHTTP, issue #2437): two `test_log_size_cap` cases load the caps from yaml through `init_logging` and check that the log says "smaller than logging.max_file_size_mb" only when `max_total_size_mb` is on and below `max_file_size_mb`.
