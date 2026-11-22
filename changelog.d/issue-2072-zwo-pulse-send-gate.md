### Fixed
- **ZWO AM mount: AbortSlew, Park and FindHome no longer send their command ahead of a guide pulse step already being sent** (AlpacaCore, issue #2072): a pulse step checked that it was still wanted and then sent `:Mg` with no lock held, so an AbortSlew, Park or FindHome that cancelled the pulse in that gap could send `:Q`, `:hP` or `:hC` first, and the late `:Mg` then reached the mount after the command. Each pulse send is now counted in the same locked step as its check, and the pulse cancel that every motion command runs (`reset_position_offsets()`) waits for a counted send before the command goes out. The same wait covers Sync, Unpark, SetPark and SlewToTargetAsync.

### Added (tests)
- **ZWO pulse send gate case** (issue #2072): AbortSlew, Park and FindHome during a guide pulse step held between its check and its `:Mg` wait for it and send their command after it, within the 1 s budget.
