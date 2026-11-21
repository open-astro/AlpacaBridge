### Added (tests)

- **Telescope contract sweep probes the refusal order** (tests, issue #2257): PulseGuide, MoveAxis, SlewToCoordinates(Async) and the guide-rate setters are probed with bad arguments while disconnected, connected and parked (a new tier-2 case parks the mount and waits for AtPark). Probes a driver fails today are listed per driver in `kProbeExemptions` and checked to still fail, so the fix that turns one green must delete its line; members a driver does not implement keep today's answer under a permanent exemption.
