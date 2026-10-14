### Fixed
- **SynScan re-GOTO after a far-miss repeat starts from the target** (issue #1120, SynScan): when a GOTO landing more than 600" off was repeated to the target, the aim carried over from earlier landing rounds stayed in place, so a later re-GOTO was aimed by the old residual as well as the repeat's. The repeat now resets the carried aim to the target, and the next re-GOTO is corrected by the repeat's residual alone (`test_synscan_goto_landing.cpp`, "the re-GOTO after a far-miss repeat is aimed by the repeat's residual alone (#1120)").

### Added (tests)
- **SynScan far-miss aim case** (issue #1120): the Alt-Az landing fake records the Dec of each GOTO; one case checks the fourth GOTO's RA and Dec aim after a far-miss repeat, with an out-of-tolerance RA miss on the first GOTO.
