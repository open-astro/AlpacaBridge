### Fixed
- **iOptron `AbortSlew` no longer lets a queued async GOTO reach the mount after it returns** (issue #768): `AbortSlew` now cancels and joins the async slew dispatch thread before it sends `:Q#`, and sends `:Q#` even when the cached status says the mount is not slewing.
