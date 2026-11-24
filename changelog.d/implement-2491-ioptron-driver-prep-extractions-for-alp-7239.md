### Changed

- **iOptron telescope driver internals**, iOptron driver, issue #2491. Behaviour-free preparation for serving the mount from `util::PolledState`: the status ingest (`ingest_status_locked`), one status read path (`status_view_locked`) and the Slewing settle trim, landing gate and clear (`advance_motion_locked`) are now their own functions, the synchronous slew's failed-read threshold has its own constant, and the unused `calibrate_dec_guide_locked` is removed. The existing iOptron tests pass unchanged.
