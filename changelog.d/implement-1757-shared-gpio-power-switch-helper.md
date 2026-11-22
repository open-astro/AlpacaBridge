### Changed

- **The ASIAIR Pro, Plus CM4 and Plus RK3568 switches share one soft-PWM engine** (AlpacaCore, issue #1757). The two ASIAIR port wrappers were near copies of the same port table, value cache, per-port PWM worker threads and two-phase close. That code is now `util::SoftPwmPorts`, which drives either board through a small `util::GpioLineBackend` seam (libgpiod v2 for the Pro and Plus CM4, ZWO's `pwm_gpio.ko` ioctls for the RK3568), and both wrappers are deleted. Clients see no change: the same device names, descriptions, UniqueIDs, port tables, PWM defaults (1000 Hz Pro, 50 Hz Plus), connect that changes no port, and disconnect that never power-cycles a port. The iOptron iMate and ToupTek StellaVita keep their own wrappers until a follow-up. The duty-cycle edges (`PWM 50 % at 50 Hz` high at t0, low at t0 + 10 ms; 25 % low at 5 ms; the 1000, 50 and 100 Hz periods) are pinned on a fake clock in `test_soft_pwm_ports.cpp`.

### Fixed

- **A failed PWM write now stops that port and says so on the ASIAIR Plus RK3568, and SetSwitchValue on it no longer reports success** (AlpacaCore, issue #1757): the Plus PWM worker used to `break` out of its loop on a failed ioctl with no log line, while the ASCOM API went on reporting success. The engine logs at ERROR, stops that port's worker for every backend, and answers `DriverException` for that port until the next Connect; the other ports keep working. Backed by "a failed PWM write logs ERROR, stops that port's worker and no other" in `test_soft_pwm_ports.cpp`.

### Added (tests)

- **Soft-PWM engine cases** (AlpacaCore, issue #1757): `test_soft_pwm_ports.cpp` covers open, boolean and PWM writes, steady 0 and 100 %, close settling, an unreadable backend, failed writes, read-only ports and the value checks over `FakeGpioLineBackend`, with no real sleep; one `[stress]` case storms `set_value` while the engine opens and closes, in `test_zwo_asiair_concurrency_stress.cpp`. The two ASIAIR Pro wrapper cases that drove the libgpiod stub moved onto the fake with the same assertions.
