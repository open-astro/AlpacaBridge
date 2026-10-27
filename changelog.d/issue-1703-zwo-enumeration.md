### Fixed
- **ZWO EFW, EAF and CAA: unreadable devices no longer hide healthy devices** (AlpacaCore, issue #1703): enumeration skips failed SDK ID or property reads with a warning, preserves the original SDK slot of each readable device, and binds configured indexes by SDK slot rather than filtered list position; for entries with unreadable properties, see Breaking changes.

### Breaking changes
- **ZWO accessory index binding no longer uses incomplete enumeration entries** (AlpacaCore, issue #1703): an index-based Connect now refuses a slot whose SDK property read fails during enumeration, even if a later read after opening the device could succeed. Explicit-ID binding is unchanged. **After upgrading:** restore readable device properties and retry Connect; do not change the index to the position of another readable device in the filtered list.
