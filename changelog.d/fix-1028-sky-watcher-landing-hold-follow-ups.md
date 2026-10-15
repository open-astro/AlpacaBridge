### Changed

- **Sky-Watcher landing hold comments and docs**, Sky-Watcher driver, issue #1028. Comment and instruction-text corrections only, no behaviour change: the landing hold also runs when tracking was off, the goto-overhead comment and `.github/instructions/skywatcher.instructions.md` carry the measured 0.26 s refinement goto instead of the stale 3.1 s, and the synchronous-slew budget comment names the landing hold (`kMaxLandingHoldSeconds`).
