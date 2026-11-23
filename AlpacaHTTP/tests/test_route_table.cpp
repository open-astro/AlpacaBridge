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

// #646: pins the router's accepted surface and its rejection responses.
//
// Run order: F, A, B, C, D. F runs first so a fixture/router mismatch reports
// as F's message instead of as A's per-method failures.
//
//  A. Route table: for every device type name and every method name in the
//     committed snapshot (router_route_table_fixture.h), the verb mask the live
//     router derives for it equals the snapshot. The mask is read off the
//     router's own gate order, with no driver registered: an unknown method
//     answers "Unknown method", a known method on the wrong verb answers
//     "does not accept <VERB>", and a known method on an accepted verb gets
//     past both gates and stops at "Device not found".
//  B. Completeness: the k*Methods tables scanned out of the router sources
//     equal the snapshot, so a brand-new route (which A cannot name) fails too.
//  C. Dispatch reach: with a do-nothing driver registered per device type,
//     every accepted (type, method, verb) is answered by its dispatcher and
//     never by the "not yet implemented" fallback.
//  F. Type names: every name in fixtures/device_type_names.txt is recognised
//     by the router and "telescopes" is not, and the names scanned out of
//     is_known_device_type_name() equal the fixture (#657). Checks A and C
//     iterate that fixture too.
//  D. Rejection fixture: exact HTTP status, Alpaca error number and message
//     for a wrong verb, an unknown method, an out-of-range device number and a
//     NaN parameter, as on origin/main c4640d6e.
//  G. Management routes (#2485): for every row of kManagementFixture, both
//     path spellings and five verbs, with and without a foreign Origin: a wrong
//     verb gets its family's answer, every state-changing route refuses a
//     cross-origin request with 403, and an accepted verb is never refused.
//
// The per-request cost check that used to be E is gone (#657): a wall-clock
// ratio was coarse in Release. scripts/check_docs_drift.py check 14 now
// requires every std::regex in router.cpp to be static instead.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/device_registry.h>
#include <alpacahttp/request.h>
#include <alpacahttp/router.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "route_table_stubs.h"
#include "router_route_table_fixture.h"
#include "test_assert.h"

namespace {

using route_table_fixture::kGet;
using route_table_fixture::kPut;
using route_table_fixture::kRoutes;
using route_table_fixture::RouteRow;

constexpr int kProbeDevice = 4646;
constexpr int kInvalidValue = alpacacore::AlpacaError::InvalidValue;

// The accepted device-type names live in tests/fixtures/device_type_names.txt,
// not here: a copy in this file could only agree with router.cpp by hand (#657).
// ALPACAHTTP_ROUTER_SRC_DIR is <AlpacaHTTP>/src, so the tests dir is its sibling.
std::vector<std::string> load_type_names() {
    const auto path =
        std::filesystem::path(ALPACAHTTP_ROUTER_SRC_DIR).parent_path() / "tests" / "fixtures" / "device_type_names.txt";
    std::ifstream in(path);
    if (!in) {
        std::cerr << "cannot open " << path << "\n";
    }
    EXPECT(static_cast<bool>(in));
    std::vector<std::string> names;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        EXPECT(std::all_of(line.begin(), line.end(),
                           [](char c) { return std::islower(static_cast<unsigned char>(c)) != 0; }));
        EXPECT(std::find(names.begin(), names.end(), line) == names.end());
        names.push_back(line);
    }
    EXPECT(!names.empty());
    return names;
}

// A Device API 400 is a text/plain message with no JSON envelope (#1476): error_number stays kNoEnvelope.
constexpr int kNoEnvelope = -1;

struct Answer {
    int status = 0;
    int error_number = kNoEnvelope;
    std::string message;
    std::string content_type;
    std::string allow;
    unsigned client_tx_id = 0;
};

// No Origin header unless `foreign_origin`: a foreign Origin is answered 403 before the wrong-verb 400.
Answer send(alpacahttp::Router& router, const std::string& verb, const std::string& path,
            const std::string& body = std::string(), bool foreign_origin = false) {
    alpacahttp::Request request;
    std::ostringstream raw;
    raw << verb << " " << path << " HTTP/1.1\r\nHost: localhost\r\n";
    if (foreign_origin) {
        raw << "Origin: http://evil.example\r\n";
    }
    if (!body.empty()) {
        raw << "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: " << body.size() << "\r\n";
    }
    raw << "\r\n" << body;
    EXPECT(request.parse(raw.str()));
    const auto response = router.route(request, 1);
    Answer answer;
    answer.status = response.status_code();
    answer.content_type = response.get_header("Content-Type");
    answer.allow = response.get_header("Allow");
    const auto json = nlohmann::json::parse(response.body(), nullptr, false);
    if (json.is_object()) {
        answer.error_number = json.value("ErrorNumber", -1);
        answer.message = json.value("ErrorMessage", "");
        answer.client_tx_id = json.value("ClientTransactionID", 0U);
    } else {
        answer.message = response.body();
    }
    return answer;
}

bool starts_with(const std::string& text, const std::string& prefix) { return text.rfind(prefix, 0) == 0; }

std::string mask_name(unsigned mask) {
    switch (mask) {
        case 0:
            return "none";
        case kGet:
            return "GET";
        case kPut:
            return "PUT";
        case kGet | kPut:
            return "GET|PUT";
        default:
            return "?";
    }
}

// Verb mask of `method` for `type` per the snapshot: the common block wins,
// then the type's own block, else 0.
unsigned expected_mask(const std::string& type, const std::string& method) {
    unsigned common = 0;
    unsigned own = 0;
    for (const RouteRow& row : kRoutes) {
        if (method != row.method) {
            continue;
        }
        if (std::string(row.type) == "common") {
            common = row.mask;
        } else if (type == row.type) {
            own = row.mask;
        }
    }
    return common != 0 ? common : own;
}

void report(const char* section, const std::vector<std::string>& failures) {
    for (const auto& line : failures) {
        std::cerr << section << ": " << line << "\n";
    }
    std::cerr << section << ": " << failures.size() << " mismatch(es)\n";
    EXPECT(failures.empty());
}

// A. Live router vs snapshot, derived without any driver registered.
void check_route_table(alpacahttp::Router& router) {
    std::set<std::string> methods;
    for (const RouteRow& row : kRoutes) {
        methods.insert(row.method);
    }
    // The snapshot must be unambiguous, or expected_mask() is meaningless.
    std::set<std::string> seen;
    for (const RouteRow& row : kRoutes) {
        EXPECT(seen.insert(std::string(row.type) + "/" + row.method).second);
        if (std::string(row.type) != "common") {
            // A per-type row shadowed by a common row would never be consulted.
            EXPECT(expected_mask("common", row.method) == 0);
        }
    }

    const std::vector<std::string> types = load_type_names();
    std::vector<std::string> failures;
    std::size_t probes = 0;
    for (const auto& type : types) {
        for (const auto& method : methods) {
            unsigned live = 0;
            for (const auto& verb : {std::string("GET"), std::string("PUT")}) {
                const Answer a =
                    send(router, verb, "/api/v1/" + type + "/" + std::to_string(kProbeDevice) + "/" + method);
                ++probes;
                const unsigned bit = verb == "GET" ? kGet : kPut;
                if (a.status != 400 || a.error_number != kNoEnvelope || a.content_type.find("text/plain") != 0) {
                    failures.push_back(type + "/" + method + " " + verb + ": status " + std::to_string(a.status) +
                                       " type '" + a.content_type + "' (want 400 text/plain from a gate)");
                } else if (starts_with(a.message, "Device not found: ")) {
                    live |= bit;
                } else if (!starts_with(a.message, "Unknown method: ") &&
                           !starts_with(a.message, "Method '" + method + "' does not accept ")) {
                    failures.push_back(type + "/" + method + " " + verb + ": unexpected message '" + a.message + "'");
                }
            }
            const unsigned want = expected_mask(type, method);
            if (live != want) {
                failures.push_back(type + "/" + method + ": router accepts " + mask_name(live) + ", snapshot says " +
                                   mask_name(want));
            }
        }
    }
    EXPECT(probes > 0);
    std::cout << "A: " << probes << " probes over " << types.size() << " type names x " << methods.size()
              << " method names\n";
    report("A route table", failures);
}

// B. The tables in the router sources vs the snapshot (catches added rows).
bool is_word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

std::size_t skip_space(const std::string& text, std::size_t i) {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) {
        ++i;
    }
    return i;
}

bool consume(const std::string& text, std::size_t& i, const char* literal) {
    const std::string lit(literal);
    if (text.compare(i, lit.size(), lit) != 0) {
        return false;
    }
    i += lit.size();
    return true;
}

// Plain string scan rather than std::regex: under ASan+UBSan a std::regex pass
// over the whole router source cost minutes in the CI sanitizer job (#646).
// Reads `{"name", kVerbGet | kVerbPut}` entries; sets `raw` for every
// `{"name",` seen and returns the entries whose verb spelling it recognises.
std::vector<std::pair<std::string, unsigned>> parse_entries(const std::string& body, std::size_t& raw) {
    std::vector<std::pair<std::string, unsigned>> parsed;
    raw = 0;
    std::size_t i = 0;
    while ((i = body.find('{', i)) != std::string::npos) {
        std::size_t j = skip_space(body, i + 1);
        if (j >= body.size() || body[j] != '"') {
            i = j;
            continue;
        }
        std::size_t k = j + 1;
        while (k < body.size() && body[k] != '"' && body[k] != '\n') {
            ++k;
        }
        if (k >= body.size() || body[k] != '"') {
            i = k;
            continue;
        }
        // Any quoted name counts as an entry; only a word-character name can
        // parse, so an empty or oddly spelled name is reported, not skipped.
        const std::string name = body.substr(j + 1, k - j - 1);
        const bool name_ok = !name.empty() && std::all_of(name.begin(), name.end(), is_word_char);
        k = skip_space(body, k + 1);
        if (k >= body.size() || body[k] != ',') {
            i = k;
            continue;
        }
        ++raw;
        k = skip_space(body, k + 1);
        unsigned mask = 0;
        if (consume(body, k, "kVerbGet")) {
            mask = kGet;
            std::size_t after = skip_space(body, k);
            if (after < body.size() && body[after] == '|') {
                after = skip_space(body, after + 1);
                if (consume(body, after, "kVerbPut")) {
                    mask = kGet | kPut;
                    k = after;
                }
            }
        } else if (consume(body, k, "kVerbPut")) {
            mask = kPut;
        }
        k = skip_space(body, k);
        if (name_ok && mask != 0 && k < body.size() && body[k] == '}') {
            parsed.emplace_back(name, mask);
        }
        i = k;
    }
    return parsed;
}

void check_source_tables() {
    std::map<std::string, unsigned> source;
    std::size_t tables = 0;
    for (const auto& file : std::filesystem::recursive_directory_iterator(ALPACAHTTP_ROUTER_SRC_DIR)) {
        const auto ext = file.path().extension();
        if (!file.is_regular_file() || (ext != ".cpp" && ext != ".h")) {
            continue;
        }
        std::ifstream in(file.path());
        std::stringstream buffer;
        buffer << in.rdbuf();
        const std::string text = buffer.str();
        // A table opens with `k<Type>Methods = {`.
        for (std::size_t at = text.find("Methods"); at != std::string::npos; at = text.find("Methods", at + 1)) {
            std::size_t name_start = at;
            while (name_start > 0 && is_word_char(text[name_start - 1])) {
                --name_start;
            }
            std::size_t after = skip_space(text, at + 7);
            if (text[name_start] != 'k' || name_start + 1 == at || after >= text.size() || text[after] != '=') {
                continue;
            }
            after = skip_space(text, after + 1);
            if (after >= text.size() || text[after] != '{') {
                continue;
            }
            ++tables;
            const std::string type_name = text.substr(name_start + 1, at - name_start - 1);
            std::string type = type_name;
            for (char& c : type) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            const std::size_t begin = after + 1;
            const std::size_t end = text.find("\n};", begin);
            EXPECT(end != std::string::npos);
            const std::string body = text.substr(begin, end - begin);
            // Every {"name", ...} entry must parse; an unrecognised verb spelling
            // (e.g. kVerbPut | kVerbGet) would otherwise be skipped silently.
            std::size_t raw_count = 0;
            const auto entries = parse_entries(body, raw_count);
            for (const auto& [name, mask] : entries) {
                const std::string key = type + "/" + name;
                if (!source.emplace(key, mask).second) {
                    report("B source tables", {"duplicate table row " + key});
                }
            }
            if (raw_count != entries.size()) {
                report("B source tables",
                       {"k" + type_name + "Methods: " + std::to_string(raw_count) + " entries, only " +
                        std::to_string(entries.size()) + " parsed (unrecognised name or verb spelling)"});
            }
        }
    }
    std::cout << "B: scanned " << tables << " tables, " << source.size() << " rows\n";
    EXPECT(tables == 11);

    std::map<std::string, unsigned> snapshot;
    for (const RouteRow& row : kRoutes) {
        snapshot[std::string(row.type) + "/" + row.method] = row.mask;
    }
    std::vector<std::string> failures;
    for (const auto& [key, mask] : source) {
        const auto it = snapshot.find(key);
        if (it == snapshot.end()) {
            failures.push_back("in router.cpp but not in the snapshot: " + key + " " + mask_name(mask));
        } else if (it->second != mask) {
            failures.push_back(key + ": router.cpp " + mask_name(mask) + ", snapshot " + mask_name(it->second));
        }
    }
    for (const auto& [key, mask] : snapshot) {
        if (source.count(key) == 0) {
            failures.push_back("in the snapshot but not in router.cpp: " + key + " " + mask_name(mask));
        }
    }
    report("B source tables", failures);
}

std::vector<std::shared_ptr<alpacacore::AlpacaDriver>> make_stubs() {
    using namespace route_table_stubs;
    return {std::make_shared<CameraStub>(kProbeDevice),
            std::make_shared<TelescopeStub>(kProbeDevice),
            std::make_shared<FilterWheelStub>(kProbeDevice),
            std::make_shared<FocuserStub>(kProbeDevice),
            std::make_shared<RotatorStub>(kProbeDevice),
            std::make_shared<DomeStub>(kProbeDevice),
            std::make_shared<SwitchStub>(kProbeDevice),
            std::make_shared<CoverCalibratorStub>(kProbeDevice),
            std::make_shared<ObservingConditionsStub>(kProbeDevice),
            std::make_shared<SafetyMonitorStub>(kProbeDevice)};
}

// C. Every accepted (type, method, verb) reaches a dispatcher branch.
void check_dispatch_reach(alpacahttp::Router& router) {
    std::vector<std::string> failures;
    std::size_t requests = 0;
    for (const auto& type : load_type_names()) {
        for (const RouteRow& row : kRoutes) {
            if (std::string(row.type) != "common" && type != row.type) {
                continue;
            }
            for (const auto& [verb, bit] : {std::pair<std::string, unsigned>{"GET", kGet}, {"PUT", kPut}}) {
                if ((row.mask & bit) == 0) {
                    continue;
                }
                const std::string path = "/api/v1/" + type + "/" + std::to_string(kProbeDevice) + "/" + row.method;
                const Answer a = send(router, verb, path, verb == "PUT" ? "ClientID=1" : "");
                ++requests;
                if (starts_with(a.message, "Method '" + std::string(row.method) + "' not yet implemented") ||
                    starts_with(a.message, "Unknown method: ") || starts_with(a.message, "Device not found: ") ||
                    a.message.find("does not accept") != std::string::npos) {
                    failures.push_back(type + "/" + row.method + " " + verb + " -> HTTP " + std::to_string(a.status) +
                                       " '" + a.message + "'");
                }
            }
        }
    }
    EXPECT(requests > 0);
    std::cout << "C: " << requests << " dispatched requests\n";
    report("C dispatch reach", failures);
}

struct Rejection {
    const char* what;
    const char* verb;
    const char* path;
    const char* body;
    int status;
    int error_number;
    const char* message;
};

// D. One request per rejection class, status + Alpaca error number + message.
void check_rejections(alpacahttp::Router& router) {
    const std::vector<Rejection> cases = {
        {"wrong verb (PUT to a GET property)", "PUT", "/api/v1/telescope/4646/altitude", "ClientID=1", 400, kNoEnvelope,
         "Method 'altitude' does not accept PUT"},
        {"wrong verb (GET to a PUT command)", "GET", "/api/v1/telescope/4646/abortslew", "", 400, kNoEnvelope,
         "Method 'abortslew' does not accept GET"},
        {"wrong verb (POST)", "POST", "/api/v1/telescope/4646/altitude", "", 400, kNoEnvelope,
         "Method 'altitude' does not accept POST"},
        {"unknown method", "GET", "/api/v1/telescope/4646/notarealmethod", "", 400, kNoEnvelope,
         "Unknown method: notarealmethod"},
        {"unknown method (case matters)", "GET", "/api/v1/telescope/4646/Altitude", "", 400, kNoEnvelope,
         "Unknown method: Altitude"},
        {"device number over uint32", "GET", "/api/v1/telescope/4294967296/connected", "", 400, kNoEnvelope,
         "Invalid device number for device type: telescope"},
        {"device number over uint64", "GET", "/api/v1/telescope/99999999999999999999/connected", "", 400, kNoEnvelope,
         "Invalid device number for device type: telescope"},
        {"device number in range but unregistered", "GET", "/api/v1/telescope/4242/connected", "", 400, kNoEnvelope,
         "Device not found: telescope #4242"},
        {"unknown device type", "GET", "/api/v1/wibble/0/connected", "", 400, kNoEnvelope,
         "Unknown device type: wibble"},
        // NaN-class parameters do not parse as an Alpaca number: HTTP 400 text (#1476).
        {"NaN parameter", "PUT", "/api/v1/telescope/4646/targetdeclination", "TargetDeclination=nan&ClientID=1", 400,
         kNoEnvelope, "Invalid value for parameter: TargetDeclination"},
        {"inf parameter", "PUT", "/api/v1/telescope/4646/targetdeclination", "TargetDeclination=inf&ClientID=1", 400,
         kNoEnvelope, "Invalid value for parameter: TargetDeclination"},
        {"-infinity parameter", "PUT", "/api/v1/telescope/4646/targetdeclination",
         "TargetDeclination=-infinity&ClientID=1", 400, kNoEnvelope, "Invalid value for parameter: TargetDeclination"},
        {"hex-float parameter", "PUT", "/api/v1/telescope/4646/targetdeclination", "TargetDeclination=0x1p3&ClientID=1",
         400, kNoEnvelope, "Invalid value for parameter: TargetDeclination"},
    };
    std::vector<std::string> failures;
    for (const auto& c : cases) {
        const Answer a = send(router, c.verb, c.path, c.body);
        const bool message_ok = std::string(c.message).empty() || a.message == c.message;
        if (a.status != c.status || a.error_number != c.error_number || !message_ok) {
            failures.push_back(std::string(c.what) + ": got HTTP " + std::to_string(a.status) + " error " +
                               std::to_string(a.error_number) + " '" + a.message + "', want HTTP " +
                               std::to_string(c.status) + " error " + std::to_string(c.error_number) + " '" +
                               c.message + "'");
        }
    }
    std::cout << "D: " << cases.size() << " rejection cases\n";
    report("D rejections", failures);
}

// F. The device-type names: fixture vs router, both directions.
// Limit: the scan reads only the kDeviceTypes initialiser in router.cpp. A name accepted some other way
// (an extra `||` in is_known_device_type_name(), a second set, an alias) is not seen by the scan or the probes.
std::set<std::string> scan_router_type_names() {
    std::ifstream in(std::filesystem::path(ALPACAHTTP_ROUTER_SRC_DIR) / "http" / "router.cpp");
    EXPECT(static_cast<bool>(in));
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();
    std::set<std::string> names;
    const std::size_t fn = text.find("bool is_known_device_type_name(");
    EXPECT(fn != std::string::npos);
    const std::size_t open = text.find("kDeviceTypes", fn);
    EXPECT(open != std::string::npos);
    const std::size_t begin = text.find('{', open);
    const std::size_t end = text.find("};", begin);
    EXPECT(begin != std::string::npos && end != std::string::npos);
    for (std::size_t i = begin; i < end;) {
        const std::size_t q = text.find('"', i);
        if (q == std::string::npos || q >= end) {
            break;
        }
        const std::size_t close = text.find('"', q + 1);
        EXPECT(close != std::string::npos);
        names.insert(text.substr(q + 1, close - q - 1));
        i = close + 1;
    }
    return names;
}

void check_type_names(alpacahttp::Router& router) {
    std::vector<std::string> failures;
    const std::vector<std::string> fixture = load_type_names();
    for (const auto& name : fixture) {
        const Answer a = send(router, "GET", "/api/v1/" + name + "/" + std::to_string(kProbeDevice) + "/connected");
        if (a.status != 400 || a.error_number != kNoEnvelope || !starts_with(a.message, "Device not found: ")) {
            failures.push_back("fixture name '" + name + "' is not recognised by the router: HTTP " +
                               std::to_string(a.status) + " '" + a.message + "'");
        }
    }
    const Answer plural = send(router, "GET", "/api/v1/telescopes/" + std::to_string(kProbeDevice) + "/connected");
    if (plural.status != 400 || plural.error_number != kNoEnvelope ||
        plural.message != "Unknown device type: telescopes") {
        failures.push_back("'telescopes' must be rejected as an unknown type, got HTTP " +
                           std::to_string(plural.status) + " '" + plural.message + "'");
    }
    const std::set<std::string> scanned = scan_router_type_names();
    EXPECT(!scanned.empty());
    const std::set<std::string> listed(fixture.begin(), fixture.end());
    for (const auto& name : scanned) {
        if (listed.count(name) == 0) {
            failures.push_back("router.cpp accepts '" + name + "' but device_type_names.txt lacks it");
        }
    }
    for (const auto& name : listed) {
        if (scanned.count(name) == 0) {
            failures.push_back("device_type_names.txt lists '" + name + "' but is_known_device_type_name() lacks it");
        }
    }
    std::cout << "F: " << fixture.size() + 1 << " names probed, " << scanned.size() << " scanned from the router\n";
    report("F type names", failures);
}

// G. The management surface, one row per route (#2485).
enum class Guard { None, BeforeVerbCheck, AfterVerbCheck };
enum class WrongVerb { None, AlpacaText, MethodNotAllowed, Handler };

constexpr unsigned kVerbGet = 1U << 0;
constexpr unsigned kVerbPost = 1U << 1;
constexpr unsigned kVerbPut = 1U << 2;
constexpr unsigned kVerbDelete = 1U << 3;
constexpr unsigned kVerbOther = 1U << 4;  // PATCH, which parse_method does not know
constexpr unsigned kEveryVerb = kVerbGet | kVerbPost | kVerbPut | kVerbDelete | kVerbOther;

struct ManagementRow {
    const char* path;   // a tail is written {name}; the probe replaces it with `tail`
    const char* alias;  // the unversioned spelling
    const char* tail;
    unsigned verbs;
    Guard guard;
    const char* guard_what;
    WrongVerb wrong_verb;
    const char* wrong_verb_message;
    unsigned safe_verbs;  // verbs the probe may send without an Origin: they change nothing here
    // open-astro#674: the wrong-verb 405 echoes a ClientTransactionID sent only in the JSON body.
    bool body_transaction_id = false;
};

constexpr const char* kPostPut = "Method not allowed. Use POST or PUT.";

// clang-format off
const std::vector<ManagementRow> kManagementFixture = {
    {"/api", "/api/", nullptr, kEveryVerb, Guard::None, "", WrongVerb::None, "", kEveryVerb},
    {"/management/v1/description", "/management/description", nullptr, kVerbGet | kVerbPost | kVerbPut,
     Guard::BeforeVerbCheck, "server description", WrongVerb::AlpacaText,
     "Unsupported HTTP method for description endpoint", kVerbGet | kVerbPost | kVerbPut},
    {"/management/v1/apiversions", "/management/apiversions", nullptr, kVerbGet, Guard::None, "",
     WrongVerb::AlpacaText, "Unsupported HTTP method for apiversions endpoint", kVerbGet},
    {"/management/v1/buildinfo", "/management/buildinfo", nullptr, kEveryVerb, Guard::None, "", WrongVerb::None, "",
     kEveryVerb},
    {"/management/v1/configureddevices", "/management/configureddevices", nullptr, kVerbGet, Guard::None, "",
     WrongVerb::AlpacaText, "Unsupported HTTP method for configureddevices endpoint", kVerbGet},
    {"/management/v1/faileddevices", "/management/faileddevices", nullptr, kVerbGet, Guard::None, "",
     WrongVerb::MethodNotAllowed, "Unsupported HTTP method for faileddevices endpoint", kVerbGet},
    {"/management/v1/devicecatalog", "/management/devicecatalog", nullptr, kVerbGet, Guard::None, "",
     WrongVerb::MethodNotAllowed, "Unsupported HTTP method for devicecatalog endpoint", kVerbGet},
    {"/management/v1/thermalcameras", "/management/thermalcameras", nullptr, kVerbGet, Guard::None, "",
     WrongVerb::MethodNotAllowed, "Unsupported HTTP method for thermalcameras endpoint", kVerbGet},
    {"/management/v1/attachedcameras", "/management/attachedcameras", nullptr, kVerbGet, Guard::None, "",
     WrongVerb::MethodNotAllowed, "Unsupported HTTP method for attachedcameras endpoint", kVerbGet},
    {"/management/v1/attacheddevices", "/management/attacheddevices", nullptr, kVerbGet, Guard::None, "",
     WrongVerb::MethodNotAllowed, "Unsupported HTTP method for attacheddevices endpoint", kVerbGet},
    {"/management/v1/networkadapters", "/management/networkadapters", nullptr, kVerbGet, Guard::None, "",
     WrongVerb::MethodNotAllowed, "Unsupported HTTP method for networkadapters endpoint", kVerbGet},
    {"/management/v1/configuredevice", "/management/configuredevice", nullptr, kVerbPost | kVerbPut,
     Guard::BeforeVerbCheck, "device configuration", WrongVerb::MethodNotAllowed, kPostPut, kVerbPost | kVerbPut},
    {"/management/v1/removedevice", "/management/removedevice", nullptr, kVerbPost | kVerbPut,
     Guard::BeforeVerbCheck, "device removal", WrongVerb::MethodNotAllowed, kPostPut, kVerbPost | kVerbPut},
    {"/management/v1/updatedevice", "/management/updatedevice", nullptr, kVerbPost | kVerbPut,
     Guard::BeforeVerbCheck, "device update", WrongVerb::MethodNotAllowed, kPostPut, kVerbPost | kVerbPut},
    {"/management/v1/loglevel", "/management/loglevel", nullptr, kVerbGet | kVerbPost | kVerbPut,
     Guard::BeforeVerbCheck, "log level", WrongVerb::MethodNotAllowed,
     "Unsupported HTTP method for log level endpoint", kVerbGet | kVerbPost | kVerbPut},
    {"/management/v1/logs", "/management/logs", nullptr, kVerbGet, Guard::None, "", WrongVerb::MethodNotAllowed,
     "Unsupported HTTP method for logs endpoint", kVerbGet},
    // The collection DELETE removes every log file: never sent without an Origin.
    {"/management/v1/logfiles", "/management/logfiles", nullptr, kVerbGet | kVerbDelete, Guard::AfterVerbCheck,
     "log files", WrongVerb::MethodNotAllowed, "Unsupported HTTP method for log files endpoint", kVerbGet},
    // An invalid file name: GET and DELETE are refused after the verb check.
    {"/management/v1/logfiles/{name}", "/management/logfiles/{name}", "not-a-log-file", kVerbGet | kVerbDelete,
     Guard::BeforeVerbCheck, "log file", WrongVerb::MethodNotAllowed, "Unsupported HTTP method for log file endpoint",
     kVerbGet | kVerbDelete},
    // No shutdown or restart callback is installed, and synctime refuses a missing Epoch.
    {"/management/v1/shutdown", "/management/shutdown", nullptr, kVerbPost | kVerbPut, Guard::BeforeVerbCheck,
     "shutdown", WrongVerb::MethodNotAllowed, kPostPut, kVerbPost | kVerbPut},
    {"/management/v1/restart", "/management/restart", nullptr, kVerbPost | kVerbPut, Guard::BeforeVerbCheck,
     "restart", WrongVerb::MethodNotAllowed, kPostPut, kVerbPost | kVerbPut},
    {"/management/v1/synctime", "/management/synctime", nullptr, kVerbGet | kVerbPost | kVerbPut,
     Guard::BeforeVerbCheck, "time synchronisation", WrongVerb::MethodNotAllowed,
     "Method not allowed. GET reads the server time; POST or PUT sets it.", kVerbGet | kVerbPost | kVerbPut, true},
    // Sub-routed: the handler checks the verb per sub-path (test_routing covers them). Only the guard is probed.
    {"/management/v1/wifi/{sub}", "/management/wifi/{sub}", "status", kVerbGet | kVerbPost | kVerbPut | kVerbDelete,
     Guard::BeforeVerbCheck, "WiFi management", WrongVerb::Handler, "", 0},
    {"/management/v1/update/{sub}", "/management/update/{sub}", "check", kVerbGet | kVerbPost | kVerbPut,
     Guard::BeforeVerbCheck, "software update", WrongVerb::Handler, "", 0},
};
// clang-format on

// The Allow header of the 405 family: the accepted verbs in this order.
std::string allow_list(unsigned verbs) {
    std::string allow;
    for (const auto& [bit, name] : {std::pair<unsigned, const char*>{kVerbGet, "GET"},
                                    {kVerbPost, "POST"},
                                    {kVerbPut, "PUT"},
                                    {kVerbDelete, "DELETE"}}) {
        if ((verbs & bit) != 0) {
            allow += allow.empty() ? std::string(name) : std::string(", ") + name;
        }
    }
    return allow;
}

std::string describe(const Answer& a) {
    return "HTTP " + std::to_string(a.status) + " error " + std::to_string(a.error_number) + " type '" +
           a.content_type + "' Allow '" + a.allow + "' '" + a.message + "'";
}

bool is_wrong_verb_answer(const Answer& a) {
    return a.status == 405 || (a.status == 400 && a.content_type.find("text/plain") == 0 &&
                               starts_with(a.message, "Unsupported HTTP method for "));
}

// Empty when `a` is the row's wrong-verb answer, else what was wanted.
std::string wrong_verb_mismatch(const ManagementRow& row, const Answer& a) {
    if (row.wrong_verb == WrongVerb::AlpacaText) {
        // Alpaca API Reference s2.5 (#1508): 400 with a text message, no envelope.
        if (a.status == 400 && a.error_number == kNoEnvelope && a.content_type.find("text/plain") == 0 &&
            a.message == row.wrong_verb_message && a.allow.empty()) {
            return "";
        }
        return "want 400 text/plain '" + std::string(row.wrong_verb_message) + "'";
    }
    // The AlpacaBridge-only paths (#2484): 405 with Allow and InvalidOperation.
    if (a.status == 405 && a.error_number == alpacacore::AlpacaError::InvalidOperation &&
        a.content_type.find("application/json") == 0 && a.message == row.wrong_verb_message &&
        a.allow == allow_list(row.verbs)) {
        return "";
    }
    return "want 405 Allow '" + allow_list(row.verbs) + "' InvalidOperation '" + row.wrong_verb_message + "'";
}

void check_management_routes(alpacahttp::Router& router) {
    std::vector<std::string> failures;
    std::size_t probes = 0;
    const std::vector<std::pair<std::string, unsigned>> verbs = {
        {"GET", kVerbGet}, {"POST", kVerbPost}, {"PUT", kVerbPut}, {"DELETE", kVerbDelete}, {"PATCH", kVerbOther}};
    for (const ManagementRow& row : kManagementFixture) {
        // A route that accepts a verb other than GET changes state, so it is guarded. The WrongVerb::None rows
        // (/api and buildinfo) answer every verb alike and change nothing.
        if (row.wrong_verb != WrongVerb::None && (row.verbs & ~kVerbGet) != 0 && row.guard == Guard::None) {
            failures.push_back(std::string(row.path) + ": accepts a state-changing verb with no origin guard");
        }
        const std::string guard_message = "Cross-origin " + std::string(row.guard_what) + " requests are not allowed";
        for (const char* spelling : {row.path, row.alias}) {
            std::string path(spelling);
            if (row.tail != nullptr) {
                path = path.substr(0, path.find('{')) + row.tail;
            }
            for (const auto& [verb, bit] : verbs) {
                const bool accepted = (row.verbs & bit) != 0;
                const bool safe = (row.safe_verbs & bit) != 0;
                const std::string where = verb + " " + path;

                // No Origin: a wrong verb gets the family's answer, an accepted one reaches the handler.
                if (row.wrong_verb != WrongVerb::Handler && (!accepted || safe)) {
                    // A wrong verb carries a body ID, which only a body_transaction_id row echoes.
                    const Answer a = send(router, verb, path, accepted ? "" : R"({"ClientTransactionID": 4242})");
                    ++probes;
                    if (!accepted) {
                        if (const std::string why = wrong_verb_mismatch(row, a); !why.empty()) {
                            failures.push_back(where + ": " + describe(a) + ", " + why);
                        }
                        const unsigned echoed = row.body_transaction_id ? 4242U : 0U;
                        if (row.wrong_verb == WrongVerb::MethodNotAllowed && a.client_tx_id != echoed) {
                            failures.push_back(where + ": ClientTransactionID " + std::to_string(a.client_tx_id) +
                                               ", want " + std::to_string(echoed));
                        }
                    } else if (a.status == 403 || is_wrong_verb_answer(a)) {
                        failures.push_back(where + ": accepted verb refused, " + describe(a));
                    }
                }

                // A foreign Origin. The guard passes every GET.
                const bool guarded = bit != kVerbGet && (row.guard == Guard::BeforeVerbCheck ||
                                                         (row.guard == Guard::AfterVerbCheck && accepted));
                if (!guarded && (row.wrong_verb == WrongVerb::Handler || (accepted && !safe))) {
                    continue;
                }
                const Answer a = send(router, verb, path, std::string(), true);
                ++probes;
                if (guarded) {
                    if (a.status != 403 || a.error_number != kInvalidValue || a.message != guard_message) {
                        failures.push_back(where + " cross-origin: " + describe(a) + ", want 403 '" + guard_message +
                                           "'");
                    }
                } else if (!accepted) {
                    if (const std::string why = wrong_verb_mismatch(row, a); !why.empty()) {
                        failures.push_back(where + " cross-origin: " + describe(a) + ", " + why);
                    }
                } else if (a.status == 403 || is_wrong_verb_answer(a)) {
                    failures.push_back(where + " cross-origin: accepted verb refused, " + describe(a));
                }
            }
        }
    }
    std::cout << "G: " << probes << " probes over " << kManagementFixture.size() << " management routes\n";
    report("G management routes", failures);
}

}  // namespace

int main() {
    alpacahttp::Router router;

    check_type_names(router);
    check_route_table(router);
    check_source_tables();
    {
        // case: every management route answers a wrong verb per its family and refuses a cross-origin write (#2485)
        check_management_routes(router);
    }

    auto& registry = alpacacore::management::DeviceRegistry::instance();
    const auto stubs = make_stubs();
    for (const auto& stub : stubs) {
        EXPECT(registry.register_device(stub));
    }
    check_dispatch_reach(router);
    check_rejections(router);
    for (const auto& stub : stubs) {
        registry.unregister_device(stub->get_device_type(), kProbeDevice);
    }

    std::cout << "test_route_table: all checks passed\n";
    return 0;
}
