# Design decisions

One `NNNN-<slug>.md` record per non-obvious choice or reversal, with a Status
line under the title, then Context, Decision, Alternatives rejected,
Consequences and Links. Keep the current rule at its implementation or
instruction owner and link to the rationale here.

Status is one of:

- `proposed`: written so a pilot change can be reviewed against it; not yet a rule.
- `accepted`: the rule. A record can be accepted before its code lands, when the
  migration that implements it is planned in slices.
- `superseded by NNNN`: kept for history; the named record is the rule.

A record is referred to by its slug until it is written, and numbered then.

- [SkyWatcher pointing clock and UTCDate readback](0001-skywatcher-pointing-clock.md)
- [HTTP server thread ownership](0002-server-thread-ownership.md)
- [Documentation drift gates](0003-docs-drift-gates.md)
- [Device catalog](0004-device-catalog.md)
- [Task clock](0005-task-clock.md)
