# Failure records

One record per incident, with Summary, Root Cause, Prevention and Evidence.
Name the issue/PR and regression test when known; explicitly identify coverage
that is manual or still missing. These records explain the current rules in
`AGENTS.md` and the scoped instruction files rather than replacing those rules.

- [QHY filter-wheel position cache](0001-qhy-filterwheel-position-cache.md)
- [ToupTek thermal-poller join race](0002-touptek-thermal-poller-race.md)
- [PR #99 sibling-fix misses](0003-review-sibling-fixes.md)
- [NDEBUG-disabled HTTP assertions](0004-ndebug-disabled-http-assertions.md)
- [Failed server bind leaving a joinable thread](0005-server-failed-bind-thread.md)
- [ASan allocator incompatible with the Pi 4's 39-bit VA kernel](0010-rpi4-asan-39-bit-va.md)
- [Resolved July 2026 audit snapshot](2026-07-11-code-audit.md) — historical evidence; line numbers and observations describe that snapshot.

EQMOD-style support uses the `skywatcher` direct motor-controller driver:

- [Classic-board detection and baud failures](0006-eqmod-board-detection.md)
- [Pointing validation and southern direction failures](0007-eqmod-pointing-validation.md)
- [Cross-axis stop and tracking failures](0008-eqmod-cross-axis-motion.md)

GPhoto DSLR/mirrorless cameras:

- [Nikon bulb close failed after a full config-tree walk](0009-gphoto-nikon-bulb-full-config-walk.md)
