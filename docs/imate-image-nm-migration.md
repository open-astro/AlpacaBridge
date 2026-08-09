# iMate image — NetworkManager migration notes

Record of everything installed/changed on the iMate test rig (openastro.lan,
OrangePi 3 LTS, Armbian 26.5.1 trixie, kernel 6.18.33-current-sunxi64) during
the live WiFi-manager migration test on **2026-08-09**, so the aw-flashtool
image can be updated properly. Companion to `wifi-manager-design.md`
(board matrix + open question 0).

## Packages installed on the test rig

Installed via `apt-get install network-manager polkitd` from the standard
Debian 13 repos (versions as installed):

| Package | Version | Note |
|---|---|---|
| network-manager | 1.52.1-1 | The WiFi-manager backend |
| polkitd | 126-2 | NM authorization; also needed for the AlpacaBridge polkit rule |
| libnm0 | 1.52.1-1 | dependency |
| libpolkit-gobject-1-0 / libpolkit-agent-1-0 | 126-2 | dependency |
| libjansson4 | 2.14-2+b3 | dependency |
| libmm-glib0 | 1.24.0-1+deb13u1 | dependency |
| libndp0 | 1.9-1+b1 | dependency |
| libteamdctl0 | 1.31-1+b2 | dependency |
| dnsmasq-base | 2.91-1+deb13u1 | NM shared-mode DHCP (distinct from the full `dnsmasq` daemon already on the image) |

Recommended-but-skipped (not needed): modemmanager, ppp.

## Config files created on the test rig

`/etc/NetworkManager/conf.d/10-unmanaged-ethernet.conf` — currently parks NM
entirely so it cannot fight the stock hostapd stack:

```ini
[keyfile]
unmanaged-devices=interface-name:end0;interface-name:lo;interface-name:wlan0
```

Rig state after testing: stock behavior fully restored (hostapd AP
`iMate_85F2D7`, 5 GHz ch40, 172.24.1.1 + dnsmasq DHCP); NM + polkitd
installed but inert per the conf above.

## Test results (what the image must account for)

1. **NM 5 GHz AP works on `unisoc_wifi`** — an open (unencrypted) NM hotspot
   (`mode=ap`, `band=a`, `channel=40`, `ipv4.method=shared`) activates and
   serves 10.42.0.1/24.
2. **WPA2 under NM fails**: the driver returns `EOPNOTSUPP` to
   wpa_supplicant's group-key install (`nl80211: set_key default failed;
   err=-95` → `AP-DISABLED`). hostapd's key-install sequence works fine.
   This is the single blocker for the migration.
3. **Regulatory domain**: 5 GHz AP init fails under the default WORLD (00)
   regdom (channels are no-IR). hostapd self-heals via `country_code=US` in
   its conf; the NM path does not set country, so the image must set it
   persistently.

## What the aw-flashtool image update needs

1. **Resolve the WPA2 blocker (pick one, see design doc open question 0):**
   - Patch `unisoc_wifi`'s `set_key` handling in the image kernel to accept
     wpa_supplicant's default/group-key sequence (preferred — we build the
     kernel), or
   - Keep hostapd as an iMate-only AP backend and have the WiFi manager use
     NM for client mode only on this board.
2. `apt-get install network-manager polkitd` in the image build.
3. **Persistent regdom**: e.g. `options cfg80211 ieee80211_regdom=US` in
   `/etc/modprobe.d/` (or set per user country at first boot — needs a
   decision for non-US users; hostapd conf hardcodes US today).
4. Hand wlan0 to NM: replace the parked conf above with one unmanaging only
   `end0`/`lo` (ethernet stays on systemd-networkd), or let NM manage
   ethernet too — decide once, consistently across images.
5. Convert the AP definition: hostapd.conf (`hw_mode=a`, ch40, HT40+, WPA2)
   becomes an NM keyfile connection (`mode=ap`, `band=a`, `channel=40`,
   `ipv4.method=shared`). NM's shared mode replaces the static
   `openastro-ap-up.service` (172.24.1.1) and the full `dnsmasq` daemon's
   wlan0 DHCP scope — NM uses `dnsmasq-base` internally at 10.42.0.1/24
   (or pin the subnet; design doc open question 2).
6. Port `openastro-ap-ssid.service` (MAC-derived SSID `iMate_XXXXXX`) to
   rewrite the NM connection's SSID instead of hostapd.conf.
7. Disable/remove for wlan0: `hostapd`, `dnsmasq` (daemon), the
   `openastro-ap-up.service` — only after (1) is resolved.
8. Ship the AlpacaBridge polkit rule (see design doc, Architecture) so the
   `alpacabridge` user can drive NM.

Note the driver quirk for anything scripted: unprivileged `iw` returns empty
output on `unisoc_wifi` (no error) — always probe capabilities as root.
