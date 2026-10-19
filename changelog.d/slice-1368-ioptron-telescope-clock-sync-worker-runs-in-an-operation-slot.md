### Changed

- **iOptron clock-sync worker on an operation slot**, iOptron telescope driver, issue #1368. The connect-time site and clock sync now runs in a `util::AsyncOperation` slot instead of a hand-rolled `std::thread`; disconnect and the destructor cancel and join it, and a body that throws is logged and kept by the slot until the next start. Covered by `AlpacaCore/tests/test_ioptron_clock_sync_slot.cpp`.
