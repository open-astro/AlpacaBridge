### Fixed

- **iOptron mount, iEAF and iEFW claim their serial port exclusively, so another iOptron auto-detect cannot probe it** (iOptron, issue #995): the connected link now takes `TIOCEXCL` (released with `TIOCNXCL` on close), so a probe's open of that port fails with `EBUSY` and the probe skips it instead of flushing the connected driver's reply and writing its own query; covered by `iOptron iEAF Focuser Driver - a connected port refuses a second open` in `test_ioptron_ieaf_focuser.cpp`.
