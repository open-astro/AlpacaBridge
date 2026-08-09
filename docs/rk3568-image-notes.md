# ASIAIR Plus RK3568 image — WiFi-manager requirements & fixes

Findings from live testing on the rig (astro.lan, ZWO AirPlus-RK3568, BSP
kernel 4.19.219, Debian 13 userland) on **2026-08-09**. Companion to
`wifi-manager-design.md` (board matrix). Written as a work list for a Claude
session in the RK3568 image-build repo — each item states the exact change.

## Validated working (no image changes needed for these)

- NetworkManager 1.52.1-1 installed, active, manages wlan0.
- **5 GHz WPA2 NM hotspot works**: `nmcli con add type wifi ... 802-11-wireless.mode ap
  802-11-wireless.band a 802-11-wireless.channel 36 ipv4.method shared
  wifi-sec.key-mgmt wpa-psk` → `AP-ENABLED` at 5180 MHz, DHCP served by
  NM-spawned dnsmasq (full `dnsmasq` package present, standalone service
  disabled — keep it that way).
- WiFi driver: `bcmdhd_wifi6` (rkwifi BSP, AP6256, dual-band
  `fw_bcm43456c5_ag.bin` firmware). No iMate-style wpa_supplicant set_key bug.
- Known quirk, handled in AlpacaBridge (not the image): NM reports
  `WIFI-PROPERTIES.5GHZ: no` on this driver even though 5 GHz works.

## Required fix (blocks the WiFi manager)

1. **Install `polkitd`** in the image. It is not installed and the polkit
   service is inactive. Without it, NM D-Bus calls from the unprivileged
   `alpacabridge` user cannot be authorized, so the AlpacaBridge polkit rule
   (shipped in our .deb; see wifi-manager-design.md Architecture) does nothing.
   Verify after boot: `systemctl is-active polkit` → active (it is D-Bus
   activated; installing the package is sufficient).

## Recommended fixes (same image update)

2. **Install `wireless-regdb`** — kernel logs
   `Direct firmware load for regulatory.db failed with error -2`.
3. **Fix hardcoded country `ccode=DE`** in `/lib/firmware/nvram_ap6256.txt`
   AND `/vendor/etc/firmware/nvram_ap6256.txt` (`ccode=DE` / `regrev=0`).
   Germany disallows 5 GHz ch 149–165 that US users should have. Either set
   per-market at first boot or default `ccode=US` to match the other images
   (note wifi-manager-design.md open question: country selection UX).
4. **Enable time sync** — no NTP daemon exists (timesyncd/chrony/ntpsec all
   absent/inactive); the rig's clock had drifted to Sep 2025 (corrected
   manually 2026-08-09). Add + enable `systemd-timesyncd`.
5. **Install `iw`** for field diagnostics (tiny; unprivileged `iw` works on
   this driver, unlike unisoc).
6. **Verify `apt update` succeeds** now the clock is fixed — fetches failed
   while the clock was a year stale (likely TLS/Valid-Until); sources
   (`deb http://deb.debian.org/debian trixie main`) look correct. If it still
   fails, debug DNS/IPv6 from the BSP network stack.
7. **Update AlpacaBridge to current** (rig runs 3.0.0; 3.3.0 adds Sync Time
   and serial-scan hardening).

## Nice-to-know for future work

- The vendor driver exposes a dedicated `uap0` AP interface (plus
  `p2p-dev-*`) — the stock ASIAIR's concurrent AP+STA path. Unused by our
  design v1, but it is the candidate mechanism if this board ever needs
  hotspot + client simultaneously.
- NM-spawned dnsmasq logs `chown of PID file /run/nm-dnsmasq-wlan0.pid
  failed: please add capability CAP_CHOWN` — cosmetic, DHCP works.
- Two defunct dnsmasq zombies observed parented to NM after hotspot
  restarts — cosmetic on 4.19, but worth rechecking after image changes.
