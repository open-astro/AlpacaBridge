# Development

This document covers building AlpacaBridge from source, running the test suite, and adding new drivers. End-user install instructions live in the [README](README.md).

## Repository layout

- `AlpacaCore/` — driver library and vendor integrations ([README](AlpacaCore/README.md))
- `AlpacaHTTP/` — HTTP server exposing the Alpaca API ([README](AlpacaHTTP/README.md))
- `debian/` — Debian packaging used by the apt.openastro.net repository
- `AGENTS.md` — AI-assisted driver development workflow and rules
- `SUPPORTED-DRIVERS.md` — ConformU-validated driver matrix

## Build dependencies

On Debian 13 (Trixie):

```sh
sudo apt install build-essential cmake g++ \
    libusb-1.0-0-dev libudev-dev \
    nlohmann-json3-dev libcurl4-openssl-dev \
    catch2
```

## Build and run

```sh
chmod +x build_and_run.sh
./build_and_run.sh
```

When the server starts, it prints:

```
AlpacaHTTP is running. Open http://localhost:6800/ in your browser.
```

AlpacaBridge listens on port **6800** by default, both for source builds and the apt-installed service.

## Run the test suite

```sh
chmod +x run_all_tests.sh
./run_all_tests.sh
```

## Build options

- `ALPACAHTTP_USE_BOOST_BEAST` (default: `OFF`) — build AlpacaHTTP with Boost.Beast
- `ALPACACORE_ENABLE_ALL_VENDORS` (default: `ON`) — set to `OFF` to disable bundled vendor drivers

## Installing a source build as a systemd service

For development on a target machine without pulling from apt, `install_alpaca_service.sh` builds from the working tree and installs a systemd unit:

```sh
chmod +x install_alpaca_service.sh
./install_alpaca_service.sh install
```

Commands:

- `install` — build, install udev rules, create and start the systemd service
- `update` — stop the service, rebuild, restart
- `uninstall` — stop and remove the systemd service
- `status` — show service status

For production use, prefer the apt package.

## Building custom drivers

AlpacaBridge is designed around AI-assisted driver development. The workspace ships with Cursor rules and an agent guide so AI tools can follow the established patterns.

For a full walkthrough of the build/test/run loop and AI-assisted driver development, see the video: [Building the Future of Astrophotography & EAA with AlpacaBridge](https://youtu.be/7yPSW0KXQzM).


1. Place the vendor SDK in `AlpacaCore/external/`
2. Use an AI coding tool (Cursor or similar) — it will read [AGENTS.md](AGENTS.md) and the rules in `AlpacaCore/.cursor/rules/` to understand the architecture
3. Test against the workspace test scripts
4. Validate with ConformU before adding the driver to [SUPPORTED-DRIVERS.md](SUPPORTED-DRIVERS.md)

See [AlpacaCore/README.md](AlpacaCore/README.md) for a deeper walkthrough of driver development.

## Packaging

The Debian package published to [apt.openastro.net](https://apt.openastro.net) is built from the `debian/` directory on this repository. The package installs:

- Binary at `/usr/bin/alpacabridge`
- Bundled vendor libraries under `/usr/lib/alpacabridge`
- Configuration under `/etc/alpacabridge`
- Systemd unit `alpacabridge.service` running as the `alpacabridge` system user
