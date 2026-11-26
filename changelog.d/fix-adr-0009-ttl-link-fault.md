### Fixed
- **Player One switch and PW filter wheel, ToupTek AAF focuser, AFW filter wheel and thermal switch** (AlpacaCore, decision 0009): while a device's link fault is latched, the management listing and web UI now show the fault text instead of an empty one, so a connected device that stopped answering shows as faulted, not healthy. The text clears with the next good reply.
