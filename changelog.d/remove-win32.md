### Changed (internal)

- **AlpacaCore: the Windows branches are gone**, issue #2686. Every `#ifdef _WIN32` / `#ifndef _WIN32` arm under `AlpacaCore/src`, `AlpacaCore/include` and `AlpacaCore/tests` is removed and the POSIX code stays as it was. The Windows code was never built or tested: AlpacaBridge is Linux arm64 only. Comments that described a Windows branch are removed too. No driver behaviour, request, response or error text changes.
