# AlpacaBridge

<img src="https://www.openastro.net/wp-content/uploads/2026/01/AlpacaBridge.png" alt="AlpacaBridge logo" width="420">

AlpacaBridge is a complete ASCOM Alpaca server for Linux. Install it on a machine at the mount, connect your gear once, and access cameras, focusers, filter wheels, rotators, mounts, and weather stations over the network from any Alpaca-compatible software (N.I.N.A., SGP, and others). All drivers are validated against ConformU.

#### [1.0.3] - 2026-05-07 &middot; [Changelog](CHANGELOG.md)

## Features

- **Turnkey Alpaca server** — installs as a systemd service, starts on boot
- **[ConformU-validated drivers](SUPPORTED-DRIVERS.md)** — cameras (ZWO, QHY, SVBONY, ToupTek, Player One), mounts (iOptron, SynScan, Celestron, ZWO), focusers (ZWO, Gemini), filter wheels, rotators, switches, and weather stations (WeeWX)
- **Auto-detection** — USB and Wi-Fi devices are discovered automatically, no manual port configuration required
- **Bundled vendor SDKs and udev rules** — plug in supported USB devices and go
- **Web management UI** — configure devices from any browser on your network

## AlpacaBridge Overview

AlpacaBridge runs on any Debian 13 machine — a Raspberry Pi at the mount, a mini PC in the observatory, or an off-the-shelf astronomy control unit like a ToupTek StellaVita, ZWO ASIAIR, or iOptron iMate repurposed with a fresh Debian install. See the [Wiki](https://github.com/open-astro/AlpacaBridge/wiki) for setup guides covering these devices.

ScopeTrader covered the project launch and what it means for the Alpaca ecosystem: [OpenAstro AlpacaBridge Launches and Why It Matters](https://scopetrader.com/openastro-alpacabridge-launches-and-why-it-matters/).

Watch a full install on a ToupTek StellaVita: [Installing AlpacaBridge on a ToupTek StellaVita](https://youtu.be/nVAS45OTltA).

## Supported platforms

- **Debian 13 (Trixie)** on `arm64` (Raspberry Pi 3B+/4/5, Rockchip SBCs, OrangePi, iOptron iMate)

## Learn more

- [Wiki](https://github.com/open-astro/AlpacaBridge/wiki) — setup guides for Raspberry Pi, StellaVita, ASIAIR, iMate, and troubleshooting
- [SUPPORTED-DRIVERS.md](SUPPORTED-DRIVERS.md) — validated driver matrix
- [Development Guide](docs/development.md) — building from source, writing drivers, running tests

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
