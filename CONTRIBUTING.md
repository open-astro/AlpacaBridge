# Contributing to AlpacaHTTP

Thank you for your interest in contributing to AlpacaHTTP!

## Development Setup

1. Clone the repository
2. Ensure you have AlpacaCore as a dependency
3. Build with CMake:
   ```bash
   mkdir build && cd build
   cmake ..
   make
   ```

## Code Style

- Use C++20 features
- Follow RAII principles
- Use `#pragma once` in headers
- Prefer `std::string_view` when appropriate
- Use `enum class` for enums
- Use `std::chrono` for time/durations
- No `using namespace std;` in headers

## Architecture

AlpacaHTTP is a transport layer only. It does NOT:
- Implement device logic
- Touch vendor SDKs
- Duplicate AlpacaCore behavior

All device operations go through AlpacaCore.

## Testing

Run tests with:
```bash
cd build
ctest
```

## License

By contributing, you agree that your contributions will be licensed under the Server Side Public License v1.

