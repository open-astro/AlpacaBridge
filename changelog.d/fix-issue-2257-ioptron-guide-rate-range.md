### Fixed

- **iOptron GuideRate setters refuse a bad rate instead of clamping it** (iOptron telescope, issue #2257): a rate that is not greater than 0 now throws `InvalidValue` before the connection check, including through GuideRateRightAscension and GuideRateDeclination, which answered `NotConnected` while disconnected. A rate outside 0.01-0.90 (RA) or 0.10-0.99 (Dec) of the sidereal rate also throws `InvalidValue` before the connection check and sends nothing; before, it was clamped and the unclamped request was stored.
