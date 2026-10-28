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

#include <alpacacore/util/atomic_file.h>
#include <alpacacore/util/device_location_store.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/serial_by_id_scan.h>
#include <alpacacore/util/serial_port_registry.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace alpacacore::util {

namespace {

constexpr const char* kTag = "DeviceLocation";

std::mutex& store_mutex() {
    static std::mutex m;
    return m;
}

std::string& path_override() {
    static std::string p;
    return p;
}

std::string effective_path_locked() {
    return path_override().empty() ? DeviceLocationStore::default_path() : path_override();
}

// Parses a JSON string literal starting at text[i] == '"'; leaves i past the closing quote.
bool parse_string(const std::string& text, std::size_t& i, std::string& out) {
    out.clear();
    if (i >= text.size() || text[i] != '"') return false;
    ++i;
    while (i < text.size() && text[i] != '"') {
        if (text[i] == '\\' && i + 1 < text.size()) ++i;
        out += text[i++];
    }
    if (i >= text.size()) return false;
    ++i;
    return true;
}

// Parses {"key": "value", ...}. Returns false (and an empty map) on anything else.
bool parse(const std::string& text, std::map<std::string, std::string>& out) {
    out.clear();
    std::size_t i = 0;
    auto skip = [&] {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) ++i;
    };
    skip();
    if (i >= text.size() || text[i] != '{') return false;
    ++i;
    skip();
    if (i < text.size() && text[i] == '}') return true;
    while (i < text.size()) {
        skip();
        std::string key;
        std::string value;
        if (!parse_string(text, i, key)) break;
        skip();
        if (i >= text.size() || text[i] != ':') break;
        ++i;
        skip();
        if (!parse_string(text, i, value)) break;
        out[key] = value;
        skip();
        if (i < text.size() && text[i] == ',') {
            ++i;
            continue;
        }
        if (i < text.size() && text[i] == '}') return true;
        break;
    }
    out.clear();
    return false;
}

std::map<std::string, std::string> read_all(const std::string& path) {
    std::map<std::string, std::string> m;
    std::error_code ec;
    const bool present = std::filesystem::exists(path, ec);
    std::ifstream in(path);
    if (!in) {
        if (present) {
            ALPACA_LOG_WARN(kTag, "Could not read remembered device locations from " + path + "; treating as empty");
        }
        return m;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (!parse(ss.str(), m)) {
        ALPACA_LOG_WARN(kTag, "Device location file " + path + " is not valid; treating as empty");
    }
    return m;
}

std::string escape(const std::string& s) {
    std::string r;
    for (const char c : s) {
        if (c == '"' || c == '\\') r += '\\';
        r += c;
    }
    return r;
}

// Writes @p m to @p path (write_file_atomic). Throws on failure. Caller holds store_mutex().
void write_all_locked(const std::string& path, const std::map<std::string, std::string>& m) {
    const std::filesystem::path p(path);
    std::error_code ec;
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
    }
    std::ostringstream out;
    out << "{\n";
    bool first = true;
    for (const auto& [k, v] : m) {
        out << (first ? "" : ",\n") << "  \"" << escape(k) << "\": \"" << escape(v) << "\"";
        first = false;
    }
    out << "\n}\n";
    write_file_atomic(p, out.str());
}

}  // namespace

std::string DeviceLocationStore::default_path() { return "config/device_locations.json"; }

std::string DeviceLocationStore::set_default_path(const std::string& path) {
    std::lock_guard<std::mutex> lock(store_mutex());
    std::string previous = path_override();
    path_override() = path;
    return previous;
}

std::optional<std::string> DeviceLocationStore::load(const std::string& key) {
    std::lock_guard<std::mutex> lock(store_mutex());
    try {
        const auto m = read_all(effective_path_locked());
        const auto it = m.find(key);
        if (it == m.end() || it->second.empty()) return std::nullopt;
        return it->second;
    } catch (...) {
        return std::nullopt;
    }
}

bool DeviceLocationStore::save(const std::string& key, const std::string& location) {
    if (key.empty() || location.empty()) return false;
    std::lock_guard<std::mutex> lock(store_mutex());
    const std::string path = effective_path_locked();
    try {
        auto m = read_all(path);
        const auto it = m.find(key);
        if (it != m.end() && it->second == location) return true;
        m[key] = location;
        write_all_locked(path, m);
        return true;
    } catch (const std::exception& e) {
        ALPACA_LOG_WARN(kTag, std::string("Could not remember the device location in ") + path + ": " + e.what());
        return false;
    } catch (...) {
        return false;
    }
}

std::string location_key(const std::string& unique_id, int index) { return unique_id + "#" + std::to_string(index); }

std::string serial_location_for(const std::string& port_path, const std::string& by_id_dir) {
    std::error_code ec;
    const std::filesystem::path dir(by_id_dir);
    if (!path_exists(dir)) return port_path;
    const std::string target = canonical_serial_path(port_path);
    for (const auto& sym : list_serial_by_id(dir)) {
        const auto resolved = std::filesystem::canonical(sym.path, ec);
        if (!ec && resolved.string() == target) return sym.path.string();
        ec.clear();
    }
    return port_path;
}

std::string network_location_for(const std::string& host, int tcp_port) {
    return host + ":" + std::to_string(tcp_port);
}

bool parse_network_location(const std::string& location, std::string& host, int& tcp_port) {
    const auto colon = location.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= location.size() || location.front() == '/') {
        return false;
    }
    int port = 0;
    for (std::size_t i = colon + 1; i < location.size(); ++i) {
        if (location[i] < '0' || location[i] > '9') return false;
        port = port * 10 + (location[i] - '0');
        if (port > 65535) return false;
    }
    if (port < 1) return false;
    host = location.substr(0, colon);
    tcp_port = port;
    return true;
}

std::optional<std::string> recall_serial_location(const std::string& key,
                                                  const std::function<bool(const std::string&)>& handshake) {
    auto remembered = DeviceLocationStore::load(key);
    if (!remembered || remembered->front() != '/') return std::nullopt;
    const auto holder = serial_port_holder(*remembered);
    if (holder && holder->kind == SerialClaimKind::Link) {
        ALPACA_LOG_INFO(kTag, "Remembered port " + *remembered + " is held by " + holder->owner + "; searching");
        return std::nullopt;
    }
    if (!handshake(*remembered)) {
        ALPACA_LOG_INFO(kTag, "Remembered port " + *remembered + " did not answer; searching");
        return std::nullopt;
    }
    return remembered;
}

}  // namespace alpacacore::util
