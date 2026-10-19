---
applyTo: "debian/**,changelog.d/**,CHANGELOG.md,VERSION,build_and_run.sh,install_alpaca_service.sh,**/CMakeLists.txt,AlpacaCore/external/**,docs/releases/**"
---

## Build, packaging and release

Shared rules for every vendor, moved verbatim from `AGENTS.md`, which keeps the
core rules and routes here. A rule that applies to a second vendor belongs in
the matching shared file, not in a vendor file.

## CMake and Vendor Integration

- Guard each vendor behind explicit build options.
- When adding a vendor in `AlpacaCore/CMakeLists.txt`, always update:
  1. `option(ALPACACORE_ENABLE_<VENDOR> ...)`
  2. `ALPACACORE_ENABLE_ALL_VENDORS` logic (only if implemented)
  3. conditional `add_subdirectory(src/vendors/<vendor>)` + `target_link_libraries(alpacacore_builtins PRIVATE alpacacore_<vendor>)`. A vendor whose descriptor factory `register_builtin_factories()` registers (`AlpacaCore/src/catalog/builtin_catalog.cpp`) also needs `target_compile_definitions(alpacacore_builtins PRIVATE ALPACACORE_ENABLE_<VENDOR>)` next to that link, or the registration compiles empty with no build error (see the Astroasis block in `AlpacaCore/CMakeLists.txt`)
  4. install rules for vendor target
- The dependency runs one way: `alpacacore` <- `alpacacore_<vendor>` <- `alpacacore_builtins` <- `alpacahttp` (#710). The vendor's own `CMakeLists.txt` links `alpacacore` PRIVATE; `alpacacore` never links a vendor library, takes no `ALPACACORE_ENABLE_<VENDOR>` definition and compiles no `src/vendors/` source (`scripts/check_layering.py` rules L1-L3).
- If vendor libs are discovered by pkg-config, prefer imported targets (example: `PkgConfig::LIBUSB`) so dependent test binaries get correct link paths.
- When adding a new vendor SDK under `AlpacaCore/external/<vendor>/`, add an allowlist entry to `AlpacaCore/.gitignore` so the SDK binaries (`.a`, `.so`, `.dll`, firmware files, etc.) are not blocked by the global compiled-file ignore rules. Follow the existing pattern: `!external/<VENDOR>/**`.

## Vendor SDK Shared Library Packaging

**Every new camera vendor MUST ship its `.so` in the `.deb`, regardless of whether AlpacaBridge itself statically links the library.** Camera `.so` files are also consumed by companion projects (e.g. SmartGuider at `/home/dev/Documents/GitHub/SmartGuider/` — uses `libASICamera2.so` via `zwoasi`, `libqhyccd.so`, etc. for autoguiding) which dynamically `dlopen` them from the system library path. If the `.so` is missing or not registered with `ldconfig`, guiding fails at runtime with no warning from the AlpacaBridge build or test suite. The fact that the AlpacaBridge server binary links fine is NOT evidence that packaging is correct.

Non-camera vendors (focusers, mounts, switches, rotators) also ship their `.so` if the SDK provides one, for consistency — but the camera rule is non-negotiable.

### Mandatory Checklist — New Vendor SDK with `.so`

Do **all** of the following when adding a new vendor SDK. Skipping any step will either silently break companion projects (step 1), break CI / clean clones (step 2), or break runtime loading on installed systems (steps 3–5).

1. **Store** static (`.a`) and shared (`.so`) libraries under `AlpacaCore/external/<VENDOR>/` in the arm64 subdirectory the upstream SDK uses (commonly `lib/linux/armv8/`, `lib/linux/arm64/`, or `lib/armv8/`). Document the exact path in the vendor-specific notes section below. Do not commit x86_64/x64 SDK binaries — AlpacaBridge is arm64-only and they would only bloat the repo.
2. **Allowlist in `.gitignore`**: add `!external/<VENDOR>/**` to `AlpacaCore/.gitignore` **before** committing the SDK files. The global `*.so` ignore rule will silently drop the library from the commit otherwise. Verify with `git check-ignore -v <path-to-.so>` — the output must show the `!external/<VENDOR>/**` rule winning.
3. **`debian/rules`** — copy `.so*` to `$(STAGING)/usr/lib/alpacabridge/` in `override_dh_auto_install`, alongside existing QHY/ZWO/SVBONY entries. Add a `<VENDOR>_LIB_DIR` variable at the top of the file pointing at the arm64 SDK path.
4. **`build_and_run.sh`** — copy `.so*` to `/usr/local/lib/`, run `ldconfig`. Add inside the udev rules block alongside existing QHY/ZWO/SVBONY install logic.
5. **`install_alpaca_service.sh`** — same as `build_and_run.sh`, inside the `install_udev_rules()` function. Keep the two scripts in sync — they must install the same set of vendor libraries.

### Bundled library built from a vendored tarball (libgphoto2)

libgphoto2 is not a prebuilt `.so` under `external/`: `scripts/build_bundled_gphoto.sh` builds the vendored tarball into `/usr/lib/alpacabridge/libgphoto2`, which is found through the executables' `DT_RPATH` and never registered with ldconfig ([decision 0012](../../docs/decisions/0012-bundled-libgphoto2.md)). A new file shipped under that prefix, or a change to how it is found, goes through `debian/rules` and `scripts/check_bundled_gphoto.sh`, and the version and sha256 pins live only in that script (CI and `debian/rules` run it; the acceptance script and the installers read them through `--print-pins`).

### Dynamic Linker Registration

The `.deb` ships `/etc/ld.so.conf.d/alpacabridge.conf` which adds `/usr/lib/alpacabridge` to the system library search path. The `postinst` script runs `ldconfig` so libraries are discoverable immediately after install. This is what makes companion projects (SmartGuider's `zwoasi`, `ctypes.CDLL('libtoupcam.so')`, etc.) able to find vendor libraries without setting `LD_LIBRARY_PATH`.

### SDK Version Bumps

When a vendor releases a new SDK version, update the `.so` files in `external/` and bump symlink targets (e.g. `libASICamera2.so.1.41` → `libASICamera2.so.1.42`). If the vendor's SDK path is versioned (e.g. ToupTek's `toupcamsdk.20260128/`), update the path reference in `debian/rules`, `build_and_run.sh`, `install_alpaca_service.sh`, AND `AlpacaCore/src/vendors/<vendor>/CMakeLists.txt` in the same commit. Then run `python3 scripts/dependency_snapshot.py --pin` and check the entry's `version` in `scripts/dependency_sdk_versions.json`: `--check` (the `docs-drift` CI job) fails while the vendored files differ from their `sha256` pin, and the release SBOM refuses any file under `/usr/lib/alpacabridge/` or `/usr/lib/firmware/` in the `.deb` that matches no git-tracked vendored file (`/usr/sbin/fxload`, the udev rules and the libgphoto2 bundle under `/usr/lib/alpacabridge/libgphoto2/`, which is built from a vendored tarball, are outside that check; see [Bundled library built from a vendored tarball](#bundled-library-built-from-a-vendored-tarball-libgphoto2)).

### Driver version (DriverVersion)

Every driver's `get_driver_version()` returns `alpacacore::kVersion` (from `<alpacacore/version.h>`), which is the single workspace `VERSION` file injected at build time via the `ALPACACORE_VERSION` compile definition. AlpacaCore's top-level `CMakeLists.txt` sets this with directory-scoped `add_compile_definitions(...)` right after `project()`, so it reaches the core lib, every per-vendor sub-library, and the tests. **Do not hardcode a version string** in a driver. To bump the reported version for the whole project, edit the `VERSION` file only. Unit tests assert `get_driver_version() == alpacacore::kVersion` rather than a literal, so they don't need updating on a version bump.

### Verification

After wiring a new camera vendor, verify the `.so` is reachable as SmartGuider would see it:

```bash
# After ./build_and_run.sh or dpkg -i alpacabridge_*.deb:
ldconfig -p | grep <libname>                    # must list the .so
python3 -c "import ctypes; ctypes.CDLL('<libname>.so')"  # must not raise OSError
```

Failing this check means guiding will fail at runtime, no matter how green the AlpacaBridge test suite is.

## Debian Packaging

- Package files live in `debian/` (control, rules, copyright, service file, maintainer scripts).
- **`debian/changelog` is generated, never edited.** It is untracked/gitignored and derived from the root `CHANGELOG.md` by `scripts/changelog_to_deb.py` (same design as the OpenAstro Guider). Build the package with `scripts/build_deb.sh`, which generates the changelog (version from the `VERSION` file, validated with `dpkg-parsechangelog`) and then runs `dpkg-buildpackage -us -uc -b`. Do not run `dpkg-buildpackage` directly on a fresh checkout — it will fail on the missing `debian/changelog`. In-progress work is the `changelog.d/` fragments (see the changelog rule below) plus any legacy `## [X.Y.Z] - UNRELEASED` section still in `CHANGELOG.md`; the generator synthesizes an `UNRELEASED` stanza from them when `VERSION` has not been released yet, and warns when `VERSION` (a beta by its base version: `5.0.0~beta1` matches `[5.0.0]`) and a legacy section label disagree. A beta `VERSION` (`X.Y.Z~betaN`) gets a `beta` stanza instead, dated noon UTC on the README badge date (`build_deb.sh` passes it as `--date`, which the generator requires for a beta), so a beta `.deb` is reproducible rather than stamped with the build time.
- The `.deb` installs to:
  - `/usr/bin/alpacabridge` — server binary.
  - `/usr/lib/alpacabridge/` — vendor shared libraries (e.g. `libqhyccd.so`, `libASICamera2.so`).
  - `/usr/share/alpacabridge/web/` — web UI static files.
  - `/lib/firmware/qhy/` — QHY camera firmware files.
  - `/lib/udev/rules.d/` — udev rules for USB device permissions.
  - `/usr/sbin/fxload` — QHY firmware loader.
  - `/etc/alpacabridge/` — default config (`registered_devices.json`).
- When adding a new vendor with shared libraries, update `debian/rules` `override_dh_auto_install` to copy them into `$(STAGING)/usr/lib/alpacabridge/`.
- To cut a release or a beta, run `/bump-release` (`.claude/commands/bump-release.md`; beta and stable modes, [beta channel](../../docs/beta-channel.md)). A stable release bumps the `VERSION` file, assembles the `changelog.d/` fragments into the dated `## [X.Y.Z]` CHANGELOG.md section (`scripts/changelog_fragments.py --release`), updates the README badge and device count, writes the plain-language notes in `docs/releases/<version>.md` that become the GitHub Release body, and tags the merge. A beta sets `VERSION` `X.Y.0~betaN` and the badge, writes its notes file under `docs/releases/` from `--preview`, consumes no fragments and writes no dated section — **do NOT edit `debian/changelog`; it is generated** (see the packaging note above).

### Beta channel and stable branches

Development stays on `main`; a release is not cut from `main` alone. `/bump-release` cuts
`stable/X.Y` from `main`, tags betas `vX.Y.Z-beta.N` on it (`VERSION` `X.Y.Z~betaN`, the Debian
spelling; `release.yml` maps the tag through `scripts/release_tag.py`), promotes the same branch
to `vX.Y.Z`, and every beta tag and the promotion are merged back into `main` with a merge commit
(never squash). A merge down keeps the receiving branch's `VERSION` and README badge, so `main` never
carries a beta `VERSION`; only a stable release newer than `main`'s comes across. The stable branch
takes bug and stability fixes only, landed on the branch (PR with `--base stable/X.Y`), never
cherry-picked; a fix is admitted only when it repairs a defect or regression introduced after the
previous stable release (the last stable tag before `X.Y.0`), which therefore did not have it: a
defect that release also had, a feature, a refactor, a new driver, a test, tooling or docs change
targets `main`, whatever its severity or milestone, and so does a fix that adds a new SDK seam or
fake, a client-visible capability-flag change, an exposure or slew state-machine rewrite, or a
driver change that needs a fresh ConformU run, unless the maintainer admits that fix by name on the
promotion milestone; `stable/X.Y` retires when the next release is promoted, not when its branch
is cut.
Cutting, tagging, promoting and the merge-down PR stay with the upstream maintainer and
diegopereiran. Branch model, promotion criteria, opt-in steps and the three version spellings:
[docs/beta-channel.md](../../docs/beta-channel.md).

### Version bump policy (SemVer)

The release derives the NEXT version from the `changelog.d/` fragments
(`python3 scripts/changelog_fragments.py --bump`); a PR never sets one. `VERSION` and the README
badge stay at the last release until `/bump-release` Step 2 writes them (check 4 ties `VERSION`
to the dated badge line, so bumping it early would date a release that has not happened). The
bump size rule: **new driver/feature (an unqualified `### Added` fragment entry) = minor bump;
fix- or docs-only = patch; breaking change (dropped platform, config-schema break, a
`### Breaking changes` entry) = major.** The bump is the highest severity across all fragments
since the last release, counting the categories of a legacy `## [X.Y.Z] - UNRELEASED` section still in
`CHANGELOG.md` as well, and its heading label is a floor. `/commit` and `/submit-pr` check the fragment's categories.

### Changelog fragments (one file per PR)

Every PR that changes code, tests, scripts, CI or docs adds `changelog.d/<branch-slug>.md` (the
branch name after its last `/`, lowercased) and **never edits `CHANGELOG.md`** (except to correct a misfiled or wrong entry, which the PR
description says): parallel PRs that all edited
its one UNRELEASED section conflicted on every merge to main. Format, categories and commands are
in [`changelog.d/README.md`](../../changelog.d/README.md), including its "Breaking or not" test, which
applies to fixes too: a saved config that no longer loads or a call that now fails is breaking even
when it is the fix. `scripts/changelog_fragments.py --check`
validates every fragment (the `docs-drift` CI job and pre-flight run it with `--self-test`), and
apart from such corrections only `/bump-release` writes `CHANGELOG.md`, through `--release`.

## Privileged operations from the daemon (polkit + root helper unit)

The service runs as the `alpacabridge` user with `NoNewPrivileges=true`, so
`sudo`, setuid helpers and `pkexec` cannot work from inside it, and the project
policy is no subprocesses in the daemon. Three mechanisms exist; pick by what
the operation needs, never add a fourth without a decision record:

- **An ambient capability** for an in-process syscall (`CAP_SYS_TIME` for
  `clock_settime`, `CAP_NET_ADMIN` for nl80211). Granted in
  `debian/alpacabridge.service`; keep `CapabilityBoundingSet` equal to the
  ambient set.
- **A polkit rule on a system D-Bus service** the daemon already talks to
  in-process over sd-bus (NetworkManager for the WiFi card,
  `debian/alpacabridge.polkit-rules`).
- **A root-owned oneshot systemd unit started over sd-bus, authorised by a
  polkit rule scoped to that one unit and the `start` verb** (the software
  update, `debian/alpacabridge-update.service` +
  `debian/alpacabridge-software-update` + `debian/alpacabridge-update.polkit-rules`,
  `docs/software-update.md`). Use this for anything that must run as root and
  has no D-Bus API (apt, dpkg, mount, mkfs). Rules that came out of building it:
  - **The daemon passes no arguments.** What runs is fixed in the unit's
    `ExecStart`; parameters travel through a file the daemon writes and the
    helper reads if they ever must, never through the D-Bus call. The one
    exception is the update channel switch (`alpacabridge-channel@.service`,
    `debian/alpacabridge-set-update-channel`, issue #1103): its template
    instance name is the argument, which is safe only because the polkit rule
    names each allowed instance by exact string (`stable`, `beta`), the daemon
    takes the unit from a two-entry table and never builds it from request
    text, and the helper rejects any other argument before it touches apt. A
    new template unit needs all three.
  - **Pass only `alpacabridge.service` to `dh_installsystemd`**
    (`override_dh_installsystemd` in `debian/rules`). It otherwise generates
    enable/start/restart snippets for EVERY non-template unit in the package,
    `[Install]` section or not, and the new postinst would restart the helper
    from inside the apt run the helper is executing.
  - **A root helper never writes into a directory the service user owns.**
    The transcript first went to the daemon's `LogsDirectory` (`/var/log/AlpacaBridge`,
    owned by `alpacabridge`): root truncating and `chmod`-ing a path there
    follows a symlink the service user planted, and no `[ -L ]` check in the
    script closes the race (PR #745 review). Give the helper unit its own
    `LogsDirectory=` (root:root 0755) and pin the path on both sides with a test
    (`kUpdateLogPath` vs `LOG=` in the script; the channel helper's
    `kChannelLogPath` and `kChannelDropInPath` are pinned the same way).
  - **The helper's transcript is the durable record**, not systemd's state: a
    finished oneshot unit is garbage-collected and `LoadUnit` then reports it
    as never run. The helper ends its log with a result marker the daemon
    parses; `installer_state()` trusts systemd for "running" (active, or a
    start job still queued), "failed" (`ActiveState=failed`) and
    "unavailable" (`LoadState` not loaded), and resolves an inactive unit
    from the transcript marker (`classify_installer_state()`).
  - **Read unit state with `LoadUnit`, not `GetUnit`**: `GetUnit` answers
    `NoSuchUnit` for a unit that is not loaded, which a never-started helper is.
  - **Start the unit in a separate cgroup from the daemon** (any unit is) when
    the operation restarts the daemon itself; a child process would die with it.
  - **Inject the privileged half behind a seam** (`SoftwareUpdateBackend`) so
    the policy (what may start, when, what the status means) is unit-tested
    over a scripted backend; the sd-bus and libcurl code is validated on the
    rig, where `polkitd` and the packaged rule exist.
