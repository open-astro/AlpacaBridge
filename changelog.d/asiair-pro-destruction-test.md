### Fixed

- **The ASIAIR Pro destruction stress case no longer fails on a re-read of the PWM line** (ZWO, issue #2267): the case waited for GPIO 18 to read low, then required a second read to be low, and the 1 kHz PWM worker could drive the line high between the two reads; it now requires the low level its wait loop saw; test-only, no production change.
