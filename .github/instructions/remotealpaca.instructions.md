---
applyTo: "AlpacaCore/src/vendors/remotealpaca/**,AlpacaCore/include/alpacacore/vendor/remotealpaca/**,AlpacaCore/tests/*remotealpaca*,AlpacaCore/tests/fake_alpaca_server.h,AlpacaCore/tests/alpaca_upstream_script.h"
---

### remotealpaca

Devices: ObservingConditions, SafetyMonitor, Rotator, FilterWheel, Switch, Telescope, Focuser, Camera. The vendor serves a device of an upstream Alpaca server as a proxied device on this server
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
3 for SafetyMonitor, FilterWheel and Switch, 4 for Rotator, Telescope, Focuser and Camera.

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
- `get_image_array()` (the Camera, #2703) is the one call that does not hold the client's mutex across its request (rule
  6): it takes its ClientTransactionID under it and then runs beside the other calls, since a full frame can take a
  minute and the poll would wait that long, its frame aging past the loss bound. It GETs `imagearray` with
  `Accept: application/imagebytes` through `util::HttpClient::get_with()` (its own body cap and timeout) and decodes
  ImageBytes (Byte, Int16, UInt16, Int32, and UInt32 up to the Int32 range; rank 2, or rank 3 with three planes) from
  the wire's x-major order into the row-major `ImageArray`. A JSON reply is read for its `ErrorNumber` only: a JSON
  image is a transport error (a full frame as JSON text is far too large to parse here).

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
  through). Refused while the link is faulted. The body forwards the move and writes `IsMoving` true through:
  `MoveAbsolute` and `MoveMechanical` always (the frame's angle can be a poll period old, so it cannot say the rotator
  is already there), a relative `Move` unless its offset is 0 (that one leaves the polled value: an earlier move may
  still run). Then it reads the upstream's `TargetPosition` and writes it through. A read with no usable reply leaves `TargetPosition` to the next
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

#### The FilterWheel proxied device (issue #1753 slice 5, #2706)

The same config, self-loop check, Connect (Platform 7 is `IFilterWheelV3`; a version 1 or 2 upstream gets
`Connected=true`), branded Name, snapshot, poll and link rules as the ObservingConditions device, with the same loss
bound. Every FilterWheel member exists in every interface version, so none maps to `NotImplemented` by version.

- **Connect** reads `Names` and `FocusOffsets` once (`RemoteAlpacaClient::get_int_list` reads the offsets). Every
  IFilterWheel has both, so an upstream error on either fails the Connect with that error unchanged, as does a wheel
  with no slot. The slot data lives in `util::FilterWheelSlots` (the shared FilterWheel rule), with the upstream's
  slot count published: the upstream's lineup replaces what this server held at each Connect (a delta from the
  native wheels, whose config wins over what the wheel stores: a proxied wheel has no lineup config, the upstream's
  is the wheel's). A names or offsets write (the AlpacaBridge PUT extension) changes this server's copy only, never
  the upstream's, and is checked against the count while connected; a lost link keeps the count until the
  Disconnect, which clears it.
- **Snapshot.** The frame is `Position`, a value or the upstream's error: -1 while the upstream's wheel moves, as it
  reports. A stale frame is served; a latched fault refuses it with `DriverException`. `Names` and `FocusOffsets`
  keep answering while faulted and while disconnected. `DeviceState` is the filter wheel base's, its `TimeStamp` the
  request time (the base has no `device_state_age()`).
- **Position write.** A negative slot is `InvalidValue` before the connection check, a slot past the count
  `InvalidValue` once connected. It is refused while the link is faulted. Its body forwards the write and writes
  `Position` -1 through, even for the slot the last poll saw (another client may have turned the wheel since), so a
  client reads the move at once; the next poll reports where it landed, and an upstream error on the write passes
  through. Writes reach the upstream in caller order (the caller order above): a second write sent while the first
  one's body runs waits for it. A refused connection is a lost link at once; another call with no reply is `DriverException`. Actions and `Command*` are not forwarded.

#### The Switch proxied device (issue #1753 slice 5, #2709)

The same config, self-loop check, Connect (Platform 7 is `ISwitchV3`; a version 2 upstream gets `Connected=true`),
branded Name, snapshot, poll and link rules as the ObservingConditions device, with the same loss bound.

- **Connect** reads `MaxSwitch`, then for each switch its name, description, `MinSwitchValue`, `MaxSwitchValue`,
  `SwitchStep`, `CanWrite` and (`ISwitchV3`) `CanAsync`, each a value or the upstream's error, served unchanged. They
  are static metadata: they keep answering while the link is faulted. An upstream error on `MaxSwitch`, or a count
  outside 0 to `kMaxProxiedSwitches` (64), fails the Connect.
- **Snapshot.** The frame is `GetSwitch` and `GetSwitchValue` of every switch, and `StateChangeComplete` of each switch
  whose `CanAsync` the Connect read true; a switch that is not asynchronous changes within its write, so its
  `StateChangeComplete` is true. The poll is therefore two calls per switch (three for an asynchronous one), one at a
  time on the client's mutex, which is why `MaxSwitch` is bounded. A stale frame is served; a latched fault refuses
  it with `DriverException`. `DeviceState` is the switch base's, from the same getters, its `TimeStamp` the frame's
  age (`device_state_age()`).
- **Ids and values.** A negative id and a non-finite value are `InvalidValue` before the connection check; an id at or
  past `MaxSwitch` and a value outside the switch's range are `InvalidValue` after it, with nothing sent.
- **Writes** (`SetSwitch`, `SetSwitchValue`, `SetAsync`, `SetAsyncValue`, `SetSwitchName`) are refused while the link
  is faulted, run in the command slot in caller order (the proxy's command order), and pass the upstream's refusal (a
  read-only switch) through unchanged. Each write-through sets only what that write set, from its own arguments and
  the switch's static range, never from a frame read before the command: `SetSwitch` writes `GetSwitch` and
  `GetSwitchValue` (the range's end), `SetSwitchValue` writes `GetSwitchValue` and `GetSwitch` (true above the
  minimum), `SetAsync` and `SetAsyncValue` write `StateChangeComplete` false for an asynchronous switch (the poll then
  measures the upstream's change), and `SetSwitchName` replaces the name the Connect read.
- **ISwitchV2 upstream.** It has no asynchronous members (`UpstreamProxy::upstream_interface_version()` says which the
  upstream is): `CanAsync` is false without asking it, `StateChangeComplete` is true, and `SetAsync` and
  `SetAsyncValue` are `MethodNotImplemented` with nothing sent. `CancelAsync` is the router's `NotImplemented` for
  every Switch driver.
- A refused connection on a write is a lost link at once; another call with no reply is `DriverException`. Actions and
  `Command*` are not forwarded.

#### The Telescope proxied device (issue #1753 slice 5, #2710)

The same config, self-loop check, Connect (Platform 7 is `ITelescopeV4`; an `ITelescopeV3` upstream gets
`Connected=true`), branded Name, snapshot, poll and link rules as the ObservingConditions device, with the same loss
bound. The mount rules apply on top (AGENTS.md: ConformU telescope runs on a bare mount only, also through a proxy).

- **Connect** refuses an upstream older than `ITelescopeV3` (every Alpaca telescope is V3 or later; V4 added only the
  members this server answers itself), then reads once, each a value or the upstream's error: every `Can*` but
  `CanSlew` and `CanSlewAltAz`, `CanMoveAxis` and `AxisRates` for axes 0 to 2, `AlignmentMode`, `EquatorialSystem`,
  the optics, the site, `TrackingRate`, `TrackingRates`, the rate offsets, the guide rates, the targets,
  `DoesRefraction` and `SlewSettleTime`. They answer while the link is faulted. This server's writes forward them and
  write them through; a change another client makes upstream shows at the next Connect. The `Can*` getters answer
  while disconnected (the last Connect's answer, false before any) under one state-lock hold.
- **Snapshot.** The frame is the twelve ITelescopeV4 `DeviceState` members, twelve GETs per poll: `RightAscension`,
  `Declination`, `Altitude`, `Azimuth`, `SiderealTime`, `UTCDate`, `Slewing`, `Tracking`, `AtPark`, `AtHome`,
  `SideOfPier`, `IsPulseGuiding`. `UTCDate` and `SiderealTime` are served advanced by the frame's age. A stale frame
  is served; a latched fault refuses it; `DeviceState` is the telescope base's, its `TimeStamp` the frame's age.
- **Commands** refuse in the gate's order (`util/telescope_command_gate.h`): the argument ranges every telescope
  shares (`InvalidValue`), connected, the link fault; the upstream judges parked, Tracking and its own ranges, and its
  error passes through. The async slews, `Park`, `FindHome`, a `SideOfPier` write and a non-zero `MoveAxis` write
  `Slewing` true through; only a poll writes it false, never a fault or a loss. `PulseGuide` writes `IsPulseGuiding`
  true, `Unpark` `AtPark` false, the syncs the synced position; a coordinate slew or sync reads the upstream's targets
  back (a read with no reply leaves them). `DestinationSideOfPier` is forwarded in the command slot.
- **Stops.** `AbortSlew` and `MoveAxis(axis, 0)` are forwarded even while the link is faulted (refused only for a lost
  link); `AbortSlew` leaves `Slewing` to the poll (ITelescopeV4: true until the motion stopped).
- **Not proxied:** the synchronous slews (`SlewToCoordinates`, `SlewToTarget`, `SlewToAltAz`) answer
  `NotImplemented` after their argument and connection checks, and `CanSlew` and `CanSlewAltAz` read false
  (ITelescopeV4 deprecates them, and an Alpaca mount should not implement them). The optics setters (config
  injection) are `NotImplemented`: the optics are the upstream's.
- **MoveAxis cap.** A non-zero `MoveAxis` starts a per-axis slot body (`util::run_motion_deadline()`,
  `util::kMaxMoveAxisDuration`), because the upstream may cap nothing. Its verified stop sends `MoveAxis(axis, 0)` as
  a command (caller order) and reads `Slewing`, `RightAscension` and `Declination` live from its own thread; while
  the other axis runs this server's `MoveAxis`, `Slewing` reads stopped for it and the other coordinate is pinned. The
  outcome is kept for `Slewing` until the next motion command or Connect. It starts inside the move's command body
  and stops at its deadline only an axis this server still moves. A renewal supersedes it, a stop or `AbortSlew` the
  upstream took cancels it, Connect and Disconnect join it before the command slot.
- **Relink stop.** A link lost while this server's GoTo, Park, FindHome, flip or `MoveAxis` ran (and no poll since
  read `Slewing` false), or while any axis ran this server's `MoveAxis`, owes the stop (`util::RelinkMotionStop`,
  kept across a Disconnect): the next Connect sends `AbortSlew` and `MoveAxis(axis, 0)` on every movable axis before
  the first frame, whatever `Slewing` reads. An upstream error answer counts as sent; a call with no reply fails the
  Connect and keeps it owed. A motion PUT makes the motion this server's before it goes out (a PUT with no reply may
  have started the mount); only an upstream error answer gives it back. A `MoveAxis` PUT with no reply also gets a
  best-effort stop at once, and stays capped when that stop gets no reply either.
- **Disconnect** stops each axis this server's `MoveAxis` still runs, before the forwarded Disconnect. The stops run
  as a command, so they queue behind a move already sent; from the Disconnect's start a motion body refuses its PUT
  (`NotConnected`), and the caps are joined after the stops. The first stop with no reply owes the relink stop. `AbortSlew` and
  `MoveAxis(axis, 0)` cancel a cap only once the upstream answered the stop.

#### The Focuser proxied device (issue #1753 slice 5, #2707)

The same config, self-loop check, Connect (Platform 7 is `IFocuserV4`; a version 3 or older upstream gets
`Connected=true`), branded Name, snapshot, poll and link rules as the ObservingConditions device, with the same loss
bound.

- **Connect** reads `Absolute`, `MaxStep`, `MaxIncrement`, `StepSize` and `TempCompAvailable` once, each a value or the
  upstream's error (an upstream that lacks one, `StepSize` most often, answers `NotImplemented`, served unchanged).
  They are static metadata: they keep answering while the link is faulted. `Absolute` also answers while
  disconnected: the last Connect's value, true before any.
- **Snapshot.** The frame is `IsMoving`, `Position`, `Temperature` and `TempComp`, each a value or the upstream's
  error. A stale frame is served, never refused (decision 0010 rule 6): each getter answers the last poll's value until
  the loss bound. A latched fault refuses the frame with `DriverException`. `DeviceState` is the focuser base's, from
  the same getters, its `TimeStamp` the frame's age (`device_state_age()`).
- **Move** clamps an absolute target to `0..MaxStep` (#2028; `with_clamping_focuser` in the sweep) and sends a relative
  step as given. It is refused while the link is faulted. Its body forwards `Move` and writes `IsMoving` true through:
  always for an absolute Move (the frame's `Position` can be a poll period old, so it is no evidence the target is
  reached), and for a relative step that is not 0. A step of 0 leaves the polled value, since an earlier move may still
  run. `Position` stays the measured one until the next poll.
- **Halt** is forwarded even while the link is faulted: the safety stop is refused only for a lost link
  (`NotConnected`). Its body writes `IsMoving` false through. A Halt sent while a Move's body runs waits for it (the
  caller order of `run_command()`), so it reaches the upstream after the Move.
- **TempComp write** is forwarded (the upstream refuses it with its own error when it has no compensation), refused
  while faulted, and written through on success.
- A refused connection on any of the three is a lost link at once; another call with no reply is `DriverException`.
  Actions and `Command*` are not forwarded.

#### The Camera proxied device (issue #1753 slice 5, #2703)

The same config, self-loop check, Connect (Platform 7 is `ICameraV4`; a version 3 or older upstream gets
`Connected=true`), branded Name, snapshot, poll and link rules as the ObservingConditions device, with the same loss
bound. The upstream runs the exposure: no exposure body runs here, so there is no capture session (decision 0020).

- **Connect** reads the sensor (sizes, bins, pixel sizes, the `Can*` flags, the exposure, gain and offset limits and
  lists, `ReadoutModes`, `SensorName`, `SensorType`, `MaxADU`, `ElectronsPerADU`, `FullWellCapacity`, `HasShutter`,
  the Bayer offsets) and the client's settings (`BinX`/`BinY`, `NumX`/`NumY`, `StartX`/`StartY`, `Gain`, `Offset`,
  `ReadoutMode`, `FastReadout`, `CoolerOn`, `SetCCDTemperature`, `SubExposureDuration`) once, each a value or the
  upstream's error. Both keep answering while the link is faulted. The settings are written through by this server's
  writes and are not polled: a change another client of the upstream makes shows at the next Connect. The `Can*`
  flags also answer while disconnected: the last Connect's value, false before any. Connect also asks
  `CCDTemperature`, `CoolerPower`, `HeatSinkTemperature` and `IsPulseGuiding` once: an upstream `NotImplemented`
  leaves that member out of the poll, its getter answering that error unchanged.
- **Members by version.** ICameraV2 added the Bayer offsets, `CanFastReadout`, `ExposureMax`/`Min`/`Resolution`,
  `FastReadout`, `Gain*`, `Gains`, `PercentCompleted`, `ReadoutMode(s)`, `SensorName` and `SensorType`; ICameraV3
  `Offset*`, `Offsets` and `SubExposureDuration`. An older upstream is never asked them: each answers `NotImplemented`
  naming the upstream's version (`CanFastReadout` reads false).
- **Snapshot.** The frame is `CameraState`, `ImageReady`, `PercentCompleted` (asked only while the sampled
  `CameraState` is Exposing), `CCDTemperature`, `CoolerPower`, `HeatSinkTemperature`, `IsPulseGuiding` and
  `LastExposureDuration`/`LastExposureStartTime` (asked when `ImageReady` turns true, and in the Connect's first frame;
  otherwise carried over). Two to seven calls a poll; five on a cooled camera with no heat-sink sensor, idle. A stale
  frame is served; a latched fault refuses it with `DriverException`. `DeviceState`'s `TimeStamp` is the frame's age
  (the camera base's `device_state_age()`). The upstream's `InvalidOperation` on `ImageArray` and `LastExposure*` is
  flagged asked too early (#1167, #2182).
- **StartExposure** follows the camera argument order: the duration (`util::check_exposure_duration`), the connection,
  `ExposureMin`/`ExposureMax` as the Connect read them (`util::exposure_microseconds`: below the minimum runs at it).
  The frame against the sensor and an exposure already running are the upstream's to judge (its `InvalidValue` or
  `InvalidOperation` passes through): the settings held here may be stale. Refused while faulted. The body writes
  `CameraState` Exposing, `ImageReady` false and `PercentCompleted` 0 through; only the poll writes `ImageReady` true.
- **Setters**: each static check (`util::check_bin`, `check_frame_size`, `check_frame_start`, `check_ccd_setpoint`, a
  negative `ReadoutMode` or `SubExposureDuration`) before the connection check, then the bounds the Connect read
  (`MaxBinX`/`MaxBinY`, the `ReadoutModes` count), refused while faulted, forwarded and written through. A bin write
  reads the other bin and the frame back (an upstream without asymmetric binning sets both); a `Gain` or
  `ReadoutMode` write reads `ElectronsPerADU` and `FullWellCapacity` back. A read-back with no reply keeps the value
  held and does not fail the write. **PulseGuide** (direction 0 to 3, a duration of 0 to
  `kDefaultMaxPulseGuideMs`, #2292) writes `IsPulseGuiding` true through, as the Telescope's does; the next poll
  serves the upstream's answer.
- **AbortExposure and StopExposure** are forwarded even while the link is faulted (refused only for a lost link) and
  read `CameraState` back. They reach the upstream after an earlier command (the caller order).
- **ImageArray** is fetched in the command path, never polled: refused while faulted, then the client's
  `get_image_array()` with the bounds the Connect computed (`image_fetch_bounds()`): a body cap of the full sensor at
  four bytes an element plus 64 KiB (three planes for a Color sensor or one that did not say), at most 512 MiB and at
  least 1 MiB, and a timeout of 5 s plus the cap at 1 MiB/s, at most 300 s. The image is held until the next
  StartExposure, a Disconnect or a frame that reads `ImageReady` false; until then a read answers it again without a
  call. `ImageArrayVariant` is `NotImplemented` (COM only). The download is a command, so a command sent during it
  waits for it, and a Disconnect joins it (its timeout bounds both).
- A refused connection on any command or on the image download is a lost link at once; another call with no reply is
  `DriverException`. Actions and `Command*` are not forwarded.

Tests use the loopback `FakeAlpacaServer` (`AlpacaCore/tests/fake_alpaca_server.h`, 127.0.0.1 only, with stall,
drop, HTTP 500, bad JSON and no-ErrorNumber modes) over the real libcurl client, and `FakeHttpClient` where a case
counts calls. The device tests (`test_remotealpaca_observingconditions.cpp`, `test_remotealpaca_safetymonitor.cpp`,
`test_remotealpaca_rotator.cpp`, `test_remotealpaca_filterwheel.cpp`, `test_remotealpaca_switch.cpp`,
`test_remotealpaca_telescope.cpp` (its `[goto-safety]` cases cover the cap and the relink stop),
`test_remotealpaca_focuser.cpp`, `test_remotealpaca_camera.cpp`, the contract sweep and the `[stress]` cases in `test_remotealpaca_concurrency_stress.cpp`) answer from
`AlpacaUpstreamScript` (`AlpacaCore/tests/alpaca_upstream_script.h`, a responder for `FakeHttpClient`;
`Kind::SafetyMonitor` scripts a SafetyMonitor upstream, `Kind::Rotator` a Rotator whose moves run until
`finish_move()` or a Halt, `Kind::FilterWheel` a FilterWheel whose Position write reads -1 until
`finish_wheel_move()`, `Kind::Switch` a Switch keyed by `Id` whose asynchronous writes run until `finish_async()`,
`Kind::Telescope` a German mount whose slews land at once or, with `hold_motion(true)`, run until `finish_motion()`
or AbortSlew, `Kind::Focuser` a Focuser whose Move runs until `finish_move()` or a Halt, `Kind::Camera` an 8 x 6
camera whose exposures run until `finish_exposure()` or StopExposure and whose image it serves as ImageBytes) with the
poll on `FakeTaskClock`. The contract sweep's snapshot
rig for the SafetyMonitor states `IsSafe`'s lost answer in `SnapshotRig::lost_answer`.

ConformU against the ASCOM Alpaca Simulators (OmniSim) runs on the rig host, never in CI. The fake ConformU row
(`scripts/conformu_fake_runs.tsv`, #2696) runs the driver over `AlpacaUpstreamScript` through `FakeHttpClient`
(`conformu_observingconditions_factories.h`), with the poll on the real task clock; it is fake evidence only.
