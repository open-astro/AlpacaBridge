---
description: Build the AlpacaBridge .deb and deploy it to a test SBC over SSH — install, restart the service, and verify the running version — so /conformu tests the current build
argument-hint: [host] [user]
allowed-tools: Read, Bash, Grep, Glob
---

You are the test-deployment assistant for AlpacaBridge. Your job is to get the CURRENT working tree running on a test SBC: build the Debian package locally, copy it to the device over SSH, install it, restart the service, and verify the device is actually running the build you just made. This is the step between implementing a driver (`/driver-build`) and validating it on hardware (`/conformu`) — ConformU results are only meaningful when the SBC runs the build under test.

You DO NOT run ConformU — hand off to `/conformu` at the end. You DO NOT commit anything.

## How to use arguments

The user invoked this command with: $ARGUMENTS

- First argument (if present) is the target host (e.g. `astro.lan`), second is the SSH user.
- Anything not provided is asked for in Step 1.

## Step 1 — Gather session inputs

Ask **one at a time**, presenting arguments as defaults where given:

1. **Target host** — the SBC's hostname or IP. Default to `astro.lan` (the project's standard test rig) if no argument was given.
2. **SSH user** — the login user on the SBC. Must be able to `sudo` (installing a package and restarting a systemd service require it).
3. **Alpaca port** — default `6800`; used for the post-deploy verification.

### 1b. Validate the inputs (HARD STOP on failure)

The host, user, and port are interpolated into `ssh`/`scp` command lines, so validate them against a strict allowlist BEFORE using them in any command — even though they normally come from the developer's own keyboard, a value pasted from elsewhere must never be able to smuggle shell metacharacters or an ssh option (e.g. a leading `-`):

```bash
HOST='<host>'; USER='<user>'; PORT='<port>'
[[ "$HOST" =~ ^[A-Za-z0-9][A-Za-z0-9.-]*$ ]] || { echo "REJECT: invalid host"; exit 1; }
[[ "$USER" =~ ^[a-z_][a-z0-9_-]*$ ]]         || { echo "REJECT: invalid user"; exit 1; }
[[ "$PORT" =~ ^[0-9]{1,5}$ ]]                || { echo "REJECT: invalid port"; exit 1; }
echo "validated: ${USER}@${HOST}:${PORT}"
```

If any value is rejected, stop and re-ask — do not "fix it up" silently. Every later command double-quotes the interpolated values; the allowlist plus quoting means no input can be interpreted as shell syntax or an option flag.

Then confirm:

> "Ready to build the .deb from the current working tree (branch `<branch>`, VERSION `<version>`) and deploy to `<user>@<host>`, then restart `alpacabridge.service` and verify. Proceed?"

## Step 2 — Pre-flight checks (HARD STOP if any fail)

### 2a. Working tree state

```bash
git branch --show-current
git status --short
```

Report the branch and any uncommitted changes. Uncommitted changes are ALLOWED (deploying work-in-progress to test is the point of this skill), but say clearly what is being deployed — branch, HEAD short-hash, and whether the tree is dirty — so a ConformU result can always be traced back to what was actually tested.

### 2b. SSH reachability (never prompt for a password mid-flow)

```bash
ssh -o BatchMode=yes -o ConnectTimeout=5 -- "<user>@<host>" 'echo ok && sudo -n true && dpkg --print-architecture'
```

- If the connection fails: stop and tell the user to check the host/network or set up key auth (`ssh-copy-id <user>@<host>`).
- If `sudo -n true` fails (password required): stop and tell the user passwordless sudo is needed for unattended install/restart, or they can run the install commands from Step 4 manually.
- If the architecture is not `arm64`: **STOP** — never install on a non-arm64 device.

### 2c. Local build prerequisites

```bash
command -v dpkg-buildpackage || echo MISSING
```

If missing: `sudo apt install devscripts debhelper` and the build prerequisites from `docs/development.md`.

## Step 3 — Build the .deb

```bash
./scripts/build_deb.sh
```

This regenerates `debian/changelog` from `CHANGELOG.md` and builds `../alpacabridge_<VERSION>_arm64.deb` (one directory above the repo root). Stream the output; a full build takes several minutes.

On failure, show the error and stop — do not deploy a stale artifact. Verify the artifact exists and note its size and timestamp:

```bash
VERSION="$(tr -d '[:space:]' < VERSION)"
ls -l "../alpacabridge_${VERSION}_arm64.deb"
```

If an artifact with that name predates this build (timestamp check), rebuild rather than shipping it.

## Step 4 — Deploy and install

The restart is structurally gated on the install having actually succeeded: the package must report `install ok installed` before `systemctl restart` runs. A partial install followed by a restart would leave the service running a broken package — never restart on a failed install.

```bash
scp -- "../alpacabridge_${VERSION}_arm64.deb" "<user>@<host>:/tmp/"
ssh -- "<user>@<host>" "sudo dpkg -i /tmp/alpacabridge_${VERSION}_arm64.deb || sudo apt-get -y -f install"
ssh -- "<user>@<host>" "dpkg-query -W -f='\${Status} \${Version}\n' alpacabridge"
```

- `apt-get -f install` only runs if `dpkg -i` reported unmet dependencies.
- **Gate:** the `dpkg-query` line must print `install ok installed <VERSION>` (the version just built). If the status is anything else (`half-configured`, `unpacked`, an older version), **STOP** — do NOT restart the service. Show the dpkg/apt output, leave the currently-running (old but working) service untouched, and tell the user the install failed.

Only after the gate passes:

```bash
ssh -- "<user>@<host>" "sudo systemctl restart alpacabridge && rm -f /tmp/alpacabridge_${VERSION}_arm64.deb"
```

- If the service fails to restart, pull the journal and show it:
  ```bash
  ssh -- "<user>@<host>" "sudo journalctl -u alpacabridge -n 50 --no-pager"
  ```
  **STOP** on a crash loop — a broken deploy must be fixed before ConformU, and the previous working version is gone, so tell the user plainly that the device is down until this is resolved.

## Step 5 — Verify the device runs the new build

Poll until the management API answers (the service takes a few seconds to start; give it up to 30):

```bash
for i in $(seq 1 30); do
  curl -sS --max-time 2 "http://<host>:<port>/management/v1/description" && break
  sleep 1
done
```

Then confirm the reported server version matches the deployed VERSION:

```bash
curl -sS --max-time 5 "http://<host>:<port>/management/v1/description" | jq .
```

- **Version matches** → deploy verified.
- **Version differs or unreachable** → the device is NOT running the new build; diagnose (service status, journal) before letting anyone run ConformU.

## Step 6 — Hand off to /conformu

Report the result:

> "Deployed `alpacabridge <version>` (branch `<branch>`, `<short-hash>`<, dirty tree>) to `<host>` — service restarted and version verified. Reconnect/verify the device under test in the web UI (`http://<host>:<port>/`), then run `/conformu <vendor> <model>` and point it at `http://<host>:<port>`."

Do NOT start ConformU yourself. Do NOT commit or push anything from this skill.

## Workflow notes

- Deploying a dirty tree is fine for iteration, but a ConformU log destined for `AlpacaCore/conformu/` (via `/conformu`) should come from the exact commit that will be pushed — re-run this skill from the clean, committed branch before the final validation run.
- arm64 only: the built package and the target must both be arm64. On a non-arm64 dev machine (see "Development machines" in `docs/development.md`), build inside the Debian 13 arm64 VM.
