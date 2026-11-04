### Added (tests)
- **SynScan: the possibly-moving mark after a failed cancel-path pulse stop is covered** (SynScan, issue #1725): the test case "a cancelled pulse whose stop fails logs the failure" gains a section in which a replacement PulseGuide on a dropped link is refused with `InvalidOperation`, which fails when the `manual_axis_slewing_` write added in #2024 is removed.
