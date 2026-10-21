---
applyTo: ".github/workflows/**,.github/scripts/**,scripts/**"
---

## CI gates and the review bot

Shared rules for every vendor, moved from `AGENTS.md` (which keeps the core rules
and routes here); the CI section is condensed to author rules. A rule that applies
to a second vendor belongs in the matching shared file, not in a vendor file.

### Review bot on fork PRs (`safe-to-review` label)

The Claude review bot (`.github/workflows/claude-review.yml`) runs automatically on
every push to a same-repo PR branch. Fork PRs are gated: the bot runs only while a
maintainer has applied the **`safe-to-review`** label — applying it triggers the
first review immediately, later pushes keep reviewing while the label stays on, and
removing the label stops the bot. The label is the maintainer's trust decision: the
workflow runs with `pull_request_target` (definition always taken from `main`, so a
fork can't alter the bot's prompt/tools), and the residual risk of the bot *reading*
hostile PR content is accepted per-PR by whoever applies the label.

Trusted fork contributors are also listed in the workflow's `allowed_non_write_users`
input (specific usernames, never `*`): without it, a fork contributor's own push to a
labeled PR fails the review job in seconds ("Actor does not have write permissions").
To onboard a new fork contributor, add their username to that list — do **not** grant
them collaborator/write access; the `safe-to-review` label stays the per-PR trust gate.

The bot reviews only a head whose CI build and test jobs passed: the job's first step
("Wait for the CI build and test jobs", which owns the list) waits up to 45 minutes for them
and fails before the agent runs when one is red, so a red push costs no review. The step
matches those jobs' `name:` in `ci.yml`, so renaming one means updating the list in the same
PR (the step fails at once with "never reported" otherwise).
`/pr-checker` and `/submit-pr` read that failure as poll exit `4` (CI red: fix and push) or
`5` (CI did not finish in time: approve or wait, then re-run the review), never as a broken
review workflow.

## Continuous Integration and Pre-flight

Related decision: [Documentation drift gates](../../docs/decisions/0003-docs-drift-gates.md).
Each gate script's docstring (for timeouts and the nightly job, the workflow file) is the
authoritative description of what it enforces; the bullets below hold only what an
author must do. Read the docstring before changing a gate.

- CI (`.github/workflows/ci.yml`) runs on every PR, all on the native arm64 runner. Its jobs, by id (docs-drift check 18 holds this list to the job ids in `ci.yml`, both ways): `build-test` (vendors OFF), `build-vendors` (vendors ON, instrumented for gcov; per-component and combined line coverage in the job summary), `sanitizers` (ASan+UBSan), `sanitizers-tsan` (ThreadSanitizer over the `[stress]` connect/disconnect/operate concurrency suite, all vendors ON, plus `[stress-guard]` for the harness's own self-tests), `format` (clang-format on changed lines), `clang-tidy` (changed lines), `package-deb` (builds the `.deb` in `debian:trixie`, installs it and runs `scripts/check_bundled_gphoto.sh`), `unicode` (`.github/scripts/check-unicode.py`), `docs-drift` (`scripts/check_docs_drift.py`), `connect-error-hook` (`scripts/check_connect_error_hook.py`), `serial-port-ownership` (`scripts/check_serial_port_ownership.py`), `layering` (`scripts/check_layering.py`), `stress-registration` (`scripts/check_stress_registration.py`), `contract-sweep` (`scripts/check_contract_sweep.py`), `conformu-reports` (`scripts/check_conformu_reports.py`), `shellcheck`, `javascript` (`node --check` and `node --test`), `cppcheck` (changed files), and `zizmor` (workflow audit).
- **PR body gates** (`.github/workflows/pr-body.yml`, not required checks): `falsified-by`
  (`scripts/check_falsified_by.py`, pre-flight gate 2b2) needs a `## Falsified by` line for
  every test case the PR adds or renames (see the [testing rules](testing.instructions.md#new-test-cases-name-their-falsifying-mutation));
  `pr-template` (`scripts/check_pr_template.py`, gate 2b3) needs every `## ` section of
  `.github/PULL_REQUEST_TEMPLATE.md` with real content and the Checklist items kept. Both read the
  body only through `env: PR_BODY` (never in a `run:` line) and run locally with `PR_BODY_FILE`.
  A merge-down PR skips `falsified-by`; `scripts/merge_down.py` decides it (`docs/beta-channel.md`).
  `decision-records` (`scripts/check_adr_scope.py`) needs the `## Decision records` section to name
  every record in `docs/decisions/` the PR changes.
  `gh pr create --body` and agents skip GitHub's template fill-in, so write every section.
- **Decision-record authorization** (`.github/workflows/decision-records.yml`, the `adr-authorization`
  commit status): a PR that changes `docs/decisions/` needs a collaborator's `ADR authorized <head sha>`
  comment; `scripts/check_adr_authorization.py` runs from the default branch, never from the PR. The
  review bot gets the in-scope rules from `scripts/check_adr_scope.py --list`, run from the base branch.
  See `docs/decisions/README.md` ("Changing a record").
- **Layering** (`scripts/check_layering.py`, gate 2f): the vendor-include baselines in the script
  only go down; the PR that deletes includes lowers them. It also enforces CMake rules L1-L3
  (see [CMake and Vendor Integration](packaging-release.instructions.md#cmake-and-vendor-integration)).
- **Contract sweep** (`AlpacaCore/tests/contract_sweep.h`, `test_contract_sweep.cpp`,
  `scripts/check_contract_sweep.py`, gate 2g): every (vendor, device type) pair the server builds
  needs one registry entry; a driver that differs states its expectation in that entry with a
  source (protocol document, hardware run, or assumption), never a copy of the assertions. A
  catalog-built pair cannot be allow-listed. A connectable fake goes in `kFakeConnectableRoster`
  (tier 2); a row whose fake cannot hold a connect open says why in `connecting_unobservable`.
- **Docs drift** (`scripts/check_docs_drift.py`, the `docs-drift` job): its docstring lists all
  checks. The ones authors trip: bump the `zizmor` version and sha256 in `ci.yml` and
  `ci_preflight.sh` together; keep the cppcheck `--suppress` lists identical; give each new CI job id
  a description in parentheses in the roster bullet above; a new `SUPPORTED-DRIVERS.md` vendor
  heading needs its README brand item, the recounted brand word and a
  `SUPPORTED_HEADING_TO_README_BRAND` entry in the same PR; give a new decision record the
  sections and Rules lines `docs/decisions/README.md` describes, and add an Adoption row when a
  driver starts naming a module's symbol (`TaskClock`, `AsyncOperation`, `StateSnapshot`,
  `kLinkStalenessBound`, `UtcOffset`, a `Locked*` decorator) (checks 22 and 23); keep `SUPPORTED-DRIVERS.md`'s
  `## Updated` date no older than the README badge date; give each workflow cache a key prefix no
  other key starts with; keep `AGENTS.md` under its 15,000-byte budget; keep every error-code
  value written in `AGENTS.md`, the ascom-contract and testing instructions or `driver-build.md`
  equal to `alpaca_errors.h` (check 17); never state a count of the
  blocking-`get_connected()` drivers, name them; and keep every `std::regex` in `router.cpp` `static`.
- **Nightly optimized run** (`.github/workflows/nightly-build.yml`, job `test-optimized`, issue
  #1313): `build-vendors` is `-O0 --coverage`, so the all-vendors suite also runs `Release`
  (`-DNDEBUG`) nightly, off the PR path. The nightly `build` job also installs the `.deb` it
  builds and runs `scripts/check_bundled_gphoto.sh` before it publishes, as `package-deb` does on
  every PR, so a nightly `.deb` that fails the check is never published.
- **Every job in every workflow has a `timeout-minutes` bound** (issue #363), sized from healthy
  green runtimes with wide headroom. A `[stress]` regression hangs rather than fails, and GitHub's
  6-hour default wasted six hours of runner time first. Give every new job a bound; when a job
  legitimately outgrows it, raise the number rather than trim the work. `claude-review` (75 min:
  up to 45 waiting for CI, then the review) is outlasted by the 80-minute verdict poll in
  `/pr-checker` and `/submit-pr`; a poll timeout is a re-poll.
- **Pre-flight** (`scripts/ci_preflight.sh`, the `/submit-pr` Step 4 gate) runs every gate that
  needs no build, in about a minute, and installs missing tools; it also runs on macOS's bash 3.2.
  The build and test gates (both `run_all_tests.sh` passes, clang-tidy, ASan+UBSan) run
  only with `PREFLIGHT_FULL=1` and are reported `[SKIP]` otherwise: CI runs them on every PR, and
  the review waits for them. In a full run the ASan+UBSan pass is on by default (issue #588;
  `RUN_SANITIZERS=0` skips it); `RUN_TSAN=1` adds the stress suite in either mode. **When a
  sanitizer cannot observe a case, rather than having found a defect in it, gate the case out of
  sanitized builds; never loosen the assertion** (example: the `ALPACACORE_TESTS_SANITIZED` guard
  in `AlpacaCore/tests/test_fake_pty_write.cpp`). A genuine finding is a bug: fix it.
- **Pi 4 ASan host limitation:** the Raspberry Pi kernel's 39-bit AArch64 address space cannot host
  ASan; run the sanitizer pass on a 48-bit-VA host, and never read the abort as a pass
  ([failure record](../../docs/failures/0010-rpi4-asan-39-bit-va.md)).
- **cppcheck is pinned to 2.17.x, built from source in CI**: the runner's apt 2.13 classifies some
  checks differently, and pre-flight uses the dev box's 2.17, so the two disagreed.
- **Web UI JavaScript** (`javascript` job): `node --check` plus `node --test
  AlpacaHTTP/tests/web/*.test.js` (issue #385). **Pure helpers go in `AlpacaHTTP/web/format.js`,
  not `app.js`**: it touches no DOM, `index.html` loads it first, and its
  `typeof module !== 'undefined'` export block lets the tests `require` it with no browser stub. A
  helper that needs `document` stays in `app.js`, untested; do not grow a DOM stub.
- **Stress suite** (`sanitizers-tsan`, issue #101): TSan over the `[stress]` connect/disconnect/operate
  storms in `AlpacaCore/tests/concurrency_stress.h`. `scripts/check_stress_registration.py`'s docstring
  holds the rules and the masked-pair list. What an author does:
  - **Add a `[stress]` case when you add or substantially change a driver**: one factory and one
    operate callback (`test_touptek_concurrency_stress.cpp` is the shape) in a
    `<something>_concurrency_stress.cpp` file listed inside the vendor's `if(TARGET alpacacore_<vendor>)`
    block in `AlpacaCore/tests/CMakeLists.txt`. Keep example macros in comments; the scan strips
    comments, not string literals.
  - **Wrap every call in `alpacacore::test::StressCallGuard`** (issue #322), never a local
    `try/catch` or `call()` helper: the harness wraps the whole callback in one try, so one throw
    would skip every later call. End the case with all three lines, `INFO(guard.report());`,
    `CHECK(guard.unexpected_count() == 0)` and `CHECK(guard.total_calls() > 0)` (#379, #334).
  - **The guard's constructor argument replaces the default `{NotConnected}` set**: pass
    `{NotConnected, PropertyNotImplemented}`, not `{PropertyNotImplemented}`. A registration that
    runs connected (`fake_mount_server.h`, `fake_skywatcher_mount.h`, a full-seam fake) widens it
    with what a live driver legitimately throws (`InvalidValue`, `MethodNotImplemented`,
    `InvalidWhileParked`); the three not-implemented errors share one code.
  - **SDK callbacks**: the TSan suppressions mute any report with a vendor-blob frame, so a driver
    callback that runs on an SDK thread is visible only through a fake-seam storm; register one in
    the same change that adds the callback.
  - A driver without a registration, or masked by another driver of the same pair, is covered only
    by review against the [concurrency checklist](driver-lifecycle.instructions.md#driver-concurrency--lifecycle-read-before-writing-a-driver).
- **SDK decorators take their mutex on every forward** (#394): `check_docs_drift.py` compares
  `QHYSDK`, `LockedQHYSDK` and the sweep list in `test_qhy_fake_sdk.cpp`, and fails on a forward
  that skips `locked()`, except `cancel_exposure()`, which takes its own `cancel_mutex_` so it can
  interrupt a download blocked on the handle (#339). Do not move it back under `mutex_`. Model a
  new decorator the same way.
