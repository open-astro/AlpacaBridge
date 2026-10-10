# Issue tracker: GitHub

Issues and specs for this repo live as GitHub issues. Use the `gh` CLI for all operations.

Issues live on **`open-astro/AlpacaBridge`**, not on whichever remote a given
clone happens to call `origin`. Every example below carries
`--repo open-astro/AlpacaBridge` explicitly for that reason — copy it as
written, not the remote name from your own checkout.

## Conventions

- **Create an issue**: `gh issue create --repo open-astro/AlpacaBridge --title "..." --label P<n> --body "..."`, with the priority label from the [Priority](#priority) scale below and a type label (`bug`, `enhancement` or `documentation`). Use a heredoc for multi-line bodies.
- **Read an issue**: `gh issue view <number> --repo open-astro/AlpacaBridge --comments`, filtering comments by `jq` and also fetching labels.
- **List issues**: `gh issue list --repo open-astro/AlpacaBridge --state open --json number,title,body,labels,comments --jq '[.[] | {number, title, body, labels: [.labels[].name], comments: [.comments[].body]}]'` with appropriate `--label` and `--state` filters.
- **Comment on an issue**: `gh issue comment <number> --repo open-astro/AlpacaBridge --body "..."`
- **Apply / remove labels**: `gh issue edit <number> --repo open-astro/AlpacaBridge --add-label "..."` / `--remove-label "..."`
- **Close**: `gh issue close <number> --repo open-astro/AlpacaBridge --comment "..."`

## Priority

Every open issue carries exactly one priority label, `P1` to `P5`. Add it when
you create the issue (`gh issue create --repo open-astro/AlpacaBridge --label P3
...`), and keep the priority out of the title. Issues closed before 2026-10-10
still carry the old `[P1]`-`[P5]` title prefix instead; leave those as they are.
Set the priority from the impact you verified against the current code, not
from the reporter's guess or a review bot's badge: a bot labels by its own scale,
and on a stale diff it can flag code the change never touched.

| Level | Meaning | Test | Example |
|---|---|---|---|
| `P1` | Unsafe or broken for real users now | Moves hardware unasked or ignores a stop or limit on a supported path; loses or corrupts saved config; crashes or hangs the server; blocks a release | #768, #763 |
| `P2` | Wrong behavior a user or ConformU will hit | Wrong answer from a commonly used member; an ASCOM contract violation ConformU flags; a safety guard with a meaningful gap; a driver that fails validation | #880, #824, #832 |
| `P3` | Real defect, narrow trigger | Needs an unusual sequence, timing or configuration; has a workaround; degrades but does not break | #870 |
| `P4` | Latent, cosmetic, tooling or docs | No user impact today; only in a non-default setup; developer tooling or documentation | #889, #890 |
| `P5` | Follow-up or housekeeping | Refactor, report refresh, added test coverage, nice-to-have | #610, #611 |

Rules:

- **Hardware safety raises the level by at least one, to no lower than `P2`.**
  That covers anything that can move a mount, focuser, rotator, filter wheel or
  cover the user did not ask for, or keep it moving after a stop, an abort or a
  limit.
- **Between two levels, take the higher one when hardware can move**, and the
  lower one otherwise.
- **Change the label when new evidence changes the impact** (`gh issue edit
  <number> --repo open-astro/AlpacaBridge --remove-label P4 --add-label P2`), and
  say why in a comment.
