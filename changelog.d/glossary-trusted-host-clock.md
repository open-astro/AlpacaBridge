### Changed (docs)

- **Glossary: synchronized vs trusted host clock** (docs, issue #2478): `CONTEXT.md` defines a synchronized host clock (the kernel reports NTP discipline) and a trusted host clock (synchronized, or set or confirmed since the server started by a client `UTCDate` write, Sync Time or an observed clock step), and says a clock loaded from a hardware RTC at boot is neither. The terms are the ones the mount clock issues #2478, #2479 and #2480 use. No code change.
