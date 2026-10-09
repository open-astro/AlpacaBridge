# Beta channel and stable branches

Development stays on `main`. Each minor or major release is cut onto a stable branch
`stable/X.Y`, shipped to an opt-in apt `beta` component, hardened with bug fixes only,
and then promoted to stable. Day-to-day PR work on `main` does not change.

## For users: opt in to the beta

Do both edits together. The web UI update check does not read apt sources; it fetches only
`update_packages_url`, and the install always goes through apt. With the apt line alone the
update card never offers a beta. With the URL alone it offers a beta that apt cannot find.

1. Add `beta` to the apt line (same line as the README install step):

   ```sh
   echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/openastro-archive-keyring.gpg] \
   https://apt.openastro.net trixie main beta" \
       | sudo tee /etc/apt/sources.list.d/openastro.list
   ```

2. Under `server:` in the AlpacaBridge config set (see [software-update.md](software-update.md)):

   ```yaml
   update_packages_url: https://apt.openastro.net/dists/trixie/beta/binary-arm64/Packages
   ```

The URL names `binary-arm64` on purpose: AlpacaBridge ships for arm64 only.

**Opt out** by removing `beta` from the apt line and restoring the default
`update_packages_url`. apt does not downgrade on its own, so a user who leaves mid-beta stays
on the beta build until the stable release overtakes it.

Every stable release is published to both `main` and `beta`, so the beta index always holds the
newest build of either kind.

## Branch model

- `main`: all development. PRs go here as usual.
- `stable/X.Y`: long-lived, cut from `main` when it holds what the release should contain. The
  name is `stable/X.Y`, not `release/...`, because `/bump-release` uses `release/X.Y.Z` for its
  short-lived release PR branch.
- At most two stable branches exist at once: the current stable and, during a beta, the next one.

## Lifecycle of one release

1. **Cut.** Branch `stable/X.Y` from `main`. A release PR on that branch sets `VERSION` to
   `X.Y.0~beta1` (or the next `~betaN`), updates the README badge, and adds
   `docs/releases/X.Y.0-beta.N.md`, generated with
   `python3 scripts/changelog_fragments.py --preview --version X.Y.0~betaN`. Tag the merge
   `vX.Y.0-beta.N`. The `changelog.d/` fragments are not consumed. A legacy
   `## [A.B.C] - UNRELEASED` heading in `CHANGELOG.md` is renamed to `X.Y.0` (heading only), so
   the `.deb` build does not warn that it disagrees with `VERSION`.
2. **Publish.** `release.yml` creates a GitHub pre-release from the notes file. The `.deb` is
   published to the apt `beta` component only (outside this repository).
3. **Fix.** A bug found in beta is fixed by a PR against `stable/X.Y`. It is not fixed on `main`
   first and cherry-picked. A fix for code the branch does not have goes to `main` as usual.
4. **Re-beta and merge down.** Each batch of fixes becomes the next beta tag. After every beta
   tag, one PR merges `stable/X.Y` into `main` with the merge-commit method (never squash). Its
   head is a short-lived `merge-down/X.Y-to-main` branch cut from `stable/X.Y`, never the stable
   branch itself (the head of a merged PR is deleted, and updating a PR head merges `main` into
   it); a ruleset protecting `stable/**` against deletion and force-pushes is required first.
   **Version files:** every merge down keeps the receiving branch's `VERSION` and README badge, so
   `main` never carries a beta `VERSION` and a dev build from `main` never reports the same version
   as a published beta. The one exception is a merge into `main` that brings a stable release
   newer than `main`'s `VERSION` (step 6, or a hotfix before the next promotion): that release is
   now the newest, so its `VERSION` and badge come across. `docs/releases/` notes and code come
   across as they are. `/submit-pr --merge-down` resolves this in the head before the PR opens.
5. **Promote.** `/bump-release` runs on `stable/X.Y`: `VERSION` becomes `X.Y.0`, the fragments are
   assembled into the dated section, tag `vX.Y.0`. The package goes to both apt components.
6. **Final merge down.** Merge `stable/X.Y` into `main` once more. It carries the dated CHANGELOG
   section, the `VERSION` and badge bump (the exception above), and the deletion of the consumed
   fragments. If `main` touched the same lines as a branch fix since the last merge down, the
   maintainer resolves the conflict by hand; merging after every beta keeps these small.
7. **Hotfix.** A stable regression becomes `X.Y.1` on the same branch: fixed on `stable/X.Y`,
   tagged, then merged down. No beta round is needed unless the maintainer wants one. Once
   `stable/X.(Y+1)` exists, merge the hotfix down into both it and `main`, as two PRs with two
   heads (`merge-down/X.Y-to-main` and `merge-down/X.Y-to-X.(Y+1)`; `/submit-pr --merge-down
   stable/X.Y --into stable/X.(Y+1)` for the second), so merging one never deletes the other's
   head. The version-file rule above applies to both; in `CHANGELOG.md` the dated `## [X.Y.1]`
   section is inserted in version order below the receiving branch's sections; the code fix and
   its consumed fragment come across as they are. The falsified-by gate exempts both PRs
   (`scripts/merge_down.py`).
8. **Retire.** `stable/X.Y` stays maintained for hotfixes while the next branch is in beta, and
   retires when the next minor or major is promoted, not when its branch is cut. A retired branch
   stays for history and gets no further tags.

## Rules for the stable branch

- Bug and stability fixes only: crashes, hangs, races, wrong ASCOM behaviour, packaging and
  install failures, regressions from the previous stable, and docs fixes for the release itself.
  No features, no refactors, no new drivers.
- Fixes land on the stable branch and reach `main` through the merge down. No cherry-picks.
- Fix PRs target `stable/X.Y` (`/submit-pr --base stable/X.Y`), run the same CI and review bot, and
  each adds its `changelog.d/` fragment as usual; the fragment travels to `main` in the merge down
  and is consumed at promotion.
- CI and CodeQL also run on pushes to `stable/**`.
- Protection on `stable/**` matches `main` (required checks, pull requests only, no deletion or
  force-push). It is the repository ruleset "stable branch protection" (id 24766374, #924), set in
  GitHub settings, not in this repository; it also allows only the merge-commit method and does
  not enforce status checks on branch creation, so cutting `stable/X.Y` from `main` is not
  blocked. Classic branch protection is not enough for the merge downs: `/submit-pr --merge-down`
  checks that an active ruleset on `refs/heads/stable/**` has the `deletion` and
  `non_fast_forward` rules and stops without it.

## Promotion criteria

All three hold before promoting:

- At least 14 days since the last beta tag with no open regression labelled for that release.
- At least one full rig session by the maintainer on the final beta build.
- A ConformU re-run on the rig for every driver touched during the beta.

## Who does what

The upstream maintainer and diegopereiran cut stable branches, tag betas, promote, and open the
merge-down PR. Contributors send fix PRs against `stable/X.Y` like any other PR.

## Version spellings

Git refs cannot contain `~`, and Debian needs `~` to sort a pre-release below its final version.
The web UI update check compares versions by Debian rules, where `5.0.0-beta.1` would sort above
`5.0.0`; so `VERSION` uses the Debian spelling and only the git tag uses the hyphen form.
`scripts/release_tag.py` maps one to the other for `release.yml`.

| Stage       | Git tag       | `VERSION` file | .deb version | apt component |
| ----------- | ------------- | -------------- | ------------ | ------------- |
| First beta  | v5.0.0-beta.1 | 5.0.0~beta1    | 5.0.0~beta1  | beta          |
| Second beta | v5.0.0-beta.2 | 5.0.0~beta2    | 5.0.0~beta2  | beta          |
| Stable      | v5.0.0        | 5.0.0          | 5.0.0        | main + beta   |
| Hotfix      | v5.0.1        | 5.0.1          | 5.0.1        | main + beta   |
| Next beta   | v5.1.0-beta.1 | 5.1.0~beta1    | 5.1.0~beta1  | beta          |

Debian order: `5.0.0~beta1 < 5.0.0~beta2 < 5.0.0 < 5.0.1 < 5.1.0~beta1`, so a beta user moves to
the stable release when it ships and on to the next beta after, with no manual switching.
`changelog_fragments.py` reads a beta `VERSION` as its base version (`--bump --version
5.0.0~beta2` prints `5.0.0` at least; `--release` takes the bare `5.0.0` only, so a beta never
consumes the fragments).
