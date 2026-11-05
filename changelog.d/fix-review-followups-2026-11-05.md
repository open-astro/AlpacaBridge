### Added (tests)

- **Exact error codes and fake-clock tests**, issue #2088. Unsupported-action and other error assertions check the exact ASCOM code; the Celestron pulse, WandererAstro link-health and switch TTL tests run on the fake task clock; new StateSnapshot, Player One switch stress, ToupTek-family wrapper and server exit-status cases.

### Changed

- **WeeWX poll**, issue #2085. The feed poll runs in an operation slot on the driver's task clock (decisions 0005 and 0006).
- **Discovery logging**, AlpacaHTTP, issue #1741. A received datagram is logged at DEBUG before it is checked, so a LAN host spraying UDP cannot fill the log.
- **Decision records**, issue #2080. Decisions 0006, 0016 and 0017 are restated without exception clauses, and the docs-drift and serial-port checks now catch the word "exception" in a rule, a `decision NNNN` citation of a missing record, and a raw read on a copied fd.
- **CI and scripts**, issues #1755 and #2089. New `duplication` job (PMD CPD duplicated-line ratchet, limit 6.9%, not required yet; the dependency snapshot lists the PMD pin); pre-flight self-test fixes; the ConformU checker's fake-report rule matches a `Fake` folder or `Fake-*` file only; failure records 0012 to 0015 carry the standard sections.

### Removed

- **`ALPACAHTTP_ENABLE_DISCOVERY`**, issue #1741: the option could not link when off; discovery is switched at runtime by `discovery.enabled`.

### Fixed

- **Argument checks before the connection check**, SynScan driver, issue #2084. SlewToCoordinates, SlewToCoordinatesAsync and SyncToCoordinates now return `InvalidValue` for an out-of-range RA or Dec while disconnected, not `NotConnected`.
- **Static web files**, AlpacaHTTP, issue #1741. The content type comes from the file's extension (`favicon.ico` is `image/x-icon`), and a directory under `/web/` is 404 instead of an empty 200.
- **Update check over https only**, AlpacaHTTP, issue #1741. The Packages index and release notes are fetched over https, redirects included.
- **Driver follow-ups**, issue #2085. A failed ZWO EAF open no longer leaves the enumerated name in `Name`; a refused QHY CFW3 move keeps the earlier failed-move text; the Gemini flat panel cannot lose the wakeup of a replaced calibrator body; the ZWO mount's UTCDate write and connect-time time sync no longer interleave.
- **Router and server**, issue #2086. A throw during a restart no longer leaves the server reporting running or refusing later restarts, and the shutdown and restart callbacks are set and read under a lock.
