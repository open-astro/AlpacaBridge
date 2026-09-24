# Validating instruction discovery

Run `python3 scripts/check_docs_drift.py` and
`python3 scripts/check_instruction_structure.py --self-test` from the repository root.
The drift gate checks the Claude entry point, agreement between GitHub and Claude
path metadata, vendor source/header/test coverage, the instruction index, and
relative links in scoped guidance, memory, `CONTEXT.md`, `docs/architecture.md`, and the
Claude skills under `.claude/skills/`. Its existing prose checks also scan
the relocated instruction files. Negative fixtures check that removing the Claude
entry point, changing an adapter's vendor, removing metadata, or breaking the index
is detected.

Claude Code imports the core and index through root `CLAUDE.md`. Its native
`.claude/rules/` path rules tell it which canonical file to read in full. The index
also covers requests that mention a vendor before any source file is opened and
changes to shared configuration that affect multiple vendors. The review workflow
restores these instruction dependencies from the trusted base, just as it restores
`AGENTS.md` and `.claude/`.

These are structural tests, not a guarantee of identical model behavior. No live
Claude session is part of this gate. To smoke-test a client release, start a fresh
session at the root and again inside each component, and ask it to identify the
instruction files it loaded before answering:

- For a Gemini focuser task, load the Gemini guidance; the 9600-baud telescope
  communication statement belongs to Celestron, not Gemini.
- For a SkyWatcher UTCDate change, load SkyWatcher, the shared concurrency rules,
  and the pointing-clock decision.
- For a test-only QHY change, load QHY and the shared test requirements.
- For HTTP startup or WiFi management, load both HTTP conformance and WiFi rules.
- For an iCAM configuration change, load iOptron and Player One.

Inspect the client's loaded rules and file-read trace rather than accepting its
claim that it read a file. All substantive rules have one canonical owner; adapters
and indexes contain routing instructions only. Cross-vendor rules stay in `AGENTS.md`.
