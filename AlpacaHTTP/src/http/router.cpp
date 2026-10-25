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

#include <alpacacore/alpaca_defs.h>
#include <alpacacore/camera_driver.h>
#include <alpacacore/catalog/builtin_catalog.h>
#include <alpacacore/device_registry.h>
#include <alpacacore/filterwheel_driver.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/alignment_mode.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/hardware_config_refusal.h>
#include <alpacacore/util/image_validation.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/rotator_sync_offset_store.h>
#include <alpacacore/util/serial_io.h>
#include <alpacacore/util/unique_id.h>
#include <alpacahttp/config.h>
#include <alpacahttp/json_utils.h>
#include <alpacahttp/router.h>
#include <alpacahttp/util/error_mapping.h>
#include <alpacahttp/util/host_timezone.h>
#include <alpacahttp/util/log_text.h>
#include <alpacahttp/util/logging_adapter.h>
#include <alpacahttp/util/yaml_comment.h>
#include <alpacahttp/version.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "../core/catalog_json.h"
#include "device_call.h"
#include "image_payload.h"

namespace {

using alpacacore::logging::LogLevel;
using alpacahttp::find_json_value;
using alpacahttp::get_form_value;
using alpacahttp::to_lower_copy;

const std::filesystem::path kPersistedDevicesFile = std::filesystem::path("config") / "registered_devices.json";

const std::array<std::pair<const char*, LogLevel>, 6> kLogLevelMap = {{
    {"TRACE", LogLevel::Trace},
    {"DEBUG", LogLevel::Debug},
    {"INFO", LogLevel::Info},
    {"WARN", LogLevel::Warn},
    {"WARNING", LogLevel::Warn},
    {"ERROR", LogLevel::Error},
}};

std::string normalize_level_string(std::string level) {
    std::transform(level.begin(), level.end(), level.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return level;
}

std::string log_level_to_string(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARNING";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Critical: return "CRITICAL";
        default: return "INFO";
    }
}

// Gzip-compress a buffer (RFC 1952 framing via zlib windowBits 15+16).
// Throws std::runtime_error on any zlib failure.
std::string gzip_compress(const std::string& input) {
    // zlib's avail_in/avail_out are 32-bit; refuse rather than truncate.
    if (input.size() >= std::numeric_limits<uInt>::max() / 2) {
        throw std::runtime_error("Input too large to gzip in one pass");
    }

    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("deflateInit2 failed");
    }
    struct ZStreamGuard {
        z_stream* stream;
        ~ZStreamGuard() { deflateEnd(stream); }
    } guard{&stream};

    std::string output;
    // The input-size guard above keeps deflateBound's result (slightly larger
    // than the input) within uInt range, so both avail casts below are safe.
    // Tighten one only together with the other.
    output.resize(deflateBound(&stream, static_cast<uLong>(input.size())));
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());

    const int rc = deflate(&stream, Z_FINISH);
    if (rc != Z_STREAM_END) {
        throw std::runtime_error("deflate failed (rc=" + std::to_string(rc) + ")");
    }
    output.resize(stream.total_out);
    return output;
}

// Escape a value for a double-quoted scalar in the hand-rolled config
// writer. Config::unquote_string() reverses these escapes on load, so the
// two must stay symmetric. Newlines and other control characters must never
// reach the file verbatim - the line-based reader would misparse them as
// new config entries.
std::string escape_yaml_string(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(ch) >= 0x20) {
                    escaped.push_back(ch);
                }
                // Other control characters are dropped - they have no
                // meaning in a friendly name or location string.
                break;
        }
    }
    return escaped;
}

using ConfigKeyValues = std::vector<std::pair<std::string, std::string>>;
// One top-level section and the keys to set under it, e.g. {"http", {...}}.
using ConfigSectionValues = std::pair<std::string, ConfigKeyValues>;

// Update keys under one or more top-level sections of the config file (the
// `server:`, `http:` and `discovery:` ones) in a single read/rewrite pass, so a request that
// sets several values can never leave the file with only some of them
// applied. An existing key is replaced in place, a missing key is appended to
// its section, and a missing section is appended to the file.
bool update_config_values(const std::string& config_path, const std::vector<ConfigSectionValues>& sections,
                          std::string& error_message) {
    if (config_path.empty()) {
        error_message = "Config path not set";
        return false;
    }
    const bool any_values =
        std::any_of(sections.begin(), sections.end(), [](const auto& section) { return !section.second.empty(); });
    if (!any_values) {
        return true;
    }

    std::ifstream input(config_path);
    if (!input.is_open()) {
        std::ofstream output(config_path, std::ios::trunc);
        if (!output.is_open()) {
            error_message = "Unable to open config file for writing";
            return false;
        }
        for (const auto& [section, values] : sections) {
            if (values.empty()) {
                continue;
            }
            output << section << ":\n";
            for (const auto& [key, value] : values) {
                output << "  " << key << ": \"" << escape_yaml_string(value) << "\"\n";
            }
        }
        return true;
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }

    auto leading_spaces = [](const std::string& text) {
        std::size_t count = 0;
        while (count < text.size() && text[count] == ' ') {
            ++count;
        }
        return count;
    };

    auto trim_copy = [](std::string_view value) {
        std::size_t start = 0;
        std::size_t end = value.size();
        while (start < end && std::isspace(static_cast<unsigned char>(value[start]))) {
            ++start;
        }
        while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
            --end;
        }
        return std::string(value.substr(start, end - start));
    };

    // Index into `sections` of the section the current line sits in, or
    // sections.size() outside every section we edit.
    std::size_t current = sections.size();
    std::vector<bool> section_found(sections.size(), false);
    std::vector<std::vector<bool>> written;
    written.reserve(sections.size());
    for (const auto& section : sections) {
        written.emplace_back(section.second.size(), false);
    }
    std::vector<std::string> output;
    output.reserve(lines.size() + 8);

    // Output index just past the last content line of the current section, so
    // keys added at the section's end land before its trailing blank and
    // comment lines.
    std::size_t section_end = 0;

    auto append_unwritten = [&](std::size_t section_index, std::size_t indent) {
        const auto& values = sections[section_index].second;
        std::vector<std::string> added;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (!written[section_index][i]) {
                added.push_back(std::string(indent, ' ') + values[i].first + ": \"" +
                                escape_yaml_string(values[i].second) + "\"");
                written[section_index][i] = true;
            }
        }
        if (added.empty()) {
            return;
        }
        output.insert(output.begin() + static_cast<std::ptrdiff_t>(std::min(section_end, output.size())), added.begin(),
                      added.end());
    };

    for (const auto& current_line : lines) {
        std::string stripped_comment = alpacahttp::util::strip_yaml_comment(current_line);
        std::string trimmed = trim_copy(stripped_comment);
        std::size_t indent = leading_spaces(current_line);

        // Only a top-level key ends a section; blank and comment lines at
        // column 0 sit inside it (a hand-edited file may leave a gap).
        if (indent == 0 && !(current < sections.size() && trimmed.empty())) {
            if (current < sections.size()) {
                append_unwritten(current, 2);
            }
            current = sections.size();
            for (std::size_t i = 0; i < sections.size(); ++i) {
                if (trimmed == sections[i].first + ":") {
                    current = i;
                    section_found[i] = true;
                    break;
                }
            }
            output.push_back(current_line);
            section_end = output.size();
            continue;
        }

        bool replaced = false;
        if (current < sections.size() && !trimmed.empty()) {
            auto delimiter = trimmed.find(':');
            if (delimiter != std::string::npos) {
                std::string key = trim_copy(trimmed.substr(0, delimiter));
                const auto& values = sections[current].second;
                for (std::size_t i = 0; i < values.size() && !replaced; ++i) {
                    if (values[i].first == key) {
                        // Keep the old line's trailing comment, with the
                        // whitespace that sat before the '#'.
                        std::string replacement(indent, ' ');
                        replacement += key;
                        replacement += ": \"";
                        replacement += escape_yaml_string(values[i].second);
                        replacement += '"';
                        if (stripped_comment.size() < current_line.size()) {
                            std::size_t gap = stripped_comment.size();
                            while (gap > 0 && std::isspace(static_cast<unsigned char>(stripped_comment[gap - 1]))) {
                                --gap;
                            }
                            replacement.append(stripped_comment, gap, std::string::npos);
                            replacement.append(current_line, stripped_comment.size(), std::string::npos);
                        }
                        output.push_back(std::move(replacement));
                        written[current][i] = true;
                        replaced = true;
                    }
                }
            }
        }

        if (!replaced) {
            output.push_back(current_line);
        }
        if (!trimmed.empty()) {
            section_end = output.size();
        }
    }

    if (current < sections.size()) {
        append_unwritten(current, 2);
    }

    for (std::size_t i = 0; i < sections.size(); ++i) {
        if (section_found[i] || sections[i].second.empty()) {
            continue;
        }
        if (!output.empty() && !output.back().empty()) {
            output.push_back("");
        }
        output.push_back(sections[i].first + ":");
        section_end = output.size();
        append_unwritten(i, 2);
    }

    std::ofstream output_file(config_path, std::ios::trunc);
    if (!output_file.is_open()) {
        error_message = "Unable to open config file for writing";
        return false;
    }
    for (const auto& output_line : output) {
        output_file << output_line << '\n';
    }

    return true;
}

std::optional<LogLevel> parse_log_level_string(const std::string& input) {
    std::string level = normalize_level_string(input);
    if (level == "CRITICAL") {
        return LogLevel::Critical;
    }
    for (const auto& entry : kLogLevelMap) {
        if (level == entry.first) {
            return entry.second;
        }
    }
    if (level == "FATAL") {
        return LogLevel::Critical;
    }
    return std::nullopt;
}

// The (vendor, deviceType, deviceNumber) key of a persisted-list entry, read
// through typed guards rather than value() (#388): a hand-edited file can
// carry "deviceNumber": "3", and that entry stays in persisted_devices_ (only
// its registration fails), so every walk over the list must step past it
// instead of throwing type_error on a PUT or POST aimed at a different,
// valid device. Absent or wrong-typed type/number yields no key; vendor is
// "" when missing. deviceType is lowercased, since the web UI and the tests
// post it lowercase while the registry answers "Telescope".
struct PersistedKey {
    std::string vendor{};
    std::string device_type{};  // lowercase
    int device_number = -1;
};

// Decision 0018: the persisted UniqueID of a configured device. A stored or
// client-sent value is kept when it is a keepable ID (a version 4 UUID or a
// long non-slot string such as ZWO_UID_<hex>) that no other entry holds (the
// web UI edits a device by removing and re-adding it, sending the ID back);
// only an entry with none, a device-number ID or a duplicate gets a fresh GUID.
std::string stored_unique_id(const nlohmann::json& entry) {
    const auto it = entry.find("uniqueId");
    if (it == entry.end() || !it->is_string()) return {};
    const std::string value = it->get<std::string>();
    return alpacacore::util::is_keepable_unique_id(value) ? value : std::string();
}

std::string choose_unique_id(const nlohmann::json& entry, const std::set<std::string>& taken) {
    std::string value = stored_unique_id(entry);
    while (value.empty() || taken.count(value) != 0) {
        value = alpacacore::util::generate_unique_id();
    }
    return value;
}

using alpacahttp::catalog_json::whole_json_int;

std::optional<PersistedKey> persisted_key(const nlohmann::json& entry) {
    if (!entry.is_object()) {
        return std::nullopt;
    }
    const auto type_it = entry.find("deviceType");
    const auto number_it = entry.find("deviceNumber");
    // The numbers catalog_json::get_int() accepts at registration (whole_json_int):
    // a hand-written 3.0 registers as device 3, so the walks over the
    // persisted list must find that same entry or removedevice would drop it
    // from the registry and leave it in the file. A number registration
    // refuses (4294967299, 3.5) has no key, like a wrong-typed one.
    if (type_it == entry.end() || !type_it->is_string() || number_it == entry.end()) {
        return std::nullopt;
    }
    const auto number = whole_json_int(*number_it);
    if (!number) {
        return std::nullopt;
    }
    PersistedKey key;
    key.device_type = to_lower_copy(type_it->get<std::string>());
    key.device_number = *number;
    const auto vendor_it = entry.find("vendor");
    if (vendor_it != entry.end() && vendor_it->is_string()) {
        key.vendor = vendor_it->get<std::string>();
    }
    return key;
}

// Throw a parameter-validation failure with an explicit ASCOM error code, so
// the ErrorNumber on the wire is deterministic rather than inferred from the
// message text. For a value that parsed but is out of range (InvalidValue,
// 0x401, HTTP 200); a missing or unparseable parameter is throw_bad_request().
[[noreturn]] void throw_invalid_value(const std::string& message) {
    throw alpacacore::AlpacaException(message, alpacacore::AlpacaError::InvalidValue);
}

bool is_lowercase_ascii(const std::string& value) {
    for (unsigned char c : value) {
        if (c >= 'A' && c <= 'Z') {
            return false;
        }
    }
    return true;
}

// The verb masks below mirror the GET/PUT branches of each dispatch_*_method,
// not AlpacaDeviceAPI_v1.yaml. They include this repo's extensions: PUT on
// filter wheel names/focusoffsets and rotator targetposition (GET-only in the
// spec), and switch cancelasync (no path in the spec; kept only so it answers
// NotImplemented instead of "Unknown method", #576). Narrowing a mask to
// match the spec would turn a call that works today into a 400.
enum : std::uint8_t {
    kVerbGet = 1,
    kVerbPut = 2,
};

const std::unordered_map<std::string, unsigned> kCommonMethods = {
    {"action", kVerbPut},           {"commandblind", kVerbPut}, {"commandbool", kVerbPut},
    {"commandstring", kVerbPut},    {"connect", kVerbPut},      {"connected", kVerbGet | kVerbPut},
    {"connecting", kVerbGet},       {"description", kVerbGet},  {"devicestate", kVerbGet},
    {"disconnect", kVerbPut},       {"driverinfo", kVerbGet},   {"driverversion", kVerbGet},
    {"interfaceversion", kVerbGet}, {"name", kVerbGet},         {"supportedactions", kVerbGet},
};

const std::unordered_map<std::string, unsigned> kTelescopeMethods = {
    {"abortslew", kVerbPut},
    {"alignmentmode", kVerbGet},
    {"altitude", kVerbGet},
    {"aperturearea", kVerbGet},
    {"aperturediameter", kVerbGet},
    {"athome", kVerbGet},
    {"atpark", kVerbGet},
    {"axisrates", kVerbGet},
    {"azimuth", kVerbGet},
    {"canfindhome", kVerbGet},
    {"canmoveaxis", kVerbGet},
    {"canpark", kVerbGet},
    {"canpulseguide", kVerbGet},
    {"cansetdeclinationrate", kVerbGet},
    {"cansetguiderates", kVerbGet},
    {"cansetpark", kVerbGet},
    {"cansetpierside", kVerbGet},
    {"cansetrightascensionrate", kVerbGet},
    {"cansettracking", kVerbGet},
    {"canslew", kVerbGet},
    {"canslewaltaz", kVerbGet},
    {"canslewaltazasync", kVerbGet},
    {"canslewasync", kVerbGet},
    {"cansync", kVerbGet},
    {"cansyncaltaz", kVerbGet},
    {"canunpark", kVerbGet},
    {"declination", kVerbGet},
    {"declinationrate", kVerbGet | kVerbPut},
    {"destinationsideofpier", kVerbGet},
    {"doesrefraction", kVerbGet | kVerbPut},
    {"equatorialsystem", kVerbGet},
    {"findhome", kVerbPut},
    {"focallength", kVerbGet},
    {"guideratedeclination", kVerbGet | kVerbPut},
    {"guideraterightascension", kVerbGet | kVerbPut},
    {"ispulseguiding", kVerbGet},
    {"moveaxis", kVerbPut},
    {"park", kVerbPut},
    {"pulseguide", kVerbPut},
    {"rightascension", kVerbGet},
    {"rightascensionrate", kVerbGet | kVerbPut},
    {"setpark", kVerbPut},
    {"sideofpier", kVerbGet | kVerbPut},
    {"siderealtime", kVerbGet},
    {"siteelevation", kVerbGet | kVerbPut},
    {"sitelatitude", kVerbGet | kVerbPut},
    {"sitelongitude", kVerbGet | kVerbPut},
    {"slewing", kVerbGet},
    {"slewsettletime", kVerbGet | kVerbPut},
    {"slewtoaltaz", kVerbPut},
    {"slewtoaltazasync", kVerbPut},
    {"slewtocoordinates", kVerbPut},
    {"slewtocoordinatesasync", kVerbPut},
    {"slewtotarget", kVerbPut},
    {"slewtotargetasync", kVerbPut},
    {"synctoaltaz", kVerbPut},
    {"synctocoordinates", kVerbPut},
    {"synctotarget", kVerbPut},
    {"targetdeclination", kVerbGet | kVerbPut},
    {"targetrightascension", kVerbGet | kVerbPut},
    {"tracking", kVerbGet | kVerbPut},
    {"trackingrate", kVerbGet | kVerbPut},
    {"trackingrates", kVerbGet},
    {"unpark", kVerbPut},
    {"utcdate", kVerbGet | kVerbPut},
};

const std::unordered_map<std::string, unsigned> kCameraMethods = {
    {"abortexposure", kVerbPut},
    {"bayeroffsetx", kVerbGet},
    {"bayeroffsety", kVerbGet},
    {"binx", kVerbGet | kVerbPut},
    {"biny", kVerbGet | kVerbPut},
    {"camerastate", kVerbGet},
    {"cameraxsize", kVerbGet},
    {"cameraysize", kVerbGet},
    {"canabortexposure", kVerbGet},
    {"canasymmetricbin", kVerbGet},
    {"canfastreadout", kVerbGet},
    {"cangetcoolerpower", kVerbGet},
    {"canpulseguide", kVerbGet},
    {"cansetccdtemperature", kVerbGet},
    {"canstopexposure", kVerbGet},
    {"ccdtemperature", kVerbGet},
    {"cooleron", kVerbGet | kVerbPut},
    {"coolerpower", kVerbGet},
    {"electronsperadu", kVerbGet},
    {"exposuremax", kVerbGet},
    {"exposuremin", kVerbGet},
    {"exposureresolution", kVerbGet},
    {"fastreadout", kVerbGet | kVerbPut},
    {"fullwellcapacity", kVerbGet},
    {"gain", kVerbGet | kVerbPut},
    {"gainmax", kVerbGet},
    {"gainmin", kVerbGet},
    {"gains", kVerbGet},
    {"hasshutter", kVerbGet},
    {"heatsinktemperature", kVerbGet},
    {"imagearray", kVerbGet},
    {"imagearrayvariant", kVerbGet},
    {"imageready", kVerbGet},
    {"ispulseguiding", kVerbGet},
    {"lastexposureduration", kVerbGet},
    {"lastexposurestarttime", kVerbGet},
    {"maxadu", kVerbGet},
    {"maxbinx", kVerbGet},
    {"maxbiny", kVerbGet},
    {"numx", kVerbGet | kVerbPut},
    {"numy", kVerbGet | kVerbPut},
    {"offset", kVerbGet | kVerbPut},
    {"offsetmax", kVerbGet},
    {"offsetmin", kVerbGet},
    {"offsets", kVerbGet},
    {"percentcompleted", kVerbGet},
    {"pixelsizex", kVerbGet},
    {"pixelsizey", kVerbGet},
    {"pulseguide", kVerbPut},
    {"readoutmode", kVerbGet | kVerbPut},
    {"readoutmodes", kVerbGet},
    {"sensorname", kVerbGet},
    {"sensortype", kVerbGet},
    {"setccdtemperature", kVerbGet | kVerbPut},
    {"startexposure", kVerbPut},
    {"startx", kVerbGet | kVerbPut},
    {"starty", kVerbGet | kVerbPut},
    {"stopexposure", kVerbPut},
    {"subexposureduration", kVerbGet | kVerbPut},
};

const std::unordered_map<std::string, unsigned> kFilterWheelMethods = {
    {"focusoffsets", kVerbGet | kVerbPut},
    {"names", kVerbGet | kVerbPut},
    {"position", kVerbGet | kVerbPut},
};

const std::unordered_map<std::string, unsigned> kFocuserMethods = {
    {"absolute", kVerbGet},          {"halt", kVerbPut},        {"ismoving", kVerbGet},
    {"maxincrement", kVerbGet},      {"maxstep", kVerbGet},     {"move", kVerbPut},
    {"position", kVerbGet},          {"stepsize", kVerbGet},    {"tempcomp", kVerbGet | kVerbPut},
    {"tempcompavailable", kVerbGet}, {"temperature", kVerbGet},
};

const std::unordered_map<std::string, unsigned> kRotatorMethods = {
    {"canreverse", kVerbGet},         {"halt", kVerbPut},     {"ismoving", kVerbGet},
    {"mechanicalposition", kVerbGet}, {"move", kVerbPut},     {"moveabsolute", kVerbPut},
    {"movemechanical", kVerbPut},     {"position", kVerbGet}, {"reverse", kVerbGet | kVerbPut},
    {"stepsize", kVerbGet},           {"sync", kVerbPut},     {"targetposition", kVerbGet | kVerbPut},
};

const std::unordered_map<std::string, unsigned> kDomeMethods = {
    {"abortslew", kVerbPut},      {"altitude", kVerbGet},
    {"athome", kVerbGet},         {"atpark", kVerbGet},
    {"azimuth", kVerbGet},        {"canfindhome", kVerbGet},
    {"canpark", kVerbGet},        {"cansetaltitude", kVerbGet},
    {"cansetazimuth", kVerbGet},  {"cansetpark", kVerbGet},
    {"cansetshutter", kVerbGet},  {"canslave", kVerbGet},
    {"cansyncazimuth", kVerbGet}, {"closeshutter", kVerbPut},
    {"findhome", kVerbPut},       {"openshutter", kVerbPut},
    {"park", kVerbPut},           {"setpark", kVerbPut},
    {"shutterstatus", kVerbGet},  {"slaved", kVerbGet | kVerbPut},
    {"slewing", kVerbGet},        {"slewtoaltitude", kVerbPut},
    {"slewtoazimuth", kVerbPut},  {"synctoazimuth", kVerbPut},
};

const std::unordered_map<std::string, unsigned> kSwitchMethods = {
    {"canasync", kVerbGet},
    {"cancelasync", kVerbGet | kVerbPut},
    {"canwrite", kVerbGet},
    {"getswitch", kVerbGet},
    {"getswitchdescription", kVerbGet},
    {"getswitchname", kVerbGet},
    {"getswitchvalue", kVerbGet},
    {"maxswitch", kVerbGet},
    {"maxswitchvalue", kVerbGet},
    {"minswitchvalue", kVerbGet},
    {"setasync", kVerbPut},
    {"setasyncvalue", kVerbPut},
    {"setswitch", kVerbPut},
    {"setswitchname", kVerbPut},
    {"setswitchvalue", kVerbPut},
    {"statechangecomplete", kVerbGet},
    {"switchstep", kVerbGet},
};

const std::unordered_map<std::string, unsigned> kCoverCalibratorMethods = {
    {"brightness", kVerbGet},    {"calibratorchanging", kVerbGet}, {"calibratoroff", kVerbPut},
    {"calibratoron", kVerbPut},  {"calibratorstate", kVerbGet},    {"closecover", kVerbPut},
    {"covermoving", kVerbGet},   {"coverstate", kVerbGet},         {"haltcover", kVerbPut},
    {"maxbrightness", kVerbGet}, {"opencover", kVerbPut},
};

const std::unordered_map<std::string, unsigned> kObservingConditionsMethods = {
    {"averageperiod", kVerbGet | kVerbPut},
    {"cloudcover", kVerbGet},
    {"dewpoint", kVerbGet},
    {"humidity", kVerbGet},
    {"pressure", kVerbGet},
    {"rainrate", kVerbGet},
    {"refresh", kVerbPut},
    {"sensordescription", kVerbGet},
    {"skybrightness", kVerbGet},
    {"skyquality", kVerbGet},
    {"skytemperature", kVerbGet},
    {"starfwhm", kVerbGet},
    {"temperature", kVerbGet},
    {"timesincelastupdate", kVerbGet},
    {"winddirection", kVerbGet},
    {"windgust", kVerbGet},
    {"windspeed", kVerbGet},
};

const std::unordered_map<std::string, unsigned> kSafetyMonitorMethods = {
    {"issafe", kVerbGet},
};

bool is_known_device_type_name(const std::string& type_name) {
    static const std::unordered_set<std::string> kDeviceTypes = {
        "camera", "telescope",       "filterwheel",         "focuser",       "rotator", "dome",
        "switch", "covercalibrator", "observingconditions", "safetymonitor",
    };
    return kDeviceTypes.count(type_name) > 0;
}

unsigned lookup_verbs(const std::unordered_map<std::string, unsigned>& table, const std::string& method_name) {
    auto it = table.find(method_name);
    return it != table.end() ? it->second : 0;
}

// Returns the bitmask (kVerbGet | kVerbPut) of HTTP verbs `method_name`
// accepts for `type`, or 0 if the name is unknown for that device type.
// #574: this used to be is_valid_method(), a bool with no verb information,
// so a known name on the wrong verb (or POST/DELETE) fell through to one of
// the "not yet implemented" fallbacks below instead of being rejected here.
unsigned allowed_verbs(alpacacore::DeviceType type, const std::string& method_name) {
    unsigned common = lookup_verbs(kCommonMethods, method_name);
    if (common != 0) {
        return common;
    }
    switch (type) {
        case alpacacore::DeviceType::Camera:
            return lookup_verbs(kCameraMethods, method_name);
        case alpacacore::DeviceType::Telescope:
            return lookup_verbs(kTelescopeMethods, method_name);
        case alpacacore::DeviceType::FilterWheel:
            return lookup_verbs(kFilterWheelMethods, method_name);
        case alpacacore::DeviceType::Focuser:
            return lookup_verbs(kFocuserMethods, method_name);
        case alpacacore::DeviceType::Rotator:
            return lookup_verbs(kRotatorMethods, method_name);
        case alpacacore::DeviceType::Dome:
            return lookup_verbs(kDomeMethods, method_name);
        case alpacacore::DeviceType::Switch:
            return lookup_verbs(kSwitchMethods, method_name);
        case alpacacore::DeviceType::CoverCalibrator:
            return lookup_verbs(kCoverCalibratorMethods, method_name);
        case alpacacore::DeviceType::ObservingConditions:
            return lookup_verbs(kObservingConditionsMethods, method_name);
        case alpacacore::DeviceType::SafetyMonitor:
            return lookup_verbs(kSafetyMonitorMethods, method_name);
        default:
            return 0;
    }
}

const char* http_method_name(alpacahttp::HttpMethod method) {
    switch (method) {
        case alpacahttp::HttpMethod::GET:
            return "GET";
        case alpacahttp::HttpMethod::POST:
            return "POST";
        case alpacahttp::HttpMethod::PUT:
            return "PUT";
        case alpacahttp::HttpMethod::DELETE_:
            return "DELETE";
        default:
            return "UNKNOWN";
    }
}

void apply_error_status(alpacahttp::Response& response, std::int32_t error_code) {
    if (response.status_code() != 200) {
        return;
    }
    if (error_code != alpacahttp::util::ErrorCode::SUCCESS) {
        // Alpaca responses must remain HTTP 200; ErrorNumber conveys failures.
        return;
    }
}

std::optional<std::string> get_query_param_case_insensitive(const alpacahttp::Request& request, const std::string& key) {
    if (request.has_query_param(key)) {
        return request.get_query_param(key);
    }
    const std::string target = to_lower_copy(key);
    for (const auto& entry : request.query_params()) {
        if (to_lower_copy(entry.first) == target) {
            return entry.second;
        }
    }
    return std::nullopt;
}

// Per-client Connected tracking (issue #160): identify the caller by its
// Alpaca ClientID (query param on GET, form/JSON field on PUT), qualified by
// the connection's peer address (issue #163) so two clients on different
// hosts that happen to share a ClientID — or both omit it — get distinct
// registry slots. Clients that send no ClientID share one anonymous slot per
// peer address, so they still participate in refcounting rather than tearing
// the device link down for everyone.
constexpr const char* kAnonymousClientKey = "<anonymous>";

// A registration not refreshed within this window is considered abandoned
// (client crashed or vanished without PUT connected=false) and no longer
// blocks the last-client-out disconnect. Any request from the client to the
// device refreshes its registration, so ordinary polling keeps it alive.
constexpr std::chrono::minutes kClientConnectionStaleAfter{10};

// The caller's registry identity plus whether it actually supplied a
// ClientID — GET connected falls back to raw device state for ClientID-less
// probes, which can't be told from the key alone once the peer address is
// folded in.
struct ClientKey {
    std::string key;
    bool has_client_id = false;
};

ClientKey extract_client_key(const alpacahttp::Request& request) {
    ClientKey result;
    if (request.method() == alpacahttp::HttpMethod::GET) {
        if (auto value = get_query_param_case_insensitive(request, "ClientID")) {
            result.key = *value;
            result.has_client_id = true;
        }
    } else if (!request.body().empty()) {
        if (auto json_opt = alpacahttp::parse_json(request.body())) {
            if (const auto* val = find_json_value(*json_opt, "ClientID")) {
                if (val->is_string()) {
                    result.key = val->get<std::string>();
                    result.has_client_id = true;
                } else if (val->is_number_integer()) {
                    result.key = std::to_string(val->get<std::int64_t>());
                    result.has_client_id = true;
                }
            }
        }
        if (!result.has_client_id) {
            if (auto value = get_form_value(request.body(), "ClientID")) {
                result.key = *value;
                result.has_client_id = true;
            }
        }
    }
    if (!result.has_client_id) {
        result.key = kAnonymousClientKey;
    }
    // Qualify by peer address (issue #163). The address is length-prefixed so
    // the composite key decodes unambiguously — ClientID is an arbitrary
    // client-supplied string, so any plain delimiter could be forged into a
    // collision with another client's (address, ClientID) pair. Empty when
    // unknown (unit tests, or getpeername failure): "0##<id>" degrades to a
    // ClientID-only identity that no address-qualified key can collide with.
    const std::string& addr = request.remote_address();
    result.key = std::to_string(addr.size()) + "#" + addr + "#" + result.key;
    return result;
}

nlohmann::json make_log_level_payload() {
    nlohmann::json payload;
    payload["Level"] = log_level_to_string(alpacacore::logging::get_log_level());
    payload["SupportedLevels"] = {"TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"};
    return payload;
}

} // namespace

namespace alpacahttp {

// open-astro#765: configuredevice answers 400 for a refusal that starts with this.
constexpr const char* kHardwareConfigRefusal = alpacacore::util::kHardwareConfigRefusal;

namespace {
// Issue #358: a driver that refuses a connect explains why, and the client
// never saw it -- the reason reached the server log and stopped there, so
// every "why won't it connect" question started with asking the operator for
// the log. AsyncConnectable now keeps the reason; this reads it back for the
// 38 drivers that derive from it, and falls back to the old constant for
// anything that does not, or when the failure produced no text.
//
// The text goes out verbatim. Driver messages are written for logs and can
// name host paths (a serial port, a config field), which is exactly what an
// operator needs to act on and is no more than the same message already
// visible in the log file the web UI serves.
std::string connect_failure_reason(const alpacacore::AlpacaDriver& device) {
    std::string reason = device.get_last_connect_error();
    return reason.empty() ? std::string("Connection failed") : reason;
}

// open-astro#711: libstdc++ std::regex recurses once per repeated character,
// so a long path segment overflowed the worker stack and killed the server.
// route() refuses anything longer before a regex sees it. The longest valid
// path today is under 100 bytes; 2048 stays far below the crash depth.
constexpr std::size_t kMaxRequestPathBytes = 2048;

// open-astro#392: the host part of a Host header or an http.allowed_hosts
// entry: port removed, lowercased, one trailing dot removed. A bracketed IPv6
// literal keeps its brackets. std::nullopt when the value cannot name a host:
// an unbracketed name with two or more colons (a bare IPv6 literal, which a
// browser never sends), an unclosed bracket, or text after the bracket that is
// not a port.
std::optional<std::string> normalize_host(std::string_view value) {
    std::string_view name = value;
    if (!name.empty() && name.front() == '[') {
        const auto close = name.find(']');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        const auto rest = name.substr(close + 1);
        if (!rest.empty() && rest.front() != ':') {
            return std::nullopt;
        }
        name = name.substr(0, close + 1);
    } else if (const auto colon = name.find(':'); colon != std::string_view::npos) {
        if (name.find(':', colon + 1) != std::string_view::npos) {
            return std::nullopt;
        }
        name = name.substr(0, colon);
    }
    std::string normalized(name);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!normalized.empty() && normalized.back() == '.') {
        normalized.pop_back();
    }
    return normalized;
}

// The entries set_allowed_hosts() stores: trimmed, normalized like a Host
// header, empty and unusable ones dropped. The description PUT compares and
// persists this form, so file, memory and the lockout check all see one list.
std::vector<std::string> normalize_host_list(const std::vector<std::string>& hosts) {
    std::vector<std::string> normalized;
    for (const auto& entry : hosts) {
        const auto first = entry.find_first_not_of(" \t");
        if (first == std::string::npos) {
            continue;
        }
        auto name = normalize_host(std::string_view(entry).substr(first, entry.find_last_not_of(" \t") - first + 1));
        if (name && !name->empty() && *name != ".") {
            normalized.push_back(std::move(*name));
        }
    }
    return normalized;
}

// open-astro#392: whether a normalized allowed-hosts entry can match a Host
// header: an optional leading '.' and dot-separated labels of [a-z0-9_-]
// (1..63 each, 253 in all), or a bracketed IPv6 literal.
bool valid_host_entry(const std::string& entry) {
    if (entry.size() > 2 && entry.front() == '[' && entry.back() == ']') {
        // inet_pton stops at a NUL, so refuse anything but address characters first.
        const std::string inner = entry.substr(1, entry.size() - 2);
        const bool chars_ok = std::all_of(inner.begin(), inner.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == ':' || c == '.';
        });
        in6_addr addr{};
        return chars_ok && inet_pton(AF_INET6, inner.c_str(), &addr) == 1;
    }
    std::string_view rest = entry;
    if (!rest.empty() && rest.front() == '.') {
        rest.remove_prefix(1);
    }
    if (rest.empty() || rest.size() > 253) {
        return false;
    }
    std::size_t label = 0;
    for (const char c : rest) {
        if (c == '.') {
            if (label == 0) {
                return false;
            }
            label = 0;
            continue;
        }
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok || ++label > 63) {
            return false;
        }
    }
    return label != 0;
}

constexpr std::size_t kMaxAllowedHostEntries = 64;

// The description PUT refuses a bad entry instead of dropping it. Fills
// `normalized` and returns "" when the list is acceptable, else the message
// for the first problem. A port suffix must be digits (normalize_host would
// otherwise read "http://x.lan" as the host "http").
std::string allowed_hosts_problem(const std::vector<std::string>& raw, std::vector<std::string>& normalized) {
    if (raw.size() > kMaxAllowedHostEntries) {
        return "AllowedHosts holds more than 64 entries";
    }
    normalized.clear();
    for (const auto& entry : raw) {
        auto name = normalize_host(entry);
        bool ok = name && valid_host_entry(*name);
        if (ok) {
            const auto port_at = entry.find(':', entry.front() == '[' ? entry.find(']') : 0);
            if (port_at != std::string::npos) {
                const auto port = std::string_view(entry).substr(port_at + 1);
                ok = !port.empty() && port.size() <= 5 &&
                     std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
            }
        }
        if (!ok) {
            std::string shown = entry.substr(0, 80);
            std::replace_if(shown.begin(), shown.end(), [](unsigned char c) { return c < 0x20 || c > 0x7e; }, '?');
            return "AllowedHosts entry '" + shown + "' is not a host name";
        }
        normalized.push_back(std::move(*name));
    }
    return {};
}

std::string join_host_list(const std::vector<std::string>& hosts) {
    std::string joined;
    for (const auto& host : hosts) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += host;
    }
    return joined;
}

// `suffix` starts with a dot; the name must have at least one character
// before it, so ".local" alone is not a .local name.
bool has_label_suffix(std::string_view name, std::string_view suffix) {
    return name.size() > suffix.size() && name.substr(name.size() - suffix.size()) == suffix;
}

// A parser, not a pattern: "1.2.3.4.evil.example" and "0x7f.1" are names.
bool is_ip_literal(const std::string& name) {
    if (name.size() > 2 && name.front() == '[' && name.back() == ']') {
        in6_addr v6{};
        return ::inet_pton(AF_INET6, name.substr(1, name.size() - 2).c_str(), &v6) == 1;
    }
    in_addr v4{};
    return ::inet_pton(AF_INET, name.c_str(), &v4) == 1;
}

// `name` is normalized and non-empty. The reserved suffixes never resolve
// through public DNS (RFC 6761 .localhost, RFC 6762 .local, RFC 8375
// .home.arpa, ICANN 2024 .internal), so a rebinding attacker cannot use them.
bool host_allowed(const std::string& name, const std::vector<std::string>& allowed_hosts,
                  const std::string& machine_hostname) {
    if (is_ip_literal(name) || name == "localhost" || has_label_suffix(name, ".localhost")) {
        return true;
    }
    if (!machine_hostname.empty() && (name == machine_hostname || name == machine_hostname + ".local")) {
        return true;
    }
    for (const std::string_view suffix : {".local", ".home.arpa", ".internal"}) {
        if (has_label_suffix(name, suffix)) {
            return true;
        }
    }
    for (const auto& entry : allowed_hosts) {
        if (entry.front() == '.') {
            if (name == std::string_view(entry).substr(1) || has_label_suffix(name, entry)) {
                return true;
            }
        } else if (name == entry) {
            return true;
        }
    }
    return false;
}

std::string read_machine_hostname() {
    std::array<char, 256> buffer{};
    if (::gethostname(buffer.data(), buffer.size() - 1) != 0) {
        return {};
    }
    return normalize_host(buffer.data()).value_or(std::string());
}

// open-astro#392: DNS rebinding. After a rebind the browser addresses this
// server by the attacker's name, sends it as Host (and as Origin, so the
// Origin==Host guard below passes), and treats the server as same-origin.
// Refuse any Host that is not allowed, for every method and path. A missing or
// empty Host is allowed: HTTP/1.0 clients omit it, a browser never does.
// Logged at DEBUG only: a page under attack repeats the request.
std::optional<Response> reject_disallowed_host(const Request& request, std::uint32_t server_tx_id,
                                               const std::vector<std::string>& allowed_hosts,
                                               const std::string& machine_hostname) {
    const std::string host = request.get_header("Host");
    if (host.empty()) {
        return std::nullopt;
    }
    const auto name = normalize_host(host);
    if (name && !name->empty() && host_allowed(*name, allowed_hosts, machine_hostname)) {
        return std::nullopt;
    }
    // The header is client text: cut it to 255 bytes and replace anything
    // but printable ASCII, so the JSON body always serializes.
    std::string shown = host.substr(0, 255);
    std::replace_if(shown.begin(), shown.end(), [](unsigned char c) { return c < 0x20 || c > 0x7e; }, '?');
    util::log_debug("Refused request for Host '" + shown + "'");
    // Same ClientTransactionID precedence as reject_cross_origin_request():
    // the query string's, else the JSON body's (open-astro#384, #509).
    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
    }
    if (client_tx_id == 0 && !request.body().empty()) {
        if (auto json_opt = parse_json(request.body())) {
            client_tx_id = extract_client_transaction_id(*json_opt);
        }
    }
    AlpacaResponse alpaca_response =
        make_error_response(client_tx_id, server_tx_id, util::ErrorCode::INVALID_VALUE,
                            "Host '" + shown +
                                "' is not allowed; open the web UI by IP address and add it under Allowed host "
                                "names, or add it to http.allowed_hosts");
    Response resp;
    resp.set_content_type("application/json");
    resp.set_status(403, "Forbidden");
    resp.set_body(alpaca_response);
    return resp;
}

// Defined further down with the management guards, but declared here because
// every state-changing management handler needs it and handle_description()
// is the first of them in file order. Also used by the four device setters
// with a side effect beyond the driver: UTCDate steps the host clock
// (open-astro#401) and the three site setters rewrite persisted config (#444).
//
// Takes the client's ClientTransactionID as well as the server's: the 403 body
// echoes it like every other error path in these handlers (open-astro#384).
std::optional<Response> reject_cross_origin_request(const Request& request, std::uint32_t client_tx_id,
                                                    std::uint32_t server_tx_id, const char* what);

// A verb the Alpaca Management API does not define for `what`: HTTP 400 with a
// text/plain message (Alpaca API Reference s2.5, #1508).
void set_wrong_verb_text(Response& response, const char* what);

// A Device API request the router cannot interpret: HTTP 400 text/plain (#1476).
void set_bad_request_text(Response& response, const std::string& message);

}  // namespace

Router::Router() : Router(CatalogExtension{}) {}

Router::Router(const CatalogExtension& extend_catalog) : machine_hostname_(read_machine_hostname()) {
    alpacacore::catalog::register_builtin_schemas(catalog_);
    alpacacore::catalog::register_builtin_factories(catalog_);
    if (extend_catalog) {
        extend_catalog(catalog_);
    }
    set_server_info("AlpacaHTTP", "AlpacaHTTP", alpacahttp::kVersion, "", "");
    load_persisted_devices();
}
Router::~Router() = default;

void Router::run_motion_watchdogs(std::chrono::steady_clock::time_point now) {
    const auto interval = motion_watchdog_interval();
    if (interval <= std::chrono::milliseconds::zero()) {
        return;
    }
    // Snapshot as shared_ptr copies: the registry never calls back into the
    // router, and this loop must not hold any registry lock while it does
    // mount I/O (get_slewing()/abort_slew(), up to the transport timeout) --
    // see the header comment's lock-order note.
    auto devices =
        alpacacore::management::DeviceRegistry::instance().get_devices_by_type(alpacacore::DeviceType::Telescope);
    for (const auto& device : devices) {
        auto* telescope = dynamic_cast<alpacacore::TelescopeDriver*>(device.get());
        if (telescope == nullptr) {
            continue;
        }
        try {
            telescope->stop_motion_if_client_silent(now, interval);
        } catch (const std::exception& ex) {
            // stop_motion_if_client_silent() is declared noexcept, so an
            // exception escaping it calls std::terminate() before this
            // handler runs: this catch cannot fire today. It is kept only so
            // the loop stays safe if that function ever loses noexcept; the
            // real guard is the try/catch inside the function itself.
            util::log_error("Motion watchdog check failed for telescope #" +
                            std::to_string(telescope->get_device_number()) + ": " + ex.what());
        }
    }
}

void Router::set_management_driver(std::shared_ptr<alpacacore::ManagementDriver> mgmt_driver) {
    management_driver_ = mgmt_driver;
}

void Router::set_server_info(std::string server_name, std::string manufacturer, std::string manufacturer_version,
                             std::string location, std::string profile_name) {
    std::lock_guard<std::mutex> lock(server_info_mutex_);
    server_name_ = std::move(server_name);
    manufacturer_ = std::move(manufacturer);
    manufacturer_version_ = std::move(manufacturer_version);
    location_ = std::move(location);
    profile_name_ = std::move(profile_name);
}

void Router::set_allowed_hosts(const std::vector<std::string>& hosts) {
    auto normalized = std::make_shared<std::vector<std::string>>(normalize_host_list(hosts));
    std::lock_guard<std::mutex> lock(allowed_hosts_mutex_);
    allowed_hosts_ = std::move(normalized);
}

void Router::set_config_path(std::string config_path) {
    std::lock_guard<std::mutex> lock(server_info_mutex_);
    config_path_ = std::move(config_path);
}

// open-astro#302: test-only seam, see router.h.
//
// open-astro#399: this used to replace the whole HostClock. Every
// host_clock_ dereference in route() runs on a request thread, and since #314
// the server's RTC probe thread is a second reader that is not a request
// path, so destroying the object left another thread holding a freed one --
// a use-after-free, not a stale read, prevented only by a comment. It now
// replaces the clock's hooks in place: the object outlives every reader and
// the enabled flag and step latches carry over on their own, so this no
// longer has to save and restore them.
void Router::set_host_clock_hooks(alpacacore::util::HostClock::IsSynchronizedFn is_synchronized,
                                  alpacacore::util::HostClock::SetTimeFn set_time,
                                  alpacacore::util::HostClock::HasRtcFn has_rtc) {
    host_clock_.set_hooks(std::move(is_synchronized), std::move(set_time), std::move(has_rtc));
}

// open-astro#675: the HostClock::set_hooks() shape (#399). A plain
// assignment destroyed the callable a request thread could be inside; the
// snapshot a reader copied stays alive until its call returns.
void Router::set_now_fn(NowFn now_fn) {
    auto next = std::make_shared<const NowFn>(std::move(now_fn));
    std::lock_guard<std::mutex> lock(now_fn_mutex_);
    now_fn_ = std::move(next);
}

std::shared_ptr<const Router::NowFn> Router::current_now_fn() const {
    std::lock_guard<std::mutex> lock(now_fn_mutex_);
    return now_fn_;
}

void Router::set_shutdown_callback(std::function<void()> callback) {
    shutdown_callback_ = callback;
}

void Router::set_restart_callback(std::function<void()> callback) {
    restart_callback_ = callback;
}

Response Router::route(const Request& request, std::uint32_t server_transaction_id) {
    Response response;
    response.set_content_type("application/json");

    // Log incoming requests for debugging
    std::string method_str = (request.method() == HttpMethod::GET) ? "GET" : 
                           (request.method() == HttpMethod::POST) ? "POST" : 
                           (request.method() == HttpMethod::PUT) ? "PUT" : "UNKNOWN";
    util::log_debug("HTTP " + method_str + " " + util::escape_for_log(request.path()));

    try {
        // open-astro#711: before any regex, static file or setup handler. The
        // message gives the lengths, never the path itself.
        if (request.path().size() > kMaxRequestPathBytes) {
            set_bad_request_text(response, "Request path is " + std::to_string(request.path().size()) +
                                               " bytes; the limit is " + std::to_string(kMaxRequestPathBytes) +
                                               " bytes");
            return response;
        }

        // open-astro#392: before static files, setup pages and routing. Only
        // when http.host_check_enabled is on.
        if (host_check_enabled_.load(std::memory_order_acquire)) {
            std::shared_ptr<const std::vector<std::string>> allowed_hosts;
            {
                std::lock_guard<std::mutex> lock(allowed_hosts_mutex_);
                allowed_hosts = allowed_hosts_;
            }
            if (auto refused =
                    reject_disallowed_host(request, server_transaction_id, *allowed_hosts, machine_hostname_)) {
                return *refused;
            }
        }

        // Handle static file requests (web UI)
        if (request.path().find("/web/") == 0) {
            return handle_static_file(request);
        }

        // Handle root path - serve web UI
        if (request.path() == "/" || request.path() == "") {
            if (request.method() == HttpMethod::GET) {
                return handle_static_file(request);
            }
        }

        // Handle driver setup pages: /setup/v1/{devicetype}/{devicenumber}/setup
        if (request.path().find("/setup/v1/") == 0) {
            return handle_setup(request, server_transaction_id);
        }

        RouteMatch match = parse_route(request.path());

        // Check if route is valid
        if (!match.is_management && match.device_type.empty()) {
            // No valid route matched - return 404
            response.set_status(404, "Not Found");
            std::uint32_t client_tx_id = 0;
            if (request.has_query_param("ClientTransactionID")) {
                client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
            }
            AlpacaResponse alpaca_response = make_error_response(
                client_tx_id, server_transaction_id,
                util::ErrorCode::INVALID_VALUE,
                "Endpoint not found: " + request.path()
            );
            response.set_body(alpaca_response);
            return response;
        }

        if (match.is_management) {
            return handle_management(request, match, server_transaction_id);
        } else {
            return handle_device(request, match, server_transaction_id);
        }
    } catch (const std::exception& e) {
        util::log_error("Router error: " + std::string(e.what()));
        AlpacaResponse alpaca_response = make_error_response(
            0, server_transaction_id,
            util::ErrorCode::DRIVER_ERROR,
            "Internal server error: " + std::string(e.what())
        );
        response.set_body(alpaca_response);
        return response;
    }
}

RouteMatch Router::parse_route(const std::string& path) {
    RouteMatch match;

    // Root endpoint - return helpful information (JSON API)
    if (path == "/api" || path == "/api/") {
        match.is_management = true;
        match.management_endpoint = "root";
        return match;
    }

    // Management endpoints (support both /management/v1/... and /management/... for compatibility)
    if (path == "/management/v1/description" || path == "/management/description") {
        match.is_management = true;
        match.management_endpoint = "description";
        return match;
    }
    if (path == "/management/v1/apiversions" || path == "/management/apiversions") {
        match.is_management = true;
        match.management_endpoint = "apiversions";
        return match;
    }
    if (path == "/management/v1/buildinfo" || path == "/management/buildinfo") {
        match.is_management = true;
        match.management_endpoint = "buildinfo";
        return match;
    }
    if (path == "/management/v1/configureddevices" || path == "/management/configureddevices") {
        match.is_management = true;
        match.management_endpoint = "configureddevices";
        return match;
    }
    if (path == "/management/v1/faileddevices" || path == "/management/faileddevices") {
        match.is_management = true;
        match.management_endpoint = "faileddevices";
        return match;
    }
    if (path == "/management/v1/devicecatalog" || path == "/management/devicecatalog") {
        match.is_management = true;
        match.management_endpoint = "devicecatalog";
        return match;
    }
    if (path == "/management/v1/thermalcameras" || path == "/management/thermalcameras") {
        match.is_management = true;
        match.management_endpoint = "thermalcameras";
        return match;
    }
    if (path == "/management/v1/configuredevice" || path == "/management/configuredevice") {
        match.is_management = true;
        match.management_endpoint = "configuredevice";
        return match;
    }
    if (path == "/management/v1/removedevice" || path == "/management/removedevice") {
        match.is_management = true;
        match.management_endpoint = "removedevice";
        return match;
    }
    if (path == "/management/v1/loglevel" || path == "/management/loglevel") {
        match.is_management = true;
        match.management_endpoint = "loglevel";
        return match;
    }
    if (path == "/management/v1/logs" || path == "/management/logs") {
        match.is_management = true;
        match.management_endpoint = "logs";
        return match;
    }
    if (path == "/management/v1/logfiles" || path == "/management/logfiles") {
        match.is_management = true;
        match.management_endpoint = "logfiles";
        return match;
    }
    {
        static const std::regex kLogfileItemRegex(
            R"(^/management/(?:v1/)?logfiles/([^/?]+)/?$)");
        std::smatch logfile_match;
        if (std::regex_match(path, logfile_match, kLogfileItemRegex)) {
            match.is_management = true;
            match.management_endpoint = "logfile";
            match.method_name = logfile_match[1].str();
            return match;
        }
    }
    if (path == "/management/v1/shutdown" || path == "/management/shutdown") {
        match.is_management = true;
        match.management_endpoint = "shutdown";
        return match;
    }
    if (path == "/management/v1/restart" || path == "/management/restart") {
        match.is_management = true;
        match.management_endpoint = "restart";
        return match;
    }
    if (path == "/management/v1/synctime" || path == "/management/synctime") {
        match.is_management = true;
        match.management_endpoint = "synctime";
        return match;
    }
    {
        // WiFi manager: /management/v1/wifi/<sub>[/<arg>] — sub+arg travel in
        // method_name ("status", "profiles", "profiles/<uuid>", ...).
        static const std::regex kWifiRegex(R"(^/management/(?:v1/)?wifi/([a-z]+(?:/[^/?]+)?)/?$)");
        std::smatch wifi_match;
        if (std::regex_match(path, wifi_match, kWifiRegex)) {
            match.is_management = true;
            match.management_endpoint = "wifi";
            match.method_name = wifi_match[1].str();
            return match;
        }
    }
    {
        // Software update: /management/v1/update/<status|check|install|channel>.
        static const std::regex kUpdateRegex(R"(^/management/(?:v1/)?update/([a-z]+)/?$)");
        std::smatch update_match;
        if (std::regex_match(path, update_match, kUpdateRegex)) {
            match.is_management = true;
            match.management_endpoint = "update";
            match.method_name = update_match[1].str();
            return match;
        }
    }

    // Device API: /api/v1/{devicetype}/{devicenumber}/{method}
    // Static: compiling a std::regex costs far more than matching it, and this
    // runs on every device request (#646).
    static const std::regex device_regex(R"(/api/v1/([^/]+)/(\d+)/([^/?]+))");
    std::smatch matches;
    if (std::regex_match(path, matches, device_regex)) {
        match.device_type = matches[1].str();
        // #574: the yaml defines device_number as uint32 (0..4294967295).
        // std::stoul is a 64-bit parse on LP64 (arm64 Linux, this build), so
        // a digit string like "4294967296" (2^32) used to parse cleanly and
        // then wrap to 0 on the cast below, silently addressing device 0
        // instead of being rejected. Parse into an unsigned long long,
        // require the whole digit string to be consumed, and range-check
        // against uint32_t's max before narrowing. A digit string that
        // overflows even a 64-bit parse (stoull throws out_of_range) is
        // caught the same way as the range-check failure.
        try {
            std::size_t pos = 0;
            unsigned long long device_number = std::stoull(matches[2].str(), &pos);
            if (pos != matches[2].str().size() || device_number > std::numeric_limits<std::uint32_t>::max()) {
                match.device_number_invalid = true;
            } else {
                match.device_number = static_cast<std::uint32_t>(device_number);
            }
        } catch (const std::exception&) {
            match.device_number_invalid = true;
        }
        match.method_name = matches[3].str();
        return match;
    }

    // Not found
    return match;
}

Response Router::handle_management(const Request& request, const RouteMatch& match, std::uint32_t server_tx_id) {
    if (match.management_endpoint == "root") {
        return handle_root(request, server_tx_id);
    } else if (match.management_endpoint == "description") {
        return handle_description(request, server_tx_id);
    } else if (match.management_endpoint == "apiversions") {
        return handle_api_versions(request, server_tx_id);
    } else if (match.management_endpoint == "buildinfo") {
        return handle_build_info(request, server_tx_id);
    } else if (match.management_endpoint == "configureddevices") {
        return handle_configured_devices(request, server_tx_id);
    } else if (match.management_endpoint == "faileddevices") {
        return handle_failed_devices(request, server_tx_id);
    } else if (match.management_endpoint == "devicecatalog") {
        return handle_device_catalog(request, server_tx_id);
    } else if (match.management_endpoint == "thermalcameras") {
        return handle_thermal_cameras(request, server_tx_id);
    } else if (match.management_endpoint == "configuredevice") {
        return handle_configure_device(request, server_tx_id);
    } else if (match.management_endpoint == "removedevice") {
        return handle_remove_device(request, server_tx_id);
    } else if (match.management_endpoint == "loglevel") {
        return handle_log_level(request, server_tx_id);
    } else if (match.management_endpoint == "logs") {
        return handle_logs(request, server_tx_id);
    } else if (match.management_endpoint == "logfiles") {
        return handle_log_files_list(request, server_tx_id);
    } else if (match.management_endpoint == "logfile") {
        return handle_log_file_item(request, match.method_name, server_tx_id);
    } else if (match.management_endpoint == "shutdown") {
        return handle_shutdown(request, server_tx_id);
    } else if (match.management_endpoint == "restart") {
        return handle_restart(request, server_tx_id);
    } else if (match.management_endpoint == "synctime") {
        return handle_sync_time(request, server_tx_id);
    } else if (match.management_endpoint == "wifi") {
        return handle_wifi(request, match, server_tx_id);
    } else if (match.management_endpoint == "update") {
        return handle_software_update(request, match, server_tx_id);
    }

    Response response;
    AlpacaResponse alpaca_response = make_error_response(
        0, server_tx_id,
        util::ErrorCode::INVALID_VALUE,
        "Unknown management endpoint"
    );
    response.set_body(alpaca_response);
    return response;
}

nlohmann::json Router::build_description_payload() const {
    nlohmann::json desc;

    // open-astro#392: the Host check settings the web UI edits.
    auto add_host_check_fields = [this](nlohmann::json& target) {
        std::shared_ptr<const std::vector<std::string>> hosts;
        {
            std::lock_guard<std::mutex> lock(allowed_hosts_mutex_);
            hosts = allowed_hosts_;
        }
        target["HostCheckEnabled"] = host_check_enabled_.load(std::memory_order_acquire);
        target["AllowedHosts"] = join_host_list(*hosts);
        // #1509: additive; every key the spec requires stays.
        target["DiscoveryPort"] = discovery_port_.load(std::memory_order_acquire);
    };

    if (management_driver_) {
        desc["ServerName"] = management_driver_->get_name();
        desc["Manufacturer"] = management_driver_->get_manufacturer();
        desc["ManufacturerVersion"] = management_driver_->get_manufacturer_version();
        desc["Location"] = management_driver_->get_location();
        {
            std::lock_guard<std::mutex> lock(server_info_mutex_);
            desc["ProfileName"] = profile_name_;
        }
        add_clock_fields(desc);
        add_host_check_fields(desc);
        return desc;
    }

    std::string server_name;
    std::string manufacturer;
    std::string manufacturer_version;
    std::string location;
    std::string profile_name;
    {
        std::lock_guard<std::mutex> lock(server_info_mutex_);
        server_name = server_name_;
        manufacturer = manufacturer_;
        manufacturer_version = manufacturer_version_;
        location = location_;
        profile_name = profile_name_;
    }

    desc["ServerName"] = server_name;
    desc["Manufacturer"] = manufacturer;
    desc["ManufacturerVersion"] = manufacturer_version;
    desc["Location"] = location;
    desc["ProfileName"] = profile_name;
    add_clock_fields(desc);
    add_host_check_fields(desc);
    return desc;
}

// open-astro#289/#292: a telescope driver about to compute LST from a clock
// nothing has disciplined. Says which of the three states it is in and what
// will, or will not, correct it: a client UTCDate write is still to come, or
// setting the clock has been refused, or clients are not allowed to. Called
// from both connect paths (legacy PUT connected and the ITelescopeV4 PUT
// connect initiator).
void Router::warn_if_clock_undisciplined(alpacacore::AlpacaDriver& device) const {
    if (device.get_device_type() != alpacacore::DeviceType::Telescope || host_clock_.synchronized() ||
        host_clock_.stepped_by_client()) {
        return;
    }
    // open-astro#292: a clock the kernel loaded from a hardware RTC at boot is
    // usually right to seconds, so it is INFO -- but still a WARN when nothing
    // is allowed to correct it.
    // has_rtc() is a memory read: the probe behind it runs at startup and on
    // the server's RTC probe thread, never here (open-astro#314). It used to read
    // sysfs inline, which on a bus-attached RTC is an I2C transaction that can
    // block for the adapter timeout, on the connect initiator ascom-contract.instructions.md times
    // against the 1 s STANDARD target and with the connection op mutex held.
    const bool rtc = host_clock_.has_rtc();
    // enabled() alone does not mean "a client will correct it": without
    // CAP_SYS_TIME every step is refused and the clock is never corrected at
    // all, which is the one state that must not be quiet at connect.
    // A refused step is checked before the opt-out: Sync Time is the same
    // clock_settime in this process, so once the kernel has refused one, that
    // button is dead too and must not be recommended by either branch.
    const bool refused = host_clock_.step_ever_failed();
    const bool correctable = host_clock_.enabled() && !refused;
    const std::string msg =
        "Telescope " + std::to_string(device.get_device_number()) +
        (rtc ? " connecting on the hardware RTC's time (no NTP; the RTC's accuracy is unverified, nothing has "
               "checked it since it was last set)"
             : " connecting with an undisciplined host clock (no NTP, not yet set by a client)") +
        (correctable ? "; goto/LST math runs on it until a client writes UTCDate"
         : refused   ? "; goto/LST math runs on it and nothing in this process can correct it (setting the clock was "
                       "refused: no CAP_SYS_TIME, which the packaged systemd unit grants). Set the clock outside the "
                       "service, e.g. scripts/sync-clock.sh over SSH"
                   : "; goto/LST math runs on it, and syncSystemClockFromClients is off so no client will correct it. "
                     "Use the web UI's Sync Time");
    if (rtc && correctable) {
        util::log_info(msg);
    } else {
        util::log_warning(msg);
    }
}

// open-astro#289: clock state next to the server identity so the web UI and
// clients can see whether pointing math is running on a trusted clock.
void Router::add_clock_fields(nlohmann::json& desc) const {
    desc["ClockSynchronized"] = host_clock_.synchronized();
    desc["ClockSource"] = host_clock_.source();  // "ntp" | "client" | "rtc" | "none"
    desc["SyncSystemClockFromClients"] = host_clock_.enabled();
    // open-astro#354: the host's IANA zone ("" when unknown) so the web UI
    // header clock can render in the same zone as the log lines.
    desc["TimeZone"] = util::host_time_zone();
}

Response Router::handle_description(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    response.set_content_type("application/json");

    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
    }

    // Issue #348: the whole management surface is unauthenticated under the
    // documented trusted-LAN threat model, which is a deliberate stance; the
    // point is that every state-changing endpoint should take that stance on
    // purpose rather than differ by accident. A POST with Content-Type:
    // text/plain is not preflighted and the handlers parse the body
    // regardless of content type, so nothing on the browser side stops a
    // drive-by request from reaching this.
    // PUT/POST here rewrites the server description and the
    // SyncSystemClockFromClients opt-out, and that opt-out is what keeps a
    // cross-origin clock step from being accepted at all -- so leaving this
    // endpoint unguarded would have handed back the guard on synctime.
    if (auto rejected = reject_cross_origin_request(request, client_tx_id, server_tx_id, "server description")) {
        return *rejected;
    }

    try {
        if (request.method() == HttpMethod::POST || request.method() == HttpMethod::PUT) {
            if (request.body().empty()) {
                AlpacaResponse err = make_error_response(
                    client_tx_id, server_tx_id,
                    util::ErrorCode::VALUE_NOT_SET,
                    "Missing request body"
                );
                response.set_body(err);
                response.set_status(400, "Bad Request");  // #1476: a write it cannot interpret (ADR 0007)
                return response;
            }

            auto json_opt = parse_json(request.body());
            if (!json_opt) {
                AlpacaResponse err = make_error_response(
                    client_tx_id, server_tx_id,
                    util::ErrorCode::INVALID_VALUE,
                    "Invalid JSON payload"
                );
                response.set_body(err);
                response.set_status(400, "Bad Request");  // #1476
                return response;
            }

            const auto& body = *json_opt;
            auto body_client_tx = extract_client_transaction_id(body);
            if (body_client_tx != 0) {
                client_tx_id = body_client_tx;
            }

            // #1476: a wrong-type field is an invalid value (400), never a
            // type_error from get<std::string>(). The first spelling present wins.
            const auto read_string_field = [&body](std::initializer_list<const char*> keys) {
                std::optional<std::string> value;
                for (const char* key : keys) {
                    if (!body.contains(key)) continue;
                    if (!body[key].is_string()) throw_invalid_value(std::string("Invalid value for ") + key);
                    value = body[key].get<std::string>();
                    break;
                }
                return value;
            };
            const std::optional<std::string> new_location = read_string_field({"Location", "location"});
            const std::optional<std::string> new_profile_name =
                read_string_field({"ProfileName", "profileName", "profile_name"});

            std::optional<bool> new_sync_clock;
            for (const char* key :
                 {"SyncSystemClockFromClients", "syncSystemClockFromClients", "sync_system_clock_from_clients"}) {
                if (body.contains(key)) {
                    const auto& v = body[key];
                    bool parsed = false;
                    if (v.is_boolean()) {
                        parsed = v.get<bool>();
                    } else if (v.is_string()) {
                        parsed = parse_bool_value(v.get<std::string>(), key);
                    } else {
                        throw_invalid_value(std::string("Invalid value for ") + key);
                    }
                    new_sync_clock = parsed;
                    break;
                }
            }

            // open-astro#392: the Host check settings. Types are checked
            // here, before anything is applied.
            std::optional<bool> new_host_check;
            for (const char* key : {"HostCheckEnabled", "hostCheckEnabled", "host_check_enabled"}) {
                if (body.contains(key)) {
                    const auto& v = body[key];
                    if (v.is_boolean()) {
                        new_host_check = v.get<bool>();
                    } else if (v.is_string()) {
                        new_host_check = parse_bool_value(v.get<std::string>(), key);
                    } else {
                        throw_invalid_value(std::string("Invalid value for ") + key);
                    }
                    break;
                }
            }
            std::optional<std::vector<std::string>> new_allowed_hosts;
            for (const char* key : {"AllowedHosts", "allowedHosts", "allowed_hosts"}) {
                if (body.contains(key)) {
                    if (!body[key].is_string()) {
                        throw_invalid_value(std::string("Invalid value for ") + key);
                    }
                    std::vector<std::string> normalized_hosts;
                    const auto problem =
                        allowed_hosts_problem(split_host_list(body[key].get<std::string>()), normalized_hosts);
                    if (!problem.empty()) {
                        response.set_body(
                            make_error_response(client_tx_id, server_tx_id, util::ErrorCode::INVALID_VALUE, problem));
                        response.set_status(400, "Bad Request");
                        return response;
                    }
                    new_allowed_hosts = std::move(normalized_hosts);
                    break;
                }
            }
            // #1509: discovery.port, a JSON integer or a numeric string, 1-65535.
            std::optional<std::uint16_t> new_discovery_port;
            for (const char* key : {"DiscoveryPort", "discoveryPort", "discovery_port"}) {
                if (body.contains(key)) {
                    const auto& v = body[key];
                    long long parsed = 0;
                    bool numeric = false;
                    if (v.is_number_integer()) {
                        parsed = v.get<long long>();
                        numeric = true;
                    } else if (v.is_string()) {
                        const std::string text = v.get<std::string>();
                        if (!text.empty() && text.size() <= 5 &&
                            std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); })) {
                            parsed = std::stoll(text);
                            numeric = true;
                        }
                    }
                    if (!numeric || parsed < 1 || parsed > 65535) {
                        response.set_body(make_error_response(client_tx_id, server_tx_id,
                                                              util::ErrorCode::INVALID_VALUE,
                                                              "DiscoveryPort must be an integer from 1 to 65535"));
                        response.set_status(400, "Bad Request");
                        return response;
                    }
                    new_discovery_port = static_cast<std::uint16_t>(parsed);
                    break;
                }
            }
            if (!new_location && !new_profile_name && !new_sync_clock && !new_host_check && !new_allowed_hosts &&
                !new_discovery_port) {
                AlpacaResponse err = make_error_response(
                    client_tx_id, server_tx_id, util::ErrorCode::VALUE_NOT_SET,
                    "Request must include a 'Location', 'ProfileName', 'SyncSystemClockFromClients', "
                    "'HostCheckEnabled', 'AllowedHosts' or 'DiscoveryPort' property");
                response.set_body(err);
                response.set_status(400, "Bad Request");  // #1476
                return response;
            }

            if (new_location && management_driver_) {
                AlpacaResponse err =
                    make_error_response(client_tx_id, server_tx_id, util::ErrorCode::INVALID_OPERATION,
                                        "Location updates are not supported when a management driver is active");
                response.set_body(err);
                return response;
            }

            // open-astro#392: validate, persist and apply as one step.
            std::lock_guard<std::mutex> write_lock(description_write_mutex_);

            const auto refuse_with_400 = [&](const std::string& message) {
                response.set_body(
                    make_error_response(client_tx_id, server_tx_id, util::ErrorCode::INVALID_VALUE, message));
                response.set_status(400, "Bad Request");
                return response;
            };

            const bool host_settings_carried = new_host_check.has_value() || new_allowed_hosts.has_value();
            std::vector<std::string> current_hosts;
            {
                std::lock_guard<std::mutex> lock(allowed_hosts_mutex_);
                current_hosts = *allowed_hosts_;
            }
            const bool current_host_check = host_check_enabled_.load(std::memory_order_acquire);

            // No self-lockout: the request that turns the check on, or edits
            // the list while it is on, must itself pass the new settings.
            // Same predicate as route().
            const bool resulting_host_check = new_host_check.value_or(current_host_check);
            const std::vector<std::string>& resulting_hosts = new_allowed_hosts ? *new_allowed_hosts : current_hosts;
            if (host_settings_carried && resulting_host_check) {
                const std::string host = request.get_header("Host");
                if (!host.empty()) {
                    const auto name = normalize_host(host);
                    if (!name || name->empty() || !host_allowed(*name, resulting_hosts, machine_hostname_)) {
                        std::string shown = host.substr(0, 255);
                        std::replace_if(
                            shown.begin(), shown.end(), [](unsigned char c) { return c < 0x20 || c > 0x7e; }, '?');
                        return refuse_with_400(
                            "Host '" + shown +
                            "' would be refused by these settings; add it to the allowed host names or use the "
                            "IP address");
                    }
                }
            }

            std::string config_path;
            {
                std::lock_guard<std::mutex> lock(server_info_mutex_);
                config_path = config_path_;
            }

            if (!config_path.empty()) {
                std::vector<std::pair<std::string, std::string>> persist_values;
                std::vector<std::pair<std::string, std::string>> persist_http_values;
                if (new_host_check) {
                    persist_http_values.emplace_back("host_check_enabled", *new_host_check ? "true" : "false");
                }
                if (new_allowed_hosts) {
                    persist_http_values.emplace_back("allowed_hosts", join_host_list(*new_allowed_hosts));
                }
                if (new_location) {
                    persist_values.emplace_back("location", *new_location);
                }
                if (new_profile_name) {
                    persist_values.emplace_back("profile_name", *new_profile_name);
                }
                if (new_sync_clock) {
                    persist_values.emplace_back("sync_system_clock_from_clients", *new_sync_clock ? "true" : "false");
                }
                std::vector<std::pair<std::string, std::string>> persist_discovery_values;
                if (new_discovery_port) {
                    persist_discovery_values.emplace_back("port", std::to_string(*new_discovery_port));
                }
                std::string persist_error;
                if (!update_config_values(config_path,
                                          {{"server", persist_values},
                                           {"http", persist_http_values},
                                           {"discovery", persist_discovery_values}},
                                          persist_error)) {
                    AlpacaResponse err = make_error_response(client_tx_id, server_tx_id, util::ErrorCode::DRIVER_ERROR,
                                                             "Failed to persist server settings: " + persist_error);
                    response.set_body(err);
                    return response;
                }
            }

            {
                std::lock_guard<std::mutex> lock(server_info_mutex_);
                if (new_location) {
                    location_ = *new_location;
                }
                if (new_profile_name) {
                    profile_name_ = *new_profile_name;
                }
            }
            if (new_sync_clock) {
                host_clock_.set_enabled(*new_sync_clock);
                util::log_info(std::string("syncSystemClockFromClients ") + (*new_sync_clock ? "enabled" : "disabled") +
                               " by " + request.remote_address());
            }
            // Never judge a request by the new flag with the old list: turn
            // the check on after the list, off before it.
            if (new_host_check && !*new_host_check) {
                set_host_check_enabled(false);
            }
            if (new_allowed_hosts) {
                set_allowed_hosts(*new_allowed_hosts);
            }
            if (new_host_check && *new_host_check) {
                set_host_check_enabled(true);
            }
            if (new_discovery_port) {
                set_discovery_port(*new_discovery_port);
                util::log_info("Discovery port set to " + std::to_string(*new_discovery_port) + " by " +
                               request.remote_address() + "; it applies when the service restarts");
            }
            if (new_host_check || new_allowed_hosts) {
                util::log_info("Host check settings changed by " + request.remote_address() +
                               ": enabled=" + (host_check_enabled_.load(std::memory_order_acquire) ? "true" : "false") +
                               ", allowed hosts='" + join_host_list(resulting_hosts) + "'");
            }
        } else if (request.method() != HttpMethod::GET) {
            // Alpaca API Reference s2.5: a method the path does not define is a
            // text message with HTTP 400 (#1508).
            set_wrong_verb_text(response, "description");
            return response;
        }

        nlohmann::json desc = build_description_payload();
        AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
        alpaca_response.value = desc;
        response.set_body(alpaca_response);
    } catch (const std::exception& e) {
        const bool is_write = request.method() == HttpMethod::POST || request.method() == HttpMethod::PUT;
        // A JSON type_error in a write is a field the server cannot interpret,
        // whatever field a later change adds without its own type check.
        const bool type_mismatch = dynamic_cast<const nlohmann::json::type_error*>(&e) != nullptr;
        const auto error_code =
            is_write && type_mismatch ? util::ErrorCode::INVALID_VALUE : util::exception_to_error_code(e);
        AlpacaResponse alpaca_response = make_error_response(client_tx_id, server_tx_id, error_code, e.what());
        response.set_body(alpaca_response);
        // #1476: an invalid field value in a write is a request the server
        // cannot interpret, answered 400 like the AllowedHosts refusal. A valid
        // write that fails (persist error, management driver) stays HTTP 200.
        // A client can trigger the 400, so it logs at DEBUG (ADR 0007 rule 9).
        if (is_write && error_code == util::ErrorCode::INVALID_VALUE) {
            util::log_debug("Refused description write: " + util::escape_for_log(e.what()));
            response.set_status(400, "Bad Request");
        } else {
            util::log_error(std::string(is_write ? "Error writing description: " : "Error getting description: ") +
                            e.what());
        }
    }

    return response;
}

Response Router::handle_configured_devices(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    
    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
    }

    // #1486: the Management API defines this path for GET only.
    if (request.method() != HttpMethod::GET) {
        set_wrong_verb_text(response, "configureddevices");  // #1508: 400 + text, not 405
        return response;
    }

    try {
        load_persisted_devices();

        // Snapshot under the mutex so the iteration below (which calls into
        // the device registry) never races configure/remove handlers.
        std::vector<nlohmann::json> persisted_snapshot;
        {
            std::lock_guard<std::mutex> lock(persisted_devices_mutex_);
            persisted_snapshot = persisted_devices_;
        }

        auto find_config = [&persisted_snapshot](const std::string& device_type,
                                                 int device_number) -> const nlohmann::json* {
            std::string target_type = device_type;
            std::transform(target_type.begin(), target_type.end(), target_type.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            for (const auto& entry : persisted_snapshot) {
                const auto key = persisted_key(entry);
                if (key && key->device_type == target_type && key->device_number == device_number) {
                    return &entry;
                }
            }
            return nullptr;
        };

        // Get devices from AlpacaCore registry
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        auto capabilities = registry.get_all_device_capabilities();

        nlohmann::json devices = nlohmann::json::array();
        for (const auto& cap : capabilities) {
            nlohmann::json device;
            device["DeviceName"] = cap.name;
            device["DeviceType"] = alpacacore::device_type_to_string(cap.type);
            device["DeviceNumber"] = cap.device_number;
            device["UniqueID"] = cap.unique_id;

            if (const auto* config = find_config(device["DeviceType"].get<std::string>(), cap.device_number)) {
                device["Vendor"] = config->value("vendor", "");
                device["Config"] = *config;
            }

            // Surface the device's hardware firmware and/or vendor SDK version in
            // the web UI only. These are deliberately NOT added to the ASCOM
            // DriverInfo string so NINA and other Alpaca clients keep a clean
            // driver string. Each field is present only when the live driver
            // reports a value (the hooks return std::nullopt when unknown /
            // disconnected). Firmware and SdkVersion are distinct: Firmware is
            // the device's own firmware, SdkVersion is the vendor library version.
            if (auto driver = registry.get_device(cap.type, cap.device_number)) {
                // Connected feeds the web UI status dot (green/yellow). Omitted
                // when the call throws, so the UI shows "unknown" (red).
                // get_connecting() is lock-free; a driver mid-connect can hold its
                // coarse mutex across get_connected() for the whole connect, so
                // report false without asking (as the Connected endpoint does).
                try {
                    device["Connected"] = !driver->get_connecting() && driver->get_connected();
                } catch (const std::exception& e) {
                    util::log_warning("Connected query failed for " + cap.name + ": " + e.what());
                }
                // LinkFault: the latched link-health text of a driver that keeps
                // one. Connected stays true while it stands. Only when non-empty.
                try {
                    if (auto link_fault = driver->get_link_fault(); !link_fault.empty()) {
                        device["LinkFault"] = std::move(link_fault);
                    }
                } catch (const std::exception& e) {
                    util::log_warning("LinkFault query failed for " + cap.name + ": " + e.what());
                }
                try {
                    if (auto firmware = driver->get_device_firmware(); firmware.has_value()) {
                        device["Firmware"] = *firmware;
                    }
                } catch (const std::exception& e) {
                    util::log_warning("Firmware query failed for " + cap.name + ": " + e.what());
                }
                try {
                    if (auto sdk = driver->get_device_sdk_version(); sdk.has_value()) {
                        device["SdkVersion"] = *sdk;
                    }
                } catch (const std::exception& e) {
                    util::log_warning("SDK version query failed for " + cap.name + ": " + e.what());
                }

                // Issue #358, the Platform 7 half. PUT /connect returns success
                // immediately by design and completion is observed through
                // Connecting, so when the task fails there is no response left
                // to carry an error: the client -- NINA 3.x prefers this path
                // -- sees Connecting go false and Connected stay false, with
                // no message anywhere in the protocol. The reason has nowhere
                // to go in ASCOM, so it surfaces here instead, where the web
                // UI can show the operator what the driver actually said.
                // Present only while a failure stands; the next attempt clears
                // it. Like Firmware and SdkVersion, deliberately not part of
                // any ASCOM response.
                // try/catch like the two hooks above it: every implementation
                // today is the macro (a mutex and a string copy) so nothing can
                // throw in practice, but an override that does must not take
                // the whole device listing down with it.
                try {
                    if (std::string reason = driver->get_last_connect_error(); !reason.empty()) {
                        device["LastConnectError"] = reason;
                    }
                } catch (const std::exception& e) {
                    util::log_warning("Connect-error query failed for " + cap.name + ": " + e.what());
                }
            }
            devices.push_back(device);
        }

        AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
        alpaca_response.value = devices;
        response.set_body(alpaca_response);
    } catch (const std::exception& e) {
        util::log_error("Error getting configured devices: " + std::string(e.what()));
        AlpacaResponse alpaca_response =
            make_error_response(client_tx_id, server_tx_id, util::exception_to_error_code(e), e.what());
        response.set_body(alpaca_response);
    }

    return response;
}

Response Router::handle_failed_devices(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    response.set_content_type("application/json");

    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
    }

    if (request.method() != HttpMethod::GET) {
        AlpacaResponse alpaca_response =
            make_error_response(client_tx_id, server_tx_id, util::ErrorCode::INVALID_OPERATION,
                                "Unsupported HTTP method for faileddevices endpoint");
        response.set_body(alpaca_response);
        response.set_status(405, "Method Not Allowed");
        response.set_header("Allow", "GET");
        return response;
    }

    try {
        load_persisted_devices();

        std::vector<nlohmann::json> persisted_snapshot;
        {
            std::lock_guard<std::mutex> lock(persisted_devices_mutex_);
            persisted_snapshot = persisted_devices_;
        }
        auto capabilities = alpacacore::management::DeviceRegistry::instance().get_all_device_capabilities();

        nlohmann::json devices = nlohmann::json::array();
        for (const auto& entry : persisted_snapshot) {
            const auto pkey = persisted_key(entry);
            std::string ptype = pkey ? pkey->device_type : "";
            int pnum = pkey ? pkey->device_number : -1;
            bool already_listed = false;
            for (const auto& cap : capabilities) {
                std::string cap_type = alpacacore::device_type_to_string(cap.type);
                std::transform(cap_type.begin(), cap_type.end(), cap_type.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::string entry_type = ptype;
                std::transform(entry_type.begin(), entry_type.end(), entry_type.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (cap_type == entry_type && cap.device_number == pnum) {
                    already_listed = true;
                    break;
                }
            }
            if (!already_listed) {
                nlohmann::json device;
                device["DeviceName"] = entry.value("vendor", "unknown") + " (failed to load)";
                device["DeviceType"] = ptype;
                device["DeviceNumber"] = pnum;
                device["UniqueID"] = stored_unique_id(entry);  // decision 0018
                device["Vendor"] = entry.value("vendor", "");
                device["Config"] = entry;
                device["LoadError"] = true;
                devices.push_back(device);
            }
        }

        AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
        alpaca_response.value = devices;
        response.set_body(alpaca_response);
    } catch (const std::exception& e) {
        util::log_error("Error getting failed devices: " + std::string(e.what()));
        AlpacaResponse alpaca_response =
            make_error_response(client_tx_id, server_tx_id, util::exception_to_error_code(e), e.what());
        response.set_body(alpaca_response);
    }

    return response;
}

Response Router::handle_device(const Request& request, const RouteMatch& match, std::uint32_t server_tx_id) {
    Response response;

    // One DeviceCall per request: it reads the client transaction id and parses
    // the body at most once for every dispatcher below.
    const DeviceCall call(request, server_tx_id);
    const std::uint32_t client_tx_id = call.client_tx_id();

    if (!is_lowercase_ascii(match.device_type) || !is_known_device_type_name(match.device_type)) {
        // Alpaca spec: HTTP 400 indicates the device could not interpret the
        // request, e.g. a misspelt device type or invalid device number.
        set_bad_request_text(response, "Unknown device type: " + match.device_type);
        return response;
    }

    // #574: a device-number digit string that doesn't fit in a uint32_t
    // (overflowed to a wrapped value, or overflowed even a 64-bit parse)
    // used to reach the device lookup as the wrapped/default number, or
    // throw out_of_range and come back as an internal-error 200. Reject it
    // the same way an unknown device type is rejected, above.
    if (match.device_number_invalid) {
        set_bad_request_text(response, "Invalid device number for device type: " + match.device_type);
        return response;
    }

    try {
        // Convert device type string to enum
        alpacacore::DeviceType device_type = string_to_device_type(match.device_type);

        const unsigned method_verbs =
            is_lowercase_ascii(match.method_name) ? allowed_verbs(device_type, match.method_name) : 0;
        if (method_verbs == 0) {
            set_bad_request_text(response, "Unknown method: " + match.method_name);
            return response;
        }

        // #574: reject a request whose verb the method does not accept (a PUT
        // to a GET property, a GET to a PUT command, any POST or DELETE)
        // before it reaches the ASCOM operation
        // (.claude/skills/ascom-alpaca-protocol/references/http-api-contract.md)
        // instead of letting it fall through to a "not yet implemented"
        // dispatcher fallback that answered HTTP 200 with 0x400. A foreign
        // Origin is refused with 403 first, so UTCDate's #401 guard (which
        // runs for every verb) keeps its 403 for a forged POST; the Site*
        // guards (#444) sit in their PUT branch only, so a forged POST there
        // now gets 403 too, where it used to reach the 200/0x400 fallback.
        const unsigned request_verb = request.method() == HttpMethod::GET   ? kVerbGet
                                      : request.method() == HttpMethod::PUT ? kVerbPut
                                                                            : 0U;
        if ((method_verbs & request_verb) == 0) {
            if (auto rejected = reject_cross_origin_request(request, client_tx_id, server_tx_id, "device method")) {
                return *rejected;
            }
            set_bad_request_text(
                response, "Method '" + match.method_name + "' does not accept " + http_method_name(request.method()));
            return response;
        }

        // Get device from registry
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        // #627: the registry keys devices by int, so a valid uint32 number
        // above INT_MAX names no device; it takes the not-found reply below
        // instead of a narrowing cast (4294967295 would look up -1).
        std::shared_ptr<alpacacore::AlpacaDriver> device;
        if (match.device_number <= static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            device = registry.get_device(device_type, static_cast<int>(match.device_number));
        }

        if (!device) {
            set_bad_request_text(response,
                                 "Device not found: " + match.device_type + " #" + std::to_string(match.device_number));
            return response;
        }

        // Any request from a registered client refreshes its Connected
        // registration so routine polling keeps it from expiring as stale
        // (issue #160).
        touch_client_connection(device.get(), extract_client_key(request).key);

        // open-astro#547: the single device-dispatch choke point for the
        // client-silence motion watchdog. Any request addressed to this
        // telescope -- including the client's own Slewing polls -- counts
        // as activity, with no per-endpoint list to keep in sync.
        //
        // A long-running call can block inside dispatch_device_method() below
        // for longer than the watchdog interval, while THIS client is
        // actively waiting on its own response -- note_client_activity()
        // only stamps once, at intake, so on its own it does not cover that
        // (review finding: the watchdog could abort a client's own in-flight
        // slew). in_flight_guard keeps this request counted as activity for
        // its whole duration via RAII, so it still decrements on an
        // exception out of dispatch.
        auto* telescope = dynamic_cast<alpacacore::TelescopeDriver*>(device.get());
        if (telescope != nullptr) {
            telescope->note_client_activity(std::chrono::steady_clock::now());
        }
        struct InFlightGuard {
            alpacacore::TelescopeDriver* driver;
            explicit InFlightGuard(alpacacore::TelescopeDriver* d) : driver(d) {
                if (driver != nullptr) {
                    driver->begin_client_request();
                }
            }
            ~InFlightGuard() {
                if (driver != nullptr) {
                    driver->end_client_request();
                }
            }
            InFlightGuard(const InFlightGuard&) = delete;
            InFlightGuard& operator=(const InFlightGuard&) = delete;
        } in_flight_guard(telescope);

        // Dispatch the method call
        return dispatch_device_method(device, match.method_name, call);

    } catch (const std::exception& e) {
        util::log_error("Device error: " + std::string(e.what()));
        auto error_code = util::exception_to_error_code(e);
        AlpacaResponse alpaca_response = make_error_response(client_tx_id, server_tx_id, error_code, e.what());
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
    }

    return response;
}

alpacacore::DeviceType Router::string_to_device_type(const std::string& type_str) const {
    if (type_str == "camera") return alpacacore::DeviceType::Camera;
    if (type_str == "telescope") return alpacacore::DeviceType::Telescope;
    if (type_str == "filterwheel") return alpacacore::DeviceType::FilterWheel;
    if (type_str == "focuser") return alpacacore::DeviceType::Focuser;
    if (type_str == "rotator") return alpacacore::DeviceType::Rotator;
    if (type_str == "dome") return alpacacore::DeviceType::Dome;
    if (type_str == "switch") return alpacacore::DeviceType::Switch;
    if (type_str == "covercalibrator") return alpacacore::DeviceType::CoverCalibrator;
    if (type_str == "observingconditions") return alpacacore::DeviceType::ObservingConditions;
    if (type_str == "safetymonitor") return alpacacore::DeviceType::SafetyMonitor;

    throw std::runtime_error("Unknown device type: " + type_str);
}

namespace {
// A Device API request the router cannot interpret: HTTP 400 with a short
// text/plain message (Alpaca API Reference s2.5 and the Device API 400 text).
void set_bad_request_text(Response& response, const std::string& message) {
    response.set_status(400, "Bad Request");
    response.set_content_type("text/plain");
    response.set_body(message);
}

void set_wrong_verb_text(Response& response, const char* what) {
    response.set_status(400, "Bad Request");
    response.set_content_type("text/plain");
    response.set_body(std::string("Unsupported HTTP method for ") + what + " endpoint");
}
}  // namespace

namespace {
void prune_stale_client_connections(std::unordered_map<std::string, std::chrono::steady_clock::time_point>& clients) {
    const auto cutoff = std::chrono::steady_clock::now() - kClientConnectionStaleAfter;
    for (auto it = clients.begin(); it != clients.end();) {
        if (it->second < cutoff) {
            it = clients.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace

std::size_t Router::register_client_connection(const void* device, const std::string& client_key) {
    std::lock_guard<std::mutex> lock(client_connections_mutex_);
    auto& clients = client_connections_[device];
    prune_stale_client_connections(clients);
    clients[client_key] = std::chrono::steady_clock::now();
    return clients.size();
}

std::size_t Router::unregister_client_connection(const void* device, const std::string& client_key) {
    std::lock_guard<std::mutex> lock(client_connections_mutex_);
    auto it = client_connections_.find(device);
    if (it == client_connections_.end()) {
        return 0;
    }
    it->second.erase(client_key);
    prune_stale_client_connections(it->second);
    if (it->second.empty()) {
        client_connections_.erase(it);
        return 0;
    }
    return it->second.size();
}

bool Router::client_connection_registered(const void* device, const std::string& client_key) {
    std::lock_guard<std::mutex> lock(client_connections_mutex_);
    auto it = client_connections_.find(device);
    if (it == client_connections_.end()) {
        return false;
    }
    auto client_it = it->second.find(client_key);
    if (client_it == it->second.end()) {
        return false;
    }
    // A lookup is client activity: refresh so steady polling never goes stale.
    client_it->second = std::chrono::steady_clock::now();
    return true;
}

void Router::touch_client_connection(const void* device, const std::string& client_key) {
    std::lock_guard<std::mutex> lock(client_connections_mutex_);
    auto it = client_connections_.find(device);
    if (it == client_connections_.end()) {
        return;
    }
    auto client_it = it->second.find(client_key);
    if (client_it != it->second.end()) {
        client_it->second = std::chrono::steady_clock::now();
    }
}

void Router::clear_client_connections(const void* device) {
    std::lock_guard<std::mutex> lock(client_connections_mutex_);
    client_connections_.erase(device);
    // connection_op_mutexes_ is deliberately NOT erased: this runs while the
    // op mutex is held (dead-link sweep inside a PUT handler), and erasing
    // would let a concurrent op mint a fresh mutex and bypass serialization.
    // The map is bounded by registered-device count; a recycled pointer
    // sharing an old mutex only means harmless extra serialization.
}

void Router::purge_device_connection_state(const void* device) {
    std::lock_guard<std::mutex> lock(client_connections_mutex_);
    client_connections_.erase(device);
    connection_op_mutexes_.erase(device);
}

bool Router::device_is_current(const std::shared_ptr<alpacacore::AlpacaDriver>& device) {
    auto& registry = alpacacore::management::DeviceRegistry::instance();
    return registry.get_device(device->get_device_type(), device->get_device_number()) == device;
}

std::shared_ptr<std::mutex> Router::device_connection_op_mutex(
    const std::shared_ptr<alpacacore::AlpacaDriver>& device) {
    std::lock_guard<std::mutex> lock(client_connections_mutex_);
    auto it = connection_op_mutexes_.find(device.get());
    if (it != connection_op_mutexes_.end()) {
        return it->second;
    }
    // Insert only for a device the registry still resolves: a straggler op on
    // a removed device (purge_device_connection_state already ran) gets an
    // ephemeral mutex instead of re-inserting an entry nobody will ever reap
    // (PR #164 review). Lock order: client_connections_mutex_ -> registry
    // mutex; the registry never calls back into the router.
    if (!device_is_current(device)) {
        return std::make_shared<std::mutex>();
    }
    auto mutex = std::make_shared<std::mutex>();
    connection_op_mutexes_[device.get()] = mutex;
    return mutex;
}

Response Router::dispatch_device_method(std::shared_ptr<alpacacore::AlpacaDriver> device,
                                        const std::string& method_name, const DeviceCall& call) {
    const Request& request = call.request();

    return call.run("device method", [&]() -> Response {
        // Handle common AlpacaDriver methods
        if (method_name == "name") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(device->get_name());
            }
        }
        else if (method_name == "description") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(device->get_description());
            }
        }
        else if (method_name == "driverinfo") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(device->get_driver_info());
            }
        }
        else if (method_name == "driverversion") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(device->get_driver_version());
            }
        }
        else if (method_name == "interfaceversion") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(static_cast<std::int32_t>(device->get_interface_version()));
            }
        }
        else if (method_name == "connected") {
            if (request.method() == HttpMethod::GET) {
                // A dead upstream link (failed or dropped, not mid-transition)
                // invalidates every client's registration so all observers see
                // the disconnect and reconnect explicitly (issue #160).
                // try_lock, not lock: if a connection op is in flight the
                // state is mid-transition and the sweep must skip — and it
                // must not run inside PUT's register-then-connect gap, where
                // the device still looks dead and the sweep would wipe the
                // registration just added. Never blocks a polling GET behind
                // a slow connect.
                const auto op_mutex = device_connection_op_mutex(device);
                if (op_mutex->try_lock()) {
                    std::lock_guard<std::mutex> op_lock(*op_mutex, std::adopt_lock);
                    if (!device->get_connecting() && !device->get_connected()) {
                        clear_client_connections(device.get());
                    }
                }
                // get_connecting() first, and short-circuit: it is the one
                // non-blocking signal the driver base guarantees, while a
                // driver's get_connected() may take the state mutex that its
                // connect sequence holds for the whole handshake (SynScan was
                // the original: 25 s on a silent handset, issue #130, whose
                // fix made that getter lock-free; the telescopes named there still have
                // the shape -- see async_connectable.h). Reading it
                // mid-transition stalled this poll for the entire connect,
                // the very client timeout the PUT wait below exists to
                // prevent. While a task is in flight the answer is false: a
                // connect is not connected yet, and a disconnect is on its way
                // down (the drivers clear the flag at the start of teardown).
                const bool device_connected = !device->get_connecting() && device->get_connected();
                const ClientKey client = extract_client_key(request);
                bool value;
                if (!client.has_client_id) {
                    // No ClientID → can't answer per-client; report device state.
                    value = device_connected;
                } else {
                    // Per-client semantics: a client only reads true if IT
                    // connected, not merely because another client did.
                    value = device_connected && client_connection_registered(device.get(), client.key);
                }
                return call.ok(value);
            }
            else if (request.method() == HttpMethod::PUT) {
                const bool connected = call.get_bool("Connected");

                // Per-client Connected refcounting (issue #160): the first
                // client in powers the upstream link, the last one out turns
                // it off. A dead link (not mid-transition) invalidates stale
                // registrations first so they can't block a fresh connect or
                // pin the decision below. The per-device op mutex makes the
                // whole decision + driver call atomic against other clients'
                // connection ops: without it, a client connecting during
                // another client's last-out teardown window could register
                // against a link that is about to drop and be left believing
                // it holds a live connection.
                const std::string client_key = extract_client_key(request).key;
                const auto op_mutex = device_connection_op_mutex(device);
                std::lock_guard<std::mutex> op_lock(*op_mutex);
                if (!device->get_connecting() && !device->get_connected()) {
                    clear_client_connections(device.get());
                }

                if (connected) {
                    // Straggler guard: a request that fetched the device
                    // before a concurrent removedevice must not re-insert a
                    // registration nobody will reap (PR #164 review).
                    if (!device_is_current(device)) {
                        throw alpacacore::AlpacaException("Device has been removed",
                                                          alpacacore::AlpacaError::InvalidOperation);
                    }
                    register_client_connection(device.get(), client_key);
                }

                // get_connecting() is read first at every step here: a
                // driver's get_connected() may block on the state mutex its
                // connect sequence holds for the whole handshake (the SynScan
                // hand controller was the original, issue #130; its getter is
                // lock-free now, the telescopes named in async_connectable.h
                // still block), and calling it
                // while a task is in
                // flight stalled this handler for the entire connect, so the
                // wait deadline below never fired. A connect requested while a
                // task is in flight is still handed to the driver: the base
                // class queues it against an in-flight disconnect and drops
                // it against an in-flight connect
                // (.github/instructions/alpaca-http-conformance.instructions.md).
                if (connected && (device->get_connecting() || !device->get_connected())) {
                    // Start the async connect, then wait for it: Connected is
                    // synchronous in ASCOM, so this returns only once the
                    // device is connected or with the error (issue #776). The
                    // wait used to end at 8 s and reply success with the
                    // connect still running, which a connect that then failed
                    // turned into a success the client could never take back.
                    //
                    // Bounded, not open-ended (connect_wait_limit(), 60 s by
                    // default): the slowest connects known are a CFW3 boot
                    // (~17 s) and a first connect that has to scan first (two
                    // CFW3 boots, the 5.5 s iOptron Wi-Fi sweep), all well
                    // inside it, and a driver whose handshake never returns
                    // must not hold this worker and the device's op mutex
                    // forever. Other devices and every GET on this one stay
                    // served meanwhile: only this device's PUT connect /
                    // disconnect queue behind the op mutex, as they did for
                    // the old 8 s.
                    warn_if_clock_undisciplined(*device);
                    try {
                        device->connect();
                    } catch (const alpacacore::AlpacaException& e) {
                        // A synchronous connect() reports its own failure.
                        unregister_client_connection(device.get(), client_key);
                        // ASCOM Connected: "Do not use a NotConnectedException
                        // here".
                        if (e.error_code() == alpacacore::AlpacaError::NotConnected) {
                            throw alpacacore::AlpacaException(e.what(), alpacacore::AlpacaError::DriverException);
                        }
                        throw;
                    } catch (...) {
                        unregister_client_connection(device.get(), client_key);
                        throw;
                    }
                    const auto limit = connect_wait_limit();
                    const auto deadline = std::chrono::steady_clock::now() + limit;
                    while (device->get_connecting() && std::chrono::steady_clock::now() < deadline) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                    if (device->get_connecting()) {
                        // Withdraw the connect when no other client holds the
                        // device, so it cannot come up behind a client that was
                        // told it failed. The base class records the
                        // disconnect against the in-flight connect and runs it
                        // when the connect returns.
                        if (unregister_client_connection(device.get(), client_key) == 0) {
                            device->disconnect();
                        }
                        throw alpacacore::AlpacaException(
                            device->get_name() + " was still connecting after " +
                                std::to_string(std::chrono::duration_cast<std::chrono::seconds>(limit).count()) +
                                " s; the connect was abandoned",
                            alpacacore::AlpacaError::DriverException);
                    }
                    if (!device->get_connected()) {
                        // Failed connect: this client holds no live link.
                        unregister_client_connection(device.get(), client_key);
                        // The driver's own words when it has them, as a
                        // DriverException: ASCOM Connected says "Do not use a
                        // NotConnectedException here" (issue #776).
                        throw alpacacore::AlpacaException(connect_failure_reason(*device),
                                                          alpacacore::AlpacaError::DriverException);
                    }
                } else if (!connected && unregister_client_connection(device.get(), client_key) == 0) {
                    // A lost-link getter can report false before driver cleanup.
                    // Deliver explicit disconnect even then, as PUT /disconnect
                    // does; the driver owns idempotency and runtime-state reset.
                    // Last client out: tear down the upstream link. While other
                    // clients remain registered the device stays connected —
                    // one client's disconnect must not take the mount away
                    // from the imaging app still using it.
                    device->disconnect();
                    auto deadline = std::chrono::steady_clock::now()
                                  + std::chrono::seconds(8);
                    // Wait on get_connecting() alone, not get_connected(): some
                    // drivers clear their connected flag at the START of teardown
                    // (driver-lifecycle.instructions.md: state must be cleared before a throwing SDK
                    // call), so get_connected() can flip false while the
                    // disconnect task is still in flight. Gating on it here let
                    // this handler return "done" before the task actually
                    // finished, letting the next Connect() race the still-running
                    // task and get silently dropped by AsyncConnectable's
                    // connect-vs-in-flight-disconnect rule (QHY ConformU failure).
                    while (device->get_connecting() && std::chrono::steady_clock::now() < deadline) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(50));
                    }
                }
                return call.ok_void();
            }
        }
        else if (method_name == "supportedactions") {
            if (request.method() == HttpMethod::GET) {
                nlohmann::json actions = nlohmann::json::array();
                for (const auto& action : device->get_supported_actions()) {
                    actions.push_back(action);
                }
                return call.ok(actions);
            }
        }
        else if (method_name == "action") {
            if (request.method() == HttpMethod::PUT) {
                std::string action_name;
                std::string action_parameters = "{}";

                if (const auto* val = call.json_field("Action")) {
                    if (!val->is_string()) {
                        throw_bad_request("Invalid JSON value for parameter: Action");
                    }
                    action_name = val->get<std::string>();
                }
                if (const auto* val = call.json_field("Parameters")) {
                    action_parameters = val->dump();
                }
                if (action_name.empty()) {
                    if (auto value = call.form_field("Action")) {
                        action_name = *value;
                    }
                }
                if (auto value = call.form_field("Parameters")) {
                    action_parameters = *value;
                }
                if (action_name.empty()) {
                    throw_bad_request("Missing parameter: Action");
                }
                
                std::string result = device->action(action_name, action_parameters);
                return call.ok(result);
            }
        }
        else if (method_name == "commandblind") {
            if (request.method() == HttpMethod::PUT) {
                std::string command;
                bool raw = false;

                if (const auto* val = call.json_field("Command")) {
                    if (!val->is_string()) {
                        throw_bad_request("Invalid JSON value for parameter: Command");
                    }
                    command = val->get<std::string>();
                }
                if (const auto* val = call.json_field("Raw")) {
                    if (val->is_boolean()) {
                        raw = val->get<bool>();
                    } else if (val->is_string()) {
                        raw = parse_bool_value(val->get<std::string>(), "Raw");
                    } else {
                        throw_bad_request("Invalid JSON value for parameter: Raw");
                    }
                }
                if (command.empty()) {
                    if (auto value = call.form_field("Command")) {
                        command = *value;
                    }
                }
                if (auto value = call.form_field("Raw")) {
                    raw = parse_bool_value(*value, "Raw");
                }
                if (command.empty()) {
                    throw_bad_request("Missing parameter: Command");
                }
                
                device->command_blind(command, raw);
                return call.ok_void();
            }
        }
        else if (method_name == "commandbool") {
            if (request.method() == HttpMethod::PUT) {
                std::string command;
                bool raw = false;

                if (const auto* val = call.json_field("Command")) {
                    if (!val->is_string()) {
                        throw_bad_request("Invalid JSON value for parameter: Command");
                    }
                    command = val->get<std::string>();
                }
                if (const auto* val = call.json_field("Raw")) {
                    if (val->is_boolean()) {
                        raw = val->get<bool>();
                    } else if (val->is_string()) {
                        raw = parse_bool_value(val->get<std::string>(), "Raw");
                    } else {
                        throw_bad_request("Invalid JSON value for parameter: Raw");
                    }
                }
                if (command.empty()) {
                    if (auto value = call.form_field("Command")) {
                        command = *value;
                    }
                }
                if (auto value = call.form_field("Raw")) {
                    raw = parse_bool_value(*value, "Raw");
                }
                if (command.empty()) {
                    throw_bad_request("Missing parameter: Command");
                }
                
                bool result = device->command_bool(command, raw);
                return call.ok(result);
            }
        }
        else if (method_name == "commandstring") {
            if (request.method() == HttpMethod::PUT) {
                std::string command;
                bool raw = false;

                if (const auto* val = call.json_field("Command")) {
                    if (!val->is_string()) {
                        throw_bad_request("Invalid JSON value for parameter: Command");
                    }
                    command = val->get<std::string>();
                }
                if (const auto* val = call.json_field("Raw")) {
                    if (val->is_boolean()) {
                        raw = val->get<bool>();
                    } else if (val->is_string()) {
                        raw = parse_bool_value(val->get<std::string>(), "Raw");
                    } else {
                        throw_bad_request("Invalid JSON value for parameter: Raw");
                    }
                }
                if (command.empty()) {
                    if (auto value = call.form_field("Command")) {
                        command = *value;
                    }
                }
                if (auto value = call.form_field("Raw")) {
                    raw = parse_bool_value(*value, "Raw");
                }
                if (command.empty()) {
                    throw_bad_request("Missing parameter: Command");
                }
                
                std::string result = device->command_string(command, raw);
                return call.ok(result);
            }
        }
        else if (method_name == "connect") {
            if (request.method() == HttpMethod::PUT) {
                // Same per-client refcounting as PUT connected (issue #160).
                // Deliberately NOT the poll/unregister-on-failure path used by
                // PUT connected: Platform 7 Connect must return immediately,
                // with completion observed via Connecting. If the async
                // connect fails, this client's registration is a phantom until
                // the dead-link sweep clears it — harmless, since GET
                // connected ANDs the registration with real device state.
                const auto op_mutex = device_connection_op_mutex(device);
                std::lock_guard<std::mutex> op_lock(*op_mutex);
                if (!device->get_connecting() && !device->get_connected()) {
                    clear_client_connections(device.get());
                }
                if (!device_is_current(device)) {
                    // Straggler guard — see PUT connected (PR #164 review).
                    throw alpacacore::AlpacaException("Device has been removed",
                                                      alpacacore::AlpacaError::InvalidOperation);
                }
                register_client_connection(device.get(), extract_client_key(request).key);
                // get_connecting() first (see PUT connected): a mid-task
                // connect goes to the driver, whose base class reconciles it.
                if (device->get_connecting() || !device->get_connected()) {
                    // Same clock check as PUT connected: Platform 7 clients
                    // (NINA 3.x) prefer this path, so the warning has to cover
                    // both or it never fires for them (open-astro#289).
                    warn_if_clock_undisciplined(*device);
                    device->connect();
                }
                return call.ok_void();
            }
        }
        else if (method_name == "disconnect") {
            if (request.method() == HttpMethod::PUT) {
                const auto op_mutex = device_connection_op_mutex(device);
                std::lock_guard<std::mutex> op_lock(*op_mutex);
                if (!device->get_connecting() && !device->get_connected()) {
                    clear_client_connections(device.get());
                }
                // Last client out tears down the link; otherwise only this
                // client's registration is dropped (issue #160).
                if (unregister_client_connection(device.get(), extract_client_key(request).key) == 0) {
                    device->disconnect();
                }
                return call.ok_void();
            }
        }
        else if (method_name == "connecting") {
            if (request.method() == HttpMethod::GET) {
                // Issue #776: Connecting is the completion property of
                // Connect(), so a connect that failed raises its error here,
                // on every read, until the next Connect or Disconnect. Read
                // after get_connecting(): the base stores the error before it
                // publishes the task as finished.
                const bool connecting = device->get_connecting();
                if (!connecting) {
                    if (std::string failure = device->get_connecting_error(); !failure.empty()) {
                        throw alpacacore::AlpacaException(failure, alpacacore::AlpacaError::DriverException);
                    }
                }
                return call.ok(connecting);
            }
        }
        else if (method_name == "devicestate") {
            if (request.method() == HttpMethod::GET) {
                nlohmann::json values = nlohmann::json::array();
                for (const auto& entry : device->get_device_state()) {
                    nlohmann::json obj;
                    obj["Name"] = entry.name;
                    std::visit([&obj](const auto& val) { obj["Value"] = val; }, entry.value);
                    values.push_back(obj);
                }
                return call.ok(values);
            }
        }
        
        // Try device-specific methods
        alpacacore::DeviceType device_type = device->get_device_type();
        
        // Handle telescope-specific methods
        if (device_type == alpacacore::DeviceType::Telescope) {
            auto telescope = std::dynamic_pointer_cast<alpacacore::TelescopeDriver>(device);
            if (telescope) {
                return dispatch_telescope_method(telescope, method_name, call);
            }
        }
        if (device_type == alpacacore::DeviceType::Camera) {
            auto camera = std::dynamic_pointer_cast<alpacacore::CameraDriver>(device);
            if (camera) {
                return dispatch_camera_method(camera, method_name, call);
            }
        }
        if (device_type == alpacacore::DeviceType::Switch) {
            auto sw = std::dynamic_pointer_cast<alpacacore::SwitchDriver>(device);
            if (sw) {
                return dispatch_switch_method(sw, method_name, call);
            }
        }
        if (device_type == alpacacore::DeviceType::FilterWheel) {
            auto filterwheel = std::dynamic_pointer_cast<alpacacore::FilterWheelDriver>(device);
            if (filterwheel) {
                return dispatch_filterwheel_method(filterwheel, method_name, call);
            }
        }
        if (device_type == alpacacore::DeviceType::Focuser) {
            auto focuser = std::dynamic_pointer_cast<alpacacore::FocuserDriver>(device);
            if (focuser) {
                return dispatch_focuser_method(focuser, method_name, call);
            }
        }
        if (device_type == alpacacore::DeviceType::Rotator) {
            auto rotator = std::dynamic_pointer_cast<alpacacore::RotatorDriver>(device);
            if (rotator) {
                return dispatch_rotator_method(rotator, method_name, call);
            }
        }
        if (device_type == alpacacore::DeviceType::Dome) {
            auto dome = std::dynamic_pointer_cast<alpacacore::DomeDriver>(device);
            if (dome) {
                return dispatch_dome_method(dome, method_name, call);
            }
        }
        if (device_type == alpacacore::DeviceType::CoverCalibrator) {
            auto covercalibrator = std::dynamic_pointer_cast<alpacacore::CoverCalibratorDriver>(device);
            if (covercalibrator) {
                return dispatch_covercalibrator_method(covercalibrator, method_name, call);
            }
        }
        if (device_type == alpacacore::DeviceType::ObservingConditions) {
            auto observingconditions = std::dynamic_pointer_cast<alpacacore::ObservingConditionsDriver>(device);
            if (observingconditions) {
                return dispatch_observingconditions_method(observingconditions, method_name, call);
            }
        }
        if (device_type == alpacacore::DeviceType::SafetyMonitor) {
            auto safetymonitor = std::dynamic_pointer_cast<alpacacore::SafetyMonitorDriver>(device);
            if (safetymonitor) {
                return dispatch_safetymonitor_method(safetymonitor, method_name, call);
            }
        }
        
        // For other device types or if cast failed, return not implemented
        return call.error(util::ErrorCode::NOT_IMPLEMENTED,
                          "Method '" + method_name + "' not yet implemented for device type " +
                              std::string(alpacacore::device_type_to_string(device_type)));
    });
}

Response Router::dispatch_telescope_method(std::shared_ptr<alpacacore::TelescopeDriver> telescope,
                                           const std::string& method_name, const DeviceCall& call) {
    const Request& request = call.request();

    return call.run("telescope method '" + method_name + "'", [&]() -> Response {
        // GET-only boolean properties
        if (request.method() == HttpMethod::GET) {
            if (method_name == "cansetrightascensionrate") {
                return call.ok(telescope->get_can_set_right_ascension_rate());
            }
            else if (method_name == "cansetdeclinationrate") {
                return call.ok(telescope->get_can_set_declination_rate());
            }
            else if (method_name == "cansetguiderates") {
                return call.ok(telescope->get_can_set_guide_rates());
            }
            else if (method_name == "cansettracking") {
                return call.ok(telescope->get_can_set_tracking());
            }
            else if (method_name == "cansetpark") {
                return call.ok(telescope->get_can_set_park());
            }
            else if (method_name == "cansetpierside") {
                return call.ok(telescope->get_can_set_pier_side());
            }
            else if (method_name == "canfindhome") {
                return call.ok(telescope->get_can_find_home());
            }
            else if (method_name == "canpark") {
                return call.ok(telescope->get_can_park());
            }
            else if (method_name == "canpulseguide") {
                return call.ok(telescope->get_can_pulse_guide());
            }
            else if (method_name == "canslew") {
                return call.ok(false);  // #775: synchronous slews are not served
            }
            else if (method_name == "canslewasync") {
                return call.ok(telescope->get_can_slew_async());
            }
            else if (method_name == "cansync") {
                return call.ok(telescope->get_can_sync());
            }
            else if (method_name == "canunpark") {
                return call.ok(telescope->get_can_unpark());
            }
            else if (method_name == "canslewaltaz") {
                return call.ok(false);  // #775: synchronous slews are not served
            }
            else if (method_name == "canslewaltazasync") {
                return call.ok(telescope->get_can_slew_alt_az_async());
            }
            else if (method_name == "cansyncaltaz") {
                return call.ok(telescope->get_can_sync_alt_az());
            }
            else if (method_name == "alignmentmode") {
                int mode = static_cast<int>(telescope->get_alignment_mode());
                return call.ok(mode);
            }
            else if (method_name == "equatorialsystem") {
                return call.ok(static_cast<int>(telescope->get_equatorial_system()));
            }
            else if (method_name == "doesrefraction") {
                return call.ok(telescope->get_does_refraction());
            }
            else if (method_name == "ispulseguiding") {
                return call.ok(telescope->get_is_pulse_guiding());
            }
            else if (method_name == "slewsettletime") {
                return call.ok(telescope->get_slew_settle_time());
            } else if (method_name == "utcdate") {
                return call.ok(alpacacore::device_state_timestamp(telescope->get_utc_date()));
            } else if (method_name == "axisrates") {
                int axis = call.get_int("Axis");
                // AxisRates must return an array of rate objects, even if only one range.
                auto ranges = telescope->get_axis_rate_ranges(axis);
                nlohmann::json rates_array = nlohmann::json::array();
                for (const auto& range : ranges) {
                    nlohmann::json rate_obj;
                    rate_obj["Minimum"] = range.first;
                    rate_obj["Maximum"] = range.second;
                    rates_array.push_back(rate_obj);
                }
                return call.ok(rates_array);
            } else if (method_name == "canmoveaxis") {
                int axis = call.get_int("Axis");
                bool can_move = telescope->get_can_move_axis(axis);
                return call.ok(can_move);
            } else if (method_name == "destinationsideofpier") {
                auto ra_value = get_query_param_case_insensitive(request, "RightAscension");
                if (!ra_value) {
                    throw_bad_request("Missing parameter: RightAscension");
                }
                auto dec_value = get_query_param_case_insensitive(request, "Declination");
                if (!dec_value) {
                    throw_bad_request("Missing parameter: Declination");
                }
                double ra = parse_double_value(*ra_value, "RightAscension");
                double dec = parse_double_value(*dec_value, "Declination");
                int side = telescope->get_destination_side_of_pier(ra, dec);
                return call.ok(side);
            } else if (method_name == "athome") {
                return call.ok(telescope->get_at_home());
            } else if (method_name == "atpark") {
                return call.ok(telescope->get_at_park());
            } else if (method_name == "slewing") {
                return call.ok(telescope->get_slewing());
            } else if (method_name == "tracking") {
                return call.ok(telescope->get_tracking());
            } else if (method_name == "altitude") {
                return call.ok(telescope->get_altitude());
            } else if (method_name == "azimuth") {
                return call.ok(telescope->get_azimuth());
            } else if (method_name == "declination") {
                return call.ok(telescope->get_declination());
            } else if (method_name == "rightascension") {
                return call.ok(telescope->get_right_ascension());
            } else if (method_name == "declinationrate") {
                return call.ok(telescope->get_declination_rate());
            } else if (method_name == "rightascensionrate") {
                return call.ok(telescope->get_right_ascension_rate());
            } else if (method_name == "trackingrate") {
                int rate = telescope->get_tracking_rate();
                return call.ok(rate);
            } else if (method_name == "trackingrates") {
                auto rates = telescope->get_tracking_rates();
                nlohmann::json rates_array = nlohmann::json::array();
                for (int rate : rates) {
                    rates_array.push_back(rate);
                }
                return call.ok(rates_array);
            } else if (method_name == "targetdeclination") {
                return call.ok(telescope->get_target_declination());
            } else if (method_name == "targetrightascension") {
                return call.ok(telescope->get_target_right_ascension());
            } else if (method_name == "siderealtime") {
                return call.ok(telescope->get_sidereal_time());
            } else if (method_name == "siteelevation") {
                return call.ok(telescope->get_site_elevation());
            } else if (method_name == "sitelatitude") {
                return call.ok(telescope->get_site_latitude());
            } else if (method_name == "sitelongitude") {
                return call.ok(telescope->get_site_longitude());
            } else if (method_name == "focallength") {
                return call.ok(telescope->get_focal_length());
            } else if (method_name == "aperturediameter") {
                return call.ok(telescope->get_aperture_diameter());
            } else if (method_name == "aperturearea") {
                return call.ok(telescope->get_aperture_area());
            } else if (method_name == "sideofpier") {
                return call.ok(telescope->get_side_of_pier());
            } else if (method_name == "guideratedeclination") {
                auto guide_rate = telescope->get_guide_rate();
                return call.ok(guide_rate.dec);
            } else if (method_name == "guideraterightascension") {
                auto guide_rate = telescope->get_guide_rate();
                return call.ok(guide_rate.ra);
            }
        }
        
        // GET/PUT properties
        if (method_name == "doesrefraction") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(telescope->get_does_refraction());
            }
            else if (request.method() == HttpMethod::PUT) {
                bool value = call.get_bool("DoesRefraction");
                telescope->set_does_refraction(value);
                return call.ok_void();
            }
        }
        else if (method_name == "slewsettletime") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(telescope->get_slew_settle_time());
            }
            else if (request.method() == HttpMethod::PUT) {
                int value = call.get_int("SlewSettleTime");
                telescope->set_slew_settle_time(value);
                return call.ok_void();
            }
        }
        else if (method_name == "utcdate") {
            // open-astro#401: a UTCDate write can step the host clock (#289)
            // and latch ClockSource, the same host-level effect the synctime
            // endpoint guards. A browser page on another origin must not be
            // able to fire it; native Alpaca clients send no Origin and pass.
            if (auto rejected =
                    reject_cross_origin_request(request, call.client_tx_id(), call.server_tx_id(), "UTCDate")) {
                return *rejected;
            }
            if (request.method() == HttpMethod::GET) {
                auto utc = telescope->get_utc_date();
                auto time_t = std::chrono::system_clock::to_time_t(utc);
                std::ostringstream oss;
                oss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%S");
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    utc.time_since_epoch()) % 1000;
                oss << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z";
                return call.ok(oss.str());
            }
            else if (request.method() == HttpMethod::PUT) {
                const std::string date_str = call.find_string("UTCDate").value_or("");
                if (date_str.empty()) {
                    throw_bad_request("Missing parameter: UTCDate or Value");
                }
                std::string cleaned = date_str;
                if (!cleaned.empty() && (cleaned.back() == 'Z' || cleaned.back() == 'z')) {
                    cleaned.pop_back();
                }
                std::string base = cleaned;
                int millis = 0;
                auto dot_pos = cleaned.find('.');
                if (dot_pos != std::string::npos) {
                    base = cleaned.substr(0, dot_pos);
                    std::string frac = cleaned.substr(dot_pos + 1);
                    if (frac.size() > 3) {
                        frac.resize(3);
                    }
                    while (frac.size() < 3) {
                        frac.push_back('0');
                    }
                    try {
                        millis = std::stoi(frac);
                    } catch (const std::exception&) {
                        throw_bad_request("Invalid UTC date format: " + date_str);
                    }
                }

                std::tm tm = {};
                std::istringstream ss(base);
                ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
                if (ss.fail()) {
                    throw_bad_request("Invalid UTC date format: " + date_str);
                }
                auto utc_time = timegm(&tm);
                if (utc_time == static_cast<std::time_t>(-1)) {
                    throw_bad_request("Failed to convert UTC date: " + date_str);
                }
                auto time_point = std::chrono::system_clock::from_time_t(utc_time) +
                    std::chrono::milliseconds(millis);
                // open-astro#289: on an SBC with no NTP and no RTC this write is
                // the only correct time the host will ever see. Step the system
                // clock from it when the kernel reports the clock undisciplined
                // (never over NTP/chrony/GPS), then hand the driver the same value.
                {
                    const auto step = host_clock_.step_from_client(time_point);
                    using Outcome = alpacacore::util::HostClock::Outcome;
                    const std::string what = "UTCDate from " + request.remote_address() + " for telescope/" +
                                             std::to_string(telescope->get_device_number()) + ": host clock " +
                                             alpacacore::util::HostClock::outcome_name(step.outcome) +
                                             " (client - host = " + std::to_string(step.delta.count()) + " ms)";
                    if (step.outcome == Outcome::Stepped) {
                        util::log_info(what);
                    } else if (step.outcome == Outcome::Failed) {
                        util::log_warning(what + ": " + step.error);
                    } else if (step.outcome == Outcome::SkippedSynchronized &&
                               (step.delta > alpacacore::util::HostClock::kClientDisagreementWarn ||
                                step.delta < -alpacacore::util::HostClock::kClientDisagreementWarn)) {
                        // Rendered in ms, not seconds: the threshold is a
                        // millisecond constant, and dividing by 1000 would
                        // log "2 s" for a future 2500 ms value -- which
                        // defeats the point of having one shared constant.
                        util::log_warning(what + "; NTP-disciplined host and client disagree by more than " +
                                          std::to_string(alpacacore::util::HostClock::kClientDisagreementWarn.count()) +
                                          " ms");
                    } else {
                        util::log_debug(what);
                    }
                }
                telescope->set_utc_date(time_point);
                return call.ok_void();
            }
        }
        else if (method_name == "tracking") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(telescope->get_tracking());
            }
            else if (request.method() == HttpMethod::PUT) {
                bool value = call.get_bool("Tracking");
                telescope->set_tracking(value);
                return call.ok_void();
            }
        }
        else if (method_name == "declinationrate") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(telescope->get_declination_rate());
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = call.get_double("DeclinationRate");
                telescope->set_declination_rate(value);
                return call.ok_void();
            }
        }
        else if (method_name == "rightascensionrate") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(telescope->get_right_ascension_rate());
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = call.get_double("RightAscensionRate");
                telescope->set_right_ascension_rate(value);
                return call.ok_void();
            }
        }
        else if (method_name == "trackingrate") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(telescope->get_tracking_rate());
            }
            else if (request.method() == HttpMethod::PUT) {
                int value = call.get_int("TrackingRate");
                telescope->set_tracking_rate(value);
                return call.ok_void();
            }
        }
        else if (method_name == "targetdeclination") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(telescope->get_target_declination());
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = call.get_double("TargetDeclination");
                telescope->set_target_declination(value);
                return call.ok_void();
            }
        }
        else if (method_name == "targetrightascension") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(telescope->get_target_right_ascension());
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = call.get_double("TargetRightAscension");
                telescope->set_target_right_ascension(value);
                return call.ok_void();
            }
        }
        else if (method_name == "siteelevation") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(telescope->get_site_elevation());
            }
            else if (request.method() == HttpMethod::PUT) {
                // open-astro#444: this PUT rewrites persisted device config,
                // the effect configuredevice already guards (#401 precedent).
                if (auto rejected = reject_cross_origin_request(request, call.client_tx_id(), call.server_tx_id(),
                                                                "SiteElevation")) {
                    return *rejected;
                }
                double value = call.get_double("SiteElevation");
                telescope->set_site_elevation(value);
                persist_client_site(*telescope, "siteElevation", value);  // open-astro#444
                return call.ok_void();
            }
        }
        else if (method_name == "sitelatitude") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(telescope->get_site_latitude());
            }
            else if (request.method() == HttpMethod::PUT) {
                // open-astro#444: this PUT rewrites persisted device config,
                // the effect configuredevice already guards (#401 precedent).
                if (auto rejected = reject_cross_origin_request(request, call.client_tx_id(), call.server_tx_id(),
                                                                "SiteLatitude")) {
                    return *rejected;
                }
                double value = call.get_double("SiteLatitude");
                telescope->set_site_latitude(value);
                persist_client_site(*telescope, "siteLatitude", value);  // open-astro#444
                return call.ok_void();
            }
        }
        else if (method_name == "sitelongitude") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(telescope->get_site_longitude());
            }
            else if (request.method() == HttpMethod::PUT) {
                // open-astro#444: this PUT rewrites persisted device config,
                // the effect configuredevice already guards (#401 precedent).
                if (auto rejected = reject_cross_origin_request(request, call.client_tx_id(), call.server_tx_id(),
                                                                "SiteLongitude")) {
                    return *rejected;
                }
                double value = call.get_double("SiteLongitude");
                telescope->set_site_longitude(value);
                persist_client_site(*telescope, "siteLongitude", value);  // open-astro#444
                return call.ok_void();
            }
        }
        else if (method_name == "sideofpier") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(telescope->get_side_of_pier());
            }
            else if (request.method() == HttpMethod::PUT) {
                int value = call.get_int("SideOfPier");
                telescope->set_side_of_pier(value);
                return call.ok_void();
            }
        }
        else if (method_name == "guideratedeclination") {
            if (request.method() == HttpMethod::GET) {
                auto guide_rate = telescope->get_guide_rate();
                return call.ok(guide_rate.dec);
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = call.get_double("GuideRateDeclination");
                auto guide_rate = telescope->get_guide_rate();
                guide_rate.dec = value;
                telescope->set_guide_rate(guide_rate);
                return call.ok_void();
            }
        }
        else if (method_name == "guideraterightascension") {
            if (request.method() == HttpMethod::GET) {
                auto guide_rate = telescope->get_guide_rate();
                return call.ok(guide_rate.ra);
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = call.get_double("GuideRateRightAscension");
                auto guide_rate = telescope->get_guide_rate();
                guide_rate.ra = value;
                telescope->set_guide_rate(guide_rate);
                return call.ok_void();
            }
        }
        
        // PUT-only methods (actions)
        if (request.method() == HttpMethod::PUT) {
            if (method_name == "abortslew") {
                telescope->abort_slew();
                return call.ok_void();
            }
            else if (method_name == "moveaxis") {
                // Debug logging
                if (!request.body().empty()) {
                    util::log_info("moveaxis body: " + util::escape_for_log(request.body()));
                }
                int axis = call.get_int("Axis");
                double rate = call.get_double("Rate");
                util::log_info("moveaxis parsed: Axis=" + std::to_string(axis) + ", Rate=" + std::to_string(rate));
                telescope->move_axis(axis, rate);
                return call.ok_void();
            }
            else if (method_name == "slewtoaltazasync") {
                double altitude = call.get_double("Altitude");
                double azimuth = call.get_double("Azimuth");
                telescope->slew_to_alt_az_async(altitude, azimuth);
                return call.ok_void();
            }
            else if (method_name == "slewtoaltaz") {
                // #775: ASCOM's slew FAQ steers clients to the async forms; refuse before
                // any parameter parse, target write or driver call.
                throw alpacacore::AlpacaException("Synchronous slews are not implemented; use the async form",
                                                  alpacacore::AlpacaError::MethodNotImplemented);
            }
            else if (method_name == "synctoaltaz") {
                double altitude = call.get_double("Altitude");
                double azimuth = call.get_double("Azimuth");
                telescope->sync_to_alt_az(altitude, azimuth);
                return call.ok_void();
            }
            else if (method_name == "park") {
                telescope->park();
                return call.ok_void();
            }
            else if (method_name == "unpark") {
                telescope->unpark();
                return call.ok_void();
            }
            else if (method_name == "findhome") {
                telescope->find_home();
                return call.ok_void();
            }
            else if (method_name == "setpark") {
                telescope->set_park();
                return call.ok_void();
            }
            else if (method_name == "slewtotarget") {
                // #775: ASCOM's slew FAQ steers clients to the async forms; refuse before
                // any parameter parse, target write or driver call.
                throw alpacacore::AlpacaException("Synchronous slews are not implemented; use the async form",
                                                  alpacacore::AlpacaError::MethodNotImplemented);
            }
            else if (method_name == "slewtotargetasync") {
                telescope->slew_to_target_async();
                return call.ok_void();
            }
            else if (method_name == "slewtocoordinates") {
                // #775: ASCOM's slew FAQ steers clients to the async forms; refuse before
                // any parameter parse, target write or driver call.
                throw alpacacore::AlpacaException("Synchronous slews are not implemented; use the async form",
                                                  alpacacore::AlpacaError::MethodNotImplemented);
            }
            else if (method_name == "slewtocoordinatesasync") {
                double ra = call.get_double("RightAscension");
                double dec = call.get_double("Declination");
                // slew_to_coordinates_async sets targets internally; skip
                // redundant set_target calls to avoid extra mutex round-trips.
                telescope->slew_to_coordinates_async(ra, dec);
                return call.ok_void();
            }
            else if (method_name == "synctotarget") {
                telescope->sync_to_target();
                return call.ok_void();
            }
            else if (method_name == "synctocoordinates") {
                double ra = call.get_double("RightAscension");
                double dec = call.get_double("Declination");
                telescope->sync_to_coordinates(ra, dec);
                return call.ok_void();
            }
            else if (method_name == "pulseguide") {
                int direction = call.get_int("Direction");
                int duration = call.get_int("Duration");
                telescope->pulse_guide(direction, duration);
                return call.ok_void();
            }
        }
        
        // Method not found
        return call.error(util::ErrorCode::NOT_IMPLEMENTED,
                          "Method '" + method_name + "' not yet implemented for Telescope");
    });
}

Response Router::dispatch_camera_method(std::shared_ptr<alpacacore::CameraDriver> camera,
                                        const std::string& method_name, const DeviceCall& call) {
    const Request& request = call.request();

    auto dispatch_start = std::chrono::steady_clock::now();
    alpacacore::logging::log(alpacacore::logging::LogLevel::Trace, "AlpacaHTTP",
        "dispatch_camera_method entry: method=" + method_name);
    struct DispatchExitLog {
        std::string method_name;
        std::chrono::steady_clock::time_point start;
        ~DispatchExitLog() {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            alpacacore::logging::log(alpacacore::logging::LogLevel::Trace, "AlpacaHTTP",
                "dispatch_camera_method exit: method=" + method_name + " duration_ms=" + std::to_string(ms));
        }
    } dispatch_exit_log{method_name, dispatch_start};

    auto format_utc = [](std::chrono::system_clock::time_point time_point) -> std::string {
        auto time_t = std::chrono::system_clock::to_time_t(time_point);
        std::ostringstream oss;
        oss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%S");
        return oss.str();
    };

    return call.run("camera method '" + method_name + "'", [&]() -> Response {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "bayeroffsetx") {
                return call.ok(camera->get_bayer_offset_x());
            } else if (method_name == "bayeroffsety") {
                return call.ok(camera->get_bayer_offset_y());
            } else if (method_name == "binx") {
                return call.ok(camera->get_bin_x());
            } else if (method_name == "biny") {
                return call.ok(camera->get_bin_y());
            } else if (method_name == "camerastate") {
                return call.ok(static_cast<int>(camera->get_camera_state()));
            } else if (method_name == "cameraxsize") {
                return call.ok(camera->get_camera_x_size());
            } else if (method_name == "cameraysize") {
                return call.ok(camera->get_camera_y_size());
            } else if (method_name == "canabortexposure") {
                return call.ok(camera->get_can_abort_exposure());
            } else if (method_name == "canasymmetricbin") {
                return call.ok(camera->get_can_asymmetric_bin());
            } else if (method_name == "canfastreadout") {
                return call.ok(camera->get_can_fast_readout());
            } else if (method_name == "cangetcoolerpower") {
                return call.ok(camera->get_can_get_cooler_power());
            } else if (method_name == "canpulseguide") {
                return call.ok(camera->get_can_pulse_guide());
            } else if (method_name == "cansetccdtemperature") {
                return call.ok(camera->get_can_set_ccd_temperature());
            } else if (method_name == "canstopexposure") {
                return call.ok(camera->get_can_stop_exposure());
            } else if (method_name == "ccdtemperature") {
                return call.ok(camera->get_ccd_temperature());
            } else if (method_name == "cooleron") {
                return call.ok(camera->get_cooler_on());
            } else if (method_name == "coolerpower") {
                return call.ok(camera->get_cooler_power());
            } else if (method_name == "electronsperadu") {
                return call.ok(camera->get_electrons_per_adu());
            } else if (method_name == "exposuremax") {
                return call.ok(camera->get_exposure_max());
            } else if (method_name == "exposuremin") {
                return call.ok(camera->get_exposure_min());
            } else if (method_name == "exposureresolution") {
                return call.ok(camera->get_exposure_resolution());
            } else if (method_name == "fastreadout") {
                return call.ok(camera->get_fast_readout());
            } else if (method_name == "fullwellcapacity") {
                return call.ok(camera->get_full_well_capacity());
            } else if (method_name == "gain") {
                return call.ok(camera->get_gain());
            } else if (method_name == "gainmax") {
                return call.ok(camera->get_gain_max());
            } else if (method_name == "gainmin") {
                return call.ok(camera->get_gain_min());
            } else if (method_name == "gains") {
                nlohmann::json gains = nlohmann::json::array();
                for (const auto& gain : camera->get_gains()) {
                    gains.push_back(gain);
                }
                return call.ok(gains);
            } else if (method_name == "hasshutter") {
                return call.ok(camera->get_has_shutter());
            } else if (method_name == "heatsinktemperature") {
                return call.ok(camera->get_heat_sink_temperature());
            } else if (method_name == "imagearray") {
                const bool has_accept = request.has_header("accept");
                const std::string accept = has_accept ? request.get_header("accept") : std::string();
                util::log_debug("Camera imagearray Accept: " + (has_accept ? util::escape_for_log(accept) : "<none>") +
                                ", imagebytes=" + std::string(accepts_imagebytes(accept) ? "true" : "false"));
                auto payload = build(camera->get_image_array(), accept, call);
                Response response;
                response.set_content_type(payload.content_type);
                response.set_body(std::move(payload.body));
                return response;
            } else if (method_name == "imagearrayvariant") {
                if (camera->get_connecting() || !camera->get_connected()) {
                    throw alpacacore::AlpacaException("Camera is not connected", alpacacore::AlpacaError::NotConnected);
                }
                // ImageArrayVariant is COM-only; Alpaca must refuse it without retrieving a frame.
                throw alpacacore::AlpacaException("ImageArrayVariant is not supported by Alpaca; use ImageArray",
                                                  alpacacore::AlpacaError::PropertyNotImplemented);
            } else if (method_name == "imageready") {
                return call.ok(camera->get_image_ready());
            } else if (method_name == "ispulseguiding") {
                return call.ok(camera->get_is_pulse_guiding());
            } else if (method_name == "lastexposureduration") {
                return call.ok(camera->get_last_exposure_duration());
            } else if (method_name == "lastexposurestarttime") {
                return call.ok(format_utc(camera->get_last_exposure_start_time()));
            } else if (method_name == "maxadu") {
                return call.ok(camera->get_max_adu());
            } else if (method_name == "maxbinx") {
                return call.ok(camera->get_max_bin_x());
            } else if (method_name == "maxbiny") {
                return call.ok(camera->get_max_bin_y());
            } else if (method_name == "numx") {
                return call.ok(camera->get_num_x());
            } else if (method_name == "numy") {
                return call.ok(camera->get_num_y());
            } else if (method_name == "offset") {
                return call.ok(camera->get_offset());
            } else if (method_name == "offsetmax") {
                return call.ok(camera->get_offset_max());
            } else if (method_name == "offsetmin") {
                return call.ok(camera->get_offset_min());
            } else if (method_name == "offsets") {
                nlohmann::json offsets = nlohmann::json::array();
                for (const auto& offset : camera->get_offsets()) {
                    offsets.push_back(offset);
                }
                return call.ok(offsets);
            } else if (method_name == "percentcompleted") {
                auto percent = camera->get_percent_completed_checked();
                auto value = static_cast<std::int32_t>(percent);
                return call.ok(value);
            } else if (method_name == "pixelsizex") {
                return call.ok(camera->get_pixel_size_x());
            } else if (method_name == "pixelsizey") {
                return call.ok(camera->get_pixel_size_y());
            } else if (method_name == "readoutmode") {
                return call.ok(camera->get_readout_mode());
            } else if (method_name == "readoutmodes") {
                nlohmann::json modes = nlohmann::json::array();
                for (const auto& mode : camera->get_readout_modes()) {
                    modes.push_back(mode);
                }
                return call.ok(modes);
            } else if (method_name == "sensorname") {
                return call.ok(camera->get_sensor_name());
            } else if (method_name == "sensortype") {
                return call.ok(static_cast<int>(camera->get_sensor_type()));
            } else if (method_name == "setccdtemperature") {
                return call.ok(camera->get_set_ccd_temperature());
            } else if (method_name == "startx") {
                return call.ok(camera->get_start_x());
            } else if (method_name == "starty") {
                return call.ok(camera->get_start_y());
            } else if (method_name == "subexposureduration") {
                return call.ok(camera->get_sub_exposure_duration());
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "binx") {
                int value = call.get_int("BinX");
                camera->set_bin_x(value);
                return call.ok_void();
            } else if (method_name == "biny") {
                int value = call.get_int("BinY");
                camera->set_bin_y(value);
                return call.ok_void();
            } else if (method_name == "fastreadout") {
                bool value = call.get_bool("FastReadout");
                camera->set_fast_readout(value);
                return call.ok_void();
            } else if (method_name == "gain") {
                int value = call.get_int("Gain");
                camera->set_gain(value);
                return call.ok_void();
            } else if (method_name == "cooleron") {
                bool value = call.get_bool("CoolerOn");
                camera->set_cooler_on(value);
                return call.ok_void();
            } else if (method_name == "setccdtemperature") {
                double value = call.get_double("SetCCDTemperature");
                camera->set_set_ccd_temperature(value);
                return call.ok_void();
            } else if (method_name == "numx") {
                int value = call.get_int("NumX");
                camera->set_num_x(value);
                return call.ok_void();
            } else if (method_name == "numy") {
                int value = call.get_int("NumY");
                camera->set_num_y(value);
                return call.ok_void();
            } else if (method_name == "offset") {
                int value = call.get_int("Offset");
                camera->set_offset(value);
                return call.ok_void();
            } else if (method_name == "readoutmode") {
                int value = call.get_int("ReadoutMode");
                camera->set_readout_mode(value);
                return call.ok_void();
            } else if (method_name == "startx") {
                int value = call.get_int("StartX");
                camera->set_start_x(value);
                return call.ok_void();
            } else if (method_name == "starty") {
                int value = call.get_int("StartY");
                camera->set_start_y(value);
                return call.ok_void();
            } else if (method_name == "subexposureduration") {
                double value = call.get_double("SubExposureDuration");
                camera->set_sub_exposure_duration(value);
                return call.ok_void();
            } else if (method_name == "startexposure") {
                double duration = call.get_double("Duration");
                bool light = call.get_bool("Light");
                camera->start_exposure(duration, light);
                return call.ok_void();
            } else if (method_name == "pulseguide") {
                int direction = call.get_int("Direction");
                int duration = call.get_int("Duration");
                camera->pulse_guide(direction, duration);
                return call.ok_void();
            } else if (method_name == "stopexposure") {
                camera->stop_exposure();
                return call.ok_void();
            } else if (method_name == "abortexposure") {
                camera->abort_exposure();
                return call.ok_void();
            }
        }

        return call.error(util::ErrorCode::NOT_IMPLEMENTED,
                          "Method '" + method_name + "' not yet implemented for Camera");
    });
}

Response Router::dispatch_switch_method(std::shared_ptr<alpacacore::SwitchDriver> sw, const std::string& method_name,
                                        const DeviceCall& call) {
    const Request& request = call.request();

    return call.run("switch method '" + method_name + "'", [&]() -> Response {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "maxswitch") {
                return call.ok(sw->get_max_switch());
            } else if (method_name == "cancelasync") {
                // The switch driver has no cancel member, so succeeding here
                // would report a cancel that never happened (#576). The Id is
                // still validated first: an out-of-range one is InvalidValue.
                (void)sw->get_can_async(call.get_int("Id"));
                throw alpacacore::AlpacaException("Switch does not support cancelling an asynchronous operation",
                                                  alpacacore::AlpacaError::NotImplemented);
            } else if (method_name == "canasync") {
                int id = call.get_int("Id");
                return call.ok(sw->get_can_async(id));
            } else if (method_name == "canwrite") {
                int id = call.get_int("Id");
                return call.ok(sw->get_can_write(id));
            } else if (method_name == "getswitch") {
                int id = call.get_int("Id");
                return call.ok(sw->get_switch(id));
            } else if (method_name == "getswitchvalue") {
                int id = call.get_int("Id");
                return call.ok(sw->get_switch_value(id));
            } else if (method_name == "getswitchname") {
                int id = call.get_int("Id");
                return call.ok(sw->get_switch_name(id));
            } else if (method_name == "getswitchdescription") {
                int id = call.get_int("Id");
                return call.ok(sw->get_switch_description(id));
            } else if (method_name == "minswitchvalue") {
                int id = call.get_int("Id");
                return call.ok(sw->get_min_switch_value(id));
            } else if (method_name == "maxswitchvalue") {
                int id = call.get_int("Id");
                return call.ok(sw->get_max_switch_value(id));
            } else if (method_name == "statechangecomplete") {
                int id = call.get_int("Id");
                return call.ok(sw->get_state_change_complete(id));
            } else if (method_name == "switchstep") {
                int id = call.get_int("Id");
                return call.ok(sw->get_switch_step(id));
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "setswitch") {
                int id = call.get_int("Id");
                bool state = call.get_bool("State");
                sw->set_switch(id, state);
                return call.ok_void();
            } else if (method_name == "cancelasync") {
                // The switch driver has no cancel member, so succeeding here
                // would report a cancel that never happened (#576). The Id is
                // still validated first: an out-of-range one is InvalidValue.
                (void)sw->get_can_async(call.get_int("Id"));
                throw alpacacore::AlpacaException("Switch does not support cancelling an asynchronous operation",
                                                  alpacacore::AlpacaError::NotImplemented);
            } else if (method_name == "setasync") {
                int id = call.get_int("Id");
                bool state = call.get_bool("State");
                sw->set_async(id, state);
                return call.ok_void();
            } else if (method_name == "setswitchvalue") {
                int id = call.get_int("Id");
                double value = call.get_double("Value");
                sw->set_switch_value(id, value);
                return call.ok_void();
            } else if (method_name == "setasyncvalue") {
                int id = call.get_int("Id");
                double value = call.get_double("Value");
                sw->set_async_value(id, value);
                return call.ok_void();
            } else if (method_name == "setswitchname") {
                int id = call.get_int("Id");
                std::string name = call.get_string("Name");
                sw->set_switch_name(id, name);
                return call.ok_void();
            }
        }

        return call.error(util::ErrorCode::NOT_IMPLEMENTED,
                          "Method '" + method_name + "' not yet implemented for Switch");
    });
}

Response Router::dispatch_filterwheel_method(std::shared_ptr<alpacacore::FilterWheelDriver> filterwheel,
                                             const std::string& method_name, const DeviceCall& call) {
    const Request& request = call.request();

    return call.run("filter wheel method '" + method_name + "'", [&]() -> Response {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "position") {
                return call.ok(filterwheel->get_position());
            } else if (method_name == "names") {
                nlohmann::json names = nlohmann::json::array();
                for (const auto& name : filterwheel->get_names()) {
                    names.push_back(name);
                }
                return call.ok(names);
            } else if (method_name == "focusoffsets") {
                nlohmann::json offsets = nlohmann::json::array();
                for (int offset : filterwheel->get_focus_offsets()) {
                    offsets.push_back(offset);
                }
                return call.ok(offsets);
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "position") {
                int value = call.get_int("Position");
                filterwheel->set_position(value);
                return call.ok_void();
            } else if (method_name == "names") {
                const auto* names_val = call.json_field("Names");
                if (!names_val) {
                    throw_bad_request("Missing parameter: Names");
                }
                if (!names_val->is_array()) {
                    throw_bad_request("Names must be an array of strings");
                }
                for (const auto& name : *names_val) {
                    if (!name.is_string()) {
                        throw_bad_request("Names must be an array of strings");
                    }
                }
                std::vector<std::string> names = names_val->get<std::vector<std::string>>();
                filterwheel->set_names(names);
                return call.ok_void();
            } else if (method_name == "focusoffsets") {
                const auto* offsets_val = call.json_field("FocusOffsets");
                if (!offsets_val) {
                    throw_bad_request("Missing parameter: FocusOffsets");
                }
                if (!offsets_val->is_array()) {
                    throw_bad_request("FocusOffsets must be an array of integers");
                }
                std::vector<int> offsets;
                offsets.reserve(offsets_val->size());
                for (const auto& offset : *offsets_val) {
                    offsets.push_back(json_int_value(offset, "FocusOffsets"));
                }
                filterwheel->set_focus_offsets(offsets);
                return call.ok_void();
            }
        }

        return call.error(util::ErrorCode::NOT_IMPLEMENTED,
                          "Method '" + method_name + "' not yet implemented for FilterWheel");
    });
}

Response Router::dispatch_focuser_method(std::shared_ptr<alpacacore::FocuserDriver> focuser,
                                         const std::string& method_name, const DeviceCall& call) {
    const Request& request = call.request();

    return call.run("focuser method '" + method_name + "'", [&]() -> Response {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "absolute") {
                return call.ok(focuser->get_absolute());
            } else if (method_name == "ismoving") {
                return call.ok(focuser->get_is_moving());
            } else if (method_name == "maxincrement") {
                return call.ok(focuser->get_max_increment());
            } else if (method_name == "maxstep") {
                return call.ok(focuser->get_max_step());
            } else if (method_name == "position") {
                return call.ok(focuser->get_position());
            } else if (method_name == "stepsize") {
                return call.ok(focuser->get_step_size());
            } else if (method_name == "tempcompavailable") {
                return call.ok(focuser->get_temp_comp_available());
            } else if (method_name == "tempcomp") {
                return call.ok(focuser->get_temp_comp());
            } else if (method_name == "temperature") {
                return call.ok(focuser->get_temperature());
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "tempcomp") {
                bool value = call.get_bool("TempComp");
                focuser->set_temp_comp(value);
                return call.ok_void();
            } else if (method_name == "move") {
                int position = call.get_int("Position");
                focuser->move(position);
                return call.ok_void();
            } else if (method_name == "halt") {
                focuser->halt();
                return call.ok_void();
            }
        }

        return call.error(util::ErrorCode::NOT_IMPLEMENTED,
                          "Method '" + method_name + "' not yet implemented for Focuser");
    });
}

Response Router::dispatch_rotator_method(std::shared_ptr<alpacacore::RotatorDriver> rotator,
                                         const std::string& method_name, const DeviceCall& call) {
    const Request& request = call.request();

    return call.run("rotator method '" + method_name + "'", [&]() -> Response {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "canreverse") {
                return call.ok(rotator->get_can_reverse());
            } else if (method_name == "reverse") {
                return call.ok(rotator->get_reverse());
            } else if (method_name == "ismoving") {
                return call.ok(rotator->get_is_moving());
            } else if (method_name == "mechanicalposition") {
                return call.ok(rotator->get_mechanical_position());
            } else if (method_name == "position") {
                return call.ok(rotator->get_position());
            } else if (method_name == "stepsize") {
                return call.ok(rotator->get_step_size());
            } else if (method_name == "targetposition") {
                return call.ok(rotator->get_target_position());
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "reverse") {
                bool value = call.get_bool("Reverse");
                rotator->set_reverse(value);
                return call.ok_void();
            } else if (method_name == "move") {
                double position = call.get_double("Position");
                rotator->move(position);
                return call.ok_void();
            } else if (method_name == "moveabsolute") {
                double position = call.get_double("Position");
                rotator->move_absolute(position);
                return call.ok_void();
            } else if (method_name == "movemechanical") {
                double position = call.get_double("Position");
                rotator->move_mechanical(position);
                return call.ok_void();
            } else if (method_name == "halt") {
                rotator->halt();
                return call.ok_void();
            } else if (method_name == "sync") {
                double position = call.get_double("Position");
                rotator->sync(position);
                return call.ok_void();
            } else if (method_name == "targetposition") {
                double position = call.get_double("TargetPosition");
                rotator->set_target_position(position);
                return call.ok_void();
            }
        }

        return call.error(util::ErrorCode::NOT_IMPLEMENTED,
                          "Method '" + method_name + "' not yet implemented for Rotator");
    });
}

Response Router::dispatch_dome_method(std::shared_ptr<alpacacore::DomeDriver> dome, const std::string& method_name,
                                      const DeviceCall& call) {
    const Request& request = call.request();

    return call.run("dome method '" + method_name + "'", [&]() -> Response {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "altitude") {
                return call.ok(dome->get_altitude());
            } else if (method_name == "athome") {
                return call.ok(dome->get_at_home());
            } else if (method_name == "atpark") {
                return call.ok(dome->get_at_park());
            } else if (method_name == "azimuth") {
                return call.ok(dome->get_azimuth());
            } else if (method_name == "canfindhome") {
                return call.ok(dome->get_can_find_home());
            } else if (method_name == "canpark") {
                return call.ok(dome->get_can_park());
            } else if (method_name == "cansetaltitude") {
                return call.ok(dome->get_can_set_altitude());
            } else if (method_name == "cansetazimuth") {
                return call.ok(dome->get_can_set_azimuth());
            } else if (method_name == "cansetpark") {
                return call.ok(dome->get_can_set_park());
            } else if (method_name == "cansetshutter") {
                return call.ok(dome->get_can_set_shutter());
            } else if (method_name == "cansyncazimuth") {
                return call.ok(dome->get_can_sync_azimuth());
            } else if (method_name == "canslave") {
                return call.ok(dome->get_can_slave());
            } else if (method_name == "slaved") {
                return call.ok(dome->get_slaved());
            } else if (method_name == "slewing") {
                return call.ok(dome->get_slewing());
            } else if (method_name == "shutterstatus") {
                return call.ok(dome->get_shutter_status());
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "slaved") {
                bool value = call.get_bool("Slaved");
                dome->set_slaved(value);
                return call.ok_void();
            } else if (method_name == "abortslew") {
                dome->abort_slew();
                return call.ok_void();
            } else if (method_name == "closeshutter") {
                dome->close_shutter();
                return call.ok_void();
            } else if (method_name == "findhome") {
                dome->find_home();
                return call.ok_void();
            } else if (method_name == "openshutter") {
                dome->open_shutter();
                return call.ok_void();
            } else if (method_name == "park") {
                dome->park();
                return call.ok_void();
            } else if (method_name == "setpark") {
                dome->set_park();
                return call.ok_void();
            } else if (method_name == "slewtoazimuth") {
                double azimuth = call.get_double("Azimuth");
                dome->slew_to_azimuth(azimuth);
                return call.ok_void();
            } else if (method_name == "slewtoaltitude") {
                double altitude = call.get_double("Altitude");
                dome->slew_to_altitude(altitude);
                return call.ok_void();
            } else if (method_name == "synctoazimuth") {
                double azimuth = call.get_double("Azimuth");
                dome->sync_to_azimuth(azimuth);
                return call.ok_void();
            }
        }

        return call.error(util::ErrorCode::NOT_IMPLEMENTED,
                          "Method '" + method_name + "' not yet implemented for Dome");
    });
}

Response Router::dispatch_covercalibrator_method(std::shared_ptr<alpacacore::CoverCalibratorDriver> covercalibrator,
                                                 const std::string& method_name, const DeviceCall& call) {
    const Request& request = call.request();

    return call.run("cover calibrator method '" + method_name + "'", [&]() -> Response {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "brightness") {
                return call.ok(covercalibrator->get_brightness());
            } else if (method_name == "calibratorchanging") {
                return call.ok(covercalibrator->get_calibrator_changing());
            } else if (method_name == "calibratorstate") {
                return call.ok(static_cast<int>(covercalibrator->get_calibrator_state()));
            } else if (method_name == "covermoving") {
                return call.ok(covercalibrator->get_cover_moving());
            } else if (method_name == "coverstate") {
                return call.ok(static_cast<int>(covercalibrator->get_cover_state()));
            } else if (method_name == "maxbrightness") {
                return call.ok(covercalibrator->get_max_brightness());
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "calibratoron") {
                int brightness = call.get_int("Brightness");
                covercalibrator->calibrator_on(brightness);
                return call.ok_void();
            } else if (method_name == "calibratoroff") {
                covercalibrator->calibrator_off();
                return call.ok_void();
            } else if (method_name == "opencover") {
                covercalibrator->open_cover();
                return call.ok_void();
            } else if (method_name == "closecover") {
                covercalibrator->close_cover();
                return call.ok_void();
            } else if (method_name == "haltcover") {
                covercalibrator->halt_cover();
                return call.ok_void();
            }
        }

        return call.error(util::ErrorCode::NOT_IMPLEMENTED,
                          "Method '" + method_name + "' not yet implemented for CoverCalibrator");
    });
}

Response Router::dispatch_observingconditions_method(
    std::shared_ptr<alpacacore::ObservingConditionsDriver> observingconditions, const std::string& method_name,
    const DeviceCall& call) {
    const Request& request = call.request();

    auto parse_property_name = [&]() -> std::string {
        if (auto value = get_query_param_case_insensitive(request, "PropertyName")) {
            return *value;
        }
        if (auto value = get_query_param_case_insensitive(request, "SensorName")) {
            return *value;
        }
        if (const auto* val = call.json_field("PropertyName")) {
            if (!val->is_string()) {
                throw_bad_request("Invalid JSON value for parameter: PropertyName");
            }
            return val->get<std::string>();
        }
        if (const auto* val = call.json_field("SensorName")) {
            if (!val->is_string()) {
                throw_bad_request("Invalid JSON value for parameter: SensorName");
            }
            return val->get<std::string>();
        }
        if (auto value = call.form_field("PropertyName")) {
            return *value;
        }
        if (auto value = call.form_field("SensorName")) {
            return *value;
        }
        throw_bad_request("Missing parameter: SensorName");
    };

    return call.run("observing conditions method '" + method_name + "'", [&]() -> Response {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "averageperiod") {
                return call.ok(observingconditions->get_average_period());
            } else if (method_name == "cloudcover") {
                return call.ok(observingconditions->get_cloud_cover());
            } else if (method_name == "dewpoint") {
                return call.ok(observingconditions->get_dew_point());
            } else if (method_name == "humidity") {
                return call.ok(observingconditions->get_humidity());
            } else if (method_name == "pressure") {
                return call.ok(observingconditions->get_pressure());
            } else if (method_name == "rainrate") {
                return call.ok(observingconditions->get_rain_rate());
            } else if (method_name == "skybrightness") {
                return call.ok(observingconditions->get_sky_brightness());
            } else if (method_name == "skyquality") {
                return call.ok(observingconditions->get_sky_quality());
            } else if (method_name == "skytemperature") {
                return call.ok(observingconditions->get_sky_temperature());
            } else if (method_name == "starfwhm") {
                return call.ok(observingconditions->get_star_fwhm());
            } else if (method_name == "temperature") {
                return call.ok(observingconditions->get_temperature());
            } else if (method_name == "winddirection") {
                return call.ok(observingconditions->get_wind_direction());
            } else if (method_name == "windgust") {
                return call.ok(observingconditions->get_wind_gust());
            } else if (method_name == "windspeed") {
                return call.ok(observingconditions->get_wind_speed());
            } else if (method_name == "timesincelastupdate") {
                std::string property_name = parse_property_name();
                return call.ok(observingconditions->get_time_since_last_update(property_name));
            } else if (method_name == "sensordescription") {
                std::string property_name = parse_property_name();
                return call.ok(observingconditions->get_sensor_description(property_name));
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "averageperiod") {
                double value = call.get_double("AveragePeriod");
                observingconditions->set_average_period(value);
                return call.ok_void();
            } else if (method_name == "refresh") {
                observingconditions->refresh();
                return call.ok_void();
            }
        }

        return call.error(util::ErrorCode::NOT_IMPLEMENTED,
                          "Method '" + method_name + "' not yet implemented for ObservingConditions");
    });
}

Response Router::dispatch_safetymonitor_method(std::shared_ptr<alpacacore::SafetyMonitorDriver> safetymonitor,
                                               const std::string& method_name, const DeviceCall& call) {
    const Request& request = call.request();

    // No SafetyMonitor method reads a parameter today, so the 400 text path of run()
    // is unreachable here. Kept so a method that adds one answers 400 text like the
    // other dispatchers.
    return call.run("safety monitor method '" + method_name + "'", [&]() -> Response {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "issafe") {
                return call.ok(safetymonitor->get_is_safe());
            }
        }

        return call.error(util::ErrorCode::NOT_IMPLEMENTED,
                          "Method '" + method_name + "' not yet implemented for SafetyMonitor");
    });
}

Response Router::handle_root(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    response.set_content_type("application/json");
    
    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
    }

    // Return a simple JSON response with server information
    nlohmann::json info;
    std::string server_name;
    std::string version;
    {
        std::lock_guard<std::mutex> lock(server_info_mutex_);
        server_name = server_name_;
        version = manufacturer_version_;
    }

    if (management_driver_) {
        server_name = management_driver_->get_name();
        version = management_driver_->get_version();
    }

    info["ServerName"] = server_name;
    info["Version"] = version;
    info["ManagementAPI"] = "/management/v1";
    info["DeviceAPI"] = "/api/v1";
    info["Endpoints"] = nlohmann::json::array({
        "/management/v1/description",
        "/management/v1/configureddevices",
        "/api/v1/{devicetype}/{devicenumber}/{method}"
    });

    AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
    alpaca_response.value = info;
    response.set_body(alpaca_response);
    return response;
}

Response Router::handle_api_versions(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    response.set_content_type("application/json");
    
    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
    }

    // #1486: the Management API defines this path for GET only.
    if (request.method() != HttpMethod::GET) {
        set_wrong_verb_text(response, "apiversions");  // #1508: 400 + text, not 405
        return response;
    }

    try {
        // Alpaca API versions endpoint returns array of supported API versions
        // Currently only version 1 is defined
        nlohmann::json versions = nlohmann::json::array({1});

        AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
        alpaca_response.value = versions;
        response.set_body(alpaca_response);
        
        util::log_info("API versions response: " + versions.dump());
    } catch (const std::exception& e) {
        util::log_error("Error getting API versions: " + std::string(e.what()));
        AlpacaResponse alpaca_response =
            make_error_response(client_tx_id, server_tx_id, util::exception_to_error_code(e), e.what());
        response.set_body(alpaca_response);
    }

    return response;
}

Response Router::handle_build_info(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    response.set_content_type("application/json");

    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
    }

    try {
        // Independent of the ASCOM-spec description payload's ManufacturerVersion
        // (a static release number from VERSION) -- this reflects the actual git
        // checkout, so a dev build on a feature branch doesn't read as a release.
        nlohmann::json info;
        info["Version"] = alpacahttp::kVersion;
        info["GitBranch"] = alpacahttp::kGitBranch;
        info["GitCommit"] = alpacahttp::kGitCommit;
        info["GitDirty"] = alpacahttp::kGitDirty;
        info["GitIsRelease"] = alpacahttp::kGitIsRelease;
        info["GitRemoteUrl"] = alpacahttp::kGitRemoteUrl;

        AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
        alpaca_response.value = info;
        response.set_body(alpaca_response);
    } catch (const std::exception& e) {
        util::log_error("Error getting build info: " + std::string(e.what()));
        AlpacaResponse alpaca_response =
            make_error_response(client_tx_id, server_tx_id, util::exception_to_error_code(e), e.what());
        response.set_body(alpaca_response);
    }

    return response;
}

Response Router::handle_device_catalog(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    response.set_content_type("application/json");

    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
    }

    if (request.method() != HttpMethod::GET) {
        AlpacaResponse alpaca_response =
            make_error_response(client_tx_id, server_tx_id, util::ErrorCode::INVALID_OPERATION,
                                "Unsupported HTTP method for devicecatalog endpoint");
        response.set_body(alpaca_response);
        response.set_status(405, "Method Not Allowed");
        response.set_header("Allow", "GET");
        return response;
    }

    try {
        AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
        alpaca_response.value = catalog_json::describe_json(catalog_);
        response.set_body(alpaca_response);
    } catch (const std::exception& e) {
        util::log_error("Error getting device catalog: " + std::string(e.what()));
        AlpacaResponse alpaca_response =
            make_error_response(client_tx_id, server_tx_id, util::exception_to_error_code(e), e.what());
        response.set_body(alpaca_response);
    }

    return response;
}

// The cameras a vendor's camera-attached thermal switch can bind to, for the web
// UI's camera picker: GET /management/v1/thermalcameras?vendor=zwo. The lister
// (registered in the catalog when the vendor is built) may open cameras through
// the vendor's ref-counted wrapper to read their controls and serial.
Response Router::handle_thermal_cameras(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    response.set_content_type("application/json");

    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
    }

    if (request.method() != HttpMethod::GET) {
        AlpacaResponse alpaca_response =
            make_error_response(client_tx_id, server_tx_id, util::ErrorCode::INVALID_OPERATION,
                                "Unsupported HTTP method for thermalcameras endpoint");
        response.set_body(alpaca_response);
        response.set_status(405, "Method Not Allowed");
        response.set_header("Allow", "GET");
        return response;
    }

    const std::string vendor = request.get_query_param("vendor");
    try {
        const auto cameras = catalog_.list_thermal_cameras(vendor);
        if (!cameras.has_value()) {
            AlpacaResponse alpaca_response = make_error_response(
                client_tx_id, server_tx_id, util::ErrorCode::INVALID_VALUE,
                "No thermal switch camera listing for vendor '" + util::escape_for_log(vendor) + "' in this build");
            response.set_body(alpaca_response);
            return response;
        }
        nlohmann::json list = nlohmann::json::array();
        for (const auto& camera : *cameras) {
            list.push_back({{"Index", camera.index},
                            {"Name", camera.name},
                            {"Serial", camera.serial},
                            {"HasThermal", camera.has_thermal}});
        }
        AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
        alpaca_response.value = std::move(list);
        response.set_body(alpaca_response);
    } catch (const std::exception& e) {
        util::log_error("Error listing thermal switch cameras: " + std::string(e.what()));
        AlpacaResponse alpaca_response =
            make_error_response(client_tx_id, server_tx_id, util::exception_to_error_code(e), e.what());
        response.set_body(alpaca_response);
    }

    return response;
}

Response Router::handle_static_file(const Request& request) {
    Response response;
    std::string file_path = request.path();
    
    // Map root to index.html
    if (file_path == "/" || file_path == "") {
        file_path = "/web/index.html";
    }
    
    // Remove leading slash and build full path
    if (file_path[0] == '/') {
        file_path = file_path.substr(1);
    }
    
    // Security: Only allow files from web directory
    if (!file_path.starts_with("web/") && file_path != "web/index.html") {
        response.set_status(403, "Forbidden");
        response.set_body("Access denied");
        return response;
    }

    // Security: reject any path containing a ".." segment (path traversal).
    // Checked on the raw request path so "web/../../etc/passwd" never reaches
    // the filesystem lookups below.
    {
        const std::filesystem::path requested(file_path);
        for (const auto& segment : requested) {
            if (segment == "..") {
                response.set_status(404, "Not Found");
                response.set_body("File not found");
                return response;
            }
        }
    }

    // Extract filename from path (e.g., web/index.html -> index.html)
    std::string filename = file_path.substr(4);  // Skip leading "web/" (4 characters)

    // Try multiple possible locations for web files
    std::vector<std::string> possible_roots = {
        "web",                         // Current directory
        "../web",                      // Parent directory (if running from build/)
        "../../web",                   // Two levels up
        "../AlpacaHTTP/web",           // From build directory
        "AlpacaHTTP/web",              // Alternative location
        "/usr/share/alpacabridge/web"  // Packaged install (.deb)
    };

    std::ifstream file;
    std::string full_path;
    bool found = false;

    for (const auto& root : possible_roots) {
        // Security: canonicalize and confine the resolved path under the web
        // root; symlinks or residual traversal escaping the root are rejected.
        std::error_code ec;
        const auto canonical_root = std::filesystem::weakly_canonical(root, ec);
        if (ec) {
            continue;
        }
        const auto canonical_path = std::filesystem::weakly_canonical(std::filesystem::path(root) / filename, ec);
        if (ec) {
            continue;
        }
        const auto root_str = canonical_root.string();
        const auto path_str = canonical_path.string();
        if (path_str.size() <= root_str.size() || !path_str.starts_with(root_str) ||
            path_str[root_str.size()] != std::filesystem::path::preferred_separator) {
            continue;
        }
        file.open(canonical_path, std::ios::binary);
        if (file.is_open()) {
            full_path = path_str;
            found = true;
            break;
        }
        file.close();
    }

    if (!found) {
        response.set_status(404, "Not Found");
        std::string error_msg = "File not found: " + file_path + "\n";
        error_msg += "Searched in:\n";
        for (const auto& root : possible_roots) {
            error_msg += "  - ";
            error_msg += root;
            error_msg += "/";
            error_msg += filename;
            error_msg += "\n";
        }
        response.set_body(error_msg);
        return response;
    }
    
    // Read file content
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    
    // Set appropriate content type
    if (file_path.find(".html") != std::string::npos) {
        response.set_content_type("text/html");
    } else if (file_path.find(".css") != std::string::npos) {
        response.set_content_type("text/css");
    } else if (file_path.find(".js") != std::string::npos) {
        response.set_content_type("application/javascript");
    } else if (file_path.find(".json") != std::string::npos) {
        response.set_content_type("application/json");
    } else {
        response.set_content_type("text/plain");
    }
    
    response.set_body(content);
    return response;
}

Response Router::handle_setup(const Request& request, std::uint32_t server_tx_id) {
    Response response;

    util::log_info("Handling setup endpoint: " + util::escape_for_log(request.path()));

    // Setup endpoints are expected to return an HTML page.
    // We provide a simple stub page that points users to the web UI.
    // Example path: /setup/v1/telescope/0/setup
    static const std::regex setup_regex(R"(/setup/v1/([^/]+)/(\d+)/setup)");
    std::smatch matches;

    if (!std::regex_match(request.path(), matches, setup_regex)) {
        // Client input, so DEBUG, escaped and cut to 256 bytes (#740).
        util::log_debug("Setup endpoint regex did not match: " + util::escape_for_log(request.path()));
        // Not a valid setup path; return 404 as Alpaca error.
        response.set_status(404, "Not Found");
        AlpacaResponse alpaca_response = make_error_response(
            0, server_tx_id,
            util::ErrorCode::INVALID_VALUE,
            "Endpoint not found: " + request.path()
        );
        response.set_body(alpaca_response);
        return response;
    }

    util::log_info("Setup endpoint matched for device type=" + matches[1].str() +
                   " device=" + matches[2].str());

    response.set_status(200, "OK");
    response.set_content_type("text/html");
    response.set_body(
        "<!DOCTYPE html><html><head><title>Alpaca Device Setup</title></head>"
        "<body><h2>Alpaca Device Setup</h2>"
        "<p>This device does not expose a custom setup page. "
        "Please use the AlpacaHTTP web interface at <a href=\"/\">/</a> to configure devices.</p>"
        "</body></html>"
    );
    return response;
}

namespace {

// Issue #860: `alignmentMode` ("auto", "altaz" or "equatorial") tells the
// SynScan and Celestron drivers the geometry of a mount whose handset does not
// report it. Returns the value when it is one of those three, nullopt when it
// is absent or unknown; an unknown value reads as "auto" and is not persisted.
std::optional<std::string> known_alignment_mode(const nlohmann::json& config) {
    if (!catalog_json::has_value(config, "alignmentMode") || !config.at("alignmentMode").is_string()) {
        return std::nullopt;
    }
    std::string mode = config.at("alignmentMode").get<std::string>();
    if (alpacacore::util::is_known_alignment_mode(mode)) {
        return mode;
    }
    return std::nullopt;
}

// #860 for a catalog descriptor with an `alignmentMode` field (Celestron,
// SynScan): the
// config without the key when its value is not one of the three known strings,
// so a wrong-typed or unknown value drops like the deleted arm's did instead
// of failing the typed read. Other configs come back unchanged. Keyed on the
// field name, not the vendor: do not add a second copy.
nlohmann::json without_unknown_alignment_mode(const nlohmann::json& config,
                                              std::span<const alpacacore::catalog::FieldRef> fields) {
    const bool declared = std::any_of(fields.begin(), fields.end(), [](const alpacacore::catalog::FieldRef& f) {
        return std::string_view(f.key) == "alignmentMode";
    });
    if (!declared || !config.is_object() || known_alignment_mode(config) || !config.contains("alignmentMode")) {
        return config;
    }
    nlohmann::json out = config;
    out.erase("alignmentMode");
    return out;
}

// A RecordList field (ports[]) is applied positionally, and the deleted ToupTek
// arm skipped an entry that is not an object ("ports":[null]) instead of refusing
// it. The typed read refuses a non-object entry, so each one becomes an empty
// object here: it keeps its position and overrides nothing. Other configs come
// back unchanged.
nlohmann::json with_object_record_entries(const nlohmann::json& config,
                                          std::span<const alpacacore::catalog::FieldRef> fields) {
    if (!config.is_object()) return config;
    nlohmann::json out = config;
    for (const auto& f : fields) {
        if (f.kind != alpacacore::catalog::FieldRef::Kind::RecordList) continue;
        const auto it = out.find(f.key);
        if (it == out.end() || !it->is_array()) continue;
        for (auto& elem : *it) {
            if (!elem.is_object()) elem = nlohmann::json::object();
        }
    }
    return out;
}

// Whether config_from_json can read `value` as the integer an Int selector holds: a
// JSON integer, or a float with no fractional part, inside the int64 range.
bool is_readable_integer(const nlohmann::json& value) {
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    }
    if (value.is_number_integer()) return true;
    if (!value.is_number_float()) return false;
    const double d = value.get<double>();
    constexpr double kTwo63 = 9223372036854775808.0;
    return std::isfinite(d) && d == std::trunc(d) && d >= -kTwo63 && d < kTwo63;
}

// A stored selector (an enumeration index or device id) that is not an integer
// config_from_json can read -- "1", true, 1.5 -- is removed from `config` and its
// key returned, so the entry still registers and stays listed with the selector
// unset, as it did before the selectors were catalog fields. Used only for a
// schema with forget_dropped_selectors, whose callers also forget the key in the
// stored entry.
std::vector<std::string> drop_unreadable_selectors(nlohmann::json& config,
                                                   std::span<const alpacacore::catalog::FieldRef> fields) {
    std::vector<std::string> dropped;
    if (!config.is_object()) return dropped;
    for (const auto& f : fields) {
        if (f.role != alpacacore::catalog::Role::EnumerationIndex && f.role != alpacacore::catalog::Role::DeviceId) {
            continue;
        }
        const auto it = config.find(f.key);
        if (it == config.end() || it->is_null() || is_readable_integer(*it)) continue;
        config.erase(it);
        dropped.emplace_back(f.key);
    }
    return dropped;
}

}  // namespace

Response Router::handle_configure_device(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    response.set_content_type("application/json");
    
    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
    }

    // Issue #348: the whole management surface is unauthenticated under the
    // documented trusted-LAN threat model, which is a deliberate stance; the
    // point is that every state-changing endpoint should take that stance on
    // purpose rather than differ by accident. A POST with Content-Type:
    // text/plain is not preflighted and the handlers parse the body
    // regardless of content type, so nothing on the browser side stops a
    // drive-by request from reaching this.
    // Rewrites persisted device configuration, which survives a restart.
    if (auto rejected = reject_cross_origin_request(request, client_tx_id, server_tx_id, "device configuration")) {
        return *rejected;
    }

    // Only allow POST or PUT requests
    if (request.method() != HttpMethod::POST && request.method() != HttpMethod::PUT) {
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::INVALID_VALUE,
            "Method not allowed. Use POST or PUT."
        );
        response.set_body(alpaca_response);
        response.set_status(405, "Method Not Allowed");
        response.set_header("Allow", "POST, PUT");
        return response;
    }
    
    try {
        load_persisted_devices();

        // Parse JSON body
        nlohmann::json config;
        try {
            config = nlohmann::json::parse(request.body());
        } catch (const nlohmann::json::exception& e) {
            AlpacaResponse alpaca_response = make_error_response(
                client_tx_id, server_tx_id,
                util::ErrorCode::INVALID_VALUE,
                "Invalid JSON: " + std::string(e.what())
            );
            response.set_body(alpaca_response);
            return response;
        }

        std::string vendor = catalog_json::get_string(config, "vendor");
        std::string device_type_str = catalog_json::get_string(config, "deviceType");

        if (vendor.empty() || device_type_str.empty()) {
            AlpacaResponse alpaca_response = make_error_response(
                client_tx_id, server_tx_id,
                util::ErrorCode::INVALID_VALUE,
                "Missing required fields: deviceType and vendor"
            );
            response.set_body(alpaca_response);
            return response;
        }

        // open-astro#444: the opt-out flag is vendor-agnostic and consumed by
        // the router alone, so no catalog descriptor declares it.
        // Refuse a non-boolean here (the #388 rule: a wrong-typed field is
        // named to the caller, never left to throw from a later read) rather
        // than let sanitize_device_config() copy it verbatim into the file.
        if (catalog_json::has_value(config, "learnSiteFromClient") && !config["learnSiteFromClient"].is_boolean()) {
            AlpacaResponse alpaca_response =
                make_error_response(client_tx_id, server_tx_id, util::ErrorCode::INVALID_VALUE,
                                    std::string("Device config field 'learnSiteFromClient' must be a boolean (got ") +
                                        config["learnSiteFromClient"].type_name() + ")");
            response.set_body(alpaca_response);
            return response;
        }

        // Decision 0018: keep a stored ID the client sends back (the web UI's edit
        // removes the entry first, so its own ID is free again) unless another
        // entry holds it, or another configuredevice call chose it and has not
        // stored its entry yet; otherwise mint one. The entry this call replaces
        // (same vendor, type and number, as add_or_replace_persisted_device()
        // matches) is not another entry. The choice is reserved until this call
        // stores its entry or fails.
        {
            std::lock_guard<std::mutex> lock(persisted_devices_mutex_);
            std::set<std::string> taken = reserved_unique_ids_;
            const auto replaced = persisted_key(config);
            for (const auto& entry : persisted_devices_) {
                const auto key = persisted_key(entry);
                if (replaced && key && key->vendor == replaced->vendor && key->device_type == replaced->device_type &&
                    key->device_number == replaced->device_number) {
                    continue;
                }
                if (const std::string held = stored_unique_id(entry); !held.empty()) taken.insert(held);
            }
            config["uniqueId"] = choose_unique_id(config, taken);
            reserved_unique_ids_.insert(config["uniqueId"].get<std::string>());
        }
        struct UniqueIdReservation {
            std::function<void()> release;
            ~UniqueIdReservation() { release(); }
        } reservation{[this, unique_id = config["uniqueId"].get<std::string>()] {
            std::lock_guard<std::mutex> lock(persisted_devices_mutex_);
            reserved_unique_ids_.erase(unique_id);
        }};

        std::string error_message;
        nlohmann::json learned_config = nlohmann::json::object();
        if (!register_device_from_config(config, error_message, ConfigSource::Api, &learned_config)) {
            if (error_message.empty()) {
                error_message = "Failed to register device. Please verify the configuration.";
            }
            std::int32_t error_code = util::ErrorCode::INVALID_VALUE;
            if (error_message.find("not yet supported") != std::string::npos ||
                error_message.find("not enabled") != std::string::npos) {
                error_code = util::ErrorCode::NOT_IMPLEMENTED;
            }
            AlpacaResponse alpaca_response = make_error_response(
                client_tx_id, server_tx_id,
                error_code,
                error_message
            );
            response.set_body(alpaca_response);
            if (error_message.rfind(kHardwareConfigRefusal, 0) == 0) {
                response.set_status(400, "Bad Request");
            }
            return response;
        }

        nlohmann::json stored_config = config;
        stored_config.update(learned_config);
        add_or_replace_persisted_device(sanitize_device_config(stored_config));
        save_persisted_devices();

        AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
        alpaca_response.value = std::string("Device registered successfully");
        response.set_body(alpaca_response);
        return response;
    } catch (const std::exception& e) {
        // A config field the client typed wrong is InvalidValue: DEBUG, not ERROR (ADR 0007 rule 9).
        if (const auto* alpaca = dynamic_cast<const alpacacore::AlpacaException*>(&e)) {
            log_alpaca_exception("Error configuring device", *alpaca);
        } else {
            util::log_error("Error configuring device: " + util::escape_for_log(e.what(), 4096));
        }
        AlpacaResponse alpaca_response =
            make_error_response(client_tx_id, server_tx_id, util::exception_to_error_code(e), e.what());
        response.set_body(alpaca_response);
        return response;
    }
}

Response Router::handle_remove_device(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    response.set_content_type("application/json");
    
    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
    }

    // Issue #348: the whole management surface is unauthenticated under the
    // documented trusted-LAN threat model, which is a deliberate stance; the
    // point is that every state-changing endpoint should take that stance on
    // purpose rather than differ by accident. A POST with Content-Type:
    // text/plain is not preflighted and the handlers parse the body
    // regardless of content type, so nothing on the browser side stops a
    // drive-by request from reaching this.
    // Removes a configured device, taking its persisted entry with it.
    if (auto rejected = reject_cross_origin_request(request, client_tx_id, server_tx_id, "device removal")) {
        return *rejected;
    }

    // Only allow POST or PUT requests
    if (request.method() != HttpMethod::POST && request.method() != HttpMethod::PUT) {
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::INVALID_VALUE,
            "Method not allowed. Use POST or PUT."
        );
        response.set_body(alpaca_response);
        response.set_status(405, "Method Not Allowed");
        response.set_header("Allow", "POST, PUT");
        return response;
    }
    
    try {
        load_persisted_devices();

        // Parse JSON body
        nlohmann::json config;
        try {
            config = nlohmann::json::parse(request.body());
        } catch (const nlohmann::json::exception& e) {
            AlpacaResponse alpaca_response = make_error_response(
                client_tx_id, server_tx_id,
                util::ErrorCode::INVALID_VALUE,
                "Invalid JSON: " + std::string(e.what())
            );
            response.set_body(alpaca_response);
            return response;
        }

        std::string device_type_str = catalog_json::get_string(config, "deviceType");
        std::string vendor = catalog_json::get_string(config, "vendor");
        int device_number = catalog_json::get_int(config, "deviceNumber", -1);

        if (device_type_str.empty() || device_number < 0) {
            AlpacaResponse alpaca_response = make_error_response(
                client_tx_id, server_tx_id,
                util::ErrorCode::INVALID_VALUE,
                "Missing required fields: deviceType and deviceNumber"
            );
            response.set_body(alpaca_response);
            return response;
        }
        
        // Convert device type string to enum
        alpacacore::DeviceType device_type;
        try {
            device_type = string_to_device_type(device_type_str);
        } catch (const std::exception& e) {
            AlpacaResponse alpaca_response = make_error_response(
                client_tx_id, server_tx_id,
                util::ErrorCode::INVALID_VALUE,
                "Invalid device type: " + device_type_str
            );
            response.set_body(alpaca_response);
            return response;
        }
        
        auto& registry = alpacacore::management::DeviceRegistry::instance();

        // Drop any per-client Connected registrations before the driver goes
        // away; a re-registered device at the same address must start clean.
        // The op mutex is held across clear + registry removal so an
        // in-flight connection op on this device can't re-register between
        // the clear and the removal (PR #161 review).
        bool was_registered = false;
        if (auto device = registry.get_device(device_type, device_number)) {
            {
                const auto op_mutex = device_connection_op_mutex(device);
                std::lock_guard<std::mutex> op_lock(*op_mutex);
                clear_client_connections(device.get());
                was_registered = registry.unregister_device(device_type, device_number);
            }
            // Reap the device's registry + op-mutex entries now that it is
            // out of the DeviceRegistry (issue #162: the op-mutex map grew
            // unboundedly across add/remove cycles). Must happen AFTER the
            // op lock is released — erasing under it is the mint-a-fresh-
            // mutex trap. Stragglers that fetched the device shared_ptr
            // pre-removal cannot re-insert entries after this: the
            // device_is_current guards in device_connection_op_mutex and
            // the registering handlers refuse a device the registry no
            // longer resolves (PR #164 review).
            purge_device_connection_state(device.get());
        } else {
            was_registered = registry.unregister_device(device_type, device_number);
        }

        bool was_persisted = remove_persisted_device(vendor, device_type_str, device_number);

        if (was_registered || was_persisted) {
            if (was_persisted) {
                save_persisted_devices();
            }
            AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
            alpaca_response.value = std::string("Device removed successfully");
            response.set_body(alpaca_response);
            util::log_info("Removed device: " + device_type_str + " #" + std::to_string(device_number));
            return response;
        } else {
            AlpacaResponse alpaca_response = make_error_response(
                client_tx_id, server_tx_id,
                util::ErrorCode::INVALID_VALUE,
                "Device not found: " + device_type_str + " #" + std::to_string(device_number)
            );
            response.set_body(alpaca_response);
            return response;
        }
        
    } catch (const std::exception& e) {
        // A config field the client typed wrong is InvalidValue: DEBUG, not ERROR (ADR 0007 rule 9).
        if (const auto* alpaca = dynamic_cast<const alpacacore::AlpacaException*>(&e)) {
            log_alpaca_exception("Error removing device", *alpaca);
        } else {
            util::log_error("Error removing device: " + util::escape_for_log(e.what(), 4096));
        }
        AlpacaResponse alpaca_response =
            make_error_response(client_tx_id, server_tx_id, util::exception_to_error_code(e), e.what());
        response.set_body(alpaca_response);
        return response;
    }
}

Response Router::handle_log_level(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    response.set_content_type("application/json");

    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
    }

    // Issue #348: the whole management surface is unauthenticated under the
    // documented trusted-LAN threat model, which is a deliberate stance; the
    // point is that every state-changing endpoint should take that stance on
    // purpose rather than differ by accident. A POST with Content-Type:
    // text/plain is not preflighted and the handlers parse the body
    // regardless of content type, so nothing on the browser side stops a
    // drive-by request from reaching this.
    // Changing verbosity is the quiet one: it is how evidence of any of the
    // others gets turned down after the fact. GET is exempt, so the web UI's
    // polling of the current level is unaffected.
    if (auto rejected = reject_cross_origin_request(request, client_tx_id, server_tx_id, "log level")) {
        return *rejected;
    }

    auto send_payload = [&](std::uint32_t ctx_id) {
        AlpacaResponse alpaca_response(ctx_id, server_tx_id);
        alpaca_response.value = make_log_level_payload();
        response.set_body(alpaca_response);
        return response;
    };

    try {
        if (request.method() == HttpMethod::GET) {
            return send_payload(client_tx_id);
        }

        if (request.method() == HttpMethod::POST || request.method() == HttpMethod::PUT) {
            if (request.body().empty()) {
                AlpacaResponse err = make_error_response(
                    client_tx_id, server_tx_id,
                    util::ErrorCode::VALUE_NOT_SET,
                    "Missing request body"
                );
                response.set_body(err);
                return response;
            }

            auto json_opt = parse_json(request.body());
            if (!json_opt) {
                AlpacaResponse err = make_error_response(
                    client_tx_id, server_tx_id,
                    util::ErrorCode::INVALID_VALUE,
                    "Invalid JSON payload"
                );
                response.set_body(err);
                return response;
            }

            const auto& body = *json_opt;
            auto body_client_tx = extract_client_transaction_id(body);
            if (body_client_tx != 0) {
                client_tx_id = body_client_tx;
            }

            std::string requested_level;
            if (body.contains("level")) {
                requested_level = body["level"].get<std::string>();
            } else if (body.contains("Level")) {
                requested_level = body["Level"].get<std::string>();
            } else {
                AlpacaResponse err = make_error_response(
                    client_tx_id, server_tx_id,
                    util::ErrorCode::VALUE_NOT_SET,
                    "Request must include a 'level' property"
                );
                response.set_body(err);
                return response;
            }

            auto parsed_level = parse_log_level_string(requested_level);
            if (!parsed_level.has_value()) {
                AlpacaResponse err = make_error_response(
                    client_tx_id, server_tx_id,
                    util::ErrorCode::INVALID_VALUE,
                    "Unknown log level: " + requested_level
                );
                response.set_body(err);
                return response;
            }

            alpacacore::logging::set_log_level(*parsed_level);
            try {
                util::save_runtime_log_level(*parsed_level);
            } catch (const std::exception& save_err) {
                // Persistence is best-effort; failure must not block the API.
                util::log_warning(std::string("Failed to persist log level: ") +
                                  save_err.what());
            }
            util::log_info("Log level set to " + log_level_to_string(*parsed_level));
            return send_payload(client_tx_id);
        }

        AlpacaResponse err = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::INVALID_OPERATION,
            "Unsupported HTTP method for log level endpoint"
        );
        response.set_body(err);
        return response;
    } catch (const std::exception& e) {
        AlpacaResponse err = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::DRIVER_ERROR,
            "Failed to update log level: " + std::string(e.what())
        );
        response.set_body(err);
        return response;
    }
}

Response Router::handle_logs(const Request& request, std::uint32_t server_tx_id) {
    Response response;

    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
    }

    if (request.method() != HttpMethod::GET) {
        response.set_content_type("application/json");
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::INVALID_OPERATION,
            "Unsupported HTTP method for logs endpoint"
        );
        response.set_body(alpaca_response);
        return response;
    }

    // Serve today's on-disk daily file. The in-memory history buffer was
    // removed in favor of the durable per-day files in logging.directory.
    const std::string today = util::current_log_filename();
    const std::string log_directory = util::get_log_directory();
    std::string logs;
    bool have_logs = false;

    if (!log_directory.empty()) {
        const std::filesystem::path path =
            std::filesystem::path(log_directory) / today;
        std::error_code exists_ec;
        if (std::filesystem::exists(path, exists_ec)) {
            try {
                logs = util::read_log_file(today);
                have_logs = true;
            } catch (const std::exception& e) {
                // Real failure (size cap, permission denied, etc.) — surface
                // to the client rather than silently returning an empty body.
                response.set_content_type("application/json");
                AlpacaResponse err = make_error_response(
                    client_tx_id, server_tx_id,
                    util::ErrorCode::DRIVER_ERROR,
                    std::string("Failed to read log file: ") + e.what()
                );
                response.set_body(err);
                return response;
            }
        }
    }
    (void)have_logs;  // empty-body path is intentional when no file exists yet

    std::string format;
    if (request.has_query_param("format")) {
        format = request.get_query_param("format");
        std::transform(format.begin(), format.end(), format.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }

    if (format == "plain" || format == "text") {
        response.set_content_type("text/plain");
        if (request.has_query_param("download")) {
            response.set_header("Content-Disposition",
                "attachment; filename=\"" + today + "\"");
        }
        response.set_body(logs);
        return response;
    }

    response.set_content_type("application/json");
    AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
    alpaca_response.value = logs;
    response.set_body(alpaca_response);
    return response;
}


Response Router::handle_log_files_list(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    response.set_content_type("application/json");

    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
    }

    if (request.method() == HttpMethod::DELETE_) {
        // Issue #348: the collection DELETE removes EVERY log file, which is
        // the same evidence-removal shape as the per-file DELETE next to it
        // and as turning the log level down. Guarding the per-file form and
        // not this one would have been the accident the audit exists to
        // remove. The web UI's deleteAllLogFiles() is same-origin, so this is
        // a no-op for it; GET (the listing) stays exempt.
        if (auto rejected = reject_cross_origin_request(request, client_tx_id, server_tx_id, "log files")) {
            return *rejected;
        }
        const auto files = util::list_log_files();
        const std::filesystem::path log_directory = util::get_log_directory();
        std::size_t deleted = 0;
        try {
            for (const auto& info : files) {
                try {
                    util::delete_log_file(info.name);
                } catch (const std::exception&) {
                    // A concurrent request may have deleted it between the
                    // snapshot and now; the goal state is reached either way.
                    std::error_code exists_ec;
                    if (log_directory.empty() || std::filesystem::exists(log_directory / info.name, exists_ec)) {
                        throw;
                    }
                }
                ++deleted;
            }
            util::log_info("Deleted " + std::to_string(deleted) + " log file(s)");
            AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
            nlohmann::json payload;
            payload["DeletedCount"] = deleted;
            alpaca_response.value = payload;
            response.set_body(alpaca_response);
            return response;
        } catch (const std::exception& e) {
            // Partial deletion is possible — tell the caller exactly how far
            // it got rather than leaving the outcome ambiguous. Details (which
            // can include errno text) go to the server log, not the response.
            util::log_warning("Delete log files failed: " + std::string(e.what()));
            AlpacaResponse err = make_error_response(client_tx_id, server_tx_id, util::ErrorCode::DRIVER_ERROR,
                                                     "Failed to delete log files (deleted " + std::to_string(deleted) +
                                                         " of " + std::to_string(files.size()) +
                                                         " before the failure); see the server log for details");
            response.set_body(err);
            return response;
        }
    }

    if (request.method() != HttpMethod::GET) {
        AlpacaResponse err = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::INVALID_OPERATION,
            "Unsupported HTTP method for log files endpoint"
        );
        response.set_body(err);
        return response;
    }

    if (request.has_query_param("download")) {
        try {
            auto files = util::list_log_files();
            // Cap the combined archive so a long retention window cannot
            // build an OOM-sized string on a small SBC. Keep the newest
            // files (the relevant ones for debugging) and drop the oldest;
            // the newest file is always included.
            constexpr std::uint64_t kMaxArchiveBytes = 200ull * 1024 * 1024;
            // Must stay within gzip_compress's input-size guard (uInt-based);
            // raising the cap past it would make the download always fail.
            static_assert(kMaxArchiveBytes < std::numeric_limits<uInt>::max() / 2,
                          "archive cap must fit gzip_compress's single-pass input guard");
            std::size_t included = 0;
            std::uint64_t total_bytes = 0;
            for (const auto& info : files) {
                // Budget what read_log_file can actually return: a file over
                // its per-file cap contributes only a short error note, so it
                // must not consume its full on-disk size from the budget and
                // crowd out smaller, readable files.
                const std::uint64_t effective = std::min<std::uint64_t>(info.size, util::kMaxLogFileReadBytes);
                if (included > 0 && total_bytes + effective > kMaxArchiveBytes) {
                    break;
                }
                total_bytes += effective;
                ++included;
            }
            // list_log_files() is newest-first; emit oldest-first so the
            // combined file reads chronologically.
            std::string combined;
            if (included < files.size()) {
                combined += "===== " + std::to_string(files.size() - included) +
                            " older log file(s) omitted: archive capped at 200 MiB =====\n\n";
            }
            for (std::size_t i = included; i-- > 0;) {
                const auto& info = files[i];
                combined += "===== " + info.name + " =====\n";
                try {
                    combined += util::read_log_file(info.name);
                } catch (const std::exception& e) {
                    combined += std::string("[unable to read: ") + e.what() + "]\n";
                }
                if (!combined.empty() && combined.back() != '\n') {
                    combined += '\n';
                }
                combined += '\n';
            }
            response.set_content_type("application/gzip");
            response.set_header("Content-Disposition", "attachment; filename=\"alpacabridge-logs.txt.gz\"");
            response.set_body(gzip_compress(combined));
            return response;
        } catch (const std::exception& e) {
            response.set_content_type("application/json");
            AlpacaResponse err = make_error_response(client_tx_id, server_tx_id, util::ErrorCode::DRIVER_ERROR,
                                                     std::string("Failed to build log archive: ") + e.what());
            response.set_body(err);
            return response;
        }
    }

    try {
        nlohmann::json payload;
        payload["Directory"] = util::get_log_directory();
        nlohmann::json files = nlohmann::json::array();
        for (const auto& info : util::list_log_files()) {
            nlohmann::json entry;
            entry["Name"] = info.name;
            entry["Size"] = info.size;
            entry["Modified"] = info.modified_unix;
            files.push_back(std::move(entry));
        }
        payload["Files"] = std::move(files);

        AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
        alpaca_response.value = payload;
        response.set_body(alpaca_response);
        return response;
    } catch (const std::exception& e) {
        // Catch here so the Alpaca error envelope carries the caller's
        // ClientTransactionID rather than the route()-level fallback of 0.
        AlpacaResponse err = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::DRIVER_ERROR,
            std::string("Failed to list log files: ") + e.what()
        );
        response.set_body(err);
        return response;
    }
}

Response Router::handle_log_file_item(const Request& request,
                                      const std::string& filename,
                                      std::uint32_t server_tx_id) {
    Response response;

    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
    }

    // Issue #348: DELETE here destroys a log file, which is the same
    // evidence-removal shape as turning the log level down. Placed before the
    // filename validation so a cross-origin caller learns nothing about which
    // names exist. GET is exempt, so the web UI's log viewer is unaffected.
    if (auto rejected = reject_cross_origin_request(request, client_tx_id, server_tx_id, "log file")) {
        return *rejected;
    }

    if (!util::is_valid_log_filename(filename)) {
        response.set_content_type("application/json");
        AlpacaResponse err = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::INVALID_VALUE,
            "Invalid log file name"
        );
        response.set_body(err);
        return response;
    }

    if (request.method() == HttpMethod::GET) {
        try {
            const std::string contents = util::read_log_file(filename);
            response.set_content_type("text/plain");
            if (request.has_query_param("download")) {
                response.set_header(
                    "Content-Disposition",
                    "attachment; filename=\"" + filename + "\"");
            }
            response.set_body(contents);
            return response;
        } catch (const std::exception& e) {
            response.set_content_type("application/json");
            // DRIVER_ERROR is the right runtime-failure code here: the
            // endpoint exists (so NOT_IMPLEMENTED would be misleading); the
            // failure is "file missing", "size cap exceeded", or generic I/O.
            AlpacaResponse err = make_error_response(
                client_tx_id, server_tx_id,
                util::ErrorCode::DRIVER_ERROR,
                std::string("Failed to read log file: ") + e.what()
            );
            response.set_body(err);
            return response;
        }
    }

    if (request.method() == HttpMethod::DELETE_) {
        try {
            util::delete_log_file(filename);
            util::log_info("Deleted log file " + filename);
            response.set_content_type("application/json");
            AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
            nlohmann::json payload;
            payload["Deleted"] = filename;
            alpaca_response.value = payload;
            response.set_body(alpaca_response);
            return response;
        } catch (const std::exception& e) {
            response.set_content_type("application/json");
            AlpacaResponse err = make_error_response(
                client_tx_id, server_tx_id,
                util::ErrorCode::DRIVER_ERROR,
                std::string("Failed to delete log file: ") + e.what()
            );
            response.set_body(err);
            return response;
        }
    }

    response.set_content_type("application/json");
    AlpacaResponse err = make_error_response(
        client_tx_id, server_tx_id,
        util::ErrorCode::INVALID_OPERATION,
        "Unsupported HTTP method for log file endpoint"
    );
    response.set_body(err);
    return response;
}

Response Router::handle_shutdown(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    response.set_content_type("application/json");
    
    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
    }

    // Issue #348: the whole management surface is unauthenticated under the
    // documented trusted-LAN threat model, which is a deliberate stance; the
    // point is that every state-changing endpoint should take that stance on
    // purpose rather than differ by accident. A POST with Content-Type:
    // text/plain is not preflighted and the handlers parse the body
    // regardless of content type, so nothing on the browser side stops a
    // drive-by request from reaching this.
    // Stops the daemon. On a remote rig undoing this needs physical access.
    if (auto rejected = reject_cross_origin_request(request, client_tx_id, server_tx_id, "shutdown")) {
        return *rejected;
    }

    // Only allow POST or PUT requests
    if (request.method() != HttpMethod::POST && request.method() != HttpMethod::PUT) {
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::INVALID_VALUE,
            "Method not allowed. Use POST or PUT."
        );
        response.set_body(alpaca_response);
        response.set_status(405, "Method Not Allowed");
        response.set_header("Allow", "POST, PUT");
        return response;
    }
    
    // Call shutdown callback if set
    if (shutdown_callback_) {
        AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
        alpaca_response.value = std::string("Shutdown initiated");
        response.set_body(alpaca_response);
        
        // Call callback asynchronously (after response is sent)
        // Use a small delay to ensure response is sent first
        std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (shutdown_callback_) {
                shutdown_callback_();
            }
        }).detach();
        
        return response;
    } else {
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::NOT_IMPLEMENTED,
            "Shutdown callback not configured"
        );
        response.set_body(alpaca_response);
        return response;
    }
}

namespace {

// CSRF guard shared by the state-changing management endpoints (PR #198
// follow-up; extended to synctime by issue #298). These endpoints are
// unauthenticated under the trusted-LAN threat model, so a malicious page
// open in a browser on the same LAN could otherwise fire a cross-origin
// request at them. Browsers always attach an Origin header to a cross-origin
// mutating request, so reject any whose Origin host does not match the Host
// the request was addressed to. Non-browser clients (curl, native apps) send
// no Origin and are unaffected, as is the same-origin web portal.
//
// Returns the 403 response to send, or std::nullopt when the request may
// proceed. `what` names the endpoint in the error message.
// `client_tx_id` is the value the caller already parsed from the request.
// The Alpaca convention is that ClientTransactionID echoes what the client
// sent, and clients are allowed to match responses to requests on it; this
// path used to hardcode 0, so the one reply whose explanation a client most
// needs to surface ("your origin was refused") was the one reply it could not
// attribute (issue #384).
std::optional<Response> reject_cross_origin_request(const Request& request, std::uint32_t client_tx_id,
                                                    std::uint32_t server_tx_id, const char* what) {
    if (request.method() == HttpMethod::GET || !request.has_header("Origin")) {
        return std::nullopt;
    }
    const std::string origin = request.get_header("Origin");
    const std::string host = request.get_header("Host");
    // Strip scheme from Origin, then compare host[:port] exactly.
    const auto scheme_end = origin.find("://");
    const std::string origin_host = scheme_end == std::string::npos ? origin : origin.substr(scheme_end + 3);
    if (!host.empty() && origin_host == host) {
        return std::nullopt;
    }
    // Issue #509: the ID may arrive only in the JSON body, which the handlers
    // have not read yet when they call this guard. Precedence: a non-zero
    // query-string ID wins over the body here, whereas handle_description()
    // and handle_log_level() let a non-zero body ID override the query one.
    if (client_tx_id == 0 && !request.body().empty()) {
        if (auto json_opt = parse_json(request.body())) {
            client_tx_id = extract_client_transaction_id(*json_opt);
        }
    }
    AlpacaResponse alpaca_response =
        make_error_response(client_tx_id, server_tx_id, util::ErrorCode::INVALID_VALUE,
                            std::string("Cross-origin ") + what + " requests are not allowed");
    Response resp;
    resp.set_content_type("application/json");
    resp.set_status(403, "Forbidden");
    resp.set_body(alpaca_response);
    return resp;
}

}  // namespace

Response Router::handle_sync_time(const Request& request, std::uint32_t server_tx_id) {
    // Note: like the restart/shutdown management endpoints, this is
    // intentionally unauthenticated — the web UI is served on the LAN and the
    // threat model assumes a trusted network. Setting the system clock has a
    // wider blast radius than restart/shutdown (an incorrect clock can break
    // TLS validation / log ordering elsewhere on the SBC), so the epoch is
    // sanity-bounded to 2000-2100 UTC below. Deployments on untrusted networks
    // should firewall the management port.
    //
    // open-astro#676: one copy of the 2000..2100 UTC window, shared with the
    // UTCDate client-step path.
    using alpacacore::util::HostClock;
    Response response;
    response.set_content_type("application/json");

    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
    }
    // open-astro#674: an ID sent only in the JSON body is echoed on every
    // reply, not only on the cross-origin 403 (#509), with the same
    // precedence: a non-zero query-string ID wins.
    if (client_tx_id == 0 && !request.body().empty()) {
        if (auto json_opt = parse_json(request.body())) {
            client_tx_id = extract_client_transaction_id(*json_opt);
        }
    }

    // Since open-astro#291 a successful set has a second effect beyond the
    // clock: it marks the host client-stepped, which suppresses the
    // undisciplined-clock WARN on the next telescope connect. A cross-origin
    // POST could otherwise move the clock by years *and* hide the log line
    // that would have explained the resulting pointing error.
    if (auto rejected = reject_cross_origin_request(request, client_tx_id, server_tx_id, "time synchronisation")) {
        return *rejected;
    }

    // GET returns the server's current time (epoch seconds) without changing
    // anything — the web UI polls this to display a live server clock and to
    // detect drift against the browser's clock.
    if (request.method() == HttpMethod::GET) {
        const auto now_seconds = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>((*current_now_fn())().time_since_epoch()).count());
        // open-astro#670: report a clock POST would refuse to set as an error,
        // not as a Value the UI would render as a year-1970 or year-2100+ time.
        if (now_seconds < HostClock::kMinEpoch || now_seconds > HostClock::kMaxEpoch) {
            response.set_body(make_error_response(
                client_tx_id, server_tx_id, util::ErrorCode::INVALID_OPERATION,
                "Host clock is outside 2000-01-01..2100-01-01 UTC; set the time with POST /management/v1/synctime."));
            return response;
        }
        AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
        alpaca_response.value = now_seconds;
        response.set_body(alpaca_response);
        return response;
    }

    // Only POST or PUT may set the clock.
    if (request.method() != HttpMethod::POST && request.method() != HttpMethod::PUT) {
        AlpacaResponse alpaca_response =
            make_error_response(client_tx_id, server_tx_id, util::ErrorCode::INVALID_VALUE,
                                "Method not allowed. GET reads the server time; POST or PUT sets it.");
        response.set_body(alpaca_response);
        response.set_status(405, "Method Not Allowed");
        response.set_header("Allow", "GET, POST, PUT");
        return response;
    }

    // Parse the client's Unix epoch (seconds). Accept either {"Epoch": n} or
    // {"Value": n} for compatibility with the other management endpoints.
    std::int64_t epoch_seconds = -1;
    try {
        auto json_opt = parse_json(request.body());
        if (json_opt) {
            if (const auto* val = find_json_value(*json_opt, "Epoch")) {
                if (val->is_number_integer() || val->is_number_unsigned()) {
                    epoch_seconds = val->get<std::int64_t>();
                }
            } else if (const auto* val = find_json_value(*json_opt, "Value")) {
                if (val->is_number_integer() || val->is_number_unsigned()) {
                    epoch_seconds = val->get<std::int64_t>();
                }
            }
        }
    } catch (const std::exception&) {
        epoch_seconds = -1;
    }

    // Sanity range: 2000-01-01 .. 2100-01-01 UTC. Reject anything outside —
    // a bogus value (or a clock reset) would break Alpaca timestamps worse
    // than not syncing at all.
    if (epoch_seconds < HostClock::kMinEpoch || epoch_seconds > HostClock::kMaxEpoch) {
        AlpacaResponse alpaca_response =
            make_error_response(client_tx_id, server_tx_id, util::ErrorCode::INVALID_VALUE,
                                "Epoch must be a Unix timestamp in seconds between 2000-01-01 and 2100-01-01 UTC");
        response.set_body(alpaca_response);
        return response;
    }

    struct timespec ts {};
    ts.tv_sec = static_cast<time_t>(epoch_seconds);
    ts.tv_nsec = 0;
    if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
        // open-astro#292: an operator who only ever presses Sync Time would
        // otherwise never trip the latch, and the connect line would keep
        // claiming the clock is about to be corrected.
        host_clock_.mark_step_failed();
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id, util::ErrorCode::DRIVER_ERROR,
            std::string("clock_settime failed (requires CAP_SYS_TIME): ") + alpacacore::util::errno_string(errno));
        response.set_body(alpaca_response);
        return response;
    }
    // The clock is now client-set for the management readout and the
    // connect-time warning; clock_settime alone leaves STA_UNSYNC set.
    host_clock_.mark_stepped();

    AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
    alpaca_response.value = epoch_seconds;
    response.set_body(alpaca_response);
    return response;
}

util::WifiManager& Router::wifi_manager() {
    std::lock_guard<std::mutex> lock(wifi_manager_init_mutex_);
    if (!wifi_manager_) {
        // Same relative state dir as registered_devices.json — resolves to
        // /var/lib/alpacabridge/config under the packaged service.
        wifi_manager_ = std::make_unique<util::WifiManager>("config");
        wifi_manager_->apply_persisted_country();
    }
    return *wifi_manager_;
}

Response Router::handle_wifi(const Request& request, const RouteMatch& match, std::uint32_t server_tx_id) {
    // Unauthenticated like the other management endpoints (trusted-LAN threat
    // model — see handle_sync_time). Privileged operations are bounded by the
    // polkit rule (NetworkManager actions) and CAP_NET_ADMIN (country only).
    Response response;
    response.set_content_type("application/json");

    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
    }

    // CSRF guard for state-changing operations (PR #198 follow-up). Same
    // rejection the synctime endpoint uses; see reject_cross_origin_request().
    if (auto rejected = reject_cross_origin_request(request, client_tx_id, server_tx_id, "WiFi management")) {
        return *rejected;
    }

    const std::string& sub = match.method_name;
    const bool is_get = request.method() == HttpMethod::GET;
    const bool is_put = request.method() == HttpMethod::PUT || request.method() == HttpMethod::POST;
    const bool is_delete = request.method() == HttpMethod::DELETE_;

    auto body_json = [&]() -> nlohmann::json {
        auto parsed = parse_json(request.body());
        if (!parsed || !parsed->is_object()) {
            throw util::WifiError("request body must be a JSON object");
        }
        return *parsed;
    };
    auto fail = [&](int code, const std::string& msg) {
        AlpacaResponse alpaca_response = make_error_response(client_tx_id, server_tx_id, code, msg);
        response.set_body(alpaca_response);
        return response;
    };
    auto get_ssid = [](const nlohmann::json& body) {
        if (const auto* hex = find_json_value(body, "SsidHex")) {
            if (!hex->is_string()) throw util::WifiError("SsidHex (string) is required");
            return util::ssid_from_hex(hex->get<std::string>());
        }
        const auto* ssid = find_json_value(body, "Ssid");
        if (!ssid || !ssid->is_string()) throw util::WifiError("Ssid (string) or SsidHex (string) is required");
        return ssid->get<std::string>();
    };

    try {
        auto& wifi = wifi_manager();
        AlpacaResponse ok(client_tx_id, server_tx_id);

        if (sub == "status" && is_get) {
            ok.value = wifi.status();
        } else if (sub == "radio" && is_put) {
            auto body = body_json();
            const auto* v = find_json_value(body, "Enabled");
            if (!v || !v->is_boolean()) throw util::WifiError("Enabled (bool) is required");
            wifi.set_wireless_enabled(v->get<bool>());
            ok.value = nlohmann::json{{"Enabled", v->get<bool>()}};
        } else if (sub == "scan" && is_get) {
            ok.value = wifi.scan();
        } else if (sub == "profiles" && is_get) {
            ok.value = wifi.profiles();
        } else if (sub == "profiles" && is_put) {
            auto body = body_json();
            const auto ssid = get_ssid(body);
            std::string passphrase;
            if (const auto* p = find_json_value(body, "Passphrase"); p && p->is_string()) {
                passphrase = p->get<std::string>();
            }
            bool autoconnect = true;
            if (const auto* a = find_json_value(body, "Autoconnect"); a && a->is_boolean()) {
                autoconnect = a->get<bool>();
            }
            int priority = 0;
            if (const auto* pr = find_json_value(body, "Priority"); pr && pr->is_number_integer()) {
                // Range-check before narrowing: get<int>() wraps 2^32 to 0.
                if (pr->is_number_unsigned() ? !std::in_range<int>(pr->get<std::uint64_t>())
                                             : !std::in_range<int>(pr->get<std::int64_t>())) {
                    throw util::WifiError("Priority must be an int");
                }
                priority = static_cast<int>(pr->get<std::int64_t>());
            }
            ok.value = wifi.save_profile(ssid, passphrase, autoconnect, priority);
        } else if (sub.rfind("profiles/", 0) == 0 && is_delete) {
            wifi.delete_profile(sub.substr(std::string("profiles/").size()));
            ok.value = nlohmann::json{{"Deleted", true}};
        } else if (sub == "connect" && is_put) {
            auto body = body_json();
            const auto* uuid = find_json_value(body, "Uuid");
            if (!uuid || !uuid->is_string()) throw util::WifiError("Uuid (string) is required");
            // The response races the association: if the client reached us
            // over this wifi link it may lose connectivity right after this
            // returns. The web UI warns before calling.
            wifi.connect_profile(uuid->get<std::string>());
            ok.value = nlohmann::json{{"Connecting", true}};
        } else if (sub == "ap" && is_get) {
            ok.value = wifi.get_ap();
        } else if (sub == "ap" && is_put) {
            auto body = body_json();
            const auto ssid = get_ssid(body);
            std::string passphrase;
            if (const auto* p = find_json_value(body, "Passphrase"); p && p->is_string()) {
                passphrase = p->get<std::string>();
            }
            std::string band = "a";
            if (const auto* b = find_json_value(body, "Band"); b && b->is_string()) {
                band = b->get<std::string>();
            }
            std::uint32_t channel = 0;
            if (const auto* c = find_json_value(body, "Channel"); c && c->is_number_unsigned()) {
                if (!std::in_range<std::uint32_t>(c->get<std::uint64_t>())) {
                    throw util::WifiError("Channel must be a uint32");
                }
                channel = static_cast<std::uint32_t>(c->get<std::uint64_t>());
            }
            bool enabled = true;
            if (const auto* e = find_json_value(body, "Enabled"); e && e->is_boolean()) {
                enabled = e->get<bool>();
            }
            ok.value = wifi.set_ap(ssid, passphrase, band, channel, enabled);
        } else if (sub == "country" && is_get) {
            ok.value = wifi.get_country();
        } else if (sub == "country" && is_put) {
            auto body = body_json();
            const auto* cc = find_json_value(body, "Alpha2");
            if (!cc || !cc->is_string()) throw util::WifiError("Alpha2 (string) is required");
            wifi.set_country(cc->get<std::string>());
            ok.value = nlohmann::json{{"Alpha2", cc->get<std::string>()}};
        } else {
            return fail(util::ErrorCode::INVALID_VALUE, "Unknown wifi endpoint or method: " + sub);
        }

        response.set_body(ok);
        return response;
    } catch (const util::WifiError& e) {
        return fail(util::ErrorCode::DRIVER_ERROR, std::string("WiFi: ") + e.what());
    } catch (const std::exception& e) {
        return fail(util::ErrorCode::DRIVER_ERROR, std::string("WiFi (internal): ") + e.what());
    }
}

void Router::set_software_update_manager(std::unique_ptr<util::SoftwareUpdateManager> manager) {
    software_update_ = std::move(manager);
}

Response Router::handle_software_update(const Request& request, const RouteMatch& match, std::uint32_t server_tx_id) {
    // Unauthenticated like every other management endpoint (trusted-LAN
    // threat model, see handle_sync_time). What a caller can make happen is
    // bounded outside this process: the helper unit installs one fixed
    // package from the host's own signed apt sources, and the polkit rule
    // lets the service user start three units and nothing else: the update
    // unit and the two channel instances. The channel route takes no request
    // text into a unit name (a two-entry table picks it). The check is
    // read-only.
    Response response;
    response.set_content_type("application/json");

    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
    }

    // CSRF guard for the state-changing sub-endpoints (check fetches from
    // the network, install starts the helper); GET status is exempt.
    if (auto rejected = reject_cross_origin_request(request, client_tx_id, server_tx_id, "software update")) {
        return *rejected;
    }

    auto fail = [&](std::int32_t code, const std::string& msg) {
        AlpacaResponse alpaca_response = make_error_response(client_tx_id, server_tx_id, code, msg);
        response.set_body(alpaca_response);
        return response;
    };

    if (!software_update_) {
        return fail(util::ErrorCode::NOT_IMPLEMENTED, "Software update not configured");
    }

    const std::string& sub = match.method_name;
    const bool is_get = request.method() == HttpMethod::GET;
    const bool is_put = request.method() == HttpMethod::PUT || request.method() == HttpMethod::POST;

    try {
        AlpacaResponse ok(client_tx_id, server_tx_id);
        if (sub == "status" && is_get) {
            ok.value = software_update_->status();
        } else if (sub == "check" && is_put) {
            ok.value = software_update_->check();
        } else if (sub == "install" && is_put) {
            ok.value = software_update_->install();
        } else if (sub == "channel" && is_put) {
            // Body {"Channel":"stable"|"beta"}; anything else is refused
            // before the manager is reached. The unit started is picked from
            // the manager's two-entry table, never built from this text, and
            // the text is not echoed.
            const auto body = nlohmann::json::parse(request.body(), nullptr, false);
            std::optional<util::UpdateChannel> channel;
            if (body.is_object() && body.contains("Channel") && body["Channel"].is_string()) {
                channel = util::parse_channel(body["Channel"].get<std::string>());
            }
            if (!channel) {
                auto bad = fail(util::ErrorCode::INVALID_VALUE, "Channel must be \"stable\" or \"beta\"");
                bad.set_status(400, "Bad Request");
                return bad;
            }
            ok.value = software_update_->set_channel(*channel);
        } else {
            return fail(util::ErrorCode::INVALID_VALUE, "Unknown update endpoint or method: " + sub);
        }
        response.set_body(ok);
        return response;
    } catch (const util::SoftwareUpdateError& e) {
        return fail(e.alpaca_error(), e.what());
    } catch (const std::exception& e) {
        return fail(util::ErrorCode::DRIVER_ERROR, std::string("Software update (internal): ") + e.what());
    }
}

Response Router::handle_restart(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    response.set_content_type("application/json");

    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
    }

    // Issue #348: the whole management surface is unauthenticated under the
    // documented trusted-LAN threat model, which is a deliberate stance; the
    // point is that every state-changing endpoint should take that stance on
    // purpose rather than differ by accident. A POST with Content-Type:
    // text/plain is not preflighted and the handlers parse the body
    // regardless of content type, so nothing on the browser side stops a
    // drive-by request from reaching this.
    // Restarts the daemon, dropping every connected client mid-session --
    // an imaging run lost to a page the operator merely had open.
    if (auto rejected = reject_cross_origin_request(request, client_tx_id, server_tx_id, "restart")) {
        return *rejected;
    }

    if (request.method() != HttpMethod::POST && request.method() != HttpMethod::PUT) {
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::INVALID_VALUE,
            "Method not allowed. Use POST or PUT."
        );
        response.set_body(alpaca_response);
        response.set_status(405, "Method Not Allowed");
        response.set_header("Allow", "POST, PUT");
        return response;
    }

    if (restart_callback_) {
        AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
        alpaca_response.value = std::string("Restart initiated");
        response.set_body(alpaca_response);

        std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (restart_callback_) {
                restart_callback_();
            }
        }).detach();

        return response;
    }

    AlpacaResponse alpaca_response = make_error_response(
        client_tx_id, server_tx_id,
        util::ErrorCode::NOT_IMPLEMENTED,
        "Restart callback not configured"
    );
    response.set_body(alpaca_response);
    return response;
}

namespace {
// The subject of the WARN the two helpers below log: "Persisted skywatcher
// telescope 1". Kept in one place so the two lines read alike in the log.
std::string persisted_device_subject(const std::string& vendor, const std::string& device_type, int device_number) {
    return "Persisted " + vendor + " " + device_type + " " + std::to_string(device_number);
}

// open-astro#664: the catalog consult in register_device_from_config() below.
// DeviceCatalog::find_schema is private, so the router can only ask
// describe() for the DescriptorView of a key.
std::optional<alpacacore::catalog::DescriptorView> find_descriptor(const alpacacore::catalog::DeviceCatalog& catalog,
                                                                   const alpacacore::catalog::DeviceKey& key) {
    for (const auto& view : catalog.describe()) {
        if (view.key == key) return view;
    }
    return std::nullopt;
}

// The vendor as the deleted arm texts spelled it: Schema::vendor_label when the
// descriptor sets one ("Player One"), else the first word of its display name
// ("Astroasis Oasis Focuser" -> "Astroasis").
std::string vendor_label(const alpacacore::catalog::DescriptorView& view) {
    if (!view.vendor_label.empty()) return std::string(view.vendor_label);
    const std::string name(view.display_name);
    const auto space = name.find(' ');
    return space == std::string::npos ? name : name.substr(0, space);
}

// #508 item 2: connectionType is compared in lower case everywhere, so the
// config is folded once on the way into registration and into the stored entry
// (catalog::fold_connection_type_value). An unknown value stays unknown.
nlohmann::json with_folded_connection_type(const nlohmann::json& config) {
    if (!config.is_object()) return config;
    auto it = config.find("connectionType");
    if (it == config.end() || !it->is_string()) return config;
    nlohmann::json folded = config;
    folded["connectionType"] = alpacacore::catalog::fold_connection_type_value(it->get<std::string>());
    return folded;
}
}  // namespace

bool Router::register_device_from_config(const nlohmann::json& raw_config, std::string& error_message,
                                         ConfigSource source, nlohmann::json* learned_config) {
    const nlohmann::json config = with_folded_connection_type(raw_config);
    std::string device_type_str = catalog_json::get_string(config, "deviceType");
    std::string vendor = catalog_json::get_string(config, "vendor");
    int device_number = catalog_json::get_int(config, "deviceNumber", -1);

    if (device_type_str.empty() || vendor.empty() || device_number < 0) {
        error_message = "Missing required fields: deviceType, vendor, and deviceNumber";
        return false;
    }

    auto& registry = alpacacore::management::DeviceRegistry::instance();

    // open-astro#664: the catalog is consulted before the not-supported answer below. A
    // device_type_str the catalog doesn't recognise (string_to_device_type()
    // throws) just skips the consult -- the final "not yet
    // supported" message still answers, unchanged. Only that lookup is
    // guarded: an AlpacaException out of config_from_json below must
    // propagate like a catalog_json::get_string() failure does today, not be swallowed here.
    std::optional<alpacacore::DeviceType> device_type_key;
    try {
        device_type_key = string_to_device_type(device_type_str);
    } catch (const std::exception& ex) {
        // Unknown device_type_str: answer not supported.
        util::log_debug("register_device_from_config: device type \"" + util::escape_for_log(device_type_str) +
                        "\" is not catalog-recognized (" + util::escape_for_log(ex.what()) +
                        "); the device is answered as not supported");
    }
    if (device_type_key) {
        const alpacacore::catalog::DeviceKey key{vendor, *device_type_key};
        if (auto view = find_descriptor(catalog_, key)) {
            nlohmann::json prepared =
                with_object_record_entries(without_unknown_alignment_mode(config, view->fields), view->fields);
            if (view->forget_dropped_selectors && source == ConfigSource::Persisted) {
                for (const std::string& dropped : drop_unreadable_selectors(prepared, view->fields)) {
                    util::log_warning(persisted_device_subject(vendor, device_type_str, device_number) +
                                      " has invalid '" + dropped + "' (expected an integer); dropping it");
                    if (learned_config != nullptr) (*learned_config)[dropped] = nullptr;
                }
            }
            const alpacacore::catalog::DeviceConfig typed = catalog_json::config_from_json(prepared, view->fields);
            const auto result =
                catalog_.normalize(key, typed,
                                   source == ConfigSource::Api ? alpacacore::catalog::Source::Api
                                                               : alpacacore::catalog::Source::Persisted);
            if (source == ConfigSource::Api) {
                if (result.rejection) {
                    error_message = *result.rejection;
                    return false;
                }
            } else {
                // ALP-271: normalize() has already substituted a usable value (the field's default, or
                // unset), so the device is not refusing to connect over this.
                for (const auto& warning : result.warnings) {
                    util::log_warning(persisted_device_subject(vendor, device_type_str, device_number) +
                                      " config normalized: " + warning +
                                      ". The saved value is not used: the field falls back to its default, or "
                                      "stays unset if it has none. Registered so it stays listed and editable in "
                                      "the web UI.");
                }
                if (learned_config != nullptr) {
                    // #508 item 5: a saved site coordinate that the descriptor declares and
                    // normalize() dropped as out of range is forgotten in the stored entry
                    // too, so the entry holds what the driver uses and the next save does not
                    // write it back. A coordinate the descriptor does not declare is never in
                    // `normalized`, so it is left alone.
                    const auto normalized = catalog_json::json_from_config(result.config);
                    for (const char* key : {"siteLatitude", "siteLongitude"}) {
                        const auto original = config.find(key);
                        const bool declared =
                            std::any_of(view->fields.begin(), view->fields.end(),
                                        [key](const auto& field) { return std::string_view{field.key} == key; });
                        if (declared && original != config.end() && original->is_number() &&
                            normalized.find(key) == normalized.end()) {
                            (*learned_config)[key] = nullptr;
                        }
                    }
                }
                if (view->forget_dropped_selectors && learned_config != nullptr) {
                    const auto normalized = catalog_json::json_from_config(result.config);
                    for (const auto& field : view->fields) {
                        if (field.role != alpacacore::catalog::Role::EnumerationIndex &&
                            field.role != alpacacore::catalog::Role::DeviceId) {
                            continue;
                        }
                        const auto original = config.find(field.key);
                        if (original != config.end() && !original->is_null() &&
                            normalized.find(field.key) == normalized.end()) {
                            (*learned_config)[field.key] = nullptr;
                        }
                    }
                }
            }
            if (!view->available) {
                error_message = vendor_label(*view) + " support not enabled. Rebuild with -D" +
                                std::string(view->build_option) + "=ON";
                return false;
            }
            // A factory with a bind hook sees the other stored entries of its key
            // (copied under the mutex, which is released before the hook runs: it may
            // enumerate the SDK) and may hand learned config back. Any other factory
            // gets no peers and learns nothing.
            std::vector<alpacacore::catalog::PeerEntry> peers;
            if (catalog_.has_bind(key)) {
                const std::string wanted_type = to_lower_copy(device_type_str);
                std::lock_guard<std::mutex> lock(persisted_devices_mutex_);
                for (const auto& other : persisted_devices_) {
                    const auto other_key = persisted_key(other);
                    if (!other_key || other_key->vendor != vendor || other_key->device_type != wanted_type) {
                        continue;
                    }
                    const std::string subject =
                        persisted_device_subject(vendor, device_type_str, other_key->device_number);
                    try {
                        nlohmann::json neighbour = with_object_record_entries(other, view->fields);
                        if (view->forget_dropped_selectors) drop_unreadable_selectors(neighbour, view->fields);
                        // A hand-edited neighbour with a wrong-typed field stays a peer without
                        // that field, so its other identity fields (a uniqueId) are still claimed.
                        for (const auto& f : view->fields) {
                            const auto it = neighbour.find(f.key);
                            if (it == neighbour.end()) continue;
                            nlohmann::json single = nlohmann::json::object();
                            single[f.key] = *it;
                            try {
                                (void)catalog_json::config_from_json(single, view->fields);
                            } catch (const std::exception& ex) {
                                util::log_debug("register_device_from_config: ignoring field " + std::string(f.key) +
                                                " of a stored " + subject + " as a peer (" +
                                                util::escape_for_log(ex.what()) + ")");
                                neighbour.erase(f.key);
                            }
                        }
                        peers.push_back(
                            {other_key->device_number, catalog_json::config_from_json(neighbour, view->fields)});
                    } catch (const std::exception& ex) {
                        util::log_debug("register_device_from_config: ignoring a stored " + subject + " as a peer (" +
                                        util::escape_for_log(ex.what()) + ")");
                    }
                }
            }
            auto bound = catalog_.bind(key, result.config, device_number,
                                       {source == ConfigSource::Api ? alpacacore::catalog::Source::Api
                                                                    : alpacacore::catalog::Source::Persisted,
                                        peers});
            if (bound.rejection) {
                error_message = *bound.rejection;
                return false;
            }
            if (learned_config != nullptr) {
                const nlohmann::json learned = catalog_json::json_from_config(bound.learned);
                for (const auto& [field, value] : learned.items()) {
                    (*learned_config)[field] = value;
                }
                for (const auto& field : bound.erased) {
                    (*learned_config)[field] = nullptr;
                }
            }
            // Decision 0018: the configured device's stored ID is its UniqueID.
            std::string unique_id = stored_unique_id(config);
            if (unique_id.empty()) {
                unique_id = alpacacore::util::generate_unique_id();
                if (learned_config != nullptr) (*learned_config)["uniqueId"] = unique_id;
            }
            bound.driver->assign_unique_id(unique_id);
            if (registry.register_device(std::shared_ptr<alpacacore::AlpacaDriver>(std::move(bound.driver)))) {
                util::log_info("Registered " + vendor_label(*view) + " " + device_type_str);
                return true;
            }
            error_message = "Failed to register device. Device may already exist.";
            return false;
        }
    }

    error_message = "Vendor/device type combination not yet supported: " + vendor + "/" + device_type_str;
    return false;
}

namespace {
// open-astro#664: the catalog-driven half of Router::sanitize_device_config()
// below. Every declared, non-secret, present-and-non-null field survives;
// undeclared keys drop. A RecordList recurses element-wise so a nested
// secret (or undeclared nested key) drops too; type checking a value is
// DeviceCatalog::normalize's job, not this function's, so a RecordList field
// holding something other than an array is copied through unexamined.
nlohmann::json sanitize_fields_json(const nlohmann::json& config,
                                    std::span<const alpacacore::catalog::FieldRef> fields) {
    nlohmann::json out = nlohmann::json::object();
    for (const auto& f : fields) {
        if (f.role == alpacacore::catalog::Role::Secret) continue;
        const auto it = config.find(f.key);
        if (it == config.end() || it->is_null()) continue;
        if (f.kind == alpacacore::catalog::FieldRef::Kind::RecordList && it->is_array()) {
            nlohmann::json records = nlohmann::json::array();
            for (const auto& elem : *it) {
                records.push_back(elem.is_object() ? sanitize_fields_json(elem, f.record_fields) : elem);
            }
            out[f.key] = std::move(records);
        } else {
            out[f.key] = *it;
        }
    }
    return out;
}
}  // namespace

nlohmann::json Router::sanitize_device_config(const nlohmann::json& raw_config) const {
    const nlohmann::json config = with_folded_connection_type(raw_config);
    nlohmann::json sanitized = nlohmann::json::object();

    auto copy_if_present = [&](const char* key) {
        if (catalog_json::has_value(config, key)) {
            sanitized[key] = config.at(key);
        }
    };

    copy_if_present("vendor");
    copy_if_present("deviceType");
    copy_if_present("deviceNumber");

    std::string vendor = catalog_json::get_string(config, "vendor");
    std::string device_type = catalog_json::get_string(config, "deviceType");

    // open-astro#664: a vendor/type is sanitized from its catalog descriptor's own
    // field list (every vendor is one now). The vendor-agnostic keys at the end are
    // kept for any config, as they were through the deleted arms.
    try {
        const alpacacore::catalog::DeviceKey key{vendor, string_to_device_type(device_type)};
        if (auto view = find_descriptor(catalog_, key)) {
            const auto extra = sanitize_fields_json(without_unknown_alignment_mode(config, view->fields), view->fields);
            for (const auto& [k, v] : extra.items()) {
                sanitized[k] = v;
            }
        }
    } catch (const std::exception& ex) {
        // Unknown device_type: only the vendor-agnostic keys below are kept.
        util::log_debug("sanitize_device_config: device type \"" + util::escape_for_log(device_type) +
                        "\" is not catalog-recognized (" + util::escape_for_log(ex.what()) +
                        "); only the vendor-agnostic keys are kept");
    }

    copy_if_present("responseTimeoutMs");
    copy_if_present("apertureDiameter");
    copy_if_present("focalLength");
    copy_if_present("siteLatitude");
    copy_if_present("siteLongitude");
    copy_if_present("siteElevation");
    copy_if_present("learnSiteFromClient");  // open-astro#444
    copy_if_present("syncTimeOnConnect");
    // Decision 0018: the server-owned stored ID, for every device type.
    if (const std::string unique_id = stored_unique_id(config); !unique_id.empty()) {
        sanitized["uniqueId"] = unique_id;
    }

    return sanitized;
}

void Router::persist_client_site(const alpacacore::AlpacaDriver& device, const char* key, double value) {
    // A NaN passes every driver's range check (both comparisons are false)
    // and parses, so the setter takes it for the session; nlohmann dumps a
    // non-finite double as null, which the next start reads as "absent" and
    // the driver then refuses to connect. The surveyed value stays on disk.
    // The ioptron schema's normalize applies the same rule on the config path.
    if (!std::isfinite(value)) {
        return;
    }
    if (device.get_device_type() != alpacacore::DeviceType::Telescope) {
        return;
    }
    const int device_number = device.get_device_number();
    bool changed = false;
    bool declined = false;
    std::string vendor{};
    {
        std::lock_guard<std::mutex> lock(persisted_devices_mutex_);
        for (auto& entry : persisted_devices_) {
            // Typed reads (persisted_key), never value(): a malformed
            // neighbour in the list is simply not a match.
            const auto pkey = persisted_key(entry);
            if (!pkey || pkey->device_type != "telescope" || pkey->device_number != device_number) {
                continue;
            }
            vendor = pkey->vendor;
            // The opt-out: an operator with a surveyed pier position does not
            // want a phone's GPS, good to perhaps 5 m, overwriting it. Default
            // on, because the value the mount is using RIGHT NOW is the one
            // the client set, and a restart should start from the same site
            // the last session ended on rather than an older one.
            // Typed, not value(): a hand-edited file can carry "false" or 0
            // here, and nlohmann's value() throws on that, which would turn
            // every site PUT into an error. Only a boolean false is the
            // opt-out; the API path refuses any other type at registration.
            const auto flag = entry.find("learnSiteFromClient");
            if (flag != entry.end() && flag->is_boolean() && !flag->get<bool>()) {
                declined = true;
                break;
            }
            // Unchanged: no file write on a client that re-sends its site on
            // every connect (most do).
            const auto current = entry.find(key);
            if (current != entry.end() && current->is_number() && current->get<double>() == value) {
                break;
            }
            entry[key] = value;
            changed = true;
            break;
        }
    }
    if (declined) {
        util::log_debug("Telescope " + std::to_string(device_number) + ": client " + key +
                        " not persisted (learnSiteFromClient is false)");
        return;
    }
    if (!changed) {
        return;  // no persisted entry (a device registered for this process only), or same value
    }
    // File I/O on a PUT setter, not on a getter or DeviceState, so a polling client never waits on it
    // (the lock-free get_connected() rule in alpaca-http-conformance.instructions.md is unaffected);
    // it runs once per changed value.
    std::ostringstream msg;
    msg << "Telescope " << device_number << " (" << vendor << "): client " << key << " = " << std::fixed
        << std::setprecision(6) << value;
    if (save_persisted_devices()) {
        util::log_info(msg.str() + " persisted to config/registered_devices.json");
    } else {
        // The entry in memory holds the value (the driver is using it), so
        // the same value re-sent takes the unchanged skip; say plainly that
        // the file does not have it rather than claiming it was persisted.
        util::log_warning(msg.str() +
                          " is in use for this session but could NOT be written to "
                          "config/registered_devices.json (see the error above)");
    }
}

void Router::add_or_replace_persisted_device(const nlohmann::json& config) {
    if (!catalog_json::has_value(config, "deviceType") || !catalog_json::has_value(config, "vendor") ||
        !catalog_json::has_value(config, "deviceNumber")) {
        return;
    }

    std::lock_guard<std::mutex> lock(persisted_devices_mutex_);
    const std::string vendor = catalog_json::get_string(config, "vendor");
    const std::string device_type = to_lower_copy(catalog_json::get_string(config, "deviceType"));
    const int device_number = catalog_json::get_int(config, "deviceNumber", -1);
    for (auto& existing : persisted_devices_) {
        const auto key = persisted_key(existing);
        if (key && key->vendor == vendor && key->device_type == device_type && key->device_number == device_number) {
            existing = config;
            return;
        }
    }

    persisted_devices_.push_back(config);
}

bool Router::remove_persisted_device(const std::string& vendor, const std::string& device_type, int device_number) {
    if (device_type.empty() || device_number < 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(persisted_devices_mutex_);
    std::size_t before = persisted_devices_.size();
    persisted_devices_.erase(std::remove_if(persisted_devices_.begin(), persisted_devices_.end(),
                                            [&](const nlohmann::json& entry) {
                                                const auto key = persisted_key(entry);
                                                if (!key || key->device_type != to_lower_copy(device_type) ||
                                                    key->device_number != device_number) {
                                                    return false;
                                                }
                                                return vendor.empty() || key->vendor == vendor;
                                            }),
                             persisted_devices_.end());
    return persisted_devices_.size() < before;
}

bool Router::save_persisted_devices() const {
    try {
        if (kPersistedDevicesFile.has_parent_path()) {
            std::filesystem::create_directories(kPersistedDevicesFile.parent_path());
        }

        // One writer at a time, and never in place: site PUTs from two
        // clients run on two workers (#444), and two truncate-and-write
        // passes on the same file end as one dump's head plus the other's
        // tail, which the next start cannot parse and so loads NO devices.
        // Writing a sibling temp file and renaming it over the real one
        // makes each save atomic for a concurrent reader: the file is always
        // a complete dump. (Atomic against readers, not against power loss:
        // without fsync on the temp file and its directory, a crash right
        // after the rename can still leave a zero-length file on ext4.)
        //
        // The file lock is taken BEFORE the snapshot (lock order: file, then
        // list, the same everywhere). Snapshotting first let worker A copy
        // the list, lose the CPU, and rename its older copy over the newer
        // dump worker B had just written: memory right, disk one revision
        // stale until the next changed PUT, lost across a restart.
        std::lock_guard<std::mutex> file_lock(persisted_file_mutex_);
        nlohmann::json payload;
        {
            std::lock_guard<std::mutex> lock(persisted_devices_mutex_);
            payload = persisted_devices_;
        }
        const std::filesystem::path temp = kPersistedDevicesFile.string() + ".tmp";
        try {
            {
                std::ofstream out(temp, std::ios::trunc);
                if (!out) {
                    throw std::runtime_error("Unable to open " + temp.string() + " for writing");
                }
                out << payload.dump(4);
                out.flush();
                if (!out) {
                    throw std::runtime_error("Unable to write " + temp.string());
                }
            }
            // The rename swaps the inode, so the mode the package's postinst
            // (or an operator) set on the real file would otherwise be
            // replaced by this process's umask. Carry it over.
            std::error_code ec;
            const auto existing = std::filesystem::status(kPersistedDevicesFile, ec);
            if (!ec && std::filesystem::is_regular_file(existing)) {
                std::filesystem::permissions(temp, existing.permissions(), ec);
            }
            std::filesystem::rename(temp, kPersistedDevicesFile);
        } catch (...) {
            // Never leave a half-written .tmp beside the real file.
            std::error_code ec;
            std::filesystem::remove(temp, ec);
            throw;
        }
        return true;
    } catch (const std::exception& e) {
        util::log_error("Failed to persist registered devices: " + std::string(e.what()));
        return false;
    }
}

void Router::load_persisted_devices() {
    // Read the file and populate persisted_devices_ under the mutex; register
    // the loaded devices afterwards so the lock is never held across
    // driver-registry calls (which may block on hardware).
    nlohmann::json payload = nlohmann::json::array();
    bool assigned_unique_ids = false;
    std::set<std::string> live_unique_ids;
    // Entries this start gives their first GUID (the upgrade, or a hand-edited
    // file): only their drivers adopt state saved under a driver-built ID.
    std::set<std::string> first_unique_ids;
    {
        std::lock_guard<std::mutex> lock(persisted_devices_mutex_);
        if (persisted_devices_loaded_) {
            return;
        }

        persisted_devices_loaded_ = true;

        try {
            if (!std::filesystem::exists(kPersistedDevicesFile)) {
                persisted_devices_.clear();
                return;
            }

            std::ifstream in(kPersistedDevicesFile);
            if (!in) {
                throw std::runtime_error("Unable to open " + kPersistedDevicesFile.string() + " for reading");
            }

            in >> payload;

            if (!payload.is_array()) {
                throw std::runtime_error("Persisted device file must contain a JSON array");
            }

            persisted_devices_.clear();
            // Decision 0018: every stored entry without a keepable ID gets a GUID once (the
            // upgrade, or a hand-edited file), and a duplicated one is replaced in the later entry.
            std::set<std::string> taken;
            for (auto& entry : payload) {
                nlohmann::json sanitized = sanitize_device_config(entry);
                const std::string unique_id = choose_unique_id(sanitized, taken);
                if (stored_unique_id(sanitized).empty()) first_unique_ids.insert(unique_id);
                if (stored_unique_id(sanitized) != unique_id) {
                    assigned_unique_ids = true;
                    if (entry.is_object()) entry["uniqueId"] = unique_id;
                    sanitized["uniqueId"] = unique_id;
                }
                taken.insert(unique_id);
                persisted_devices_.push_back(std::move(sanitized));
            }
            live_unique_ids = std::move(taken);
        } catch (const std::exception& e) {
            util::log_error("Failed to load registered devices: " + std::string(e.what()));
            return;
        }
    }
    if (assigned_unique_ids) {
        util::log_info("Assigned a persisted UniqueID to the stored devices that had none (decision 0018)");
        if (!save_persisted_devices() && !first_unique_ids.empty()) {
            // The next start would mint other GUIDs and prune whatever moved onto
            // these, so nothing moves until the file can keep them.
            util::log_warning("Could not store the new UniqueIDs; saved rotator offsets stay under their old IDs");
            first_unique_ids.clear();
        }
    }
    // A removed rotator's offset goes here, not at removal: the web UI's edit
    // removes and re-adds the entry with the same stored ID (decisions 0008, 0018).
    if (const std::size_t pruned = alpacacore::util::RotatorSyncOffsetStore::prune_unknown_unique_ids(live_unique_ids);
        pruned > 0) {
        util::log_info("Removed " + std::to_string(pruned) + " sync offset(s) of rotators no longer configured");
    }

    for (const auto& entry : payload) {
        std::string error_message;
        try {
            nlohmann::json learned_config = nlohmann::json::object();
            if (!register_device_from_config(entry, error_message, ConfigSource::Persisted, &learned_config)) {
                // The message can quote the saved vendor / deviceType; the file is
                // outside the server's control, so escape it (no cut: the whole
                // message is the diagnosis).
                util::log_warning("Skipping persisted device: " +
                                  util::escape_for_log(error_message, error_message.size()));
            } else if (!learned_config.empty()) {
                // #914: what the registration learned about the device (a ZWO
                // camera's serial and model name) goes back into the
                // stored entry, so the next start binds by it.
                const auto learned_key = persisted_key(entry);
                bool stored = false;
                if (learned_key.has_value()) {
                    const PersistedKey& wanted = *learned_key;
                    std::lock_guard<std::mutex> lock(persisted_devices_mutex_);
                    for (auto& saved : persisted_devices_) {
                        const auto key = persisted_key(saved);
                        if (!key.has_value()) {
                            continue;
                        }
                        const PersistedKey& have = *key;
                        if (have.vendor == wanted.vendor && have.device_type == wanted.device_type &&
                            have.device_number == wanted.device_number) {
                            for (const auto& [field, value] : learned_config.items()) {
                                if (value.is_null()) {
                                    saved.erase(field);
                                } else {
                                    saved[field] = value;
                                }
                            }
                            stored = true;
                            break;
                        }
                    }
                }
                if (stored) {
                    save_persisted_devices();
                }
            }
        } catch (const std::exception& e) {
            util::log_error("Failed to load persisted device: " + std::string(e.what()));
        }
    }

    // Decision 0018: state a driver saved under its own ID moves to the GUID
    // here, once, for the devices that held their number before this start. A
    // device configured later keeps a GUID from the start and never adopts, so
    // it cannot inherit what a removed device saved under the same number.
    if (!first_unique_ids.empty()) {
        for (const auto& device : alpacacore::management::DeviceRegistry::instance().get_all_devices()) {
            if (first_unique_ids.count(device->get_unique_id()) == 0) {
                continue;
            }
            try {
                device->adopt_legacy_state();
            } catch (const std::exception& e) {
                util::log_warning("Could not move the saved state of " + device->get_unique_id() + ": " + e.what());
            }
        }
    }
}

} // namespace alpacahttp
