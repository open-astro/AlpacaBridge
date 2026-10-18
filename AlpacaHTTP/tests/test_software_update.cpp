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

// Software update (docs/software-update.md): dpkg version ordering, Packages
// index parsing, and the manager's policy over a scripted backend. The
// production backend (libcurl + sd-bus) is not exercised here; test_routing
// covers the endpoints over the same scripted backend.

#include <alpacahttp/software_update.h>
#include <alpacahttp/util/error_mapping.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "test_assert.h"

namespace {

using alpacahttp::util::compare_debian_versions;
using alpacahttp::util::find_package_version;
using alpacahttp::util::InstallerState;
using alpacahttp::util::SoftwareUpdateBackend;
using alpacahttp::util::SoftwareUpdateError;
using alpacahttp::util::SoftwareUpdateManager;

// Scripted backend: canned index (or a throw), a recorded start, a settable
// installer state.
class FakeBackend final : public SoftwareUpdateBackend {
public:
    std::string index;
    bool fetch_throws = false;
    bool start_throws = false;
    int starts = 0;
    std::string last_url;
    std::vector<std::string> urls;  // every fetch, in order
    // Release notes served for a URL containing "notes"; empty = throw 404.
    std::string notes;
    InstallerState state;

    std::string fetch_url(const std::string& url, std::chrono::milliseconds) override {
        last_url = url;
        urls.push_back(url);
        if (fetch_throws) {
            throw SoftwareUpdateError(alpacahttp::util::ErrorCode::DRIVER_ERROR,
                                      "Update check failed: Could not resolve host: apt.example");
        }
        if (url.find("notes") != std::string::npos) {
            if (notes.empty()) {
                throw SoftwareUpdateError(alpacahttp::util::ErrorCode::DRIVER_ERROR,
                                          "Update check failed: the repository answered HTTP 404 for " + url);
            }
            return notes;
        }
        return index;
    }
    void start_installer() override {
        ++starts;
        if (start_throws) {
            throw SoftwareUpdateError(alpacahttp::util::ErrorCode::DRIVER_ERROR,
                                      "Update install failed: could not start unit: Access denied");
        }
        state.state = "running";
        state.detail = "ActiveState=activating SubState=start";
    }
    InstallerState installer_state() override { return state; }

    // Channel seam: the host's channel, the switches asked for, and the
    // state the channel helper reports. A switch starts "running" and does
    // not change `channel` (the test finishes it by hand).
    alpacahttp::util::UpdateChannel channel = alpacahttp::util::UpdateChannel::Stable;
    std::vector<alpacahttp::util::UpdateChannel> switches;
    bool switch_throws = false;
    InstallerState channel_state;
    alpacahttp::util::UpdateChannel read_channel() override { return channel; }
    void start_channel_switch(alpacahttp::util::UpdateChannel requested) override {
        if (switch_throws) {
            throw SoftwareUpdateError(alpacahttp::util::ErrorCode::DRIVER_ERROR,
                                      "Channel switch failed: could not start unit: Access denied");
        }
        switches.push_back(requested);
        channel_state.state = "running";
    }
    InstallerState channel_switch_state() override { return channel_state; }
};

// The index the live repository served on 2026-09-27, with the dbgsym stanza
// that must never be mistaken for the package itself.
const char* kLiveIndex =
    "Package: alpacabridge\n"
    "Version: 4.1.0\n"
    "Architecture: arm64\n"
    "Maintainer: OpenAstro <support@openastro.net>\n"
    "Depends: libc6 (>= 2.34), adduser\n"
    "Filename: pool/main/alpacabride/alpacabridge_4.1.0_arm64.deb\n"
    "Size: 16113008\n"
    "Description: ASCOM Alpaca HTTP server for astronomical devices\n"
    " AlpacaBridge provides an ASCOM Alpaca-compatible HTTP server\n"
    " .\n"
    " Includes bundled vendor SDKs.\n"
    "\n"
    "Package: alpacabridge-dbgsym\n"
    "Version: 9.9.9\n"
    "Architecture: arm64\n"
    "Filename: pool/main/alpacabride/alpacabridge-dbgsym_4.1.0_arm64.deb\n"
    "\n";

std::string status_state(const nlohmann::json& status) { return status["Installer"]["State"].get<std::string>(); }

alpacahttp::util::SoftwareUpdateSettings settings(const std::string& installed, const std::string& packages_url,
                                                  const std::string& notes_template = "",
                                                  const std::string& release_template = "") {
    return {installed, packages_url, "alpacabridge", notes_template, release_template};
}

}  // namespace

int main() {
    std::cout << "Testing software update helpers...\n";

    // --- compare_debian_versions: pinned against `dpkg --compare-versions`
    // on Debian 13 (each row was checked with the real tool).
    {
        struct Row {
            const char* a;
            const char* b;
            int expected;  // sign of compare(a, b)
        };
        const Row rows[] = {
            {"4.1.0", "4.2.0", -1},
            {"4.1.0", "4.10.0", -1},  // numeric, not lexical
            {"4.1.0", "4.1.0", 0},
            {"4.1.0~rc1", "4.1.0", -1},  // tilde sorts before the end of string
            {"4.1.0", "4.1.0-1", -1},    // a revision sorts after none
            {"4.1.0-1", "4.1.0-2", -1},
            {"1.0a", "1.0", 1},   // a letter sorts after the end
            {"1:1.0", "2.0", 1},  // epoch wins
            {"4.1.0+git1", "4.1.0", 1},
            {"4.1.0~beta", "4.1.0~alpha", 1},
            {"4.1.0-1~bpo", "4.1.0-1", -1},
            {"1.0.0", "1.0", 1},
            {"1a", "1b", -1},
            {"1.0-a", "1.0-1", 1},     // letters before digits in a non-digit run (a > end)
            {"4.01.0", "4.1.0", 0},    // leading zeros are ignored
            {" 4.1.0\n", "4.1.0", 0},  // surrounding whitespace is ignored
        };
        for (const auto& row : rows) {
            const int forward = compare_debian_versions(row.a, row.b);
            const int backward = compare_debian_versions(row.b, row.a);
            const int sign = forward < 0 ? -1 : (forward > 0 ? 1 : 0);
            const int back_sign = backward < 0 ? -1 : (backward > 0 ? 1 : 0);
            if (sign != row.expected || back_sign != -row.expected) {
                std::cerr << "compare(" << row.a << ", " << row.b << ") = " << forward << ", reverse " << backward
                          << ", expected sign " << row.expected << "\n";
            }
            EXPECT(sign == row.expected);
            EXPECT(back_sign == -row.expected);
        }
    }

    // --- find_package_version
    {
        const auto v = find_package_version(kLiveIndex, "alpacabridge");
        EXPECT(v.has_value());
        EXPECT(*v == "4.1.0");
        // The dbgsym stanza carries a higher number and must not match.
        EXPECT(!find_package_version(kLiveIndex, "alpacabridge-dbg").has_value());
        EXPECT(find_package_version(kLiveIndex, "alpacabridge-dbgsym").value_or("") == "9.9.9");
        EXPECT(!find_package_version("", "alpacabridge").has_value());
        EXPECT(!find_package_version("Package: other\nVersion: 1.0\n", "alpacabridge").has_value());
        // Several stanzas for one package (a repository keeping old
        // versions): the newest wins regardless of order.
        const std::string several =
            "Package: alpacabridge\nVersion: 4.2.0\n\nPackage: alpacabridge\nVersion: 4.10.0\n\n"
            "Package: alpacabridge\nVersion: 4.9.1\n";
        EXPECT(find_package_version(several, "alpacabridge").value_or("") == "4.10.0");
        // CRLF line endings and a missing trailing newline both parse.
        EXPECT(find_package_version("Package: alpacabridge\r\nVersion: 4.3.0\r\n", "alpacabridge").value_or("") ==
               "4.3.0");
        EXPECT(find_package_version("Package: alpacabridge\nVersion: 4.3.1", "alpacabridge").value_or("") == "4.3.1");
        // A stanza without a Version field contributes nothing.
        EXPECT(!find_package_version("Package: alpacabridge\nArchitecture: arm64\n", "alpacabridge").has_value());
    }

    // --- manager: status before any check
    {
        auto backend = std::make_unique<FakeBackend>();
        auto* fake = backend.get();
        fake->index = kLiveIndex;
        SoftwareUpdateManager manager(settings("4.1.0", "https://apt.example/Packages"), std::move(backend));

        const auto status = manager.status();
        EXPECT(status["InstalledVersion"] == "4.1.0");
        EXPECT(status["LatestVersion"].is_null());
        EXPECT(status["UpdateAvailable"] == false);
        EXPECT(status["CheckedAt"].is_null());
        EXPECT(status["CheckError"].is_null());
        EXPECT(status["PackagesUrl"] == "https://apt.example/Packages");
        EXPECT(status_state(status) == "idle");
        EXPECT(fake->last_url.empty());  // status never fetches

        // Install before any check is refused without touching the backend.
        bool threw = false;
        try {
            manager.install();
        } catch (const SoftwareUpdateError& e) {
            threw = true;
            EXPECT(e.alpaca_error() == alpacahttp::util::ErrorCode::INVALID_OPERATION);
        }
        EXPECT(threw);
        EXPECT(fake->starts == 0);
    }

    // --- manager: check finds the same version -> up to date, install refused
    {
        auto backend = std::make_unique<FakeBackend>();
        auto* fake = backend.get();
        fake->index = kLiveIndex;
        SoftwareUpdateManager manager(settings("4.1.0", "https://apt.example/Packages"), std::move(backend));

        const auto status = manager.check();
        EXPECT(fake->last_url == "https://apt.example/Packages");
        EXPECT(status["LatestVersion"] == "4.1.0");
        EXPECT(status["UpdateAvailable"] == false);
        EXPECT(status["CheckedAt"].is_number_integer());
        EXPECT(status["CheckError"].is_null());

        bool threw = false;
        try {
            manager.install();
        } catch (const SoftwareUpdateError& e) {
            threw = true;
            EXPECT(e.alpaca_error() == alpacahttp::util::ErrorCode::INVALID_OPERATION);
        }
        EXPECT(threw);
        EXPECT(fake->starts == 0);
    }

    // --- manager: a dev build newer than the repository is "up to date" too
    {
        auto backend = std::make_unique<FakeBackend>();
        backend->index = kLiveIndex;
        SoftwareUpdateManager manager(settings("4.2.0", "u"), std::move(backend));
        const auto status = manager.check();
        EXPECT(status["LatestVersion"] == "4.1.0");
        EXPECT(status["UpdateAvailable"] == false);
    }

    // --- manager: newer version -> available, install starts the helper once
    {
        auto backend = std::make_unique<FakeBackend>();
        auto* fake = backend.get();
        fake->index = "Package: alpacabridge\nVersion: 4.2.0\n";
        SoftwareUpdateManager manager(settings("4.1.0", "u"), std::move(backend));

        const auto checked = manager.check();
        EXPECT(checked["LatestVersion"] == "4.2.0");
        EXPECT(checked["UpdateAvailable"] == true);

        const auto installed = manager.install();
        EXPECT(fake->starts == 1);
        EXPECT(status_state(installed) == "running");
        EXPECT(installed["Installer"]["Detail"].get<std::string>().find("activating") != std::string::npos);

        // A second install while the helper runs is refused, not restarted.
        bool threw = false;
        try {
            manager.install();
        } catch (const SoftwareUpdateError& e) {
            threw = true;
            EXPECT(e.alpaca_error() == alpacahttp::util::ErrorCode::INVALID_OPERATION);
        }
        EXPECT(threw);
        EXPECT(fake->starts == 1);

        // After the run the status carries the helper's transcript.
        fake->state.state = "succeeded";
        fake->state.log = "=== RESULT: success\n";
        const auto after = manager.status();
        EXPECT(status_state(after) == "succeeded");
        EXPECT(after["Installer"]["Log"] == "=== RESULT: success\n");
    }

    // --- manager: the helper unit is missing (source build) -> NOT_IMPLEMENTED
    {
        auto backend = std::make_unique<FakeBackend>();
        auto* fake = backend.get();
        fake->index = "Package: alpacabridge\nVersion: 4.2.0\n";
        fake->state.state = "unavailable";
        fake->state.detail = "alpacabridge-update.service is not installed on this host";
        SoftwareUpdateManager manager(settings("4.1.0", "u"), std::move(backend));
        manager.check();
        bool threw = false;
        try {
            manager.install();
        } catch (const SoftwareUpdateError& e) {
            threw = true;
            EXPECT(e.alpaca_error() == alpacahttp::util::ErrorCode::NOT_IMPLEMENTED);
            EXPECT(std::string(e.what()).find("not installed on this host") != std::string::npos);
        }
        EXPECT(threw);
        EXPECT(fake->starts == 0);
    }

    // --- manager: the backend cannot start the unit -> its error propagates
    {
        auto backend = std::make_unique<FakeBackend>();
        auto* fake = backend.get();
        fake->index = "Package: alpacabridge\nVersion: 4.2.0\n";
        fake->start_throws = true;
        SoftwareUpdateManager manager(settings("4.1.0", "u"), std::move(backend));
        manager.check();
        bool threw = false;
        try {
            manager.install();
        } catch (const SoftwareUpdateError& e) {
            threw = true;
            EXPECT(e.alpaca_error() == alpacahttp::util::ErrorCode::DRIVER_ERROR);
            EXPECT(std::string(e.what()).find("Access denied") != std::string::npos);
        }
        EXPECT(threw);
        EXPECT(fake->starts == 1);
    }

    // --- manager: a failed fetch throws AND is recorded as CheckError; a
    // later successful check clears it and a prior result never lingers.
    {
        auto backend = std::make_unique<FakeBackend>();
        auto* fake = backend.get();
        fake->fetch_throws = true;
        SoftwareUpdateManager manager(settings("4.1.0", "u"), std::move(backend));
        bool threw = false;
        try {
            manager.check();
        } catch (const SoftwareUpdateError& e) {
            threw = true;
            EXPECT(e.alpaca_error() == alpacahttp::util::ErrorCode::DRIVER_ERROR);
        }
        EXPECT(threw);
        auto status = manager.status();
        EXPECT(status["CheckError"].is_string());
        EXPECT(status["CheckError"].get<std::string>().find("Could not resolve host") != std::string::npos);
        EXPECT(status["CheckedAt"].is_number_integer());
        EXPECT(status["LatestVersion"].is_null());
        EXPECT(status["UpdateAvailable"] == false);

        // Install is still refused: no successful check has happened.
        threw = false;
        try {
            manager.install();
        } catch (const SoftwareUpdateError& e) {
            threw = true;
            EXPECT(e.alpaca_error() == alpacahttp::util::ErrorCode::INVALID_OPERATION);
        }
        EXPECT(threw);

        fake->fetch_throws = false;
        fake->index = "Package: alpacabridge\nVersion: 4.2.0\n";
        status = manager.check();
        EXPECT(status["CheckError"].is_null());
        EXPECT(status["UpdateAvailable"] == true);
    }

    // --- manager: an index without the package is a failed check
    {
        auto backend = std::make_unique<FakeBackend>();
        backend->index = "Package: something-else\nVersion: 1.0\n";
        SoftwareUpdateManager manager(settings("4.1.0", "https://apt.example/Packages"), std::move(backend));
        bool threw = false;
        try {
            manager.check();
        } catch (const SoftwareUpdateError& e) {
            threw = true;
            EXPECT(std::string(e.what()).find("no 'alpacabridge' package listed") != std::string::npos);
        }
        EXPECT(threw);
        EXPECT(manager.status()["CheckError"].is_string());
    }

    // --- expand_version_template
    {
        using alpacahttp::util::expand_version_template;
        EXPECT(expand_version_template("https://x/v{version}/docs/releases/{version}.md", "4.2.0") ==
               "https://x/v4.2.0/docs/releases/4.2.0.md");
        EXPECT(expand_version_template("no placeholder", "4.2.0") == "no placeholder");
        EXPECT(expand_version_template("", "4.2.0").empty());
        EXPECT(expand_version_template(alpacahttp::util::kDefaultReleaseNotesUrl, "4.1.0") ==
               "https://raw.githubusercontent.com/open-astro/AlpacaBridge/v4.1.0/docs/releases/4.1.0.md");
        EXPECT(expand_version_template(alpacahttp::util::kDefaultReleaseUrl, "4.1.0") ==
               "https://github.com/open-astro/AlpacaBridge/releases/tag/v4.1.0");
        // A beta is X.Y.Z~betaN in the apt index but tagged vX.Y.Z-beta.N with
        // notes in docs/releases/X.Y.Z-beta.N.md (scripts/release_tag.py).
        EXPECT(expand_version_template(alpacahttp::util::kDefaultReleaseNotesUrl, "5.0.0~beta2") ==
               "https://raw.githubusercontent.com/open-astro/AlpacaBridge/v5.0.0-beta.2/docs/releases/5.0.0-beta.2.md");
        EXPECT(expand_version_template(alpacahttp::util::kDefaultReleaseUrl, "5.0.0~beta2") ==
               "https://github.com/open-astro/AlpacaBridge/releases/tag/v5.0.0-beta.2");
        EXPECT(expand_version_template("{version}", "5.0.0~beta10") == "5.0.0-beta.10");
        // Anything that is not the beta spelling passes through untouched.
        EXPECT(expand_version_template("{version}", "5.0.0~rc1") == "5.0.0~rc1");
        EXPECT(expand_version_template("{version}", "5.0.0~beta") == "5.0.0~beta");
        // scripts/release_tag_cases.txt pins this mapping and release_tag.py's
        // inverse to the same pairs: "<tag> <VERSION>" per line, '#' comments.
        {
            const std::filesystem::path cases =
                std::filesystem::path(ALPACAHTTP_REPO_ROOT) / "scripts" / "release_tag_cases.txt";
            std::ifstream in(cases);
            EXPECT(in.is_open());
            int pairs = 0;
            std::string line;
            while (std::getline(in, line)) {
                // Same rule as release_tag.py's read_cases(): a '#' starts a
                // comment (whole line or trailing), blanks are skipped, and
                // every other line holds exactly two fields.
                line = line.substr(0, line.find('#'));
                std::istringstream fields(line);
                std::string tag, version, extra;
                if (!(fields >> tag)) continue;
                EXPECT(static_cast<bool>(fields >> version));
                EXPECT(!(fields >> extra));
                EXPECT(expand_version_template("v{version}", version) == tag);
                ++pairs;
            }
            EXPECT(pairs >= 2);
        }
    }

    // --- release notes: fetched only when an update is available, from the
    // expanded template, and carried with both links in the status
    {
        auto backend = std::make_unique<FakeBackend>();
        auto* fake = backend.get();
        fake->index = "Package: alpacabridge\nVersion: 4.2.0\n";
        fake->notes = "# AlpacaBridge 4.2.0\n\n- **Update from the web UI.**\n";
        SoftwareUpdateManager manager(settings("4.1.0", "https://apt.example/Packages",
                                               "https://notes.example/{version}.md", "https://rel.example/v{version}"),
                                      std::move(backend));

        // No notes before a check, and none fetched by status().
        auto status = manager.status();
        EXPECT(status["ReleaseNotes"].is_null());
        EXPECT(status["ReleaseNotesUrl"].is_null());
        EXPECT(status["ReleaseUrl"].is_null());
        EXPECT(fake->urls.empty());

        status = manager.check();
        EXPECT(fake->urls.size() == 2);
        EXPECT(fake->urls[0] == "https://apt.example/Packages");
        EXPECT(fake->urls[1] == "https://notes.example/4.2.0.md");
        EXPECT(status["UpdateAvailable"] == true);
        EXPECT(status["ReleaseNotes"] == "# AlpacaBridge 4.2.0\n\n- **Update from the web UI.**\n");
        EXPECT(status["ReleaseNotesUrl"] == "https://notes.example/4.2.0.md");
        EXPECT(status["ReleaseUrl"] == "https://rel.example/v4.2.0");
        // status() keeps answering from the cache without refetching.
        status = manager.status();
        EXPECT(status["ReleaseNotes"].is_string());
        EXPECT(fake->urls.size() == 2);
    }

    // --- release notes: up to date -> no notes fetch at all
    {
        auto backend = std::make_unique<FakeBackend>();
        auto* fake = backend.get();
        fake->index = kLiveIndex;
        fake->notes = "would be wrong to show";
        SoftwareUpdateManager manager(settings("4.1.0", "u", "https://notes.example/{version}.md", "r"),
                                      std::move(backend));
        const auto status = manager.check();
        EXPECT(fake->urls.size() == 1);
        EXPECT(status["ReleaseNotes"].is_null());
        EXPECT(status["ReleaseNotesUrl"].is_null());
        EXPECT(status["ReleaseUrl"].is_null());
    }

    // --- release notes: a failed notes fetch does not fail the check, and
    // the links still point the operator at the release page
    {
        auto backend = std::make_unique<FakeBackend>();
        auto* fake = backend.get();
        fake->index = "Package: alpacabridge\nVersion: 4.2.0\n";
        fake->notes.clear();  // 404
        SoftwareUpdateManager manager(
            settings("4.1.0", "u", "https://notes.example/{version}.md", "https://rel.example/v{version}"),
            std::move(backend));
        const auto status = manager.check();
        EXPECT(fake->urls.size() == 2);
        EXPECT(status["UpdateAvailable"] == true);
        EXPECT(status["CheckError"].is_null());
        EXPECT(status["ReleaseNotes"].is_null());
        EXPECT(status["ReleaseUrl"] == "https://rel.example/v4.2.0");
        // Install is still allowed: the notes are informational.
        manager.install();
        EXPECT(fake->starts == 1);
    }

    // --- release notes: an empty template disables the fetch; oversized
    // notes are truncated with a marker
    {
        auto backend = std::make_unique<FakeBackend>();
        auto* fake = backend.get();
        fake->index = "Package: alpacabridge\nVersion: 4.2.0\n";
        fake->notes = "x";
        SoftwareUpdateManager manager(settings("4.1.0", "u", "", ""), std::move(backend));
        auto status = manager.check();
        EXPECT(fake->urls.size() == 1);
        EXPECT(status["ReleaseNotes"].is_null());
        EXPECT(status["ReleaseNotesUrl"].is_null());
        EXPECT(status["ReleaseUrl"].is_null());

        auto big_backend = std::make_unique<FakeBackend>();
        auto* big = big_backend.get();
        big->index = "Package: alpacabridge\nVersion: 4.2.0\n";
        big->notes = std::string(SoftwareUpdateManager::kReleaseNotesMaxBytes + 1000, 'n');
        SoftwareUpdateManager big_manager(settings("4.1.0", "u", "https://notes.example/{version}.md", ""),
                                          std::move(big_backend));
        status = big_manager.check();
        const std::string notes = status["ReleaseNotes"].get<std::string>();
        EXPECT(notes.size() < SoftwareUpdateManager::kReleaseNotesMaxBytes + 100);
        EXPECT(notes.find("(notes truncated)") != std::string::npos);
    }

    // --- the transcript path is fixed, root-owned, and the same on both
    // sides (PR #745 review: a transcript in the daemon's own log directory
    // let the root helper follow a symlink the service user planted).
    {
        using alpacahttp::util::kUpdateLogPath;
        const std::string path = kUpdateLogPath;
        EXPECT(path.rfind("/var/log/alpacabridge-update/", 0) == 0);
        EXPECT(path.find("/var/log/AlpacaBridge/") == std::string::npos);

        const std::filesystem::path repo = ALPACAHTTP_REPO_ROOT;
        const auto read = [](const std::filesystem::path& p) {
            std::ifstream in(p);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        };
        const std::string script = read(repo / "debian" / "alpacabridge-software-update");
        EXPECT(!script.empty());
        EXPECT(script.find("LOG=" + path + "\n") != std::string::npos);
        const std::string unit = read(repo / "debian" / "alpacabridge-update.service");
        EXPECT(!unit.empty());
        // The directory component must be the helper unit's own LogsDirectory=.
        const std::string dir = std::filesystem::path(path).parent_path().filename().string();
        EXPECT(unit.find("LogsDirectory=" + dir + "\n") != std::string::npos);
        // And the daemon's own unit must not claim it.
        const std::string daemon_unit = read(repo / "debian" / "alpacabridge.service");
        EXPECT(!daemon_unit.empty());
        EXPECT(daemon_unit.find("LogsDirectory=" + dir + "\n") == std::string::npos);
    }

    // --- a failed check after a successful one keeps the last good answer:
    // UpdateAvailable stays true beside the CheckError, and install() still
    // goes ahead (the last good answer is still valid; the page shows both).
    {
        auto backend = std::make_unique<FakeBackend>();
        auto* fake = backend.get();
        fake->index = "Package: alpacabridge\nVersion: 4.2.0\n";
        SoftwareUpdateManager manager(settings("4.1.0", "u"), std::move(backend));
        EXPECT(manager.check()["UpdateAvailable"] == true);
        fake->fetch_throws = true;
        bool threw = false;
        try {
            manager.check();
        } catch (const SoftwareUpdateError&) {
            threw = true;
        }
        EXPECT(threw);
        const auto status = manager.status();
        EXPECT(status["CheckError"].is_string());
        EXPECT(status["LatestVersion"] == "4.2.0");
        EXPECT(status["UpdateAvailable"] == true);
        manager.install();
        EXPECT(fake->starts == 1);
    }

    // --- the check's fetches run outside the manager's mutex: a status()
    // poll from another thread returns while a fetch is still blocked.
    // Moving the fetch back under the lock makes this case hang until the
    // latch is released, and fail.
    {
        class BlockingBackend final : public SoftwareUpdateBackend {
        public:
            std::mutex m;
            std::condition_variable cv;
            bool release = false;
            bool blocked = false;
            std::string fetch_url(const std::string&, std::chrono::milliseconds) override {
                std::unique_lock<std::mutex> lock(m);
                blocked = true;
                cv.notify_all();
                cv.wait(lock, [&] { return release; });
                return "Package: alpacabridge\nVersion: 4.2.0\n";
            }
            void start_installer() override {}
            InstallerState installer_state() override { return {}; }
            alpacahttp::util::UpdateChannel read_channel() override { return alpacahttp::util::UpdateChannel::Stable; }
            void start_channel_switch(alpacahttp::util::UpdateChannel) override {}
            InstallerState channel_switch_state() override { return {}; }
        };
        auto backend = std::make_unique<BlockingBackend>();
        auto* blocking = backend.get();
        SoftwareUpdateManager manager(settings("4.1.0", "u"), std::move(backend));

        std::thread checker([&] { manager.check(); });
        {
            std::unique_lock<std::mutex> lock(blocking->m);
            blocking->cv.wait(lock, [&] { return blocking->blocked; });
        }
        // The fetch is parked. status() must answer now, not after release.
        std::atomic<bool> status_returned{false};
        std::thread poller([&] {
            const auto status = manager.status();
            EXPECT(status["LatestVersion"].is_null());
            status_returned = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!status_returned && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT(status_returned);
        {
            std::lock_guard<std::mutex> lock(blocking->m);
            blocking->release = true;
        }
        blocking->cv.notify_all();
        checker.join();
        poller.join();
        EXPECT(manager.status()["LatestVersion"] == "4.2.0");
    }

    // --- an empty packages_url disables the check: status says so, check()
    // is NOT_IMPLEMENTED, nothing is fetched and no CheckError is recorded.
    {
        auto backend = std::make_unique<FakeBackend>();
        auto* fake = backend.get();
        fake->index = "Package: alpacabridge\nVersion: 4.2.0\n";
        SoftwareUpdateManager manager(settings("4.1.0", ""), std::move(backend));
        auto status = manager.status();
        EXPECT(status["CheckEnabled"] == false);
        bool threw = false;
        try {
            manager.check();
        } catch (const SoftwareUpdateError& e) {
            threw = true;
            EXPECT(e.alpaca_error() == alpacahttp::util::ErrorCode::NOT_IMPLEMENTED);
            EXPECT(std::string(e.what()).find("turned off") != std::string::npos);
        }
        EXPECT(threw);
        EXPECT(fake->urls.empty());
        status = manager.status();
        EXPECT(status["CheckError"].is_null());
        EXPECT(status["CheckedAt"].is_null());
        EXPECT(status["UpdateAvailable"] == false);

        auto enabled = std::make_unique<FakeBackend>();
        enabled->index = kLiveIndex;
        SoftwareUpdateManager on(settings("4.1.0", "u"), std::move(enabled));
        EXPECT(on.status()["CheckEnabled"] == true);
    }

    // --- single flight: a check arriving while another is fetching waits for
    // it and returns its result without a second fetch.
    {
        class BlockingBackend final : public SoftwareUpdateBackend {
        public:
            std::mutex m;
            std::condition_variable cv;
            bool release = false;
            int fetches = 0;
            std::string fetch_url(const std::string&, std::chrono::milliseconds) override {
                std::unique_lock<std::mutex> lock(m);
                ++fetches;
                cv.notify_all();
                cv.wait(lock, [&] { return release; });
                return "Package: alpacabridge\nVersion: 4.2.0\n";
            }
            void start_installer() override {}
            InstallerState installer_state() override { return {}; }
            alpacahttp::util::UpdateChannel read_channel() override { return alpacahttp::util::UpdateChannel::Stable; }
            void start_channel_switch(alpacahttp::util::UpdateChannel) override {}
            InstallerState channel_switch_state() override { return {}; }
        };
        auto backend = std::make_unique<BlockingBackend>();
        auto* blocking = backend.get();
        // No notes template: exactly one fetch per check.
        SoftwareUpdateManager manager(settings("4.1.0", "u"), std::move(backend));
        std::thread first([&] { manager.check(); });
        {
            std::unique_lock<std::mutex> lock(blocking->m);
            blocking->cv.wait(lock, [&] { return blocking->fetches == 1; });
        }
        nlohmann::json second_result;
        std::thread second([&] { second_result = manager.check(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        {
            std::lock_guard<std::mutex> lock(blocking->m);
            EXPECT(blocking->fetches == 1);  // the second check did not fetch
            blocking->release = true;
        }
        blocking->cv.notify_all();
        first.join();
        second.join();
        EXPECT(blocking->fetches == 1);
        EXPECT(second_result["LatestVersion"] == "4.2.0");
        EXPECT(second_result["UpdateAvailable"] == true);
    }

    // --- install() makes its sd-bus calls outside the mutex and refuses a
    // second install while the first is still starting: with the start
    // parked on a latch, status() answers and a concurrent install() throws
    // INVALID_OPERATION without a second start. Putting the start back under
    // the lock hangs the poll; dropping the in-flight flag doubles the start.
    {
        class BlockingInstallBackend final : public SoftwareUpdateBackend {
        public:
            std::mutex m;
            std::condition_variable cv;
            bool release = false;
            bool parked = false;
            int starts = 0;
            std::string fetch_url(const std::string&, std::chrono::milliseconds) override {
                return "Package: alpacabridge\nVersion: 4.2.0\n";
            }
            void start_installer() override {
                std::unique_lock<std::mutex> lock(m);
                ++starts;
                parked = true;
                cv.notify_all();
                cv.wait(lock, [&] { return release; });
                state.state = "running";
            }
            InstallerState installer_state() override {
                std::lock_guard<std::mutex> lock(m);
                return state;
            }
            alpacahttp::util::UpdateChannel read_channel() override { return alpacahttp::util::UpdateChannel::Stable; }
            void start_channel_switch(alpacahttp::util::UpdateChannel) override {}
            InstallerState channel_switch_state() override { return {}; }
            InstallerState state;
        };
        auto backend = std::make_unique<BlockingInstallBackend>();
        auto* blocking = backend.get();
        SoftwareUpdateManager manager(settings("4.1.0", "u"), std::move(backend));
        EXPECT(manager.check()["UpdateAvailable"] == true);

        std::thread installer([&] { manager.install(); });
        {
            std::unique_lock<std::mutex> lock(blocking->m);
            blocking->cv.wait(lock, [&] { return blocking->parked; });
        }
        // The start is parked (state still idle). status() must answer now.
        std::atomic<bool> status_returned{false};
        std::thread poller([&] {
            const auto status = manager.status();
            EXPECT(status["UpdateAvailable"] == true);
            status_returned = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!status_returned && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT(status_returned);
        // A second install is refused by the in-flight flag, not started twice.
        bool threw = false;
        try {
            manager.install();
        } catch (const SoftwareUpdateError& e) {
            threw = true;
            EXPECT(e.alpaca_error() == alpacahttp::util::ErrorCode::INVALID_OPERATION);
        }
        EXPECT(threw);
        {
            std::lock_guard<std::mutex> lock(blocking->m);
            EXPECT(blocking->starts == 1);
            blocking->release = true;
        }
        blocking->cv.notify_all();
        installer.join();
        poller.join();
        EXPECT(blocking->starts == 1);
        EXPECT(status_state(manager.status()) == "running");
    }

    // --- classify_installer_state: the durable-record contract, pure.
    {
        using alpacahttp::util::classify_installer_state;
        const std::string ok = "=== started\n=== RESULT: success 2026-09-30T00:00:00Z installed 4.2.0\n";
        const std::string bad = "=== started\n=== RESULT: failure 2026-09-30T00:00:00Z exit 100\n";
        const std::string both = bad + "=== started again\n=== RESULT: success\n";
        EXPECT(classify_installer_state("activating", "start", "", false, "").state == "running");
        EXPECT(classify_installer_state("active", "running", "", false, ok).state == "running");
        EXPECT(classify_installer_state("deactivating", "stop", "", false, "").state == "running");
        // A queued start job with the unit still inactive is running, and the
        // previous transcript's marker must not be reported as the result.
        const auto queued = classify_installer_state("inactive", "dead", "success", true, ok);
        EXPECT(queued.state == "running");
        EXPECT(queued.detail.find("Job=pending") != std::string::npos);
        EXPECT(classify_installer_state("failed", "failed", "exit-code", false, ok).state == "failed");
        EXPECT(classify_installer_state("inactive", "dead", "success", false, ok).state == "succeeded");
        EXPECT(classify_installer_state("inactive", "dead", "success", false, bad).state == "failed");
        EXPECT(classify_installer_state("inactive", "dead", "success", false, both).state == "succeeded");
        const auto killed = classify_installer_state("inactive", "dead", "success", false, "=== started\n");
        EXPECT(killed.state == "failed");
        EXPECT(killed.detail.find("no result recorded") != std::string::npos);
        const auto idle = classify_installer_state("inactive", "dead", "success", false, "");
        EXPECT(idle.state == "idle");
        EXPECT(idle.detail == "ActiveState=inactive SubState=dead Result=success");
        EXPECT(idle.log.empty());
        EXPECT(classify_installer_state("inactive", "dead", "success", false, ok).log == ok);
    }

    {
        // case: a finished channel switch reports the helper's result line as its detail
        using alpacahttp::util::classify_channel_switch_log;
        const auto failed = classify_channel_switch_log(
            "=== started\n=== RESULT: failure 2026-09-30T00:00:00Z apt-get update exit 100\n");
        EXPECT(failed.state == "failed");
        EXPECT(failed.detail == "failure 2026-09-30T00:00:00Z apt-get update exit 100");
        EXPECT(classify_channel_switch_log("").state == "idle");
    }

    // --- update channel (issue #1103) ---
    using alpacahttp::util::channel_apt_component;
    using alpacahttp::util::channel_name;
    using alpacahttp::util::channel_packages_url;
    using alpacahttp::util::channel_unit_name;
    using alpacahttp::util::kDefaultPackagesUrl;
    using alpacahttp::util::parse_channel;
    using alpacahttp::util::UpdateChannel;
    const std::filesystem::path repo_root = ALPACAHTTP_REPO_ROOT;
    const auto read_file = [](const std::filesystem::path& file) {
        std::ifstream in(file);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };

    {
        // case: polkit rule names the three units by exact string and nothing else
        const std::string rules = read_file(repo_root / "debian" / "alpacabridge-update.polkit-rules");
        EXPECT(!rules.empty());
        EXPECT(rules.find("unit !== \"alpacabridge-update.service\"") != std::string::npos);
        for (const auto channel : {UpdateChannel::Stable, UpdateChannel::Beta}) {
            EXPECT(rules.find(std::string("unit !== \"") + channel_unit_name(channel) + "\"") != std::string::npos);
        }
        EXPECT(rules.find("action.lookup(\"verb\") === \"start\"") != std::string::npos);
        // Exactly three unit names appear in the code (comments excluded).
        std::string code;
        std::istringstream lines(rules);
        for (std::string line; std::getline(lines, line);) {
            if (line.rfind("//", 0) != 0) code += line + "\n";
        }
        // No prefix, regex or substring matching: string equality only.
        for (const char* forbidden : {"indexOf", "match(", "RegExp", "startsWith", "endsWith", "test(", "search("}) {
            EXPECT(code.find(forbidden) == std::string::npos);
        }
        std::size_t units = 0;
        for (std::size_t pos = 0; (pos = code.find("alpacabridge-", pos)) != std::string::npos; ++pos) ++units;
        EXPECT(units == 3);
    }

    {
        // case: channel helper refuses any argument but stable or beta before touching anything
        const std::string helper = (repo_root / "debian" / "alpacabridge-set-update-channel").string();
        const auto run = [&](const std::string& args) {
            // stderr goes to the null device: the usage message is expected.
            return std::system(("bash " + helper + " " + args + " >/dev/null 2>&1").c_str());
        };
        EXPECT(run("") != 0);
        EXPECT(run("bogus") != 0);
        EXPECT(run("beta stable") != 0);
        EXPECT(run("'beta;touch x'") != 0);
        EXPECT(run("Beta") != 0);
        EXPECT(run("../beta") != 0);
        const std::string text = read_file(helper);
        // The validation precedes the first write: the case statement comes
        // before the transcript is opened and before apt runs.
        const auto validated = text.find(std::string(channel_name(UpdateChannel::Stable)) + "|" +
                                         channel_name(UpdateChannel::Beta) + ") ;;");
        EXPECT(validated != std::string::npos);
        EXPECT(validated < text.find(": > \"$LOG\""));
        EXPECT(validated < text.find("apt-get update"));
        // Never edits openastro.list; both ends of the result marker exist.
        EXPECT(text.find("> /etc/apt/sources.list.d/openastro.list") == std::string::npos);
        EXPECT(text.find("=== RESULT: success") != std::string::npos);
        EXPECT(text.find("=== RESULT: failure") != std::string::npos);
        EXPECT(text.find("mktemp \"$DROPIN_DIR/") != std::string::npos);
        EXPECT(text.find("chmod 0644 \"$tmp\"") != std::string::npos);
        EXPECT(text.find("mv -f \"$tmp\" \"$DROPIN\"") != std::string::npos);
        EXPECT(text.find("rm -f \"$DROPIN\"") != std::string::npos);
        // A failing apt-get update is a failed switch: the exit status is
        // kept, the drop-in is put back, and the marker carries the status.
        const auto apt = text.find("apt-get update -o");
        EXPECT(apt != std::string::npos);
        const auto after_apt = text.substr(apt);
        EXPECT(after_apt.find("rc=$?") != std::string::npos);
        EXPECT(after_apt.find("if [ \"$rc\" -ne 0 ]") != std::string::npos);
        EXPECT(after_apt.find("=== RESULT: failure $(stamp) apt-get update exit $rc") != std::string::npos);
        EXPECT(after_apt.find("exit \"$rc\"") != std::string::npos);
        EXPECT(after_apt.find("mv -f \"$backup\" \"$DROPIN\"") != std::string::npos);
        EXPECT(after_apt.find("=== RESULT: failure") < after_apt.find("=== RESULT: success"));
    }

    {
        // case: channel mapping ties the check URL, the apt component and the unit name
        EXPECT(parse_channel("stable") == UpdateChannel::Stable);
        EXPECT(parse_channel("beta") == UpdateChannel::Beta);
        EXPECT(!parse_channel("Beta").has_value());
        EXPECT(!parse_channel("stable.service").has_value());
        EXPECT(!parse_channel("").has_value());
        EXPECT(std::string(channel_name(UpdateChannel::Beta)) == "beta");
        EXPECT(channel_packages_url(UpdateChannel::Stable) == kDefaultPackagesUrl);
        for (const auto channel : {UpdateChannel::Stable, UpdateChannel::Beta}) {
            // Issue acceptance item 2: the check URL component and the apt
            // drop-in Components: are the same string, for each channel.
            const std::string component = channel_apt_component(channel);
            EXPECT(channel_packages_url(channel) ==
                   "https://apt.openastro.net/dists/trixie/" + component + "/binary-arm64/Packages");
            EXPECT(std::string(channel_unit_name(channel)) ==
                   std::string("alpacabridge-channel@") + channel_name(channel) + ".service");
        }
        EXPECT(std::string(channel_apt_component(UpdateChannel::Beta)) == "beta");
        EXPECT(std::string(channel_apt_component(UpdateChannel::Stable)) == "main");

        // The helper writes the Components: line for beta, and the paths it and
        // the unit use are the ones the daemon reads.
        const std::string helper = read_file(repo_root / "debian" / "alpacabridge-set-update-channel");
        EXPECT(!helper.empty());
        EXPECT(helper.find(std::string("Components: ") + channel_apt_component(UpdateChannel::Beta) + "\n") !=
               std::string::npos);
        EXPECT(helper.find(std::string("DROPIN=\"$DROPIN_DIR/") +
                           std::filesystem::path(alpacahttp::util::kChannelDropInPath).filename().string() + "\"\n") !=
               std::string::npos);
        EXPECT(helper.find(
                   "DROPIN_DIR=" + std::filesystem::path(alpacahttp::util::kChannelDropInPath).parent_path().string() +
                   "\n") != std::string::npos);
        EXPECT(helper.find(std::string("LOG=") + alpacahttp::util::kChannelLogPath + "\n") != std::string::npos);
        const std::string unit = read_file(repo_root / "debian" / "alpacabridge-channel@.service");
        EXPECT(unit.find("ExecStart=/usr/libexec/alpacabridge/set-update-channel %i\n") != std::string::npos);
        EXPECT(unit.find("Type=oneshot\n") != std::string::npos);
        EXPECT(unit.find("LogsDirectory=alpacabridge-update\n") != std::string::npos);
        EXPECT(unit.find("LogsDirectoryMode=0755\n") != std::string::npos);
        EXPECT(unit.find("PrivateTmp=true\n") != std::string::npos);
        EXPECT(unit.find("TimeoutStartSec=") != std::string::npos);
        EXPECT(unit.find("\n[Install]") == std::string::npos);
    }

    {
        // case: the check URL follows the channel unless update_packages_url overrides it
        auto backend = std::make_unique<FakeBackend>();
        auto* fake = backend.get();
        fake->index = "Package: alpacabridge\nVersion: 5.0.0~beta5\n";
        SoftwareUpdateManager manager(settings("5.0.0~beta4", kDefaultPackagesUrl), std::move(backend));
        auto status = manager.check();
        EXPECT(fake->last_url == channel_packages_url(UpdateChannel::Stable));
        EXPECT(status["Channel"] == "stable");
        EXPECT(status["ChannelOverridden"] == false);
        EXPECT(status["PackagesUrl"] == channel_packages_url(UpdateChannel::Stable));
        fake->channel = UpdateChannel::Beta;
        status = manager.check();
        EXPECT(fake->last_url == channel_packages_url(UpdateChannel::Beta));
        EXPECT(status["Channel"] == "beta");
        EXPECT(status["PackagesUrl"] == channel_packages_url(UpdateChannel::Beta));
        EXPECT(status["CheckEnabled"] == true);

        auto custom_backend = std::make_unique<FakeBackend>();
        auto* custom = custom_backend.get();
        custom->index = "Package: alpacabridge\nVersion: 5.0.0~beta5\n";
        custom->channel = UpdateChannel::Beta;
        SoftwareUpdateManager overridden(settings("5.0.0~beta4", "https://mirror.example/Packages"),
                                         std::move(custom_backend));
        status = overridden.check();
        EXPECT(custom->last_url == "https://mirror.example/Packages");
        EXPECT(status["ChannelOverridden"] == true);
        EXPECT(status["PackagesUrl"] == "https://mirror.example/Packages");

        // Empty still disables the check and is not an override.
        SoftwareUpdateManager disabled(settings("5.0.0~beta4", ""), std::make_unique<FakeBackend>());
        EXPECT(disabled.status()["CheckEnabled"] == false);
        EXPECT(disabled.status()["ChannelOverridden"] == false);
    }

    {
        // case: a check made for the other channel is not offered after a switch
        auto backend = std::make_unique<FakeBackend>();
        auto* fake = backend.get();
        fake->channel = UpdateChannel::Beta;
        fake->index = "Package: alpacabridge\nVersion: 5.0.0~beta5\n";
        SoftwareUpdateManager manager(settings("5.0.0~beta4", kDefaultPackagesUrl), std::move(backend));
        EXPECT(manager.check()["UpdateAvailable"] == true);
        fake->channel = UpdateChannel::Stable;
        const auto status = manager.status();
        EXPECT(status["UpdateAvailable"] == false);
        EXPECT(status["LatestVersion"].is_null());
        EXPECT(status["ReleaseNotesUrl"].is_null());
        EXPECT(status["Channel"] == "stable");
        EXPECT(manager.status()["UpdateAvailable"] == false);
        bool threw = false;
        try {
            manager.install();
        } catch (const SoftwareUpdateError& e) {
            threw = e.alpaca_error() == alpacahttp::util::ErrorCode::INVALID_OPERATION;
        }
        EXPECT(threw);
        EXPECT(fake->starts == 0);
    }

    {
        // case: set_channel starts the unit instance from the table and refuses while busy
        const auto expect_refused = [](SoftwareUpdateManager& manager, UpdateChannel channel, std::int32_t code) {
            try {
                manager.set_channel(channel);
            } catch (const SoftwareUpdateError& e) {
                return e.alpaca_error() == code;
            }
            return false;
        };
        const std::int32_t invalid_operation = alpacahttp::util::ErrorCode::INVALID_OPERATION;

        auto backend = std::make_unique<FakeBackend>();
        auto* fake = backend.get();
        SoftwareUpdateManager manager(settings("5.0.0~beta4", kDefaultPackagesUrl), std::move(backend));
        auto status = manager.set_channel(UpdateChannel::Beta);
        EXPECT(fake->switches.size() == 1);
        EXPECT(fake->switches[0] == UpdateChannel::Beta);
        EXPECT(status["ChannelSwitch"]["State"] == "running");
        // A second switch, and an install, are refused while one is running.
        EXPECT(expect_refused(manager, UpdateChannel::Stable, invalid_operation));
        EXPECT(fake->switches.size() == 1);
        // Finished: another switch goes through, and the update installer is not refused by it.
        fake->channel_state.state = "succeeded";
        manager.set_channel(UpdateChannel::Stable);
        EXPECT(fake->switches.size() == 2);
        EXPECT(fake->switches[1] == UpdateChannel::Stable);

        // The update installer running refuses a switch.
        auto busy_backend = std::make_unique<FakeBackend>();
        busy_backend->state.state = "running";
        auto* busy = busy_backend.get();
        SoftwareUpdateManager busy_manager(settings("5.0.0~beta4", kDefaultPackagesUrl), std::move(busy_backend));
        EXPECT(expect_refused(busy_manager, UpdateChannel::Beta, invalid_operation));
        EXPECT(busy->switches.empty());

        // An explicit update_packages_url pins the channel.
        auto pinned_backend = std::make_unique<FakeBackend>();
        auto* pinned = pinned_backend.get();
        SoftwareUpdateManager pinned_manager(settings("5.0.0~beta4", "https://mirror.example/Packages"),
                                             std::move(pinned_backend));
        EXPECT(expect_refused(pinned_manager, UpdateChannel::Beta, invalid_operation));
        EXPECT(pinned->switches.empty());

        // No helper unit on this host: NOT_IMPLEMENTED, nothing started.
        auto missing_backend = std::make_unique<FakeBackend>();
        missing_backend->channel_state.state = "unavailable";
        auto* missing = missing_backend.get();
        SoftwareUpdateManager missing_manager(settings("5.0.0~beta4", kDefaultPackagesUrl), std::move(missing_backend));
        EXPECT(expect_refused(missing_manager, UpdateChannel::Beta, alpacahttp::util::ErrorCode::NOT_IMPLEMENTED));
        EXPECT(missing->switches.empty());

        // A refusal from systemd/polkit reaches the caller and does not wedge
        // the manager: the next switch is attempted.
        auto denied_backend = std::make_unique<FakeBackend>();
        auto* denied = denied_backend.get();
        denied->switch_throws = true;
        SoftwareUpdateManager denied_manager(settings("5.0.0~beta4", kDefaultPackagesUrl), std::move(denied_backend));
        EXPECT(expect_refused(denied_manager, UpdateChannel::Beta, alpacahttp::util::ErrorCode::DRIVER_ERROR));
        denied->switch_throws = false;
        denied_manager.set_channel(UpdateChannel::Beta);
        EXPECT(denied->switches.size() == 1);
    }

    {
        // case: an update installer run refuses nothing it did not before, but waits for a running switch
        auto backend = std::make_unique<FakeBackend>();
        auto* fake = backend.get();
        fake->index = "Package: alpacabridge\nVersion: 5.0.0\n";
        SoftwareUpdateManager manager(settings("5.0.0~beta4", kDefaultPackagesUrl), std::move(backend));
        manager.check();
        fake->channel_state.state = "running";
        bool refused = false;
        try {
            manager.install();
        } catch (const SoftwareUpdateError& e) {
            refused = e.alpaca_error() == alpacahttp::util::ErrorCode::INVALID_OPERATION;
        }
        EXPECT(refused);
        EXPECT(fake->starts == 0);
        fake->channel_state.state = "idle";
        manager.install();
        EXPECT(fake->starts == 1);
    }

    {
        // case: the system backend reads the channel from the drop-in marker file
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "alpacabridge-channel-marker-test";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        const std::string marker = (dir / "alpacabridge-beta.sources").string();
        alpacahttp::util::SystemSoftwareUpdateBackend backend("alpacabridge-update.service", (dir / "u.log").string(),
                                                              marker, (dir / "c.log").string());
        EXPECT(backend.read_channel() == UpdateChannel::Stable);
        std::ofstream(marker) << "Types: deb\n";
        EXPECT(backend.read_channel() == UpdateChannel::Beta);
        std::filesystem::remove(marker);
        EXPECT(backend.read_channel() == UpdateChannel::Stable);
        std::filesystem::remove_all(dir);
    }

    std::cout << "All software update tests passed!\n";
    return 0;
}
