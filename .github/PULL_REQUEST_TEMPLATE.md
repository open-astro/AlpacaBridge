<!-- Write all pull request text in Simplified Technical English (ASD-STE100): short sentences, one instruction per sentence, simple approved vocabulary, and the active voice. -->

## Thinking Path

<!--
  Required. Trace your reasoning from the top of the project down to this
  change: what AlpacaBridge is, the subsystem, driver or device involved,
  the problem, why it matters, what this pull request does, and the benefit.
  Use blockquote style. Aim for 5-8 steps.
-->

> - AlpacaBridge is the open source server that runs native ASCOM Alpaca drivers on a single-board computer at the telescope
> - [Which subsystem, driver or device is involved]
> - [What problem or gap exists]
> - [Why it needs to be addressed]
> - This pull request ...
> - The benefit is ...

## Linked Issues or Issue Description

<!--
  Required. Pick ONE of the two paths below.

  (A) Issue exists: link it with `Fixes #123`, `Closes #123` or `Refs #123`.
      Link duplicates and closely related issues and pull requests too, with
      a short note on why each is related.

  (B) No issue exists: describe the problem here. Keep these bold labels,
      each alone on its own line, with real content under each:
        Bug:     **What happened?**  **Expected behavior**  **Steps to reproduce**
        Feature: **Problem**  **Proposed change**  **Alternatives considered**
      (the same fields as .github/ISSUE_TEMPLATE/bug_report.md and
      feature_request.md).

  Only reference PUBLIC GitHub issues and pull requests (`#NNN` or
  github.com/open-astro/AlpacaBridge URLs). No private tracker ids, internal
  links, or localhost/LAN addresses: other contributors cannot open them.
-->

-

## What Changed

<!-- Bullet list of concrete changes. One bullet per logical unit. Name the file or component first (driver, AlpacaHTTP router, web UI, tests, docs, CI). -->

-

## Verification

<!--
  How can a reviewer confirm this works? Give the exact commands and their
  results: the RED run of each new test before the fix, the GREEN run after
  it (`ctest -R <tests>`, count of passed tests), the build, format and lint
  checks. Name each check you did not run locally and what covers it (for
  example: whole ctest suite, sanitizers, arm64 build -> pull request CI).
  Hardware: name the device, firmware and platform, or say "not tested on
  hardware". Driver pull requests: give the ConformU result and the report
  path (AlpacaCore/conformu/<Vendor>/<Model>/<arch>/).
  Web UI changes: add a Before / After screenshot table.
-->

-

## Risks

<!--
  What could go wrong? Mention behavior changes visible to clients, config or
  settings migration, hardware safety (slews, parking, cooler, motion limits),
  breaking changes, or "Low risk" if it is genuinely minor.
-->

-

## Model Used

<!--
  Required. Name the AI model used to produce or assist with this change:
  provider and model name, exact model ID, context window if relevant,
  reasoning mode, and tools (shell, file edits, test runs).
  If no AI model was used, write "None - human-authored".
-->

-

## Checklist

- [ ] I have included a thinking path that traces from project context to this change
- [ ] I have specified the model used (with version and capability details)
- [ ] I have searched GitHub for duplicate or related issues and pull requests and linked them above
- [ ] I have either (a) linked existing issues with `Fixes #` / `Closes #` / `Refs #` OR (b) described the issue in-PR following the relevant issue template
- [ ] I have not referenced private or internal tracker ids or links (only public GitHub `#NNN` / `github.com/open-astro/AlpacaBridge` URLs)
- [ ] My branch name describes the change (e.g. `docs/...`, `fix/...`) and contains no private tracker id
- [ ] I have followed AGENTS.md (build, test, formatting and ASCOM contract rules)
- [ ] I have run the targeted tests locally and they pass
- [ ] I have added or updated tests where applicable
- [ ] I have added a `changelog.d/` fragment (AGENTS.md changelog rule) and updated the relevant documentation
- [ ] I have considered and documented any risks above
- [ ] All CI gates are green
- [ ] Claude Review passes with no open P1, P2s, recommendations, or follow-ups
- [ ] I will address all review-bot and reviewer comments before requesting merge
