---
applyTo: "AlpacaHTTP/**,docs/wifi-*.md"
---

### WiFi manager (AlpacaHTTP, 3.4.0)

Related decision: [LAN surface threat model](../../docs/decisions/0007-lan-surface-threat-model.md) (who may reach the Wi-Fi API and what it can change).

- NM D-Bus property types matter: `ActiveConnection`, `Ip4Config`, and
  `ActiveAccessPoint` are object paths ("o"), not strings — reading them
  with `sd_bus_get_property_string` fails sd-bus's type check silently
  (empty result, no error), which presented as "hotspot shows off while
  broadcasting". Use a dedicated "o"-typed reader.
- Shared-mode (hotspot) activation needs polkit
  `org.freedesktop.NetworkManager.wifi.share.protected`/`.share.open` in
  addition to `network-control` — the failure ("Not authorized to share
  connections via wifi") only appears at ActivateConnection time and was
  found by live-testing the endpoint, not by review.
- NM `Update()` with a `802-11-wireless-security` section declaring
  `key-mgmt` but omitting `psk` PRESERVES the stored secret
  (hardware-verified on the Pi 5 rig). `GetSettings()` never returns
  secrets, so this cannot be confirmed from read-back — verify on hardware
  when in doubt.
- One `sd_bus*` connection is not thread-safe: every method serializes on
  one mutex, and that mutex must stay held across any waits between bus
  calls (a review round caught an unlock-during-sleep race). Blocking work
  that touches no bus state (nl80211 netlink) belongs outside that mutex,
  on its own serialization if ordering matters.
- Vendor driver capability reports lie: RK3568 `bcmdhd` tells NM 2.4-only
  while 5 GHz hotspots work; iMate `unisoc_wifi` returns nothing to
  unprivileged `iw` and rejects wpa_supplicant's WPA2 group-key install
  (hostapd's sequence works). Board matrix in `docs/wifi-manager-design.md`.
- 5 GHz AP init fails under the WORLD/00 regdom on some drivers — the
  persisted country must be applied at daemon startup (main.cpp), BEFORE
  NM's boot-time AP autoconnect, not lazily on first request.
- Even when 5 GHz AP init *succeeds* under WORLD/00, the AP can beacon on a
  world-domain-forbidden channel (seen live: NM auto picked ch 149 on the
  OPi 4 Pro) that clients refuse to see or join — "the network disappeared".
  `set_ap` therefore rejects band "a" until a country is persisted, and the
  web UI front-runs that with a message pointing at the country selector
  (3.5.1). 2.4 GHz is exempt: ch 1-11 are world-domain legal, which is why
  the shipped images default to 2.4 GHz ch 6.
- The review bot login is `github-actions`; every push restarts a full
  review round — batch fixes. Test rig persisted-device state makes
  `test_routing` fail with "already registered". Since #274 each of the two
  router-backed binaries runs in its own ctest `WORKING_DIRECTORY`, so the
  files to clear are `AlpacaHTTP/build/test_routing_cwd/config/` and
  `AlpacaHTTP/build/test_persisted_devices_cwd/config/`, not
  `build/config/`.
