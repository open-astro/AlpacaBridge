# AlpacaBridge

<img src="https://www.openastro.net/wp-content/uploads/2026/01/AlpacaBridge.png" alt="AlpacaBridge logo" width="420">

AlpacaBridge is a complete ASCOM Alpaca server for Linux. Install it on a machine at the mount, connect your gear once, and access cameras, focusers, filter wheels, rotators, mounts, and weather stations over the network from any Alpaca-compatible software (N.I.N.A., SGP, and others). All drivers are validated against ConformU.

#### [1.0.2] - 2026-04-18 &middot; [Changelog](CHANGELOG.md)

## Features

- **Turnkey Alpaca server** — installs as a systemd service, starts on boot
- **ConformU-validated drivers** — pre-built support for ZWO cameras, iOptron mounts, and more (see [SUPPORTED-DRIVERS.md](SUPPORTED-DRIVERS.md))
- **Bundled vendor SDKs and udev rules** — plug in supported USB devices and go
- **Web management UI** — configure devices from any browser on your network

## AlpacaBridge Overview

ScopeTrader covered the project launch and what it means for the Alpaca ecosystem: [OpenAstro AlpacaBridge Launches and Why It Matters](https://scopetrader.com/openastro-alpacabridge-launches-and-why-it-matters/).

Watch a full install on a ToupTek StellaVita: [Installing AlpacaBridge on a ToupTek StellaVita](https://youtu.be/nVAS45OTltA).

## Supported platforms

- **Debian 13 (Trixie)** on `amd64` (Intel/AMD 64-bit) or `arm64` (Raspberry Pi 3B+, 4 & 5)

## Learn more

- [Wiki](https://github.com/open-astro/AlpacaBridge/wiki) — user guides, configuration, and troubleshooting
- [SUPPORTED-DRIVERS.md](SUPPORTED-DRIVERS.md) — validated driver matrix
- [DEVELOPMENT.md](DEVELOPMENT.md) — building from source, writing drivers, running tests

## Installation

AlpacaBridge is distributed via the OpenAstro APT repository at [apt.openastro.net](https://apt.openastro.net).

### 1. Add the OpenAstro signing key

```sh
sudo curl -fsSL https://apt.openastro.net/repo/openastro-archive-keyring.gpg \
    | sudo gpg --dearmor -o /usr/share/keyrings/openastro-archive-keyring.gpg
```

### 2. Add the repository

```sh
echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/openastro-archive-keyring.gpg] \
https://apt.openastro.net trixie main" \
    | sudo tee /etc/apt/sources.list.d/openastro.list
```

### 3. Install

```sh
sudo apt update
sudo apt install alpacabridge
```

The service starts automatically and runs as the `alpacabridge` system user.

## Using AlpacaBridge

Once installed, open:

- **Management UI:** http://localhost:6800/management/
- **Device API:** http://localhost:6800/

From another machine on your network, substitute the hostname or IP address of the server.

### Updating

```sh
sudo apt update
sudo apt upgrade alpacabridge
```

### Uninstalling

```sh
sudo apt remove alpacabridge
```

## Support the project

AlpacaBridge is developed and maintained independently. Contributions help fund hardware access, testing, and ongoing standards-compliance work.

[![PayPal](https://img.shields.io/badge/Donate-PayPal-blue.svg)](https://www.paypal.com/paypalme/joeytroynm)

## License

See [LICENSE](LICENSE).
