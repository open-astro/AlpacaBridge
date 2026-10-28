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

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace alpacacore::util {

namespace {

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

std::map<std::string, double> read_flat_number_file(const std::string& path, const char* log_category,
                                                    const char* what) {
    std::map<std::string, double> m;
    std::error_code ec;
    const bool present = std::filesystem::exists(path, ec);
    std::ifstream in(path);
    if (!in) {
        if (present) {
            ALPACA_LOG_WARN(log_category,
                            std::string("Could not read ") + what + " from " + path + "; treating as empty");
        }
        return m;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (!parse(ss.str(), m)) {
        ALPACA_LOG_WARN(log_category,
                        std::string("The ") + what + " file " + path + " is not valid; treating as empty");
    }
    return m;
}

void write_flat_number_file(const std::string& path, const std::map<std::string, double>& m) {
    const std::filesystem::path p(path);
    std::error_code ec;
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
    }
    char num[64];
    std::ostringstream out;
    out << "{\n";
    bool first = true;
    for (const auto& [k, v] : m) {
        std::snprintf(num, sizeof(num), "%.17g", v);
        out << (first ? "" : ",\n") << "  \"" << escape(k) << "\": " << num;
        first = false;
    }
    out << "\n}\n";
    write_file_atomic(p, out.str());
}

}  // namespace alpacacore::util
