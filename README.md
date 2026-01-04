# AlpacaBridge

<img src="https://www.openastro.net/wp-content/uploads/2026/01/AlpacaBridge.png" alt="AlpacaBridge logo" width="420">

AlpacaBridge is a workspace that brings together AlpacaCore and AlpacaHTTP so you can build, test, and run a complete Alpaca server from one place. It is meant to be a simple, starter-friendly entry point for local development.

## Quick Start

### Run all tests

macOS/Linux:
```sh
chmod +x run_all_tests.sh
./run_all_tests.sh
```

Windows (CMD/PowerShell):
```bat
run_all_tests.cmd
```

### Build and run the HTTP server

macOS/Linux:
```sh
chmod +x build_and_run.sh
./build_and_run.sh
```

Windows (CMD/PowerShell):
```bat
build_and_run.cmd
```

When the server starts, it prints:
`AlpacaHTTP is running. Open http://localhost:6800/ in your browser.`

## Optional settings

- `ALPACAHTTP_USE_BOOST_BEAST` (default: OFF): set to ON to build AlpacaHTTP with Boost.Beast.
- `ALPACACORE_ENABLE_ALL_VENDORS` (default: ON): set to OFF to disable vendor drivers.
- `ALPACA_BUILD_CONFIG` (Windows only): set to `Debug` or `Release` for multi-config generators.

## Learn more

For deeper details, see the project READMEs:

- [AlpacaCore/README.md](AlpacaCore/README.md)
- [AlpacaHTTP/README.md](AlpacaHTTP/README.md)
