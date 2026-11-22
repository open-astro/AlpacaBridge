### Changed (docs)

- **Astroasis Oasis focuser: the GetConfig layout for the 2.1.0.1 ASCOM driver is documented**, `AlpacaCore/external/oasisastro/README.md`, issue #2082. The 2.1.0.1 SDK still sends `0x30` (18-byte reply) or `0x3a` (40-byte reply) by firmware protocol version; the new remote-hand-controller fields (`RHC_STEP1..3`, `RHC_STEP_FACTOR`) sit inside the existing 40-byte reply, so the driver's 18- and 40-byte checks reject no shipping firmware. No driver change.
