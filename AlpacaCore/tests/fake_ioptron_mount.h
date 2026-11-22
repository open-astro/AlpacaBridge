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
// :MountInfo / :GLS / :GEP / :GAC / :SRA / :Sd / :MS1 / :AG / :RG / :Z* to drive the telescope
// driver through connect -> GOTO -> settle and observe the GOTO final-
// approach RA trim (goto_refine_active()). The mount "lands" every GOTO at
// the commanded target plus a configurable RA error, exactly the HAE29C /
// HAE16 firmware signature, and reports a pulse-guide :ZQ/:ZS as having
// closed the error. The arrow commands :mw#/:me#/:mn#/:ms# step the modelled
// RA or Dec (issue #2424). Everything else gets a harmless ack.
//
// ConformU mode (#2614, the fake ConformU row of #2078): constructed with a FakeMountMotionModel::Config, the same
// wire answers the reads and the motion commands from the shared motion model instead: a GOTO moves over time
// (:GLS reads 2 until it lands), :MP1# parks (6 once it arrives), :MH#/:MSH# go to the zero position (7), tracking
// follows :ST1#/:ST0#, a :Z pulse and the arrows move the axes at the guide and :SR rates, :SLO/:SLA set the site and
// :SUT/:GUT keep the mount's clock. The scripted behaviour above is the default and does not change.

#ifndef _WIN32

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "connect_hold_gate.h"
#include "fake_mount_motion_model.h"
#include "fake_mount_server.h"
#include "fake_pty_write.h"

namespace alpacacore::test {

class FakeIoptronMount {
public:
    /// @param model_code 4-digit :MountInfo reply, e.g. "0012" (HAE16 EQ), "0025" (HEM27)
    /// @param landing_ra_error_arcsec RA offset (east positive) the mount settles at after a GOTO
    FakeIoptronMount() : FakeIoptronMount("0036", 0.0) {}  // HAE29C; PolledRig default-constructs its fake

    FakeIoptronMount(std::string model_code, double landing_ra_error_arcsec)
        : model_code_(std::move(model_code)),
          landing_error_arcsec_(landing_ra_error_arcsec),
          server_([this](const std::string& chunk) { return respond(chunk); }) {}

    /// ConformU mode (#2614): answers from a FakeMountMotionModel built from @p motion (see the file comment). The
    /// scripted knobs below that shape a reply (set_reject_goto(), set_fail_reads(), hold_next_reply(), ...) still
    /// apply; the ones that script the motion itself (set_position(), set_system_status(), set_park_lands(), the
    /// landing error) are the model's business here.
    FakeIoptronMount(std::string model_code, FakeMountMotionModel::Config motion)
        : model_code_(std::move(model_code)),
          landing_error_arcsec_(0.0),
          motion_(std::make_unique<FakeMountMotionModel>(std::move(motion))),
          server_([this](const std::string& chunk) { return respond(chunk); }) {
        longitude_units_.store(std::llround(motion_->site_longitude_degrees() * 3600.0 * 100.0));
        latitude_field_.store(std::llround((motion_->site_latitude_degrees() + 90.0) * 3600.0 * 100.0));
    }

    /// The motion model of a ConformU-mode mount; null for the scripted default.
    FakeMountMotionModel* motion() const { return motion_.get(); }

    /// The model setup of the fake ConformU row: the made-up Sydney Opera House site (never a real observing site),
    /// a 6 degree/s GOTO (the driver's top MoveAxis rate) and the zero position the mount parks and homes at, the
    /// counterweight-down pole pointing the HAE29C finds (hour angle -6 h, Dec at the site's pole, issue #2420).
    static FakeMountMotionModel::Config conformu_motion_config() {
        FakeMountMotionModel::Config config;
        config.latitude_degrees = -33.856784;
        config.longitude_degrees = 151.215297;
        config.slew_deg_per_s = 6.0;
        config.park = config.home = {-6.0, -90.0};
        return config;
    }

    bool ok() const { return server_.ok(); }
    int port() const { return server_.port(); }

    /// The ":MountInfo" reply for a connect that comes after this call (a rig builds its fake before the case picks a
    /// model). Not for a mount that is already connected.
    void set_model_code(std::string model_code) { model_code_ = std::move(model_code); }

    /// The RA offset (arcsec, east positive) the next GOTO lands at, as the constructor's landing_ra_error_arcsec.
    void set_landing_ra_error(double arcsec) { landing_error_arcsec_.store(arcsec); }

    /// Every reply is held for @p delay (real time) from now on, 0 to stop. Unlike hold_next_reply() it is not spent
    /// by one chunk and it never waits on a ConnectHoldGate, so a mount that answers slowly stays slow: a getter that
    /// sends an exchange pays the delay, and the poll's three reads pay it three times. A reply held by
    /// hold_next_reply() when this is called keeps its hold alone; the replies after it pay the delay.
    void set_reply_delay(std::chrono::milliseconds delay) { reply_delay_ms_.store(static_cast<int>(delay.count())); }

    /// When the command @p cmd (exactly, "#" included) is received the ":GLS" system status becomes @p status, as a
    /// firmware that reacts to it: ":ST0#" ends a wedged park at 6 (parked), ":Q#" ends a slew. The command is
    /// still handled as before. Replaces an earlier rule for the same command.
    void set_status_on_command(std::string cmd, int status) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_on_command_[std::move(cmd)] = status;
    }

    /// open-astro#575: answer ":MS1"/":MS2" with "0" (GOTO rejected, as the
    /// firmware does for a target below the altitude limit) instead of "1".
    void set_reject_goto(bool reject) { reject_goto_.store(reject); }

    /// Issue #2395: ":MS1#" (normal position) answers "0", but ":MS2#" (counterweight up) is accepted and moves the
    /// fake to the target, so a driver that re-aims a refused GOTO shows up as motion, not only as a ledger entry.
    void set_reject_normal_goto(bool reject) { reject_normal_goto_.store(reject); }

    /// Issue #2396: answer ":MS1#" with @p reply ("" is no reply at all) in place of "1". With @p moves the GOTO
    /// still runs (status 2 and the landing, as an accepted one); without it nothing moves. Cleared by
    /// clear_goto_reply().
    void set_goto_reply(std::string reply, bool moves) {
        std::lock_guard<std::mutex> lock(mutex_);
        goto_reply_ = std::move(reply);
        goto_reply_moves_ = moves;
        goto_reply_set_ = true;
    }
    void clear_goto_reply() {
        std::lock_guard<std::mutex> lock(mutex_);
        goto_reply_set_ = false;
    }

    /// Issue #2395: the next @p n position, Alt/Az or status reads (":GEP", ":GAC", ":GLS") get the unparseable "0#",
    /// then the reads answer again.
    void fail_next_reads(int n) { fail_next_reads_.store(n); }

    /// Issue #2396: after the next accepted ":MS1#", the next @p n position reads (":GEP#" only) get the unparseable
    /// "0#", so a synchronous slew's landing wait sees failed reads while its status reads still answer.
    void fail_position_reads_after_goto(int n) { fail_gep_after_goto_.store(n); }

    /// Issue #2396: the next @p n position reads (":GEP#" only) get the unparseable "0#" from now on.
    void fail_next_position_reads(int n) { gep_failing_.store(n); }

    /// Issue #2396: every ":GEP#" read first moves the modelled RA by @p arcsec (0.01 arcsec units underneath), so the
    /// position never settles. 0 (the default) keeps it still.
    void set_position_drift_per_read(double arcsec) { drift_units_.store(std::llround(arcsec * 100.0)); }

    /// Issue #2434: the ":AG#" guide-rate read answers "nnnn#", RA then Dec in hundredths of sidereal; ":RG" writes
    /// it. Default "5050#" (0.50x both). An empty @p reply answers the unparseable "0#".
    void set_guide_rate_reply(std::string reply) {
        std::lock_guard<std::mutex> lock(mutex_);
        guide_rate_reply_ = std::move(reply);
    }

    /// open-astro#728: answer the position, Alt/Az and status reads (":GEP", ":GAC", ":GLS") with a short "0#" the
    /// driver cannot parse, so every such read fails until this is cleared. Commands are still acknowledged.
    void set_fail_reads(bool fail) { fail_reads_.store(fail); }

    /// open-astro#728: reset the driver's connection, so its next send (a blind ":Q#" included) fails.
    void reset_link() { server_.reset_connections(); }

    /// Firmware system status the ":GLS" read reports: 1 = tracking (default), 2 = slewing. Lets a test make the
    /// firmware say "moving" with no GOTO issued, as during a pulse-guide or a slew started from the handset.
    void set_system_status(int status) { system_status_.store(status); }

    /// Issue #2000, HAE29C firmware: an accepted GOTO reports status 2 (slewing, not tracking) until it lands, ":Q#"
    /// stops it without turning tracking on (status 0), and ":ST1#" / ":ST0#" set status 1 / 0. Off by default: a
    /// GOTO then lands at once and ":ST"/":Q" change no status, as before.
    void set_goto_tracks_off(bool on) { goto_tracks_off_.store(on); }

    /// Issue #2562: ":MP1#" (park) makes the ":GLS" system status read 6 (parked) at once. Off by default: a Park then
    /// changes no status, as before, and a case sets the status it needs with set_system_status().
    void set_park_lands(bool on) { park_lands_.store(on); }

    /// Issue #2186, HAE29C firmware: a ":Z" pulse-guide command makes the ":GLS" system status read 3 (auto-guiding)
    /// in place of 1 for the whole of the pulse. The fake has no pulse timer, so the status stays 3 until a test sets
    /// another with set_system_status(). Off by default: a pulse then changes no status, as before.
    void set_pulse_reports_guiding(bool on) { pulse_reports_guiding_.store(on); }

    /// Issue #2421: false answers a ":Z" pulse-guide command with nothing, as the HAE29C firmware (and the v3 spec)
    /// does. On by default: a pulse is acked with "1", which also tells a TCP test the command was recorded and its
    /// :ZQ/:ZS trim applied before the driver call returned.
    void set_pulse_acks(bool on) { pulse_acks_.store(on); }

    /// Tracking-rate digit of the ":GLS" reply (0 sidereal, 1 lunar, 2 solar, 3 King, 4 custom), as the firmware
    /// reports the rate the mount tracks at. A ":RTn#" command sets it to n, as the firmware does. Default 0.
    void set_tracking_rate_digit(int rate) { tracking_rate_digit_.store(rate); }

    /// Pier-side digit of the ":GEP" reply (0 pier east, 1 pier west, 2 indeterminate). Default 0. It also picks
    /// which way ":mn#"/":ms#" turn the modelled Dec (see the arrow-motion model in handle()).
    void set_pier_side(int side) { pier_side_.store(side); }

    /// Where the ":GEP" read says the mount points, until a GOTO, a trim pulse or an arrow command moves it.
    void set_position(double ra_hours, double dec_degrees) {
        ra_units_.store(std::llround(ra_hours * 15.0 * 3600.0 * 100.0));
        dec_units_.store(std::llround(dec_degrees * 3600.0 * 100.0));
    }

    /// Where the fake points now (what ":GEP" answers).
    double ra_hours() const { return static_cast<double>(ra_units_.load()) / (15.0 * 3600.0 * 100.0); }
    double dec_degrees() const { return static_cast<double>(dec_units_.load()) / (3600.0 * 100.0); }

    /// Answers ":GAC" with this Alt/Az (0.01 arcsec units) from now on; unset, the read gets the unparseable "0#".
    void set_alt_az(double altitude_degrees, double azimuth_degrees) {
        alt_units_.store(std::llround(altitude_degrees * 3600.0 * 100.0));
        az_units_.store(std::llround(azimuth_degrees * 3600.0 * 100.0));
        altaz_enabled_.store(true);
    }

    void set_utc_reply(std::string reply) {
        std::lock_guard<std::mutex> lock(mutex_);
        utc_reply_ = std::move(reply);
    }

    /// Reply to the park position read (":GPC"), 8 Alt then 9 Az digits in 0.01 arcsec and '#'. Empty (the default)
    /// keeps the "0#" the driver cannot parse, so a HAE29C Park arms no wedged-park finalizer.
    void set_park_position_reply(std::string reply) {
        std::lock_guard<std::mutex> lock(mutex_);
        park_position_reply_ = std::move(reply);
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

    /// Answer ":GEP#" with @p reply in place of the modelled position; "" restores the model.
    void set_position_reply(std::string reply) {
        std::lock_guard<std::mutex> lock(mutex_);
        position_reply_ = std::move(reply);
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

    /// The reply to one received chunk, recorded like a TCP one; PtyIoptronMount answers through it.
    std::string respond_to(const std::string& chunk) { return respond(chunk); }

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
        // Read before the hold, so a delay set while a reply is held applies to the replies after it only.
        const int delay_ms = reply_delay_ms_.load();
        if (const int hold_ms = hold_ms_.exchange(0); hold_ms > 0) {
            holding_.store(true);
            alpacacore::test::connect_hold(std::chrono::milliseconds(hold_ms));  // connect_hold_gate.h
        }
        if (delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
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
            if (const auto rule = status_on_command_.find(cmd); rule != status_on_command_.end()) {
                system_status_.store(rule->second);
            }
        }
        if (cmd == ":MountInfo#") {
            return model_code_;  // 4 bytes, no '#', as the real firmware
        }
        if (fail_reads_.load() && (cmd == ":GEP#" || cmd == ":GAC#" || cmd == ":GLS#")) {
            return "0#";
        }
        if ((cmd == ":GEP#" || cmd == ":GAC#" || cmd == ":GLS#") && fail_next_reads_.load() > 0) {
            fail_next_reads_.fetch_sub(1);
            return "0#";
        }
        if (cmd == ":GEP#" && gep_failing_.load() > 0) {
            gep_failing_.fetch_sub(1);
            return "0#";
        }
        if (motion_) {
            if (auto reply = handle_modelled(cmd)) return *reply;
        }
        if (cmd == ":AG#") {
            std::lock_guard<std::mutex> lock(mutex_);
            return guide_rate_reply_.empty() ? std::string("0#") : guide_rate_reply_;
        }
        if (cmd.rfind(":RG", 0) == 0 && cmd.size() == 8) {
            std::lock_guard<std::mutex> lock(mutex_);
            guide_rate_reply_ = cmd.substr(3, 4) + "#";
            return "1";  // the generic set ack below, as before
        }
        if (cmd == ":GLS#") {
            // sign + 16 digits (site: longitude, then latitude + 90 degrees), then 6 status digits: GPS,
            // system status (1 = tracking, 2 = slewing), rate, speed, time source, hemisphere.
            char site[32];
            const long long lon = longitude_units_.load();
            std::snprintf(site, sizeof(site), "%c%08lld%08lld", lon < 0 ? '-' : '+', lon < 0 ? -lon : lon,
                          latitude_field_.load());
            return std::string(site) + std::to_string(gps_status_.load()) + std::to_string(system_status_.load()) +
                   std::to_string(tracking_rate_digit_.load()) + "001#";
        }
        if (cmd == ":GUT#") {
            std::lock_guard<std::mutex> lock(mutex_);
            return utc_reply_;
        }
        if (cmd == ":GPC#") {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!park_position_reply_.empty()) return park_position_reply_;
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
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!position_reply_.empty()) return position_reply_;
            }
            const long long ra = ra_units_.fetch_add(drift_units_.load()) + drift_units_.load();
            const long long dec = dec_units_.load();
            return signed_eight(dec) + nine(ra) + std::to_string(pier_side_.load()) + "1#";
        }
        if (cmd == ":GAC#" && altaz_enabled_.load()) {
            // Altitude (sign + 8 digits) then azimuth (9 digits), 0.01 arcsec: the point set_alt_az() gave, so
            // the Alt/Az cache is primed at connect like the position and status caches (#1829). Off by default:
            // the wrapper's HAE29C park pre-check then skips its :GAC/:GPC comparison, as the older cases expect.
            return signed_eight(alt_units_.load()) + nine(az_units_.load()) + "#";
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
            if (reject_goto_.load() || (cmd == ":MS1#" && reject_normal_goto_.load())) {
                return "0";  // rejected: nothing moves
            }
            // The firmware's altitude limit (":GAL#" answers 0 degrees below): a target under the horizon is refused.
            if (motion_ && motion_->altitude_degrees_of(pending_ra_hours(), pending_dec_degrees()) < 0.0) {
                return "0";
            }
            std::string reply = "1";
            if (cmd == ":MS1#") {
                std::lock_guard<std::mutex> lock(mutex_);
                if (goto_reply_set_) {
                    reply = goto_reply_;
                    if (!goto_reply_moves_) return reply;  // nothing moves
                }
            }
            gep_failing_.store(fail_gep_after_goto_.exchange(0));
            if (motion_) {
                // The mount slews over the travel time and tracks once it arrives (:GLS reads 2, then 1).
                motion_->set_tracking(true);
                (void)motion_->goto_radec(pending_ra_hours(), pending_dec_degrees());
                return reply;
            }
            // Land at the target plus the firmware's final-approach error.
            ra_units_.store(pending_ra_ + static_cast<long long>(landing_error_arcsec_.load() * 100.0));
            dec_units_.store(pending_dec_);
            if (goto_tracks_off_.load()) system_status_.store(2);
            return reply;
        }
        if (cmd == ":MSH#") {
            // Issue #2420, HAE29C firmware: the zero sensor search is acked with a bare '1' and no '#', so a driver
            // that reads a '#'-terminated reply waits out its timeout. Its own branch, so a change to the generic
            // ":M" ack below cannot hand the search a '#' (issue #2432).
            return "1";
        }
        if (goto_tracks_off_.load() && (cmd == ":Q#" || cmd == ":ST1#" || cmd == ":ST0#")) {
            if (cmd == ":ST1#") {
                system_status_.store(1);
            } else if (cmd == ":ST0#" || system_status_.load() == 2) {
                system_status_.store(0);
            }
            return "1";
        }
        if (pulse_reports_guiding_.load() && cmd.rfind(":Z", 0) == 0) {
            system_status_.store(3);  // auto-guiding, for the whole of the pulse (issue #2186)
        }
        if (cmd.size() == 5 && cmd.rfind(":RT", 0) == 0 && cmd[3] >= '0' && cmd[3] <= '4') {
            tracking_rate_digit_.store(cmd[3] - '0');
            return "1";
        }
        if (cmd == ":mw#" || cmd == ":me#" || cmd == ":ms#" || cmd == ":mn#") {
            // Arrow motion (issue #2424), modelled as one kArrowStepUnits step per command, in iOptron's own
            // labels (RS-232 v3.10): ":mw#" RA+ and ":me#" RA-, where RA+ raises the RightAscension reading as the
            // ":ZS" pulse (RA+) does on the HAE16 and HAE29C (the GOTO trim). ":ms#" is Dec+ and ":mn#" Dec-, a
            // mechanical sense: on pier side 0 Dec+ lowers the Declination reading and on side 1 it raises it, the
            // Dec pulse polarity the driver assumes. The #2424 HAE29C rig check (2026-11-20, USB) confirmed this on
            // both pier sides.
            // No reply, as the firmware.
            if (cmd == ":mw#" || cmd == ":me#") {
                ra_units_.fetch_add(cmd == ":mw#" ? kArrowStepUnits : -kArrowStepUnits);
            } else {
                const bool dec_plus = (cmd == ":ms#");
                const bool raises_dec = (pier_side_.load() == 1) ? dec_plus : !dec_plus;
                dec_units_.fetch_add(raises_dec ? kArrowStepUnits : -kArrowStepUnits);
            }
            return "";
        }
        if (cmd.rfind(":ZQ", 0) == 0 || cmd.rfind(":ZS", 0) == 0) {
            // A pulse-guide trim closes the residual exactly.
            ra_units_.store(pending_ra_);
        }
        if (cmd.rfind(":Z", 0) == 0) {
            return pulse_acks_.load() ? "1" : "";
        }
        if (cmd == ":MP1#" && park_lands_.load()) {
            system_status_.store(6);
        }
        if (cmd.rfind(":S", 0) == 0 || cmd.rfind(":M", 0) == 0 || cmd.rfind(":R", 0) == 0 || cmd.rfind(":Q", 0) == 0) {
            return "1";  // set / motion / rate acks
        }
        return "0#";  // any other query: harmless, per-command parse failures are tolerated
    }

    // ---- ConformU mode: the commands the shared motion model answers (#2614) ----

    double pending_ra_hours() const { return static_cast<double>(pending_ra_) / (15.0 * 3600.0 * 100.0); }
    double pending_dec_degrees() const { return static_cast<double>(pending_dec_) / (3600.0 * 100.0); }

    // iOptron's arrow speeds (":SRn#", n 1..9) as the driver offers them as AxisRates: 1, 2, 8, 16, 64, 128, 256 and
    // 512 times 15 arcsec/s, then the 6 degree/s maximum.
    static double arrow_rate_deg_per_s(int level) {
        static constexpr double kSteps[] = {1.0, 2.0, 8.0, 16.0, 64.0, 128.0, 256.0, 512.0};
        if (level >= 1 && level <= 8) return kSteps[level - 1] * 15.0 / 3600.0;
        return 6.0;
    }

    // The ":AG#" guide rate in degrees per second: RA then Dec, each two digits of hundredths of sidereal.
    double guide_rate_deg_per_s(bool ra_axis) {
        std::string reply;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            reply = guide_rate_reply_;
        }
        if (reply.size() < 4) return 0.5 * FakeMountMotionModel::kSiderealDegPerSec;
        return std::atoi(reply.substr(ra_axis ? 0 : 2, 2).c_str()) / 100.0 * FakeMountMotionModel::kSiderealDegPerSec;
    }

    // Dec polarity of the arrow and pulse commands, in iOptron's labels (":ms#" and ":ZE#" are Dec+, ":mn#" and
    // ":ZC#" Dec-): on pier side 0 Dec+ lowers the reading, on side 1 it raises it (the #2424 rig check).
    double dec_sign(bool labelled_dec_plus) const {
        const bool raises_dec = (motion_->pier_side() == 1) ? labelled_dec_plus : !labelled_dec_plus;
        return raises_dec ? 1.0 : -1.0;
    }

    static long long ra_units_of(double ra_hours) {
        const long long units = std::llround(ra_hours * 15.0 * 3600.0 * 100.0);
        return units >= 129600000LL ? units - 129600000LL : units;
    }

    // The ":GLS#" system status the model implies: 6 parked, 2 slewing (a GOTO, Park, the zero search or an arrow
    // move), 7 stopped at the zero position, 3 auto-guiding for the length of a pulse, 1 tracking, else 0 stopped.
    static int modelled_status(const FakeMountMotionModel::State& m) {
        if (m.parked) return 6;
        if (m.slewing || m.axis_moving[0] || m.axis_moving[1]) return 2;
        if (m.at_home) return 7;
        if (m.pulse_active) return 3;
        return m.tracking ? 1 : 0;
    }

    std::optional<std::string> handle_modelled(const std::string& cmd) {
        using Axis = FakeMountMotionModel::Axis;
        if (cmd == ":GEP#") {
            const auto m = motion_->state();
            return signed_eight(std::llround(m.dec_degrees * 3600.0 * 100.0)) + nine(ra_units_of(m.ra_hours)) +
                   std::to_string(m.pier_side) + "1#";
        }
        if (cmd == ":GAC#") {
            return signed_eight(std::llround(motion_->altitude_degrees() * 3600.0 * 100.0)) +
                   nine(std::llround(motion_->azimuth_degrees() * 3600.0 * 100.0) % 129600000LL) + "#";
        }
        if (cmd == ":GLS#") {
            char site[32];
            const long long lon = longitude_units_.load();
            std::snprintf(site, sizeof(site), "%c%08lld%08lld", lon < 0 ? '-' : '+', lon < 0 ? -lon : lon,
                          latitude_field_.load());
            return std::string(site) + std::to_string(gps_status_.load()) +
                   std::to_string(modelled_status(motion_->state())) + std::to_string(tracking_rate_digit_.load()) +
                   "001#";
        }
        if (cmd.rfind(":SLO", 0) == 0 || cmd.rfind(":SLA", 0) == 0) {
            const long long units = std::atoll(cmd.substr(4, 9).c_str());
            const bool is_lon = (cmd[3] == 'O');
            (is_lon ? longitude_units_ : latitude_field_).store(is_lon ? units : units + 90LL * 360000LL);
            motion_->set_site(static_cast<double>(latitude_field_.load() - 90LL * 360000LL) / 360000.0,
                              static_cast<double>(longitude_units_.load()) / 360000.0);
            return std::string("1");
        }
        if (cmd.rfind(":SUT", 0) == 0) {
            utc_offset_ms_.store(std::atoll(cmd.substr(4, 13).c_str()) - ms_since_j2000(motion_->now()));
            return std::string("1");
        }
        if (cmd == ":GUT#") {
            char b[32];
            std::snprintf(b, sizeof(b), "+000%d%013lld#", dst_observed_.load() ? 1 : 0,
                          ms_since_j2000(motion_->now()) + utc_offset_ms_.load());
            return std::string(b);
        }
        if (cmd == ":SDS1#" || cmd == ":SDS0#") {
            dst_observed_.store(cmd == ":SDS1#");
            return std::string("1");
        }
        if (cmd.size() == 5 && cmd.rfind(":SR", 0) == 0 && cmd[3] >= '1' && cmd[3] <= '9') {
            arrow_level_.store(cmd[3] - '0');
            return std::string("1");
        }
        if (cmd == ":mw#" || cmd == ":me#") {
            motion_->move_axis(Axis::Ra, (cmd == ":mw#" ? 1.0 : -1.0) * arrow_rate_deg_per_s(arrow_level_.load()));
            return std::string();  // no reply, as the firmware
        }
        if (cmd == ":ms#" || cmd == ":mn#") {
            motion_->move_axis(Axis::Dec, dec_sign(cmd == ":ms#") * arrow_rate_deg_per_s(arrow_level_.load()));
            return std::string();
        }
        if (cmd == ":qR#" || cmd == ":qD#") {
            motion_->move_axis(cmd == ":qR#" ? Axis::Ra : Axis::Dec, 0.0);
            return std::string("1");
        }
        if (cmd == ":Q#") {
            motion_->stop();
            return std::string("1");
        }
        if (cmd == ":ST1#" || cmd == ":ST0#") {
            motion_->set_tracking(cmd == ":ST1#");
            return std::string("1");
        }
        if (cmd == ":MP1#") {
            motion_->park();
            return std::string("1");
        }
        if (cmd == ":MP0#") {
            motion_->unpark();
            return std::string("1");
        }
        if (cmd == ":MH#" || cmd == ":MSH#") {
            motion_->go_home();
            return std::string("1");  // ":MSH#" is a bare '1' with no '#' (issue #2420), as the scripted branch
        }
        if (cmd == ":CM#") {
            motion_->set_position(pending_ra_hours(), pending_dec_degrees());
            return std::string("1");
        }
        if (cmd.size() == 9 && cmd.rfind(":Z", 0) == 0 && std::string("SQEC").find(cmd[2]) != std::string::npos) {
            using std::chrono::milliseconds;
            const milliseconds duration(std::atoi(cmd.substr(3, 5).c_str()));
            if (cmd[2] == 'S' || cmd[2] == 'Q') {
                motion_->pulse(Axis::Ra, (cmd[2] == 'S' ? 1.0 : -1.0) * guide_rate_deg_per_s(true), duration);
            } else {
                motion_->pulse(Axis::Dec, dec_sign(cmd[2] == 'E') * guide_rate_deg_per_s(false), duration);
            }
            return std::string(pulse_acks_.load() ? "1" : "");
        }
        return std::nullopt;
    }

    static long long ms_since_j2000(FakeMountMotionModel::TimePoint at) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   at - FakeMountMotionModel::TimePoint{std::chrono::seconds(946728000)})
            .count();
    }

    static constexpr long long kArrowStepUnits = 60LL * 100LL;  // 60 arcsec in 0.01 arcsec units
    std::atomic<int> pier_side_{0};
    std::atomic<int> system_status_{1};
    std::atomic<int> tracking_rate_digit_{0};
    std::atomic<int> gps_status_{0};
    std::atomic<long long> longitude_units_{0};
    std::atomic<long long> latitude_field_{0};
    std::string model_code_;
    std::atomic<double> landing_error_arcsec_;
    std::string position_reply_;  // under mutex_
    std::atomic<int> hold_ms_{0};
    std::atomic<int> reply_delay_ms_{0};
    std::map<std::string, int> status_on_command_;  // under mutex_
    std::atomic<bool> holding_{false};
    mutable std::mutex mutex_;
    std::string utc_reply_ = "+00000000000000000#";
    std::string park_position_reply_;
    std::vector<std::string> commands_;
    long long pending_ra_ = 0;
    long long pending_dec_ = 0;
    std::atomic<long long> ra_units_{0};
    std::atomic<long long> dec_units_{0};
    std::atomic<bool> altaz_enabled_{false};
    std::atomic<long long> alt_units_{0};
    std::atomic<long long> az_units_{0};
    std::atomic<bool> park_lands_{false};
    std::atomic<bool> reject_goto_{false};
    std::atomic<bool> reject_normal_goto_{false};
    std::string goto_reply_;
    bool goto_reply_moves_ = false;
    bool goto_reply_set_ = false;
    std::atomic<int> fail_next_reads_{0};
    std::atomic<int> fail_gep_after_goto_{0};
    std::atomic<int> gep_failing_{0};
    std::atomic<long long> drift_units_{0};
    std::string guide_rate_reply_ = "5050#";
    std::atomic<bool> fail_reads_{false};
    std::atomic<bool> goto_tracks_off_{false};
    std::atomic<bool> pulse_reports_guiding_{false};
    std::atomic<bool> pulse_acks_{true};
    std::unique_ptr<FakeMountMotionModel> motion_;  // ConformU mode only; before server_, whose thread reads it
    std::atomic<int> arrow_level_{1};
    std::atomic<long long> utc_offset_ms_{0};
    std::atomic<bool> dst_observed_{false};
    FakeMountServer server_;
};

/// The same responder over a pty, for the serial transport: a USB cable can be pulled (issue #1723). Every
/// '#'-terminated command goes through FakeIoptronMount::respond_to(), so the setters and commands() of model()
/// apply; its TCP server stays idle.
class PtyIoptronMount {
public:
    explicit PtyIoptronMount(std::string model_code = "0012")
        : model_(std::move(model_code), /*landing_ra_error_arcsec=*/0.0), pty_("PtyIoptronMount") {
        reader_ = std::thread([this] { run(); });
    }
    ~PtyIoptronMount() { stop_reader(); }
    PtyIoptronMount(const PtyIoptronMount&) = delete;
    PtyIoptronMount& operator=(const PtyIoptronMount&) = delete;

    FakeIoptronMount& model() { return model_; }
    std::string slave_path() const { return pty_.slave_path(); }

    /// A USB unplug while the mount is idle: both pty ends close, so the driver's next write fails with EIO and its
    /// next read hangs up. Not reversible.
    void unplug() {
        stop_reader();
        unplugged_.store(true);  // before the close: whoever sees the hangup also sees unplugged()
        pty_.sever();
    }

    /// A USB unplug in the middle of an exchange: the first command `when` accepts is recorded and left unanswered,
    /// and both pty ends close, so the driver's read for its reply (or its ack drain) hangs up. Set it before the
    /// exchange; `when` runs on the reader thread. Not reversible.
    void unplug_on(std::function<bool(const std::string& cmd)> when) {
        std::lock_guard<std::mutex> lock(mutex_);
        unplug_on_ = std::move(when);
    }

    bool unplugged() const { return unplugged_.load(); }

private:
    void stop_reader() {
        stop_.store(true);
        if (reader_.joinable()) reader_.join();
    }

    void run() {
        std::string pending;
        char buf[64];
        while (!stop_.load()) {
            struct pollfd pfd {};
            pfd.fd = pty_.master_fd();
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, 20) <= 0) continue;
            const ssize_t n = ::read(pty_.master_fd(), buf, sizeof(buf));
            if (n <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));  // EIO: no slave open right now
                continue;
            }
            for (ssize_t i = 0; i < n; ++i) {
                pending += buf[i];
                if (buf[i] != '#') continue;
                const std::string cmd = pending;
                pending.clear();
                bool pull = false;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    pull = unplug_on_ && unplug_on_(cmd);
                }
                if (pull) {
                    (void)model_.respond_to(cmd);  // recorded only; the reply never goes out
                    unplugged_.store(true);        // before the close, as unplug()
                    pty_.sever();
                    return;
                }
                const std::string reply = model_.respond_to(cmd);
                if (!reply.empty()) pty_write_bounded(pty_.master_fd(), reply, stop_);
            }
        }
    }

    FakeIoptronMount model_;
    PtyPair pty_;
    std::thread reader_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> unplugged_{false};
    mutable std::mutex mutex_;
    std::function<bool(const std::string&)> unplug_on_;
};

}  // namespace alpacacore::test

#endif  // !_WIN32
