// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#include "alpacacore/util/rig_identity.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace alpacacore::rig {

namespace {

std::string hex_only_upper(const std::string& in) {
    std::string out;
    for (unsigned char c : in) {
        if (std::isxdigit(c)) out.push_back(static_cast<char>(std::toupper(c)));
    }
    return out;
}

std::string read_first_line(const std::filesystem::path& path) {
    std::ifstream f(path);
    std::string line;
    if (f && std::getline(f, line)) return line;
    return {};
}

// Last 4 hex digits of an interface's MAC, or "" if unreadable / all-zero.
std::string mac_suffix(const std::filesystem::path& address_file) {
    std::string hex = hex_only_upper(read_first_line(address_file));
    if (hex.size() < 12) return {};
    if (hex.find_first_not_of('0') == std::string::npos) return {};
    return hex.substr(hex.size() - 4);
}

std::string resolve() {
    // 1. explicit override
    if (const char* env = std::getenv("ALPACACORE_RIG_ID")) {
        std::string hex = hex_only_upper(env);
        if (hex.size() >= 4) return hex.substr(hex.size() - 4);
    }

    std::error_code ec;
    const std::filesystem::path net{"/sys/class/net"};

    // 2. wlan0 -- identical to the images' openastro-ssid suffixer
    if (auto s = mac_suffix(net / "wlan0" / "address"); !s.empty()) return s;

    // 3. first other non-loopback interface with a real MAC (sorted so the
    //    choice is stable across boots)
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(net, ec)) {
        auto name = entry.path().filename().string();
        if (name != "lo") names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
        if (auto s = mac_suffix(net / name / "address"); !s.empty()) return s;
    }

    // 4. machine-id
    std::string mid = hex_only_upper(read_first_line("/etc/machine-id"));
    if (mid.size() >= 4) return mid.substr(0, 4);

    // 5. give up but stay well-formed
    return "0000";
}

} // namespace

const std::string& rig_id() {
    static const std::string id = resolve();
    return id;
}

std::string prefix_device_name(const std::string& name) {
    return rig_id() + ": " + name;
}

std::string scope_unique_id(const std::string& unique_id) {
    return unique_id + "-" + rig_id();
}

} // namespace alpacacore::rig
