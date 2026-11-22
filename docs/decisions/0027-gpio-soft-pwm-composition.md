# GPIO soft-PWM composition

Status: proposed

## Context

Four boards drive 12 V power ports from GPIO: the ZWO ASIAIR Pro and Plus CM4 (libgpiod), the ZWO ASIAIR Plus RK3568 (ioctls on `/dev/pwm-gpio-misc`), the iOptron iMate and the ToupTek StellaVita (libgpiod). Each had its own port wrapper, and the wrappers held the same engine four times: a port table, a value cache, one soft-PWM worker thread per PWM port, a two-phase close that settles each PWM line, and the claim that reads the lines and changes none ([decision 0021](0021-power-port-power-on-state.md)). Together the four source files were 1,740 lines (366, 567, 433 and 374), and a fix to one (the 2026 PWM race, the silent exit of the Plus worker on a failed write) had to be found and copied by hand. The Switch driver layer was already shared by [issue #2264](https://github.com/open-astro/AlpacaBridge/issues/2264) (`util::PowerPortSwitchDriver`); the wrappers were what was left.

Only the ASIAIR Plus tests could not run a worker on a virtual clock: every GPIO case reached the hardware through a C-symbol stub in `zwo_asiair_stubs.cpp`, so PWM cases waited on real time and a duty split could only be checked statistically.

## Decision

Compose, in two layers, with no base class ([decision 0013](0013-designs-to-avoid.md) designs 5 and 1).

- **`util::GpioLineBackend`** is the only thing that reaches a kernel: `open(device_path, lines)` claims the lines and returns the level each holds (or nothing when the backend cannot read them), `write(line_index, high)` returns false on failure, `close()` releases. `util::GpiodLineBackend` is libgpiod v2; `PwmGpioMiscBackend` is the ASIAIR Plus ioctl path and keeps the polarity inversion and the kernel index offset inside it. The seam has the shape of the other seams ([decision 0015](0015-sdk-seams.md)): an interface with a protected non-virtual destructor, a fake (`FakeGpioLineBackend`), and no decorator, because the engine serialises every call itself.
- **`util::SoftPwmPorts`** is the port table and the value and PWM engine. A board's wrapper holds it as a member; a board supplies its table, its backend and its profile strings. It owns one worker thread per PWM port, started by `open()` and joined by `close()`, and it waits for each period's absolute edges on the `TaskClock` ([decision 0005](0005-task-clock.md)). `util::OwnedSoftPwmPorts<Backend>` is the wrapper type a factory hands to `util::PowerPortSwitchDriver`.

A port is armed when its level is known: read at `open()`, or chosen by a client. `close()` settles armed PWM ports at the steady level of their duty and never drives a boolean line. A backend that cannot read its lines (the RK3568, whose `GET_LEVEL` returns the module's cache) leaves every PWM port unarmed, so a connect writes nothing and a close does not settle a port no client set.

## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. A GPIO power-port board's port table, value cache and PWM worker threads are one `util::SoftPwmPorts` held as a member; a board's driver or wrapper has no PWM thread of its own and no base class, CRTP or mixin for it. Scope: `AlpacaCore/src/vendors/zwo/zwo_asiair_switch_driver.cpp`, `AlpacaCore/src/vendors/zwo/zwo_asiair_plus_switch_driver.cpp`, `AlpacaCore/include/alpacacore/util/soft_pwm_ports.h`. Checked by: review (no `std::thread` or `pwm_loop` in a board's driver or wrapper, no class deriving from `SoftPwmPorts`).
2. Only a `util::GpioLineBackend` reaches libgpiod or the `pwm_gpio.ko` ioctls; the engine includes neither. Scope: `AlpacaCore/include/alpacacore/util/soft_pwm_ports.h`, `AlpacaCore/src/core/util/soft_pwm_ports.cpp`. Checked by: the core build, which links no libgpiod and fails on a `gpiod.h` include, and review (no `ioctl` or `gpiod_` call in the engine).
3. Every backend write returns its result. A failed write logs at ERROR, stops that port's worker for every backend, and answers `DriverException` from `set_value` on that port until the next `open()`. Scope: `AlpacaCore/src/core/util/soft_pwm_ports.cpp`, `AlpacaCore/include/alpacacore/util/gpiod_line_backend.h`, `AlpacaCore/src/vendors/zwo/pwm_gpio_misc_backend.cpp`. Checked by: `AlpacaCore/tests/test_soft_pwm_ports.cpp` ("a failed PWM write logs ERROR, stops that port's worker and no other"). <!-- allowed -->
4. Period waits and edges go through the `TaskClock`; the engine never sleeps or reads `std::chrono::steady_clock` itself. Scope: `AlpacaCore/src/core/util/soft_pwm_ports.cpp`. Checked by: `AlpacaCore/tests/test_soft_pwm_ports.cpp` ("PWM 50 % at 50 Hz is one high at t0 and one low 10 ms later": a raw sleep leaves the fake clock without a waiter and the case fails).
5. Lock order is `lifecycle_mutex_`, then `io_mutex_` (every backend call), then `mutex_` (state, a leaf never held across a backend call or a join); `close()` signals the workers under `mutex_`, joins outside it, and settles and releases under `io_mutex_`. Scope: `AlpacaCore/src/core/util/soft_pwm_ports.cpp`, `AlpacaCore/include/alpacacore/util/soft_pwm_ports.h`. Checked by: `AlpacaCore/tests/test_soft_pwm_ports.cpp` ("close waits for a boolean write in flight") and the `[stress]` case "set_value storm during close" in `AlpacaCore/tests/test_zwo_asiair_concurrency_stress.cpp`.
6. `close()` settles each armed PWM port at the steady level of its duty (above 0 high) and never drives a boolean line; a PWM port no client armed on a backend that cannot read its lines is neither written nor settled. Scope: `AlpacaCore/src/core/util/soft_pwm_ports.cpp`. Checked by: `AlpacaCore/tests/test_soft_pwm_ports.cpp` ("close settles each armed PWM line at its steady level and joins the workers", "on a backend that cannot read, an unarmed PWM port is neither written nor settled").
7. `open()` claims the lines at the level they hold and writes none ([decision 0021](0021-power-port-power-on-state.md) rule 1). Scope: `AlpacaCore/src/core/util/soft_pwm_ports.cpp`, `AlpacaCore/include/alpacacore/util/gpiod_line_backend.h`, `AlpacaCore/src/vendors/zwo/pwm_gpio_misc_backend.cpp`. Checked by: `AlpacaCore/tests/test_soft_pwm_ports.cpp` ("open claims the writable lines once and writes nothing").
8. The ASIAIR Plus kernel index offset and polarity inversion live in `PwmGpioMiscBackend`, never in a port table or a driver. Scope: `AlpacaCore/src/vendors/zwo/pwm_gpio_misc_backend.cpp`, `AlpacaCore/src/vendors/zwo/zwo_asiair_plus_switch_driver.cpp`. Checked by: `AlpacaCore/tests/test_zwo_asiair_plus_switch.cpp` ("every ISwitchV3 member answers while connected" reads the raw level per kernel index; a flipped polarity or offset fails it).
9. A test of the engine runs over `FakeGpioLineBackend` and `FakeTaskClock` and does not sleep; a wait for something that must not happen is a bounded wait on a condition variable. Scope: `AlpacaCore/tests/test_soft_pwm_ports.cpp`. Checked by: review (no `sleep_for` in the file).

## Alternatives rejected

- **A `GpioPowerSwitchDriver` base class.** Rejected by [decision 0013](0013-designs-to-avoid.md) design 5: the driver layer was already one class template (#2264), and a second base would give each board two hooks to override.
- **Share only the Switch method bodies and keep four wrappers.** The wrappers were the larger copy and held the subtle part (boot-level preserve, two-phase close, settle); the method bodies were forwarders.
- **One backend only (libgpiod) and keep the RK3568 wrapper separate.** The RK3568 shares the whole engine and differs only in how a level reaches the pin; a backend interface is the smallest seam that makes it one engine.
- **Hardware or kernel PWM.** Unavailable or unreachable on every board shipped.
- **Keep the C-symbol stubs as the only test fake.** They cover two boards, cannot give a virtual clock, and need a real second per PWM case; a fake backend gives every board the same cases and a `[stress]` case.
- **A raw `steady_clock` and `sleep_until` in the worker.** New code in a module several tickets will test; [decision 0005](0005-task-clock.md) puts waits on the clock, and `FakeTaskClock` makes the duty split exact.

## Consequences

- The ASIAIR Pro and Plus lose 1,074 lines of wrapper source and header (366, 567, 61 and 80) and gain 767 lines of engine and backends, comments included; the iMate and StellaVita wrappers (433 and 374 source lines) are what the follow-up replaces.
- The engine is core code: it builds and its cases run on every host, while the two backends need libgpiod or the ASIAIR Plus kernel module and build on arm64 only.
- `set_value` on a PWM port whose worker stopped now answers an error where the old wrappers reported success.
- SET_MODE and ENABLE on the RK3568 go once per line since the open, where the old boolean path sent them with every write.
- Until the iMate and StellaVita move over, their two wrappers still carry their own copy of the engine, and the soft-PWM rule in the ASCOM contract instructions says so.

## Adoption

Scan: `AlpacaCore/src/vendors/*/*_driver.cpp`
Symbol: `OwnedSoftPwmPorts`

| Driver | Source | State | Issue | Notes |
| --- | --- | --- | --- | --- |
| ZWO ASIAIR Pro and Plus CM4 switch | `AlpacaCore/src/vendors/zwo/zwo_asiair_switch_driver.cpp` | adopted | #1757 | over `util::GpiodLineBackend` |
| ZWO ASIAIR Plus RK3568 switch | `AlpacaCore/src/vendors/zwo/zwo_asiair_plus_switch_driver.cpp` | adopted | #1757 | over `PwmGpioMiscBackend` |
| iOptron iMate PowerBox switch | — | planned | #1757 | follow-up: `ioptron_powerbox_wrapper.cpp` is deleted |
| ToupTek StellaVita switch | — | planned | #1757 | follow-up: `touptek_powerbox_wrapper.cpp` is deleted |

## How to reverse

Reverting the ASIAIR Pro or the Plus restores its wrapper and driver from the commit before this record, and the other board is unaffected. The engine and the backends can stay as unused code or go in the same revert. This record then goes `superseded`, and the soft-PWM rule in the ASCOM contract instructions returns to its earlier text. Clients see no difference either way: port tables, frequencies, boot-level preserve and the no-power-cycle disconnect are the same before and after.

## Links

- Issue [#1757](https://github.com/open-astro/AlpacaBridge/issues/1757): the shared GPIO power-switch helper.
- [Power port power-on state](0021-power-port-power-on-state.md), [Task clock](0005-task-clock.md), [SDK seams](0015-sdk-seams.md), [Getters never wait on device I/O](0016-getters-never-wait-on-device-io.md), [Designs to avoid](0013-designs-to-avoid.md).
- Rules for the drivers: the GPIO section of [the ASCOM contract instructions](../../.github/instructions/ascom-contract.instructions.md#gpio-power-switch--soft-pwm-drivers-general-rules).
