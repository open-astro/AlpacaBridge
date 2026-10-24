# Domain glossary

Terms a contributor must not confuse. Each entry says what the term is and what
not to call it. Module names (Device Catalog, task clock, and so on) live in
[docs/architecture.md](docs/architecture.md#modules); design rationale lives in
[docs/decisions/](docs/decisions/README.md).

## Device number vs enumeration index

- **Device number**: the Alpaca `{device_number}` in `/api/v1/{device_type}/{device_number}/...`.
  AlpacaBridge assigns it per device type, independent of vendor, and it is what
  clients address. Persisted as `deviceNumber`.
- **Enumeration index**: which unit on a vendor SDK's bus the driver opens, numbered
  from 0 per (vendor, device type) in the order the SDK lists them (`cameraIndex`,
  `focuserIndex`, `filterwheelIndex`, `rotatorIndex`). It can change when hardware is
  re-plugged; some vendors also take a stable id field (`cameraId`). Serial and network
  devices have none.
- Avoid: "device index", "camera number", or using either word for the other. A ZWO
  camera and a Player One camera can both be enumeration index 0 while having
  different device numbers.

## API config vs persisted config

- **API config**: a config arriving through `/management/v1/configuredevice`
  (`ConfigSource::Api`). A validation failure rejects it with a message.
- **Persisted config**: an entry loaded from `registered_devices.json` at startup
  (`ConfigSource::Persisted`). A validation failure is normalized with a warning and
  the device is still registered, so it stays listed and editable in the web UI and
  refuses to connect until fixed (each descriptor's `Schema::normalize`, applied by `DeviceCatalog::normalize`, #380).
- Avoid: "saved config" for both, or "invalid config" without saying which source;
  the same fault has two different outcomes.

## Cancelled vs superseded

- **Cancelled**: a client asked a running operation to stop (AbortSlew, Halt,
  `MoveAxis(axis, 0)`, disconnect). The operation brings the device to a safe stop and
  publishes that state.
- **Superseded**: a newer operation now owns the same axes or device. The old body
  touches neither hardware nor driver state, except to stop a move it started in a
  wire turn it still holds; the newer one is responsible for both.
  SkyWatcher tells the two apart by hand today through its driver-wide motion
  generation counter.
- Avoid: "aborted" or "stopped" for both. Treating a superseded body as cancelled
  would send a stop to axes the newer operation owns.

## Connected vs link health

- **Connected**: the ASCOM `Connected` state, set by the client. It stays true while a
  link is faulted; the client, not the driver, decides whether to reconnect.
- **Link health**: whether the transport is delivering (`util::StreamLinkHealth`,
  `util::PolledLinkHealth`). A faulted link makes reads and writes throw
  `DriverException` ("communications compromised"), not `NotConnected`.
- **Lost link**: in a driver that has adopted
  [decision 0009](docs/decisions/0009-link-loss-and-relink-policy.md), a link is lost
  when the transport is gone or when a fault has stood past `util::kLinkStalenessBound`
  (30 s). `Connected` then reads false and operations throw `NotConnected` until the
  client connects again. Each driver's row in that record's Adoption table says which
  evidence it acts on at once (for example a removed device node, #445) and which it
  leaves to the bound.
- Avoid: "disconnected" for a faulted link, or `NotConnected` for a link that has been
  silent for less than the staleness bound.

## Fake evidence vs rig evidence

- **Fake evidence**: a hardware-free test against a fake under `AlpacaCore/tests/`
  (fake SDK seams, `AlpacaCore/tests/fake_mount_server.h`,
  `AlpacaCore/tests/fake_skywatcher_mount.h`, the pty fakes). It proves how our code
  behaves against a modelled device, and only as far as the fake is faithful; a fake
  should be the harsher of the two where it and the hardware disagree.
- **Rig evidence**: a run on real hardware, such as a ConformU report on arm64 under
  `AlpacaCore/conformu/`, for the exact commit being claimed.
- Avoid: "tested", "verified" or "validated" without saying which. A fake-only change
  says "ConformU not re-run" in its PR body.

## Retired vs superseded record

- **Superseded record**: a decision record in `docs/decisions/` replaced by a
  newer one; the newer record is the rule and the old one names it.
- **Retired record**: a decision record whose choice stopped being true with no
  successor, because the module or hardware it governed is gone. It keeps its
  history and has no rules.
- Avoid: "deprecated" or "obsolete" for either; a superseded record points
  somewhere, a retired one does not.

## Review-checked vs script-checked rule

- **Script-checked rule**: a decision-record rule whose `Checked by` names a test
  or a drift check that fails when the rule is broken.
- **Review-checked rule**: a rule only a reader can apply; its `Checked by` says
  what evidence the reader looks for, and the review bot applies it.
- Avoid: "unenforced" for a review-checked rule; it is enforced by the review
  bot, with the evidence the rule names.

## Record rule vs topic-file rule

- **Record rule**: a numbered line in a decision record's Rules section, with a
  scope glob and a check. It exists because the decision behind it was hard to
  reverse, surprising, and a trade-off.
- **Topic-file rule**: a rule in a shared topic file or vendor file under
  `.github/instructions/`, with no record behind it, because the choice fails at
  least one of those three tests.
- Avoid: calling a topic-file rule an "ADR" or writing a record for a plain
  convention.
