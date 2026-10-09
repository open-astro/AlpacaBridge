# Software update from the web UI

The Server Info tab has a **Software Update** card. **Check for Updates** asks
the OpenAstro APT repository for the newest `alpacabridge` release and compares
it with the running build. When a newer release exists, the card shows that
release's plain-language notes (the same `docs/releases/X.Y.Z.md` text that
becomes the GitHub Release body, fetched at the release tag) with a link to
the release page, so the operator decides with the changes in front of them.
**Install Update** then runs the same upgrade
`sudo apt install --only-upgrade alpacabridge` would, shows the transcript
while it runs, and reloads the page on the new version once the service has
restarted.

Nothing updates on its own. The check runs only when the button is pressed,
and the install only after the confirmation dialog.

## Privilege model

The service runs as the `alpacabridge` user with `NoNewPrivileges=true`, so it
cannot run `apt` and cannot use `sudo`. The feature is split along that line:

- **The check needs no privilege.** The daemon fetches the repository's
  `Packages` index over HTTPS (libcurl, in-process), reads the `alpacabridge`
  stanza and compares its `Version` with the running build using dpkg's own
  ordering (`~` before release, epochs, numeric runs). Nothing is written.
- **The install runs in a root-owned systemd unit, not in the daemon.** The
  daemon asks systemd over the system D-Bus (sd-bus, in-process, no
  subprocess) to start `alpacabridge-update.service`, a `Type=oneshot` unit
  whose `ExecStart` is fixed to `/usr/libexec/alpacabridge/software-update`.
  The helper runs `apt-get update` and `apt-get install --only-upgrade
  alpacabridge` against the host's own configured apt sources, so the
  repository signature is verified exactly as it is for a manual upgrade. The
  daemon passes no arguments and cannot change what the unit runs.
- **polkit scopes the D-Bus call.** The package ships
  `/usr/share/polkit-1/rules.d/50-alpacabridge-update.rules`, which answers
  `YES` for the `alpacabridge` user on
  `org.freedesktop.systemd1.manage-units` only when the unit is
  `alpacabridge-update.service` and the verb is `start`. Every other unit and
  verb falls through to the default (admin authentication), which a service
  user cannot satisfy. This is the same shape as the WiFi card's
  NetworkManager rule and requires the `polkitd` package.

The helper unit is outside `alpacabridge.service`'s control group, so it
survives the restart that the new package's `postinst` performs part-way
through the upgrade. `debian/rules` passes only `alpacabridge.service` to
`dh_installsystemd`, so the package never generates enable, start or restart
snippets for the helper: an upgrade must not restart the updater from inside
the updater.

The helper writes its transcript to `/var/log/alpacabridge-update/update.log`
(truncated at the start of each run, world-readable) and ends it with one of
two markers, `=== RESULT: success` or `=== RESULT: failure`. It also records
`apt-cache policy alpacabridge` before installing, because apt installs
whatever the host's own sources and pinning select, which can differ from
the version the check saw on the repository index; when they differ the UI
reports that the run finished without changing the installed version and the
transcript shows why. The daemon reads
the tail of that file for the UI and uses the marker as the durable record of
the last run, because a finished oneshot unit is garbage-collected by systemd
and reads as never-run afterwards. A start job that systemd still holds for
the unit (queued behind `network-online.target`, before the helper has
truncated the previous transcript) is reported as `running`, so the previous
run's marker is never mistaken for the new run's result.

The transcript directory is the helper unit's own `LogsDirectory=`, created
`root:root 0755`, and deliberately not the daemon's `/var/log/AlpacaBridge`:
that directory belongs to the `alpacabridge` user, and a root process that
truncates and `chmod`s a path there would follow a symlink the service user
planted, turning the update button into a way to clobber any root-owned file.
No check inside the script can close that race, only a directory the service
user cannot write to. The path is fixed on both sides (`kUpdateLogPath` in
`software_update.h`, `LOG=` in the script) and `test_software_update` fails if
they disagree or if the daemon's unit claims the same directory.

## API

All three live under `/management/v1/update/` (the unversioned
`/management/update/` alias works too) and use the standard Alpaca envelope.
`check` and `install` accept `PUT` as well as `POST`, like the other
state-changing management endpoints; the examples use `POST`.
The state-changing pair carries the cross-origin guard documented in
[wifi-api.md](wifi-api.md): a browser request whose `Origin` does not match
`Host` gets HTTP 403 with `ErrorMessage` "Cross-origin software update requests
are not allowed". `GET status` is exempt, like every other GET.

- `GET /management/v1/update/status` returns the last check result and the
  installer state without any network fetch (it reads the unit's state over
  the local system bus and the transcript file):

  ```json
  {
    "InstalledVersion": "4.1.0",
    "CheckEnabled": true,
    "LatestVersion": "4.2.0",
    "UpdateAvailable": true,
    "CheckedAt": 1790535600,
    "CheckError": null,
    "PackagesUrl": "https://apt.openastro.net/dists/trixie/main/binary-arm64/Packages",
    "ReleaseNotes": "# AlpacaBridge 4.2.0\n\nOne paragraph...\n\n## Read this first\n\n- ...",
    "ReleaseNotesUrl": "https://raw.githubusercontent.com/open-astro/AlpacaBridge/v4.2.0/docs/releases/4.2.0.md",
    "ReleaseUrl": "https://github.com/open-astro/AlpacaBridge/releases/tag/v4.2.0",
    "Installer": {
      "State": "running",
      "Detail": "ActiveState=activating SubState=start",
      "Log": "=== AlpacaBridge software update started 2026-09-28T02:20:00Z\n..."
    }
  }
  ```

  `CheckEnabled` is `false` when `update_packages_url` is empty; the card
  then disables Check for Updates and says so, and `POST check` answers
  0x400 (`NotImplemented`) without fetching anything.
  `LatestVersion` and `CheckedAt` are `null` until a check has run.
  `CheckError` carries the last failed check's message and is `null`
  otherwise. `Installer.State` is one of `idle`, `running`, `succeeded`,
  `failed` and `unavailable` (the helper unit is not installed on this host,
  which is what a source build reports). `Installer.Log` is the last 8 KiB of
  the transcript. `ReleaseNotes` is the newer version's notes as Markdown
  (`null` when no update is available, or when the notes could not be
  fetched, which never fails the check); `ReleaseNotesUrl` and `ReleaseUrl`
  are the expanded templates, `null` without an update. The web UI renders the
  notes with a fixed Markdown subset (headings, bullets, paragraphs, bold,
  code, `http(s)` links, fenced code), escaping everything first, so notes
  fetched from the network can never inject markup; the title line is dropped
  because the card names the version.

- `POST /management/v1/update/check` fetches the index and answers the same
  payload. It is single-flight: a check arriving while another is fetching
  waits for it and returns its result rather than fetching again, so repeated
  presses from several tabs pin one worker thread, not one per press (a
  failure in the shared run is then reported as `CheckError` in the returned
  payload rather than as an error envelope). A failed fetch or an index with no `alpacabridge` stanza answers
  `ErrorNumber` 0x500 (`DriverException`) with the reason, and the reason is
  also kept in `CheckError`.

- `POST /management/v1/update/install` starts the helper and answers the same
  payload with `Installer.State` = `running`. It refuses with 0x40B
  (`InvalidOperation`) when no successful check has found a newer version or
  when a run is already in progress, with 0x400 (`NotImplemented`) when the
  helper unit is not installed, and with 0x500 when systemd refuses the start
  (the message names polkit and the rule file).

When the manager is not wired at all (a test router), every sub-endpoint
answers 0x400 "Software update not configured".

## Configuration

`update_packages_url` under `server:` in `default.yaml` (environment override
`ALPACAHTTP_UPDATE_PACKAGES_URL`) is the index the check reads. The default is
the arm64 Trixie index at apt.openastro.net. Only the check uses it; the
install always goes through the host's own apt sources. An empty value
disables the check.

`update_release_notes_url` and `update_release_url` (environment
`ALPACAHTTP_UPDATE_RELEASE_NOTES_URL`, `ALPACAHTTP_UPDATE_RELEASE_URL`) are
templates in which `{version}` is replaced by the newer version: where its
plain-language notes are read from (default: the `docs/releases/X.Y.Z.md`
file at the `vX.Y.Z` tag on GitHub) and the release page the card links to.
A beta that the index lists as `X.Y.Z~betaN` is written in its tag spelling
`X.Y.Z-beta.N`, which is how its tag and notes file are named (see
[beta-channel.md](beta-channel.md)); a custom template keyed on the Debian
spelling must allow for that. Empty disables each; the notes are
informational and never gate the install.

## Operator notes

- The helper passes `--force-confold`, so a changed packaged `default.yaml`
  conffile keeps the installed copy on upgrade, exactly as a manual
  `apt install` with that option would; the new version's copy lands next to
  it as `default.yaml.dpkg-dist`.
- A dev build reporting a version newer than the repository reads as up to
  date. A dev build with the same version as the repository also reads as up
  to date; the check compares numbers, not commits.
- The upgrade disconnects every Alpaca client for a few seconds when the
  service restarts. Do not start it mid-exposure or mid-slew. The
  confirmation dialog says so.
- A failed run keeps its transcript in `/var/log/alpacabridge-update/update.log`, and
  `journalctl -u alpacabridge-update` has the same text.
- On a host without `polkitd`, or where the rule file is missing, the install
  answers with a message naming both; the check still works, and
  `sudo apt install --only-upgrade alpacabridge` is the fallback.
- On the service side the helper runs with a 30 minute `TimeoutStartSec`, so
  a wedged mirror cannot leave the unit active forever; the UI then reports the
  run as failed with "no result recorded".
