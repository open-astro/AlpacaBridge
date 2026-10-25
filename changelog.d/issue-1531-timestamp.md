### Fixed
- **Snapshot timestamp documentation** (AlpacaCore, issue #1531): clarify that ZWO DeviceState retains the last mandatory GU plus RA/Dec measurement age across partial publications, with the partial frame's age as the pre-measurement fallback; runtime behavior is unchanged.

### Added (tests)
- **ZWO staged timestamp coverage** (AlpacaCore, issue #1531): exercise failed coordinate pairs, delayed optional publication, moving warm-up and complete-sample recovery using the existing fake mount and task clock.
