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
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "fake_mount_server.h"

namespace alpacacore::test {

class FakeIoptronMount {
public:
    /// @param model_code 4-digit :MountInfo reply, e.g. "0012" (HAE16 EQ), "0025" (HEM27)
    /// @param landing_ra_error_arcsec RA offset (east positive) the mount settles at after a GOTO
    FakeIoptronMount(std::string model_code, double landing_ra_error_arcsec)
        : model_code_(std::move(model_code)), landing_error_arcsec_(landing_ra_error_arcsec),
          server_([this](const std::string& chunk) { return respond(chunk); }) {}

    bool ok() const { return server_.ok(); }
    int port() const { return server_.port(); }

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
        if (cmd == ":GLS#") {
            // sign + 16 digits (site), then 6 status digits: GPS, system
            // status (1 = tracking, 2 = slewing), rate, speed, time source,
            // hemisphere.
            return "+0000000000000000" "0" "1" "0" "0" "0" "1#";
        }
        if (cmd == ":GEP#") {
            const long long ra = ra_units_.load();
            const long long dec = dec_units_.load();
            return signed_eight(dec) + nine(ra) + "0" "1#";
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
        if (cmd.rfind(":Z", 0) == 0 || cmd.rfind(":S", 0) == 0 || cmd.rfind(":M", 0) == 0 ||
            cmd.rfind(":R", 0) == 0 || cmd.rfind(":Q", 0) == 0) {
            return "1";  // set / motion / rate acks
        }
        return "0#";  // any other query: harmless, per-command parse failures are tolerated
    }

    std::string model_code_;
    double landing_error_arcsec_;
    mutable std::mutex mutex_;
    std::vector<std::string> commands_;
    long long pending_ra_ = 0;
    long long pending_dec_ = 0;
    std::atomic<long long> ra_units_{0};
    std::atomic<long long> dec_units_{0};
    FakeMountServer server_;
};

}  // namespace alpacacore::test

#endif  // !_WIN32
