### Fixed
- **SynScan Telescope Driver** (AlpacaCore, issue #2412): AbortSlew is checked. While the handset still reports a GOTO or the position still moves, the stops are sent again, three rounds in all, and AbortSlew then fails and Slewing reports it. A GOTO the handset never starts within 2 s is stopped and fails instead of passing as a success. A 16-bit (V3) GOTO that comes to rest more than 10 arcmin from its target now fails, as a 24-bit one does.

### Breaking changes
- **SynScan Telescope Driver** (AlpacaCore, issue #2412): SyncToCoordinates and SyncToTarget are refused with InvalidValue when the target is more than 5 degrees from where the mount reports it points. AbortSlew during motion now takes about 0.75 s, because it waits until the mount reads still. **After upgrading:** slew near the object before you sync to it.
