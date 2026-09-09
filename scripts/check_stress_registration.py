#!/usr/bin/env python3
"""Fail when a vendor driver has no `[stress]` concurrency-suite coverage.

AGENTS.md requires every new or substantially-changed driver to register a
`[stress]` TEST_CASE with the ThreadSanitizer concurrency suite
(`AlpacaCore/tests/concurrency_stress.h`, wired into the `sanitizers-tsan` CI
job). Registration is manual today, and nothing failed when it was skipped.

Run from the repo root:  python3 scripts/check_stress_registration.py

Coverage is tracked per (vendor, Alpaca device type) pair, not per vendor.
A vendor-level check (does `test_<vendor>_concurrency_stress.cpp` exist at
all?) would pass ZWO or ToupTek in full the moment any one of their drivers
is registered -- today that would hide ZWO's un-stressed rotator, focuser
and two ASIAIR switch drivers, and ToupTek's un-stressed focuser, behind a
green check. Keying on device type as well catches those.

The device type for a driver file is read from its own
`get_device_type() const override { return DeviceType::X; }` rather than
guessed from the filename: `gemini_flatpanel_driver.cpp` actually returns
DeviceType::CoverCalibrator, so a filename-based guess would be wrong.

New driver, no stress test yet? Either add the `[stress]` TEST_CASE (see
`test_touptek_concurrency_stress.cpp` for the shape: one factory + one
operate callback), or add the (vendor, device type) pair to ALLOWLIST below
with a comment. The allow-list is meant to shrink, not grow -- an entry left
in place after coverage is added will itself fail the check (see below), so
there is nothing to remember to clean up by hand.
"""

import re
import subprocess
import sys

VENDORS_PREFIX = "AlpacaCore/src/vendors/"
STRESS_TEST_GLOB_PREFIX = "AlpacaCore/tests/test_"
STRESS_TEST_GLOB_SUFFIX = "_concurrency_stress.cpp"

DEVICE_TYPE_RE = re.compile(r"DeviceType::([A-Za-z]+)")
TEST_CASE_TAGS_RE = re.compile(r'TEST_CASE\s*\([^,]*,\s*"((?:\[[^\]]+\])+)"')
TAG_RE = re.compile(r"\[([^\]]+)\]")

# (vendor, device type) pairs with no [stress] TEST_CASE yet. Seeded from the
# gap found when this check was introduced (2026-09) so the check starts
# green; each line is a driver this repo already knows is uncovered.
#
# Remove an entry the same PR that adds its [stress] coverage -- a
# still-covered entry left behind is itself a failure (see main()), so
# nothing here can silently go stale.
ALLOWLIST = {
    ("astroasis", "focuser"),
    ("gemini", "covercalibrator"),
    ("gemini", "focuser"),
    ("gemini", "switch"),
    ("ioptron", "filterwheel"),
    ("ioptron", "focuser"),
    ("ioptron", "switch"),
    ("playerone", "switch"),
    ("qhy", "camera"),
    ("qhy", "filterwheel"),
    ("touptek", "focuser"),
    ("wandererastro", "covercalibrator"),
    ("wandererastro", "filterwheel"),
    ("wandererastro", "rotator"),
    ("wandererastro", "switch"),
    ("weewx", "observingconditions"),
    ("zwo", "focuser"),
    ("zwo", "rotator"),
    ("zwo", "switch"),
}


def tracked_files(pattern):
    out = subprocess.run(
        ["git", "ls-files", pattern],
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
    ).stdout
    return [p for p in out.splitlines() if p]


def driver_device_type(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    m = DEVICE_TYPE_RE.search(text)
    if not m:
        return None
    return m.group(1).lower()


def find_drivers():
    """{(vendor, device_type): [driver file paths]}"""
    drivers = {}
    for path in tracked_files(VENDORS_PREFIX + "*_driver.cpp"):
        # AlpacaCore/src/vendors/<vendor>/<name>_driver.cpp
        parts = path[len(VENDORS_PREFIX):].split("/")
        if len(parts) != 2:
            continue
        vendor = parts[0]
        device_type = driver_device_type(path)
        if device_type is None:
            print("WARNING: could not determine device type for %s "
                  "(no DeviceType::X found) -- treating as uncovered" % path)
            device_type = "unknown"
        drivers.setdefault((vendor, device_type), []).append(path)
    return drivers


def find_registered_pairs(known_vendors, known_device_types):
    """{(vendor, device_type)} covered by at least one [stress] TEST_CASE.

    Matches tags against the vendor/device-type vocabulary the driver scan
    itself found, rather than assuming a fixed tag order -- a TEST_CASE is
    tagged [vendor][device_type][stress] plus sometimes more (e.g.
    [round-4]), and this only needs to find the two tags that are actually a
    known vendor and a known device type.
    """
    registered = set()
    for path in tracked_files(STRESS_TEST_GLOB_PREFIX + "*" + STRESS_TEST_GLOB_SUFFIX):
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        for m in TEST_CASE_TAGS_RE.finditer(text):
            tags = [t.lower() for t in TAG_RE.findall(m.group(1))]
            if "stress" not in tags:
                continue
            vendor_tags = [t for t in tags if t in known_vendors]
            dtype_tags = [t for t in tags if t in known_device_types]
            for vendor in vendor_tags:
                for dtype in dtype_tags:
                    registered.add((vendor, dtype))
    return registered


def main():
    drivers = find_drivers()
    known_vendors = {v for v, _ in drivers}
    known_device_types = {d for _, d in drivers}
    registered = find_registered_pairs(known_vendors, known_device_types)

    failures = []

    for (vendor, dtype), paths in sorted(drivers.items()):
        covered = (vendor, dtype) in registered
        allowed = (vendor, dtype) in ALLOWLIST
        if not covered and not allowed:
            failures.append(
                "MISSING: %s/%s has no [stress] TEST_CASE and is not in "
                "ALLOWLIST (%s): %s"
                % (vendor, dtype, __file__, ", ".join(paths))
            )
        if covered and allowed:
            failures.append(
                "STALE ALLOWLIST ENTRY: %s/%s now has [stress] coverage -- "
                "remove ('%s', '%s') from ALLOWLIST in %s"
                % (vendor, dtype, vendor, dtype, __file__)
            )

    driver_pairs = set(drivers)
    for vendor, dtype in sorted(ALLOWLIST - driver_pairs):
        failures.append(
            "STALE ALLOWLIST ENTRY: %s/%s has no matching driver anymore -- "
            "remove ('%s', '%s') from ALLOWLIST in %s"
            % (vendor, dtype, vendor, dtype, __file__)
        )

    if failures:
        print("Stress-test registration check failed:\n")
        for f in failures:
            print("  " + f)
        print("\n%d finding(s)." % len(failures))
        return 1

    print("Stress-test registration OK -- %d driver/device-type pairs checked, "
          "%d allow-listed as not-yet-covered." % (len(drivers), len(ALLOWLIST)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
