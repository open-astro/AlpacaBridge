### Fixed
- **SynScan: precise RightAscension and Azimuth read the right digits** (AlpacaCore, issue #785): the handset's precise `e`/`z` reply is `XXXXXX00,YYYYYY00#`, and the parser kept the last six digits of the first field instead of the first six, so RA and Azimuth were decoded from the low bits plus the padding and raced through 24 h on a still mount. Declination and Altitude were already correct.

### Added (tests)
- **SynScan precise position decode** (issue #785): a fake handset answering the verbatim EQM-35 Pro replies; RA, Dec, Azimuth and Altitude must decode to the upper 24 bits of each field.
