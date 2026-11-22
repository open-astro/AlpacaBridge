### Changed

- **One host clock write gate for the five mounts with a clock** (AlpacaCore, issue #2480): a new `util::HostClockWriteGate` (`host_clock_write_gate.h`) owns the connect-time decision whether the host clock may go into the mount, the untrusted-clock warning text, and the host clock change a driver still owes the mount; iOptron, ZWO AM, SynScan, OnStep and Celestron NexStar hold one each instead of their own copies. The wire writes stay in each driver. No behaviour change.
