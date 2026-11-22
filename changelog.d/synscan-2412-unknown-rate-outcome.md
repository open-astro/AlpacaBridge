### Fixed
- **SynScan Telescope Driver** (AlpacaCore): a MoveAxis whose rate command fails on a live link is still bounded (issue #2412). The command may have reached the handset before it failed, so the driver keeps the axis reading as moving and stops it once the 60 s MoveAxis limit runs out, instead of leaving a possible move with no limit. Tested on the fake mount only, not on hardware.
