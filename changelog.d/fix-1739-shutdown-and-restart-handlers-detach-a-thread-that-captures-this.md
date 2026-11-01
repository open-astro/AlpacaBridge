### Fixed

- **POST shutdown and restart no longer run their callback against a destroyed Server** (AlpacaHTTP, issue #1739). The Router now owns the 100 ms delayed thread, and `Server::~Server()` cancels and joins it first, so a callback that was still waiting is dropped instead of reaching the freed Server and Router. Covered by `AlpacaHTTP/tests/test_router_shutdown.cpp`: it destroys a Server right after each POST and asserts the callback never runs, and it swaps the Router's callback after the POST and asserts the original runs once.
