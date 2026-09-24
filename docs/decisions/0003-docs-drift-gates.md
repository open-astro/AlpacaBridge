# Keep documentation drift checks beside the facts they enforce

Status: accepted

## Context

Several repo facts were written twice and silently diverged: build options and their table, tool pins and suppressions, blocking `get_connected()` rosters, SDK seam method lists, TSan run/guard pairs, and path references. The checker records the issue history for each rule in its docstring and beside the implementation.

## Decision

`scripts/check_docs_drift.py` derives or compares each fact from its owning files and runs in CI and pre-flight. Its path-reference check covers `AGENTS.md`, `CONTEXT.md`, scoped instruction files, agent-skills docs in `docs/agents/`, Claude skill docs in `.claude/skills/`, and the records in `docs/failures/` and `docs/decisions/`. References to decision records in first-party code comments must resolve. A vendored reference doc that is generated from a repo file pins that file's hash so the two cannot diverge: the `ascom-alpaca-protocol` skill pins the LF-normalized SHA-256 of `docs/AlpacaDeviceAPI_v1.yaml`, which `/driver-build` Step 0 refreshes from upstream. Extractor logic has literal self-test fixtures where a regex or pairing error could otherwise make a check vacuously green.

## Alternatives rejected

Maintain prose-only reminders: the blocking-getter roster and TSan commands already drifted that way. Match counts or substrings alone: they can pass after the wrong item changes or a run and its guard become unpaired. Rely on a developer's local generated files: an ignored path can exist locally and be missing from a clean checkout.

## Consequences

New instruction or memory files must be included in path validation; new duplicated facts need an explicit comparison or one authoritative owner. Checker changes should include a fixture that fails for the drift they claim to catch. Path-reference check 7 and the instruction-structure link check take a `root` and are fixture-driven (issue #452). Floors are still partial and the residue is deferred, not decided against: inside check 7 the `.github/instructions/` loop, the `docs/failures`/`docs/decisions` loop and the `CONTEXT.md` scan (which passes floor 0) have no `MIN_*` of their own (only `docs/agents/` and `.claude/skills/` do), and the other docs-drift checks have neither a root seam nor a floor.

## Links

- Tracker [#268](https://github.com/open-astro/AlpacaBridge/issues/268); blocking-getter history [#315](https://github.com/open-astro/AlpacaBridge/issues/315), [#381](https://github.com/open-astro/AlpacaBridge/issues/381), [#407](https://github.com/open-astro/AlpacaBridge/issues/407); TSan pairing [#341](https://github.com/open-astro/AlpacaBridge/issues/341), [#455](https://github.com/open-astro/AlpacaBridge/issues/455); path checks [#457](https://github.com/open-astro/AlpacaBridge/issues/457).
- Gate and regression fixtures: `scripts/check_docs_drift.py` (`--self-test`), invoked by `scripts/ci_preflight.sh` and `.github/workflows/ci.yml`.
