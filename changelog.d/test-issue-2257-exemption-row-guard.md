### Added (tests)

- **Contract sweep checks its probe exemption rows** (tests, issue #2257): every `kProbeExemptions` row of a compiled vendor must name a registry entry or tier-2 host id and a telescope probe, so a renamed probe or a mistyped id cannot leave a row that matches nothing.
