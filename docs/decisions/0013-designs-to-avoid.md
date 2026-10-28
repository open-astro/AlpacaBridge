# Designs to avoid

Status: accepted

## Context

The deep-module migration (catalog, task clock, operation slot, state snapshot, SDK seams) replaces repeated hand-written code with a few small modules that each hide one concern. Seven shapes keep coming back in design talk because they look like the next step. Each would give back the locality those modules bought. This record writes down why each is rejected and what to do instead, so a review can point here and a new proposal has to answer the reasons.

## Decision

Do not build any of the seven designs below. Each section says what it is, why it is rejected and what to do instead.

### 1. A universal `VendorAdapter`

What it is: one interface that every vendor implements (connect, enumerate, read, write) so that the HTTP layer and the drivers talk to vendors in one shape.

Why rejected: vendors differ in what they are: an SDK handle (ZWO, QHY), a serial protocol, a TCP or UDP link, a scripting server over TCP (Bisque, TheSkyX). An interface wide enough to cover them is either lowest-common-denominator or a bag of optional calls, and each ASCOM device type already has its own interface in `AlpacaCore/include/alpacacore/*_driver.h`. The vendor wrapper layer in `AGENTS.md` ("3-layer driver pattern") is already the adapter, written per vendor where the differences are.

Instead: keep the Alpaca interface, vendor wrapper, vendor implementation layers. Put what is truly shared in small modules with a narrow job: `AsyncConnectable` and `util::AsyncOperation` ([decision 0006](0006-async-operation-ownership.md)), `TaskClock` ([decision 0005](0005-task-clock.md)), `StateSnapshot` ([decision 0010](0010-snapshot-served-state.md)). Describe configuration per (vendor, device) with a catalog descriptor ([decision 0004](0004-device-catalog.md)), not with a shared class.

### 2. A universal telescope driver

What it is: one telescope driver parameterised by a protocol, so SynScan, Sky-Watcher, Celestron, OnStep, iOptron and Bisque share one set of slew, sync, park and tracking code.

Why rejected: the mounts disagree on what the controller does and what the driver must do. Sky-Watcher dead-reckons position from axis angles ([decision 0010](0010-snapshot-served-state.md), Sky-Watcher adoption, point 2); Celestron and Bisque serve a polled frame with different slow fields; link-loss behaviour is per transport ([decision 0009](0009-link-loss-and-relink-policy.md)). A common driver would carry each mount's special cases behind flags, and a change for one mount could change another's ConformU result. Telescope ConformU runs need a bare mount, so a regression is costly to find.

Instead: one `TelescopeDriver` per vendor over the shared Alpaca interface (`AlpacaCore/include/alpacacore/telescope_driver.h`). Share through helpers a driver composes: `util/park_slew.h`, `util/motion_policy.h`, `util/link_health.h`, `util/state_snapshot.h`. Add a helper when a second driver needs the same behaviour, not before.

### 3. Protocol inheritance

What it is: one vendor's protocol class derived from another's (for example a Sky-Watcher protocol class deriving from the SynScan one) because the command sets look alike.

Why rejected: similar wire formats are not the same contract. Sky-Watcher's motor controller shares its command set document with SynScan (`AlpacaCore/external/SynScan/SkyWatcher_Motor_Controller_Command_Set.md`) but has its own wrapper (`vendor/skywatcher/skywatcher_protocol_wrapper.h`) and timing rules. A derived class inherits every virtual and every fix of its base, so a change to one protocol silently changes the other, and a test for one needs the other's fakes.

Instead: one protocol wrapper per vendor with its own tests and fake. When two wrappers need the same parsing or I/O code, move that code into a free function or small class under `util/` (for example `util/serial_io.h`) and have both call it.

### 4. JSON inside AlpacaCore

What it is: using `nlohmann::json` (or any JSON type) in AlpacaCore, for config, the catalog or device state.

Why rejected: AlpacaCore is vendor-neutral device logic with no transport. `AGENTS.md` says "No HTTP/REST/sockets/JSON transport code" in AlpacaCore. JSON in the core would tie device logic to one wire format and make the layering gate (AlpacaHTTP to AlpacaCore to vendor) meaningless.

Instead: describe configs as typed structs with field roles in the catalog descriptors and let AlpacaHTTP bridge JSON to them in one place ([decision 0004](0004-device-catalog.md), which rejected the catalog-with-JSON alternative for this reason).

### 5. A base class per helper

What it is: a new base class for each piece of shared behaviour (a polling base, a cache base, a link-health base, a park base) that drivers inherit.

Why rejected: a driver then inherits several bases, each with its own virtual hooks, lifetime and locking, and the lock order set by [decision 0016](0016-getters-never-wait-on-device-io.md) is hard to keep. `AsyncConnectable` is the one inheritance exception, kept because the connect thread lifecycle must live in one place and needs the driver's own destructor ordering ([driver-lifecycle](../../.github/instructions/driver-lifecycle.instructions.md)). Everything else the migration produced is a value a driver owns: `util::AsyncOperation` slot, `StateSnapshot<T>`, `TaskClock`.

Instead: compose. A driver holds the helper as a member, constructs it with the injected clock, and joins it before its other members die. A helper that needs a callback takes a function, not a virtual override.

### 6. Pre-built Dome and SafetyMonitor drivers or abstraction

What it is: writing a shared Dome or SafetyMonitor implementation (or an abstraction layer over shutters, roof sensors and safety inputs) before a real device needs it.

Why rejected: the base interfaces exist (`dome_driver.h`, `safetymonitor_driver.h`, under the ten Alpaca device types) and no vendor implements them as of this record (the first Dome or SafetyMonitor driver PR must update this section). An abstraction written without a device guesses at what dome and roof hardware share and is wrong the first time a real one arrives. It also adds code that no test against hardware can check.

Instead: write the first Dome or SafetyMonitor driver for a named device, through the usual three layers, with a catalog descriptor and a fake for tests ([testing](../../.github/instructions/testing.instructions.md)). Extract a shared helper only when a second device shows what is common. Shutter control belongs to the Dome interface; do not add a separate device type (`AGENTS.md`, Out of Scope Guardrails).

### 7. A production `ByteChannel`

What it is: an abstract byte-stream interface between every serial or socket protocol wrapper and its file descriptor, so that tests replace the transport in process instead of through a pseudo-terminal or a fake server.

Why rejected: the serial and socket wrappers are already tested end to end through pty fakes (`PtyPair` and `pty_write_bounded()` in `AlpacaCore/tests/fake_pty_write.h`) and fake servers, which exercise the real `termios`, `poll()` and timeout code a `ByteChannel` would bypass. The pty fakes' known failure, a worker that hangs in a blocking `write()` (#424), was fixed in the fakes with a bounded, non-blocking write (PR #443, which fixed #424), and their pty setup has one owner, `PtyPair` (#387); no production layer was added. A `ByteChannel` would put a second I/O path into every wrapper for a test-only need.

Instead: keep pty fakes and fake servers for serial and socket drivers, and the per-library seams of [decision 0015](0015-sdk-seams.md) for vendor SDKs. Fix a flaky pty fake in the fake.

Reopen when: pty timing is still the source of test flakes after the fakes are fixed. Then write a record that supersedes this section, with the flake evidence.

## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. No universal `VendorAdapter` interface across vendors (section 1). Scope: `AlpacaCore/include/alpacacore/**`, `AlpacaCore/src/**`. Checked by: review.
2. No universal telescope driver parameterised by protocol (section 2). Scope: `AlpacaCore/src/vendors/**`. Checked by: review.
3. No vendor protocol class derived from another vendor's (section 3). Scope: `AlpacaCore/include/alpacacore/vendor/**`, `AlpacaCore/src/vendors/**`. Checked by: review.
4. No JSON type in AlpacaCore (section 4). Scope: `AlpacaCore/src/**`, `AlpacaCore/include/**`. Checked by: review.
5. No base class per shared helper; `AsyncConnectable` is the only one (section 5). Scope: `AlpacaCore/include/alpacacore/**`. Checked by: review.
6. No shared Dome or SafetyMonitor implementation before a named device needs it (section 6). Scope: `AlpacaCore/src/vendors/**`. Checked by: review.
7. No production `ByteChannel` between the serial and socket wrappers and their file descriptors (section 7). Scope: `AlpacaCore/src/vendors/**`, `AlpacaCore/include/alpacacore/util/*.h`. Checked by: review.

## Alternatives rejected

- **Allow each design behind a feature flag or for one pilot vendor.** The cost is paid in review and in the lock order, not in the flag; a pilot becomes the precedent.
- **Leave the list in the migration plan only.** The reasons would not sit beside the code a reviewer reads, and the plan is not a repository file.

## Consequences

- A review that meets one of these designs cites this record; the author either uses the "instead" or writes a new record that supersedes the relevant section first.
- New shared behaviour arrives as a small composed module with its own record when the choice is non-obvious.
- Some code stays repeated across vendors until a second consumer shows what is common. That is accepted.

## Links

- [Device catalog](0004-device-catalog.md), [Task clock](0005-task-clock.md), [Async operation ownership](0006-async-operation-ownership.md), [Snapshot-served state](0010-snapshot-served-state.md), [SDK seams](0015-sdk-seams.md)
- Upstream issue [#1381](https://github.com/open-astro/AlpacaBridge/issues/1381)
