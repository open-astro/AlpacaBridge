# Power port power-on state

Status: accepted

## Context

The GPIO power-port switches (ZWO ASIAIR Pro, Plus CM4 and Plus RK3568, ToupTek StellaVita, iOptron iMate) drive 12 V DC outputs that power cameras, mounts, dew heaters and flat panels. Every one of these boards powers its ports on at boot, before AlpacaBridge runs: a `config.txt` `gpio=...=op,dh` line on the Pi boards, a boot service on the stock iMate, the pad pull-ups on the RK3568. A dew heater set to 30 % the night before came back at 100 % after every reboot, unattended (issue #300). Two earlier fixes (#422, #433) drove the ports off from `set_connected(true)`; with the per-client Connected registry every reconnect re-fired it, so a NINA restart cut a live heater or camera, and both were closed.

The libgpiod wrappers also claimed their lines with an initial value of high, so a connect powered on a port the boot had left off, and reported every port on whatever it held.

## Decision

Three parts, applied to the four GPIO power-port drivers.

- **A connect reads the ports and changes none.** The libgpiod wrappers claim their lines as-is, read each level and reconfigure them to outputs at that level (`util::claim_outputs_at_current_level`, `alpacacore/util/gpiod_output_claim.h`). The cached value follows the line, except that a cache whose steady level matches is kept, so a reconnect resumes a PWM duty (`util::reconcile_cached_value`). The RK3568 cannot read its ports (`PWM_GPIO_GET_LEVEL` returns the module's last write, not the pad), so it keeps its "on" cache and its read-only connect.
- **What a client sets is saved.** Every successful `SetSwitch` / `SetSwitchValue` writes the port's level in percent to `config/power_port_states.json`, keyed `<UniqueID>/<port>` (`util::PowerPortStateStore`), relative to the working directory like the rotator offsets ([decision 0008](0008-rotator-sync-offset-persistence.md)). A failed save logs a WARNING and the write still succeeds: the port was set, and nothing in ASCOM asks a switch to persist. A removed device's entries are pruned at the next start.
- **The power-on state is applied once per server start, never at connect.** Each device's `powerOnState` config field is `leave` (the default: never touch a port at start), `restore` (put back the saved values) or `off` (every writable port off). The router calls `AlpacaDriver::apply_power_on_state(new_boot)` for every configured device at the end of `load_persisted_devices()`. `new_boot` compares the kernel boot ID with the one the last start recorded (`config/power_port_boot_id`). On the first start after a boot, `off` turns the ports off; on a later start in the same boot (a service restart or an update), `off` restores the saved values like `restore`, so an update never cuts power to gear in use. Values `off` applies are saved too, so a restart cannot bring back an older "on". A driver that applies anything keeps its lines claimed afterwards, without reporting Connected, because a released line loses a PWM duty; the first Connect takes them over, and a Disconnect before any Connect keeps them.

Making the ports come up off before AlpacaBridge runs is an image change, per board, outside this repository.

## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. A GPIO power-port wrapper's `open()` never drives a line to a level it did not read from that line. Scope: `AlpacaCore/src/vendors/zwo/zwo_asiair_protocol_wrapper.cpp`, `AlpacaCore/src/vendors/zwo/zwo_asiair_plus_protocol_wrapper.cpp`, `AlpacaCore/src/vendors/touptek/touptek_powerbox_wrapper.cpp`, `AlpacaCore/src/vendors/ioptron/ioptron_powerbox_wrapper.cpp`, `AlpacaCore/include/alpacacore/util/gpiod_output_claim.h`. Checked by: `AlpacaCore/tests/test_zwo_asiair_switch.cpp` ("open claims each line at its level and changes none"), `AlpacaCore/tests/test_ioptron_switch.cpp` ("connecting reports the line levels and writes nothing"), `AlpacaCore/tests/test_zwo_asiair_plus_switch.cpp` ("connect writes nothing to a PWM port no client set").
2. A power-port driver writes a port at server start only from `apply_power_on_state()`, which the router calls once per start from `load_persisted_devices()`; `set_connected()` and a device added or edited later never apply it. Scope: `AlpacaHTTP/src/http/router.cpp`, `AlpacaCore/src/vendors/zwo/zwo_asiair_switch_driver.cpp`, `AlpacaCore/src/vendors/zwo/zwo_asiair_plus_switch_driver.cpp`, `AlpacaCore/src/vendors/touptek/touptek_switch_driver.cpp`, `AlpacaCore/src/vendors/ioptron/ioptron_switch_driver.cpp`. Checked by: `AlpacaHTTP/tests/test_routing.cpp` ("a start asks every device once for its power-on state and says whether it follows a reboot"), `AlpacaCore/tests/test_touptek_powerbox_seam.cpp` ("the server-start state never changes a port a client already connected to").
3. `off` turns ports off only on the first start after a boot; a start within the same boot restores the saved values, and `leave` writes nothing. Scope: `AlpacaCore/src/core/util/power_port_state_store.cpp`. Checked by: `AlpacaCore/tests/test_power_port_state_store.cpp` ("off within the same boot restores what was saved", "begin_server_start tells a reboot from a service restart").
4. Every successful client write saves the port's level in percent through `util::PowerPortStateStore`, keyed by `get_unique_id()`; a failed save never fails the write. Scope: `AlpacaCore/src/vendors/zwo/zwo_asiair_switch_driver.cpp`, `AlpacaCore/src/vendors/zwo/zwo_asiair_plus_switch_driver.cpp`, `AlpacaCore/src/vendors/touptek/touptek_switch_driver.cpp`, `AlpacaCore/src/vendors/ioptron/ioptron_switch_driver.cpp`. Checked by: `AlpacaCore/tests/test_touptek_powerbox_seam.cpp` ("a client's write is saved per UniqueID and port").
5. Lines a server start drove stay claimed through a Disconnect that no Connect preceded; only a Disconnect after a Connect, or the driver's destruction, releases them. Scope: `AlpacaCore/src/vendors/zwo/zwo_asiair_switch_driver.cpp`, `AlpacaCore/src/vendors/zwo/zwo_asiair_plus_switch_driver.cpp`, `AlpacaCore/src/vendors/touptek/touptek_switch_driver.cpp`, `AlpacaCore/src/vendors/ioptron/ioptron_switch_driver.cpp`. Checked by: `AlpacaCore/tests/test_touptek_powerbox_seam.cpp` ("restore at server start sets the saved ports and holds them without connecting").

## Alternatives rejected

- **Drive the ports off at connect (#422, #433).** Connected is per client; every reconnect would re-apply it and cut live gear.
- **Default `off`.** An upgrade would cut power to every camera and mount on the first reboot after it, unannounced. `leave` keeps today's behaviour until the user chooses.
- **Release the lines after applying.** On the Pi kernels a released output keeps its level, but a PWM duty needs its worker, and an `off` port would rely on kernel behaviour per board.
- **Store the values in the device config.** Every write would pass through the router and rewrite `registered_devices.json`, and runtime state would mix with settings (the reason decision 0008 gives).
- **Apply `off` on every start.** A service restart during a session (an apt upgrade, the web UI's update) would cut power to gear in use.

## Consequences

- A user who wants ports off at power-on picks `off`; the ports are still on between the boot and the server start (26 s measured on an ASIAIR Plus CM4, 2026-10-25). Closing that gap is an image change per board.
- With `leave`, nothing changes at start, as before; a connect now reports a port the boot left off as off.
- A boot ID file that cannot be written makes every start look like a new boot, so `off` applies on a service restart too; it is logged.
- A server start that applies anything holds the GPIO lines, so another program cannot claim them until a client connects and disconnects or the server stops.
- The RK3568 still reports every port on at the first connect of a boot under `leave`.
- Editing a device in the web UI (remove, then re-add) while a start holds its lines destroys the driver: `close()` drives a PWM port to its steady level (full on for any duty above 0), and the new driver gets no power-on pass (rule 2). Its ports keep the levels they were left at until a client sets them.

## How to reverse

Remove the `apply_power_on_state()` loop from `Router::load_persisted_devices()` and the `powerOnState` catalog fields and form selects; the saved file is then unused and can be deleted. Reverting the wrapper claim to an output request with initial values brings back the old "on at connect".

## Links

- Issue [#300](https://github.com/open-astro/AlpacaBridge/issues/300); closed attempts [#422](https://github.com/open-astro/AlpacaBridge/pull/422), [#433](https://github.com/open-astro/AlpacaBridge/pull/433).
- [Rotator Sync offset persistence](0008-rotator-sync-offset-persistence.md), [Persisted UniqueID](0018-persisted-unique-id.md).
- Owner of the shared rule: [ASCOM contract](../../.github/instructions/ascom-contract.instructions.md#gpio-power-switch--soft-pwm-drivers-general-rules). User guide: [PowerPorts.md](../../AlpacaCore/PowerPorts.md).
