# Changelog fragments

Every PR that changes code, tests, scripts, CI or docs adds **one file** here
instead of editing `CHANGELOG.md`. Two PRs can then merge in any order without a
conflict, because no two of them touch the same file. `CHANGELOG.md` is written
only by the release step (`/bump-release`, which runs
`scripts/changelog_fragments.py --release`).

## File name

`changelog.d/<branch-slug>.md`, where `<branch-slug>` is the branch name after its
last `/` (branch `fix/fix-741-request-body-cap` gives
`changelog.d/fix-741-request-body-cap.md`). Allowed characters:
`[a-z0-9][a-z0-9._-]*`. One fragment per PR; a later commit on the same PR edits
the same file. Every file in `changelog.d/` other than this README is treated as a
fragment, so keep scratch files elsewhere.

## Body

One or more `### <Category>` subsections, each with at least one `- ` bullet. A
bullet uses the existing entry style: a bold summary, the component and the
upstream issue (`issue #N`), then the detail. Write each bullet on one line, however
long; do not wrap it. A nested `- ` bullet, a fenced code block, or a paragraph
after a blank line may follow a bullet; `--check` refuses any other line that
continues one. A fragment has no `#` or `##` heading, no version and no date; the
release supplies them.

Categories, in the order the release writes them:

- `Breaking changes`
- `Added`
- `Changed`
- `Deprecated`
- `Removed`
- `Fixed`
- `Security`

Any of them may be followed by a qualifier in parentheses, for example
`Added (tests)` or `Fixed (tooling)`. A qualified category is written right after
its base category.

```markdown
### Fixed
- **Bisque/TheSkyX: slew refuses NaN and infinity** (AlpacaCore, issue #627): what was wrong, what it does now.

### Added (tests)
- **Bisque NaN slew cases** (issue #627): three test cases.
```

## Breaking or not

"Breaking" means it breaks an existing user's setup, not a code API. Ask this of every change on
the branch, fixes included: **could someone who changes nothing but the package version see
something that worked on the last release stop working?** That means a device that loaded and now
does not, a call that succeeded and now errors, or a client that got an answer and now gets a
different one it has to handle. If yes, the change needs a `### Breaking changes` bullet, even when
it is also a fix. Keep the `Fixed` or `Changed` bullet that explains the fix, and add the Breaking
bullet for the consequence. Write the consequence and the upgrade step only in the Breaking bullet;
the original bullet ends with "for saved entries, see Breaking changes" (or similar) instead of
repeating them.

It is breaking when:

- **A saved config that loaded no longer loads.** Saved devices are checked again at every
  start-up. A new allowlist or a stricter type in a `register_device_from_config()` arm, or a
  stricter type in a catalog field, drops a saved device that the last release accepted. Two
  examples from the release after 4.2.0: GPIO lines outside the board's allowlist (issue #765),
  and `9600.5` in a catalog field that is now a whole number. A catalog field's out-of-range
  saved value is not dropped: start-up replaces it with the field's default ("config
  normalized" in the log). That is still a visible change, so say it in the entry.
- **A request that succeeded now fails**: a new refusal, a removed fallback, a removed retry or
  a stricter precondition. Examples: SynScan `Tracking=true` with no site latitude, and an iOptron
  GOTO that the driver used to retry with relaxed limits (issue #763).
- **A default, a config key, a field, an endpoint, a driver or a platform changes or goes away.**
- **The user has to act after upgrading**: re-save a device, edit the config, set a value or switch
  driver. If your bullet says "re-save it", "set X first" or "use Y instead", that bullet belongs
  under `Breaking changes`.

It is not breaking when:

- the old behaviour could not work for anyone (a value the driver already refused at connect is
  now refused at save);
- only the error number changes, to the one the ASCOM contract requires (InvalidValue before
  NotConnected);
- only log text or a diagnostic message changes;
- the change is a new optional feature that is off unless you enable it.

**A Breaking changes bullet** says what stops working, who is affected (the vendor, field or model)
and what to do, in a bold **After upgrading:** sentence. One `Breaking changes` entry makes the
release a major version (see Version below), so `/commit` and `/submit-pr` ask this question of
every fragment.

**An entry belongs to the release its code ships in.** Write it in your fragment; never add it to
a dated `## [X.Y.Z]` section of `CHANGELOG.md`. Three entries for the release after 4.2.0 were
written into the released 4.2.0 section on 2026-09-30 and had to be moved. The one exception is a
correction: a PR may edit `CHANGELOG.md` to move a misfiled entry or fix a wrong one, and its
description must say so.

## Version

The contributor never picks a version. The release derives it from the fragments
with `python3 scripts/changelog_fragments.py --bump`: major when any fragment has
`Breaking changes`, minor when any has an unqualified `Added` (a new driver or
feature), otherwise patch. A qualified `Added (tests)` does not count as minor.

## Commands

```bash
python3 scripts/changelog_fragments.py --check     # validate every fragment (CI runs this)
python3 scripts/changelog_fragments.py --preview   # the section the release would write
python3 scripts/changelog_fragments.py --bump      # the proposed next version
python3 scripts/changelog_fragments.py --self-test
```

A legacy `## [X.Y.Z] - UNRELEASED` section still in `CHANGELOG.md` is merged into
the release by category (its entries first, then the fragments by file name) and
its heading is removed.
