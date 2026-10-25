#!/usr/bin/env bash
# Hardware-free ConformU run (CI job conformu-fake): starts the test-only
# fake_skywatcher_server (a Sky-Watcher telescope over FakeSkyWatcherMount),
# runs ConformU against it and writes a report that says it is fake evidence.
#
#   scripts/run_fake_conformu.sh <fake_skywatcher_server> <conformu-binary> <output-dir>
#
# Writes <output-dir>/Fake-Linux-arm64.txt (first line "FAKE REPORT: ...", see
# scripts/check_conformu_reports.py) and server.log. Exits non-zero when ConformU
# exits non-zero or its log does not show a clean pass. Run it on arm64 only: the
# pinned ConformU is the linux-arm64 build.
set -euo pipefail

server_bin="$(realpath "${1:?usage: run_fake_conformu.sh <fake_skywatcher_server> <conformu> <output-dir>}")"
conformu_bin="${2:?missing conformu binary}"
out_dir="${3:?missing output dir}"
port=11111
base="http://127.0.0.1:${port}"

mkdir -p "$out_dir"
work="$(mktemp -d)"
server_pid=""
cleanup() {
  if [ -n "$server_pid" ]; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$work"
}
trap cleanup EXIT

# Own working directory: the router reads and writes config/registered_devices.json.
(cd "$work" && exec "$server_bin" "$port") >"$out_dir/server.log" 2>&1 &
server_pid=$!

ready=0
for _ in $(seq 1 50); do
  if curl -fsS "$base/management/apiversions" >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 0.2
done
if [ "$ready" -ne 1 ]; then
  echo "fake server did not start" >&2
  exit 1
fi

curl -fsS -X POST -H 'Content-Type: application/json' \
  -d '{"vendor":"skywatcher-fake","deviceType":"telescope","deviceNumber":0}' \
  "$base/management/v1/configuredevice" | tee "$out_dir/register.json"
echo

raw="$out_dir/conformu-raw.txt"
status=0
"$conformu_bin" conformance "$base/api/v1/telescope/0" -n "$raw" >"$out_dir/conformu-stdout.log" 2>&1 || status=$?

report="$out_dir/Fake-Linux-arm64.txt"
{
  echo "FAKE REPORT: ConformU against FakeSkyWatcherMount (simulated mount over loopback UDP), not hardware. Not hardware validation."
  [ -f "$raw" ] && cat "$raw"
} >"$report"

if [ "$status" -ne 0 ]; then
  echo "ConformU exited with status $status" >&2
  exit "$status"
fi
python3 - "$report" <<'PY'
import sys
sys.path.insert(0, "scripts")
import check_conformu_reports as c
failures = c.check_text(sys.argv[1])
for f in failures:
    print(f, file=sys.stderr)
sys.exit(1 if failures else 0)
PY
