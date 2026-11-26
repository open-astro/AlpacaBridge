### Changed

- **iOptron async slews supersede instead of joining**, iOptron telescope driver, decision 0006 rule 3. A newer `SlewToCoordinatesAsync` or `SlewToAltAzAsync` no longer cancels and joins the older slew body before it starts its own: its motion claim supersedes that body, which checks the claim under the operation lock before it sends anything, so an older GOTO either reached the mount before the newer claim or never does (open-astro#768). `AbortSlew`, disconnect and the destructor still cancel and join the slot. Covered by `AlpacaCore/tests/test_ioptron_async_slew_failure.cpp`.
