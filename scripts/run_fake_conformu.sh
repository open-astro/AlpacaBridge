#!/usr/bin/env bash
# Hardware-free ConformU runs (workflow conformu-fake.yml: PRs that touch its
# paths, nightly on main, on demand, issue #2078). For every `run` row of the
# run list it starts the test-only fake_conformu_server on that row's fake,
# runs ConformU against it and writes a report that says it is fake evidence.
#
#   scripts/run_fake_conformu.sh <fake_conformu_server> <conformu-binary> <output-dir> [run-list]
#
# The run list defaults to scripts/conformu_fake_runs.tsv (columns vendor,
# deviceType, fakeHeader, status, slice, reportPath; check 29 of
# scripts/check_docs_drift.py keeps it in step with kFakeConnectableRoster).
# Each row writes <output-dir>/<reportPath below AlpacaCore/conformu/>, whose
# first line is "FAKE REPORT: ..." (see scripts/check_conformu_reports.py),
# with its server.log, register.json and the raw ConformU log beside it.
# Every row runs even after one fails; the script exits non-zero when any
# ConformU run exits non-zero or its log does not show a clean pass. Run it on
# arm64 only: the pinned ConformU is the linux-arm64 build.
set -euo pipefail

usage="usage: run_fake_conformu.sh <fake_conformu_server> <conformu> <output-dir> [run-list]"
server_bin="$(realpath "${1:?$usage}")"
conformu_bin="${2:?$usage}"
out_dir="${3:?$usage}"
run_list="${4:-scripts/conformu_fake_runs.tsv}"
port=11111
base="http://127.0.0.1:${port}"

mkdir -p "$out_dir"
work=""
server_pid=""
stop_server() {
  if [ -n "$server_pid" ]; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    server_pid=""
  fi
  if [ -n "$work" ]; then
    rm -rf "$work"
    work=""
  fi
}
trap stop_server EXIT

# One run-list row: start the server on its fake, register device 0, run ConformU, write and check the report.
run_row() {
  local vendor="$1" device_type="$2" fake_header="$3" report_path="$4"
  local row_dir description raw report status=0
  row_dir="$out_dir/$(dirname "${report_path#AlpacaCore/conformu/}")"
  report="$out_dir/${report_path#AlpacaCore/conformu/}"
  mkdir -p "$row_dir"
  if ! description="$("$server_bin" --describe "$vendor" "$device_type" "$fake_header")"; then
    echo "fake_conformu_server has no host for $vendor $device_type ($fake_header)" >&2
    return 1
  fi

  # Own working directory: the router reads and writes config/registered_devices.json.
  work="$(mktemp -d)"
  (cd "$work" && exec "$server_bin" "$port" "$vendor" "$device_type" "$fake_header") >"$row_dir/server.log" 2>&1 &
  server_pid=$!

  local ready=0
  for _ in $(seq 1 50); do
    if curl -fsS "$base/management/apiversions" >/dev/null 2>&1; then
      ready=1
      break
    fi
    sleep 0.2
  done
  if [ "$ready" -ne 1 ]; then
    echo "fake server for $vendor $device_type ($fake_header) did not start" >&2
    stop_server
    return 1
  fi

  # run_row runs under `if !`, where set -e does not apply: a failed registration stops the row here, not after a
  # ConformU run against a device that does not exist.
  if ! curl -fsS -X POST -H 'Content-Type: application/json' \
      -d "{\"vendor\":\"${vendor}-fake\",\"deviceType\":\"${device_type}\",\"deviceNumber\":0}" \
      "$base/management/v1/configuredevice" | tee "$row_dir/register.json"; then
    echo "registering ${vendor}-fake $device_type 0 failed" >&2
    stop_server
    return 1
  fi
  echo

  raw="$row_dir/conformu-raw.txt"
  "$conformu_bin" conformance "$base/api/v1/${device_type}/0" -n "$raw" >"$row_dir/conformu-stdout.log" 2>&1 || status=$?
  stop_server

  {
    echo "FAKE REPORT: ConformU against ${description}, not hardware. Not hardware validation."
    [ -f "$raw" ] && cat "$raw"
  } >"$report"

  if [ "$status" -ne 0 ]; then
    echo "ConformU exited with status $status for $vendor $device_type ($fake_header)" >&2
    return "$status"
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
}

rows=0
failed=0
# The list is read on fd 3, so nothing a row runs can read it from stdin.
while IFS=$'\t' read -r vendor device_type fake_header status _slice report_path <&3; do
  [ "$vendor" = "vendor" ] && continue  # the header
  case "$vendor" in "" | "#"*) continue ;; esac  # blank and comment lines, as check 29 skips them
  [ "$status" = "run" ] || continue
  rows=$((rows + 1))
  echo "== ConformU: $vendor $device_type over $fake_header -> $report_path"
  if ! run_row "$vendor" "$device_type" "$fake_header" "$report_path"; then
    failed=$((failed + 1))
  fi
done 3<"$run_list"

if [ "$rows" -eq 0 ]; then
  echo "no run rows in $run_list" >&2
  exit 1
fi
if [ "$failed" -ne 0 ]; then
  echo "$failed of $rows fake ConformU runs failed" >&2
  exit 1
fi
echo "$rows fake ConformU run(s) passed"
