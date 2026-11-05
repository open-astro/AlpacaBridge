# Firmware update checkers

AlpacaBridge does not check whether newer firmware exists for the devices it drives, and does not link to vendor firmware pages from the device card.

## Why this is out of scope

The check needs two things no supported vendor provides together: a machine-readable "latest version" source, and a device firmware string that compares to it. A 2026-11-05 research pass found:

- Celestron publishes a human firmware-history page. Its storefront exposes the page as JSON, but the versions sit inside an HTML table in the body: a scrape of an undocumented endpoint, not a feed. CGX and focus-motor strings would compare; the hand controller reports only major.minor.
- Astroasis lists firmware files on a download page with the version in the filename, and the driver reads no firmware string from the device, so there is nothing to compare.
- Gemini lists files on a download page; the focuser's reported number does not follow the page's numbering, the flat panels compare only by convention, and the power hub has no published firmware.
- OnStep mounts run vendor builds with no model code, so "newer upstream" is the wrong answer and could be unsafe to flash.
- EQMod is a Windows ASCOM driver AlpacaBridge never talks to.

A links-only fallback was considered and declined as well: a static table of vendor pages ages silently, and the appliance is often offline at a dark site. Users look up their vendor's page, as they do today. The AlpacaBridge software update check stays; it has a stable source of its own.

## Prior requests

- #1763: "Firmware/driver update checkers for Celestron, OnStep, Astroasis, Gemini and EQMod"
