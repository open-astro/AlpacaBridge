### Fixed

- **Update channel helper and purge**, packaging, issue #1756. A TERM, INT or HUP while `alpacabridge-set-update-channel` runs `apt-get update` now puts the previous `alpacabridge-beta.sources` back (or removes the one the run created), deletes its `.alpacabridge-beta.*` temp files, ends the transcript with `RESULT: failure ... killed by SIG<name>` and exits 128+n; before, a stopped unit left the new drop-in behind. Covered by the trap case in `AlpacaHTTP/tests/test_software_update.cpp`. `apt purge` now also removes `/var/log/AlpacaBridge` and `/var/log/alpacabridge-update`.
