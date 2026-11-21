# Daemon privilege

Status: accepted

## Context

The server runs on the board that also holds the telescope gear, and its HTTP API has no authentication ([decision 0007](0007-lan-surface-threat-model.md)). Some of what the web UI offers needs root: setting the system clock, setting the Wi-Fi regulatory country, managing Wi-Fi through NetworkManager, installing an update with apt, and switching the apt update channel.

`debian/alpacabridge.service` runs the daemon as the `alpacabridge` user with `NoNewPrivileges=true`. Under that flag `sudo`, `pkexec` and every setuid helper fail, so the usual ways for a service to reach root are closed. The project also keeps a policy of no subprocesses in the daemon. `docs/wifi-manager-design.md` and `.github/instructions/packaging-release.instructions.md` both name the policy; neither says where it came from. This record keeps it as it stands.

Three mechanisms were built, one feature at a time: an ambient capability (Sync Time and the regulatory country), a polkit rule on NetworkManager (the Wi-Fi card, 3.4.0), and a root oneshot unit started over D-Bus (the software update and the update channel, issue #1103). The packaging instructions say never to add a fourth without a decision record. That record did not exist until this one.

## Decision

The daemon runs as an unprivileged user, starts no process, and reaches root only through these three mechanisms. Each is picked by what the operation needs.

- **An ambient capability, for a syscall the daemon makes in-process.** `CAP_SYS_TIME` for `clock_settime` and `CAP_NET_ADMIN` for the nl80211 regulatory request. systemd grants them at exec time, which works under `NoNewPrivileges=true`. `CapabilityBoundingSet` equals the ambient set, so the daemon can never hold more.
- **A polkit rule on a system D-Bus service the daemon already calls over sd-bus.** NetworkManager authorizes the `alpacabridge` user for five named actions (`debian/alpacabridge.polkit-rules`). The daemon calls NetworkManager in-process; it does not run `nmcli`.
- **A root-owned oneshot systemd unit, started over sd-bus, for root work that has no D-Bus API.** The polkit rule names each allowed unit by exact string and allows the `start` verb only (`debian/alpacabridge-update.polkit-rules`). What the unit runs is fixed in its `ExecStart`. The daemon passes no arguments. The update channel is a template unit whose instance name is the argument; the daemon takes the unit name from a two-entry table, the polkit rule names both instances, and the helper rejects any other argument.

A fourth mechanism is a change to this record, not an addition to a `debian/` file.

The service user gets device access from groups and udev rules (`plugdev`, `dialout`, `video`, `gpio`), and is kept out of the `input` group, which would let it read keyboards.

## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. The daemon's unit runs as the `alpacabridge` user with `NoNewPrivileges=true`, and its `CapabilityBoundingSet` lists the same capabilities as its `AmbientCapabilities`. Scope: `debian/alpacabridge.service`. Checked by: review (the `User=`, `NoNewPrivileges=`, `AmbientCapabilities=` and `CapabilityBoundingSet=` lines of the unit).
2. Daemon code starts no process: it never calls `fork`, `vfork`, an `exec` function, `posix_spawn`, `popen` or `system`. Scope: `AlpacaCore/src/**`, `AlpacaCore/include/**`, `AlpacaHTTP/src/**`, `AlpacaHTTP/include/**`. Checked by: review (a search of the changed files for those calls finds none).
3. A privileged operation uses an ambient capability granted in the unit, a polkit rule on a D-Bus service the daemon calls over sd-bus, or a root oneshot unit started over sd-bus; a change that reaches root another way first changes this record. Scope: `debian/alpacabridge.service`, `debian/*.polkit-rules`, `debian/alpacabridge-*.service`, `AlpacaHTTP/src/util/software_update.cpp`, `AlpacaHTTP/src/util/wifi_manager.cpp`. Checked by: review (each new privileged call maps to one of the three, and no sudoers file, setuid bit or `pkexec` call is added).
4. The polkit rule for helper units names each allowed unit by string equality and answers `YES` for the `start` verb only. Scope: `debian/alpacabridge-update.polkit-rules`. Checked by: `AlpacaHTTP/tests/test_software_update.cpp` ("polkit rule names the three units by exact string and nothing else").
5. The daemon passes a helper unit no arguments; a template unit's instance name comes from a fixed table in the daemon and never from request text. Scope: `AlpacaHTTP/src/util/software_update.cpp`, `debian/alpacabridge-channel@.service`, `debian/alpacabridge-set-update-channel`. Checked by: `AlpacaHTTP/tests/test_software_update.cpp` ("channel mapping ties the check URL, the apt component and the unit name").
6. A root helper writes only under directories that root owns, and its transcript directory is the helper unit's own `LogsDirectory`, never the daemon's. Scope: `debian/alpacabridge-software-update`, `debian/alpacabridge-set-update-channel`, `debian/alpacabridge-update.service`, `debian/alpacabridge-channel@.service`. Checked by: `AlpacaHTTP/tests/test_software_update.cpp` ("the transcript path is fixed, root-owned, and the same on both sides").
7. The package never adds the service user to the `input` group. Scope: `debian/alpacabridge.postinst`. Checked by: review (no `usermod` line names `input`).

## Alternatives rejected

- **Run the daemon as root.** Every route of an unauthenticated API would then run with full privilege. The unit's sandbox lines (`ProtectSystem=strict`, the two-capability bounding set) exist to prevent that.
- **`sudo` with a sudoers entry, `pkexec`, or a setuid helper.** All three need the process to gain privilege at exec, which `NoNewPrivileges=true` forbids. Dropping the flag to allow them would drop it for every vendor SDK loaded into the daemon as well.
- **Shell out to `nmcli`, `iw` or `apt-get` from the daemon.** It needs a subprocess, and the tool would still run as `alpacabridge` and be refused. `docs/wifi-manager-design.md` rejects it for the Wi-Fi card on these grounds.
- **A root helper for Wi-Fi.** `docs/wifi-manager-design.md` rules it out: NetworkManager already runs as root and does its own polkit check, so the daemon needs a polkit rule and no helper.
- **A helper unit that takes parameters from the request.** Request text would reach a root command line. The fixed `ExecStart` and the two-entry unit table keep it out.

## Consequences

- A new privileged feature costs packaging work: a unit, a helper script, a polkit rule and a seam for tests, where one `sudo` line would have done.
- The second and third mechanisms need `polkitd` on the host. `debian/control` lists it under `Recommends`, so a host can be installed without it.
- A helper unit's result has to travel through a file. systemd garbage-collects a finished oneshot unit, so the helper's transcript ends with a result marker that the daemon reads.
- `dh_installsystemd` is given `alpacabridge.service` only, so an upgrade never restarts the updater from inside the updater.
- Rules 1, 2, 3 and 7 are checked by review. A script that greps the source trees and the unit file would turn them into a failing check.

## How to reverse

To allow a subprocess or `sudo`: remove `NoNewPrivileges=true` from `debian/alpacabridge.service`, ship a sudoers file, and replace the sd-bus calls in `software_update.cpp` and `wifi_manager.cpp` with command invocations. The three polkit rule files, the two helper units and the `SoftwareUpdateBackend` seam then go. The cost beyond the code is the review of every command line built from a request, which today does not exist because there is none.

## Links

- [LAN surface threat model](0007-lan-surface-threat-model.md): what a LAN client can ask the daemon to do with these privileges.
- How-to rules for the root helper unit: [packaging and release instructions](../../.github/instructions/packaging-release.instructions.md#privileged-operations-from-the-daemon-polkit--root-helper-unit).
- [Software update](../software-update.md), [Wi-Fi manager design](../wifi-manager-design.md).
- Issue [#1103](https://github.com/open-astro/AlpacaBridge/issues/1103) (update channel template unit); PR [#745](https://github.com/open-astro/AlpacaBridge/pull/745) (the transcript symlink race).
