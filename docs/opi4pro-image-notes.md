# Orange Pi 4 Pro image — NetworkManager migration notes

Findings from live testing on the rig (openastro.lan, Armbian community
trixie, vendor kernel 6.6.98-sun60iw2, AIC8800D80 WiFi) on **2026-08-09**.
Companion to `wifi-manager-design.md`. Work list for a Claude session in the
**github.com/open-astro/openastro-orangepi4pro** image repo.

## Validated working (tested live — no blockers)

- **5 GHz WPA2 NM hotspot works first try**: NM+polkitd installed, then
  `nmcli con add type wifi ... mode ap band a channel 36 ipv4.method shared
  wifi-sec.key-mgmt wpa-psk` → AP up at 5180 MHz, 10.42.0.1/24. No iMate-style
  set_key bug, no regdom issue (image already sets US and ships
  `wireless-regdb`).
- `aicwf_sdio` driver is well-behaved nl80211: unprivileged `iw` works, live
  channel is reported correctly, 25 × 5 GHz channels + AP mode advertised.
- Image hygiene already good: `iw`, `wireless-regdb`, `dnsmasq-base`, NTP
  synced, US regdom. Cleanest of the vendor-driver boards.

## Migration work list

1. **Install `network-manager` + `polkitd`** in the image build (tested
   versions: NM 1.52.1-1, polkitd 126-2 from Debian 13 repos).
2. **NM interface policy**: ship
   `/etc/NetworkManager/conf.d/10-unmanaged.conf` with
   `[keyfile] unmanaged-devices=interface-name:end0;interface-name:lo`
   (ethernet stays on systemd-networkd; NM owns wlan0 only) — or decide to
   let NM manage ethernet too, consistently across all images.
3. **Convert the AP**: current hostapd.conf runs SSID `OpenAstro`,
    2.4 GHz ch6 HT20 ("conservative defaults; revisit channel/band after
   validating the vendor driver in AP mode" — that validation is now done).
   Replace with an NM keyfile connection (`mode=ap`,
   `ipv4.method=shared`); **5 GHz (band a, ch36) is validated and can be the
   default**, with 2.4 GHz as a user-selectable fallback for range/mount
   compatibility.
   - Note the SSID is static `OpenAstro` (no MAC-derived-SSID service like
     the iMate's) — consider aligning naming across boards
     (design doc open question 4).
4. **Retire for wlan0** (after step 3): `hostapd`, the standalone `dnsmasq`
   daemon scope for wlan0 (NM's internal dnsmasq-base takes over DHCP at
   10.42.0.1 → 10.42.0.x, replacing 172.24.1.1/24 — or pin the subnet, design
   doc open question 2), and `openastro-ap-up.service` (static 172.24.1.1).
5. The AlpacaBridge polkit rule ships in the AlpacaBridge .deb (see
   wifi-manager-design.md Architecture) — nothing image-side beyond polkitd.

## New-image verification (2026-08-09, second image build)

A rebuilt image was verified on the rig: **the whole migration list above is
done** — NM 1.52.1 + polkitd active; hostapd/dnsmasq/openastro-ap-up removed;
AP is NM connection `OpenAstro-AP`, 5 GHz ch36 WPA2, `ipv4.method=shared`
pinned to `172.24.1.1/24`; end0 on networkd; wifi powersave and MAC
randomization disabled. Remaining items for the next build:

1. **AlpacaBridge not installed** — add the apt repo + package if the image
   should ship it.
2. Regdom is unset (`country 00`) — intentionally left to AlpacaBridge's
   `/management/v1/wifi/country` endpoint (see design doc); the 5 GHz AP
   happens to start anyway on `aicwf` (driver doesn't enforce no-IR), so no
   image action, but do NOT copy this assumption to the iMate where WORLD
   regdom breaks AP init.
3. ~~SSID~~ — DONE in third build (verified 2026-08-09): imager stamps
   `OpenAstro-<last 4 hex of wlan0 MAC>` (rig broadcasts `OpenAstro-915D`).
   Only item 1 (AlpacaBridge install) remains.

## Test-rig state left behind (2026-08-09)

NM 1.52.1-1 + polkitd 126-2 installed but fully parked
(`/etc/NetworkManager/conf.d/10-parked.conf` unmanages end0/lo/wlan0);
stock hostapd AP `OpenAstro` (2.4 ch6, 172.24.1.1) restored and active.
The test connection `opi-test-5g` was deleted.
