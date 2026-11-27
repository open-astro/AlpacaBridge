---
applyTo: "AlpacaCore/src/vendors/remotealpaca/**,AlpacaCore/include/alpacacore/vendor/remotealpaca/**,AlpacaCore/tests/*remotealpaca*,AlpacaCore/tests/fake_alpaca_server.h,AlpacaCore/tests/alpaca_upstream_script.h"
---

### remotealpaca

Devices: ObservingConditions, SafetyMonitor, Rotator. The vendor serves a device of an upstream Alpaca server as a proxied device on this server
(`CONTEXT.md`: native device vs proxied device). Its contract is fixed by
[decision 0023](../../docs/decisions/0023-outbound-clients-in-vendor-wrappers.md) and issue #1753; read both before
adding a device type.

No SDK: the wire is the ASCOM Alpaca Device API over `util::HttpClient`, built under `ALPACACORE_ENABLE_REMOTEALPACA`
(needs libcurl). No remotealpaca file names libcurl (decision 0023 rule 2).

What every proxied device shares lives in `UpstreamProxy` (`src/vendors/remotealpaca/upstream_proxy.h`), a member of
each driver (decision 0013 section 5): the self-loop check at Connect, the forwarded Connect and Disconnect, the
branded Name and the Description, the `util::PolledState` frame with the link's fault and loss, the command slot and
the pass-through of upstream errors. A driver keeps its frame and how to measure it, its own reads at Connect, its
commands, its `transition_mutex_` and its `get_connected()`; behaviour that does not fit the shared shape stays in the
driver. A new device type adds a driver over it, not a second copy of these rules. The first InterfaceVersion of the
proxied type that has `Connect` is the driver's (`UpstreamProxy::Setup::platform7_version`): 2 for ObservingConditions,
3 for SafetyMonitor, 4 for Rotator.

#### The upstream client (`RemoteAlpacaClient`, issue #1753 slice 2)

- One client per upstream device. Calls are serialized on the client's own mutex, so a driver's poll and its
  commands never overlap on one upstream device. That mutex is the driver's I/O lock (decision 0016).
- Every call sends the client's own ClientID and the next ClientTransactionID, never a caller's. Parameter names go
  out as the caller writes them, in canonical Alpaca casing.
- A non-zero `ErrorNumber` is thrown unchanged with `<host>:<port>: ` in front of the upstream's message (decision
  0023 rule 5). Every other failed call is a `RemoteTransportError` (`DriverException`); the driver decides whether
  it is a lost link (`NotConnected`) or one failed call. A refused connection (`util::HttpConnectRefused` from the
  seam, `ECONNREFUSED` on the connect) is the subclass `RemoteConnectRefused`; an unreachable host, a timeout and a
  dropped connection are not.
- The reply's JSON is read by the wrapper's own reader in `remote_alpaca_client.cpp`; no JSON type leaves the wrapper
  (decision 0013 section 4). The per-call timeout is `kRemoteCallTimeout` (5 s), not a config field.

#### The ObservingConditions proxied device (issue #1753 slice 3)

Config (`remotealpaca_fields.h`, the same four fields for every proxied device type): `upstreamHost` (a name or an IP address, no scheme), `upstreamPort` (1 to 65535,
32323 by default), `upstreamDeviceNumber` (the upstream's own number, 0 to 2147483647) and `pollPeriodSeconds` (1 to
60, 2 by default). The factory refuses each out of range with `InvalidValue`, from both config sources.

- **Self-loop (decision 0023 rule 4).** The factory is a `bind` hook: an upstream whose host resolves to one of this
  server's listening addresses on its port is refused at configure (`BindContext::listen_addresses`, filled by the
  router from the port the server listens on; the listener binds the wildcard, so every local and loopback address
  counts) and again at every Connect (`self_loop.cpp`), with a text that names the loop. Resolving a name waits on the
  system resolver.
- **Connect** reads `InterfaceVersion`, forwards `Connect` (Platform 7, then waits for the upstream's `Connecting` to
  read false, at most 30 s) or `Connected=true` (an older upstream), checks `Connected`, reads `Name` and
  `Description` (served from then on) and asks each sensor once: an upstream `NotImplemented` (0x400) marks the
  sensor missing, and its getters, `TimeSinceLastUpdate` and `SensorDescription` answer that error unchanged. Then it
  measures the first frame, so the getters answer when Connect returns. A failure after the forwarded Connect
  forwards Disconnect. Disconnect is forwarded too, unless the link was lost. `UniqueID` is the router's stored ID.
- **Snapshot (decision 0010).** The poll measures every sensor the upstream has, each sensor's
  `TimeSinceLastUpdate`, the empty-name one and `AveragePeriod` into `util::PolledState` (rule 7, #2496), once per
  poll period, start to start; `max_age` is two periods. The driver holds no `PolledLinkHealth`, poll slot or loss
  latch of its own. A stale frame is served, never refused: the sensor getters, `AveragePeriod` and
  `TimeSinceLastUpdate` (the upstream's value plus the frame's age on the task clock). `SensorDescription`, `Name` and
  `Description` are read at Connect. An upstream error on one member is kept in the frame and thrown unchanged by its
  getter; it is an answer, not a failed poll. `DeviceState` is the base class's, from the same getters: the upstream's
  values, omitted while this server's link is faulted and empty while it is lost (the base class is `final`, so no
  extra member is added).
- **Link (decision 0009).** A poll with no usable reply is one failed exchange; three in a row latch the fault
  (`Connected` stays true, the frame is refused with `DriverException`, the next good poll clears it), and a fault
  standing, or a frame no poll or `Refresh` refreshed, for `max(util::kLinkStalenessBound, 3 x poll period)` (the
  module's `loss_bound`) is a lost link. A
  refused connection on the poll or a command is a lost link at once; on the Connect it fails the Connect, and the link
  was never up. Lost: `Connected` false, members
  `NotConnected`, `DeviceState` empty, the poll ends, the frame is no longer served; `get_link_fault()` keeps the reason until
  the next Connect or Disconnect; only the client's Connect relinks. No `LinkLostStamp` (no #521 consumer).
- **Commands.** `Refresh` (forwarded, then a new frame measured and published through the module, so its
  `measured_at` is the Refresh's; it does not feed the fault latch) and an `AveragePeriod` write (forwarded, then
  written through to the frame) run as bodies in a command `util::AsyncOperation` slot; the caller waits for the body and gets
  its error unchanged. Commands of every proxied device are serialized in caller order (`UpstreamProxy`'s command order
  mutex, held by the caller across the start and the wait), so a later command never reaches the upstream before an
  earlier one whose body the slot superseded but did not stop. A transport failure there is `DriverException`, or `NotConnected` when the link is lost.
- Actions and `Command*` are not forwarded (`ActionNotImplemented`, `NotImplemented`).

#### The SafetyMonitor proxied device (issue #1753 slice 4)

The same config, self-loop check, Connect (Platform 7 is `ISafetyMonitorV3`; a version 1 or 2 upstream gets
`Connected=true`), branded Name, snapshot, poll and link rules as the ObservingConditions device, with the same loss
bound. The Connect reads nothing of its own: the first frame is `IsSafe`, a value or the upstream's error. `IsSafe`:

- connected and not faulted: the frame's value, or the upstream's error unchanged (decision 0023 rule 5). A stale
  frame is served, never refused (decision 0010 rule 6): the upstream's last answer stands until the loss bound;
- faulted (latched, not yet lost): `DriverException` with the fault text, as every served getter (decision 0009 rule
  1); `DeviceState` then omits `IsSafe`, so it is empty;
- lost (a refused connection, a fault or a frame no poll refreshed past the loss bound, found by the poll or by the
  `IsSafe` read itself): `false`, with no exception (decision 0009 rule 3, #1753): a lost link never reads safe, and an
  observatory that polls `IsSafe` closes rather than stops on an error. `Connected` is false and `DeviceState` empty,
  as for every lost link (also the `DeviceState` read that finds the loss: the base checks `Connected` again after
  `IsSafe`), and `get_link_fault()` keeps the reason;
- never connected, connecting (a Connect, also one after a loss, ends the kept reason), after a Connect that failed
  (also one after a loss, while the upstream still refuses) or disconnected by the client: `NotConnected`, as every
  member.

It has no command: nothing runs in the command slot. Actions and `Command*` are not forwarded.

#### The Rotator proxied device (issue #1753 slice 5, #2708)

The same config, self-loop check, Connect (Platform 7 is `IRotatorV4`; a version 3 or older upstream gets
`Connected=true`), branded Name, snapshot, poll and link rules as the ObservingConditions device, with the same loss
bound.

- **Connect** reads `CanReverse` and `StepSize` once, each a value or the
  upstream's error (`StepSize` is often `NotImplemented`, served unchanged). They are static metadata: they keep
  answering while the link is faulted. `CanReverse` also answers while disconnected: the last Connect's value, true
  before any.
- **Members by version.** An `IRotatorV1` or `V2` upstream has no `MechanicalPosition`, `MoveMechanical` or `Sync`
  (IRotatorV3 added them, `UpstreamProxy::upstream_interface_version()` says which the upstream is): each answers
  `NotImplemented` naming the upstream's version, after the connection check, and
  is never called, so an older server that answers an unknown member with HTTP 400 does not fail the poll.
- **Snapshot.** The frame is `IsMoving`, `Position`, `MechanicalPosition`, `TargetPosition` and `Reverse`, each a value
  or the upstream's error. A stale frame is served, never refused (decision 0010 rule 6): each getter answers the last
  poll's value until the loss bound. A latched fault refuses the frame with `DriverException`. `DeviceState` is the
  rotator base's, from the same getters, its `TimeStamp` the frame's age (`device_state_age()`).
- **Moves** (`Move`, `MoveAbsolute`, `MoveMechanical`): a NaN or infinite angle is `InvalidValue` before the
  connection check; a finite one goes to the upstream as given, which judges its range (its `InvalidValue` passes
  through). Refused while the link is faulted. The body forwards the move, writes `IsMoving` true through when the move
  has somewhere to go (a move with nowhere to go leaves the polled value: an earlier move may still run), then reads the
  upstream's `TargetPosition` and writes it through. A read with no usable reply leaves `TargetPosition` to the next
  poll and does not fail the move. `Position` stays the measured one until the next poll.
- **Sync** is forwarded (the upstream keeps the offset; this driver stores none, so decision 0018 rule 4 has nothing to
  move) and writes `Position` through. The **Reverse** write is forwarded and written through. Both are refused while
  faulted.
- **Halt** is forwarded even while the link is faulted: the safety stop is refused only for a lost link
  (`NotConnected`). Its body writes `IsMoving` false through. A Halt sent while a move's body runs waits for it (the
  caller order above), so it reaches the upstream after the move.
- The `TargetPosition` PUT (this server's extension; IRotatorV4 has it read-only) is `NotImplemented` (0x400) once connected:
  the upstream has no member to take it.
- A refused connection on any command is a lost link at once; another call with no reply is `DriverException`.
  Actions and `Command*` are not forwarded.

Tests use the loopback `FakeAlpacaServer` (`AlpacaCore/tests/fake_alpaca_server.h`, 127.0.0.1 only, with stall,
drop, HTTP 500, bad JSON and no-ErrorNumber modes) over the real libcurl client, and `FakeHttpClient` where a case
counts calls. The device tests (`test_remotealpaca_observingconditions.cpp`, `test_remotealpaca_safetymonitor.cpp`,
`test_remotealpaca_rotator.cpp`, the contract sweep and the `[stress]` cases in `test_remotealpaca_concurrency_stress.cpp`) answer from
`AlpacaUpstreamScript` (`AlpacaCore/tests/alpaca_upstream_script.h`, a responder for `FakeHttpClient`;
`Kind::SafetyMonitor` scripts a SafetyMonitor upstream, `Kind::Rotator` a Rotator whose moves run until
`finish_move()` or a Halt) with the poll on `FakeTaskClock`. The contract sweep's snapshot
rig for the SafetyMonitor states `IsSafe`'s lost answer in `SnapshotRig::lost_answer`.

ConformU against the ASCOM Alpaca Simulators (OmniSim) runs on the rig host, never in CI. The fake ConformU row
(`scripts/conformu_fake_runs.tsv`, #2696) runs the driver over `AlpacaUpstreamScript` through `FakeHttpClient`
(`conformu_observingconditions_factories.h`), with the poll on the real task clock; it is fake evidence only.
