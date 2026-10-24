### Fixed
- **ZWO AM Auto skips held network endpoints before probing** (AlpacaCore, issue #1536): discovery logs a DEBUG skip and continues to free candidates; atomic endpoint reservations also prevent discovery-to-connect races and remain held until actual socket closure. Existing mount-serial admission and explicitly configured pairs keep their policies.

### Changed
- **AM5 qualification reports refreshed**: USB and onboard WiFi pass ConformU 4.5.3 on the same arm64 candidate binary and mount firmware 1.8.8. The WiFi report uses the onboard radio with its concurrent hotspot stopped; unresolved USB-dongle timing and association failures are disclosed in the support notes.
