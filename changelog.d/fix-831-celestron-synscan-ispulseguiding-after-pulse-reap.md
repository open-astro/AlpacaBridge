### Fixed
- **Celestron and SynScan `IsPulseGuiding` clears when another operation cancels a pulse** (issue #831): Celestron `FindHome`, `Park`, `SlewToCoordinates` and `SlewToCoordinatesAsync`, and SynScan `SlewToCoordinates`, stopped the in-flight guide pulse but left the flag true until the pulse's original end time. They now clear it with the pulse, as `AbortSlew` does.
