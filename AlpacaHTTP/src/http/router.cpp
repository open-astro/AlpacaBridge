// AlpacaHTTP
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply with all SSPL v1 requirements.

#include <alpacahttp/router.h>
#include <alpacahttp/version.h>
#include <alpacahttp/json_utils.h>
#include <alpacahttp/util/error_mapping.h>
#include <alpacahttp/util/logging_adapter.h>
#include <alpacacore/device_registry.h>
#include <alpacacore/camera_driver.h>
#include <alpacacore/filterwheel_driver.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/alpaca_defs.h>
#include <alpacacore/util/logging.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <regex>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <string_view>
#include <cmath>
#include <ctime>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <chrono>
#include <iomanip>
#include <fstream>
#include <filesystem>
#include <thread>
#include <optional>
#include <array>
#include <variant>
#include <unordered_set>
#include <limits>
#ifdef ALPACACORE_ENABLE_IOPTRON
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_SYNSCAN
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_ZWO
#include <alpacacore/vendor/zwo/zwo_camera_driver.h>
#include <alpacacore/vendor/zwo/zwo_filterwheel_driver.h>
#include <alpacacore/vendor/zwo/zwo_focuser_driver.h>
#include <alpacacore/vendor/zwo/zwo_telescope_driver.h>
#include <alpacacore/vendor/zwo/zwo_rotator_driver.h>
#include <alpacacore/vendor/zwo/zwo_switch_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_QHY
#include <alpacacore/vendor/qhy/qhy_camera_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_WEEWX
#include <alpacacore/vendor/weewx/weewx_observingconditions_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_GEMINI
#include <alpacacore/vendor/gemini/gemini_focuser_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_SVBONY
#include <alpacacore/vendor/svbony/svbony_camera_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_CELESTRON
#include <alpacacore/vendor/celestron/celestron_telescope_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_BISQUE
#include <alpacacore/vendor/bisque/bisque_telescope_driver.h>
#endif
#ifdef ALPACACORE_ENABLE_TOUPTEK
#include <alpacacore/vendor/touptek/touptek_camera_driver.h>
#endif

namespace {

using alpacacore::logging::LogLevel;

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

std::string escape_yaml_string(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        if (ch == '\\' || ch == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

bool update_location_in_config(const std::string& config_path,
                               const std::string& location,
                               std::string& error_message) {
    if (config_path.empty()) {
        error_message = "Config path not set";
        return false;
    }

    std::ifstream input(config_path);
    if (!input.is_open()) {
        std::ofstream output(config_path, std::ios::trunc);
        if (!output.is_open()) {
            error_message = "Unable to open config file for writing";
            return false;
        }
        output << "server:\n  location: \"" << escape_yaml_string(location) << "\"\n";
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

    auto strip_comment = [](const std::string& text) {
        auto pos = text.find('#');
        if (pos == std::string::npos) {
            return text;
        }
        return text.substr(0, pos);
    };

    bool in_server_section = false;
    bool server_section_found = false;
    bool location_written = false;
    std::size_t server_indent = 0;
    std::vector<std::string> output;
    output.reserve(lines.size() + 2);

    for (const auto& current_line : lines) {
        std::string stripped_comment = strip_comment(current_line);
        std::string trimmed = trim_copy(stripped_comment);
        std::size_t indent = leading_spaces(current_line);

        if (indent == 0) {
            if (in_server_section && !location_written) {
                output.push_back(std::string(server_indent + 2, ' ') +
                                 "location: \"" + escape_yaml_string(location) + "\"");
                location_written = true;
            }
            in_server_section = false;
        }

        if (indent == 0 && trimmed == "server:") {
            in_server_section = true;
            server_section_found = true;
            server_indent = indent;
            output.push_back(current_line);
            continue;
        }

        if (in_server_section && indent > server_indent && !trimmed.empty()) {
            auto delimiter = trimmed.find(':');
            if (delimiter != std::string::npos) {
                std::string key = trim_copy(trimmed.substr(0, delimiter));
                if (key == "location") {
                    output.push_back(std::string(indent, ' ') +
                                     "location: \"" + escape_yaml_string(location) + "\"");
                    location_written = true;
                    continue;
                }
            }
        }

        output.push_back(current_line);
    }

    if (in_server_section && !location_written) {
        output.push_back(std::string(server_indent + 2, ' ') +
                         "location: \"" + escape_yaml_string(location) + "\"");
        location_written = true;
    }

    if (!server_section_found) {
        if (!output.empty() && !output.back().empty()) {
            output.push_back("");
        }
        output.push_back("server:");
        output.push_back("  location: \"" + escape_yaml_string(location) + "\"");
    }

    std::ofstream output_file(config_path, std::ios::trunc);
    if (!output_file.is_open()) {
        error_message = "Unable to open config file for writing";
        return false;
    }
    for (std::size_t i = 0; i < output.size(); ++i) {
        output_file << output[i];
        if (i + 1 < output.size()) {
            output_file << '\n';
        }
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

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string url_decode(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            std::string hex = value.substr(i + 1, 2);
            char decoded = static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
            result.push_back(decoded);
            i += 2;
        } else if (value[i] == '+') {
            result.push_back(' ');
        } else {
            result.push_back(value[i]);
        }
    }
    return result;
}

bool is_lowercase_ascii(const std::string& value) {
    for (unsigned char c : value) {
        if (c >= 'A' && c <= 'Z') {
            return false;
        }
    }
    return true;
}

std::uint32_t parse_client_transaction_id(const std::string& value) {
    if (value.empty()) {
        return 0;
    }
    try {
        std::size_t pos = 0;
        long long parsed = std::stoll(value, &pos);
        if (pos != value.size()) {
            return 0;
        }
        if (parsed < 0 || parsed > static_cast<long long>(std::numeric_limits<std::uint32_t>::max())) {
            return 0;
        }
        return static_cast<std::uint32_t>(parsed);
    } catch (...) {
        return 0;
    }
}

double parse_double_value(const std::string& raw, const std::string& param_name) {
    try {
        std::size_t pos = 0;
        double value = std::stod(raw, &pos);
        if (pos != raw.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return value;
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid value for parameter: " + param_name);
    }
}

int parse_int_value(const std::string& raw, const std::string& param_name) {
    try {
        std::size_t pos = 0;
        long long value = std::stoll(raw, &pos);
        if (pos != raw.size()) {
            throw std::invalid_argument("trailing characters");
        }
        if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
            throw std::out_of_range("int range");
        }
        return static_cast<int>(value);
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid value for parameter: " + param_name);
    }
}

bool parse_bool_value(const std::string& raw, const std::string& param_name) {
    std::string lowered = to_lower_copy(raw);
    if (lowered == "true" || lowered == "1") {
        return true;
    }
    if (lowered == "false" || lowered == "0") {
        return false;
    }
    throw std::runtime_error("Invalid value for parameter: " + param_name);
}

const std::unordered_set<std::string> kCommonMethods = {
    "action",
    "commandblind",
    "commandbool",
    "commandstring",
    "connect",
    "connected",
    "connecting",
    "description",
    "devicestate",
    "disconnect",
    "driverinfo",
    "driverversion",
    "interfaceversion",
    "name",
    "supportedactions",
};

const std::unordered_set<std::string> kTelescopeMethods = {
    "abortslew",
    "alignmentmode",
    "altitude",
    "aperturearea",
    "aperturediameter",
    "athome",
    "atpark",
    "axisrates",
    "azimuth",
    "canfindhome",
    "canmoveaxis",
    "canpark",
    "canpulseguide",
    "cansetdeclinationrate",
    "cansetguiderates",
    "cansetpark",
    "cansetpierside",
    "cansetrightascensionrate",
    "cansettracking",
    "canslew",
    "canslewaltaz",
    "canslewaltazasync",
    "canslewasync",
    "cansync",
    "cansyncaltaz",
    "canunpark",
    "declination",
    "declinationrate",
    "destinationsideofpier",
    "doesrefraction",
    "equatorialsystem",
    "findhome",
    "focallength",
    "guideratedeclination",
    "guideraterightascension",
    "ispulseguiding",
    "moveaxis",
    "park",
    "pulseguide",
    "rightascension",
    "rightascensionrate",
    "setpark",
    "sideofpier",
    "siderealtime",
    "siteelevation",
    "sitelatitude",
    "sitelongitude",
    "slewing",
    "slewsettletime",
    "slewtoaltaz",
    "slewtoaltazasync",
    "slewtocoordinates",
    "slewtocoordinatesasync",
    "slewtotarget",
    "slewtotargetasync",
    "synctoaltaz",
    "synctocoordinates",
    "synctotarget",
    "targetdeclination",
    "targetrightascension",
    "tracking",
    "trackingrate",
    "trackingrates",
    "unpark",
    "utcdate",
};

const std::unordered_set<std::string> kCameraMethods = {
    "abortexposure",
    "bayeroffsetx",
    "bayeroffsety",
    "binx",
    "biny",
    "camerastate",
    "cameraxsize",
    "cameraysize",
    "canabortexposure",
    "canasymmetricbin",
    "canfastreadout",
    "cangetcoolerpower",
    "canpulseguide",
    "cansetccdtemperature",
    "canstopexposure",
    "ccdtemperature",
    "cooleron",
    "coolerpower",
    "electronsperadu",
    "exposuremax",
    "exposuremin",
    "exposureresolution",
    "fastreadout",
    "fullwellcapacity",
    "gain",
    "gainmax",
    "gainmin",
    "gains",
    "hasshutter",
    "heatsinktemperature",
    "imagearray",
    "imagearrayvariant",
    "imageready",
    "ispulseguiding",
    "lastexposureduration",
    "lastexposurestarttime",
    "maxadu",
    "maxbinx",
    "maxbiny",
    "numx",
    "numy",
    "offset",
    "offsetmax",
    "offsetmin",
    "offsets",
    "percentcompleted",
    "pixelsizex",
    "pixelsizey",
    "pulseguide",
    "readoutmode",
    "readoutmodes",
    "sensorname",
    "sensortype",
    "setccdtemperature",
    "startexposure",
    "startx",
    "starty",
    "stopexposure",
    "subexposureduration",
};

const std::unordered_set<std::string> kFilterWheelMethods = {
    "focusoffsets",
    "names",
    "position",
};

const std::unordered_set<std::string> kFocuserMethods = {
    "absolute",
    "halt",
    "ismoving",
    "maxincrement",
    "maxstep",
    "move",
    "position",
    "stepsize",
    "tempcomp",
    "tempcompavailable",
    "temperature",
};

const std::unordered_set<std::string> kRotatorMethods = {
    "canreverse",
    "halt",
    "ismoving",
    "mechanicalposition",
    "move",
    "moveabsolute",
    "movemechanical",
    "position",
    "reverse",
    "stepsize",
    "sync",
    "targetposition",
};

const std::unordered_set<std::string> kDomeMethods = {
    "abortslew",
    "altitude",
    "athome",
    "atpark",
    "azimuth",
    "canfindhome",
    "canpark",
    "cansetaltitude",
    "cansetazimuth",
    "cansetpark",
    "cansetshutter",
    "canslave",
    "canslew",
    "cansyncazimuth",
    "closeshutter",
    "findhome",
    "openshutter",
    "park",
    "setpark",
    "shutterstatus",
    "slaved",
    "slewing",
    "slewtoaltitude",
    "slewtoazimuth",
    "synctoazimuth",
};

const std::unordered_set<std::string> kShutterMethods = {
    "close",
    "open",
    "shutterstate",
};

const std::unordered_set<std::string> kSwitchMethods = {
    "cancelasync",
    "canasync",
    "canwrite",
    "getswitch",
    "getswitchdescription",
    "getswitchname",
    "getswitchvalue",
    "maxswitch",
    "maxswitchvalue",
    "minswitchvalue",
    "setasync",
    "setasyncvalue",
    "setswitch",
    "setswitchname",
    "setswitchvalue",
    "statechangecomplete",
    "switchstep",
};

const std::unordered_set<std::string> kCoverCalibratorMethods = {
    "brightness",
    "calibratorchanging",
    "calibratoroff",
    "calibratoron",
    "calibratorstate",
    "closecover",
    "covermoving",
    "coverstate",
    "haltcover",
    "maxbrightness",
    "opencover",
};

const std::unordered_set<std::string> kObservingConditionsMethods = {
    "averageperiod",
    "cloudcover",
    "dewpoint",
    "humidity",
    "pressure",
    "rainrate",
    "refresh",
    "seeing",
    "sensordescription",
    "skybrightness",
    "skyquality",
    "skytemperature",
    "starfwhm",
    "temperature",
    "timesincelastupdate",
    "winddirection",
    "windgust",
    "windspeed",
};

const std::unordered_set<std::string> kSafetyMonitorMethods = {
    "issafe",
};

bool is_known_device_type_name(const std::string& type_name) {
    static const std::unordered_set<std::string> kDeviceTypes = {
        "camera",
        "telescope",
        "mount",
        "filterwheel",
        "focuser",
        "rotator",
        "dome",
        "shutter",
        "switch",
        "covercalibrator",
        "observingconditions",
        "safetymonitor",
    };
    return kDeviceTypes.count(type_name) > 0;
}

bool is_valid_method(alpacacore::DeviceType type, const std::string& method_name) {
    if (kCommonMethods.count(method_name) > 0) {
        return true;
    }
    switch (type) {
        case alpacacore::DeviceType::Camera:
            return kCameraMethods.count(method_name) > 0;
        case alpacacore::DeviceType::Telescope:
            return kTelescopeMethods.count(method_name) > 0;
        case alpacacore::DeviceType::FilterWheel:
            return kFilterWheelMethods.count(method_name) > 0;
        case alpacacore::DeviceType::Focuser:
            return kFocuserMethods.count(method_name) > 0;
        case alpacacore::DeviceType::Rotator:
            return kRotatorMethods.count(method_name) > 0;
        case alpacacore::DeviceType::Dome:
            return kDomeMethods.count(method_name) > 0;
        case alpacacore::DeviceType::Shutter:
            return kShutterMethods.count(method_name) > 0;
        case alpacacore::DeviceType::Switch:
            return kSwitchMethods.count(method_name) > 0;
        case alpacacore::DeviceType::CoverCalibrator:
            return kCoverCalibratorMethods.count(method_name) > 0;
        case alpacacore::DeviceType::ObservingConditions:
            return kObservingConditionsMethods.count(method_name) > 0;
        case alpacacore::DeviceType::SafetyMonitor:
            return kSafetyMonitorMethods.count(method_name) > 0;
        default:
            return false;
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

const nlohmann::json* find_json_value(const nlohmann::json& json_obj, const std::string& key) {
    if (!json_obj.is_object()) {
        return nullptr;
    }
    auto it = json_obj.find(key);
    if (it != json_obj.end()) {
        return &it.value();
    }
    return nullptr;
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

std::optional<std::string> get_form_value(std::string_view body, const std::string& key) {
    if (body.empty()) {
        return std::nullopt;
    }
    std::istringstream iss{std::string(body)};
    std::string pair;
    while (std::getline(iss, pair, '&')) {
        auto eq_pos = pair.find('=');
        if (eq_pos != std::string::npos) {
            std::string form_key = url_decode(pair.substr(0, eq_pos));
            std::string value = url_decode(pair.substr(eq_pos + 1));
            if (form_key == key) {
                return value;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> get_form_value_case_insensitive(std::string_view body, const std::string& key) {
    if (body.empty()) {
        return std::nullopt;
    }
    const std::string target = to_lower_copy(key);
    std::istringstream iss{std::string(body)};
    std::string pair;
    while (std::getline(iss, pair, '&')) {
        auto eq_pos = pair.find('=');
        if (eq_pos != std::string::npos) {
            std::string form_key = url_decode(pair.substr(0, eq_pos));
            std::string value = url_decode(pair.substr(eq_pos + 1));
            if (to_lower_copy(form_key) == target) {
                return value;
            }
        }
    }
    return std::nullopt;
}

nlohmann::json make_log_level_payload() {
    nlohmann::json payload;
    payload["Level"] = log_level_to_string(alpacacore::logging::get_log_level());
    payload["SupportedLevels"] = {"TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"};
    return payload;
}

nlohmann::json make_log_history_payload() {
    nlohmann::json payload;
    const auto limit = alpacahttp::util::get_log_history_limit();
    payload["Limit"] = limit;
    payload["Unlimited"] = (limit == 0);
    return payload;
}

void append_int(std::string& out, std::int64_t value) {
    out.append(std::to_string(value));
}

std::string build_image_array_payload(const alpacacore::ImageArray& image,
                                      int type,
                                      std::uint32_t client_tx_id,
                                      std::uint32_t server_tx_id) {
    std::size_t estimate = 256;
    if (image.width > 0 && image.height > 0) {
        std::size_t pixels = static_cast<std::size_t>(image.width) *
                             static_cast<std::size_t>(image.height);
        std::size_t per_value = 6;
        if (image.rank == 3) {
            pixels *= 3;
        }
        estimate += pixels * per_value;
    }

    std::string body;
    body.reserve(estimate);
    body.append("{\"ClientTransactionID\":");
    append_int(body, client_tx_id);
    body.append(",\"ServerTransactionID\":");
    append_int(body, server_tx_id);
    body.append(",\"ErrorNumber\":0,\"ErrorMessage\":\"\",\"Type\":");
    append_int(body, type);
    body.append(",\"Rank\":");
    append_int(body, image.rank);
    body.append(",\"Value\":");

    if (image.rank == 2 && image.width > 0 && image.height > 0) {
        body.push_back('[');
        for (int x = 0; x < image.width; ++x) {
            if (x > 0) {
                body.push_back(',');
            }
            body.push_back('[');
            for (int y = 0; y < image.height; ++y) {
                if (y > 0) {
                    body.push_back(',');
                }
                std::size_t idx = static_cast<std::size_t>(y) *
                                  static_cast<std::size_t>(image.width) +
                                  static_cast<std::size_t>(x);
                std::int32_t value = 0;
                if (idx < image.data.size()) {
                    value = image.data[idx];
                }
                append_int(body, value);
            }
            body.push_back(']');
        }
        body.push_back(']');
    } else if (image.rank == 3 && image.width > 0 && image.height > 0) {
        body.push_back('[');
        for (int x = 0; x < image.width; ++x) {
            if (x > 0) {
                body.push_back(',');
            }
            body.push_back('[');
            for (int y = 0; y < image.height; ++y) {
                if (y > 0) {
                    body.push_back(',');
                }
                body.push_back('[');
                std::size_t base = (static_cast<std::size_t>(y) *
                                    static_cast<std::size_t>(image.width) +
                                    static_cast<std::size_t>(x)) * 3;
                for (int c = 0; c < 3; ++c) {
                    if (c > 0) {
                        body.push_back(',');
                    }
                    std::size_t idx = base + static_cast<std::size_t>(c);
                    std::int32_t value = 0;
                    if (idx < image.data.size()) {
                        value = image.data[idx];
                    }
                    append_int(body, value);
                }
                body.push_back(']');
            }
            body.push_back(']');
        }
        body.push_back(']');
    } else {
        body.append("[]");
    }

    body.push_back('}');
    return body;
}

constexpr std::uint32_t kImageTypeInt16 = 1;
constexpr std::uint32_t kImageTypeInt32 = 2;
constexpr std::uint32_t kImageTypeDouble = 3;
constexpr std::uint32_t kImageTypeSingle = 4;
constexpr std::uint32_t kImageTypeUInt64 = 5;
constexpr std::uint32_t kImageTypeByte = 6;
constexpr std::uint32_t kImageTypeInt64 = 7;
constexpr std::uint32_t kImageTypeUInt16 = 8;
constexpr std::uint32_t kImageTypeUInt32 = 9;

constexpr std::uint32_t kImageBytesMetadataVersion = 1;
constexpr std::size_t kImageBytesMetadataSize = 11 * sizeof(std::uint32_t);

struct ImageBytesFormat {
    std::uint32_t image_element_type = kImageTypeInt32;
    std::uint32_t transmission_element_type = kImageTypeUInt16;
    std::size_t bytes_per_element = sizeof(std::uint16_t);
};

bool is_expected_not_implemented(const alpacacore::AlpacaException& e) {
    const int error_code = e.error_code();
    return error_code == alpacacore::AlpacaError::NotImplemented ||
        error_code == alpacacore::AlpacaError::PropertyNotImplemented ||
        error_code == alpacacore::AlpacaError::MethodNotImplemented ||
        error_code == alpacacore::AlpacaError::ActionNotImplemented;
}

bool is_expected_validation_error(const alpacacore::AlpacaException& e) {
    const int error_code = e.error_code();
    return error_code == alpacacore::AlpacaError::InvalidValue ||
        error_code == alpacacore::AlpacaError::ValueNotSet;
}

void log_alpaca_exception(const std::string& context, const alpacacore::AlpacaException& e) {
    std::string message = context + ": " + std::string(e.what());
    if (is_expected_not_implemented(e) || is_expected_validation_error(e)) {
        alpacahttp::util::log_debug(message);
    } else {
        alpacahttp::util::log_error(message);
    }
}

bool accepts_imagebytes(const alpacahttp::Request& request) {
    if (!request.has_header("accept")) {
        return false;
    }
    std::string accept = to_lower_copy(request.get_header("accept"));
    return accept.find("application/imagebytes") != std::string::npos;
}

std::uint32_t image_element_type_from_variant(const std::string& variant, std::uint32_t fallback) {
    if (variant.empty()) {
        return fallback;
    }
    std::string lowered = to_lower_copy(variant);
    if (lowered == "int16" || lowered == "short") {
        return kImageTypeInt16;
    }
    if (lowered == "uint16" || lowered == "ushort") {
        return kImageTypeUInt16;
    }
    if (lowered == "byte" || lowered == "uint8") {
        return kImageTypeByte;
    }
    if (lowered == "double") {
        return kImageTypeDouble;
    }
    if (lowered == "single" || lowered == "float") {
        return kImageTypeSingle;
    }
    if (lowered == "uint32") {
        return kImageTypeUInt32;
    }
    if (lowered == "uint64" || lowered == "ulong") {
        return kImageTypeUInt64;
    }
    if (lowered == "int64" || lowered == "long") {
        return kImageTypeInt64;
    }
    if (lowered == "int32") {
        return kImageTypeInt32;
    }
    return fallback;
}

ImageBytesFormat choose_image_bytes_format(const alpacacore::ImageArray& image,
                                           std::uint32_t image_element_type) {
    ImageBytesFormat format;
    format.image_element_type = image_element_type;

    if (image_element_type == kImageTypeByte) {
        format.transmission_element_type = kImageTypeByte;
        format.bytes_per_element = sizeof(std::uint8_t);
        return format;
    }
    if (image_element_type == kImageTypeUInt16) {
        format.transmission_element_type = kImageTypeUInt16;
        format.bytes_per_element = sizeof(std::uint16_t);
        return format;
    }
    if (image_element_type == kImageTypeInt16) {
        format.transmission_element_type = kImageTypeInt16;
        format.bytes_per_element = sizeof(std::int16_t);
        return format;
    }

    if (image.data.empty()) {
        format.transmission_element_type = kImageTypeUInt16;
        format.bytes_per_element = sizeof(std::uint16_t);
        return format;
    }

    auto minmax = std::minmax_element(image.data.begin(), image.data.end());
    std::int64_t min_val = static_cast<std::int64_t>(*minmax.first);
    std::int64_t max_val = static_cast<std::int64_t>(*minmax.second);

    if (min_val >= 0 && max_val <= std::numeric_limits<std::uint8_t>::max()) {
        format.transmission_element_type = kImageTypeByte;
        format.bytes_per_element = sizeof(std::uint8_t);
    } else if (min_val >= 0 && max_val <= std::numeric_limits<std::uint16_t>::max()) {
        format.transmission_element_type = kImageTypeUInt16;
        format.bytes_per_element = sizeof(std::uint16_t);
    } else if (min_val >= std::numeric_limits<std::int16_t>::min() &&
               max_val <= std::numeric_limits<std::int16_t>::max()) {
        format.transmission_element_type = kImageTypeInt16;
        format.bytes_per_element = sizeof(std::int16_t);
    } else {
        format.transmission_element_type = kImageTypeInt32;
        format.bytes_per_element = sizeof(std::int32_t);
    }

    return format;
}

void append_uint32_le(std::string& out, std::uint32_t value) {
    char bytes[4];
    bytes[0] = static_cast<char>(value & 0xFF);
    bytes[1] = static_cast<char>((value >> 8) & 0xFF);
    bytes[2] = static_cast<char>((value >> 16) & 0xFF);
    bytes[3] = static_cast<char>((value >> 24) & 0xFF);
    out.append(bytes, sizeof(bytes));
}

void append_uint16_le(std::string& out, std::uint16_t value) {
    char bytes[2];
    bytes[0] = static_cast<char>(value & 0xFF);
    bytes[1] = static_cast<char>((value >> 8) & 0xFF);
    out.append(bytes, sizeof(bytes));
}

void append_int16_le(std::string& out, std::int16_t value) {
    append_uint16_le(out, static_cast<std::uint16_t>(value));
}

void append_uint8(std::string& out, std::uint8_t value) {
    out.push_back(static_cast<char>(value));
}

void append_int32_le(std::string& out, std::int32_t value) {
    append_uint32_le(out, static_cast<std::uint32_t>(value));
}

std::string build_image_bytes_payload(const alpacacore::ImageArray& image,
                                      const ImageBytesFormat& format,
                                      std::uint32_t client_tx_id,
                                      std::uint32_t server_tx_id) {
    std::uint32_t width = image.width > 0 ? static_cast<std::uint32_t>(image.width) : 0;
    std::uint32_t height = image.height > 0 ? static_cast<std::uint32_t>(image.height) : 0;
    std::uint32_t rank = image.rank > 0 ? static_cast<std::uint32_t>(image.rank) : 0;
    std::uint32_t planes = 0;
    if (rank == 3) {
        planes = 3;
        if (width > 0 && height > 0) {
            auto expected = static_cast<std::uint64_t>(width) * height * planes;
            if (expected == 0 || expected > image.data.size()) {
                planes = 3;
            }
        }
    }

    std::uint64_t pixel_count = 0;
    if (width > 0 && height > 0) {
        std::uint64_t base = static_cast<std::uint64_t>(width) * height;
        if (rank == 3) {
            base *= (planes == 0 ? 3 : planes);
        }
        pixel_count = base;
    }

    std::uint64_t data_bytes = pixel_count * format.bytes_per_element;
    if (data_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        data_bytes = 0;
    }

    std::string body;
    body.reserve(kImageBytesMetadataSize + static_cast<std::size_t>(data_bytes));

    append_uint32_le(body, kImageBytesMetadataVersion);
    append_uint32_le(body, 0);
    append_uint32_le(body, client_tx_id);
    append_uint32_le(body, server_tx_id);
    append_uint32_le(body, static_cast<std::uint32_t>(kImageBytesMetadataSize));
    append_uint32_le(body, format.image_element_type);
    append_uint32_le(body, format.transmission_element_type);
    append_uint32_le(body, rank);
    append_uint32_le(body, width);
    append_uint32_le(body, height);
    append_uint32_le(body, rank == 3 ? planes : 0);

    if (pixel_count == 0) {
        return body;
    }

    auto append_value = [&](std::int32_t value) {
        switch (format.transmission_element_type) {
            case kImageTypeByte: {
                std::uint8_t out = 0;
                if (value > 0) {
                    out = static_cast<std::uint8_t>(std::min<std::int32_t>(
                        value, std::numeric_limits<std::uint8_t>::max()));
                }
                append_uint8(body, out);
                break;
            }
            case kImageTypeUInt16: {
                std::uint16_t out = 0;
                if (value > 0) {
                    out = static_cast<std::uint16_t>(std::min<std::int32_t>(
                        value, std::numeric_limits<std::uint16_t>::max()));
                }
                append_uint16_le(body, out);
                break;
            }
            case kImageTypeInt16: {
                std::int16_t out = 0;
                if (value < std::numeric_limits<std::int16_t>::min()) {
                    out = std::numeric_limits<std::int16_t>::min();
                } else if (value > std::numeric_limits<std::int16_t>::max()) {
                    out = std::numeric_limits<std::int16_t>::max();
                } else {
                    out = static_cast<std::int16_t>(value);
                }
                append_int16_le(body, out);
                break;
            }
            case kImageTypeInt32:
            default:
                append_int32_le(body, value);
                break;
        }
    };

    if (rank == 2 && width > 0 && height > 0) {
        for (std::uint32_t x = 0; x < width; ++x) {
            for (std::uint32_t y = 0; y < height; ++y) {
                std::size_t idx = static_cast<std::size_t>(y) *
                                  static_cast<std::size_t>(width) +
                                  static_cast<std::size_t>(x);
                std::int32_t value = 0;
                if (idx < image.data.size()) {
                    value = image.data[idx];
                }
                append_value(value);
            }
        }
    } else if (rank == 3 && width > 0 && height > 0) {
        std::uint32_t channels = planes == 0 ? 3 : planes;
        for (std::uint32_t x = 0; x < width; ++x) {
            for (std::uint32_t y = 0; y < height; ++y) {
                std::size_t base = (static_cast<std::size_t>(y) *
                                    static_cast<std::size_t>(width) +
                                    static_cast<std::size_t>(x)) * channels;
                for (std::uint32_t c = 0; c < channels; ++c) {
                    std::size_t idx = base + static_cast<std::size_t>(c);
                    std::int32_t value = 0;
                    if (idx < image.data.size()) {
                        value = image.data[idx];
                    }
                    append_value(value);
                }
            }
        }
    }

    return body;
}

} // namespace

namespace alpacahttp {

Router::Router() {
    set_server_info("AlpacaHTTP", "AlpacaHTTP", alpacahttp::kVersion, "");
    load_persisted_devices();
}
Router::~Router() = default;

void Router::set_management_driver(std::shared_ptr<alpacacore::ManagementDriver> mgmt_driver) {
    management_driver_ = mgmt_driver;
}

void Router::set_server_info(std::string server_name,
                             std::string manufacturer,
                             std::string manufacturer_version,
                             std::string location) {
    std::lock_guard<std::mutex> lock(server_info_mutex_);
    server_name_ = std::move(server_name);
    manufacturer_ = std::move(manufacturer);
    manufacturer_version_ = std::move(manufacturer_version);
    location_ = std::move(location);
}

void Router::set_config_path(std::string config_path) {
    std::lock_guard<std::mutex> lock(server_info_mutex_);
    config_path_ = std::move(config_path);
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
    util::log_debug("HTTP " + method_str + " " + request.path());

    try {
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
    if (path == "/management/v1/configureddevices" || path == "/management/configureddevices") {
        match.is_management = true;
        match.management_endpoint = "configureddevices";
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
    if (path == "/management/v1/loghistory" || path == "/management/loghistory") {
        match.is_management = true;
        match.management_endpoint = "loghistory";
        return match;
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

    // Device API: /api/v1/{devicetype}/{devicenumber}/{method}
    std::regex device_regex(R"(/api/v1/([^/]+)/(\d+)/([^/?]+))");
    std::smatch matches;
    if (std::regex_match(path, matches, device_regex)) {
        match.device_type = matches[1].str();
        match.device_number = static_cast<std::uint32_t>(std::stoul(matches[2].str()));
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
    } else if (match.management_endpoint == "configureddevices") {
        return handle_configured_devices(request, server_tx_id);
    } else if (match.management_endpoint == "configuredevice") {
        return handle_configure_device(request, server_tx_id);
    } else if (match.management_endpoint == "removedevice") {
        return handle_remove_device(request, server_tx_id);
    } else if (match.management_endpoint == "loglevel") {
        return handle_log_level(request, server_tx_id);
    } else if (match.management_endpoint == "logs") {
        return handle_logs(request, server_tx_id);
    } else if (match.management_endpoint == "loghistory") {
        return handle_log_history(request, server_tx_id);
    } else if (match.management_endpoint == "shutdown") {
        return handle_shutdown(request, server_tx_id);
    } else if (match.management_endpoint == "restart") {
        return handle_restart(request, server_tx_id);
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

    if (management_driver_) {
        desc["ServerName"] = management_driver_->get_name();
        desc["Manufacturer"] = management_driver_->get_manufacturer();
        desc["ManufacturerVersion"] = management_driver_->get_manufacturer_version();
        desc["Location"] = management_driver_->get_location();
        return desc;
    }

    std::string server_name;
    std::string manufacturer;
    std::string manufacturer_version;
    std::string location;
    {
        std::lock_guard<std::mutex> lock(server_info_mutex_);
        server_name = server_name_;
        manufacturer = manufacturer_;
        manufacturer_version = manufacturer_version_;
        location = location_;
    }

    desc["ServerName"] = server_name;
    desc["Manufacturer"] = manufacturer;
    desc["ManufacturerVersion"] = manufacturer_version;
    desc["Location"] = location;
    return desc;
}

Response Router::handle_description(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    response.set_content_type("application/json");

    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
    }

    try {
        if (request.method() == HttpMethod::POST || request.method() == HttpMethod::PUT) {
            if (management_driver_) {
                AlpacaResponse err = make_error_response(
                    client_tx_id, server_tx_id,
                    util::ErrorCode::INVALID_OPERATION,
                    "Location updates are not supported when a management driver is active"
                );
                response.set_body(err);
                return response;
            }

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

            std::string new_location;
            if (body.contains("Location")) {
                new_location = body["Location"].get<std::string>();
            } else if (body.contains("location")) {
                new_location = body["location"].get<std::string>();
            } else {
                AlpacaResponse err = make_error_response(
                    client_tx_id, server_tx_id,
                    util::ErrorCode::VALUE_NOT_SET,
                    "Request must include a 'Location' property"
                );
                response.set_body(err);
                return response;
            }

            std::string config_path;
            {
                std::lock_guard<std::mutex> lock(server_info_mutex_);
                config_path = config_path_;
            }

            if (!config_path.empty()) {
                std::string persist_error;
                if (!update_location_in_config(config_path, new_location, persist_error)) {
                    AlpacaResponse err = make_error_response(
                        client_tx_id, server_tx_id,
                        util::ErrorCode::DRIVER_ERROR,
                        "Failed to persist location: " + persist_error
                    );
                    response.set_body(err);
                    return response;
                }
            }

            {
                std::lock_guard<std::mutex> lock(server_info_mutex_);
                location_ = new_location;
            }
        } else if (request.method() != HttpMethod::GET) {
            AlpacaResponse alpaca_response = make_error_response(
                client_tx_id, server_tx_id,
                util::ErrorCode::INVALID_OPERATION,
                "Unsupported HTTP method for description endpoint"
            );
            response.set_body(alpaca_response);
            response.set_status(405, "Method Not Allowed");
            return response;
        }

        nlohmann::json desc = build_description_payload();
        AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
        alpaca_response.value = desc.dump();
        response.set_body(alpaca_response);
    } catch (const std::exception& e) {
        util::log_error("Error getting description: " + std::string(e.what()));
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::exception_to_error_code(e),
            util::exception_to_error_message(e)
        );
        response.set_body(alpaca_response);
    }

    return response;
}

Response Router::handle_configured_devices(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    
    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
    }

    try {
        load_persisted_devices();

        auto find_config = [this](const std::string& device_type, int device_number) -> const nlohmann::json* {
            std::string target_type = device_type;
            std::transform(target_type.begin(), target_type.end(), target_type.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            for (const auto& entry : persisted_devices_) {
                std::string entry_type = entry.value("deviceType", "");
                std::transform(entry_type.begin(), entry_type.end(), entry_type.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (entry_type == target_type &&
                    entry.value("deviceNumber", -1) == device_number) {
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
            devices.push_back(device);
        }

        AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
        alpaca_response.value = devices.dump();
        response.set_body(alpaca_response);
    } catch (const std::exception& e) {
        util::log_error("Error getting configured devices: " + std::string(e.what()));
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::exception_to_error_code(e),
            util::exception_to_error_message(e)
        );
        response.set_body(alpaca_response);
    }

    return response;
}

Response Router::handle_device(const Request& request, const RouteMatch& match, std::uint32_t server_tx_id) {
    Response response;

    // Extract client transaction ID from request
    std::uint32_t client_tx_id = 0;
    if (request.method() == HttpMethod::PUT && !request.body().empty()) {
        // Try JSON first
        auto json_opt = parse_json(request.body());
        if (json_opt) {
            client_tx_id = extract_client_transaction_id(*json_opt);
        } else {
            if (auto value = get_form_value(request.body(), "ClientTransactionID")) {
                client_tx_id = parse_client_transaction_id(*value);
            } else if (auto value = get_form_value_case_insensitive(request.body(), "ClientTransactionID")) {
                client_tx_id = parse_client_transaction_id(*value);
            }
        }
    } else if (request.method() == HttpMethod::GET) {
        if (request.has_query_param("ClientTransactionID")) {
            client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
        }
    }

    if (!is_lowercase_ascii(match.device_type) || !is_known_device_type_name(match.device_type)) {
        response.set_status(404, "Not Found");
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::INVALID_VALUE,
            "Unknown device type: " + match.device_type
        );
        response.set_body(alpaca_response);
        return response;
    }

    try {
        // Convert device type string to enum
        alpacacore::DeviceType device_type = string_to_device_type(match.device_type);

        if (!is_lowercase_ascii(match.method_name) || !is_valid_method(device_type, match.method_name)) {
            response.set_status(404, "Not Found");
            AlpacaResponse alpaca_response = make_error_response(
                client_tx_id, server_tx_id,
                util::ErrorCode::INVALID_VALUE,
                "Unknown method: " + match.method_name
            );
            response.set_body(alpaca_response);
            return response;
        }
        
        // Get device from registry
        auto& registry = alpacacore::management::DeviceRegistry::instance();
        auto device = registry.get_device(device_type, static_cast<int>(match.device_number));
        
        if (!device) {
            response.set_status(404, "Not Found");
            AlpacaResponse alpaca_response = make_error_response(
                client_tx_id, server_tx_id,
                util::ErrorCode::INVALID_VALUE,
                "Device not found: " + match.device_type + " #" + std::to_string(match.device_number)
            );
            response.set_body(alpaca_response);
            return response;
        }

        // Dispatch the method call
        return dispatch_device_method(device, match.method_name, request, client_tx_id, server_tx_id);
        
    } catch (const std::exception& e) {
        util::log_error("Device error: " + std::string(e.what()));
        auto error_code = util::exception_to_error_code(e);
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            util::exception_to_error_message(e)
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
    }

    return response;
}

alpacacore::DeviceType Router::string_to_device_type(const std::string& type_str) const {
    if (type_str == "camera") return alpacacore::DeviceType::Camera;
    if (type_str == "telescope" || type_str == "mount") return alpacacore::DeviceType::Telescope;
    if (type_str == "filterwheel") return alpacacore::DeviceType::FilterWheel;
    if (type_str == "focuser") return alpacacore::DeviceType::Focuser;
    if (type_str == "rotator") return alpacacore::DeviceType::Rotator;
    if (type_str == "dome") return alpacacore::DeviceType::Dome;
    if (type_str == "shutter") return alpacacore::DeviceType::Shutter;
    if (type_str == "switch") return alpacacore::DeviceType::Switch;
    if (type_str == "covercalibrator") return alpacacore::DeviceType::CoverCalibrator;
    if (type_str == "observingconditions") return alpacacore::DeviceType::ObservingConditions;
    if (type_str == "safetymonitor") return alpacacore::DeviceType::SafetyMonitor;

    throw std::runtime_error("Unknown device type: " + type_str);
}

Response Router::dispatch_device_method(
    std::shared_ptr<alpacacore::AlpacaDriver> device,
    const std::string& method_name,
    const Request& request,
    std::uint32_t client_tx_id,
    std::uint32_t server_tx_id) {
    
    Response response;
    
    try {
        // Handle common AlpacaDriver methods
        if (method_name == "name") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, device->get_name());
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "description") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, device->get_description());
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "driverinfo") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, device->get_driver_info());
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "driverversion") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, device->get_driver_version());
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "interfaceversion") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, static_cast<std::int32_t>(device->get_interface_version()));
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "connected") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, device->get_connected());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                bool connected = false;
                bool found = false;
                
                // Try parsing as JSON first
                auto json_opt = parse_json(request.body());
                if (json_opt) {
                    if (const auto* val = find_json_value(*json_opt, "Connected")) {
                        if (val->is_boolean()) {
                            connected = val->get<bool>();
                        } else if (val->is_string()) {
                            connected = parse_bool_value(val->get<std::string>(), "Connected");
                        } else {
                            throw std::runtime_error("Invalid JSON value for parameter: Connected");
                        }
                        found = true;
                    } else if (const auto* val = find_json_value(*json_opt, "Value")) {
                        if (val->is_boolean()) {
                            connected = val->get<bool>();
                        } else if (val->is_string()) {
                            connected = parse_bool_value(val->get<std::string>(), "Connected");
                        } else {
                            throw std::runtime_error("Invalid JSON value for parameter: Connected");
                        }
                        found = true;
                    }
                }
                
                // If JSON parsing failed or didn't contain Connected/Value, try form-encoded
                if (!found) {
                    if (auto value = get_form_value(request.body(), "Connected")) {
                        connected = parse_bool_value(*value, "Connected");
                        found = true;
                    } else if (auto value = get_form_value(request.body(), "Value")) {
                        connected = parse_bool_value(*value, "Connected");
                        found = true;
                    }
                }
                
                if (!found) {
                    throw std::runtime_error("Missing parameter: Connected");
                }

                if (connected && !device->get_connected()) {
                    // Use async connect then poll for completion.
                    // Slow-connecting devices (serial focusers etc.) can exceed
                    // ASCOM Alpaca client timeouts if set_connected() blocks
                    // synchronously.  The async path + poll lets us return as
                    // soon as the handshake succeeds without hard-blocking the
                    // full worst-case duration.
                    device->connect();
                    auto deadline = std::chrono::steady_clock::now()
                                  + std::chrono::seconds(8);
                    while (!device->get_connected()
                           && device->get_connecting()
                           && std::chrono::steady_clock::now() < deadline) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(100));
                    }
                    if (!device->get_connected() && !device->get_connecting()) {
                        throw alpacacore::AlpacaException(
                            "Connection failed",
                            alpacacore::AlpacaError::NotConnected);
                    }
                } else if (!connected && device->get_connected()) {
                    device->disconnect();
                    auto deadline = std::chrono::steady_clock::now()
                                  + std::chrono::seconds(8);
                    while (device->get_connected()
                           && device->get_connecting()
                           && std::chrono::steady_clock::now() < deadline) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(50));
                    }
                }
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "supportedactions") {
            if (request.method() == HttpMethod::GET) {
                nlohmann::json actions = nlohmann::json::array();
                for (const auto& action : device->get_supported_actions()) {
                    actions.push_back(action);
                }
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, actions.dump());
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "action") {
            if (request.method() == HttpMethod::PUT) {
                auto json_opt = parse_json(request.body());
                std::string action_name;
                std::string action_parameters = "{}";
                
                if (json_opt) {
                    if (const auto* val = find_json_value(*json_opt, "Action")) {
                        if (!val->is_string()) {
                            throw std::runtime_error("Invalid JSON value for parameter: Action");
                        }
                        action_name = val->get<std::string>();
                    }
                    if (const auto* val = find_json_value(*json_opt, "Parameters")) {
                        action_parameters = val->dump();
                    }
                }
                
                if (action_name.empty()) {
                    if (auto value = get_form_value(request.body(), "Action")) {
                        action_name = *value;
                    }
                }

                if (auto value = get_form_value(request.body(), "Parameters")) {
                    action_parameters = *value;
                }

                if (action_name.empty()) {
                    throw std::runtime_error("Missing parameter: Action");
                }
                
                std::string result = device->action(action_name, action_parameters);
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, result);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "commandblind") {
            if (request.method() == HttpMethod::PUT) {
                std::string command;
                bool raw = false;
                
                auto json_opt = parse_json(request.body());
                if (json_opt) {
                    if (const auto* val = find_json_value(*json_opt, "Command")) {
                        if (!val->is_string()) {
                            throw std::runtime_error("Invalid JSON value for parameter: Command");
                        }
                        command = val->get<std::string>();
                    }
                    if (const auto* val = find_json_value(*json_opt, "Raw")) {
                        if (val->is_boolean()) {
                            raw = val->get<bool>();
                        } else if (val->is_string()) {
                            raw = parse_bool_value(val->get<std::string>(), "Raw");
                        } else {
                            throw std::runtime_error("Invalid JSON value for parameter: Raw");
                        }
                    }
                }
                
                if (command.empty()) {
                    if (auto value = get_form_value(request.body(), "Command")) {
                        command = *value;
                    }
                }
                if (auto value = get_form_value(request.body(), "Raw")) {
                    raw = parse_bool_value(*value, "Raw");
                }

                if (command.empty()) {
                    throw std::runtime_error("Missing parameter: Command");
                }
                
                device->command_blind(command, raw);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "commandbool") {
            if (request.method() == HttpMethod::PUT) {
                std::string command;
                bool raw = false;
                
                auto json_opt = parse_json(request.body());
                if (json_opt) {
                    if (const auto* val = find_json_value(*json_opt, "Command")) {
                        if (!val->is_string()) {
                            throw std::runtime_error("Invalid JSON value for parameter: Command");
                        }
                        command = val->get<std::string>();
                    }
                    if (const auto* val = find_json_value(*json_opt, "Raw")) {
                        if (val->is_boolean()) {
                            raw = val->get<bool>();
                        } else if (val->is_string()) {
                            raw = parse_bool_value(val->get<std::string>(), "Raw");
                        } else {
                            throw std::runtime_error("Invalid JSON value for parameter: Raw");
                        }
                    }
                }
                
                if (command.empty()) {
                    if (auto value = get_form_value(request.body(), "Command")) {
                        command = *value;
                    }
                }
                if (auto value = get_form_value(request.body(), "Raw")) {
                    raw = parse_bool_value(*value, "Raw");
                }

                if (command.empty()) {
                    throw std::runtime_error("Missing parameter: Command");
                }
                
                bool result = device->command_bool(command, raw);
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, result);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "commandstring") {
            if (request.method() == HttpMethod::PUT) {
                std::string command;
                bool raw = false;
                
                auto json_opt = parse_json(request.body());
                if (json_opt) {
                    if (const auto* val = find_json_value(*json_opt, "Command")) {
                        if (!val->is_string()) {
                            throw std::runtime_error("Invalid JSON value for parameter: Command");
                        }
                        command = val->get<std::string>();
                    }
                    if (const auto* val = find_json_value(*json_opt, "Raw")) {
                        if (val->is_boolean()) {
                            raw = val->get<bool>();
                        } else if (val->is_string()) {
                            raw = parse_bool_value(val->get<std::string>(), "Raw");
                        } else {
                            throw std::runtime_error("Invalid JSON value for parameter: Raw");
                        }
                    }
                }
                
                if (command.empty()) {
                    if (auto value = get_form_value(request.body(), "Command")) {
                        command = *value;
                    }
                }
                if (auto value = get_form_value(request.body(), "Raw")) {
                    raw = parse_bool_value(*value, "Raw");
                }

                if (command.empty()) {
                    throw std::runtime_error("Missing parameter: Command");
                }
                
                std::string result = device->command_string(command, raw);
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, result);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "connect") {
            if (request.method() == HttpMethod::PUT) {
                device->connect();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "disconnect") {
            if (request.method() == HttpMethod::PUT) {
                device->disconnect();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "connecting") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, device->get_connecting());
                response.set_body(alpaca_response);
                return response;
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
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, values.dump());
                response.set_body(alpaca_response);
                return response;
            }
        }
        
        // Try device-specific methods
        alpacacore::DeviceType device_type = device->get_device_type();
        
        // Handle telescope-specific methods
        if (device_type == alpacacore::DeviceType::Telescope) {
            auto telescope = std::dynamic_pointer_cast<alpacacore::TelescopeDriver>(device);
            if (telescope) {
                return dispatch_telescope_method(telescope, method_name, request, client_tx_id, server_tx_id);
            }
        }
        if (device_type == alpacacore::DeviceType::Camera) {
            auto camera = std::dynamic_pointer_cast<alpacacore::CameraDriver>(device);
            if (camera) {
                return dispatch_camera_method(camera, method_name, request, client_tx_id, server_tx_id);
            }
        }
        if (device_type == alpacacore::DeviceType::Switch) {
            auto sw = std::dynamic_pointer_cast<alpacacore::SwitchDriver>(device);
            if (sw) {
                return dispatch_switch_method(sw, method_name, request, client_tx_id, server_tx_id);
            }
        }
        if (device_type == alpacacore::DeviceType::FilterWheel) {
            auto filterwheel = std::dynamic_pointer_cast<alpacacore::FilterWheelDriver>(device);
            if (filterwheel) {
                return dispatch_filterwheel_method(filterwheel, method_name, request, client_tx_id, server_tx_id);
            }
        }
        if (device_type == alpacacore::DeviceType::Focuser) {
            auto focuser = std::dynamic_pointer_cast<alpacacore::FocuserDriver>(device);
            if (focuser) {
                return dispatch_focuser_method(focuser, method_name, request, client_tx_id, server_tx_id);
            }
        }
        if (device_type == alpacacore::DeviceType::Rotator) {
            auto rotator = std::dynamic_pointer_cast<alpacacore::RotatorDriver>(device);
            if (rotator) {
                return dispatch_rotator_method(rotator, method_name, request, client_tx_id, server_tx_id);
            }
        }
        if (device_type == alpacacore::DeviceType::Dome) {
            auto dome = std::dynamic_pointer_cast<alpacacore::DomeDriver>(device);
            if (dome) {
                return dispatch_dome_method(dome, method_name, request, client_tx_id, server_tx_id);
            }
        }
        if (device_type == alpacacore::DeviceType::Shutter) {
            auto shutter = std::dynamic_pointer_cast<alpacacore::ShutterDriver>(device);
            if (shutter) {
                return dispatch_shutter_method(shutter, method_name, request, client_tx_id, server_tx_id);
            }
        }
        if (device_type == alpacacore::DeviceType::CoverCalibrator) {
            auto covercalibrator = std::dynamic_pointer_cast<alpacacore::CoverCalibratorDriver>(device);
            if (covercalibrator) {
                return dispatch_covercalibrator_method(covercalibrator, method_name, request, client_tx_id, server_tx_id);
            }
        }
        if (device_type == alpacacore::DeviceType::ObservingConditions) {
            auto observingconditions = std::dynamic_pointer_cast<alpacacore::ObservingConditionsDriver>(device);
            if (observingconditions) {
                return dispatch_observingconditions_method(observingconditions, method_name, request, client_tx_id, server_tx_id);
            }
        }
        if (device_type == alpacacore::DeviceType::SafetyMonitor) {
            auto safetymonitor = std::dynamic_pointer_cast<alpacacore::SafetyMonitorDriver>(device);
            if (safetymonitor) {
                return dispatch_safetymonitor_method(safetymonitor, method_name, request, client_tx_id, server_tx_id);
            }
        }
        
        // For other device types or if cast failed, return not implemented
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::NOT_IMPLEMENTED,
            "Method '" + method_name + "' not yet implemented for device type " +
            std::string(alpacacore::device_type_to_string(device_type))
        );
        response.set_body(alpaca_response);
        return response;
        
    } catch (const alpacacore::AlpacaException& e) {
        log_alpaca_exception("AlpacaException in device method", e);
        auto error_code = util::map_error_code(e.error_code());
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            std::string(e.what())
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
        return response;
    } catch (const std::exception& e) {
        util::log_error("Exception in device method: " + std::string(e.what()));
        auto error_code = util::exception_to_error_code(e);
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            util::exception_to_error_message(e)
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
        return response;
    }
}

Response Router::dispatch_telescope_method(
    std::shared_ptr<alpacacore::TelescopeDriver> telescope,
    const std::string& method_name,
    const Request& request,
    std::uint32_t client_tx_id,
    std::uint32_t server_tx_id) {
    
    Response response;
    
    try {
        // Helper function to parse double from query param, JSON body, or form-encoded body
        auto parse_double = [&](const std::string& param_name) -> double {
            // Try query parameter first
            if (request.has_query_param(param_name)) {
                return parse_double_value(request.get_query_param(param_name), param_name);
            }
            // Try JSON body
            if (!request.body().empty()) {
                auto json_opt = parse_json(request.body());
                if (json_opt) {
                    if (const auto* val = find_json_value(*json_opt, param_name)) {
                        if (val->is_number()) {
                            return val->get<double>();
                        }
                        if (val->is_string()) {
                            return parse_double_value(val->get<std::string>(), param_name);
                        }
                        throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
                    }
                }
            }
            // Try form-encoded
            if (auto value = get_form_value(request.body(), param_name)) {
                return parse_double_value(*value, param_name);
            }
            throw std::runtime_error("Missing parameter: " + param_name);
        };
        
        // Helper function to parse int from query param, JSON body, or form-encoded body
        auto parse_int = [&](const std::string& param_name) -> int {
            // Try query parameter first
            if (request.has_query_param(param_name)) {
                return parse_int_value(request.get_query_param(param_name), param_name);
            }
            // Try JSON body
            if (!request.body().empty()) {
                auto json_opt = parse_json(request.body());
                if (json_opt) {
                    if (const auto* val = find_json_value(*json_opt, param_name)) {
                        if (val->is_number_integer()) {
                            return val->get<int>();
                        }
                        if (val->is_number()) {
                            return static_cast<int>(val->get<double>());
                        }
                        if (val->is_string()) {
                            return parse_int_value(val->get<std::string>(), param_name);
                        }
                        throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
                    }
                }
            }
            // Try form-encoded
            if (auto value = get_form_value(request.body(), param_name)) {
                return parse_int_value(*value, param_name);
            }
            throw std::runtime_error("Missing parameter: " + param_name);
        };
        
        // Helper function to parse bool from query param, JSON body, or form-encoded body
        auto parse_bool = [&](const std::string& param_name) -> bool {
            // Try query parameter first
            if (request.has_query_param(param_name)) {
                return parse_bool_value(request.get_query_param(param_name), param_name);
            }
            
            // Try JSON body
            auto json_opt = parse_json(request.body());
            if (json_opt) {
                if (const auto* val = find_json_value(*json_opt, param_name)) {
                    if (val->is_boolean()) {
                        return val->get<bool>();
                    }
                    if (val->is_string()) {
                        return parse_bool_value(val->get<std::string>(), param_name);
                    }
                    throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
                }
                if (const auto* val = find_json_value(*json_opt, "Value")) {
                    if (val->is_boolean()) {
                        return val->get<bool>();
                    }
                    if (val->is_string()) {
                        return parse_bool_value(val->get<std::string>(), param_name);
                    }
                    throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
                }
            }
            
            // Try form-encoded body (e.g., "Tracking=true" or "Tracking=1")
            if (auto value = get_form_value(request.body(), param_name)) {
                return parse_bool_value(*value, param_name);
            }
            if (auto value = get_form_value(request.body(), "Value")) {
                return parse_bool_value(*value, param_name);
            }
            
            throw std::runtime_error("Missing parameter: " + param_name);
        };
        
        // Helper function to parse string from query param or JSON body
        auto parse_string = [&](const std::string& param_name) -> std::string {
            if (request.has_query_param(param_name)) {
                return request.get_query_param(param_name);
            }
            auto json_opt = parse_json(request.body());
            if (json_opt) {
                if (const auto* val = find_json_value(*json_opt, param_name)) {
                    if (!val->is_string()) {
                        throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
                    }
                    return val->get<std::string>();
                }
                if (const auto* val = find_json_value(*json_opt, "Value")) {
                    if (!val->is_string()) {
                        throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
                    }
                    return val->get<std::string>();
                }
            }
            if (auto value = get_form_value(request.body(), param_name)) {
                return *value;
            }
            if (auto value = get_form_value(request.body(), "Value")) {
                return *value;
            }
            throw std::runtime_error("Missing parameter: " + param_name);
        };
        
        // GET-only boolean properties
        if (request.method() == HttpMethod::GET) {
            if (method_name == "cansetrightascensionrate") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_set_right_ascension_rate());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "cansetdeclinationrate") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_set_declination_rate());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "cansetguiderates") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_set_guide_rates());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "cansettracking") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_set_tracking());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "cansetpark") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_set_park());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "cansetpierside") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_set_pier_side());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "canfindhome") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_find_home());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "canpark") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_park());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "canpulseguide") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_pulse_guide());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "canslew") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_slew());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "canslewasync") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_slew_async());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "cansync") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_sync());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "canunpark") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_unpark());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "canslewaltaz") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_slew_alt_az());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "canslewaltazasync") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_slew_alt_az_async());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "cansyncaltaz") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_can_sync_alt_az());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "alignmentmode") {
                int mode = static_cast<int>(telescope->get_alignment_mode());
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, mode);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "equatorialsystem") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, static_cast<int>(telescope->get_equatorial_system()));
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "doesrefraction") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_does_refraction());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "ispulseguiding") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_is_pulse_guiding());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "slewsettletime") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_slew_settle_time());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "utcdate") {
                auto utc = telescope->get_utc_date();
                auto time_t = std::chrono::system_clock::to_time_t(utc);
                std::ostringstream oss;
                oss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%S");
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    utc.time_since_epoch()) % 1000;
                oss << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z";
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, oss.str());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "axisrates") {
                int axis = 0;
                if (request.has_query_param("Axis")) {
                    axis = parse_int_value(request.get_query_param("Axis"), "Axis");
                }
                // AxisRates must return an array of rate objects, even if only one range.
                auto ranges = telescope->get_axis_rate_ranges(axis);
                nlohmann::json rates_array = nlohmann::json::array();
                for (const auto& range : ranges) {
                    nlohmann::json rate_obj;
                    rate_obj["Minimum"] = range.first;
                    rate_obj["Maximum"] = range.second;
                    rates_array.push_back(rate_obj);
                }
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, rates_array.dump());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "canmoveaxis") {
                int axis = 0;
                if (request.has_query_param("Axis")) {
                    axis = parse_int_value(request.get_query_param("Axis"), "Axis");
                }
                bool can_move = telescope->get_can_move_axis(axis);
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, can_move);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "destinationsideofpier") {
                auto ra_value = get_query_param_case_insensitive(request, "RightAscension");
                if (!ra_value) {
                    throw std::runtime_error("Missing parameter: RightAscension");
                }
                auto dec_value = get_query_param_case_insensitive(request, "Declination");
                if (!dec_value) {
                    throw std::runtime_error("Missing parameter: Declination");
                }
                double ra = parse_double_value(*ra_value, "RightAscension");
                double dec = parse_double_value(*dec_value, "Declination");
                int side = telescope->get_destination_side_of_pier(ra, dec);
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, side);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "athome") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_at_home());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "atpark") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_at_park());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "slewing") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_slewing());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "tracking") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_tracking());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "altitude") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_altitude());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "azimuth") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_azimuth());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "declination") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_declination());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "rightascension") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_right_ascension());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "declinationrate") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_declination_rate());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "rightascensionrate") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_right_ascension_rate());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "trackingrate") {
                int rate = telescope->get_tracking_rate();
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, rate);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "trackingrates") {
                auto rates = telescope->get_tracking_rates();
                nlohmann::json rates_array = nlohmann::json::array();
                for (int rate : rates) {
                    rates_array.push_back(rate);
                }
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, rates_array.dump());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "targetdeclination") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_target_declination());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "targetrightascension") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_target_right_ascension());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "siderealtime") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_sidereal_time());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "siteelevation") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_site_elevation());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "sitelatitude") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_site_latitude());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "sitelongitude") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_site_longitude());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "focallength") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_focal_length());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "aperturediameter") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_aperture_diameter());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "aperturearea") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_aperture_area());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "sideofpier") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_side_of_pier());
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "guideratedeclination") {
                auto guide_rate = telescope->get_guide_rate();
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, guide_rate.dec);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "guideraterightascension") {
                auto guide_rate = telescope->get_guide_rate();
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, guide_rate.ra);
                response.set_body(alpaca_response);
                return response;
            }
        }
        
        // GET/PUT properties
        if (method_name == "doesrefraction") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_does_refraction());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                bool value = parse_bool("DoesRefraction");
                telescope->set_does_refraction(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "slewsettletime") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_slew_settle_time());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                int value = parse_int("SlewSettleTime");
                telescope->set_slew_settle_time(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "utcdate") {
            if (request.method() == HttpMethod::GET) {
                auto utc = telescope->get_utc_date();
                auto time_t = std::chrono::system_clock::to_time_t(utc);
                std::ostringstream oss;
                oss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%S");
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    utc.time_since_epoch()) % 1000;
                oss << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z";
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, oss.str());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                std::string date_str;
                if (request.has_query_param("UTCDate")) {
                    date_str = request.get_query_param("UTCDate");
                } else if (auto json_opt = parse_json(request.body())) {
                    if (const auto* val = find_json_value(*json_opt, "UTCDate")) {
                        date_str = val->get<std::string>();
                    } else if (const auto* val = find_json_value(*json_opt, "Value")) {
                        date_str = val->get<std::string>();
                    }
                }
                if (date_str.empty()) {
                    if (auto value = get_form_value(request.body(), "UTCDate")) {
                        date_str = *value;
                    } else if (auto value = get_form_value(request.body(), "Value")) {
                        date_str = *value;
                    }
                }
                if (date_str.empty()) {
                    throw std::runtime_error("Missing parameter: UTCDate or Value");
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
                        frac = frac.substr(0, 3);
                    }
                    while (frac.size() < 3) {
                        frac.push_back('0');
                    }
                    try {
                        millis = std::stoi(frac);
                    } catch (const std::exception&) {
                        throw std::runtime_error("Invalid UTC date format: " + date_str);
                    }
                }

                std::tm tm = {};
                std::istringstream ss(base);
                ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
                if (ss.fail()) {
                    throw std::runtime_error("Invalid UTC date format: " + date_str);
                }
                auto utc_time = timegm(&tm);
                if (utc_time == static_cast<std::time_t>(-1)) {
                    throw std::runtime_error("Failed to convert UTC date: " + date_str);
                }
                auto time_point = std::chrono::system_clock::from_time_t(utc_time) +
                    std::chrono::milliseconds(millis);
                telescope->set_utc_date(time_point);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "tracking") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_tracking());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                bool value = parse_bool("Tracking");
                telescope->set_tracking(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "declinationrate") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_declination_rate());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = parse_double("DeclinationRate");
                telescope->set_declination_rate(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "rightascensionrate") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_right_ascension_rate());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = parse_double("RightAscensionRate");
                telescope->set_right_ascension_rate(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "trackingrate") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_tracking_rate());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                int value = parse_int("TrackingRate");
                telescope->set_tracking_rate(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "targetdeclination") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_target_declination());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = parse_double("TargetDeclination");
                telescope->set_target_declination(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "targetrightascension") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_target_right_ascension());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = parse_double("TargetRightAscension");
                telescope->set_target_right_ascension(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "siteelevation") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_site_elevation());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = parse_double("SiteElevation");
                telescope->set_site_elevation(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "sitelatitude") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_site_latitude());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = parse_double("SiteLatitude");
                telescope->set_site_latitude(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "sitelongitude") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_site_longitude());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = parse_double("SiteLongitude");
                telescope->set_site_longitude(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "sideofpier") {
            if (request.method() == HttpMethod::GET) {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, telescope->get_side_of_pier());
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                int value = parse_int("SideOfPier");
                telescope->set_side_of_pier(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "guideratedeclination") {
            if (request.method() == HttpMethod::GET) {
                auto guide_rate = telescope->get_guide_rate();
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, guide_rate.dec);
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = parse_double("GuideRateDeclination");
                auto guide_rate = telescope->get_guide_rate();
                guide_rate.dec = value;
                telescope->set_guide_rate(guide_rate);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        else if (method_name == "guideraterightascension") {
            if (request.method() == HttpMethod::GET) {
                auto guide_rate = telescope->get_guide_rate();
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, guide_rate.ra);
                response.set_body(alpaca_response);
                return response;
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = parse_double("GuideRateRightAscension");
                auto guide_rate = telescope->get_guide_rate();
                guide_rate.ra = value;
                telescope->set_guide_rate(guide_rate);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        
        // PUT-only methods (actions)
        if (request.method() == HttpMethod::PUT) {
            if (method_name == "abortslew") {
                telescope->abort_slew();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "moveaxis") {
                // Debug logging
                if (!request.body().empty()) {
                    util::log_info("moveaxis body: " + request.body());
                }
                int axis = parse_int("Axis");
                double rate = parse_double("Rate");
                util::log_info("moveaxis parsed: Axis=" + std::to_string(axis) + ", Rate=" + std::to_string(rate));
                telescope->move_axis(axis, rate);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "slewtoaltazasync") {
                double altitude = parse_double("Altitude");
                double azimuth = parse_double("Azimuth");
                telescope->slew_to_alt_az_async(altitude, azimuth);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "slewtoaltaz") {
                double altitude = parse_double("Altitude");
                double azimuth = parse_double("Azimuth");
                telescope->slew_to_alt_az(altitude, azimuth);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "synctoaltaz") {
                double altitude = parse_double("Altitude");
                double azimuth = parse_double("Azimuth");
                telescope->sync_to_alt_az(altitude, azimuth);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "park") {
                telescope->park();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "unpark") {
                telescope->unpark();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "findhome") {
                telescope->find_home();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "setpark") {
                telescope->set_park();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "slewtotarget") {
                telescope->slew_to_target();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "slewtotargetasync") {
                telescope->slew_to_target_async();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "slewtocoordinates") {
                double ra = parse_double("RightAscension");
                double dec = parse_double("Declination");
                telescope->set_target_right_ascension(ra);
                telescope->set_target_declination(dec);
                telescope->slew_to_coordinates(ra, dec);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "slewtocoordinatesasync") {
                double ra = parse_double("RightAscension");
                double dec = parse_double("Declination");
                // slew_to_coordinates_async sets targets internally; skip
                // redundant set_target calls to avoid extra mutex round-trips.
                telescope->slew_to_coordinates_async(ra, dec);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "synctotarget") {
                telescope->sync_to_target();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "synctocoordinates") {
                double ra = parse_double("RightAscension");
                double dec = parse_double("Declination");
                telescope->sync_to_coordinates(ra, dec);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
            else if (method_name == "pulseguide") {
                int direction = parse_int("Direction");
                int duration = parse_int("Duration");
                telescope->pulse_guide(direction, duration);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }
        
        // Method not found
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::NOT_IMPLEMENTED,
            "Method '" + method_name + "' not yet implemented for Telescope"
        );
        response.set_body(alpaca_response);
        return response;
        
    } catch (const alpacacore::AlpacaException& e) {
        log_alpaca_exception("AlpacaException in telescope method '" + method_name + "'", e);
        auto error_code = util::map_error_code(e.error_code());
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            std::string(e.what())
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
        return response;
    } catch (const std::exception& e) {
        util::log_error("Exception in telescope method '" + method_name + "': " + std::string(e.what()));
        auto error_code = util::exception_to_error_code(e);
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            util::exception_to_error_message(e)
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
        return response;
    }
}

Response Router::dispatch_camera_method(
    std::shared_ptr<alpacacore::CameraDriver> camera,
    const std::string& method_name,
    const Request& request,
    std::uint32_t client_tx_id,
    std::uint32_t server_tx_id) {

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

    Response response;
    auto parse_double = [&](const std::string& param_name) -> double {
        if (request.has_query_param(param_name)) {
            return parse_double_value(request.get_query_param(param_name), param_name);
        }
        auto json_opt = parse_json(request.body());
        if (json_opt) {
            if (const auto* val = find_json_value(*json_opt, param_name)) {
                if (val->is_number()) {
                    return val->get<double>();
                }
                if (val->is_string()) {
                    return parse_double_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
            if (const auto* val = find_json_value(*json_opt, "Value")) {
                if (val->is_number()) {
                    return val->get<double>();
                }
                if (val->is_string()) {
                    return parse_double_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
        }
        if (auto value = get_form_value(request.body(), param_name)) {
            return parse_double_value(*value, param_name);
        }
        if (auto value = get_form_value(request.body(), "Value")) {
            return parse_double_value(*value, param_name);
        }
        throw std::runtime_error("Missing parameter: " + param_name);
    };

    auto parse_int = [&](const std::string& param_name) -> int {
        if (request.has_query_param(param_name)) {
            return parse_int_value(request.get_query_param(param_name), param_name);
        }
        auto json_opt = parse_json(request.body());
        if (json_opt) {
            if (const auto* val = find_json_value(*json_opt, param_name)) {
                if (val->is_number_integer()) {
                    return val->get<int>();
                }
                if (val->is_number()) {
                    return static_cast<int>(val->get<double>());
                }
                if (val->is_string()) {
                    return parse_int_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
            if (const auto* val = find_json_value(*json_opt, "Value")) {
                if (val->is_number_integer()) {
                    return val->get<int>();
                }
                if (val->is_number()) {
                    return static_cast<int>(val->get<double>());
                }
                if (val->is_string()) {
                    return parse_int_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
        }
        if (auto value = get_form_value(request.body(), param_name)) {
            return parse_int_value(*value, param_name);
        }
        if (auto value = get_form_value(request.body(), "Value")) {
            return parse_int_value(*value, param_name);
        }
        throw std::runtime_error("Missing parameter: " + param_name);
    };

    auto parse_bool = [&](const std::string& param_name) -> bool {
        if (request.has_query_param(param_name)) {
            return parse_bool_value(request.get_query_param(param_name), param_name);
        }
        auto json_opt = parse_json(request.body());
        if (json_opt) {
            if (const auto* val = find_json_value(*json_opt, param_name)) {
                if (val->is_boolean()) {
                    return val->get<bool>();
                }
                if (val->is_string()) {
                    return parse_bool_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
            if (const auto* val = find_json_value(*json_opt, "Value")) {
                if (val->is_boolean()) {
                    return val->get<bool>();
                }
                if (val->is_string()) {
                    return parse_bool_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
        }
        if (auto value = get_form_value(request.body(), param_name)) {
            return parse_bool_value(*value, param_name);
        }
        if (auto value = get_form_value(request.body(), "Value")) {
            return parse_bool_value(*value, param_name);
        }
        throw std::runtime_error("Missing parameter: " + param_name);
    };

    auto parse_string = [&](const std::string& param_name) -> std::string {
        if (request.has_query_param(param_name)) {
            return request.get_query_param(param_name);
        }
        if (auto query_value = get_query_param_case_insensitive(request, param_name)) {
            return *query_value;
        }
        auto json_opt = parse_json(request.body());
        if (json_opt) {
            if (const auto* val = find_json_value(*json_opt, param_name)) {
                if (!val->is_string()) {
                    throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
                }
                return val->get<std::string>();
            }
            if (const auto* val = find_json_value(*json_opt, "Value")) {
                if (!val->is_string()) {
                    throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
                }
                return val->get<std::string>();
            }
        }
        if (auto value = get_form_value(request.body(), param_name)) {
            return *value;
        }
        if (auto value = get_form_value(request.body(), "Value")) {
            return *value;
        }
        throw std::runtime_error("Missing parameter: " + param_name);
    };

    auto format_utc = [](std::chrono::system_clock::time_point time_point) -> std::string {
        auto time_t = std::chrono::system_clock::to_time_t(time_point);
        std::ostringstream oss;
        oss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%S");
        return oss.str();
    };

    try {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "bayeroffsetx") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_bayer_offset_x());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "bayeroffsety") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_bayer_offset_y());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "binx") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_bin_x());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "biny") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_bin_y());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "camerastate") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, static_cast<int>(camera->get_camera_state()));
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "cameraxsize") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_camera_x_size());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "cameraysize") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_camera_y_size());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "canabortexposure") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_can_abort_exposure());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "canasymmetricbin") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_can_asymmetric_bin());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "canfastreadout") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_can_fast_readout());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "cangetcoolerpower") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_can_get_cooler_power());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "canpulseguide") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_can_pulse_guide());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "cansetccdtemperature") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_can_set_ccd_temperature());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "canstopexposure") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_can_stop_exposure());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "ccdtemperature") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_ccd_temperature());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "cooleron") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_cooler_on());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "coolerpower") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_cooler_power());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "electronsperadu") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_electrons_per_adu());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "exposuremax") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_exposure_max());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "exposuremin") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_exposure_min());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "exposureresolution") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_exposure_resolution());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "fastreadout") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_fast_readout());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "fullwellcapacity") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_full_well_capacity());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "gain") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_gain());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "gainmax") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_gain_max());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "gainmin") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_gain_min());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "gains") {
                nlohmann::json gains = nlohmann::json::array();
                for (const auto& gain : camera->get_gains()) {
                    gains.push_back(gain);
                }
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, gains.dump());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "hasshutter") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_has_shutter());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "heatsinktemperature") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_heat_sink_temperature());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "imagearray") {
                util::log_debug("Camera imagearray Accept: " +
                    (request.has_header("accept") ? request.get_header("accept") : "<none>") +
                    ", imagebytes=" + std::string(accepts_imagebytes(request) ? "true" : "false"));
                if (accepts_imagebytes(request)) {
                    auto image = camera->get_image_array();
                    auto format = choose_image_bytes_format(image, kImageTypeInt32);
                    response.set_content_type("application/imagebytes");
                    response.set_body(build_image_bytes_payload(image, format, client_tx_id, server_tx_id));
                    return response;
                }

                auto image = camera->get_image_array();
                response.set_content_type("application/json");
                response.set_body(build_image_array_payload(image, 2, client_tx_id, server_tx_id));
                return response;
            } else if (method_name == "imagearrayvariant") {
                util::log_debug("Camera imagearrayvariant Accept: " +
                    (request.has_header("accept") ? request.get_header("accept") : "<none>") +
                    ", imagebytes=" + std::string(accepts_imagebytes(request) ? "true" : "false"));
                if (accepts_imagebytes(request)) {
                    auto image = camera->get_image_array();
                    auto variant = camera->get_image_array_variant();
                    auto element_type = image_element_type_from_variant(variant, kImageTypeInt32);
                    auto format = choose_image_bytes_format(image, element_type);
                    response.set_content_type("application/imagebytes");
                    response.set_body(build_image_bytes_payload(image, format, client_tx_id, server_tx_id));
                    return response;
                }

                auto image = camera->get_image_array();
                auto variant = camera->get_image_array_variant();
                int type = 2;
                if (variant == "Int16" || variant == "UInt16" || variant == "Short") {
                    type = 1;
                } else if (variant == "Double") {
                    type = 3;
                }
                response.set_content_type("application/json");
                response.set_body(build_image_array_payload(image, type, client_tx_id, server_tx_id));
                return response;
            } else if (method_name == "imageready") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_image_ready());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "ispulseguiding") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_is_pulse_guiding());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "lastexposureduration") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_last_exposure_duration());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "lastexposurestarttime") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, format_utc(camera->get_last_exposure_start_time()));
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "maxadu") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_max_adu());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "maxbinx") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_max_bin_x());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "maxbiny") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_max_bin_y());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "numx") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_num_x());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "numy") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_num_y());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "offset") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_offset());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "offsetmax") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_offset_max());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "offsetmin") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_offset_min());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "offsets") {
                nlohmann::json offsets = nlohmann::json::array();
                for (const auto& offset : camera->get_offsets()) {
                    offsets.push_back(offset);
                }
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, offsets.dump());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "percentcompleted") {
                auto percent = camera->get_percent_completed();
                auto value = static_cast<std::int32_t>(percent);
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, value);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "pixelsizex") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_pixel_size_x());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "pixelsizey") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_pixel_size_y());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "readoutmode") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_readout_mode());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "readoutmodes") {
                nlohmann::json modes = nlohmann::json::array();
                for (const auto& mode : camera->get_readout_modes()) {
                    modes.push_back(mode);
                }
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, modes.dump());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "sensorname") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_sensor_name());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "sensortype") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, static_cast<int>(camera->get_sensor_type()));
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "setccdtemperature") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_set_ccd_temperature());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "startx") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_start_x());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "starty") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_start_y());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "subexposureduration") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, camera->get_sub_exposure_duration());
                response.set_body(alpaca_response);
                return response;
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "binx") {
                int value = parse_int("BinX");
                camera->set_bin_x(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "biny") {
                int value = parse_int("BinY");
                camera->set_bin_y(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "fastreadout") {
                bool value = parse_bool("FastReadout");
                camera->set_fast_readout(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "gain") {
                int value = parse_int("Gain");
                camera->set_gain(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "cooleron") {
                bool value = parse_bool("CoolerOn");
                camera->set_cooler_on(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "setccdtemperature") {
                double value = parse_double("SetCCDTemperature");
                camera->set_set_ccd_temperature(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "numx") {
                int value = parse_int("NumX");
                camera->set_num_x(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "numy") {
                int value = parse_int("NumY");
                camera->set_num_y(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "offset") {
                int value = parse_int("Offset");
                camera->set_offset(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "readoutmode") {
                int value = parse_int("ReadoutMode");
                camera->set_readout_mode(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "startx") {
                int value = parse_int("StartX");
                camera->set_start_x(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "starty") {
                int value = parse_int("StartY");
                camera->set_start_y(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "subexposureduration") {
                double value = parse_double("SubExposureDuration");
                camera->set_sub_exposure_duration(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "startexposure") {
                double duration = parse_double("Duration");
                bool light = parse_bool("Light");
                camera->start_exposure(duration, light);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "pulseguide") {
                int direction = parse_int("Direction");
                int duration = parse_int("Duration");
                camera->pulse_guide(direction, duration);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "stopexposure") {
                camera->stop_exposure();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "abortexposure") {
                camera->abort_exposure();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }

        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::NOT_IMPLEMENTED,
            "Method '" + method_name + "' not yet implemented for Camera"
        );
        response.set_body(alpaca_response);
        return response;
    } catch (const alpacacore::AlpacaException& e) {
        log_alpaca_exception("AlpacaException in camera method '" + method_name + "'", e);
        auto error_code = util::map_error_code(e.error_code());
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            std::string(e.what())
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
        return response;
    } catch (const std::exception& e) {
        util::log_error("Exception in camera method '" + method_name + "': " + std::string(e.what()));
        auto error_code = util::exception_to_error_code(e);
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            util::exception_to_error_message(e)
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
        return response;
    }
}

Response Router::dispatch_switch_method(
    std::shared_ptr<alpacacore::SwitchDriver> sw,
    const std::string& method_name,
    const Request& request,
    std::uint32_t client_tx_id,
    std::uint32_t server_tx_id) {
    
    Response response;

    auto parse_double = [&](const std::string& param_name) -> double {
        if (request.has_query_param(param_name)) {
            return parse_double_value(request.get_query_param(param_name), param_name);
        }
        auto json_opt = parse_json(request.body());
        if (json_opt) {
            if (const auto* val = find_json_value(*json_opt, param_name)) {
                if (val->is_number()) {
                    return val->get<double>();
                }
                if (val->is_string()) {
                    return parse_double_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
            if (const auto* val = find_json_value(*json_opt, "Value")) {
                if (val->is_number()) {
                    return val->get<double>();
                }
                if (val->is_string()) {
                    return parse_double_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
        }
        if (auto value = get_form_value(request.body(), param_name)) {
            return parse_double_value(*value, param_name);
        }
        if (auto value = get_form_value(request.body(), "Value")) {
            return parse_double_value(*value, param_name);
        }
        throw std::runtime_error("Missing parameter: " + param_name);
    };

    auto parse_int = [&](const std::string& param_name) -> int {
        if (request.has_query_param(param_name)) {
            return parse_int_value(request.get_query_param(param_name), param_name);
        }
        auto json_opt = parse_json(request.body());
        if (json_opt) {
            if (const auto* val = find_json_value(*json_opt, param_name)) {
                if (val->is_number_integer()) {
                    return val->get<int>();
                }
                if (val->is_number()) {
                    return static_cast<int>(val->get<double>());
                }
                if (val->is_string()) {
                    return parse_int_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
            if (const auto* val = find_json_value(*json_opt, "Value")) {
                if (val->is_number_integer()) {
                    return val->get<int>();
                }
                if (val->is_number()) {
                    return static_cast<int>(val->get<double>());
                }
                if (val->is_string()) {
                    return parse_int_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
        }
        if (auto value = get_form_value(request.body(), param_name)) {
            return parse_int_value(*value, param_name);
        }
        if (auto value = get_form_value(request.body(), "Value")) {
            return parse_int_value(*value, param_name);
        }
        throw std::runtime_error("Missing parameter: " + param_name);
    };

    auto parse_bool = [&](const std::string& param_name) -> bool {
        if (request.has_query_param(param_name)) {
            return parse_bool_value(request.get_query_param(param_name), param_name);
        }
        auto json_opt = parse_json(request.body());
        if (json_opt) {
            if (const auto* val = find_json_value(*json_opt, param_name)) {
                if (val->is_boolean()) {
                    return val->get<bool>();
                }
                if (val->is_string()) {
                    return parse_bool_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
            if (const auto* val = find_json_value(*json_opt, "Value")) {
                if (val->is_boolean()) {
                    return val->get<bool>();
                }
                if (val->is_string()) {
                    return parse_bool_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
        }
        if (auto value = get_form_value(request.body(), param_name)) {
            return parse_bool_value(*value, param_name);
        }
        if (auto value = get_form_value(request.body(), "Value")) {
            return parse_bool_value(*value, param_name);
        }
        throw std::runtime_error("Missing parameter: " + param_name);
    };

    auto parse_string = [&](const std::string& param_name) -> std::string {
        if (request.has_query_param(param_name)) {
            return request.get_query_param(param_name);
        }
        auto json_opt = parse_json(request.body());
        if (json_opt) {
            if (const auto* val = find_json_value(*json_opt, param_name)) {
                if (!val->is_string()) {
                    throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
                }
                return val->get<std::string>();
            }
            if (const auto* val = find_json_value(*json_opt, "Value")) {
                if (!val->is_string()) {
                    throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
                }
                return val->get<std::string>();
            }
        }
        if (auto value = get_form_value(request.body(), param_name)) {
            return *value;
        }
        if (auto value = get_form_value(request.body(), "Value")) {
            return *value;
        }
        throw std::runtime_error("Missing parameter: " + param_name);
    };

    try {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "maxswitch") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, sw->get_max_switch());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "cancelasync") {
                int id = parse_int("Id");
                if (!sw->get_can_async(id)) {
                    throw alpacacore::AlpacaException(
                        "Async switch control not supported",
                        alpacacore::AlpacaError::NotImplemented
                    );
                }
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "canasync") {
                int id = parse_int("Id");
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, sw->get_can_async(id));
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "canwrite") {
                int id = parse_int("Id");
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, sw->get_can_write(id));
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "getswitch") {
                int id = parse_int("Id");
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, sw->get_switch(id));
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "getswitchvalue") {
                int id = parse_int("Id");
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, sw->get_switch_value(id));
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "getswitchname") {
                int id = parse_int("Id");
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, sw->get_switch_name(id));
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "getswitchdescription") {
                int id = parse_int("Id");
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, sw->get_switch_description(id));
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "minswitchvalue") {
                int id = parse_int("Id");
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, sw->get_min_switch_value(id));
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "maxswitchvalue") {
                int id = parse_int("Id");
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, sw->get_max_switch_value(id));
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "statechangecomplete") {
                int id = parse_int("Id");
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, sw->get_state_change_complete(id));
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "switchstep") {
                int id = parse_int("Id");
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, sw->get_switch_step(id));
                response.set_body(alpaca_response);
                return response;
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "setswitch") {
                int id = parse_int("Id");
                bool state = parse_bool("State");
                sw->set_switch(id, state);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "cancelasync") {
                int id = parse_int("Id");
                if (!sw->get_can_async(id)) {
                    throw alpacacore::AlpacaException(
                        "Async switch control not supported",
                        alpacacore::AlpacaError::NotImplemented
                    );
                }
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "setasync") {
                int id = parse_int("Id");
                bool state = parse_bool("State");
                sw->set_async(id, state);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "setswitchvalue") {
                int id = parse_int("Id");
                double value = parse_double("Value");
                sw->set_switch_value(id, value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "setasyncvalue") {
                int id = parse_int("Id");
                double value = parse_double("Value");
                sw->set_async_value(id, value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "setswitchname") {
                int id = parse_int("Id");
                std::string name = parse_string("Name");
                sw->set_switch_name(id, name);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }

        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::NOT_IMPLEMENTED,
            "Method '" + method_name + "' not yet implemented for Switch"
        );
        response.set_body(alpaca_response);
        return response;
    } catch (const alpacacore::AlpacaException& e) {
        log_alpaca_exception("AlpacaException in switch method '" + method_name + "'", e);
        auto error_code = util::map_error_code(e.error_code());
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            std::string(e.what())
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
        return response;
    } catch (const std::exception& e) {
        util::log_error("Exception in switch method '" + method_name + "': " + std::string(e.what()));
        auto error_code = util::exception_to_error_code(e);
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            util::exception_to_error_message(e)
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
        return response;
    }
}

Response Router::dispatch_filterwheel_method(
    std::shared_ptr<alpacacore::FilterWheelDriver> filterwheel,
    const std::string& method_name,
    const Request& request,
    std::uint32_t client_tx_id,
    std::uint32_t server_tx_id) {
    
    Response response;

    auto parse_int = [&](const std::string& param_name) -> int {
        if (request.has_query_param(param_name)) {
            return parse_int_value(request.get_query_param(param_name), param_name);
        }
        auto json_opt = parse_json(request.body());
        if (json_opt) {
            if (const auto* val = find_json_value(*json_opt, param_name)) {
                if (val->is_number_integer()) {
                    return val->get<int>();
                }
                if (val->is_number()) {
                    return static_cast<int>(val->get<double>());
                }
                if (val->is_string()) {
                    return parse_int_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
            if (const auto* val = find_json_value(*json_opt, "Value")) {
                if (val->is_number_integer()) {
                    return val->get<int>();
                }
                if (val->is_number()) {
                    return static_cast<int>(val->get<double>());
                }
                if (val->is_string()) {
                    return parse_int_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
        }
        if (auto value = get_form_value(request.body(), param_name)) {
            return parse_int_value(*value, param_name);
        }
        if (auto value = get_form_value(request.body(), "Value")) {
            return parse_int_value(*value, param_name);
        }
        throw std::runtime_error("Missing parameter: " + param_name);
    };

    try {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "position") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, filterwheel->get_position());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "names") {
                nlohmann::json names = nlohmann::json::array();
                for (const auto& name : filterwheel->get_names()) {
                    names.push_back(name);
                }
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, names.dump());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "focusoffsets") {
                nlohmann::json offsets = nlohmann::json::array();
                for (int offset : filterwheel->get_focus_offsets()) {
                    offsets.push_back(offset);
                }
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, offsets.dump());
                response.set_body(alpaca_response);
                return response;
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "position") {
                int value = parse_int("Position");
                filterwheel->set_position(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "names") {
                auto json_opt = parse_json(request.body());
                const auto* names_val = json_opt ? find_json_value(*json_opt, "Names") : nullptr;
                if (!names_val) {
                    throw std::runtime_error("Missing parameter: Names");
                }
                std::vector<std::string> names = names_val->get<std::vector<std::string>>();
                filterwheel->set_names(names);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "focusoffsets") {
                auto json_opt = parse_json(request.body());
                const auto* offsets_val = json_opt ? find_json_value(*json_opt, "FocusOffsets") : nullptr;
                if (!offsets_val) {
                    throw std::runtime_error("Missing parameter: FocusOffsets");
                }
                std::vector<int> offsets = offsets_val->get<std::vector<int>>();
                filterwheel->set_focus_offsets(offsets);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }

        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::NOT_IMPLEMENTED,
            "Method '" + method_name + "' not yet implemented for FilterWheel"
        );
        response.set_body(alpaca_response);
        return response;
    } catch (const alpacacore::AlpacaException& e) {
        log_alpaca_exception("AlpacaException in filter wheel method '" + method_name + "'", e);
        auto error_code = util::map_error_code(e.error_code());
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            std::string(e.what())
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
        return response;
    } catch (const std::exception& e) {
        util::log_error("Exception in filter wheel method '" + method_name + "': " + std::string(e.what()));
        auto error_code = util::exception_to_error_code(e);
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            util::exception_to_error_message(e)
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
        return response;
    }
}

Response Router::dispatch_focuser_method(
    std::shared_ptr<alpacacore::FocuserDriver> focuser,
    const std::string& method_name,
    const Request& request,
    std::uint32_t client_tx_id,
    std::uint32_t server_tx_id) {
    
    Response response;

    auto parse_int = [&](const std::string& param_name) -> int {
        if (request.has_query_param(param_name)) {
            return parse_int_value(request.get_query_param(param_name), param_name);
        }
        auto json_opt = parse_json(request.body());
        if (json_opt) {
            if (const auto* val = find_json_value(*json_opt, param_name)) {
                if (val->is_number_integer()) {
                    return val->get<int>();
                }
                if (val->is_number()) {
                    return static_cast<int>(val->get<double>());
                }
                if (val->is_string()) {
                    return parse_int_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
            if (const auto* val = find_json_value(*json_opt, "Value")) {
                if (val->is_number_integer()) {
                    return val->get<int>();
                }
                if (val->is_number()) {
                    return static_cast<int>(val->get<double>());
                }
                if (val->is_string()) {
                    return parse_int_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
        }
        if (auto value = get_form_value(request.body(), param_name)) {
            return parse_int_value(*value, param_name);
        }
        if (auto value = get_form_value(request.body(), "Value")) {
            return parse_int_value(*value, param_name);
        }
        throw std::runtime_error("Missing parameter: " + param_name);
    };

    auto parse_bool = [&](const std::string& param_name) -> bool {
        if (request.has_query_param(param_name)) {
            return parse_bool_value(request.get_query_param(param_name), param_name);
        }
        auto json_opt = parse_json(request.body());
        if (json_opt) {
            if (const auto* val = find_json_value(*json_opt, param_name)) {
                if (val->is_boolean()) {
                    return val->get<bool>();
                }
                if (val->is_string()) {
                    return parse_bool_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
            if (const auto* val = find_json_value(*json_opt, "Value")) {
                if (val->is_boolean()) {
                    return val->get<bool>();
                }
                if (val->is_string()) {
                    return parse_bool_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
        }
        if (auto value = get_form_value(request.body(), param_name)) {
            return parse_bool_value(*value, param_name);
        }
        if (auto value = get_form_value(request.body(), "Value")) {
            return parse_bool_value(*value, param_name);
        }
        throw std::runtime_error("Missing parameter: " + param_name);
    };

    try {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "absolute") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, focuser->get_absolute());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "ismoving") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, focuser->get_is_moving());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "maxincrement") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, focuser->get_max_increment());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "maxstep") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, focuser->get_max_step());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "position") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, focuser->get_position());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "stepsize") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, focuser->get_step_size());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "tempcompavailable") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, focuser->get_temp_comp_available());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "tempcomp") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, focuser->get_temp_comp());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "temperature") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, focuser->get_temperature());
                response.set_body(alpaca_response);
                return response;
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "tempcomp") {
                bool value = parse_bool("TempComp");
                focuser->set_temp_comp(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "move") {
                int position = parse_int("Position");
                focuser->move(position);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "halt") {
                focuser->halt();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }

        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::NOT_IMPLEMENTED,
            "Method '" + method_name + "' not yet implemented for Focuser"
        );
        response.set_body(alpaca_response);
        return response;
    } catch (const alpacacore::AlpacaException& e) {
        log_alpaca_exception("AlpacaException in focuser method '" + method_name + "'", e);
        auto error_code = util::map_error_code(e.error_code());
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            std::string(e.what())
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
        return response;
    } catch (const std::exception& e) {
        util::log_error("Exception in focuser method '" + method_name + "': " + std::string(e.what()));
        auto error_code = util::exception_to_error_code(e);
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            util::exception_to_error_message(e)
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
        return response;
    }
}

Response Router::dispatch_rotator_method(
    std::shared_ptr<alpacacore::RotatorDriver> rotator,
    const std::string& method_name,
    const Request& request,
    std::uint32_t client_tx_id,
    std::uint32_t server_tx_id) {
    
    Response response;

    auto parse_double = [&](const std::string& param_name) -> double {
        if (request.has_query_param(param_name)) {
            return parse_double_value(request.get_query_param(param_name), param_name);
        }
        auto json_opt = parse_json(request.body());
        if (json_opt) {
            if (const auto* val = find_json_value(*json_opt, param_name)) {
                if (val->is_number()) {
                    return val->get<double>();
                }
                if (val->is_string()) {
                    return parse_double_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
            if (const auto* val = find_json_value(*json_opt, "Value")) {
                if (val->is_number()) {
                    return val->get<double>();
                }
                if (val->is_string()) {
                    return parse_double_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
        }
        if (auto value = get_form_value(request.body(), param_name)) {
            return parse_double_value(*value, param_name);
        }
        if (auto value = get_form_value(request.body(), "Value")) {
            return parse_double_value(*value, param_name);
        }
        throw std::runtime_error("Missing parameter: " + param_name);
    };

    auto parse_bool = [&](const std::string& param_name) -> bool {
        if (request.has_query_param(param_name)) {
            return parse_bool_value(request.get_query_param(param_name), param_name);
        }
        auto json_opt = parse_json(request.body());
        if (json_opt) {
            if (const auto* val = find_json_value(*json_opt, param_name)) {
                if (val->is_boolean()) {
                    return val->get<bool>();
                }
                if (val->is_string()) {
                    return parse_bool_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
            if (const auto* val = find_json_value(*json_opt, "Value")) {
                if (val->is_boolean()) {
                    return val->get<bool>();
                }
                if (val->is_string()) {
                    return parse_bool_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
        }
        if (auto value = get_form_value(request.body(), param_name)) {
            return parse_bool_value(*value, param_name);
        }
        if (auto value = get_form_value(request.body(), "Value")) {
            return parse_bool_value(*value, param_name);
        }
        throw std::runtime_error("Missing parameter: " + param_name);
    };

    try {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "canreverse") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, rotator->get_can_reverse());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "reverse") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, rotator->get_reverse());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "ismoving") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, rotator->get_is_moving());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "mechanicalposition") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, rotator->get_mechanical_position());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "position") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, rotator->get_position());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "stepsize") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, rotator->get_step_size());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "targetposition") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, rotator->get_target_position());
                response.set_body(alpaca_response);
                return response;
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "reverse") {
                bool value = parse_bool("Reverse");
                rotator->set_reverse(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "move") {
                double position = parse_double("Position");
                rotator->move(position);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "moveabsolute") {
                double position = parse_double("Position");
                rotator->move_absolute(position);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "movemechanical") {
                double position = parse_double("Position");
                rotator->move_mechanical(position);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "halt") {
                rotator->halt();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "sync") {
                double position = parse_double("Position");
                rotator->sync(position);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "targetposition") {
                double position = parse_double("TargetPosition");
                rotator->set_target_position(position);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }

        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::NOT_IMPLEMENTED,
            "Method '" + method_name + "' not yet implemented for Rotator"
        );
        response.set_body(alpaca_response);
        return response;
    } catch (const alpacacore::AlpacaException& e) {
        log_alpaca_exception("AlpacaException in rotator method '" + method_name + "'", e);
        auto error_code = util::map_error_code(e.error_code());
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            std::string(e.what())
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
        return response;
    } catch (const std::exception& e) {
        util::log_error("Exception in rotator method '" + method_name + "': " + std::string(e.what()));
        auto error_code = util::exception_to_error_code(e);
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            util::exception_to_error_message(e)
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
        return response;
    }
}

Response Router::dispatch_dome_method(
    std::shared_ptr<alpacacore::DomeDriver> dome,
    const std::string& method_name,
    const Request& request,
    std::uint32_t client_tx_id,
    std::uint32_t server_tx_id) {
    
    Response response;

    auto parse_double = [&](const std::string& param_name) -> double {
        if (request.has_query_param(param_name)) {
            return parse_double_value(request.get_query_param(param_name), param_name);
        }
        auto json_opt = parse_json(request.body());
        if (json_opt) {
            if (const auto* val = find_json_value(*json_opt, param_name)) {
                if (val->is_number()) {
                    return val->get<double>();
                }
                if (val->is_string()) {
                    return parse_double_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
            if (const auto* val = find_json_value(*json_opt, "Value")) {
                if (val->is_number()) {
                    return val->get<double>();
                }
                if (val->is_string()) {
                    return parse_double_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
        }
        if (auto value = get_form_value(request.body(), param_name)) {
            return parse_double_value(*value, param_name);
        }
        if (auto value = get_form_value(request.body(), "Value")) {
            return parse_double_value(*value, param_name);
        }
        throw std::runtime_error("Missing parameter: " + param_name);
    };

    auto parse_bool = [&](const std::string& param_name) -> bool {
        if (request.has_query_param(param_name)) {
            return parse_bool_value(request.get_query_param(param_name), param_name);
        }
        auto json_opt = parse_json(request.body());
        if (json_opt) {
            if (const auto* val = find_json_value(*json_opt, param_name)) {
                if (val->is_boolean()) {
                    return val->get<bool>();
                }
                if (val->is_string()) {
                    return parse_bool_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
            if (const auto* val = find_json_value(*json_opt, "Value")) {
                if (val->is_boolean()) {
                    return val->get<bool>();
                }
                if (val->is_string()) {
                    return parse_bool_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
        }
        if (auto value = get_form_value(request.body(), param_name)) {
            return parse_bool_value(*value, param_name);
        }
        if (auto value = get_form_value(request.body(), "Value")) {
            return parse_bool_value(*value, param_name);
        }
        throw std::runtime_error("Missing parameter: " + param_name);
    };

    try {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "altitude") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, dome->get_altitude());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "athome") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, dome->get_at_home());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "atpark") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, dome->get_at_park());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "azimuth") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, dome->get_azimuth());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "canfindhome") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, dome->get_can_find_home());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "canpark") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, dome->get_can_park());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "cansetaltitude") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, dome->get_can_set_altitude());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "cansetazimuth") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, dome->get_can_set_azimuth());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "cansetpark") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, dome->get_can_set_park());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "cansetshutter") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, dome->get_can_set_shutter());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "canslew") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, dome->get_can_slew());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "cansyncazimuth") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, dome->get_can_sync_azimuth());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "canslave") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, dome->get_can_slave());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "slaved") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, dome->get_slaved());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "slewing") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, dome->get_slewing());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "shutterstatus") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, dome->get_shutter_status());
                response.set_body(alpaca_response);
                return response;
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "slaved") {
                bool value = parse_bool("Slaved");
                dome->set_slaved(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "abortslew") {
                dome->abort_slew();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "closeshutter") {
                dome->close_shutter();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "findhome") {
                dome->find_home();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "openshutter") {
                dome->open_shutter();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "park") {
                dome->park();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "setpark") {
                dome->set_park();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "slewtoazimuth") {
                double azimuth = parse_double("Azimuth");
                dome->slew_to_azimuth(azimuth);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "slewtoaltitude") {
                double altitude = parse_double("Altitude");
                dome->slew_to_altitude(altitude);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "synctoazimuth") {
                double azimuth = parse_double("Azimuth");
                dome->sync_to_azimuth(azimuth);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }

        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::NOT_IMPLEMENTED,
            "Method '" + method_name + "' not yet implemented for Dome"
        );
        response.set_body(alpaca_response);
        return response;
    } catch (const alpacacore::AlpacaException& e) {
        log_alpaca_exception("AlpacaException in dome method '" + method_name + "'", e);
        auto error_code = util::map_error_code(e.error_code());
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            std::string(e.what())
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
        return response;
    } catch (const std::exception& e) {
        util::log_error("Exception in dome method '" + method_name + "': " + std::string(e.what()));
        auto error_code = util::exception_to_error_code(e);
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            util::exception_to_error_message(e)
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
        return response;
    }
}

Response Router::dispatch_shutter_method(
    std::shared_ptr<alpacacore::ShutterDriver> shutter,
    const std::string& method_name,
    const Request& request,
    std::uint32_t client_tx_id,
    std::uint32_t server_tx_id) {
    
    Response response;

    try {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "shutterstate") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, static_cast<int>(shutter->get_shutter_state()));
                response.set_body(alpaca_response);
                return response;
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "open") {
                shutter->open();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "close") {
                shutter->close();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }

        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::NOT_IMPLEMENTED,
            "Method '" + method_name + "' not yet implemented for Shutter"
        );
        response.set_body(alpaca_response);
        return response;
    } catch (const alpacacore::AlpacaException& e) {
        log_alpaca_exception("AlpacaException in shutter method '" + method_name + "'", e);
        auto error_code = util::map_error_code(e.error_code());
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            std::string(e.what())
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
        return response;
    } catch (const std::exception& e) {
        util::log_error("Exception in shutter method '" + method_name + "': " + std::string(e.what()));
        auto error_code = util::exception_to_error_code(e);
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            util::exception_to_error_message(e)
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
        return response;
    }
}

Response Router::dispatch_covercalibrator_method(
    std::shared_ptr<alpacacore::CoverCalibratorDriver> covercalibrator,
    const std::string& method_name,
    const Request& request,
    std::uint32_t client_tx_id,
    std::uint32_t server_tx_id) {
    
    Response response;

    auto parse_int = [&](const std::string& param_name) -> int {
        if (request.has_query_param(param_name)) {
            return parse_int_value(request.get_query_param(param_name), param_name);
        }
        auto json_opt = parse_json(request.body());
        if (json_opt) {
            if (const auto* val = find_json_value(*json_opt, param_name)) {
                if (val->is_number_integer()) {
                    return val->get<int>();
                }
                if (val->is_number()) {
                    return static_cast<int>(val->get<double>());
                }
                if (val->is_string()) {
                    return parse_int_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
            if (const auto* val = find_json_value(*json_opt, "Value")) {
                if (val->is_number_integer()) {
                    return val->get<int>();
                }
                if (val->is_number()) {
                    return static_cast<int>(val->get<double>());
                }
                if (val->is_string()) {
                    return parse_int_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
        }
        if (auto value = get_form_value(request.body(), param_name)) {
            return parse_int_value(*value, param_name);
        }
        if (auto value = get_form_value(request.body(), "Value")) {
            return parse_int_value(*value, param_name);
        }
        throw std::runtime_error("Missing parameter: " + param_name);
    };

    try {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "brightness") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, covercalibrator->get_brightness());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "calibratorchanging") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, covercalibrator->get_calibrator_changing());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "calibratorstate") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, static_cast<int>(covercalibrator->get_calibrator_state()));
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "covermoving") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, covercalibrator->get_cover_moving());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "coverstate") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, static_cast<int>(covercalibrator->get_cover_state()));
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "maxbrightness") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, covercalibrator->get_max_brightness());
                response.set_body(alpaca_response);
                return response;
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "calibratoron") {
                int brightness = parse_int("Brightness");
                covercalibrator->calibrator_on(brightness);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "calibratoroff") {
                covercalibrator->calibrator_off();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "opencover") {
                covercalibrator->open_cover();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "closecover") {
                covercalibrator->close_cover();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "haltcover") {
                covercalibrator->halt_cover();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }

        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::NOT_IMPLEMENTED,
            "Method '" + method_name + "' not yet implemented for CoverCalibrator"
        );
        response.set_body(alpaca_response);
        return response;
    } catch (const alpacacore::AlpacaException& e) {
        log_alpaca_exception("AlpacaException in cover calibrator method '" + method_name + "'", e);
        auto error_code = util::map_error_code(e.error_code());
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            std::string(e.what())
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
        return response;
    } catch (const std::exception& e) {
        util::log_error("Exception in cover calibrator method '" + method_name + "': " + std::string(e.what()));
        auto error_code = util::exception_to_error_code(e);
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            util::exception_to_error_message(e)
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
        return response;
    }
}

Response Router::dispatch_observingconditions_method(
    std::shared_ptr<alpacacore::ObservingConditionsDriver> observingconditions,
    const std::string& method_name,
    const Request& request,
    std::uint32_t client_tx_id,
    std::uint32_t server_tx_id) {
    
    Response response;

    auto parse_double = [&](const std::string& param_name) -> double {
        if (request.has_query_param(param_name)) {
            return parse_double_value(request.get_query_param(param_name), param_name);
        }
        auto json_opt = parse_json(request.body());
        if (json_opt) {
            if (const auto* val = find_json_value(*json_opt, param_name)) {
                if (val->is_number()) {
                    return val->get<double>();
                }
                if (val->is_string()) {
                    return parse_double_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
            if (const auto* val = find_json_value(*json_opt, "Value")) {
                if (val->is_number()) {
                    return val->get<double>();
                }
                if (val->is_string()) {
                    return parse_double_value(val->get<std::string>(), param_name);
                }
                throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
            }
        }
        if (auto value = get_form_value(request.body(), param_name)) {
            return parse_double_value(*value, param_name);
        }
        if (auto value = get_form_value(request.body(), "Value")) {
            return parse_double_value(*value, param_name);
        }
        throw std::runtime_error("Missing parameter: " + param_name);
    };

    auto parse_string = [&](const std::string& param_name) -> std::string {
        if (request.has_query_param(param_name)) {
            return request.get_query_param(param_name);
        }
        auto json_opt = parse_json(request.body());
        if (json_opt) {
            if (const auto* val = find_json_value(*json_opt, param_name)) {
                if (!val->is_string()) {
                    throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
                }
                return val->get<std::string>();
            }
            if (const auto* val = find_json_value(*json_opt, "Value")) {
                if (!val->is_string()) {
                    throw std::runtime_error("Invalid JSON value for parameter: " + param_name);
                }
                return val->get<std::string>();
            }
        }
        if (auto value = get_form_value(request.body(), param_name)) {
            return *value;
        }
        if (auto value = get_form_value(request.body(), "Value")) {
            return *value;
        }
        throw std::runtime_error("Missing parameter: " + param_name);
    };

    auto parse_property_name = [&]() -> std::string {
        if (auto value = get_query_param_case_insensitive(request, "PropertyName")) {
            return *value;
        }
        if (auto value = get_query_param_case_insensitive(request, "SensorName")) {
            return *value;
        }
        auto json_opt = parse_json(request.body());
        if (json_opt) {
            if (const auto* val = find_json_value(*json_opt, "PropertyName")) {
                if (!val->is_string()) {
                    throw std::runtime_error("Invalid JSON value for parameter: PropertyName");
                }
                return val->get<std::string>();
            }
            if (const auto* val = find_json_value(*json_opt, "SensorName")) {
                if (!val->is_string()) {
                    throw std::runtime_error("Invalid JSON value for parameter: SensorName");
                }
                return val->get<std::string>();
            }
        }
        if (auto value = get_form_value(request.body(), "PropertyName")) {
            return *value;
        }
        if (auto value = get_form_value(request.body(), "SensorName")) {
            return *value;
        }
        throw std::runtime_error("Missing parameter: PropertyName");
    };

    try {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "averageperiod") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, observingconditions->get_average_period());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "cloudcover") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, observingconditions->get_cloud_cover());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "dewpoint") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, observingconditions->get_dew_point());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "humidity") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, observingconditions->get_humidity());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "pressure") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, observingconditions->get_pressure());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "rainrate") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, observingconditions->get_rain_rate());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "skybrightness") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, observingconditions->get_sky_brightness());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "skyquality") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, observingconditions->get_sky_quality());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "skytemperature") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, observingconditions->get_sky_temperature());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "seeing") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, observingconditions->get_seeing());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "starfwhm") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, observingconditions->get_star_fwhm());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "temperature") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, observingconditions->get_temperature());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "winddirection") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, observingconditions->get_wind_direction());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "windgust") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, observingconditions->get_wind_gust());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "windspeed") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, observingconditions->get_wind_speed());
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "timesincelastupdate") {
                std::string property_name = parse_property_name();
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, observingconditions->get_time_since_last_update(property_name));
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "sensordescription") {
                std::string property_name = parse_property_name();
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, observingconditions->get_sensor_description(property_name));
                response.set_body(alpaca_response);
                return response;
            }
        }

        if (request.method() == HttpMethod::PUT) {
            if (method_name == "averageperiod") {
                double value = parse_double("AveragePeriod");
                observingconditions->set_average_period(value);
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            } else if (method_name == "refresh") {
                observingconditions->refresh();
                AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
                response.set_body(alpaca_response);
                return response;
            }
        }

        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::NOT_IMPLEMENTED,
            "Method '" + method_name + "' not yet implemented for ObservingConditions"
        );
        response.set_body(alpaca_response);
        return response;
    } catch (const alpacacore::AlpacaException& e) {
        log_alpaca_exception("AlpacaException in observing conditions method '" + method_name + "'", e);
        auto error_code = util::map_error_code(e.error_code());
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            std::string(e.what())
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
        return response;
    } catch (const std::exception& e) {
        util::log_error("Exception in observing conditions method '" + method_name + "': " + std::string(e.what()));
        auto error_code = util::exception_to_error_code(e);
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            util::exception_to_error_message(e)
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
        return response;
    }
}

Response Router::dispatch_safetymonitor_method(
    std::shared_ptr<alpacacore::SafetyMonitorDriver> safetymonitor,
    const std::string& method_name,
    const Request& request,
    std::uint32_t client_tx_id,
    std::uint32_t server_tx_id) {
    
    Response response;

    try {
        if (request.method() == HttpMethod::GET) {
            if (method_name == "issafe") {
                AlpacaResponse alpaca_response = make_success_response(
                    client_tx_id, server_tx_id, safetymonitor->get_is_safe());
                response.set_body(alpaca_response);
                return response;
            }
        }

        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::NOT_IMPLEMENTED,
            "Method '" + method_name + "' not yet implemented for SafetyMonitor"
        );
        response.set_body(alpaca_response);
        return response;
    } catch (const alpacacore::AlpacaException& e) {
        log_alpaca_exception("AlpacaException in safety monitor method '" + method_name + "'", e);
        auto error_code = util::map_error_code(e.error_code());
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            std::string(e.what())
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
        return response;
    } catch (const std::exception& e) {
        util::log_error("Exception in safety monitor method '" + method_name + "': " + std::string(e.what()));
        auto error_code = util::exception_to_error_code(e);
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            error_code,
            util::exception_to_error_message(e)
        );
        apply_error_status(response, error_code);
        response.set_body(alpaca_response);
        return response;
    }
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
    alpaca_response.value = info.dump();
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

    try {
        // Alpaca API versions endpoint returns array of supported API versions
        // Currently only version 1 is defined
        nlohmann::json versions = nlohmann::json::array({1});

        AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
        alpaca_response.value = versions.dump();
        response.set_body(alpaca_response);
        
        util::log_info("API versions response: " + versions.dump());
    } catch (const std::exception& e) {
        util::log_error("Error getting API versions: " + std::string(e.what()));
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::exception_to_error_code(e),
            util::exception_to_error_message(e)
        );
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
    if (file_path.find("web/") != 0 && file_path != "web/index.html") {
        response.set_status(403, "Forbidden");
        response.set_body("Access denied");
        return response;
    }
    
    // Extract filename from path (e.g., web/index.html -> index.html)
    std::string filename = file_path;
    size_t web_pos = file_path.find("web/");
    if (web_pos != std::string::npos) {
        filename = file_path.substr(web_pos + 4); // Skip "web/" (4 characters)
    }
    
    // Try multiple possible locations for web files
    std::vector<std::string> possible_paths = {
        "web/" + filename,                    // Current directory
        "../web/" + filename,                 // Parent directory (if running from build/)
        "../../web/" + filename,              // Two levels up
        "../AlpacaHTTP/web/" + filename,     // From build directory
        "AlpacaHTTP/web/" + filename         // Alternative location
    };
    
    std::ifstream file;
    std::string full_path;
    bool found = false;
    
    for (const auto& path : possible_paths) {
        file.open(path, std::ios::binary);
        if (file.is_open()) {
            full_path = path;
            found = true;
            break;
        }
        file.close();
    }
    
    if (!found) {
        response.set_status(404, "Not Found");
        std::string error_msg = "File not found: " + file_path + "\n";
        error_msg += "Searched in:\n";
        for (const auto& path : possible_paths) {
            error_msg += "  - " + path + "\n";
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

    util::log_info("Handling setup endpoint: " + request.path());

    // Setup endpoints are expected to return an HTML page.
    // We provide a simple stub page that points users to the web UI.
    // Example path: /setup/v1/telescope/0/setup
    std::regex setup_regex(R"(/setup/v1/([^/]+)/(\d+)/setup)");
    std::smatch matches;

    if (!std::regex_match(request.path(), matches, setup_regex)) {
        util::log_warning("Setup endpoint regex did not match: " + request.path());
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

Response Router::handle_configure_device(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    response.set_content_type("application/json");
    
    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
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
        
        std::string vendor = config.value("vendor", "");
        std::string device_type_str = config.value("deviceType", "");

        if (vendor.empty() || device_type_str.empty()) {
            AlpacaResponse alpaca_response = make_error_response(
                client_tx_id, server_tx_id,
                util::ErrorCode::INVALID_VALUE,
                "Missing required fields: deviceType and vendor"
            );
            response.set_body(alpaca_response);
            return response;
        }

        std::string error_message;
        if (!register_device_from_config(config, error_message)) {
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
            return response;
        }

        add_or_replace_persisted_device(sanitize_device_config(config));
        save_persisted_devices();

        AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
        alpaca_response.value = std::string("Device registered successfully");
        response.set_body(alpaca_response);
        return response;
    } catch (const std::exception& e) {
        util::log_error("Error configuring device: " + std::string(e.what()));
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::exception_to_error_code(e),
            util::exception_to_error_message(e)
        );
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
    
    // Only allow POST or PUT requests
    if (request.method() != HttpMethod::POST && request.method() != HttpMethod::PUT) {
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::INVALID_VALUE,
            "Method not allowed. Use POST or PUT."
        );
        response.set_body(alpaca_response);
        response.set_status(405, "Method Not Allowed");
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
        
        std::string device_type_str = config.value("deviceType", "");
        std::string vendor = config.value("vendor", "");
        int device_number = config.value("deviceNumber", -1);
        
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
        
        if (registry.unregister_device(device_type, device_number)) {
            AlpacaResponse alpaca_response(client_tx_id, server_tx_id);
            alpaca_response.value = std::string("Device removed successfully");
            response.set_body(alpaca_response);
            util::log_info("Removed device: " + device_type_str + " #" + std::to_string(device_number));
            remove_persisted_device(vendor, device_type_str, device_number);
            save_persisted_devices();
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
        util::log_error("Error removing device: " + std::string(e.what()));
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::exception_to_error_code(e),
            util::exception_to_error_message(e)
        );
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

    auto send_payload = [&](std::uint32_t ctx_id) {
        AlpacaResponse alpaca_response(ctx_id, server_tx_id);
        alpaca_response.value = make_log_level_payload().dump();
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

    const std::string logs = util::get_log_history_text();
    std::string format;
    if (request.has_query_param("format")) {
        format = request.get_query_param("format");
        std::transform(format.begin(), format.end(), format.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }

    if (format == "plain" || format == "text") {
        response.set_content_type("text/plain");
        if (request.has_query_param("download")) {
            response.set_header("Content-Disposition", "attachment; filename=\"alpacahttp-logs.txt\"");
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

Response Router::handle_log_history(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    response.set_content_type("application/json");

    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
    }

    auto send_payload = [&](std::uint32_t ctx_id) {
        AlpacaResponse alpaca_response(ctx_id, server_tx_id);
        alpaca_response.value = make_log_history_payload().dump();
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

            bool has_limit = false;
            std::size_t limit = util::get_log_history_limit();

            if (body.contains("Unlimited") || body.contains("unlimited")) {
                bool unlimited = false;
                if (body.contains("Unlimited")) {
                    unlimited = body["Unlimited"].get<bool>();
                } else {
                    unlimited = body["unlimited"].get<bool>();
                }
                limit = unlimited ? 0 : limit;
                has_limit = true;
            }

            if (body.contains("Limit") || body.contains("limit")) {
                const auto& value = body.contains("Limit") ? body["Limit"] : body["limit"];
                if (value.is_string()) {
                    std::string text = value.get<std::string>();
                    std::transform(text.begin(), text.end(), text.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (text == "unlimited" || text == "none") {
                        limit = 0;
                    } else {
                        limit = static_cast<std::size_t>(std::stoull(text));
                    }
                } else if (value.is_number_unsigned()) {
                    limit = value.get<std::size_t>();
                } else if (value.is_number_integer()) {
                    auto signed_limit = value.get<long long>();
                    if (signed_limit < 0) {
                        throw std::invalid_argument("Log history limit must be >= 0");
                    }
                    limit = static_cast<std::size_t>(signed_limit);
                } else {
                    throw std::invalid_argument("Log history limit must be a number or string");
                }
                has_limit = true;
            }

            if (!has_limit) {
                AlpacaResponse err = make_error_response(
                    client_tx_id, server_tx_id,
                    util::ErrorCode::VALUE_NOT_SET,
                    "Request must include 'limit' or 'unlimited'"
                );
                response.set_body(err);
                return response;
            }

            util::set_log_history_limit(limit);
            util::log_info("Log history limit set to " + std::to_string(limit));
            return send_payload(client_tx_id);
        }

        AlpacaResponse err = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::INVALID_OPERATION,
            "Unsupported HTTP method for log history endpoint"
        );
        response.set_body(err);
        return response;
    } catch (const std::exception& e) {
        AlpacaResponse err = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::DRIVER_ERROR,
            "Failed to update log history limit: " + std::string(e.what())
        );
        response.set_body(err);
        return response;
    }
}
Response Router::handle_shutdown(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    response.set_content_type("application/json");
    
    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
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

Response Router::handle_restart(const Request& request, std::uint32_t server_tx_id) {
    Response response;
    response.set_content_type("application/json");

    std::uint32_t client_tx_id = 0;
    if (request.has_query_param("ClientTransactionID")) {
        client_tx_id = parse_client_transaction_id(request.get_query_param("ClientTransactionID"));
    }

    if (request.method() != HttpMethod::POST && request.method() != HttpMethod::PUT) {
        AlpacaResponse alpaca_response = make_error_response(
            client_tx_id, server_tx_id,
            util::ErrorCode::INVALID_VALUE,
            "Method not allowed. Use POST or PUT."
        );
        response.set_body(alpaca_response);
        response.set_status(405, "Method Not Allowed");
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

bool Router::register_device_from_config(const nlohmann::json& config, std::string& error_message) {
    std::string device_type_str = config.value("deviceType", "");
    std::string vendor = config.value("vendor", "");
    int device_number = config.value("deviceNumber", -1);

    if (device_type_str.empty() || vendor.empty() || device_number < 0) {
        error_message = "Missing required fields: deviceType, vendor, and deviceNumber";
        return false;
    }

    auto& registry = alpacacore::management::DeviceRegistry::instance();

    if (vendor == "ioptron" && device_type_str == "telescope") {
#ifdef ALPACACORE_ENABLE_IOPTRON
        alpacacore::vendor::ioptron::ConnectionInfo conn_info;
        std::string conn_type = config.value("connectionType", "");

        if (conn_type == "serial") {
            conn_info.type = alpacacore::vendor::ioptron::ConnectionType::Serial;
            conn_info.port_path = config.value("portPath", "");
            conn_info.baud_rate = config.value("baudRate", 9600);

            if (conn_info.port_path.empty()) {
                error_message = "Serial port path is required";
                return false;
            }
        } else if (conn_type == "network") {
            conn_info.type = alpacacore::vendor::ioptron::ConnectionType::Network;
            conn_info.host = config.value("host", "");
            conn_info.tcp_port = config.value("tcpPort", 4030);

            if (conn_info.host.empty()) {
                error_message = "Host IP address is required";
                return false;
            }
        } else {
            error_message = "Invalid connection type. Use 'serial' or 'network'";
            return false;
        }

        conn_info.response_timeout_ms = config.value("responseTimeoutMs", conn_info.response_timeout_ms);

        std::optional<double> site_latitude;
        std::optional<double> site_longitude;
        std::optional<double> site_elevation;
        std::optional<bool> sync_time_on_connect;
        if (config.contains("siteLatitude")) {
            site_latitude = config.value("siteLatitude", 0.0);
        }
        if (config.contains("siteLongitude")) {
            site_longitude = config.value("siteLongitude", 0.0);
        }
        if (config.contains("siteElevation")) {
            site_elevation = config.value("siteElevation", 0.0);
        }
        if (config.contains("syncTimeOnConnect")) {
            sync_time_on_connect = config.value("syncTimeOnConnect", false);
        }

        auto telescope = alpacacore::vendor::ioptron::create_ioptron_telescope_with_site(
            device_number, conn_info, site_latitude, site_longitude, site_elevation, sync_time_on_connect);

        if (double aperture = config.value("apertureDiameter", 0.0); aperture > 0.0) {
            telescope->set_aperture_diameter(aperture);
        }
        if (double focal = config.value("focalLength", 0.0); focal > 0.0) {
            telescope->set_focal_length(focal);
        }
        if (site_elevation.has_value()) {
            telescope->set_site_elevation(site_elevation.value());
        }

        if (registry.register_device(std::shared_ptr<alpacacore::AlpacaDriver>(telescope.release()))) {
            util::log_info("Registered iOptron telescope");
            return true;
        }

        error_message = "Failed to register device. Device may already exist.";
        return false;
#else
        error_message = "iOptron support not enabled. Rebuild with -DALPACACORE_ENABLE_IOPTRON=ON";
        return false;
#endif
    }

    if (vendor == "synscan" && device_type_str == "telescope") {
#ifdef ALPACACORE_ENABLE_SYNSCAN
        alpacacore::vendor::synscan::ConnectionInfo conn_info;
        std::string conn_type = config.value("connectionType", "serial");

        if (conn_type == "serial") {
            conn_info.type = alpacacore::vendor::synscan::ConnectionType::Serial;
            conn_info.port_path = config.value("portPath", "");
            conn_info.baud_rate = config.value("baudRate", 9600);

            if (conn_info.port_path.empty()) {
                error_message = "Serial port path is required";
                return false;
            }
        } else if (conn_type == "network") {
            conn_info.type = alpacacore::vendor::synscan::ConnectionType::Network;
            conn_info.host = config.value("host", "");
            conn_info.tcp_port = config.value("tcpPort", conn_info.tcp_port);

            if (conn_info.host.empty()) {
                error_message = "Host IP address is required";
                return false;
            }
        } else {
            error_message = "Invalid connection type. Use 'serial' or 'network'";
            return false;
        }

        conn_info.response_timeout_ms = config.value("responseTimeoutMs", conn_info.response_timeout_ms);

        std::string version_value = config.value("synscanVersion", "auto");
        std::string version_normalized = to_lower_copy(version_value);
        alpacacore::vendor::synscan::SynScanVersion version = alpacacore::vendor::synscan::SynScanVersion::Auto;
        if (version_normalized == "v3" || version_normalized == "3") {
            version = alpacacore::vendor::synscan::SynScanVersion::V3;
        } else if (version_normalized == "v4" || version_normalized == "4") {
            version = alpacacore::vendor::synscan::SynScanVersion::V4;
        }

        std::optional<double> site_latitude;
        std::optional<double> site_longitude;
        std::optional<double> site_elevation;
        std::optional<bool> sync_time_on_connect;

        if (config.contains("siteLatitude")) {
            site_latitude = config.value("siteLatitude", 0.0);
        }
        if (config.contains("siteLongitude")) {
            site_longitude = config.value("siteLongitude", 0.0);
        }
        if (config.contains("siteElevation")) {
            site_elevation = config.value("siteElevation", 0.0);
        }
        if (config.contains("syncTimeOnConnect")) {
            sync_time_on_connect = config.value("syncTimeOnConnect", false);
        }

        auto telescope = alpacacore::vendor::synscan::create_synscan_telescope_with_site(
            device_number, conn_info, version, site_latitude, site_longitude, site_elevation, sync_time_on_connect);

        if (double aperture = config.value("apertureDiameter", 0.0); aperture > 0.0) {
            telescope->set_aperture_diameter(aperture);
        }
        if (double focal = config.value("focalLength", 0.0); focal > 0.0) {
            telescope->set_focal_length(focal);
        }
        if (site_elevation.has_value()) {
            telescope->set_site_elevation(site_elevation.value());
        }

        if (registry.register_device(std::shared_ptr<alpacacore::AlpacaDriver>(telescope.release()))) {
            util::log_info("Registered SynScan telescope");
            return true;
        }

        error_message = "Failed to register device. Device may already exist.";
        return false;
#else
        error_message = "SynScan support not enabled. Rebuild with -DALPACACORE_ENABLE_SYNSCAN=ON";
        return false;
#endif
    }

    if (vendor == "celestron" && device_type_str == "telescope") {
#ifdef ALPACACORE_ENABLE_CELESTRON
        std::string conn_type = config.value("connectionType", "auto");

        std::optional<double> site_latitude;
        std::optional<double> site_longitude;
        std::optional<double> site_elevation;
        std::optional<bool> sync_time_on_connect;

        if (config.contains("siteLatitude")) {
            site_latitude = config.value("siteLatitude", 0.0);
        }
        if (config.contains("siteLongitude")) {
            site_longitude = config.value("siteLongitude", 0.0);
        }
        if (config.contains("siteElevation")) {
            site_elevation = config.value("siteElevation", 0.0);
        }
        if (config.contains("syncTimeOnConnect")) {
            sync_time_on_connect = config.value("syncTimeOnConnect", false);
        }

        std::unique_ptr<alpacacore::TelescopeDriver> telescope;

        if (conn_type == "auto" || conn_type.empty()) {
            int mount_index = config.value("mountIndex", 0);
            telescope = alpacacore::vendor::celestron::create_celestron_telescope_auto(
                device_number, mount_index, site_latitude, site_longitude,
                site_elevation, sync_time_on_connect);
        } else {
            alpacacore::vendor::celestron::ConnectionInfo conn_info;

            if (conn_type == "serial") {
                conn_info.type = alpacacore::vendor::celestron::ConnectionType::Serial;
                conn_info.port_path = config.value("portPath", "");
                conn_info.baud_rate = config.value("baudRate", 9600);

                if (conn_info.port_path.empty()) {
                    error_message = "Serial port path is required";
                    return false;
                }
            } else if (conn_type == "network") {
                conn_info.type = alpacacore::vendor::celestron::ConnectionType::Network;
                conn_info.host = config.value("host", "");
                conn_info.tcp_port = config.value("tcpPort", conn_info.tcp_port);

                if (conn_info.host.empty()) {
                    error_message = "Host IP address is required";
                    return false;
                }
            } else {
                error_message = "Invalid connection type. Use 'auto', 'serial', or 'network'";
                return false;
            }

            conn_info.response_timeout_ms = config.value("responseTimeoutMs", conn_info.response_timeout_ms);

            telescope = alpacacore::vendor::celestron::create_celestron_telescope_with_site(
                device_number, conn_info, site_latitude, site_longitude,
                site_elevation, sync_time_on_connect);
        }

        if (double aperture = config.value("apertureDiameter", 0.0); aperture > 0.0) {
            telescope->set_aperture_diameter(aperture);
        }
        if (double focal = config.value("focalLength", 0.0); focal > 0.0) {
            telescope->set_focal_length(focal);
        }
        if (site_elevation.has_value()) {
            telescope->set_site_elevation(site_elevation.value());
        }

        if (registry.register_device(std::shared_ptr<alpacacore::AlpacaDriver>(telescope.release()))) {
            util::log_info("Registered Celestron telescope");
            return true;
        }

        error_message = "Failed to register device. Device may already exist.";
        return false;
#else
        error_message = "Celestron support not enabled. Rebuild with -DALPACACORE_ENABLE_CELESTRON=ON";
        return false;
#endif
    }

    if (vendor == "bisque" && device_type_str == "telescope") {
#ifdef ALPACACORE_ENABLE_BISQUE
        alpacacore::vendor::bisque::ConnectionInfo conn_info;
        conn_info.host = config.value("host", "localhost");
        conn_info.tcp_port = config.value("tcpPort", 3040);
        conn_info.response_timeout_ms = config.value("responseTimeoutMs", conn_info.response_timeout_ms);

        if (conn_info.host.empty()) {
            error_message = "Host is required for Bisque/TheSkyX connection";
            return false;
        }

        std::optional<double> site_latitude;
        std::optional<double> site_longitude;
        std::optional<double> site_elevation;

        if (config.contains("siteLatitude")) {
            site_latitude = config.value("siteLatitude", 0.0);
        }
        if (config.contains("siteLongitude")) {
            site_longitude = config.value("siteLongitude", 0.0);
        }
        if (config.contains("siteElevation")) {
            site_elevation = config.value("siteElevation", 0.0);
        }

        auto telescope = alpacacore::vendor::bisque::create_bisque_telescope_with_site(
            device_number, conn_info, site_latitude, site_longitude, site_elevation);

        if (double aperture = config.value("apertureDiameter", 0.0); aperture > 0.0) {
            telescope->set_aperture_diameter(aperture);
        }
        if (double focal = config.value("focalLength", 0.0); focal > 0.0) {
            telescope->set_focal_length(focal);
        }
        if (site_elevation.has_value()) {
            telescope->set_site_elevation(site_elevation.value());
        }

        if (registry.register_device(std::shared_ptr<alpacacore::AlpacaDriver>(telescope.release()))) {
            util::log_info("Registered Bisque/Paramount telescope");
            return true;
        }

        error_message = "Failed to register device. Device may already exist.";
        return false;
#else
        error_message = "Bisque support not enabled. Rebuild with -DALPACACORE_ENABLE_BISQUE=ON";
        return false;
#endif
    }

    if (vendor == "zwo" && device_type_str == "camera") {
#ifdef ALPACACORE_ENABLE_ZWO
        int camera_id = config.value("cameraId", -1);
        int camera_index = config.value("cameraIndex", -1);

        std::unique_ptr<alpacacore::CameraDriver> camera;
        if (camera_id >= 0) {
            camera = alpacacore::vendor::zwo::create_zwo_camera(device_number, camera_id);
        } else if (camera_index >= 0) {
            camera = alpacacore::vendor::zwo::create_zwo_camera_by_index(device_number, camera_index);
        } else {
            error_message = "ZWO camera requires cameraIndex or cameraId";
            return false;
        }

        if (registry.register_device(std::shared_ptr<alpacacore::AlpacaDriver>(camera.release()))) {
            util::log_info("Registered ZWO camera");
            return true;
        }

        error_message = "Failed to register device. Device may already exist.";
        return false;
#else
        error_message = "ZWO support not enabled. Rebuild with -DALPACACORE_ENABLE_ZWO=ON";
        return false;
#endif
    }

    if (vendor == "zwo" && device_type_str == "telescope") {
#ifdef ALPACACORE_ENABLE_ZWO
        alpacacore::vendor::zwo::ConnectionInfo conn_info;
        std::string conn_type = config.value("connectionType", "");

        if (conn_type == "serial") {
            conn_info.type = alpacacore::vendor::zwo::ConnectionType::Serial;
            conn_info.port_path = config.value("portPath", "");
            conn_info.baud_rate = config.value("baudRate", 9600);

            if (conn_info.port_path.empty()) {
                error_message = "Serial port path is required";
                return false;
            }
        } else if (conn_type == "network") {
            conn_info.type = alpacacore::vendor::zwo::ConnectionType::Network;
            conn_info.host = config.value("host", "");
            conn_info.tcp_port = config.value("tcpPort", 4030);

            if (conn_info.host.empty()) {
                error_message = "Host IP address is required";
                return false;
            }
        } else {
            error_message = "Invalid connection type. Use 'serial' or 'network'";
            return false;
        }

        conn_info.response_timeout_ms = config.value("responseTimeoutMs", conn_info.response_timeout_ms);

        std::optional<double> site_latitude;
        std::optional<double> site_longitude;
        std::optional<double> site_elevation;
        std::optional<bool> sync_time_on_connect;
        if (config.contains("siteLatitude")) {
            site_latitude = config.value("siteLatitude", 0.0);
        }
        if (config.contains("siteLongitude")) {
            site_longitude = config.value("siteLongitude", 0.0);
        }
        if (config.contains("siteElevation")) {
            site_elevation = config.value("siteElevation", 0.0);
        }
        if (config.contains("syncTimeOnConnect")) {
            sync_time_on_connect = config.value("syncTimeOnConnect", false);
        }
        auto telescope = alpacacore::vendor::zwo::create_zwo_telescope_with_site(
            device_number,
            conn_info,
            site_latitude,
            site_longitude,
            site_elevation,
            sync_time_on_connect);

        if (double aperture = config.value("apertureDiameter", 0.0); aperture > 0.0) {
            telescope->set_aperture_diameter(aperture);
        }
        if (double focal = config.value("focalLength", 0.0); focal > 0.0) {
            telescope->set_focal_length(focal);
        }
        if (site_elevation.has_value()) {
            telescope->set_site_elevation(site_elevation.value());
        }

        if (registry.register_device(std::shared_ptr<alpacacore::AlpacaDriver>(telescope.release()))) {
            util::log_info("Registered ZWO telescope");
            return true;
        }

        error_message = "Failed to register device. Device may already exist.";
        return false;
#else
        error_message = "ZWO support not enabled. Rebuild with -DALPACACORE_ENABLE_ZWO=ON";
        return false;
#endif
    }

    if (vendor == "zwo" && device_type_str == "filterwheel") {
#ifdef ALPACACORE_ENABLE_ZWO
        int wheel_id = config.value("filterwheelId", -1);
        int wheel_index = config.value("filterwheelIndex", -1);

        std::unique_ptr<alpacacore::FilterWheelDriver> wheel;
        if (wheel_id >= 0) {
            wheel = alpacacore::vendor::zwo::create_zwo_efw_filterwheel(device_number, wheel_id);
        } else if (wheel_index >= 0) {
            wheel = alpacacore::vendor::zwo::create_zwo_efw_filterwheel_by_index(device_number, wheel_index);
        } else {
            error_message = "ZWO filter wheel requires filterwheelIndex or filterwheelId";
            return false;
        }

        if (config.contains("filterNames")) {
            const auto& names_value = config.at("filterNames");
            if (!names_value.is_array()) {
                error_message = "ZWO filter wheel filterNames must be an array";
                return false;
            }
            wheel->set_names(names_value.get<std::vector<std::string>>());
        }

        if (registry.register_device(std::shared_ptr<alpacacore::AlpacaDriver>(wheel.release()))) {
            util::log_info("Registered ZWO EFW filter wheel");
            return true;
        }

        error_message = "Failed to register device. Device may already exist.";
        return false;
#else
        error_message = "ZWO support not enabled. Rebuild with -DALPACACORE_ENABLE_ZWO=ON";
        return false;
#endif
    }

    if (vendor == "zwo" && device_type_str == "focuser") {
#ifdef ALPACACORE_ENABLE_ZWO
        int focuser_id = config.value("focuserId", -1);
        int focuser_index = config.value("focuserIndex", -1);

        std::unique_ptr<alpacacore::FocuserDriver> focuser;
        if (focuser_id >= 0) {
            focuser = alpacacore::vendor::zwo::create_zwo_eaf_focuser(device_number, focuser_id);
        } else if (focuser_index >= 0) {
            focuser = alpacacore::vendor::zwo::create_zwo_eaf_focuser_by_index(device_number, focuser_index);
        } else {
            error_message = "ZWO EAF focuser requires focuserIndex or focuserId";
            return false;
        }

        if (registry.register_device(std::shared_ptr<alpacacore::AlpacaDriver>(focuser.release()))) {
            util::log_info("Registered ZWO EAF focuser");
            return true;
        }

        error_message = "Failed to register device. Device may already exist.";
        return false;
#else
        error_message = "ZWO support not enabled. Rebuild with -DALPACACORE_ENABLE_ZWO=ON";
        return false;
#endif
    }

    if (vendor == "zwo" && device_type_str == "rotator") {
#ifdef ALPACACORE_ENABLE_ZWO
        int rotator_id = config.value("rotatorId", -1);
        int rotator_index = config.value("rotatorIndex", -1);

        std::unique_ptr<alpacacore::RotatorDriver> rotator;
        if (rotator_id >= 0) {
            rotator = alpacacore::vendor::zwo::create_zwo_caa_rotator(device_number, rotator_id);
        } else if (rotator_index >= 0) {
            rotator = alpacacore::vendor::zwo::create_zwo_caa_rotator_by_index(device_number, rotator_index);
        } else {
            error_message = "ZWO rotator requires rotatorIndex or rotatorId";
            return false;
        }

        if (registry.register_device(std::shared_ptr<alpacacore::AlpacaDriver>(rotator.release()))) {
            util::log_info("Registered ZWO CAA rotator");
            return true;
        }

        error_message = "Failed to register device. Device may already exist.";
        return false;
#else
        error_message = "ZWO support not enabled. Rebuild with -DALPACACORE_ENABLE_ZWO=ON";
        return false;
#endif
    }

    if (vendor == "zwo" && device_type_str == "switch") {
#ifdef ALPACACORE_ENABLE_ZWO
        std::string switch_type = config.value("switchType", "dewheater");
        switch_type = to_lower_copy(switch_type);
        if (switch_type != "dewheater") {
            error_message = "ZWO switchType must be 'dewheater'";
            return false;
        }

        int camera_id = config.value("cameraId", -1);
        int camera_index = config.value("cameraIndex", -1);

        std::unique_ptr<alpacacore::SwitchDriver> sw;
        if (camera_id >= 0) {
            sw = alpacacore::vendor::zwo::create_zwo_dew_heater_switch(device_number, camera_id);
        } else if (camera_index >= 0) {
            sw = alpacacore::vendor::zwo::create_zwo_dew_heater_switch_by_index(device_number, camera_index);
        } else {
            error_message = "ZWO dew heater switch requires cameraIndex or cameraId";
            return false;
        }

        if (registry.register_device(std::shared_ptr<alpacacore::AlpacaDriver>(sw.release()))) {
            util::log_info("Registered ZWO dew heater switch");
            return true;
        }

        error_message = "Failed to register device. Device may already exist.";
        return false;
#else
        error_message = "ZWO support not enabled. Rebuild with -DALPACACORE_ENABLE_ZWO=ON";
        return false;
#endif
    }

    if (vendor == "weewx" && device_type_str == "observingconditions") {
#ifdef ALPACACORE_ENABLE_WEEWX
        alpacacore::vendor::weewx::WeeWxHttpConfig weewx_config;
        weewx_config.url = config.value("weewxUrl", "");
        int poll_interval = config.value("pollIntervalSeconds", 900);
        int timeout_ms = config.value("timeoutMs", 5000);
        if (weewx_config.url.empty()) {
            error_message = "WeeWX observing conditions requires weewxUrl";
            return false;
        }
        if (poll_interval <= 0) {
            error_message = "pollIntervalSeconds must be greater than 0";
            return false;
        }
        if (timeout_ms <= 0) {
            error_message = "timeoutMs must be greater than 0";
            return false;
        }
        weewx_config.poll_interval = std::chrono::seconds(poll_interval);
        weewx_config.timeout = std::chrono::milliseconds(timeout_ms);

        auto observing = alpacacore::vendor::weewx::create_weewx_observingconditions(
            device_number, weewx_config);
        if (registry.register_device(std::shared_ptr<alpacacore::AlpacaDriver>(observing.release()))) {
            util::log_info("Registered WeeWX observing conditions");
            return true;
        }

        error_message = "Failed to register device. Device may already exist.";
        return false;
#else
        error_message = "WeeWX support not enabled. Rebuild with -DALPACACORE_ENABLE_WEEWX=ON";
        return false;
#endif
    }

    if (vendor == "qhy" && device_type_str == "camera") {
#ifdef ALPACACORE_ENABLE_QHY
        std::string camera_id = config.value("cameraId", "");
        int camera_index = config.value("cameraIndex", -1);

        std::unique_ptr<alpacacore::CameraDriver> camera;
        if (!camera_id.empty()) {
            camera = alpacacore::vendor::qhy::create_qhy_camera(device_number, camera_id);
        } else if (camera_index >= 0) {
            camera = alpacacore::vendor::qhy::create_qhy_camera_by_index(device_number, camera_index);
        } else {
            error_message = "QHY camera requires cameraIndex or cameraId";
            return false;
        }

        if (registry.register_device(std::shared_ptr<alpacacore::AlpacaDriver>(camera.release()))) {
            util::log_info("Registered QHY camera");
            return true;
        }

        error_message = "Failed to register device. Device may already exist.";
        return false;
#else
        error_message = "QHY support not enabled. Rebuild with -DALPACACORE_ENABLE_QHY=ON";
        return false;
#endif
    }

    if (vendor == "svbony" && device_type_str == "camera") {
#ifdef ALPACACORE_ENABLE_SVBONY
        int camera_index = config.value("cameraIndex", 0);

        auto camera = alpacacore::vendor::svbony::create_svbony_camera(device_number, camera_index);

        if (registry.register_device(std::shared_ptr<alpacacore::AlpacaDriver>(camera.release()))) {
            util::log_info("Registered SVBONY camera");
            return true;
        }

        error_message = "Failed to register device. Device may already exist.";
        return false;
#else
        error_message = "SVBONY support not enabled. Rebuild with -DALPACACORE_ENABLE_SVBONY=ON";
        return false;
#endif
    }

    if (vendor == "touptek" && device_type_str == "camera") {
#ifdef ALPACACORE_ENABLE_TOUPTEK
        int camera_index = config.value("cameraIndex", 0);

        auto camera = alpacacore::vendor::touptek::create_touptek_camera(device_number, camera_index);

        if (registry.register_device(std::shared_ptr<alpacacore::AlpacaDriver>(camera.release()))) {
            util::log_info("Registered ToupTek camera");
            return true;
        }

        error_message = "Failed to register device. Device may already exist.";
        return false;
#else
        error_message = "ToupTek support not enabled. Rebuild with -DALPACACORE_ENABLE_TOUPTEK=ON";
        return false;
#endif
    }

    if (vendor == "gemini" && device_type_str == "focuser") {
#ifdef ALPACACORE_ENABLE_GEMINI
        std::string conn_type = config.value("connectionType", "auto");

        std::unique_ptr<alpacacore::FocuserDriver> focuser;
        if (conn_type == "serial") {
            std::string port_path = config.value("portPath", "");
            if (port_path.empty()) {
                // No port specified with serial mode — fall through to auto-detect
                int focuser_index = config.value("focuserIndex", 0);
                focuser = alpacacore::vendor::gemini::create_gemini_focuser_by_index(device_number, focuser_index);
            } else {
                int baud_rate = config.value("baudRate", 9600);
                focuser = alpacacore::vendor::gemini::create_gemini_focuser(device_number, port_path, baud_rate);
            }
        } else {
            // "auto" or unset — auto-detect
            int focuser_index = config.value("focuserIndex", 0);
            focuser = alpacacore::vendor::gemini::create_gemini_focuser_by_index(device_number, focuser_index);
        }

        if (registry.register_device(std::shared_ptr<alpacacore::AlpacaDriver>(focuser.release()))) {
            util::log_info("Registered Gemini focuser");
            return true;
        }

        error_message = "Failed to register device. Device may already exist.";
        return false;
#else
        error_message = "Gemini support not enabled. Rebuild with -DALPACACORE_ENABLE_GEMINI=ON";
        return false;
#endif
    }

    error_message = "Vendor/device type combination not yet supported: " + vendor + "/" + device_type_str;
    return false;
}

nlohmann::json Router::sanitize_device_config(const nlohmann::json& config) const {
    nlohmann::json sanitized = nlohmann::json::object();

    auto copy_if_present = [&](const char* key) {
        if (config.contains(key)) {
            sanitized[key] = config.at(key);
        }
    };

    copy_if_present("vendor");
    copy_if_present("deviceType");
    copy_if_present("deviceNumber");

    std::string vendor = config.value("vendor", "");
    std::string device_type = config.value("deviceType", "");
    if (vendor == "ioptron") {
        copy_if_present("connectionType");
        std::string connection_type = config.value("connectionType", "");
        if (connection_type == "serial") {
            copy_if_present("portPath");
            copy_if_present("baudRate");
        } else if (connection_type == "network") {
            copy_if_present("host");
            copy_if_present("tcpPort");
        }
    } else if (vendor == "synscan") {
        copy_if_present("synscanVersion");
        copy_if_present("connectionType");
        std::string connection_type = config.value("connectionType", "");
        if (connection_type == "serial") {
            copy_if_present("portPath");
            copy_if_present("baudRate");
        } else if (connection_type == "network") {
            copy_if_present("host");
            copy_if_present("tcpPort");
        }
    } else if (vendor == "zwo") {
        if (device_type == "telescope") {
            copy_if_present("connectionType");
            std::string connection_type = config.value("connectionType", "");
            if (connection_type == "serial") {
                copy_if_present("portPath");
                copy_if_present("baudRate");
            } else if (connection_type == "network") {
                copy_if_present("host");
                copy_if_present("tcpPort");
            }
        }
        copy_if_present("cameraIndex");
        copy_if_present("cameraId");
        copy_if_present("switchType");
        copy_if_present("filterwheelIndex");
        copy_if_present("filterwheelId");
        copy_if_present("filterNames");
        copy_if_present("focuserIndex");
        copy_if_present("focuserId");
        copy_if_present("rotatorIndex");
        copy_if_present("rotatorId");
    } else if (vendor == "qhy") {
        copy_if_present("cameraIndex");
        copy_if_present("cameraId");
    } else if (vendor == "svbony") {
        copy_if_present("cameraIndex");
    } else if (vendor == "touptek") {
        copy_if_present("cameraIndex");
    } else if (vendor == "weewx") {
        copy_if_present("weewxUrl");
        copy_if_present("pollIntervalSeconds");
        copy_if_present("timeoutMs");
    } else if (vendor == "celestron") {
        copy_if_present("connectionType");
        copy_if_present("mountIndex");
        std::string connection_type = config.value("connectionType", "");
        if (connection_type == "serial") {
            copy_if_present("portPath");
            copy_if_present("baudRate");
        } else if (connection_type == "network") {
            copy_if_present("host");
            copy_if_present("tcpPort");
        }
        // "auto" needs no extra fields — port is discovered at startup
    } else if (vendor == "gemini") {
        copy_if_present("connectionType");
        copy_if_present("focuserIndex");
        std::string connection_type = config.value("connectionType", "auto");
        if (connection_type == "serial") {
            copy_if_present("portPath");
            copy_if_present("baudRate");
        }
    } else if (vendor == "bisque") {
        copy_if_present("host");
        copy_if_present("tcpPort");
    }

    copy_if_present("responseTimeoutMs");
    copy_if_present("apertureDiameter");
    copy_if_present("focalLength");
    copy_if_present("siteLatitude");
    copy_if_present("siteLongitude");
    copy_if_present("siteElevation");
    copy_if_present("syncTimeOnConnect");

    return sanitized;
}

void Router::add_or_replace_persisted_device(const nlohmann::json& config) {
    if (!config.contains("deviceType") || !config.contains("vendor") || !config.contains("deviceNumber")) {
        return;
    }

    for (auto& existing : persisted_devices_) {
        if (existing.value("vendor", "") == config.value("vendor", "") &&
            existing.value("deviceType", "") == config.value("deviceType", "") &&
            existing.value("deviceNumber", -1) == config.value("deviceNumber", -1)) {
            existing = config;
            return;
        }
    }

    persisted_devices_.push_back(config);
}

void Router::remove_persisted_device(const std::string& vendor, const std::string& device_type, int device_number) {
    if (device_type.empty() || device_number < 0) {
        return;
    }

    persisted_devices_.erase(
        std::remove_if(
            persisted_devices_.begin(),
            persisted_devices_.end(),
            [&](const nlohmann::json& entry) {
                if (entry.value("deviceType", "") != device_type ||
                    entry.value("deviceNumber", -1) != device_number) {
                    return false;
                }
                if (vendor.empty()) {
                    return true;
                }
                return entry.value("vendor", "") == vendor;
            }),
        persisted_devices_.end()
    );
}

void Router::save_persisted_devices() const {
    try {
        if (kPersistedDevicesFile.has_parent_path()) {
            std::filesystem::create_directories(kPersistedDevicesFile.parent_path());
        }

        std::ofstream out(kPersistedDevicesFile);
        if (!out) {
            throw std::runtime_error("Unable to open " + kPersistedDevicesFile.string() + " for writing");
        }

        nlohmann::json payload = persisted_devices_;
        out << payload.dump(4);
        out.flush();
    } catch (const std::exception& e) {
        util::log_error("Failed to persist registered devices: " + std::string(e.what()));
    }
}

void Router::load_persisted_devices() {
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

        nlohmann::json payload;
        in >> payload;

        if (!payload.is_array()) {
            throw std::runtime_error("Persisted device file must contain a JSON array");
        }

        persisted_devices_.clear();
        for (const auto& entry : payload) {
            std::string error_message;
            if (register_device_from_config(entry, error_message)) {
                persisted_devices_.push_back(sanitize_device_config(entry));
            } else {
                util::log_warning("Skipping persisted device: " + error_message);
            }
        }
    } catch (const std::exception& e) {
        util::log_error("Failed to load registered devices: " + std::string(e.what()));
        persisted_devices_.clear();
    }
}

} // namespace alpacahttp
