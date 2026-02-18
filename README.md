# AlpacaBridge

<img src="https://www.openastro.net/wp-content/uploads/2026/01/AlpacaBridge.png" alt="AlpacaBridge logo" width="420">

AlpacaBridge is a unified workspace for building, testing, and running a complete ASCOM Alpaca server. It combines AlpacaCore (driver library) and AlpacaHTTP (HTTP server) with AI-assisted development tools and ConformU-validated drivers.

**Intrested? Want to help?** Join our [Discord Support Channel](https://discord.com/channels/1092619106282393610/1092619106282393613)

## AlpacaBridge Explained

- ScopeTrader: [Openastro AlpacaBridge Launches and Why it Matters](https://scopetrader.com/openastro-alpacabridge-launches-and-why-it-matters/)

- This video walks through building, testing, and running a complete ASCOM Alpaca server, including
AI-assisted driver development and ConformU validation [
Building the Future of Astrophotography & EAA with AlpacaBridge, an ASCOM Alpaca Solution!](https://youtu.be/7yPSW0KXQzM)

## Project Overview

**Key Features:**
- **Complete Alpaca Server** - Build and run a full-featured ASCOM Alpaca server with web UI
- **Comprehensive Driver Support** - Pre-built drivers for ZWO cameras, switches, and focusers, plus iOptron telescopes (all ConformU validated)
- **AI-Assisted Development** - Build custom drivers for any vendor device using AI guidance (see [AGENTS.md](AGENTS.md))
- **Convenience Scripts** - One-command build, test, and run workflows
- **Production Ready** - All drivers are ConformU validated for ASCOM Alpaca specification compliance

## Learn more

For deeper details, see the project READMEs:

- [AlpacaCore/README.md](AlpacaCore/README.md) - Core library and driver development
- [AlpacaHTTP/README.md](AlpacaHTTP/README.md) - HTTP server implementation
- [AGENTS.md](AGENTS.md) - AI-assisted development workflow and rules
- [AlpacaCore/SUPPORTED-DRIVERS.md](AlpacaCore/SUPPORTED-DRIVERS.md) - List of validated drivers

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

## Installation (Linux)

For Linux systems, you can use the installation script to build and install AlpacaCore and AlpacaHTTP as a systemd service.

```bash
chmod +x install_alpaca_service.sh
./install_alpaca_service.sh install
```

### Commands

- **`install`** - Builds AlpacaCore and AlpacaHTTP, installs udev rules, creates a systemd service, and starts the service
- **`update`** - Stops the service, rebuilds the projects, and restarts the service
- **`uninstall`** - Stops and disables the systemd service, removes the service file
- **`status`** - Shows the current status of the AlpacaHTTP service

## Building Custom Drivers

AlpacaBridge supports AI-assisted driver development, making it easy to build drivers for any vendor device. The workspace includes comprehensive guides and Cursor rules that help AI agents understand the architecture and build drivers following established patterns.

**Getting Started:**
1. Place your vendor SDK in `AlpacaCore/external/`
2. Use AI (with Cursor or similar tools) to build your driver - the AI will automatically reference [AGENTS.md](AGENTS.md) and the Cursor rules in `AlpacaCore/.cursor/rules/` to understand the architecture and build patterns
3. Test your driver using the workspace test scripts
4. Validate with ConformU before adding to [SUPPORTED-DRIVERS.md](AlpacaCore/SUPPORTED-DRIVERS.md)

**Note:** [AGENTS.md](AGENTS.md) is designed for AI agents to read when building drivers, though users can also reference it to understand the development workflow.

See [AlpacaCore/README.md](AlpacaCore/README.md) for detailed driver development guidance.
