# Finding the instructions for a task

Read `AGENTS.md` in full for every task. It owns architecture, concurrency, tests,
CI and cross-vendor rules. Then read the applicable files below in full, following
relevant links. A shared-file change can affect a vendor even when its filename
does not match a vendor glob: select by task and affected behavior as well as path.
Domain terms used across these files are defined in [CONTEXT.md](../CONTEXT.md).
For a cross-vendor audit, read all vendor files. Revisit this index as scope grows.

The `applyTo` fields support GitHub clients. Claude Code uses the matching `paths`
fields in `.claude/rules/`: those small adapters require reading the same canonical
files below, and `CLAUDE.md` imports the core and this index. Other agents follow
this index from `AGENTS.md`; the component Cursor rules also point here. Automatic
attachment is a convenience, not a substitute for completing the required reads.
If a client truncates instructions or tool output, continue reading in chunks.
When starting from a component directory, all paths below still name files in the
repository root.

## Scoped guidance

| Task or affected hardware | Read |
| --- | --- |
| ZWO, ASI cameras, EAF, EFW, CAA, AM mounts, ASIAIR power ports | [ZWO](../.github/instructions/zwo.instructions.md) |
| QHY cameras, integrated CFW, or the Q-Focuser | [QHY](../.github/instructions/qhy.instructions.md) |
| SVBONY cameras | [SVBONY](../.github/instructions/svbony.instructions.md) |
| ToupTek cameras, AFW, AAF, StellaVita, or SC715C rebadge | [ToupTek](../.github/instructions/touptek.instructions.md) |
| Player One cameras, Phoenix wheels, thermal switch, iCAM camera backend | [Player One](../.github/instructions/playerone.instructions.md) |
| SynScan handset path | [SynScan](../.github/instructions/synscan.instructions.md) |
| SkyWatcher direct motor controller, Wave, EQ-class Synta | [SkyWatcher](../.github/instructions/skywatcher.instructions.md) |
| iOptron mounts, iEAF/iAFS, iEFW, iMate, or iCAM configuration | [iOptron](../.github/instructions/ioptron.instructions.md) |
| Celestron NexStar | [Celestron](../.github/instructions/celestron.instructions.md) |
| Bisque, Paramount, TheSkyX | [Bisque](../.github/instructions/bisque.instructions.md) |
| Astroasis Oasis | [Astroasis](../.github/instructions/astroasis.instructions.md) |
| Gemini focuser, covers, flat panels, PDH | [Gemini](../.github/instructions/gemini.instructions.md) |
| WandererAstro covers, rotators, SFW wheels, boxes | [WandererAstro](../.github/instructions/wandererastro.instructions.md) |
| WeeWX weather integration | [WeeWX](../.github/instructions/weewx.instructions.md) |
| OnStep mounts | [OnStep](../.github/instructions/onstep.instructions.md) |
| GPhoto DSLR and mirrorless cameras (Canon, Nikon, Sony via libgphoto2) | [GPhoto](../.github/instructions/gphoto.instructions.md) |
| Any AlpacaHTTP change, routing, serialization, API or HTTP tests | [AlpacaHTTP conformance](../.github/instructions/alpaca-http-conformance.instructions.md) |
| WiFi management, startup, routing, web UI, polkit or network setup | [WiFi manager](../.github/instructions/wifi-manager.instructions.md) |

Vendor instructions apply to implementation, wrapper headers, SDK integration,
corresponding tests and reports. Shared HTTP routing/configuration changes also
require the affected vendor instructions. For an SC715C task, read SVBONY and
ToupTek; for an iCAM task, read iOptron and Player One. A vendor comparison requires
the files for all vendors being compared. The scoped files contain vendor-specific
deltas only; a rule affecting a second vendor belongs in `AGENTS.md`.

## Issue tracker

AGENTS.md points at `docs/agents/issue-tracker.md` for `gh` CLI conventions
when working with GitHub Issues. Read it before creating, listing, or
commenting on an issue.

## Failure and decision memory

- [Failure index](failures/README.md): incidents, root causes, prevention and concrete evidence.
- [Decision index](decisions/README.md): current choices, rejected alternatives and consequences.

Read the relevant records before changing the behavior they explain. Historical
snapshots are evidence, not overrides of current rules or current implementation.
When documenting new lessons, link each record directly from its owning vendor
instruction file or shared-rule section in `AGENTS.md`, as well as the memory index.
Update the applicable owner and index rather than
copying rules into multiple places. See [instruction validation](instruction-validation.md)
for the automated checks and their limits.
