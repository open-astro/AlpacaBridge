# WiFi Manager — Design Document

Status: **DRAFT** — design only, no implementation yet.
Branch: `feature/wifi-manager`

## Goal

Let users manage the SBC's WiFi entirely from the AlpacaBridge web portal:

1. **Client mode (STA)** — scan for networks, save multiple profiles (home network,
   remote-site hotspot, phone tether), join one, show live status. Supports both
   2.4 GHz and 5 GHz where the board's hardware allows it.
2. **Access Point mode (AP)** — the SBC broadcasts its own hotspot so that
   (a) the user can reach the portal in the field with no infrastructure, and
   (b) WiFi-controlled devices (e.g. a ZWO/iOptron mount) can join the SBC's
   network and be driven via the existing "Network/WiFi" device connection type.
3. **Fallback behavior** — on boot, try known networks; if none is reachable,
   bring up the AP automatically. An "always run hotspot" toggle covers the
   permanent mount-control use case.

Non-goal (v1): simultaneous AP + STA. Most supported chips only allow it on the
same channel/band, which makes it fragile and confusing. v1 is
**client-or-AP with automatic fallback**; concurrency can be revisited later
per-board (see Hardware matrix).

## Architecture

### Backend: NetworkManager over D-Bus

All OpenAstro images must run **NetworkManager** (Armbian images already do;
Raspberry Pi Debian 13 images to be confirmed/standardized — tracked as a
prerequisite in the rollout plan). The daemon talks to NM via the system D-Bus
(`org.freedesktop.NetworkManager`) — no shelling out to `nmcli`, no sudo, no
root helper, consistent with the project's no-subprocess policy.

**Privilege model.** The daemon runs as the unprivileged `alpacabridge` user
under a hardened unit (`NoNewPrivileges=true`). NM authorizes callers via
polkit, so the .deb ships a polkit rule granting `alpacabridge` exactly the NM
actions it needs:

```
/usr/share/polkit-1/rules.d/50-alpacabridge-wifi.rules
```

allowing (only) for user `alpacabridge`:
- `org.freedesktop.NetworkManager.wifi.scan`
- `org.freedesktop.NetworkManager.settings.modify.system`
- `org.freedesktop.NetworkManager.network-control`
- `org.freedesktop.NetworkManager.enable-disable-wifi`

No other hardening relaxation on the unit is needed. This is a new mechanism
for the repo (the precedent, Sync Time, used an ambient capability); document
it in `docs/architecture.md` when implemented.

Library choice: `libsystemd`'s `sd-bus` (small, already a transitive presence
on all target images) rather than full GDBus. Add `libsystemd-dev` to
build-deps.

### Endpoints

Management routes (unauthenticated, trusted-LAN model, same as existing
management endpoints), following the Sync Time pattern
(`router.cpp` match_route → handle_management → handler; tests in
`tests/test_routing.cpp`):

| Method | Path | Purpose |
|---|---|---|
| GET | `/management/v1/wifi/status` | Adapter state, current SSID/band/signal/IP, AP-active flag, hardware capabilities (bands, AP support) |
| GET | `/management/v1/wifi/scan` | Trigger + return scan results (SSID, band, signal, security) |
| GET | `/management/v1/wifi/profiles` | Saved profiles (never returns passphrases) |
| PUT | `/management/v1/wifi/profiles` | Add/update a profile `{Ssid, Passphrase, Autoconnect, Priority}` |
| DELETE | `/management/v1/wifi/profiles/{id}` | Remove a profile |
| PUT | `/management/v1/wifi/connect` | Connect to a profile now `{Id}` |
| GET/PUT | `/management/v1/wifi/ap` | Get/set AP config `{Enabled: "auto"\|"always"\|"off", Ssid, Passphrase, Band, Channel}` |

Notes:
- Passphrases are write-only through the API; stored by NM in
  `/etc/NetworkManager/system-connections/` (root-only, mode 600).
- `connect` responses must handle the "you just disconnected the network you're
  browsing from" problem: return immediately, apply after a short delay, and
  the UI warns the user (see UX).
- The AP profile is a normal NM connection (`mode=ap`, shared IPv4 —
  NM runs dnsmasq for DHCP on the hotspot subnet, default `10.42.0.0/24`).

### Fallback logic

Implemented with NM connection priorities plus a small watchdog in the daemon:

1. All STA profiles: `autoconnect=yes`, user-ordered priority.
2. AP profile: `autoconnect=no`; the daemon monitors NM state and activates the
   AP if no connection is active `N` seconds after WiFi is up (default 60 s).
3. While the AP is active, a periodic background rescan looks for known
   networks; if one appears and AP mode is `auto`, drop the AP and join it
   (with hysteresis so it doesn't flap). In `always` mode, never leave AP.

### Web UI

New "WiFi" card in `index.html` + handlers in `app.js` (vanilla JS, no build
step, same as everything else):

- Status header: current network, band, signal bars, IP — or "Hotspot active:
  <SSID>".
- Scan list with join dialog (passphrase field, save-as-profile).
- Saved profiles list: reorder priority, edit, delete, "Connect now".
- Hotspot section: mode (auto / always / off), SSID, passphrase, band
  selector — 5 GHz option shown only if `status.capabilities` reports it.
- Prominent warning before any action that will drop the user's current
  connection ("You are connected via this network. The device will switch;
  reconnect via <new SSID> or hotspot <AP SSID> at 10.42.0.1").

### Capability probing (dual-band "just works")

Never hardcode per-board band support. `wifi/status` reports what NM exposes
for the device (`WirelessCapabilities`, supported frequencies) and the UI adapts.
This is how "2.4 and 5 GHz based on the board they have" falls out for free —
including future boards.

## Hardware / board matrix

| Board | Chip / driver | Bands | AP mode | Validation needed |
|---|---|---|---|---|
| Pi 3B+/4/5, CM4 (StellaVita, ASIAIR Pro/Plus-CM4) | Broadcom `brcmfmac` | 2.4 + 5 | Yes | Standard; confirm NM on Pi image |
| iMate (OrangePi 3 LTS, Allwinner H6) | AW859A (mainline driver via Armbian) | 2.4 + 5 | To verify | AP mode on mainline driver; prior art in aw-flashtool AP recreation |
| Orange Pi 4 Pro / other Rockchip | Varies per board | Varies (some 2.4-only) | Varies | Capability probing handles this; validate one reference board |

## Rollout plan

- **Phase 0 (prerequisite):** confirm/standardize NetworkManager on all
  OpenAstro images (aw-flashtool / image-build concern, not this repo).
- **Phase 1:** status + scan + profiles + connect (client mode only).
  Endpoints, polkit rule, UI card, routing tests.
- **Phase 2:** AP mode + auto-fallback watchdog + always-hotspot toggle.
- **Phase 3:** hardware validation pass (Pi rig `astro.lan`, iMate, OPi rig
  `192.168.1.134`); mount-over-hotspot end-to-end test with a WiFi mount.
- Docs: user guide on openastro.net/docs (not the GitHub wiki), architecture.md
  section on the polkit mechanism, CHANGELOG minor bump per versioning policy.

## Open questions

1. Pi image network stack today — NetworkManager or not? (Blocks Phase 0.)
2. AP subnet: accept NM's default `10.42.0.0/24` or pin a documented OpenAstro
   subnet so mount-connection docs can give a fixed bridge IP?
3. Should ethernet-connected users see the WiFi card at all when no WiFi
   adapter is present? (Proposal: hide card when NM reports no wifi device.)
4. Default AP SSID/passphrase scheme — e.g. `OpenAstro-<serial suffix>` with a
   per-image or first-boot-generated passphrase printed in the portal?
