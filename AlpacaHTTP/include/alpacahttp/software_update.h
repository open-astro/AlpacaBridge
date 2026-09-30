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

#pragma once

// Software update backend for the /management/v1/update/* endpoints
// (docs/software-update.md).
//
// Two halves, deliberately split by privilege:
//
//  * The CHECK needs no privilege. The daemon fetches the APT repository's
//    Packages index over HTTPS (libcurl, in-process), finds the alpacabridge
//    stanza and compares its Version with the running build using dpkg's
//    version ordering. Nothing is written anywhere.
//
//  * The INSTALL needs root, and the daemon has none: its unit runs as the
//    alpacabridge user under NoNewPrivileges=true, so sudo and setuid cannot
//    work. Instead the daemon asks systemd over the system D-Bus (sd-bus,
//    in-process, no subprocess per project policy) to start the root-owned
//    oneshot unit alpacabridge-update.service, which runs
//    /usr/libexec/alpacabridge/software-update (apt-get update + apt-get
//    install --only-upgrade alpacabridge). The polkit rule shipped as
//    debian/alpacabridge-update.polkit-rules authorises exactly that one
//    unit and the start verb for exactly that user. The daemon passes no
//    arguments: what the helper installs is fixed in the unit file, and apt
//    verifies the repository signature as it would for a manual upgrade.
//
// The helper writes its transcript to kUpdateLogPath, a file in a directory
// that only root can write (the helper unit's own LogsDirectory=), and the
// daemon reads it back for the web UI. It must never live in a directory the
// service user can write, such as the daemon's own log directory: root
// truncating and chmod-ing a path there would follow a symlink the service
// user planted (PR #745 review), and no check in the script can close that
// race. test_software_update pins the path against the packaging files. The upgrade restarts alpacabridge.service
// from the new package's postinst, so the daemon reporting progress is the
// one being replaced; the helper unit is not in its cgroup and survives.
//
// SoftwareUpdateBackend is the seam between policy and the two privileged
// mechanisms: tests inject a scripted backend, production installs
// SystemSoftwareUpdateBackend.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace alpacahttp::util {

// Thrown by every failing operation. `alpaca_error` is the Alpaca error
// number the router should answer with (util::ErrorCode values).
class SoftwareUpdateError : public std::runtime_error {
public:
    SoftwareUpdateError(std::int32_t alpaca_error, const std::string& what)
        : std::runtime_error(what), alpaca_error_(alpaca_error) {}
    std::int32_t alpaca_error() const { return alpaca_error_; }

private:
    std::int32_t alpaca_error_;
};

// dpkg's version ordering (deb-version(7)): [epoch:]upstream[-revision],
// each of upstream and revision compared as alternating non-digit and digit
// runs, with '~' sorting before everything including the end of the string.
// Returns <0 when a sorts before b, 0 when equal, >0 when a sorts after b.
// Pure; the unit test pins it against `dpkg --compare-versions`.
int compare_debian_versions(std::string_view a, std::string_view b);

// The Version of the newest stanza for exactly `package` in an APT Packages
// index (RFC 822-style stanzas separated by blank lines). A stanza for another
// package (alpacabridge-dbgsym) never matches. nullopt when absent.
std::optional<std::string> find_package_version(std::string_view packages_index, std::string_view package);

// What the installer looks like right now, as far as the daemon can tell.
struct InstallerState {
    // "idle": never run since the log was last cleared, or nothing to report.
    // "running": the helper unit is active.
    // "succeeded" / "failed": the last run's recorded result.
    // "unavailable": the helper unit is not installed on this host (a source
    //                build, or a host that is not running the .deb).
    std::string state = "idle";
    // Human-readable systemd detail (ActiveState/SubState/Result), or why the
    // installer is unavailable.
    std::string detail;
    // Tail of the helper's transcript, empty when there is none.
    std::string log;
};

// The durable-record contract behind InstallerState, as a pure function so
// it can be pinned without systemd: `active_state`, `sub_state` and `result`
// are the unit's properties, `job_pending` is whether systemd holds a job for
// the unit (a start queued behind network-online.target, before the helper
// has truncated the previous transcript), `log` is the transcript tail.
//   running   : the unit is active/activating/deactivating, OR a job is pending
//   failed    : ActiveState=failed, OR the transcript's last marker is failure,
//               OR a transcript exists with no marker (killed or rebooted)
//   succeeded : the transcript's last marker is success
//   idle      : inactive with no transcript
InstallerState classify_installer_state(const std::string& active_state, const std::string& sub_state,
                                        const std::string& result, bool job_pending, const std::string& log);

class SoftwareUpdateBackend {
public:
    virtual ~SoftwareUpdateBackend() = default;

    // The body of the Packages index at `url`, or throws SoftwareUpdateError.
    virtual std::string fetch_url(const std::string& url, std::chrono::milliseconds timeout) = 0;

    // Ask systemd to start the helper unit. Throws SoftwareUpdateError with a
    // message an operator can act on (unit missing, polkit refused, ...).
    virtual void start_installer() = 0;

    // Never throws: a backend that cannot tell reports state "unavailable"
    // with the reason in `detail`.
    virtual InstallerState installer_state() = 0;
};

// libcurl for the index fetch, sd-bus (org.freedesktop.systemd1) for the
// helper unit, and the helper's transcript file for the log tail.
class SystemSoftwareUpdateBackend final : public SoftwareUpdateBackend {
public:
    // unit: the helper unit name (kDefaultInstallerUnit in production).
    // log_path: where the helper writes its transcript.
    SystemSoftwareUpdateBackend(std::string unit, std::string log_path);

    std::string fetch_url(const std::string& url, std::chrono::milliseconds timeout) override;
    void start_installer() override;
    InstallerState installer_state() override;

private:
    std::string unit_;
    std::string log_path_;
};

// Everything the manager needs to know about this host and the repository.
struct SoftwareUpdateSettings {
    // The running build (alpacahttp::kVersion).
    std::string installed_version;
    // The Packages index to check (Config::update_packages_url()).
    std::string packages_url;
    // The package name looked up in that index.
    std::string package;
    // Where the plain-language notes for a version live, with "{version}"
    // standing for the version number (Config::update_release_notes_url()).
    // Empty: never fetch notes.
    std::string release_notes_url_template;
    // The human-facing release page for a version, same placeholder
    // (Config::update_release_url()). Empty: no link.
    std::string release_url_template;
};

// "{version}" in `url_template` replaced by `version`, every occurrence.
std::string expand_version_template(std::string url_template, const std::string& version);

// Policy over the backend. Thread-safe. The cached result (latest version,
// check time, error, notes) is read and written under one mutex, and no
// network or sd-bus call runs while it is held: check() fetches (index,
// then notes; each bounded by kFetchTimeout), install() talks to systemd,
// and status() reads the installer state, all before taking the lock, so a
// status() poll never queues behind a slow mirror or a slow polkitd. The
// settings read meanwhile are immutable after construction. A check that
// arrives while another is fetching waits for it and returns its result
// (one fetch, one answer for both callers); an install that arrives while
// another is starting is refused.
class SoftwareUpdateManager {
public:
    static constexpr std::chrono::milliseconds kFetchTimeout{15000};
    // Bytes of the transcript the status payload carries (the tail).
    static constexpr std::size_t kLogTailBytes = 8192;
    // Bytes of release notes kept (the notes files are a few KiB; a wrong
    // URL answering a whole web page must not become the status payload).
    static constexpr std::size_t kReleaseNotesMaxBytes = 65536;

    SoftwareUpdateManager(SoftwareUpdateSettings settings, std::unique_ptr<SoftwareUpdateBackend> backend);

    // {"InstalledVersion", "CheckEnabled" (false when packages_url is empty),
    //  "LatestVersion" (null until a check succeeded),
    //  "UpdateAvailable", "CheckedAt" (Unix seconds, null until checked),
    //  "CheckError" (null unless the last check failed), "PackagesUrl",
    //  "ReleaseNotes" (the newer version's plain-language notes, Markdown;
    //  null when no update is available or the notes could not be fetched),
    //  "ReleaseNotesUrl", "ReleaseUrl" (both null without an update),
    //  "Installer": {"State", "Detail", "Log"}}
    nlohmann::json status();

    // Fetch the index, compare, cache, and return status(). Throws
    // SoftwareUpdateError (NOT_IMPLEMENTED) when packages_url is empty (the
    // check is disabled; nothing is fetched or recorded), and (DRIVER_ERROR)
    // on a failed fetch or an index with no stanza for the package; that
    // failure is also recorded as CheckError. Single-flight: a check that
    // arrives while another is fetching waits for it and returns its result
    // instead of fetching again, so repeated presses from several tabs pin
    // one worker thread, not one per press.
    // When a newer version is found the release notes are fetched too, best
    // effort: a failure there is logged and leaves ReleaseNotes null, since
    // the operator can still read them on the release page.
    nlohmann::json check();

    // Start the installer. Throws INVALID_OPERATION when no successful check
    // has found a newer version, or when the installer is already running or
    // being started; otherwise whatever the backend throws. Returns status()
    // afterwards. The sd-bus calls run outside mutex_ like the fetches.
    nlohmann::json install();

private:
    // `installer` is the backend's answer, read by the caller BEFORE taking
    // mutex_ (an sd-bus round trip must not run under the lock).
    nlohmann::json status_locked(const InstallerState& installer);
    bool update_available_locked() const;

    std::mutex mutex_;
    // Single-flight state for check() and install(), under mutex_; cv_ wakes
    // waiters when a check finishes.
    std::condition_variable cv_;
    bool check_in_flight_ = false;
    bool install_in_flight_ = false;
    SoftwareUpdateSettings settings_;
    std::unique_ptr<SoftwareUpdateBackend> backend_;

    std::optional<std::string> latest_version_;
    std::optional<std::chrono::system_clock::time_point> checked_at_;
    std::optional<std::string> check_error_;
    std::optional<std::string> release_notes_;
};

inline constexpr const char* kDefaultInstallerUnit = "alpacabridge-update.service";
inline constexpr const char* kDefaultPackageName = "alpacabridge";
inline constexpr const char* kDefaultPackagesUrl = "https://apt.openastro.net/dists/trixie/main/binary-arm64/Packages";
// Written by debian/alpacabridge-software-update under the helper unit's
// LogsDirectory=alpacabridge-update (root:root 0755). Fixed, not derived from
// the daemon's log directory: see the file comment above.
inline constexpr const char* kUpdateLogPath = "/var/log/alpacabridge-update/update.log";
// The plain-language notes /bump-release writes for every release, read at
// the release tag; the GitHub Release body is built from the same file.
inline constexpr const char* kDefaultReleaseNotesUrl =
    "https://raw.githubusercontent.com/open-astro/AlpacaBridge/v{version}/docs/releases/{version}.md";
inline constexpr const char* kDefaultReleaseUrl = "https://github.com/open-astro/AlpacaBridge/releases/tag/v{version}";

}  // namespace alpacahttp::util
