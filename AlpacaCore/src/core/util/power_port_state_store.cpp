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
#include <alpacacore/util/flat_number_file.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/power_port_state_store.h>
#include <alpacacore/util/unique_id.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <system_error>

namespace alpacacore::util {

namespace {

constexpr const char* kTag = "PowerPortState";
constexpr const char* kKernelBootIdPath = "/proc/sys/kernel/random/boot_id";

std::mutex& store_mutex() {
    static std::mutex m;
    return m;
}

std::string& path_override() {
    static std::string p;
    return p;
}

std::string& boot_id_path_override() {
    static std::string p;
    return p;
}

std::string& boot_id_override() {
    static std::string id;
    return id;
}

std::string effective_path_locked() {
    return path_override().empty() ? PowerPortStateStore::default_path() : path_override();
}

std::string effective_boot_id_path_locked() {
    return boot_id_path_override().empty() ? PowerPortStateStore::default_boot_id_path() : boot_id_path_override();
}

std::string replace(std::string& slot, const std::string& value) {
    std::string previous = slot;
    slot = value;
    return previous;
}

std::map<std::string, double> read_all(const std::string& path) {
    return read_flat_number_file(path, kTag, "power port states");
}

std::string make_key(const std::string& unique_id, int port) { return unique_id + "/" + std::to_string(port); }

// Splits "<UniqueID>/<port>" at the last '/'. False when the key has no such shape.
bool split_key(const std::string& key, std::string& unique_id, int& port) {
    const auto slash = key.rfind('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= key.size()) {
        return false;
    }
    const std::string digits = key.substr(slash + 1);
    if (!std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }) || digits.size() > 4) {
        return false;
    }
    unique_id = key.substr(0, slash);
    port = std::stoi(digits);
    return true;
}

// First line of @p path without surrounding whitespace; empty when unreadable.
std::string read_first_line(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    if (!in || !std::getline(in, line)) {
        return {};
    }
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = line.find_last_not_of(" \t\r\n");
    return line.substr(first, last - first + 1);
}

}  // namespace

std::optional<PowerOnState> parse_power_on_state(std::string_view name) {
    if (name == "leave") return PowerOnState::Leave;
    if (name == "restore") return PowerOnState::Restore;
    if (name == "off") return PowerOnState::Off;
    return std::nullopt;
}

std::vector<std::optional<int>> plan_power_on(PowerOnState state, bool new_boot, const std::vector<PowerOnPort>& ports,
                                              const std::map<int, double>& saved_percent) {
    std::vector<std::optional<int>> plan(ports.size());
    if (state == PowerOnState::Leave) {
        return plan;
    }
    for (std::size_t i = 0; i < ports.size(); ++i) {
        if (!ports[i].writable) {
            continue;
        }
        if (state == PowerOnState::Off && new_boot) {
            plan[i] = 0;
            continue;
        }
        const auto it = saved_percent.find(static_cast<int>(i));
        if (it == saved_percent.end() || !std::isfinite(it->second)) {
            continue;
        }
        const double percent = std::clamp(it->second, 0.0, 100.0);
        plan[i] = ports[i].pwm ? static_cast<int>(std::lround(percent)) : (percent > 0.0 ? 1 : 0);
    }
    return plan;
}

std::string PowerPortStateStore::default_path() { return "config/power_port_states.json"; }

std::string PowerPortStateStore::default_boot_id_path() { return "config/power_port_boot_id"; }

std::string PowerPortStateStore::set_default_path(const std::string& path) {
    std::lock_guard<std::mutex> lock(store_mutex());
    return replace(path_override(), path);
}

std::string PowerPortStateStore::set_boot_id_path(const std::string& path) {
    std::lock_guard<std::mutex> lock(store_mutex());
    return replace(boot_id_path_override(), path);
}

std::string PowerPortStateStore::set_current_boot_id_for_testing(const std::string& boot_id) {
    std::lock_guard<std::mutex> lock(store_mutex());
    return replace(boot_id_override(), boot_id);
}

std::map<int, double> PowerPortStateStore::load(const std::string& unique_id) {
    std::lock_guard<std::mutex> lock(store_mutex());
    std::map<int, double> ports;
    try {
        for (const auto& [key, value] : read_all(effective_path_locked())) {
            std::string id;
            int port = 0;
            if (split_key(key, id, port) && id == unique_id) {
                ports[port] = value;
            }
        }
    } catch (...) {
        ports.clear();
    }
    return ports;
}

bool PowerPortStateStore::save(const std::string& unique_id, int port, double percent) {
    std::lock_guard<std::mutex> lock(store_mutex());
    const std::string path = effective_path_locked();
    if (!std::isfinite(percent) || port < 0) {
        ALPACA_LOG_WARN(kTag, "Refusing to save an invalid power port state to " + path);
        return false;
    }
    try {
        auto m = read_all(path);
        m[make_key(unique_id, port)] = std::clamp(percent, 0.0, 100.0);
        write_flat_number_file(path, m);
        return true;
    } catch (const std::exception& e) {
        ALPACA_LOG_WARN(kTag, std::string("Could not save the power port state to ") + path + ": " + e.what());
        return false;
    } catch (...) {
        return false;
    }
}

std::size_t PowerPortStateStore::prune_unknown_unique_ids(const std::set<std::string>& live_unique_ids) {
    std::lock_guard<std::mutex> lock(store_mutex());
    const std::string path = effective_path_locked();
    try {
        auto m = read_all(path);
        std::size_t removed = 0;
        for (auto it = m.begin(); it != m.end();) {
            std::string id;
            int port = 0;
            if (split_key(it->first, id, port) && is_unique_id(id) && live_unique_ids.count(id) == 0) {
                it = m.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        if (removed > 0) {
            write_flat_number_file(path, m);
        }
        return removed;
    } catch (const std::exception& e) {
        ALPACA_LOG_WARN(kTag, std::string("Could not prune power port states in ") + path + ": " + e.what());
        return 0;
    } catch (...) {
        return 0;
    }
}

bool PowerPortStateStore::begin_server_start() {
    std::lock_guard<std::mutex> lock(store_mutex());
    const std::string current = boot_id_override().empty() ? read_first_line(kKernelBootIdPath) : boot_id_override();
    if (current.empty()) {
        ALPACA_LOG_WARN(kTag, std::string("Could not read the boot ID from ") + kKernelBootIdPath +
                                  "; treating this start as a service restart");
        return false;
    }
    const std::string path = effective_boot_id_path_locked();
    const bool new_boot = read_first_line(path) != current;
    if (!new_boot) {
        return false;
    }
    try {
        const std::filesystem::path p(path);
        std::error_code ec;
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path(), ec);
        }
        write_file_atomic(p, current + "\n");
    } catch (const std::exception& e) {
        // The next start will see a new boot again and apply "off" once more.
        ALPACA_LOG_WARN(kTag, std::string("Could not record the boot ID in ") + path + ": " + e.what());
    }
    return true;
}

}  // namespace alpacacore::util
