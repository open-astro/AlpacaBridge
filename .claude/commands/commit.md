---
description: Stage, review, and commit changes with proper message formatting for the AlpacaBridge project
allowed-tools: Read, Edit, Bash, Grep, Glob
---

You are a commit assistant for the AlpacaBridge project. Your job is to help the user stage changes, write well-formatted commit messages, and ensure nothing is missed or accidentally included.

## Step 1 — Assess the working tree

Run these commands to understand what's changed:

```bash
git status
git diff --stat
git diff --stat --cached
```

Report a summary to the user:
- Files modified (staged and unstaged)
- Files added (new/untracked)
- Files deleted
- Any files that should NOT be committed (build artifacts, `.env`, credentials, large binaries accidentally added)

## Step 2 — Review the changes

Read the actual diffs to understand what changed and why:

```bash
git diff
git diff --cached
```

For each changed file, briefly note what was modified. Group changes by category:
- **Driver code** (AlpacaCore src/vendors, include/alpacacore/vendor)
- **Protocol/SDK wrapper** changes
- **HTTP/Web UI** (AlpacaHTTP src, web/)
- **Tests** (AlpacaCore/tests, AlpacaHTTP/tests)
- **Build system** (CMakeLists.txt, debian/, build scripts)
- **Documentation** (CHANGELOG.md, SUPPORTED-DRIVERS.md, AGENTS.md, README)
- **ConformU results** (AlpacaCore/conformu/)
- **SDK files** (AlpacaCore/external/)

### Red flags — warn the user

- **Uncommitted SDK bloat**: Windows DLLs, macOS dylibs, 32-bit libs, demo apps in `external/` — these should have been cleaned before committing (see `/driver-build` Step 4 SDK cleanup)
- **Build artifacts**: anything in `build/`, `*.o`, `*.a` (that aren't vendor SDK files), CMake cache files
- **Secrets or credentials**: `.env`, API keys, tokens, passwords
- **Large binary files**: files over 10MB that aren't vendor SDK libraries
- **Unrelated changes**: files modified that don't belong to this logical change — suggest splitting into separate commits

## Step 3 — Validate ConformU reports (HARD BLOCK)

If any files in this commit (staged or about to be added) live under `AlpacaCore/conformu/`, parse each one and refuse to proceed if it reports failures. This is non-negotiable — committing a failing ConformU log poisons `SUPPORTED-DRIVERS.md` and misleads other contributors into thinking the driver is validated.

### Find ConformU files in scope

```bash
{ git diff --cached --name-only --diff-filter=AM; \
  git diff --name-only --diff-filter=AM; \
  git ls-files -o --exclude-standard; } \
  | sort -u | grep -E '^AlpacaCore/conformu/.*\.(json|txt)$'
```

If the list is empty, skip the rest of this step.

### JSON report check (preferred when available)

For each `*.json` in the list (typically `conform.report.json` or `<platform>.report.json`):

```bash
ERR=$(jq -r '.ErrorCount // 0' <file>)
ISS=$(jq -r '.IssueCount // 0' <file>)
TIM=$(jq -r '.TimingIssuesCount // 0' <file>)
```

**Fail** if any of `ERR`, `ISS`, or `TIM` is non-zero.

### Text log check (always run, even if JSON also exists)

For each `*.txt` in the list, **fail** if any of these are true:
- `grep -E "OUTSIDE (FAST|STANDARD|EXTENDED) RESPONSE TIME TARGET" <file>` matches → timing issue
- `grep "took longer than its target response time" <file>` matches → timing issue summary
- The file does NOT contain `Congratulations, no errors, warnings or issues found` → errors or issues

### On failure — STOP

Refuse to stage, refuse to commit the ConformU files, and tell the user exactly what failed:

> "ConformU report `<file>` failed validation: Errors=N, Issues=N, TimingIssues=N (or matching grep lines). The driver is not validated. Fix the driver, re-run ConformU until clean, replace the report, and try again. See `/driver-build` Step 10 for the full pass criteria."

Do NOT commit. Do NOT offer to commit "anyway" or "as a WIP". This block exists because a passing-looking commit makes future debugging much harder and falsely advertises the driver as validated in `SUPPORTED-DRIVERS.md` updates that follow.

## Step 4 — Update SUPPORTED-DRIVERS.md (if applicable)

If the changes include **driver code**, **ConformU results**, or **new device support**, check whether `SUPPORTED-DRIVERS.md` needs updating:

1. Read the current `SUPPORTED-DRIVERS.md` to see existing entries
2. If a **new driver** is being committed, add:
   - A new row in the appropriate device type table (Camera, Telescope, Focuser, etc.) with model, connection type, platform checkmarks, and ConformU validation link
   - A **Driver Notes** section below the table with SDK version, connection details, and any quirks
3. If **ConformU results** are being committed for an existing driver, update:
   - Platform checkmark (✓) for arm64
   - The ConformU validation link if a new report directory was added
   - Driver Notes with any new firmware or validation details
4. Update the `## Updated YYYY-MM-DD` date at the top to today's date

### SUPPORTED-DRIVERS.md format reference

Table row format:
```
| Model Name | USB | ✓ | [ConformU Validation](AlpacaCore/conformu/Vendor/Model/) |
```

Driver Notes format:
```
### Vendor Driver Notes

- **SDK**: Vendor SDK vX.Y.Z (build target)
- **Connection**: USB / Wi-Fi / Serial (details)
- **Tested model**: Model on Linux arm64
```

### Also update docs/architecture.md (vendor / SDK table)

`docs/architecture.md` carries a **Vendor drivers** table (`Vendor | Device Types | Wrapper Type
| Status`) and an `external/` SDK listing that are easy to leave behind. Whenever this commit
adds or changes vendor/driver/SDK support, bring that doc current in the **same commit** so it
never drifts — the same trigger as SUPPORTED-DRIVERS.md (new vendor, new device type for an
existing vendor, a new/updated vendor SDK, or a new wrapper).

1. Read the **Vendor drivers** table in `docs/architecture.md`.
2. Reconcile it against what's actually in the tree — derive the truth from
   `ls AlpacaCore/src/vendors/*/` and the vendor's `external/` SDK dir, not from memory:
   - **New vendor** → add a row.
   - **New device type for an existing vendor** → add it to that vendor's **Device Types** cell
     (e.g. Player One `Camera` → `Camera, FilterWheel, Switch`; ZWO Switch `dew heater` → also
     `ASIAIR power`; iOptron/ToupTek gaining a Switch driver, etc.). Be specific the way the
     existing cells are (note the model/subtype in parentheses where the table already does).
   - **Wrapper Type** — `SDK wrapper` for a vendor C library, `Protocol wrapper` for
     serial/network/GPIO, matching Layer 2 of the three-layer architecture. Use
     `SDK + protocol wrapper` when one vendor has both (e.g. ZWO and ToupTek — SDK-based
     cameras plus a protocol/GPIO wrapper for their mount or PowerBox Switch).
   - **Status** — `In development` until the driver is ConformU-validated, then `Production`
     (keep it consistent with whether SUPPORTED-DRIVERS.md lists it as validated).
3. If a **new vendor SDK directory** was added under `AlpacaCore/external/`, add it to the
   `external/` listing in the **Workspace structure** tree in the same doc.
4. Keep the table consistent with the SUPPORTED-DRIVERS.md table maintained in the
   `## Step 4 — Update SUPPORTED-DRIVERS.md` section above — they describe the same drivers
   from different angles and must not disagree about which device types a vendor supports.

## Step 5 — Update CHANGELOG.md

Every commit that changes code or adds features should have a corresponding `CHANGELOG.md` entry. The project uses [Keep a Changelog](https://keepachangelog.com/en/1.0.0/) format.

1. Read the current `CHANGELOG.md` to find the active UNRELEASED section
2. Set the UNRELEASED version per the **Versioning policy** below.
   - If no UNRELEASED section exists, create one at the top (below the header):
     ```
     ## [x.x.x] - UNRELEASED
     ```
     with the version computed from the last **released** version + this change's bump level.
     The previous top section is now a dated, released version and is no longer current, so
     **collapse it** into a `<details>` block per "Collapsible version sections" below.
   - If an UNRELEASED section already exists, ensure its version is **at least** the bump this
     change warrants — **upgrade, never downgrade**. (e.g. UNRELEASED is `[2.0.1]` from earlier
     docs changes and you're now committing a new driver → relabel the heading to `[2.1.0]`.)
3. Add entries under the appropriate subsection within the UNRELEASED block:
   - `### Added` — new drivers, new features, new files
   - `### Changed` — modifications to existing functionality
   - `### Fixed` — bug fixes
   - `### Removed` — removed features or files

### CHANGELOG entry format

Use component-tagged bullet points matching the project style:

```
### Added
- **Vendor Device Driver** (AlpacaCore): brief description of what was added
- **Vendor Device Support** (AlpacaHTTP): router registration, web UI config
- **Vendor Unit Tests**: X test cases, Y assertions

### Changed
- **Vendor Device Driver** (AlpacaCore): what was changed and why

### Fixed
- **Vendor Device Driver** (AlpacaCore): what was broken and how it was fixed
```

### Rules

- **Always use the UNRELEASED version** — never commit with a release date; that happens at release time
- **Be specific**: include vendor name, device model, and technical details
- **Group related changes** under one bullet with sub-points for complex entries (see existing entries for style)
- **Don't duplicate**: if an entry for this driver/feature already exists in UNRELEASED, update it rather than adding a new one
- If the current UNRELEASED section already has the right version number, add to it — don't create a new one

### Collapsible version sections

`CHANGELOG.md` keeps every version section collapsible so the file folds to a scannable list of
versions (same `<details>`/`<summary>` pattern as `SUPPORTED-DRIVERS.md`). The rule:

- **The current section stays expanded** — the top section (the `## [x.x.x] - UNRELEASED`
  heading, or the latest dated release when there's no UNRELEASED yet) is a plain `##` markdown
  heading, NOT wrapped in `<details>`. This is the section `/commit` edits; leave it expanded.
- **Every older, released version is collapsed.** Each is wrapped like:
  ```
  <details>
  <summary><strong>[2.0.0] - 2026-06-13</strong></summary>

  ### Added
  - ...

  </details>
  ```
  Note the blank line after `</summary>` and before `</details>` (GitHub needs it to render the
  markdown inside). The version + date go in `<summary><strong>…</strong></summary>`, replacing
  the `## [x.x.x] - date` heading.

When you open a **new** UNRELEASED section at the top (step 2), collapse the section it displaces
— the now-released top version — into this `<details>` form so only the current one stays open.
Don't touch the already-collapsed older sections.

`scripts/changelog_to_deb.py` parses both the `## [..]` heading and the collapsed `<summary>`
form, so Debian changelog generation is unaffected by collapsing — no need to special-case it.

### Versioning policy (Semantic Versioning)

AlpacaBridge is an end-user appliance, so "breaking" means **breaks an existing user's install/
setup**, not a code-API break. Bump relative to the last **released** version:

| Bump | When | Examples |
|------|------|----------|
| **MAJOR** `x.0.0` | Breaks an existing user | Drop a platform (amd64 → 2.0.0), remove a driver, config-format change needing migration, change a default that alters behavior |
| **MINOR** `x.Y.0` | New backward-compatible capability (resets patch to 0) | **A new driver**, new device/model support, a new optional feature/flag |
| **PATCH** `x.y.Z` | No new capability | Bug fix to an existing driver, ConformU re-validation, packaging fix, docs/skill/spec changes |

Quick test: **broke** an existing user → major; **added** something new → minor; **fixed/
polished** what already existed → patch.

**Cumulative carry-forward.** The UNRELEASED version reflects the **highest-severity** change
accumulated since the last release. A new driver is always a minor bump, never a patch —
e.g. `2.0.0` → docs `2.0.1` → (later) driver `2.1.0` (minor resets patch, not `2.0.2`) → docs
`2.1.1`. Within one UNRELEASED cycle, only ever raise the version, never lower it.

**`/commit` touches only `CHANGELOG.md` for versioning** — it sets the `UNRELEASED` heading and
nothing else. Do **not** modify the `VERSION` file or the `#### [x.x.x] - …` version badge in
`README.md` in this flow. Those are release actions: `/submit-pr` asks whether to bump them when
a release is being cut. `VERSION` is the canonical version read by `scripts/build_deb.sh`. If you
notice `VERSION` or the README badge disagrees with what a release should be, flag it for the
user — don't silently edit it here.

## Step 6 — Stage the right files

Stage files that belong together in one logical commit. Prefer specific file paths over `git add -A` or `git add .`.

```bash
git add <specific files>
```

If changes span multiple logical units (e.g., driver code + ConformU results + docs), ask the user whether to commit them together or split into separate commits. Common split patterns in this project:

1. **Driver implementation** — driver code + protocol/SDK wrapper + tests + CMake
2. **ConformU validation** — ConformU result files + SUPPORTED-DRIVERS.md updates
3. **HTTP/Web UI integration** — router + web UI + routing tests
4. **Documentation** — CHANGELOG, AGENTS.md, SUPPORTED-DRIVERS.md
5. **SDK addition** — external/ SDK files (often a large commit on its own)

## Step 7 — Write the commit message

Draft a commit message following the project's conventions observed in the git history.

### Format

```
<Summary line — what changed and why, under 72 characters>

<Optional body — details, grouped by component, only if the summary isn't sufficient>
```

### Summary line rules

- **Start with a verb**: Add, Fix, Update, Implement, Remove, Rewrite, Validate
  - `Add` = wholly new feature or file
  - `Fix` = bug fix
  - `Update` = enhancement to existing feature
  - `Implement` = new capability within existing code
  - `Rewrite` = significant rework
  - `Validate` / `Certify` = ConformU validation results
  - `Remove` / `Delete` = removing code or files
- **Be specific**: "Add ToupTek camera driver with end-to-end HTTP/UI integration" not "Add new driver"
- **Include the vendor and device when relevant**: "Fix iOptron HEM27 Wi-Fi pulse guide timing"
- **Include ConformU results when relevant**: "iOptron HEM27 ConformU 4.3.0 validated: 0 errors, 0 issues"
- **Under 72 characters** for the summary line

### Body (when needed)

Use a body for commits that touch multiple components or need explanation. Group by component using bold tags matching the CHANGELOG style:

```
Add Player One camera driver with end-to-end HTTP/UI integration

- **Player One Camera Driver** (AlpacaCore): SDK wrapper singleton,
  exposure via single-frame software trigger, ST4 pulse guiding,
  RAW8/RAW16/RGB24/MONO8 format support
- **Player One Device Support** (AlpacaHTTP): router registration,
  web UI vendor selection and camera-index config
- **Player One Unit Tests**: 6 test cases, 28 assertions
- **Player One SDK**: v3.10.0 libraries under external/PlayerOne/
```

### Examples from project history

Good commit messages:
- `Add ToupTek camera driver with end-to-end HTTP/UI integration`
- `Fix Celestron CGX-L SideOfPier and RA slew accuracy for ConformU 4.3.0`
- `iOptron Wi-Fi timing fix: release driver mutex during clock sync network I/O`
- `SynScan HEQ5 PRO pulse guide, auto-detection, and ConformU 4.3.0 validation`
- `Warm up SVBONY writable controls at connect to unlock gain writes`
- `Add SVBONY exposure watchdog so CameraState recovers from SDK hangs`

Avoid:
- `Updated files` / `Fixed bug` / `WIP` / `misc changes`
- Messages that don't say what vendor/device was affected
- Messages longer than 72 characters on the summary line

## Step 8 — Commit

Present the staged files and draft message to the user for approval. Then commit:

```bash
git commit -m "$(cat <<'EOF'
<commit message here>

Co-Authored-By: Claude <noreply@anthropic.com>
EOF
)"
```

After committing, run `git status` to confirm the working tree is clean (or show what's left unstaged).

## Step 9 — Follow-up suggestions

After the commit, suggest next steps if appropriate:
- "There are more unstaged changes — want to commit those separately?"
- "AGENTS.md vendor notes could be updated with what we learned — want to do that?"
- "Tests haven't been added yet — this driver needs unit tests before it's PR-ready."
- "Want to push this branch to the remote?"
- "Ready to submit a PR? Use `/submit-pr`."

Do NOT push automatically — always ask first.
