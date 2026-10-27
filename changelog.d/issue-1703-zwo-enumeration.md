### Fixed
- **ZWO EFW, EAF and CAA: unreadable devices no longer hide healthy devices** (AlpacaCore, issue #1703): enumeration skips failed SDK ID or property reads with a warning, preserves the original SDK slot of each readable device, and binds configured indexes by SDK slot rather than filtered list position.
