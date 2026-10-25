### Changed

- **Serial probe and QHY CFW3 reads on `util::read_until`**, Gemini focuser and flat panel, iOptron mount, iEAF and iEFW, Celestron, SynScan, ZWO mount, Sky-Watcher, WandererAstro and QHY CFW3 wrappers, issue #1630. Each auto-detect probe loop and the CFW3 `read_reply` now make one `read_until` call instead of a hand-written byte loop. A probe accepts and refuses the same replies as before; a timeout, overflow or hangup still means "not this device". Covered by the existing serial-ownership, probe and CFW3 tests, which pass unchanged.
