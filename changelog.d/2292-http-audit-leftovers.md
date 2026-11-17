### Fixed

- **A query parameter given twice keeps its first value**, AlpacaHTTP request parsing, issue #2292. The query string kept the last copy while a PUT form body kept the first, so one request could read a duplicated parameter two ways. Both now keep the first.
- **A far-future clock no longer prunes every log**, AlpacaHTTP logging, issue #2292. Log retention counted back from today, so a client's `synctime` to a date years ahead made every real log look older than the retention and deleted it. The cutoff now counts back from the day after the newest file logged before today when that is earlier; in normal running that is today, so nothing changes.
- **A Wi-Fi passphrase with a NUL byte is refused**, Wi-Fi manager, issue #2292. The 8-63 length check counted the NUL, then NetworkManager received the passphrase as a C string cut short at it. `save_profile` and `set_ap` now refuse it.
