# AlpacaBridge Integration and Instruction Precedence

## Existing instruction system

This skill is vendored in the repository at `.claude/skills/ascom-alpaca-protocol/`. Claude is also instructed through:

1. `CLAUDE.md`, which imports `AGENTS.md` and `docs/agent-instructions.md`.
2. `AGENTS.md`, the repository-wide architecture, safety, testing, and historical decision record.
3. `.claude/rules/*.md`, which route to scoped `.github/instructions/*.instructions.md` files.
4. `.claude/commands/*.md`, including `/driver-build` and `/conformu` workflows.
5. Vendor-specific rules and retained failure/decision records.

When this skill is used in AlpacaBridge:

- The official ASCOM contract governs public behavior and wire compatibility.
- `AGENTS.md` and scoped rules govern repository architecture, supported platforms, build/test commands, shared infrastructure, and hardware-proven quirks.
- A command file is a workflow aid, not authority to override either the official contract or higher-level repository instructions.
- If instructions disagree, stop and identify the exact conflict before coding.

## Conflicts

If a repository instruction contradicts the official ASCOM contract, treat it as a documentation defect: stop, name the exact file and statement, and correct the repository document rather than averaging the two rules together.

## Compatible uses of existing drivers

It is appropriate to reuse:

- base classes and `AsyncConnectable`;
- router and response helpers;
- fake transport seams and test harnesses;
- project naming, file placement, CMake patterns, and web UI components;
- device-type shared implementations such as base `DeviceState` behavior; and
- proven vendor-protocol adaptations when relevant to the same hardware.

It is not appropriate to copy a driver's capability choices, public state behavior, error codes, units, or endpoint semantics without checking the official contract.

## AlpacaBridge-specific semantic notes

- `DeviceState` intentionally calls individual getters and is not an atomic snapshot. Do not add a cross-device snapshot lock merely to satisfy a generic desire for atomicity.
- `AsyncConnectable` owns connection-thread lifecycle. Do not create a per-vendor replacement.
- Per-client connection tracking exists so one client's disconnect does not tear down another client's session.
- The project advertises Platform 7 interface versions (Camera 4, Telescope 4, Focuser 4, Rotator 4, FilterWheel 3, Switch 3, ObservingConditions 2, per `AGENTS.md`); changing one requires fresh real-hardware ConformU validation.
- ConformU telescope testing requires a bare mount with no OTA. This safety rule overrides convenience.
- Current repository policy targets Linux arm64 and rejects x86/amd64 builds.

## Maintenance

The skill supplies general ASCOM reference material; the repository supplies implementation-specific rules. Do not copy the generic references into `AGENTS.md`; that would duplicate a versioned standard and create another drift point.

`scripts/check_docs_drift.py` checks that every backticked repository path in this skill exists, and `scripts/check_instruction_structure.py` checks its relative Markdown links. `scripts/check_docs_drift.py` also fails when `docs/AlpacaDeviceAPI_v1.yaml` no longer matches the SHA-256 pinned in `references/version-and-sources.md`; after `/driver-build` Step 0 refreshes the schema, regenerate `references/device-api-catalog.md` and update the pin. When the official ASCOM baseline changes, update `references/version-and-sources.md` first, then the references that depend on it.
