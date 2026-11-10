### Fixed

- SynScan: the end of an RA pulse guide now sends its stop and tracking restore in one wire turn and reads the mode to restore there, so a Tracking write that reached the handset first is no longer overwritten with the old mode (issue #2069).
