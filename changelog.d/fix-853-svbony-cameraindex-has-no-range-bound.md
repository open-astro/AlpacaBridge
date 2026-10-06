### Fixed

- **SVBONY `cameraIndex` is refused at save time when negative or above INT_MAX** (AlpacaCore catalog; follow-up to PR #853). The field declares a `[0, INT_MAX]` range, so an oversized value no longer wraps in the factory's `static_cast<int>`; pinned by the `register_builtin_schemas describes the SVBONY camera` case in `AlpacaCore/tests/test_catalog_builtins.cpp`.
