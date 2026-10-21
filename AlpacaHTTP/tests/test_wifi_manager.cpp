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

#include <alpacahttp/wifi_manager.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "test_assert.h"
namespace {

using alpacahttp::util::WifiError;
using alpacahttp::util::WifiManager;
using alpacahttp::util::detail::WifiDeviceRoles;
using alpacahttp::util::detail::WifiProfileBackend;
using alpacahttp::util::detail::WifiSettings;
using alpacahttp::util::detail::WifiSettingValue;

class FakeWifiProfileBackend final : public WifiProfileBackend {
public:
    enum class Failure { None, Connections, Roles, Add, Update };

    std::vector<Connection> connections;
    WifiDeviceRoles roles{"/client", "wlan0", "/ap", "ap0", 2};
    Failure failure = Failure::None;
    int connection_reads = 0;
    int role_reads = 0;
    int add_calls = 0;
    int update_calls = 0;
    std::string updated_path;
    bool add_pinned = false;
    bool update_pinned = false;
    WifiSettings added_settings;
    WifiSettings updated_settings;

    std::vector<Connection> wifi_connections() override {
        ++connection_reads;
        if (failure == Failure::Connections) throw WifiError("scripted connection-list failure");
        return connections;
    }

    WifiDeviceRoles device_roles() override {
        ++role_reads;
        if (failure == Failure::Roles) throw WifiError("scripted device-role failure");
        return roles;
    }

    void add_connection(const WifiSettings& settings, bool pin_shared_ip4) override {
        ++add_calls;
        add_pinned = pin_shared_ip4;
        added_settings = settings;
        if (failure == Failure::Add) throw WifiError("scripted add failure");
    }

    void update_connection(const std::string& path, const WifiSettings& settings, bool pin_shared_ip4) override {
        ++update_calls;
        updated_path = path;
        update_pinned = pin_shared_ip4;
        updated_settings = settings;
        if (failure == Failure::Update) throw WifiError("scripted update failure");
    }
};

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        auto base = (std::filesystem::temp_directory_path() / "alpacahttp-wifi-test-XXXXXX").string();
        path_.assign(base.begin(), base.end());
        path_.push_back('\0');
        char* result = mkdtemp(path_.data());
        EXPECT(result != nullptr);
        path_.resize(std::char_traits<char>::length(path_.c_str()));
    }

    ~TemporaryDirectory() { std::filesystem::remove_all(path_); }
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

nlohmann::json saved_profile(const std::string& ssid, const std::string& mode = "infrastructure",
                             std::optional<std::string> id = std::string("Saved"), std::string uuid = "uuid-1") {
    nlohmann::json connection = {{"uuid", std::move(uuid)}};
    if (id) connection["id"] = *id;
    return {{"connection", std::move(connection)}, {"802-11-wireless", {{"ssid", ssid}, {"mode", mode}}}};
}

const WifiSettingValue& setting(const WifiSettings& settings, const std::string& section, const std::string& key) {
    const auto section_it =
        std::find_if(settings.begin(), settings.end(), [&](const auto& item) { return item.first == section; });
    EXPECT(section_it != settings.end());
    const auto value_it = std::find_if(section_it->second.begin(), section_it->second.end(),
                                       [&](const auto& item) { return item.first == key; });
    EXPECT(value_it != section_it->second.end());
    return value_it->second;
}

bool has_setting(const WifiSettings& settings, const std::string& section, const std::string& key) {
    const auto section_it =
        std::find_if(settings.begin(), settings.end(), [&](const auto& item) { return item.first == section; });
    return section_it != settings.end() && std::any_of(section_it->second.begin(), section_it->second.end(),
                                                       [&](const auto& item) { return item.first == key; });
}

bool has_section(const WifiSettings& settings, const std::string& section) {
    return std::any_of(settings.begin(), settings.end(), [&](const auto& item) { return item.first == section; });
}

std::string connection_id(const WifiSettings& settings) { return setting(settings, "connection", "id").string_value; }

}  // namespace

int main() {
    {
        // case: WifiManager saves new profile IDs and exact SSID bytes
        const std::vector<std::pair<std::string, std::string>> cases = {{"HomeNet", "HomeNet"},
                                                                        {"\xc3\xa9", "\xc3\xa9"},
                                                                        {std::string("\xff", 1), "Wi-Fi ff"},
                                                                        {std::string("a\0b", 3), "Wi-Fi 610062"},
                                                                        {std::string(32, 'x'), std::string(32, 'x')}};
        for (const auto& [ssid, expected_id] : cases) {
            FakeWifiProfileBackend backend;
            WifiManager manager("unused", backend);
            const auto result = manager.save_profile(ssid, "password1", false, -7);
            EXPECT(backend.connection_reads == 1);
            EXPECT(backend.role_reads == 1);
            EXPECT(backend.add_calls == 1);
            EXPECT(backend.update_calls == 0);
            EXPECT(!backend.add_pinned);
            const auto& id = setting(backend.added_settings, "connection", "id");
            EXPECT(id.type == 's');
            EXPECT(id.string_value == expected_id);
            const auto& connection_type = setting(backend.added_settings, "connection", "type");
            EXPECT(connection_type.type == 's');
            EXPECT(connection_type.string_value == "802-11-wireless");
            const auto& wire_ssid = setting(backend.added_settings, "802-11-wireless", "ssid");
            EXPECT(wire_ssid.type == 'y');
            EXPECT(wire_ssid.bytes == std::vector<std::uint8_t>(ssid.begin(), ssid.end()));
            const auto& mode = setting(backend.added_settings, "802-11-wireless", "mode");
            EXPECT(mode.type == 's');
            EXPECT(mode.string_value == "infrastructure");
            const auto& autoconnect = setting(backend.added_settings, "connection", "autoconnect");
            EXPECT(autoconnect.type == 'b');
            EXPECT(!autoconnect.boolean_value);
            EXPECT(setting(backend.added_settings, "connection", "autoconnect-priority").type == 'i');
            EXPECT(setting(backend.added_settings, "connection", "autoconnect-priority").integer_value == -7);
            EXPECT((result == nlohmann::json{{"Id", expected_id},
                                             {"Ssid", ssid},
                                             {"SsidHex", alpacahttp::util::ssid_to_hex(ssid)},
                                             {"Autoconnect", false},
                                             {"Priority", -7}}));
        }
    }

    {
        // case: WifiManager updates an exact profile and preserves its identity
        FakeWifiProfileBackend backend;
        backend.connections.emplace_back("/saved/ap", saved_profile(std::string("\xff", 1), "ap"));
        backend.connections.emplace_back("/saved/other", saved_profile(std::string("\xfe", 1)));
        backend.connections.emplace_back("/saved/replacement", saved_profile("\xef\xbf\xbd"));
        backend.connections.emplace_back(
            "/saved/exact", saved_profile(std::string("\xff", 1), "infrastructure", "Custom ID", "uuid-custom"));
        WifiManager manager("unused", backend);

        const auto result = manager.save_profile(std::string("\xff", 1), "", true, 4);
        EXPECT(backend.add_calls == 0);
        EXPECT(backend.update_calls == 1);
        EXPECT(backend.updated_path == "/saved/exact");
        EXPECT(!backend.update_pinned);
        const auto& updated_id = setting(backend.updated_settings, "connection", "id");
        EXPECT(updated_id.type == 's');
        EXPECT(updated_id.string_value == "Custom ID");
        const auto& connection_type = setting(backend.updated_settings, "connection", "type");
        EXPECT(connection_type.type == 's');
        EXPECT(connection_type.string_value == "802-11-wireless");
        const auto& autoconnect = setting(backend.updated_settings, "connection", "autoconnect");
        EXPECT(autoconnect.type == 'b');
        EXPECT(autoconnect.boolean_value);
        const auto& priority = setting(backend.updated_settings, "connection", "autoconnect-priority");
        EXPECT(priority.type == 'i');
        EXPECT(priority.integer_value == 4);
        EXPECT(setting(backend.updated_settings, "connection", "uuid").string_value == "uuid-custom");
        EXPECT(setting(backend.updated_settings, "802-11-wireless", "ssid").bytes == std::vector<std::uint8_t>{0xff});
        EXPECT((result == nlohmann::json{{"Id", "Custom ID"},
                                         {"Ssid", std::string("\xff", 1)},
                                         {"SsidHex", "ff"},
                                         {"Autoconnect", true},
                                         {"Priority", 4}}));
    }

    {
        // case: WifiManager supplies a safe ID when an existing profile has none
        FakeWifiProfileBackend backend;
        backend.connections.emplace_back(
            "/saved/no-id", saved_profile(std::string("a\0b", 3), "infrastructure", std::nullopt, "uuid-no-id"));
        WifiManager manager("unused", backend);
        const auto result = manager.save_profile(std::string("a\0b", 3), "", true, 0);
        EXPECT(backend.update_calls == 1);
        EXPECT(connection_id(backend.updated_settings) == "Wi-Fi 610062");
        EXPECT(setting(backend.updated_settings, "connection", "uuid").string_value == "uuid-no-id");
        EXPECT(result["Id"] == "Wi-Fi 610062");
    }

    {
        // case: WifiManager preserves security settings and never returns secrets
        FakeWifiProfileBackend new_open_backend;
        WifiManager new_open_manager("unused", new_open_backend);
        const auto open_result = new_open_manager.save_profile("OpenNet", "", true, 0);
        EXPECT(!has_section(new_open_backend.added_settings, "802-11-wireless-security"));
        EXPECT(!open_result.contains("Passphrase"));

        FakeWifiProfileBackend existing_backend;
        auto secured = saved_profile("SecureNet");
        secured["802-11-wireless-security"] = {{"key-mgmt", "wpa-psk"}};
        existing_backend.connections.emplace_back("/saved/secured", std::move(secured));
        WifiManager existing_manager("unused", existing_backend);
        const auto retained_result = existing_manager.save_profile("SecureNet", "", true, 0);
        const auto& key_mgmt = setting(existing_backend.updated_settings, "802-11-wireless-security", "key-mgmt");
        EXPECT(key_mgmt.type == 's');
        EXPECT(key_mgmt.string_value == "wpa-psk");
        EXPECT(!has_setting(existing_backend.updated_settings, "802-11-wireless-security", "psk"));
        EXPECT(!retained_result.contains("Passphrase"));

        FakeWifiProfileBackend password_backend;
        WifiManager password_manager("unused", password_backend);
        const auto password_result = password_manager.save_profile("SecureNet", "password1", true, 0);
        const auto& connection_type = setting(password_backend.added_settings, "connection", "type");
        EXPECT(connection_type.type == 's');
        EXPECT(connection_type.string_value == "802-11-wireless");
        const auto& key_mgmt_new = setting(password_backend.added_settings, "802-11-wireless-security", "key-mgmt");
        EXPECT(key_mgmt_new.type == 's');
        EXPECT(key_mgmt_new.string_value == "wpa-psk");
        const auto& psk = setting(password_backend.added_settings, "802-11-wireless-security", "psk");
        EXPECT(psk.type == 's');
        EXPECT(psk.string_value == "password1");
        EXPECT(!password_result.contains("Passphrase"));

        FakeWifiProfileBackend replacement_backend;
        auto replacement = saved_profile("SecureNet", "infrastructure", "Saved SecureNet", "uuid-secure");
        replacement["802-11-wireless-security"] = {{"key-mgmt", "wpa-psk"}};
        replacement_backend.connections.emplace_back("/saved/secure", std::move(replacement));
        WifiManager replacement_manager("unused", replacement_backend);
        const auto replacement_result = replacement_manager.save_profile("SecureNet", "newpassword1", true, 3);
        EXPECT(replacement_backend.add_calls == 0);
        EXPECT(replacement_backend.update_calls == 1);
        EXPECT(replacement_backend.updated_path == "/saved/secure");
        EXPECT(connection_id(replacement_backend.updated_settings) == "Saved SecureNet");
        EXPECT(setting(replacement_backend.updated_settings, "connection", "uuid").string_value == "uuid-secure");
        const auto& replacement_key_mgmt =
            setting(replacement_backend.updated_settings, "802-11-wireless-security", "key-mgmt");
        EXPECT(replacement_key_mgmt.type == 's');
        EXPECT(replacement_key_mgmt.string_value == "wpa-psk");
        const auto& replacement_psk = setting(replacement_backend.updated_settings, "802-11-wireless-security", "psk");
        EXPECT(replacement_psk.type == 's');
        EXPECT(replacement_psk.string_value == "newpassword1");
        EXPECT(!replacement_result.contains("Passphrase"));
    }

    {
        // case: WifiManager pins dual-interface profiles to the client interface
        FakeWifiProfileBackend dual;
        dual.roles = {"/client", "wlan0", "/ap", "ap0", 2};
        WifiManager dual_manager("unused", dual);
        dual_manager.save_profile("Dual", "", true, 0);
        const auto& interface_name = setting(dual.added_settings, "connection", "interface-name");
        EXPECT(interface_name.type == 's');
        EXPECT(interface_name.string_value == "wlan0");

        FakeWifiProfileBackend single;
        single.roles = {"/client", "wlan0", "/client", "wlan0", 1};
        WifiManager single_manager("unused", single);
        single_manager.save_profile("Single", "", true, 0);
        EXPECT(!has_setting(single.added_settings, "connection", "interface-name"));

        FakeWifiProfileBackend missing_interface;
        missing_interface.roles = {"/client", "", "/ap", "ap0", 2};
        WifiManager missing_manager("unused", missing_interface);
        missing_manager.save_profile("NoInterface", "", true, 0);
        EXPECT(!has_setting(missing_interface.added_settings, "connection", "interface-name"));
    }

    {
        // case: WifiManager validates profile inputs before backend access
        struct InvalidProfile {
            std::string ssid;
            std::string passphrase;
            std::string expected_error;
        };
        for (const auto& invalid : std::vector<InvalidProfile>{
                 {"", "", "Ssid must be 1-32 bytes"},
                 {std::string(33, 'x'), "", "Ssid must be 1-32 bytes"},
                 {"Home", "1234567", "Passphrase must be 8-63 characters (or empty for open networks)"},
                 {"Home", std::string(64, 'p'), "Passphrase must be 8-63 characters (or empty for open networks)"}}) {
            FakeWifiProfileBackend backend;
            WifiManager manager("unused", backend);
            bool rejected = false;
            try {
                manager.save_profile(invalid.ssid, invalid.passphrase, true, 0);
            } catch (const WifiError& error) {
                rejected = true;
                EXPECT(std::string(error.what()) == invalid.expected_error);
            }
            EXPECT(rejected);
            EXPECT(backend.connection_reads == 0);
            EXPECT(backend.role_reads == 0);
            EXPECT(backend.add_calls == 0);
            EXPECT(backend.update_calls == 0);
        }
        FakeWifiProfileBackend backend;
        WifiManager manager("unused", backend);
        manager.save_profile(std::string(32, 'x'), std::string(63, 'p'), true, 0);
        EXPECT(backend.add_calls == 1);
    }

    {
        // case: WifiManager propagates profile backend failures without success
        for (const auto failure : {FakeWifiProfileBackend::Failure::Connections, FakeWifiProfileBackend::Failure::Roles,
                                   FakeWifiProfileBackend::Failure::Add}) {
            FakeWifiProfileBackend backend;
            backend.failure = failure;
            WifiManager manager("unused", backend);
            bool rejected = false;
            try {
                manager.save_profile("New", "", true, 0);
            } catch (const WifiError&) {
                rejected = true;
            }
            EXPECT(rejected);
            EXPECT(backend.add_calls == (failure == FakeWifiProfileBackend::Failure::Add ? 1 : 0));
            EXPECT(backend.connection_reads == 1);
            EXPECT(backend.role_reads == (failure == FakeWifiProfileBackend::Failure::Connections ? 0 : 1));
            EXPECT(backend.update_calls == 0);
        }
        FakeWifiProfileBackend backend;
        backend.connections.emplace_back("/saved", saved_profile("Existing"));
        backend.failure = FakeWifiProfileBackend::Failure::Update;
        WifiManager manager("unused", backend);
        bool rejected = false;
        try {
            manager.save_profile("Existing", "", true, 0);
        } catch (const WifiError&) {
            rejected = true;
        }
        EXPECT(rejected);
        EXPECT(backend.add_calls == 0);
        EXPECT(backend.update_calls == 1);
    }

    {
        // case: WifiManager country guard cannot reach NetworkManager in injected mode
        TemporaryDirectory dir;
        FakeWifiProfileBackend backend;
        WifiManager manager(dir.path(), backend);
        bool country_required = false;
        try {
            manager.set_ap("TestAP", "password1", "a", 0, true);
        } catch (const WifiError& error) {
            country_required = std::string(error.what()).find("regulatory country") != std::string::npos;
        }
        EXPECT(country_required);

        {
            std::ofstream country(std::filesystem::path(dir.path()) / "wifi_country");
            country << "US\n";
        }
        bool injected_operation_refused = false;
        try {
            manager.set_ap("TestAP", "password1", "a", 0, true);
        } catch (const WifiError& error) {
            injected_operation_refused =
                std::string(error.what()).find("injected Wi-Fi profile backend") != std::string::npos;
        }
        EXPECT(injected_operation_refused);
        EXPECT(backend.connection_reads == 0);
        EXPECT(backend.role_reads == 0);
        EXPECT(backend.add_calls == 0);
        EXPECT(backend.update_calls == 0);
    }

    std::cout << "Wi-Fi profile backend tests passed\n";
    return 0;
}
