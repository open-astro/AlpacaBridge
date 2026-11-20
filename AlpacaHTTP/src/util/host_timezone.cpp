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

#include <alpacahttp/util/host_timezone.h>
#include <alpacahttp/util/logging_adapter.h>  // trim_view

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace alpacahttp {
namespace util {

namespace {

// "posix/Europe/Berlin" -> "Europe/Berlin": the posix/ and right/ subtrees
// hold the same zones under names a browser's Intl rejects.
std::string strip_zoneinfo_subtree(std::string zone) {
    for (const char* prefix : {"posix/", "right/"}) {
        const std::string p = prefix;
        if (zone.compare(0, p.size(), p) == 0) {
            zone.erase(0, p.size());
            break;
        }
    }
    return zone;
}

// The part of a zoneinfo path after its last "zoneinfo/" component, or ""
// when there is none: "/usr/share/zoneinfo/America/Denver" -> "America/Denver",
// "../usr/share/zoneinfo/Etc/UTC" -> "Etc/UTC" (Debian ships the relative
// form), "/etc/localtime" -> "", with the subtree prefix dropped:
// ".../zoneinfo/right/Europe/Berlin" -> "Europe/Berlin".
std::string zone_from_zoneinfo_path(const std::string& path) {
    static const std::string kMarker = "zoneinfo/";
    const auto pos = path.rfind(kMarker);
    if (pos == std::string::npos) {
        return "";
    }
    return strip_zoneinfo_subtree(path.substr(pos + kMarker.size()));
}

}  // namespace

bool looks_like_iana_zone(const std::string& name) {
    // "localtime" and "posixrules" (the two non-zone files in zoneinfo/)
    // need no special case: neither has a '/', so the slash rule below
    // already rejects them, and the resolver tests pin both.
    if (name.empty() || name.size() > 64) {
        return false;
    }
    bool slash = false;
    bool segment_started = false;
    for (const char c : name) {
        if (c == '/') {
            if (!segment_started) {
                return false;  // leading '/' or "//"
            }
            slash = true;
            segment_started = false;
            continue;
        }
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '-' || c == '+';
        if (!ok) {
            return false;
        }
        segment_started = true;
    }
    // "Etc/UTC" and "America/Argentina/Buenos_Aires" pass; anything without a
    // '/' does not, nor does a trailing '/'. That rejects POSIX rule strings
    // ("EST5EDT,M3.2.0,M11.1.0") at the cost of the few slash-free tzdb names
    // ("UTC", "EST5EDT"), which the caller then reports as unknown.
    return slash && segment_started;
}

std::string host_time_zone(const char* tz_env, const std::string& etc_dir) {
    if (tz_env != nullptr) {
        std::string tz = tz_env;
        if (!tz.empty() && tz.front() == ':') {
            tz.erase(0, 1);
        }
        // TZ may also carry an absolute zoneinfo path (":/usr/share/zoneinfo/X"),
        // or a path relative to the zoneinfo directory ("posix/America/Denver",
        // legal for glibc); the same prefix rule applies to both.
        if (!tz.empty() && tz.front() == '/') {
            tz = zone_from_zoneinfo_path(tz);
        } else {
            tz = strip_zoneinfo_subtree(tz);
        }
        if (looks_like_iana_zone(tz)) {
            return tz;
        }
        // A set-but-unusable TZ still governs localtime_r(), so do not let a
        // stale /etc/timezone contradict it; report "unknown" instead.
        return "";
    }

    // With TZ unset, tzset() reads /etc/localtime and nothing else, so the
    // symlink is the authority: it names the zone localtime_r() (and so the
    // log lines) actually use, even where /etc/timezone says otherwise. A
    // link that answers with a name this resolver cannot express (timedatectl
    // set-timezone UTC -> zoneinfo/UTC, no '/') is still an answer: report
    // "" rather than let a stale file contradict the zone the logs are in.
    std::error_code ec;
    const auto target = std::filesystem::read_symlink(etc_dir + "/localtime", ec);
    if (!ec) {
        const std::string zone = zone_from_zoneinfo_path(target.string());
        return looks_like_iana_zone(zone) ? zone : "";
    }

    // /etc/timezone only when there is no link to read (a regular-file copy
    // of the zone data carries no name).
    std::ifstream in(etc_dir + "/timezone");
    std::string line;
    if (in && std::getline(in, line)) {
        line = std::string(trim_view(line));
        if (looks_like_iana_zone(line)) {
            return line;
        }
    }
    return "";
}

std::string host_time_zone() { return host_time_zone(std::getenv("TZ"), "/etc"); }

}  // namespace util
}  // namespace alpacahttp
