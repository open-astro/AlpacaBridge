### Fixed

- **Gemini focuser connect probe is bounded** (issue #2064): Connect no longer waits the full 5 s serial timeout per exchange in its first state poll. A port that answers the handshake and then goes silent now fails Connect in about 2 s instead of 10 s, and a Disconnect issued during that probe waits about 2 s instead of 10 s. A healthy focuser still connects with a published first frame, and a focuser with no temperature sensor still connects.
