### Fixed

- **ZWO AM GuideRate setters refuse a negative or out-of-range rate** (ZWO telescope, issue #2257): a negative rate was accepted as its absolute value, and a non-zero rate outside 0.10-0.90 x sidereal was clamped once connected. Both now throw `InvalidValue` before the connection check; a rate of 0 is unchanged.
