### Fixed (tests)

- **iOptron telescope tests share two helpers** (tests, issue #2491): they age past the 30 s connect grace with `age_past_connect_grace()` (steps of at most 5 s) and latch the link fault with `latch_by_reads()`, so the polled-state slice changes each in one place. Test-only; no driver change and no case added or renamed.
