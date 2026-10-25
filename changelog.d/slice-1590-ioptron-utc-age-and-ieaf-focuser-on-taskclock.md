### Changed

- **iOptron UTC age and iEAF status cache on the task clock**, iOptron driver, issue #1590. The telescope's `UTCDate` readback now ages through `util::UtcOffset` driven by the driver's `TaskClock`, and the iEAF focuser's 100 ms status cache ages by a `TaskClock` its factories take last. Behaviour is unchanged on a real clock. Covered by `test_ioptron_telescope.cpp` (readback moves by exactly the advanced time) and `test_ioptron_ieaf_focuser.cpp` (two reads make one `:FI#` exchange, a read after `advance()` past the TTL polls again). ConformU not re-run.
