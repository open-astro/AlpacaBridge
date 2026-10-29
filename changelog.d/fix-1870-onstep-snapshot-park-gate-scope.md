### Changed (tests)

- **OnStep snapshot Park test**, OnStep telescope driver, issue #1870. The case "the poll restores tracking after a GOTO that ended without it, but not while a Park is pending" declares its Park gate before the rig, so the held Park thread no longer reads it after its scope ends (ASan stack-use-after-scope in the nightly sanitizer job) (`test_onstep_telescope_snapshot.cpp`). Test-only, no driver change.
