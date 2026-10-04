### Fixed

- **Layering gate: a missing or unreadable `AlpacaHTTP/CMakeLists.txt` now fails L2** (`scripts/check_layering.py`). The L2 cutoff stayed unset and the directory-scoped definition check was skipped silently; the gate now reports the rule as vacuous, as it already does for `AlpacaCore/CMakeLists.txt`.
