### Fixed

- **`test_server_socket` teardown races**, AlpacaHTTP tests, issue #1662. The shutdown-callback case now shares its mutex and condition variable with the callback through a `shared_ptr`, so the endpoint's detached thread can no longer touch them after the block closes (ThreadSanitizer reported `pthread_cond_destroy` against `pthread_cond_broadcast`; clean after). The join-fallback case (a2) parks the abandoned server thread in a test-owned holder and joins it before `server` and the log sink state are destroyed. Covered by `AlpacaHTTP/tests/test_server_socket.cpp`.
