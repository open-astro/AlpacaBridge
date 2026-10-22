# Connect without a site, refuse what needs one

Status: proposed

## Context

The Sky-Watcher direct motor-controller driver drives a mount that stores no site of its own. Its pointing math depends on the hemisphere: the sky frame (#432), the RA tracking direction (#250), and the Dec rate and pulse-guide sign (#253) all branch on `hemisphere_south_locked()`, which is `site_latitude_ < 0.0`. An unconfigured device would run on latitude 0.0 and put a southern rig on northern math. Since #274 the driver therefore refuses `Connected=true` until `SiteLatitude` and `SiteLongitude` are known, from the device config or a client write.

ASCOM does not ask for that. The [time and place FAQ](https://ascom-standards.org/newdocs/mount-time-place-faq.html) only requires that reading a site value that was never set throws `InvalidOperationException`. Today the site getters also return 0.0 when nothing was set.

`configuredevice` also refuses a Sky-Watcher config without both coordinates (#274, `skywatcher_catalog.cpp`), so a device created today always has a site in its config. The connect refusal is reached by a config saved before #274, which the catalog registers with a WARN so it stays editable.

Decided 2026-10-21 on issue #1470 (item 5): allow Connect without a site, and refuse the operations and site getters that need one.

## Decision

- **Connect does not need a site.** `Connected=true` and Platform 7 `Connect` succeed without one and send the mount no command whose meaning depends on the hemisphere.
- **The site getters throw until set.** `SiteLatitude` and `SiteLongitude` throw `InvalidOperation` until they are set by the device config or a client write. `SiteElevation` throws `InvalidOperation` until elevation itself is set; it does not wait for the coordinates.
- **Members that depend on the hemisphere or longitude throw until both are set.** They throw `InvalidOperation`, naming the missing setting. The set covers every member whose answer or motion goes through `hemisphere_south_locked()` or uses the longitude: `RightAscension`, `Declination`, `SiderealTime`, the slew and sync members, `Tracking=true`, `MoveAxis`, `PulseGuide`, and `RightAscensionRate` and `DeclinationRate`. Members that do not depend on the site (`Connected`, capabilities, `AtPark`, `Slewing`, `AbortSlew`, `Tracking` reads) keep answering.
- **`DeviceState` omits what throws**, as the base class already does, so a connected mount with no site reports a short list.
- **Writing the second coordinate makes the mount usable** without a reconnect.
- **`configuredevice` keeps requiring both coordinates.** This record changes the driver only; the web UI still asks for a site when a device is created.

## Rules

The rules a change is checked against. Each names where it applies and what checks it; the reasoning is in the Decision above.

1. A Sky-Watcher Connect succeeds without a site and sends no hemisphere-dependent command. Scope: `AlpacaCore/src/vendors/skywatcher/skywatcher_telescope_driver.cpp`. Checked by: review, until issue #1474 adds the test cases.
2. Every member whose result or motion depends on `hemisphere_south_locked()` or on the longitude throws `InvalidOperation` until both site coordinates are set; the site getters throw `InvalidOperation` until set. Scope: `AlpacaCore/src/vendors/skywatcher/skywatcher_telescope_driver.cpp`. Checked by: review, until issue #1474 adds the test cases.
3. A new mount driver whose controller stores no site follows rules 1 and 2 instead of refusing Connect. Scope: `AlpacaCore/src/vendors/*/*_telescope_driver.cpp`. Checked by: review.

## Alternatives rejected

- **Keep the #274 refusal at Connect.** It protects pointing, but it blocks every client that connects before it writes the site, and ASCOM does not require it.
- **Connect with a default site.** A default is the 0.0/0.0 case #274 removed: a southern rig would point on northern math with nothing reported.
- **Gate only the slews.** `RightAscension` and `Declination` go through the same sky frame, so they would report wrong positions, and `PulseGuide` and `MoveAxis` would move the wrong way.

## Consequences

- A device whose saved config has no site (one saved before #274) now connects, and a client can supply the site after connecting.
- ConformU still needs a site before a telescope run. Without one it connects and then fails every pointing test with `InvalidOperation`. Set the site in the device config first.
- The hemisphere-dependent member list in rule 2 must grow with the driver. A new member that reads `hemisphere_south_locked()` and skips the gate points wrongly, so review checks every call site of that function.
- The driver on `main` still refuses Connect until issue #1474 lands. That change needs a ConformU run on the rig.

## Links

- Issues [#274](https://github.com/open-astro/AlpacaBridge/issues/274), [#1470](https://github.com/open-astro/AlpacaBridge/issues/1470) (item 5), [#1474](https://github.com/open-astro/AlpacaBridge/issues/1474); hemisphere paths [#250](https://github.com/open-astro/AlpacaBridge/issues/250), [#253](https://github.com/open-astro/AlpacaBridge/issues/253), [#432](https://github.com/open-astro/AlpacaBridge/issues/432).
- [Sky-Watcher instructions](../../.github/instructions/skywatcher.instructions.md).
