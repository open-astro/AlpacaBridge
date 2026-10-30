### Breaking changes
- **Log archive download is serialised and smaller** (AlpacaHTTP, issue #1737): a second `GET /management/v1/logfiles?download` while one archive is building now returns Alpaca error 0x40B instead of an archive, and an archive over 32 MiB now leaves out older files it used to carry. **After upgrading:** retry after the build finishes, and fetch older days with `GET /management/v1/logfiles/{name}?download=1`.

### Fixed
- **Log archive download is capped and serialised** (AlpacaHTTP, issue #1737): `GET /management/v1/logfiles?download` is now capped at 32 MiB (was 200 MiB) and builds one archive at a time; a second request gets an Alpaca error 0x40B "Log archive build already in progress; retry shortly" instead of using more memory. The web UI shows that message, and any JSON error from the single-file download, instead of saving it as a log file; see Breaking changes.
