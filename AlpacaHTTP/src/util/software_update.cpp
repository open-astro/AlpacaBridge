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

#include "alpacahttp/software_update.h"

#include <alpacacore/util/serial_io.h>
#include <curl/curl.h>
#include <systemd/sd-bus.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

#include "alpacahttp/util/error_mapping.h"
#include "alpacahttp/util/logging_adapter.h"
#include "alpacahttp/version.h"

namespace alpacahttp::util {

// ---------------------------------------------------------------------------
// dpkg version ordering
// ---------------------------------------------------------------------------

namespace {

// dpkg's character weight for the non-digit runs (lib/dpkg/version.c,
// order()): '~' sorts before everything (even the end of the string), letters
// sort before non-letters, the end of a run weighs 0.
int debian_char_order(char c) {
    if (c == '~') return -1;
    if (std::isdigit(static_cast<unsigned char>(c))) return 0;
    if (std::isalpha(static_cast<unsigned char>(c))) return static_cast<unsigned char>(c);
    return static_cast<unsigned char>(c) + 256;
}

// verrevcmp(): alternate a non-digit run and a digit run until both strings
// are exhausted.
int compare_version_fragment(std::string_view a, std::string_view b) {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() || j < b.size()) {
        int first_diff = 0;
        while ((i < a.size() && !std::isdigit(static_cast<unsigned char>(a[i]))) ||
               (j < b.size() && !std::isdigit(static_cast<unsigned char>(b[j])))) {
            const int ac = i < a.size() ? debian_char_order(a[i]) : 0;
            const int bc = j < b.size() ? debian_char_order(b[j]) : 0;
            if (ac != bc) return ac - bc;
            ++i;
            ++j;
        }
        while (i < a.size() && a[i] == '0') ++i;
        while (j < b.size() && b[j] == '0') ++j;
        while (i < a.size() && std::isdigit(static_cast<unsigned char>(a[i])) && j < b.size() &&
               std::isdigit(static_cast<unsigned char>(b[j]))) {
            if (first_diff == 0) first_diff = a[i] - b[j];
            ++i;
            ++j;
        }
        if (i < a.size() && std::isdigit(static_cast<unsigned char>(a[i]))) return 1;
        if (j < b.size() && std::isdigit(static_cast<unsigned char>(b[j]))) return -1;
        if (first_diff != 0) return first_diff;
    }
    return 0;
}

struct DebianVersion {
    unsigned long epoch = 0;
    std::string_view upstream;
    std::string_view revision;
};

DebianVersion split_debian_version(std::string_view v) {
    DebianVersion out;
    const auto colon = v.find(':');
    if (colon != std::string_view::npos) {
        const std::string epoch_text(v.substr(0, colon));
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(epoch_text.c_str(), &end, 10);
        if (end && *end == '\0' && !epoch_text.empty()) {
            out.epoch = parsed;
            v = v.substr(colon + 1);
        }
    }
    const auto hyphen = v.rfind('-');
    if (hyphen != std::string_view::npos) {
        out.upstream = v.substr(0, hyphen);
        out.revision = v.substr(hyphen + 1);
    } else {
        out.upstream = v;
    }
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

}  // namespace

int compare_debian_versions(std::string_view a, std::string_view b) {
    const DebianVersion va = split_debian_version(trim(a));
    const DebianVersion vb = split_debian_version(trim(b));
    if (va.epoch != vb.epoch) return va.epoch < vb.epoch ? -1 : 1;
    if (const int r = compare_version_fragment(va.upstream, vb.upstream); r != 0) return r;
    return compare_version_fragment(va.revision, vb.revision);
}

// ---------------------------------------------------------------------------
// Packages index
// ---------------------------------------------------------------------------

std::optional<std::string> find_package_version(std::string_view packages_index, std::string_view package) {
    std::optional<std::string> best;
    std::string current_package;
    std::string current_version;

    auto finish_stanza = [&]() {
        if (current_package == package && !current_version.empty()) {
            if (!best || compare_debian_versions(current_version, *best) > 0) {
                best = current_version;
            }
        }
        current_package.clear();
        current_version.clear();
    };

    std::size_t pos = 0;
    while (pos <= packages_index.size()) {
        const auto nl = packages_index.find('\n', pos);
        std::string_view line =
            nl == std::string_view::npos ? packages_index.substr(pos) : packages_index.substr(pos, nl - pos);
        pos = nl == std::string_view::npos ? packages_index.size() + 1 : nl + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        if (trim(line).empty()) {
            finish_stanza();
            continue;
        }
        // Continuation lines (leading whitespace) belong to the previous
        // field; none of the fields read here are multi-line.
        if (std::isspace(static_cast<unsigned char>(line.front()))) continue;
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        const std::string_view key = trim(line.substr(0, colon));
        const std::string_view value = trim(line.substr(colon + 1));
        if (key == "Package") {
            current_package = std::string(value);
        } else if (key == "Version") {
            current_version = std::string(value);
        }
    }
    finish_stanza();
    return best;
}

// ---------------------------------------------------------------------------
// Production backend: libcurl + sd-bus + transcript file
// ---------------------------------------------------------------------------

namespace {

constexpr const char* kSystemdService = "org.freedesktop.systemd1";
constexpr const char* kSystemdManagerPath = "/org/freedesktop/systemd1";
constexpr const char* kSystemdManagerIface = "org.freedesktop.systemd1.Manager";
constexpr const char* kSystemdUnitIface = "org.freedesktop.systemd1.Unit";
constexpr const char* kSystemdServiceIface = "org.freedesktop.systemd1.Service";

// The helper script's own markers (debian/alpacabridge-software-update).
constexpr const char* kResultSuccessMarker = "=== RESULT: success";
constexpr const char* kResultFailureMarker = "=== RESULT: failure";

std::size_t curl_write_to_string(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    if (!userdata) return 0;
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

void ensure_curl_global_init() {
    static std::once_flag flag;
    std::call_once(flag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct CurlHandle {
    CURL* handle = nullptr;
    ~CurlHandle() {
        if (handle) curl_easy_cleanup(handle);
    }
};

struct BusHandle {
    sd_bus* bus = nullptr;
    ~BusHandle() {
        if (bus) sd_bus_unref(bus);
    }
};

struct BusError {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    ~BusError() { sd_bus_error_free(&err); }
    std::string name() const { return err.name ? err.name : ""; }
    std::string message(int r) const {
        return err.message ? std::string(err.message) : alpacacore::util::errno_string(-r);
    }
};

struct BusMessage {
    sd_bus_message* msg = nullptr;
    ~BusMessage() {
        if (msg) sd_bus_message_unref(msg);
    }
};

// Read one string property of the unit; empty on failure (the callers treat
// an unreadable property as "unknown", never as an error).
std::string unit_property_string(sd_bus* bus, const std::string& path, const char* iface, const char* prop) {
    BusError err;
    char* value = nullptr;
    const int r = sd_bus_get_property_string(bus, kSystemdService, path.c_str(), iface, prop, &err.err, &value);
    if (r < 0 || !value) return "";
    std::string out(value);
    std::free(value);
    return out;
}

std::string read_log_tail(const std::string& path, std::size_t max_bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return "";
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size <= 0) return "";
    const auto total = static_cast<std::size_t>(size);
    const std::size_t start = total > max_bytes ? total - max_bytes : 0;
    in.seekg(static_cast<std::streamoff>(start), std::ios::beg);
    std::string out(total - start, '\0');
    in.read(out.data(), static_cast<std::streamsize>(out.size()));
    out.resize(static_cast<std::size_t>(in.gcount()));
    if (start > 0) {
        // Drop the partial first line so the tail starts on a line boundary.
        const auto nl = out.find('\n');
        if (nl != std::string::npos) out.erase(0, nl + 1);
    }
    return out;
}

// The last result marker in the transcript, if any: 1 = success, -1 =
// failure, 0 = none.
int last_result_marker(const std::string& log) {
    const auto ok = log.rfind(kResultSuccessMarker);
    const auto bad = log.rfind(kResultFailureMarker);
    if (ok == std::string::npos && bad == std::string::npos) return 0;
    if (ok == std::string::npos) return -1;
    if (bad == std::string::npos) return 1;
    return ok > bad ? 1 : -1;
}

}  // namespace

SystemSoftwareUpdateBackend::SystemSoftwareUpdateBackend(std::string unit, std::string log_path)
    : unit_(std::move(unit)), log_path_(std::move(log_path)) {}

std::string SystemSoftwareUpdateBackend::fetch_url(const std::string& url, std::chrono::milliseconds timeout) {
    ensure_curl_global_init();
    CurlHandle curl;
    curl.handle = curl_easy_init();
    if (!curl.handle) {
        throw SoftwareUpdateError(ErrorCode::DRIVER_ERROR, "Update check failed: could not initialise the HTTP client");
    }

    std::string body;
    const std::string user_agent = std::string("AlpacaBridge/") + alpacahttp::kVersion;
    curl_easy_setopt(curl.handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.handle, CURLOPT_MAXREDIRS, 5L);
    // The URLs are operator-configured, but a redirect must never reach a
    // scheme other than http(s) (file://, ftp://, ...).
    curl_easy_setopt(curl.handle, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl.handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl.handle, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout.count()));
    curl_easy_setopt(curl.handle, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    // Multi-threaded process: never let libcurl install signal handlers for
    // its resolver timeouts.
    curl_easy_setopt(curl.handle, CURLOPT_NOSIGNAL, 1L);
    // A Packages index for this repository is a few KiB; refuse anything that
    // is clearly not one rather than buffering it.
    curl_easy_setopt(curl.handle, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(8 * 1024 * 1024));
    curl_easy_setopt(curl.handle, CURLOPT_WRITEFUNCTION, curl_write_to_string);
    curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl.handle, CURLOPT_USERAGENT, user_agent.c_str());

    const CURLcode result = curl_easy_perform(curl.handle);
    if (result != CURLE_OK) {
        throw SoftwareUpdateError(ErrorCode::DRIVER_ERROR,
                                  std::string("Update check failed: ") + curl_easy_strerror(result) + " (" + url + ")");
    }
    long status_code = 0;
    curl_easy_getinfo(curl.handle, CURLINFO_RESPONSE_CODE, &status_code);
    if (status_code >= 400) {
        throw SoftwareUpdateError(ErrorCode::DRIVER_ERROR, "Update check failed: the repository answered HTTP " +
                                                               std::to_string(status_code) + " for " + url);
    }
    return body;
}

void SystemSoftwareUpdateBackend::start_installer() {
    BusHandle bus;
    int r = sd_bus_open_system(&bus.bus);
    if (r < 0) {
        throw SoftwareUpdateError(
            ErrorCode::DRIVER_ERROR,
            "Update install failed: cannot reach systemd on the system bus: " + alpacacore::util::errno_string(-r));
    }
    BusError err;
    BusMessage reply;
    r = sd_bus_call_method(bus.bus, kSystemdService, kSystemdManagerPath, kSystemdManagerIface, "StartUnit", &err.err,
                           &reply.msg, "ss", unit_.c_str(), "replace");
    if (r < 0) {
        const std::string name = err.name();
        std::string message = "Update install failed: could not start " + unit_ + ": " + err.message(r);
        if (name == "org.freedesktop.systemd1.NoSuchUnit" || name == "org.freedesktop.systemd1.LoadFailed") {
            message +=
                ". The helper unit ships in the alpacabridge .deb; on a source build run 'sudo apt update && "
                "sudo apt install --only-upgrade alpacabridge' instead.";
        } else if (name == "org.freedesktop.DBus.Error.AccessDenied" ||
                   name == "org.freedesktop.DBus.Error.InteractiveAuthorizationRequired") {
            message +=
                ". The alpacabridge user is not authorised to start it: check that polkitd is installed and "
                "/usr/share/polkit-1/rules.d/50-alpacabridge-update.rules is present.";
        }
        throw SoftwareUpdateError(ErrorCode::DRIVER_ERROR, message);
    }
    log_info("Software update: started " + unit_);
}

InstallerState classify_installer_state(const std::string& active_state, const std::string& sub_state,
                                        const std::string& result, bool job_pending, const std::string& log) {
    InstallerState state;
    state.log = log;
    state.detail = "ActiveState=" + active_state + " SubState=" + sub_state +
                   (result.empty() ? "" : " Result=" + result) + (job_pending ? " Job=pending" : "");
    if (job_pending || active_state == "activating" || active_state == "active" || active_state == "deactivating" ||
        active_state == "reloading") {
        state.state = "running";
        return state;
    }
    if (active_state == "failed") {
        state.state = "failed";
        return state;
    }
    // Inactive: the transcript's own marker is the durable record, since a
    // finished oneshot unit is garbage-collected and reads as never-run.
    switch (last_result_marker(log)) {
        case 1:
            state.state = "succeeded";
            break;
        case -1:
            state.state = "failed";
            break;
        default:
            if (!log.empty()) {
                // Started but never reached a result: killed (TimeoutStartSec)
                // or the host rebooted mid-run.
                state.state = "failed";
                state.detail += " (no result recorded)";
            } else {
                state.state = "idle";
            }
            break;
    }
    return state;
}

InstallerState SystemSoftwareUpdateBackend::installer_state() {
    InstallerState state;
    state.log = read_log_tail(log_path_, SoftwareUpdateManager::kLogTailBytes);

    BusHandle bus;
    int r = sd_bus_open_system(&bus.bus);
    if (r < 0) {
        state.state = "unavailable";
        state.detail = "cannot reach systemd on the system bus: " + alpacacore::util::errno_string(-r);
        return state;
    }
    // LoadUnit rather than GetUnit: a unit that has never been started (or
    // that systemd garbage-collected after a run) is not loaded, and GetUnit
    // answers NoSuchUnit for it. LoadUnit is unprivileged and answers for any
    // unit file that exists.
    BusError err;
    BusMessage reply;
    r = sd_bus_call_method(bus.bus, kSystemdService, kSystemdManagerPath, kSystemdManagerIface, "LoadUnit", &err.err,
                           &reply.msg, "s", unit_.c_str());
    if (r < 0) {
        state.state = "unavailable";
        state.detail = unit_ + ": " + err.message(r);
        return state;
    }
    const char* path = nullptr;
    if (sd_bus_message_read(reply.msg, "o", &path) <= 0 || !path) {
        state.state = "unavailable";
        state.detail = unit_ + ": systemd returned no object path";
        return state;
    }
    const std::string unit_path(path);

    const std::string load_state = unit_property_string(bus.bus, unit_path, kSystemdUnitIface, "LoadState");
    if (load_state != "loaded") {
        state.state = "unavailable";
        state.detail = unit_ + " is not installed on this host (LoadState=" + (load_state.empty() ? "?" : load_state) +
                       "); on a source build update with apt instead";
        return state;
    }
    const std::string active = unit_property_string(bus.bus, unit_path, kSystemdUnitIface, "ActiveState");
    const std::string sub = unit_property_string(bus.bus, unit_path, kSystemdUnitIface, "SubState");
    const std::string result = unit_property_string(bus.bus, unit_path, kSystemdServiceIface, "Result");
    // Job: (uo) with id 0 when nothing is queued. Right after StartUnit the
    // unit can still read inactive while its start job waits on
    // network-online.target, before the helper truncates the old transcript;
    // without this the previous run's marker would be reported as the result.
    bool job_pending = false;
    {
        BusError job_err;
        BusMessage job_msg;
        if (sd_bus_get_property(bus.bus, kSystemdService, unit_path.c_str(), kSystemdUnitIface, "Job", &job_err.err,
                                &job_msg.msg, "(uo)") >= 0) {
            std::uint32_t job_id = 0;
            const char* job_path = nullptr;
            if (sd_bus_message_read(job_msg.msg, "(uo)", &job_id, &job_path) > 0) job_pending = job_id != 0;
        }
    }
    const InstallerState classified = classify_installer_state(active, sub, result, job_pending, state.log);
    state.state = classified.state;
    state.detail = classified.detail;
    return state;
}

// ---------------------------------------------------------------------------
// Manager
// ---------------------------------------------------------------------------

std::string expand_version_template(std::string url_template, const std::string& version) {
    const std::string placeholder = "{version}";
    std::size_t pos = 0;
    while ((pos = url_template.find(placeholder, pos)) != std::string::npos) {
        url_template.replace(pos, placeholder.size(), version);
        pos += version.size();
    }
    return url_template;
}

SoftwareUpdateManager::SoftwareUpdateManager(SoftwareUpdateSettings settings,
                                             std::unique_ptr<SoftwareUpdateBackend> backend)
    : settings_(std::move(settings)), backend_(std::move(backend)) {}

bool SoftwareUpdateManager::update_available_locked() const {
    return latest_version_ && compare_debian_versions(*latest_version_, settings_.installed_version) > 0;
}

nlohmann::json SoftwareUpdateManager::status_locked(const InstallerState& installer) {
    const bool available = update_available_locked();
    nlohmann::json out;
    out["InstalledVersion"] = settings_.installed_version;
    out["LatestVersion"] = latest_version_ ? nlohmann::json(*latest_version_) : nlohmann::json(nullptr);
    out["UpdateAvailable"] = available;
    out["CheckedAt"] =
        checked_at_
            ? nlohmann::json(std::chrono::duration_cast<std::chrono::seconds>(checked_at_->time_since_epoch()).count())
            : nlohmann::json(nullptr);
    out["CheckError"] = check_error_ ? nlohmann::json(*check_error_) : nlohmann::json(nullptr);
    out["PackagesUrl"] = settings_.packages_url;
    out["CheckEnabled"] = !settings_.packages_url.empty();
    out["ReleaseNotes"] = available && release_notes_ ? nlohmann::json(*release_notes_) : nlohmann::json(nullptr);
    // `available` implies latest_version_ is set; spelled out for clang-tidy.
    const std::string latest = latest_version_.value_or("");
    out["ReleaseNotesUrl"] = available && !settings_.release_notes_url_template.empty()
                                 ? nlohmann::json(expand_version_template(settings_.release_notes_url_template, latest))
                                 : nlohmann::json(nullptr);
    out["ReleaseUrl"] = available && !settings_.release_url_template.empty()
                            ? nlohmann::json(expand_version_template(settings_.release_url_template, latest))
                            : nlohmann::json(nullptr);
    out["Installer"] = nlohmann::json{{"State", installer.state}, {"Detail", installer.detail}, {"Log", installer.log}};
    return out;
}

nlohmann::json SoftwareUpdateManager::status() {
    const InstallerState installer = backend_->installer_state();
    std::lock_guard<std::mutex> lock(mutex_);
    return status_locked(installer);
}

nlohmann::json SoftwareUpdateManager::check() {
    if (settings_.packages_url.empty()) {
        // Disabled by configuration (update_packages_url empty). Nothing is
        // fetched and nothing is recorded; status() says CheckEnabled=false.
        throw SoftwareUpdateError(ErrorCode::NOT_IMPLEMENTED,
                                  "Checking for updates is turned off in the server configuration "
                                  "(update_packages_url is empty)");
    }
    // Single flight: a check that arrives while another is fetching waits
    // for that one and returns its result (a failure there is reported as
    // CheckError in the returned status, not thrown here).
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (check_in_flight_) {
            cv_.wait(lock, [&] { return !check_in_flight_; });
            lock.unlock();
            return status();
        }
        check_in_flight_ = true;
    }
    struct ClearInFlight {
        SoftwareUpdateManager& self;
        ~ClearInFlight() {
            std::lock_guard<std::mutex> lock(self.mutex_);
            self.check_in_flight_ = false;
            self.cv_.notify_all();
        }
    } clear_in_flight{*this};

    // The fetches run WITHOUT mutex_: each can take up to kFetchTimeout, and
    // a status poll from the page must not queue behind a slow mirror. The
    // settings are immutable after construction, so they are read lock-free;
    // only the cached result is written under the lock.
    std::string index;
    try {
        index = backend_->fetch_url(settings_.packages_url, kFetchTimeout);
    } catch (const SoftwareUpdateError& e) {
        std::lock_guard<std::mutex> lock(mutex_);
        checked_at_ = std::chrono::system_clock::now();
        check_error_ = e.what();
        log_warning(e.what());
        throw;
    }
    const auto version = find_package_version(index, settings_.package);
    if (!version) {
        SoftwareUpdateError error(ErrorCode::DRIVER_ERROR, "Update check failed: no '" + settings_.package +
                                                               "' package listed at " + settings_.packages_url);
        std::lock_guard<std::mutex> lock(mutex_);
        checked_at_ = std::chrono::system_clock::now();
        check_error_ = error.what();
        log_warning(error.what());
        throw error;
    }
    const bool available = compare_debian_versions(*version, settings_.installed_version) > 0;
    log_info("Software update check: installed " + settings_.installed_version + ", repository " + *version +
             (available ? " (update available)" : " (up to date)"));

    std::optional<std::string> notes;
    if (available && !settings_.release_notes_url_template.empty()) {
        const std::string notes_url = expand_version_template(settings_.release_notes_url_template, *version);
        try {
            std::string text = backend_->fetch_url(notes_url, kFetchTimeout);
            if (text.size() > kReleaseNotesMaxBytes) {
                text.resize(kReleaseNotesMaxBytes);
                text += "\n\n(notes truncated)\n";
            }
            notes = std::move(text);
        } catch (const SoftwareUpdateError& e) {
            // Best effort: the release page still has them.
            log_warning("Software update: release notes for " + *version + " not available (" + e.what() + ")");
        }
    }

    const InstallerState installer = backend_->installer_state();
    std::lock_guard<std::mutex> lock(mutex_);
    latest_version_ = *version;
    checked_at_ = std::chrono::system_clock::now();
    check_error_.reset();
    release_notes_ = std::move(notes);
    return status_locked(installer);
}

nlohmann::json SoftwareUpdateManager::install() {
    std::string latest;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!update_available_locked()) {
            throw SoftwareUpdateError(ErrorCode::INVALID_OPERATION,
                                      "No update is available to install; check for updates first");
        }
        if (install_in_flight_) {
            throw SoftwareUpdateError(ErrorCode::INVALID_OPERATION, "An update is already being installed");
        }
        install_in_flight_ = true;
        latest = latest_version_.value_or("");
    }
    struct ClearInFlight {
        SoftwareUpdateManager& self;
        ~ClearInFlight() {
            std::lock_guard<std::mutex> lock(self.mutex_);
            self.install_in_flight_ = false;
        }
    } clear_in_flight{*this};

    // sd-bus round trips outside mutex_ (a slow polkitd during boot must not
    // block status polls); install_in_flight_ keeps a second install out.
    const InstallerState current = backend_->installer_state();
    if (current.state == "running") {
        throw SoftwareUpdateError(ErrorCode::INVALID_OPERATION, "An update is already being installed");
    }
    if (current.state == "unavailable") {
        throw SoftwareUpdateError(ErrorCode::NOT_IMPLEMENTED,
                                  "Update install is not available on this host: " + current.detail);
    }
    log_info("Software update: installing " + settings_.package + " " + latest + " over " +
             settings_.installed_version);
    backend_->start_installer();
    const InstallerState started = backend_->installer_state();
    std::lock_guard<std::mutex> lock(mutex_);
    return status_locked(started);
}

}  // namespace alpacahttp::util
