### Fixed

- **AlpacaHTTP: exception text in log lines is escaped** (issue #847). A validation message that quotes a decoded client value, such as the `UTCDate` text, reached the DEBUG or ERROR log with raw control bytes, so `%0A` could start a forged log line. `log_alpaca_exception` now passes the message through `util::escape_for_log`.
