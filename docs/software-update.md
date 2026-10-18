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

The card also has an **Update channel** selector, Stable or Beta. It decides
which apt component the host follows and which index the check reads, and the
two always agree (see [Update channel](#update-channel)).

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
  NetworkManager rule and requires the `polkitd` package. The same rule also
  answers `YES` for the `start` verb on exactly two more units,
  `alpacabridge-channel@stable.service` and `alpacabridge-channel@beta.service`
  (string equality, no prefix or pattern match), described under
  [Update channel](#update-channel).

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

## Update channel

The effective channel of a host is whether the apt source
`/etc/apt/sources.list.d/alpacabridge-beta.sources` exists: present is Beta,
absent is Stable. The daemon keeps no copy of it. Choosing a channel in the card
starts one of two instances of the root-owned template unit
`alpacabridge-channel@.service` (`Type=oneshot`, no `[Install]`, kept out of
the `dh_installsystemd` snippets like the update unit). Its `ExecStart` is
`/usr/libexec/alpacabridge/set-update-channel %i`. The polkit rule names the
two instances by exact string, the daemon picks the unit from a two-entry table
and never builds a unit name from request text, and the helper itself refuses
any argument but `stable` or `beta` before it touches apt.

- **Beta** writes the deb822 source (`Types: deb`, `URIs:
  https://apt.openastro.net`, `Suites: trixie`, `Components: beta`,
  `Architectures: arm64`, `Signed-By` the OpenAstro keyring) through a
  `mktemp` file in `/etc/apt/sources.list.d`, `chmod 0644`, `mv -f`.
- **Stable** removes that file.
- Both then run `apt-get update`. If it exits non-zero the helper puts the
  source back as it was before the run (removes it if this run created it,
  restores it if Stable removed it), ends the transcript with `=== RESULT:
  failure ... apt-get update exit N` and exits with N, so the card reports the
  failure (a host that installed the `.deb` from a release lacks the keyring
  and cannot verify the beta source). `/etc/apt/sources.list.d/openastro.list` is
  never edited, and `apt purge alpacabridge` removes the beta source.

The card shows a finished switch only on the page that started or joined it, and a failed one carries the helper's result line (with the apt exit code) as its detail. The transcript is `/var/log/alpacabridge-update/channel.log` (same root-owned
`LogsDirectory=` as the update transcript, `kChannelLogPath`), ending in the
same `=== RESULT: success|failure` markers. A finished switch is judged by that
transcript, never by a template instance's own `failed` state, which stays
until something resets it. While a switch runs the card cannot start an
install, and while an install runs it cannot switch.

The check follows the channel: `https://apt.openastro.net/dists/trixie/main/binary-arm64/Packages`
for Stable, `.../beta/binary-arm64/Packages` for Beta, from one mapping in
`software_update.cpp` that also names the apt component, so the two cannot
disagree (`test_software_update` pins this). If `update_packages_url` is unset
or equal to the stock stable index, the check follows the channel. Any other
non-empty value is an explicit override: the check uses it as is, the status
reports `ChannelOverridden: true`, the selector is disabled with "set by
update_packages_url", and the channel route refuses. A result fetched for the
other channel is not reported after a switch; check again.

Choosing Stable on a beta build does not downgrade: apt never does, so the
card shows this under the selector while Stable is selected on a beta build
and repeats it in the confirmation: "You stay on X.Y.0~betaN until the stable
release overtakes it".

## API

These live under `/management/v1/update/` (the unversioned
`/management/update/` alias works too) and use the standard Alpaca envelope.
`check` and `install` accept `PUT` as well as `POST`, like the other
state-changing management endpoints; the examples use `POST`.
The state-changing endpoints (`check`, `install`, `channel`) carry the cross-origin guard documented in
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
    "Channel": "stable",
    "ChannelOverridden": false,
    "ChannelSwitch": { "State": "idle", "Detail": "", "Log": "" },
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

- `POST /management/v1/update/channel` (or `PUT`) takes `{"Channel":"stable"}`
  or `{"Channel":"beta"}`, starts the matching helper instance and answers the
  status payload with `ChannelSwitch.State` = `running`. Any other body answers
  HTTP 400 with `ErrorNumber` 0x401 (`InvalidValue`) and does not echo the
  text. It refuses with 0x40B (`InvalidOperation`) while the update installer
  or a switch is running or `update_packages_url` is an override, with 0x400
  (`NotImplemented`) when the helper unit is not installed, and with 0x500 when
  systemd refuses the start. `install` refuses with 0x40B while a switch runs.
  It carries the same cross-origin guard as `check` and `install`.

When the manager is not wired at all (a test router), every sub-endpoint
answers 0x400 "Software update not configured".

## Configuration

`update_packages_url` under `server:` in `default.yaml` (environment override
`ALPACAHTTP_UPDATE_PACKAGES_URL`) is the index the check reads. The default is
the arm64 Trixie `main` index at apt.openastro.net, which means "follow the
update channel": leave the key out (it is a commented example in the shipped
file) unless you mirror the repository. Any other non-empty value is an
override that pins the check and disables the channel selector. Only the check
uses it; the install always goes through the host's own apt sources. An empty
value disables the check.

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
  `journalctl -u alpacabridge-update` has the same text. A channel switch keeps
  its transcript in `/var/log/alpacabridge-update/channel.log`, and
  `journalctl -u alpacabridge-channel@beta` (or `@stable`) has the same text.
- On a host without `polkitd`, or where the rule file is missing, the install
  answers with a message naming both; the check still works, and
  `sudo apt install --only-upgrade alpacabridge` is the fallback.
- On the service side the helper runs with a 30 minute `TimeoutStartSec`, so
  a wedged mirror cannot leave the unit active forever; the UI then reports the
  run as failed with "no result recorded".
