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
