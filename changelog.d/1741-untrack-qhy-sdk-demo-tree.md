### Changed

- **QHY SDK tree no longer tracks the unbuilt static library, demo apps and PDF** (QHY, issue #1741): `libqhyccd.a`, `usr/local/testapp/` and `usr/local/doc/` are removed (about 16,000 lines) and ignored in `AlpacaCore/.gitignore`; CMake still links `libqhyccd.so` and `debian/rules` still ships `.so*`.
