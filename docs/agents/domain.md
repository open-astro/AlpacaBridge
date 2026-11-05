# Domain Docs

How the engineering skills consume this repo's domain documentation when exploring the codebase. This is a single-context repo; the files keep their existing names rather than the stock layout the skills assume (a root glossary file plus an adr folder under docs), because `AGENTS.md` and the drift-check scripts reference them by name.

## Before exploring, read these

- **`CONTEXT.md`** at the repo root is the glossary: terms a contributor must not confuse, each with what not to call it.
- **`docs/decisions/`** holds the ADRs (decision records), indexed by `docs/decisions/README.md`. Read the records that touch the area you are about to work in. Their `Rules` sections are binding on every change, and changing a record needs a collaborator's authorization (see `AGENTS.md`).
- **`docs/failures/`** holds resolved incident records. Read them as evidence, not as current rules.

Where a skill names a root glossary file, read `CONTEXT.md`; where it names an adr folder under docs, read `docs/decisions/`. New terms go in `CONTEXT.md`; new decisions go in `docs/decisions/` following the shape of the existing records and the README index.

## Use the glossary's vocabulary

When your output names a domain concept (in an issue title, a refactor proposal, a hypothesis, a test name), use the term as defined in `CONTEXT.md`. Don't drift to synonyms it explicitly avoids (device number vs enumeration index, cancelled vs superseded, Connected vs link health, fake vs rig evidence).

If the concept you need isn't in the glossary yet, that's a signal: either you're inventing language the project doesn't use (reconsider) or there's a real gap (note it for `/domain-modeling`).

## Flag ADR conflicts

If your output contradicts an existing decision record, surface it explicitly rather than silently overriding:

> _Contradicts decision 0016 (getters never wait on device I/O), but worth reopening because…_

Never write a code deviation into a record as an exception; file an issue to fix the code instead.
