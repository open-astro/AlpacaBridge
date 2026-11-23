### Fixed

- **NexStar blocking slew keeps a running pulse when its pier read fails**, Celestron NexStar driver, issue #2467. A blocking SlewToCoordinates reads the pier side before it claims the mount, as the async slew does, so a failed read refuses the slew and leaves a running PulseGuide alone instead of ending it. Tests also pin that PulseGuide is accepted again after a Park completes and Unpark runs, and that a refused pulse chain start stops its axis and closes the pulse (`test_celestron_nexstar_motion_owner.cpp`).
