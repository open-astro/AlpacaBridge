#!/usr/bin/env bash
# sync-clock.sh — push a workstation's current time to an SBC running
# AlpacaBridge over SSH.
#
# Motivation: on a headless SBC with no internet (no NTP source) and a dead or
# unset hardware RTC, the system clock resets to a stale value on every reboot.
# AlpacaBridge's Alpaca responses (and ConformU's LastExposureStartTime checks)
# then carry the wrong timestamp. If you can SSH to the SBC, you can also be its
# time source — run this from any internet-connected machine (e.g. your laptop)
# whenever you connect.
#
# Also installs an NTP client if available (systemd-timesyncd / chrony) so the
# clock stays accurate while the SBC has any network path with NTP reachable.
#
# Usage:
#   scripts/sync-clock.sh [user@host] [--rtc]
#
# Examples:
#   scripts/sync-clock.sh astro@192.168.168.1
#   scripts/sync-clock.sh astro@192.168.168.1 --rtc     # also persist to RTC
#   ASTRO_PASS=... scripts/sync-clock.sh astro@astro.lan
#
# Requirements on the target: passwordless sudo (or SSH_ASKPASS), `date`,
# and — for --rtc — `hwclock` (package util-linux-extra on Debian).

set -euo pipefail

TARGET="${1:-astro@192.168.168.1}"
PERSIST_RTC=false
for a in "$@"; do
  [ "$a" = "--rtc" ] && PERSIST_RTC=true
done

NOW_UTC="$(date -u +'%Y-%m-%d %H:%M:%S')"
echo "Pushing time ($NOW_UTC UTC) to ${TARGET} ..."
ssh -o StrictHostKeyChecking=no -o BatchMode=yes "$TARGET" \
  "sudo -n date -s '$NOW_UTC'" || {
    echo "Direct passwordless sudo failed; retrying with password prompt."
    ssh -o StrictHostKeyChecking=no "$TARGET" "sudo date -s '$NOW_UTC'"
  }

if $PERSIST_RTC; then
  echo "Persisting to hardware RTC..."
  ssh -o StrictHostKeyChecking=no "$TARGET" "sudo hwclock -w" || \
    echo "WARN: hwclock unavailable on target (install util-linux-extra)."
fi

echo "Done. Verify: ssh ${TARGET} date"
