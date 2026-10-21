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

#include <alpacacore/util/logging.h>
#include <alpacacore/util/rotator_sync_offset_store.h>
#include <alpacacore/util/unique_id.h>

#include <cmath>
#include <cstdio>
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

std::mutex& store_mutex() {
    static std::mutex m;
    return m;
}

std::function<void()>& after_save_hook() {
    static std::function<void()> hook;
    return hook;
}

std::string& path_override() {
    static std::string p;
    return p;
}

std::string effective_path_locked() {
    return path_override().empty() ? RotatorSyncOffsetStore::default_path() : path_override();
}

// Parses {"key": number, ...}. Returns false (and an empty map) on anything else.
bool parse(const std::string& text, std::map<std::string, double>& out) {
    out.clear();
    std::size_t i = 0;
    auto skip = [&] {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) {
            ++i;
        }
    };
    skip();
    if (i >= text.size() || text[i] != '{') {
        return false;
    }
    ++i;
    skip();
    if (i < text.size() && text[i] == '}') {
        return true;
    }
    while (i < text.size()) {
        skip();
        if (i >= text.size() || text[i] != '"') {
            out.clear();
            return false;
        }
        ++i;
        std::string key;
        while (i < text.size() && text[i] != '"') {
            if (text[i] == '\\' && i + 1 < text.size()) {
                ++i;
            }
            key += text[i++];
        }
        if (i >= text.size()) {
            out.clear();
            return false;
        }
        ++i;
        skip();
        if (i >= text.size() || text[i] != ':') {
            out.clear();
            return false;
        }
        ++i;
        skip();
        const char* start = text.c_str() + i;
        char* end = nullptr;
        const double v = std::strtod(start, &end);
        if (end == start || !std::isfinite(v)) {
            out.clear();
            return false;
        }
        i += static_cast<std::size_t>(end - start);
        out[key] = v;
        skip();
        if (i < text.size() && text[i] == ',') {
            ++i;
            continue;
        }
        if (i < text.size() && text[i] == '}') {
            return true;
        }
        break;
    }
    out.clear();
    return false;
}

std::map<std::string, double> read_all(const std::string& path) {
    std::map<std::string, double> m;
    std::error_code ec;
    const bool present = std::filesystem::exists(path, ec);
    std::ifstream in(path);
    if (!in) {
        if (present) {
            ALPACA_LOG_WARN("RotatorSyncOffset", "Could not read sync offsets from " + path + "; treating as empty");
        }
        return m;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (!parse(ss.str(), m)) {
        ALPACA_LOG_WARN("RotatorSyncOffset", "Sync offset file " + path + " is not valid; treating as empty");
    }
    return m;
}

std::string escape(const std::string& s) {
    std::string r;
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            r += '\\';
        }
        r += c;
    }
    return r;
}

}  // namespace

std::string RotatorSyncOffsetStore::default_path() { return "config/rotator_sync_offsets.json"; }

std::string RotatorSyncOffsetStore::set_default_path(const std::string& path) {
    std::lock_guard<std::mutex> lock(store_mutex());
    std::string previous = path_override();
    path_override() = path;
    return previous;
}

double RotatorSyncOffsetStore::load(const std::string& key) {
    std::lock_guard<std::mutex> lock(store_mutex());
    try {
        const auto m = read_all(effective_path_locked());
        const auto it = m.find(key);
        return it == m.end() ? 0.0 : it->second;
    } catch (...) {
        return 0.0;
    }
}

namespace {

// Writes @p m to @p path (temp file + rename). Throws on failure. Caller holds store_mutex().
void write_all_locked(const std::string& path, const std::map<std::string, double>& m) {
    const std::filesystem::path p(path);
    std::error_code ec;
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
    }
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot open " + tmp);
        }
        char num[64];
        out << "{\n";
        bool first = true;
        for (const auto& [k, v] : m) {
            std::snprintf(num, sizeof(num), "%.17g", v);
            out << (first ? "" : ",\n") << "  \"" << escape(k) << "\": " << num;
            first = false;
        }
        out << "\n}\n";
        out.flush();
        if (!out) {
            throw std::runtime_error("write failed for " + tmp);
        }
    }
    std::filesystem::rename(tmp, p);
}

// save() without the test hook.
bool save_entry(const std::string& key, double offset_degrees) {
    std::lock_guard<std::mutex> lock(store_mutex());
    const std::string path = effective_path_locked();
    if (!std::isfinite(offset_degrees)) {
        ALPACA_LOG_WARN("RotatorSyncOffset", "Refusing to save a non-finite sync offset to " + path);
        return false;
    }
    try {
        auto m = read_all(path);
        m[key] = offset_degrees;
        write_all_locked(path, m);
        return true;
    } catch (const std::exception& e) {
        ALPACA_LOG_WARN("RotatorSyncOffset", std::string("Could not save sync offset to ") + path + ": " + e.what());
        return false;
    } catch (...) {
        return false;
    }
}

}  // namespace

void RotatorSyncOffsetStore::set_after_save_hook_for_testing(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(store_mutex());
    after_save_hook() = std::move(hook);
}

bool RotatorSyncOffsetStore::save(const std::string& key, double offset_degrees) {
    const bool saved = save_entry(key, offset_degrees);
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lock(store_mutex());
        hook = after_save_hook();
    }
    if (hook) hook();
    return saved;
}

double RotatorSyncOffsetStore::load_or_adopt(const std::string& key, const std::string& legacy_key) {
    if (legacy_key.empty() || legacy_key == key) {
        return load(key);
    }
    std::lock_guard<std::mutex> lock(store_mutex());
    const std::string path = effective_path_locked();
    try {
        auto m = read_all(path);
        if (const auto it = m.find(key); it != m.end()) {
            return it->second;
        }
        const auto legacy = m.find(legacy_key);
        if (legacy == m.end()) {
            return 0.0;
        }
        const double offset = legacy->second;
        m[key] = offset;
        m.erase(legacy_key);
        try {
            write_all_locked(path, m);
            ALPACA_LOG_INFO("RotatorSyncOffset", "Moved the sync offset of " + legacy_key + " to " + key);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("RotatorSyncOffset",
                            "Could not move the sync offset of " + legacy_key + " to " + key + ": " + e.what());
        }
        return offset;
    } catch (...) {
        return 0.0;
    }
}

std::size_t RotatorSyncOffsetStore::prune_unknown_unique_ids(const std::set<std::string>& live_unique_ids) {
    std::lock_guard<std::mutex> lock(store_mutex());
    const std::string path = effective_path_locked();
    try {
        auto m = read_all(path);
        std::size_t removed = 0;
        for (auto it = m.begin(); it != m.end();) {
            if (is_unique_id(it->first) && live_unique_ids.count(it->first) == 0) {
                it = m.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        if (removed > 0) {
            write_all_locked(path, m);
        }
        return removed;
    } catch (const std::exception& e) {
        ALPACA_LOG_WARN("RotatorSyncOffset", std::string("Could not prune sync offsets in ") + path + ": " + e.what());
        return 0;
    } catch (...) {
        return 0;
    }
}

}  // namespace alpacacore::util
