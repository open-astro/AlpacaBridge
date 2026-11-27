### Changed

- **iOptron test checks DeviceState is empty while a poll fault is latched** (iOptron, issue #2683): the "site getters and DeviceState refuse at once with a fault latched" case in `test_ioptron_polled_state.cpp` now asserts the list is empty, so a getter that served the frame while faulted fails it; test-only, no production change.
