### Changed

- **A day's log file is now capped at 10 MiB by default** (AlpacaHTTP, issue #1695): `logging.max_file_size_mb` (env `ALPACAHTTP_LOG_MAX_FILE_SIZE_MB`, `0` = no cap; a negative or unparsable value keeps the yaml/default value) bounds one daily file. At the cap the sink writes one `Log file size cap reached` marker inside the cap and pauses file logging until the next day, so TRACE left on or a driver error loop can no longer fill the SD card; stderr output is unchanged. Covered by `test_log_size_cap.cpp`.
