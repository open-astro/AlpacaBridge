### Changed
- **Upstream reference baselines advanced** (tooling), issues #2294, #2295, #2296 and #2297: the Bisque Script TheSky pages were regenerated with no API change, the OnStepX command reference changed only the `<0x06>`, `:GW#` and `:Gw#` rows (the local copy is refreshed), and INDI's OnStep driver change adds no protocol the AlpacaBridge driver depends on.

### Fixed
- **OnStep: a GOTO, park or home the mount stops at a limit fails instead of reporting done** (AlpacaCore), found reviewing issue #2296. OnStep reports a limit stop (horizon, meridian, under the pole, overhead, a limit switch, an axis limit or a motor fault) only through the general error digit of `:GU#`, which the driver did not read, so the motion read as a normal end and a slew then turned tracking back on past the limit. A slew whose end shows a stop code is now judged by where it ends: away from its target it fails through `Slewing` until the next motion command or a reconnect, and on OnStepX it gets no tracking restore, also when an AbortSlew is the first to see the stop. A GOTO that lands at its target and then tracks into a limit is still a landing, with a warning in the log.
- **OnStep: a park or home that ends without arriving, or never ends, fails** (AlpacaCore). A park that ends unparked, shows `F` or carries a stop code fails `AtPark`; a home that ends away from home fails `AtHome`; and either fails after 20 minutes. A park or GOTO a relink leaves running is still watched to its end.

### Added (tests)
- **OnStep limit stops, park and home watches** (issue #2296): cases for stopped and landed GOTOs on OnStepX and OnStep 4.x, Alt/Az slews, pier flips, AbortSlew, synchronous slews, relinks, stopped, failed, abandoned and slow parks and homes, and the `:GU#` parse.
