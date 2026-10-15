### Fixed

- **iOptron: `Slewing` no longer reads true while a PulseGuide runs** (iOptron telescope, issue #843). A guider that sent a long pulse (for example a 2.7 s dither on a HAE29C) saw `Slewing` flip to true because the mount firmware reports "slewing" during the pulse, and it stopped. `Slewing` now stays false for the pulse plus the driver's one-second settle window; `IsPulseGuiding` still reports the pulse. GOTO, MoveAxis, park, home (even when a pulse arrives mid-motion), the GOTO settle trim and slews started from the handset (with no pulse running) still report `Slewing` true.
