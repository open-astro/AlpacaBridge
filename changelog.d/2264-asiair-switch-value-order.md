### Fixed

- **ASIAIR Pro, ASIAIR Plus and StellaVita power-port switches: an out-of-range SetSwitchValue answers InvalidValue while disconnected** (AlpacaCore, issue #2264). The three drivers checked the connection before the value, so a non-finite or out-of-range value sent while disconnected answered `NotConnected`; the ASCOM contract (and the iMate PowerBox) checks the value first. The order is now id, finite value, range, connection, then writability on every GPIO power-port board. A valid value still answers `NotConnected` while disconnected, and `SetSwitch` is unchanged.
