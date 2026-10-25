# Design decisions

One `NNNN-<slug>.md` record per choice that passes all three tests below,
with a Status line under the title, then Context, Decision, Rules,
Alternatives rejected, Consequences and Links. Two optional sections may sit
between them: Adoption and How to reverse. `scripts/check_docs_drift.py`
check 22 holds every record to this shape. The how-to of a rule stays at its
implementation or instruction owner; the record keeps the reasoning and the
short, checkable form below.

**Who reads a record.** The review bot and the agents that edit this
repository, first; people second. A rule the bot cannot apply from the record
alone is a defect in the record, not a style choice.

**What earns a record.** A choice gets a record only when all three hold:

1. Hard to reverse: changing it later costs real work across drivers or tools.
2. Surprising without context: a future reader would ask why it was done this way.
3. A real trade-off: there were alternatives and one was picked for named reasons.

A choice that fails any of the three is a plain rule in the matching topic
file under `.github/instructions/`, not a record.

**Rules.** The rules a change is checked against, one numbered line each:

```
1. <the rule, in one or two sentences>. Scope: `<glob>`[, `<glob>`]. Checked by: <test, script check, or review (<what to look for>)>.
```

A glob matches repository paths (`*` stays in one directory, `**` crosses
them) and must match at least one tracked file. `Scope: `**`` (any file) is
allowed only in [0013](0013-designs-to-avoid.md), whose rules reject designs
rather than govern files, and puts the rule in scope for every pull request;
elsewhere it marks a rule that belongs there or in a topic file. "Checked by" names the test or check that fails when the rule is
broken, or says `review (<what to look for>)` with the evidence a reviewer
looks for. A bare `review` is allowed only for the rules that had it when the
form changed; `DECISION_BARE_REVIEW_BASELINE` in the drift script counts them
per record and may only fall, so a touched record converts its bare rules and
lowers its figure. A rule states its own bound; it never carves out code that
deviates (that is a defect with an issue). The words except, unless, until and
for now fail check 22 on a rule line unless the line ends with
`<!-- allowed -->`, which says the bound is the rule itself. About twelve
rules is the point at which a record is carrying two topics; past it, the PR
body says why the record is not split. A superseded or retired record needs no
Rules.

**How to reverse.** Required for every record numbered 0020 or later, and for
any record that supersedes another: the record passed the hard-to-reverse
test, so this section is the part a future maintainer needs most. Earlier
records add it when next touched.

**Adoption.** A record that drivers adopt one at a time keeps its roster here,
not in prose. `scripts/check_docs_drift.py` check 23 compares it with the code:

```
Scan: `<glob of files to scan>`
Symbol: `<identifier an adopter names>`

| Driver | Source | State | Issue | Notes |
```

State is `adopted`, `partial` or `planned`. A `partial` row names the issue
that finishes it; code that deviates from a record is a defect with an issue,
never an exception written into the record. A `planned` row may give `—` as its
source. A `Symbol` column overrides the symbol for its row, and a blank cell
uses the `Symbol:` line. With a `Symbol:` line, only scanned files that name it
need a row; with none, every scanned file does. A symbol named only in a comment
does not count.

**Changing a record.** A change to any file in this directory, this index
included, needs two things before it can merge:

- A repository collaborator with write, maintain or admin permission comments
  `ADR authorized <sha>` on the PR, on a line of its own, naming a commit,
  normally the current head. The authorization holds for later pushes while
  every record file the PR changes keeps the content it had at that commit; a
  push that edits one, adds one or deletes one needs a new authorization,
  and a push that changes only code or tests does not. The author may
  authorize their own PR. `scripts/check_adr_authorization.py` posts the result
  as the `adr-authorization` commit status (`.github/workflows/decision-records.yml`).
- An approving review from a code owner other than the author
  (`.github/CODEOWNERS`).

Both are required by the "Decision records" ruleset, which repository
maintainers and admins may bypass. The PR body's `## Decision records` section
names every record the PR changes (`scripts/check_adr_scope.py`). The review
bot reads the Rules each changed file falls under and reports a broken rule as
a defect; it also checks a new record against the three tests above and its
Consequences for carve-out language.

The gate is a tripwire, not peer review: the author may authorize their own
PR and a maintainer may bypass the ruleset. What it guarantees is that no
record changes without a named person acknowledging it at a named commit,
which is the protection against an agent editing a rule to fit its diff.

Status is one of:

- `proposed`: written so a pilot change can be reviewed against it; not yet a rule.
- `accepted`: the rule. A record can be accepted before its code lands, when the
  migration that implements it is planned in slices.
- `superseded by NNNN`: kept for history; the named record is the rule.
- `retired (#NNNN)`: kept for history; the choice stopped being true with no
  successor (the module or hardware is gone). The issue or PR that retired it
  is named on the Status line; the Rules and Adoption sections are removed and
  the rest is frozen; the index line below carries a `(retired)` suffix.

A `proposed` record that is rejected is deleted, not kept. A record is
referred to by its slug until it is written, and numbered then.

- [SkyWatcher pointing clock and UTCDate readback](0001-skywatcher-pointing-clock.md)
- [HTTP server thread ownership](0002-server-thread-ownership.md)
- [Documentation drift gates](0003-docs-drift-gates.md)
- [Device catalog](0004-device-catalog.md)
- [Task clock](0005-task-clock.md)
- [Async operation ownership](0006-async-operation-ownership.md)
- [LAN surface threat model](0007-lan-surface-threat-model.md)
- [Rotator Sync offset persistence](0008-rotator-sync-offset-persistence.md)
- [Link-loss and relink policy](0009-link-loss-and-relink-policy.md)
- [Snapshot-served state](0010-snapshot-served-state.md)
- [CGX logical-disconnect reference continuity](0011-cgx-logical-reference-continuity.md)
- [Bundled libgphoto2](0012-bundled-libgphoto2.md)
- [Designs to avoid](0013-designs-to-avoid.md)
- [Mount UTC offset](0014-utc-offset.md)
- [SDK seams](0015-sdk-seams.md)
- [Getters never wait on device I/O](0016-getters-never-wait-on-device-io.md)
- [Serial port ownership](0017-serial-port-ownership.md)
- [Persisted UniqueID per configured device](0018-persisted-unique-id.md)
- [Connect without a site, refuse what needs one](0019-connect-without-site.md)
- [Camera capture session](0020-capture-session.md)
