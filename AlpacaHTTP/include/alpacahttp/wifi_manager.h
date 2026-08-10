// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#pragma once

// WiFi management backend for the /management/v1/wifi/* endpoints.
//
// Talks to NetworkManager over the system D-Bus (sd-bus, in-process — no
// subprocesses per project policy). Authorization comes from the polkit rule
// shipped in debian/alpacabridge.polkit-rules, which grants the alpacabridge
// user the NetworkManager actions used here. The regulatory country is set
// via nl80211 (equivalent of `iw reg set`), which needs CAP_NET_ADMIN — the
// systemd unit grants it as an ambient capability (same mechanism as
// CAP_SYS_TIME for synctime).
//
// All methods are synchronous and thread-safe (serialized on an internal
// mutex; NM calls are fast and the management endpoints are low-traffic).
// Every method returns nlohmann::json and throws WifiError on failure so the
// router can map failures onto Alpaca error responses uniformly.

#include <mutex>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace alpacahttp::util {

class WifiError : public std::runtime_error {
public:
    explicit WifiError(const std::string& what) : std::runtime_error(what) {}
};

class WifiManager {
public:
    // state_dir: writable directory for persistence (wifi country), normally
    // /var/lib/alpacabridge. Created lazily on first write.
    explicit WifiManager(std::string state_dir);
    ~WifiManager();

    WifiManager(const WifiManager&) = delete;
    WifiManager& operator=(const WifiManager&) = delete;

    // True if NetworkManager is reachable on the system bus AND reports at
    // least one wifi device. The router hides the feature (404-style error)
    // when false so ethernet-only setups don't show a dead card.
    bool available();

    // {"Available":bool, "WirelessEnabled":bool, "Device":str,
    //  "State":str, "ConnectionId":str, "Ssid":str, "Mode":"infrastructure"|"ap",
    //  "FrequencyMhz":u32, "SignalPercent":u8, "Ip4Address":str,
    //  "Capabilities":{"Freq2GHz":bool,"Freq5GHz":bool,"Ap":bool},
    //  "ScanSees5GHz":bool, "Country":str}
    // Capability caveat: some vendor drivers under-report to NM (bcmdhd says
    // 2.4-only while 5 GHz APs work), so ScanSees5GHz reports whether the
    // last scan saw any BSS above 5000 MHz — the UI offers 5 GHz if either
    // source says yes (see docs/wifi-manager-design.md board matrix).
    nlohmann::json status();

    // Enable/disable the wifi radio (NM WirelessEnabled).
    void set_wireless_enabled(bool enabled);

    // Triggers a scan (best-effort) and returns the visible access points:
    // [{"Ssid":str,"FrequencyMhz":u32,"SignalPercent":u8,"Security":str}]
    // sorted by signal, hidden-SSID entries dropped, deduplicated by SSID
    // keeping the strongest BSS.
    nlohmann::json scan();

    // Saved wifi profiles (802-11-wireless connections):
    // [{"Id":str,"Uuid":str,"Ssid":str,"Mode":"infrastructure"|"ap",
    //   "Autoconnect":bool,"Priority":int,"Active":bool}]
    // Passphrases are never returned.
    nlohmann::json profiles();

    // Add or update a client (infrastructure) profile. Passphrase may be
    // empty for open networks; omit to keep the existing secret when
    // updating. Returns the profile as in profiles().
    nlohmann::json save_profile(const std::string& ssid,
                                const std::string& passphrase,
                                bool autoconnect,
                                int priority);

    // Delete a saved profile by UUID. Refuses to delete the AP profile.
    void delete_profile(const std::string& uuid);

    // Activate a saved profile by UUID (client or AP). The activation is
    // asynchronous on NM's side; this returns once NM accepts the request.
    void connect_profile(const std::string& uuid);

    // AP (hotspot) configuration.
    // get: {"Configured":bool,"Ssid":str,"Band":"bg"|"a","Channel":u32,
    //       "Autoconnect":bool,"Active":bool,"Ip4Address":str}
    nlohmann::json get_ap();
    // set: creates or updates the AP profile (id kApProfileId). Empty
    // passphrase keeps the existing secret; band "a"/"bg"; channel 0 lets NM
    // pick. enabled maps to autoconnect + immediate activate/deactivate.
    nlohmann::json set_ap(const std::string& ssid,
                          const std::string& passphrase,
                          const std::string& band,
                          std::uint32_t channel,
                          bool enabled);

    // Regulatory country. get returns {"Alpha2":str} ("" if never set).
    // set validates [A-Z]{2}, applies via nl80211 REQ_SET_REG (needs
    // CAP_NET_ADMIN) and persists to <state_dir>/wifi_country so
    // apply_persisted_country() can re-apply it on daemon startup, before
    // any AP activation (some drivers refuse 5 GHz AP init under the
    // WORLD/00 domain — see docs/imate-image-nm-migration.md).
    nlohmann::json get_country();
    void set_country(const std::string& alpha2);
    // Called once at startup; no-op if nothing was persisted. Never throws.
    void apply_persisted_country();

    // The well-known NM connection id used for the hotspot across OpenAstro
    // images (the imager pre-creates it as OpenAstro-<MAC suffix>).
    static constexpr const char* kApProfileId = "OpenAstro-AP";

private:
    struct BusHandle;  // pimpl: keeps sd-bus out of this header

    // Implementation helpers live in wifi_manager.cpp; they assume mutex_ is
    // held and a bus connection is open.
    std::string state_dir_;
    std::mutex mutex_;
    BusHandle* bus_ = nullptr;

    void ensure_bus_locked();
    std::string country_file() const;
};

}  // namespace alpacahttp::util
