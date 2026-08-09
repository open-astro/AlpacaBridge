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

All OpenAstro images must run **NetworkManager**. Verified on hardware
2026-08-09: Pi-family (CM4) and ASIAIR Plus RK3568 already do; the Orange Pi
4 Pro image does not yet (see board matrix and Phase 0). The daemon talks to NM via the system D-Bus
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

Surveyed on real hardware 2026-08-09 (rc91.lan, astro.lan, openastro.lan).
"5 GHz" = what NM/the driver actually reports, not the chip's datasheet.

| Board | Chip / driver | 2.4 GHz | 5 GHz | Network stack today | AP mode | Notes |
|---|---|---|---|---|---|---|
| Pi 3B+/4/5, CM4 (StellaVita, ASIAIR Pro/Plus-CM4) | Broadcom `brcmfmac` | Yes | **Yes — validated live 2026-08-09**: 5 GHz WPA2 NM hotspot on ch36, first try. ASIAIR Pro (Pi 4B internally) and original ASIAIR (Pi 3B+, CYW43455 — oldest WiFi chip in the fleet) validated identically 2026-08-09 — same image profile, same result | NetworkManager ✅ (+polkitd, dnsmasq-base, wireless-regdb, iw all preinstalled) | Yes | Only finding: image ships WiFi radio soft-disabled (`nmcli radio wifi` = off; restored after test). The WiFi manager must run `nmcli radio wifi on`-equivalent (NM D-Bus `WirelessEnabled`) when the user first configures WiFi — treat as a feature requirement, not an image change |
| ASIAIR Plus RK3568 | AP6256 on `bcmdhd_wifi6` vendor driver (rkwifi, BSP kernel 4.19; dual-band `fw_bcm43456c5_ag.bin` firmware) | Yes | **Yes — validated live 2026-08-09**: WPA2 NM hotspot on ch36/5180 MHz (`AP-ENABLED`), and scans see 5 GHz networks. NM's `WIFI-PROPERTIES.5GHZ: no` is a bogus bcmdhd capability report — NM doesn't enforce it. **Design note: never gate the UI on NM's capability flags alone on this driver; probe via scan results too** | NetworkManager ✅ | Yes — WPA2 works under NM (no iMate-style set_key bug) | No image changes required. Cleanups when touching the image: nvram `ccode=DE` → per-user country; missing `regulatory.db`; `iw` not installed; apt repos unreachable from the BSP image (apt update fails); RTC was ~1 yr stale (fixed manually 2026-08-09). Vendor driver also exposes `uap0` (stock ASIAIR's AP+STA path) — possible post-v1 concurrency option |
| Orange Pi 4 Pro | AIC8800D80 (`aicwf_sdio`, well-behaved nl80211) | Yes | **Yes — validated live 2026-08-09**: 5 GHz WPA2 NM hotspot on ch36 first try; 25 × 5 GHz channels advertised; no set_key or regdom issues | systemd-networkd + hostapd + dnsmasq — **no NM** (NM+polkitd installed on test rig, parked) | Yes — both hostapd (stock, 2.4 ch6) and NM (5 GHz, tested) | Cleanest vendor-driver board. Migration work list: `opi4pro-image-notes.md` (openastro-orangepi4pro repo); 5 GHz can be the default AP band there |
| iMate (OrangePi 3 LTS, Allwinner H6) | AW859A / Unisoc UWE5822 (`unisoc_wifi`, Armbian mainline 6.18) | Yes | Yes — **AP runs on 5 GHz today** (hostapd `hw_mode=a` ch40 HT40+, validated live). Driver rejects VHT/80 MHz (802.11n rates only); non-DFS ch 36–48/149–165 | systemd-networkd + hostapd + dnsmasq + wpa_supplicant — **no NM**; `network-manager`/`polkitd` packages not installed (only missing pieces) | Yes (hostapd; MAC-derived SSID via openastro-ap-ssid.service) | OPi 3 **LTS** uses AW859A, not the original OPi 3's AP6256. Image = aw-flashtool. **NM migration tested live 2026-08-09**: NM+polkitd installed, open 5 GHz NM hotspot works, but **WPA2 fails** — `unisoc_wifi` returns EOPNOTSUPP to wpa_supplicant's group-key install (`set_key default failed; err=-95`), while hostapd's key sequence works. Fix in the image: patch the driver's set_key path (we build the kernel) or keep a hostapd backend exception for iMate. Regdom must be set to US before 5 GHz AP init (hostapd does it via country_code; NM path needs it set globally). NM/polkitd left installed on the test rig, all interfaces unmanaged |

The RK3568 row is why capability probing (below) must be smarter than trusting
one source: bcmdhd under-reports its capabilities to NM (claims 2.4-only) while
5 GHz APs work fine. Probe order: NM capability flags, cross-checked against
scan results (any 5 GHz BSS seen ⇒ band supported); offer 5 GHz when either
says yes, and surface activation errors gracefully if an AP attempt fails.

## Rollout plan

- **Phase 0 (prerequisite):** standardize NetworkManager on all OpenAstro
  images. Pi-family and RK3568 already run NM (verified); Orange Pi 4 Pro
  (openastro-orangepi4pro repo) and iMate (aw-flashtool) need migrating off
  networkd/hostapd — we build both images, so this is fully in our control.
- **Phase 1:** status + scan + profiles + connect (client mode only).
  Endpoints, polkit rule, UI card, routing tests.
- **Phase 2:** AP mode + auto-fallback watchdog + always-hotspot toggle.
- **Phase 3:** hardware validation pass (Pi rig `astro.lan`, iMate, OPi rig
  `192.168.1.134`); mount-over-hotspot end-to-end test with a WiFi mount.
- Docs: user guide on openastro.net/docs (not the GitHub wiki), architecture.md
  section on the polkit mechanism, CHANGELOG minor bump per versioning policy.

## Open questions

0. iMate WPA2-under-NM: patch `unisoc_wifi`'s set_key handling in the
   aw-flashtool kernel, or ship a hostapd-backend exception for this board?
   (Everything else about NM on the iMate is proven — see board matrix.)

1. ~~iMate network stack~~ — answered 2026-08-09: no NM, same
   networkd/hostapd stack as OPi 4 Pro; all four board families now surveyed
   (see board matrix). Phase 0 = migrate OPi 4 Pro + iMate images to NM.
2. AP subnet: accept NM's default `10.42.0.0/24` or pin a documented OpenAstro
   subnet so mount-connection docs can give a fixed bridge IP?
3. Should ethernet-connected users see the WiFi card at all when no WiFi
   adapter is present? (Proposal: hide card when NM reports no wifi device.)
4. Default AP SSID/passphrase scheme — e.g. `OpenAstro-<serial suffix>` with a
   per-image or first-boot-generated passphrase printed in the portal?
