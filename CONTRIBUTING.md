# Contributing to AlpacaCore

Thank you for your interest in contributing to AlpacaCore!

## Code Style

- Use modern C++20 features
- Follow RAII principles
- Use `#pragma once` in headers
- Prefer `std::string_view` for read-only string parameters
- Use `enum class` instead of plain `enum`
- Use `std::chrono` for durations
- No `using namespace std;` in headers
- Keep functions small and focused

## License

All contributions must be licensed under the Server Side Public License v1 (SSPL v1).

## Development Workflow

1. Create a feature branch
2. Make your changes
3. Add tests for new functionality
4. Ensure all tests pass
5. Submit a pull request

## Testing

All non-trivial code must have unit tests. Use Catch2 or doctest for testing.

## Documentation

- Add Doxygen comments for public APIs
- Update CHANGELOG.md for user-facing changes
- Update README.md if adding new features or build options

