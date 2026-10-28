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

#pragma once

// Scripted iOptron v3-protocol responder for FakeMountServer: enough of
// :MountInfo / :GLS / :GEP / :SRA / :Sd / :MS1 / :Z* to drive the telescope
// driver through connect -> GOTO -> settle and observe the GOTO final-
// approach RA trim (goto_refine_active()). The mount "lands" every GOTO at
// the commanded target plus a configurable RA error, exactly the HAE29C /
// HAE16 firmware signature, and reports a pulse-guide :ZQ/:ZS as having
// closed the error. Everything else gets a harmless ack.

#ifndef _WIN32

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fake_mount_server.h"

namespace alpacacore::test {

class FakeIoptronMount {
public:
    /// @param model_code 4-digit :MountInfo reply, e.g. "0012" (HAE16 EQ), "0025" (HEM27)
    /// @param landing_ra_error_arcsec RA offset (east positive) the mount settles at after a GOTO
    FakeIoptronMount(std::string model_code, double landing_ra_error_arcsec)
        : model_code_(std::move(model_code)),
          landing_error_arcsec_(landing_ra_error_arcsec),
          server_([this](const std::string& chunk) { return respond(chunk); }) {}

    bool ok() const { return server_.ok(); }
    int port() const { return server_.port(); }

    /// open-astro#575: answer ":MS1"/":MS2" with "0" (GOTO rejected, as the
    /// firmware does for a target below the altitude limit) instead of "1".
    void set_reject_goto(bool reject) { reject_goto_.store(reject); }

    /// open-astro#728: answer the position, Alt/Az and status reads (":GEP", ":GAC", ":GLS") with a short "0#" the
    /// driver cannot parse, so every such read fails until this is cleared. Commands are still acknowledged.
    void set_fail_reads(bool fail) { fail_reads_.store(fail); }

    /// open-astro#728: reset the driver's connection, so its next send (a blind ":Q#" included) fails.
    void reset_link() { server_.reset_connections(); }

    /// Firmware system status the ":GLS" read reports: 1 = tracking (default), 2 = slewing. Lets a test make the
    /// firmware say "moving" with no GOTO issued, as during a pulse-guide or a slew started from the handset.
    void set_system_status(int status) { system_status_.store(status); }

    void set_utc_reply(std::string reply) {
        std::lock_guard<std::mutex> lock(mutex_);
        utc_reply_ = std::move(reply);
    }

    /// GPS digit of the ":GLS" reply (0 no or failed GPS module, 1 GPS works but no valid data, 2 valid GPS data)
    /// together with the site that reply carries: the receiver's own reading when the digit is 2, otherwise the
    /// mount's stored site. Defaults: digit 0 and an all-zero site field.
    void set_gps(int status, double latitude_degrees, double longitude_degrees) {
        gps_status_.store(status);
        // 0.01 arcsec units: longitude signed, latitude stored as latitude + 90 degrees.
        longitude_units_.store(std::llround(longitude_degrees * 3600.0 * 100.0));
        latitude_field_.store(std::llround((latitude_degrees + 90.0) * 3600.0 * 100.0));
    }

    /// One shot, then spent: the reply to the next chunk received (one recv, which may carry several commands)
    /// is held for @p delay. A connect waiting on that reply stays open that long; used by the contract sweep to
    /// make Connecting observable.
    void hold_next_reply(std::chrono::milliseconds delay) {
        holding_.store(false);
        hold_ms_.store(static_cast<int>(delay.count()));
    }
    // True once a held reply has begun waiting, before its command is recorded: a test uses it to act while the
    // transaction is in flight. Latched until hold_next_reply() is called again.
    bool wait_until_holding(std::chrono::milliseconds timeout) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!holding_.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return holding_.load();
    }

    std::vector<std::string> commands() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return commands_;
    }
    int count(const std::string& prefix) const {
        std::lock_guard<std::mutex> lock(mutex_);
        int n = 0;
        for (const auto& c : commands_) {
            if (c.rfind(prefix, 0) == 0) {
                ++n;
            }
        }
        return n;
    }

private:
    // iOptron wire units: RA and Dec in 0.01 arcsec.
    static std::string nine(long long v) {
        char b[32];
        std::snprintf(b, sizeof(b), "%09lld", v);
        return b;
    }
    static std::string signed_eight(long long v) {
        char b[32];
        std::snprintf(b, sizeof(b), "%c%08lld", v < 0 ? '-' : '+', v < 0 ? -v : v);
        return b;
    }

    std::string respond(const std::string& chunk) {
        if (const int hold_ms = hold_ms_.exchange(0); hold_ms > 0) {
            holding_.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
        }
        std::string out;
        std::string cmd;
        for (char ch : chunk) {
            cmd += ch;
            if (ch == '#') {
                out += handle(cmd);
                cmd.clear();
            }
        }
        if (!cmd.empty()) {
            out += handle(cmd + "#");
        }
        return out;
    }

    std::string handle(const std::string& cmd) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            commands_.push_back(cmd);
        }
        if (cmd == ":MountInfo#") {
            return model_code_;  // 4 bytes, no '#', as the real firmware
        }
        if (fail_reads_.load() && (cmd == ":GEP#" || cmd == ":GAC#" || cmd == ":GLS#")) {
            return "0#";
        }
        if (cmd == ":GLS#") {
            // sign + 16 digits (site: longitude, then latitude + 90 degrees), then 6 status digits: GPS,
            // system status (1 = tracking, 2 = slewing), rate, speed, time source, hemisphere.
            char site[32];
            const long long lon = longitude_units_.load();
            std::snprintf(site, sizeof(site), "%c%08lld%08lld", lon < 0 ? '-' : '+', lon < 0 ? -lon : lon,
                          latitude_field_.load());
            return std::string(site) + std::to_string(gps_status_.load()) + std::to_string(system_status_.load()) +
                   "0001#";
        }
        if (cmd == ":GUT#") {
            std::lock_guard<std::mutex> lock(mutex_);
            return utc_reply_;
        }
        // The driver no longer reads :GAL / :GMT (#763). Keep these answers:
        // without them the old override failed its read and never sent :SAL /
        // :SMT, so the #763 case would pass with the fix reverted.
        if (cmd == ":GAL#") {
            return "+00#";  // user altitude limit 0 degrees
        }
        if (cmd.rfind(":GMT", 0) == 0) {
            return "0030#";  // meridian treatment: stop, 30 degrees past
        }
        if (cmd == ":GEP#") {
            const long long ra = ra_units_.load();
            const long long dec = dec_units_.load();
            return signed_eight(dec) + nine(ra) + "01#";
        }
        if (cmd.rfind(":SRA", 0) == 0) {
            pending_ra_ = std::atoll(cmd.substr(4, 9).c_str());
            return "1";
        }
        if (cmd.rfind(":Sd", 0) == 0) {
            pending_dec_ = std::atoll(cmd.substr(3, 9).c_str());
            return "1";
        }
        if (cmd == ":MS1#" || cmd == ":MS2#") {
            if (reject_goto_.load()) {
                return "0";  // rejected: nothing moves
            }
            // Land at the target plus the firmware's final-approach error.
            ra_units_.store(pending_ra_ + static_cast<long long>(landing_error_arcsec_ * 100.0));
            dec_units_.store(pending_dec_);
            return "1";
        }
        if (cmd.rfind(":ZQ", 0) == 0 || cmd.rfind(":ZS", 0) == 0) {
            // A pulse-guide trim closes the residual exactly.
            ra_units_.store(pending_ra_);
            return "1";
        }
        if (cmd.rfind(":Z", 0) == 0 || cmd.rfind(":S", 0) == 0 || cmd.rfind(":M", 0) == 0 || cmd.rfind(":R", 0) == 0 ||
            cmd.rfind(":Q", 0) == 0) {
            return "1";  // set / motion / rate acks
        }
        return "0#";  // any other query: harmless, per-command parse failures are tolerated
    }

    std::atomic<int> system_status_{1};
    std::atomic<int> gps_status_{0};
    std::atomic<long long> longitude_units_{0};
    std::atomic<long long> latitude_field_{0};
    std::string model_code_;
    double landing_error_arcsec_;
    std::atomic<int> hold_ms_{0};
    std::atomic<bool> holding_{false};
    mutable std::mutex mutex_;
    std::string utc_reply_ = "+00000000000000000#";
    std::vector<std::string> commands_;
    long long pending_ra_ = 0;
    long long pending_dec_ = 0;
    std::atomic<long long> ra_units_{0};
    std::atomic<long long> dec_units_{0};
    std::atomic<bool> reject_goto_{false};
    std::atomic<bool> fail_reads_{false};
    FakeMountServer server_;
};

}  // namespace alpacacore::test

#endif  // !_WIN32
