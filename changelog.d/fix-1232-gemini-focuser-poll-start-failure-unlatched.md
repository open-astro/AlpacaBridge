### Fixed

- Focusers that poll their state (Gemini, Celestron, QHY, Astroasis) now fail Connect when the state poll cannot start, instead of reporting Connected with a frozen connect-time frame (issue #1711). Test: `PolledState - start reports false when the poll thread cannot start`.
