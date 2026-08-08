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
# (For one-click time sync without SSH, use the "Sync Time" button in the
# AlpacaBridge web portal. Installing an NTP client — systemd-timesyncd/chrony —
# is a separate manual step; see docs/troubleshooting.md.)
#
# Usage:
#   scripts/sync-clock.sh [user@host] [--rtc]
#
# Examples:
#   scripts/sync-clock.sh astro@192.168.168.1
#   scripts/sync-clock.sh astro@192.168.168.1 --rtc     # also persist to RTC
#   scripts/sync-clock.sh --rtc                          # default host + RTC persist
#   ASTRO_PASS=... scripts/sync-clock.sh astro@astro.lan
#
# Requirements on the target: passwordless sudo (or SSH_ASKPASS), `date`,
# and — for --rtc — `hwclock` (package util-linux-extra on Debian).

set -euo pipefail

# Arguments may come in any order: the first non-flag argument is the host,
# --rtc may appear anywhere, anything else is an error (so a typo'd flag can't
# be silently pushed to as a hostname).
TARGET=""
PERSIST_RTC=false
for a in "$@"; do
    case "$a" in
        --rtc) PERSIST_RTC=true ;;
        -*) echo "ERROR: unknown flag '$a'" >&2; exit 2 ;;
        *)
            if [ -n "$TARGET" ]; then
                echo "ERROR: multiple hosts given ('$TARGET' and '$a')" >&2; exit 2
            fi
            TARGET="$a"
            ;;
    esac
done
TARGET="${TARGET:-astro@192.168.168.1}"

NOW_UTC="$(date -u +'%Y-%m-%d %H:%M:%S')"
echo "Pushing time ($NOW_UTC UTC) to ${TARGET} ..."
# accept-new: auto-accept an unknown host key but reject a changed one, so a
# MITM that swaps the host key is detected (full auto-accept would be silent).
# date -u: the string is UTC, so it must also be *parsed* as UTC — without -u
# the target interprets it in its local timezone and the clock ends up off by
# the TZ offset on any non-UTC SBC.
ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes "$TARGET" \
    "sudo -n date -u -s '$NOW_UTC'" || {
        echo "Direct passwordless sudo failed; retrying with password prompt."
        ssh -o StrictHostKeyChecking=accept-new "$TARGET" "sudo date -u -s '$NOW_UTC'"
    }

if $PERSIST_RTC; then
    echo "Persisting to hardware RTC..."
    ssh -o StrictHostKeyChecking=accept-new "$TARGET" "sudo hwclock -w" || \
        echo "WARN: hwclock unavailable on target (install util-linux-extra)."
fi

echo "Done. Verify: ssh ${TARGET} date"
