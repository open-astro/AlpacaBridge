### Fixed
- **iOptron: PulseGuide refused while the driver's own GOTO, Park or FindHome runs** (AlpacaCore, issue #2504): a guide pulse sent during a GOTO, a Park or a home search changed the motion the mount was running; it is now refused with InvalidOperation before anything is sent. PulseGuide asks the mount whether a Park or FindHome nobody polled has ended, as it already did for a GOTO (#2063), and a Park that landed meanwhile is refused with InvalidWhileParked.

### Added (tests)
- **iOptron PulseGuide refusal cases** (issues #2503, #2504): a pulse during a GOTO, Park or FindHome, a pulse after an unpolled one ended, and a pulse on the jogging axis and on the other axis.
