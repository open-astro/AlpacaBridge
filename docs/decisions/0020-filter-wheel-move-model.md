# Filter wheel move model

Status: proposed

## Context

Issue #1629 asks whether the filter wheel move model (what `Position` reads while the wheel turns, what a `Position` write does, and when the settled slot is cached) can become one shared module. The ZWO EFW driver and the QHY CFW3 driver are the two candidates: the ZWO driver keeps a 100 ms `util::TtlStatusCache` of the SDK's `EFWGetPosition` (#1586), and the QHY CFW3 driver hand-rolls a `pending_target_`, a `position_cache_` and a `move_thread_`-style slot (`util::AsyncOperation`, decision 0006).

The ASCOM FilterWheel interface fixes the behaviour both must show: `Position` reads -1 while the wheel moves, a `Position` write is asynchronous, and an out-of-range slot is `InvalidValue`. Range validation comes before the connection check (`.claude/skills/ascom-alpaca-protocol/references/device-api-catalog.md`, FilterWheel critical checks). The failure record [0001](../failures/0001-qhy-filterwheel-position-cache.md) adds one more: a device that reports passing slots during a move must not have one of them cached as the settled slot.

The slice's stop rule is "extract only if both pass unchanged". `AlpacaCore/tests/test_filterwheel_move_model.cpp` runs six case bodies, unchanged, over both drivers (each over its existing fake: `FakeZWOEFWSDK` behind a transit-scripting decorator, and `FakeQhyCfw3` on a pty).

Result on x86_64 over the two drivers as merged at `2e5f28ce`:

| Case | ZWO EFW | QHY CFW3 |
| --- | --- | --- |
| `Position` is -1 while moving, then the target | pass | pass |
| a valid write returns without waiting for the move | pass | pass |
| an out-of-range write is `InvalidValue` and starts no move | pass | pass |
| `InvalidValue` precedes `NotConnected` | pass | pass |
| a cache primed at connect answers without a device read | **diverges** | pass |
| transit slots (including the pre-move slot) never reach `Position` | **diverges** | pass |

The two ZWO divergences:

- **No primed cache.** `set_connected(true)` only resets the TTL frame (`zwo_filterwheel_driver.cpp:182`), so the first `Position` read after connect is an `EFWGetPosition` call (`:250`). The QHY driver seeds `position_cache_` from the connect handshake (`qhy_cfw3_filterwheel_driver.cpp:176`).
- **No pending target.** The ZWO driver has no commanded target: it returns whatever the SDK reports and passes only -1 through specially (`zwo_filterwheel_driver.cpp:262`). A device that reported the pre-move slot or a transit slot after the write would be served as the position for up to the TTL. The QHY driver answers -1 for as long as `pending_target_` is set (`qhy_cfw3_filterwheel_driver.cpp:214`) and caches the slot only when the move body reports arrival. The real EFW SDK reports -1 while the wheel turns, so the ZWO result is a gap in the driver's own guard, not a hardware fault seen on a rig.

One more difference does not show in the table: the ZWO driver does not check the upper slot bound itself in the tested path. The SDK fake refuses it with `InvalidValue`, as the SDK does, so the case passes without proving the driver's check. The QHY driver's check is pinned by the case.

Not tested here, by design: a write at or above the slot count while disconnected. The count is unknown before connect, so the ZWO driver answers `NotConnected`, and the QHY driver answers `InvalidValue` from its 16-slot protocol ceiling and `NotConnected` below it. Both answers are allowed by the interface.

## Decision

- **No shared filter wheel move-model module is extracted in this slice.** Two of six cases fail on the ZWO EFW driver, so the stop condition holds. Nothing under `AlpacaCore/src` changes.
- **The shared test stays.** The six case bodies run over both drivers. The two ZWO instantiations that diverge carry `[!shouldfail]`, so the divergence is named in the test, and CI fails when it goes away without this record being updated.
- **The next step is on the ZWO side, in its own slice.** The ZWO driver gets a pending target (-1 until the SDK reports the commanded slot) and a cache seeded at connect. Then the two `[!shouldfail]` tags come off, this record is revisited, and extraction is proposed over the two drivers with the interface sketched in this record's How to reverse section.

## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. A shared filter wheel move-model module is not extracted while a case in `test_filterwheel_move_model.cpp` diverges on one of the drivers it would serve. Scope: `AlpacaCore/tests/test_filterwheel_move_model.cpp`, `AlpacaCore/src/vendors/zwo/zwo_filterwheel_driver.cpp`, `AlpacaCore/src/vendors/qhy/qhy_cfw3_filterwheel_driver.cpp`. Checked by: review (a diff that adds a move-model header or class under `AlpacaCore/include/alpacacore/util/` while a `[!shouldfail]` tag remains in the test).
2. A `[!shouldfail]` tag on a move-model case is removed in the PR that makes the driver pass it, and that PR updates the table in this record. Scope: `AlpacaCore/tests/test_filterwheel_move_model.cpp`. Checked by: the test run (Catch2 reports a `[!shouldfail]` case that passes as a failure).
3. A filter wheel driver that joins the move-model test adds a rig to the test file and runs the existing case bodies; it does not edit or copy a body. Scope: `AlpacaCore/tests/test_filterwheel_move_model.cpp`. Checked by: review (the `TEST_CASE` pairs and the rig structs are the only parts of the file that grow).

## Alternatives rejected

- **Extract over the QHY driver and make ZWO adopt it later.** The module would be shaped by one driver's thread and serial wire, and the ZWO cache would then bend to fit it. The stop rule exists to prevent that.
- **Fix the ZWO driver in this PR.** The slice is test and record only, and a driver change needs its own red-first tests and ConformU evidence.
- **Drop the two diverging cases from the ZWO run.** The divergence would then be visible only in prose. A tagged case keeps it executable.

## Consequences

- The QHY CFW3 keeps its hand-rolled move state for now. The other wheel drivers (iEFW, Player One, ToupTek, WandererAstro) are not in this test, and nothing here says they pass it.
- A ZWO EFW on hardware can serve one `Position` frame from before the move if the SDK ever reports it. No rig has shown this, so it carries no severity claim.

## How to reverse

Delete `test_filterwheel_move_model.cpp`, its `CMakeLists.txt` entry and this record; nothing else depends on them. If extraction is later proposed, the module would own the pending target, the moving sentinel (-1 while the target is unconfirmed), the settled-slot cache and the order InvalidValue before NotConnected. The driver adapter would keep the wire: the SDK call or serial goto, the arrival read, the slot-count source and the vendor error mapping.

## Links

- Issue [#1629](https://github.com/open-astro/AlpacaBridge/issues/1629); position caches [#1586](https://github.com/open-astro/AlpacaBridge/issues/1586) (ZWO) and [#1591](https://github.com/open-astro/AlpacaBridge/issues/1591) (iEFW).
- [Failure 0001: QHY filter wheel cached a transit slot](../failures/0001-qhy-filterwheel-position-cache.md); decisions [0006](0006-async-operation-ownership.md), [0016](0016-getters-never-wait-on-device-io.md).
- [ZWO instructions](../../.github/instructions/zwo.instructions.md), [QHY instructions](../../.github/instructions/qhy.instructions.md).
