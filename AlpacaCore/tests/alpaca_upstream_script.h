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

// A scripted upstream ObservingConditions, SafetyMonitor, Rotator, FilterWheel, Switch, Telescope or Focuser device for
// the remotealpaca driver tests (issue #1753 slices 3 to 5): the responder a FakeHttpClient answers with, so the
// driver's poll runs on the fake task clock with no socket. The loopback FakeAlpacaServer (fake_alpaca_server.h) covers
// the client over real HTTP; this one answers per member and per SensorName (or, for a Switch, per Id, and for a
// Telescope, per Axis), keeps the upstream's Connected and AveragePeriod (a Rotator's moves, Sync and Reverse, a
// FilterWheel's moves, a Switch's writes, and a Focuser's moves and TempComp) as a real server does, and counts calls
// per member. A Telescope keeps its slews, park, home, MoveAxis, pulses, syncs and settings as a real mount does. An
// unscripted member answers 0x400 (not implemented), as a real server does.

#include <alpacacore/util/http_client.h>

#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "fake_http_client.h"

namespace alpacacore::test {

class AlpacaUpstreamScript {
public:
    // Every sensor IObservingConditions defines, each with a value, a description and a TimeSinceLastUpdate.
    static constexpr const char* kSensorNames[] = {
        "CloudCover",     "DewPoint", "Humidity",    "Pressure",      "RainRate", "SkyBrightness", "SkyQuality",
        "SkyTemperature", "StarFWHM", "Temperature", "WindDirection", "WindGust", "WindSpeed"};

    enum class Kind { ObservingConditions, SafetyMonitor, Rotator, FilterWheel, Switch, Telescope, Focuser };

    // ObservingConditions: an ObservingConditions 2 upstream (Platform 7) named "Alpaca Simulator Weather" with every
    // sensor: sensor i reads 10 + i, its TimeSinceLastUpdate is 1.5 s, AveragePeriod 0.
    // SafetyMonitor: a SafetyMonitor 3 upstream (Platform 7) named "Alpaca Simulator Safety Monitor" reading safe.
    // Rotator: a Rotator 4 upstream (Platform 7) named "Alpaca Simulator Rotator" at Position 100 and
    // MechanicalPosition 90 (a Sync offset of 10), TargetPosition 100, not moving, StepSize 0.5, CanReverse true,
    // Reverse false. A move runs until finish_move() (or a Halt) ends it, so a test sees IsMoving true on the upstream
    // for as long as it needs.
    // FilterWheel: a FilterWheel 3 upstream (Platform 7) named "Alpaca Simulator Filter Wheel" with four slots, Red,
    // Green, Blue and Clear, offsets 100, 200, 300 and 0, at slot 0. A Position write starts a move that reads -1 until
    // finish_wheel_move() ends it at the written slot, so a test sees the upstream moving for as long as it needs.
    // Switch: a Switch 3 upstream (Platform 7) named "Alpaca Simulator Switch" with three switches: 0 "Power" (0 to 1,
    // on), 1 "Dew Heater" (0 to 100 in steps of 1, at 40, asynchronous) and 2 "Sensor" (0 to 10 in steps of 0.5, at
    // 2.5, read-only). An asynchronous write leaves StateChangeComplete false until finish_async() applies it.
    // Telescope: an ITelescopeV4 upstream (Platform 7) named "Alpaca Simulator Telescope", a German equatorial mount at
    // RA 6 h, Dec 30 deg, tracking, not parked, not home, pier east, every capability true (CanSlew and CanSlewAltAz
    // too: the driver answers its own for those), axes 0 and 1 movable at up to 4 deg/s, axis 2 not, its targets not
    // set (InvalidOperation). A slew, Park or FindHome lands at once unless hold_motion(true): then it runs until
    // finish_motion() or AbortSlew. A MoveAxis runs until MoveAxis(axis, 0) or AbortSlew; a pulse until
    // finish_pulse(). While parked a motion command, a sync and AbortSlew answer InvalidWhileParked (0x408).
    // Focuser: an absolute Focuser 4 upstream (Platform 7) named "Alpaca Simulator Focuser" at 25000 of MaxStep 50000,
    // not moving, StepSize 20, 5.0 degrees, TempComp available and off. A Move runs until finish_move() (or a Halt).
    explicit AlpacaUpstreamScript(Kind kind = Kind::ObservingConditions) : kind_(kind) {
        set("connecting", "false");
        if (kind == Kind::Telescope) {
            telescope_ = true;
            set("interfaceversion", "4");
            set("name", "\"Alpaca Simulator Telescope\"");
            set("description", "\"ASCOM Alpaca Simulator Telescope\"");
            for (const char* can :
                 {"canfindhome", "canpark", "canpulseguide", "cansetdeclinationrate", "cansetguiderates", "cansetpark",
                  "cansetpierside", "cansetrightascensionrate", "cansettracking", "canslew", "canslewaltaz",
                  "canslewaltazasync", "canslewasync", "cansync", "cansyncaltaz", "canunpark"}) {
                set(can, "true");
            }
            set_for("canmoveaxis", "0", "true");
            set_for("canmoveaxis", "1", "true");
            set_for("canmoveaxis", "2", "false");
            set_for("axisrates", "0", "[{\"Maximum\":4.0,\"Minimum\":0.0}]");
            set_for("axisrates", "1", "[{\"Maximum\":4.0,\"Minimum\":0.0}]");
            set_for("axisrates", "2", "[]");
            set("alignmentmode", "2");
            set("equatorialsystem", "1");
            set("aperturearea", "0.0269");
            set("aperturediameter", "0.2");
            set("focallength", "1.2");
            set("doesrefraction", "false");
            set("slewsettletime", "0");
            set("sitelatitude", "51.5");
            set("sitelongitude", "-0.1");
            set("siteelevation", "35");
            set("trackingrate", "0");
            set("trackingrates", "[0,1,2,3]");
            set("rightascensionrate", "0");
            set("declinationrate", "0");
            set("guideraterightascension", "0.004178");
            set("guideratedeclination", "0.004178");
            set_error("targetrightascension", 0x40B, "TargetRightAscension has not been set");
            set_error("targetdeclination", 0x40B, "TargetDeclination has not been set");
            set("altitude", "45");
            set("azimuth", "90");
            set("siderealtime", "12.5");
            set("utcdate", "\"2026-11-26T12:00:00.000Z\"");
            set("tracking", "true");
            set("ispulseguiding", "false");
            set("athome", "false");
            set("destinationsideofpier", "0");
            set_mount_locked(6.0, 30.0);
            set_motion_locked();
            return;
        }
        if (kind == Kind::Focuser) {
            set("interfaceversion", "4");
            set("name", "\"Alpaca Simulator Focuser\"");
            set("description", "\"ASCOM Alpaca Simulator Focuser\"");
            set("absolute", "true");
            set("maxstep", "50000");
            set("maxincrement", "50000");
            set("stepsize", "20");
            set("tempcompavailable", "true");
            set("tempcomp", "false");
            set("temperature", "5.0");
            set("position", "25000");
            set("ismoving", "false");
            return;
        }
        if (kind == Kind::FilterWheel) {
            set("interfaceversion", "3");
            set("name", "\"Alpaca Simulator Filter Wheel\"");
            set("description", "\"ASCOM Alpaca Simulator FilterWheel\"");
            set("names", R"(["Red", "Green", "Blue", "Clear"])");
            set("focusoffsets", "[100, 200, 300, 0]");
            set("position", "0");
            return;
        }
        if (kind == Kind::Rotator) {
            set("interfaceversion", "4");
            set("name", "\"Alpaca Simulator Rotator\"");
            set("description", "\"ASCOM Alpaca Simulator Rotator\"");
            set("canreverse", "true");
            set("stepsize", "0.5");
            set("reverse", "false");
            set("ismoving", "false");
            set_rotator_locked(100.0, 90.0);
            set("targetposition", number(100.0));
            return;
        }
        if (kind == Kind::Switch) {
            set("interfaceversion", "3");
            set("name", "\"Alpaca Simulator Switch\"");
            set("description", "\"ASCOM Alpaca Simulator Switch\"");
            set("maxswitch", "3");
            set_switch(0, "Power", 0.0, 1.0, 1.0, true, false, 1.0);
            set_switch(1, "Dew Heater", 0.0, 100.0, 1.0, true, true, 40.0);
            set_switch(2, "Sensor", 0.0, 10.0, 0.5, false, false, 2.5);
            return;
        }
        if (kind == Kind::SafetyMonitor) {
            set("interfaceversion", "3");
            set("name", "\"Alpaca Simulator Safety Monitor\"");
            set("description", "\"ASCOM Alpaca Simulator SafetyMonitor\"");
            set("issafe", "true");
            return;
        }
        set("interfaceversion", "2");
        set("name", "\"Alpaca Simulator Weather\"");
        set("description", "\"ASCOM Alpaca Simulator ObservingConditions\"");
        set("averageperiod", "0");
        for (std::size_t i = 0; i < std::size(kSensorNames); ++i) {
            set_sensor(kSensorNames[i], 10.0 + static_cast<double>(i));
        }
        set_for("timesincelastupdate", "", "1.5");
    }

    void install(FakeHttpClient& http) {
        http.set_responder([this](const FakeHttpClient::Request& request) { return answer(request); });
    }

    // `value_json` is the Value as JSON text: "21.5", "true", "\"text\"".
    void set(const std::string& member, std::string value_json) { set_for(member, "", std::move(value_json)); }

    // A member that takes SensorName (sensordescription, timesincelastupdate), for one sensor name ("" = none).
    void set_for(const std::string& member, const std::string& sensor_name, std::string value_json) {
        std::lock_guard<std::mutex> lock(mutex_);
        replies_[{member, lower(sensor_name)}] = {std::move(value_json), 0, {}};
    }

    void set_error(const std::string& member, int error_number, std::string message) {
        set_error_for(member, "", error_number, std::move(message));
    }

    void set_error_for(const std::string& member, const std::string& sensor_name, int error_number,
                       std::string message) {
        std::lock_guard<std::mutex> lock(mutex_);
        replies_[{member, lower(sensor_name)}] = {{}, error_number, std::move(message)};
    }

    // A PUT of `member` answers this error (its GET is unchanged).
    void set_put_error(const std::string& member, int error_number, std::string message) {
        std::lock_guard<std::mutex> lock(mutex_);
        put_errors_[member] = {{}, error_number, std::move(message)};
    }

    // The sensor's value, its description and its TimeSinceLastUpdate.
    void set_sensor(const std::string& name, double value) {
        set(lower(name), number(value));
        set_for("sensordescription", name, "\"" + name + " sensor\"");
        set_for("timesincelastupdate", name, "1.5");
    }

    // A Switch's switch `id`: its static members and its value (GetSwitch is true above the minimum).
    void set_switch(int id, const std::string& name, double min, double max, double step, bool can_write,
                    bool can_async, double value) {
        const std::string at = std::to_string(id);
        set_for("getswitchname", at, "\"" + name + "\"");
        set_for("getswitchdescription", at, "\"" + name + " switch\"");
        set_for("minswitchvalue", at, number(min));
        set_for("maxswitchvalue", at, number(max));
        set_for("switchstep", at, number(step));
        set_for("canwrite", at, can_write ? "true" : "false");
        set_for("canasync", at, can_async ? "true" : "false");
        set_for("statechangecomplete", at, "true");
        set_for("getswitchvalue", at, number(value));
        set_for("getswitch", at, value > min ? "true" : "false");
    }

    // A Switch: applies every asynchronous write still running and reads StateChangeComplete true again.
    void finish_async() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, value] : pending_async_) {
            apply_switch_value_locked(id, value);
            replies_[{"statechangecomplete", id}] = {"true", 0, {}};
        }
        pending_async_.clear();
    }

    // The upstream does not implement the sensor: every member for it answers 0x400.
    void remove_sensor(const std::string& name) {
        const std::string message = "Property " + name + " is not implemented";
        set_error(lower(name), 0x400, message);
        set_error_for("sensordescription", name, 0x400, message);
        set_error_for("timesincelastupdate", name, 0x400, message);
    }

    // Calls of `member` so far (GET or PUT).
    int calls(const std::string& member) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = calls_.find(member);
        return it == calls_.end() ? 0 : it->second;
    }

    bool upstream_connected() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connected_;
    }

    // Ends the running move at its target, IsMoving false. A Rotator: Position the target, MechanicalPosition the
    // target less the Sync offset. A Focuser: Position the target.
    void finish_move() {
        std::lock_guard<std::mutex> lock(mutex_);
        replies_[{"ismoving", ""}] = {"false", 0, {}};
        if (kind_ == Kind::Focuser) {
            if (!focuser_target_.empty()) replies_[{"position", ""}] = {focuser_target_, 0, {}};
            focuser_target_.clear();
            return;
        }
        if (rotator_target_) set_rotator_locked(*rotator_target_, *rotator_target_ - (position_ - mechanical_));
        rotator_target_.reset();
    }

    // A Rotator or a Focuser: the member and the Position of the last move PUT, as sent ("" before any).
    std::string last_move() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_move_;
    }

    // A FilterWheel: ends the running move at its slot (Position is the written slot, no longer -1).
    void finish_wheel_move() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!wheel_target_.empty()) replies_[{"position", ""}] = {wheel_target_, 0, {}};
        wheel_target_.clear();
    }

    // A FilterWheel: the Position of the last Position PUT, as sent ("" before any).
    std::string last_position_put() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_position_put_;
    }

    static std::string number(double value) { return std::to_string(value); }

    // A Telescope: slews, Park and FindHome run until finish_motion() (or AbortSlew) while true; they land at once
    // while false (the default).
    void hold_motion(bool hold) {
        std::lock_guard<std::mutex> lock(mutex_);
        hold_motion_ = hold;
    }

    // A Telescope: the slew, Park or FindHome running lands (RA and Dec at the slew's target, AtPark or AtHome).
    void finish_motion() {
        std::lock_guard<std::mutex> lock(mutex_);
        land_locked();
    }

    // A Telescope: the running pulse ends.
    void finish_pulse() { set("ispulseguiding", "false"); }

    // A Telescope: whether an axis runs a MoveAxis, as the upstream has it.
    bool axis_moving(int axis) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return axis >= 0 && axis < 3 && axis_rate_[axis] != 0.0;
    }

    // A Telescope: the PUT members in the order they came ("moveaxis <Axis> <Rate>" with the values as sent), a
    // scripted PUT error included.
    std::vector<std::string> puts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return puts_;
    }

    util::HttpResponse answer(const FakeHttpClient::Request& request) {
        const std::string& url = request.url;
        const std::size_t query_at = url.find('?');
        const std::string path = url.substr(0, query_at);
        const std::string member = path.substr(path.rfind('/') + 1);
        const std::string params = request.method == "PUT"
                                       ? request.form_body
                                       : (query_at == std::string::npos ? std::string() : url.substr(query_at + 1));
        const std::string transaction = field(params, "ClientTransactionID");

        Reply reply{{}, 0x400, "Method " + member + " is not implemented"};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++calls_[member];
            if (request.method == "PUT") {
                reply = put_locked(member, params);
            } else if (member == "connected") {
                reply = {connected_ ? "true" : "false", 0, {}};
            } else {
                const auto it = replies_.find({member, key_of(params)});
                if (it != replies_.end()) reply = it->second;
            }
        }
        std::string body = "{";
        if (reply.error_number == 0 && !reply.value_json.empty()) body += "\"Value\":" + reply.value_json + ",";
        body += "\"ClientTransactionID\":" + (transaction.empty() ? std::string("0") : transaction) +
                ",\"ServerTransactionID\":1,\"ErrorNumber\":" + std::to_string(reply.error_number) +
                ",\"ErrorMessage\":\"" + reply.message + "\"}";
        return util::HttpResponse{200, body};
    }

private:
    struct Reply {
        std::string value_json;  // empty with an error or a PUT
        int error_number = 0;
        std::string message;
    };

    Reply put_locked(const std::string& member, const std::string& params) {
        if (telescope_) {
            puts_.push_back(member == "moveaxis"
                                ? member + " " + field(params, "Axis") + " " + decode(field(params, "Rate"))
                                : member);
        }
        const auto scripted = put_errors_.find(member);
        if (scripted != put_errors_.end()) return scripted->second;
        if (telescope_) {
            if (auto reply = telescope_put_locked(member, params)) return *reply;
        }
        if (member == "connect") {
            connected_ = true;
        } else if (member == "disconnect") {
            connected_ = false;
        } else if (member == "connected") {
            connected_ = lower(field(params, "Connected")) == "true";
        } else if (member == "averageperiod") {
            replies_[{"averageperiod", ""}] = {decode(field(params, "AveragePeriod")), 0, {}};
        } else if (kind_ == Kind::Focuser && member == "move") {
            // The move runs until finish_move() or a Halt.
            focuser_target_ = decode(field(params, "Position"));
            last_move_ = member + " " + focuser_target_;
            replies_[{"ismoving", ""}] = {"true", 0, {}};
        } else if (kind_ == Kind::Focuser && member == "tempcomp") {
            replies_[{"tempcomp", ""}] = {lower(field(params, "TempComp")), 0, {}};
        } else if (member == "move" || member == "moveabsolute" || member == "movemechanical") {
            // The move runs until finish_move() or a Halt; TargetPosition is where it goes, as a real rotator has it.
            const std::string sent = decode(field(params, "Position"));
            last_move_ = member + " " + sent;
            const double value = std::stod(sent);
            const double target = member == "move"           ? position_ + value
                                  : member == "moveabsolute" ? value
                                                             : value + (position_ - mechanical_);
            rotator_target_ = wrap(target);
            replies_[{"targetposition", ""}] = {number(*rotator_target_), 0, {}};
            replies_[{"ismoving", ""}] = {"true", 0, {}};
        } else if (member == "halt") {
            rotator_target_.reset();
            focuser_target_.clear();
            replies_[{"ismoving", ""}] = {"false", 0, {}};
        } else if (member == "sync") {
            // The offset moves; MechanicalPosition stays.
            set_rotator_locked(wrap(std::stod(decode(field(params, "Position")))), mechanical_);
        } else if (member == "position") {
            // The wheel turns until finish_wheel_move(), reading -1 meanwhile (IFilterWheelV3).
            wheel_target_ = last_position_put_ = decode(field(params, "Position"));
            replies_[{"position", ""}] = {"-1", 0, {}};
        } else if (member == "reverse") {
            replies_[{"reverse", ""}] = {lower(field(params, "Reverse")), 0, {}};
        } else if (member == "setswitch" || member == "setswitchvalue" || member == "setasync" ||
                   member == "setasyncvalue") {
            return put_switch_locked(member, params);
        } else if (member == "setswitchname") {
            replies_[{"getswitchname", key_of(params)}] = {"\"" + decode(field(params, "Name")) + "\"", 0, {}};
        } else if (member != "refresh") {
            return {{}, 0x400, "Method " + member + " is not implemented"};
        }
        return {{}, 0, {}};
    }

    // The second key of a reply: the SensorName (lower case), or else a Switch's Id, or else a Telescope's Axis.
    static std::string key_of(const std::string& params) {
        const std::string sensor = lower(decode(field(params, "SensorName")));
        if (!sensor.empty()) return sensor;
        const std::string id = field(params, "Id");
        return id.empty() ? lower(decode(field(params, "Axis"))) : id;
    }

    double value_locked(const std::string& member, const std::string& id) const {
        const auto it = replies_.find({member, id});
        return it == replies_.end() || it->second.value_json.empty() ? 0.0 : std::stod(it->second.value_json);
    }

    void apply_switch_value_locked(const std::string& id, double value) {
        replies_[{"getswitchvalue", id}] = {number(value), 0, {}};
        replies_[{"getswitch", id}] = {value > value_locked("minswitchvalue", id) ? "true" : "false", 0, {}};
    }

    // A Switch write, as a real server takes it: a read-only switch refuses it, an asynchronous one runs until
    // finish_async().
    Reply put_switch_locked(const std::string& member, const std::string& params) {
        const std::string id = field(params, "Id");
        const auto can_write = replies_.find({"canwrite", id});
        if (can_write == replies_.end()) return {{}, 0x401, "Switch " + id + " does not exist"};
        if (can_write->second.value_json != "true") return {{}, 0x400, "Switch " + id + " cannot be written"};
        const bool by_state = member == "setswitch" || member == "setasync";
        const double value =
            by_state ? value_locked(lower(field(params, "State")) == "true" ? "maxswitchvalue" : "minswitchvalue", id)
                     : std::stod(decode(field(params, "Value")));
        if (member == "setasync" || member == "setasyncvalue") {
            if (replies_[{"canasync", id}].value_json != "true") {
                return {{}, 0x400, "Switch " + id + " cannot be set asynchronously"};
            }
            pending_async_[id] = value;
            replies_[{"statechangecomplete", id}] = {"false", 0, {}};
        } else {
            apply_switch_value_locked(id, value);
        }
        return {{}, 0, {}};
    }

    // The Telescope's PUTs; nullopt for a member a Telescope shares with the other kinds (connect, disconnect,
    // connected).
    std::optional<Reply> telescope_put_locked(const std::string& member, const std::string& params) {
        const auto value = [&](const char* key) { return decode(field(params, key)); };
        const auto number_of = [&](const char* key) { return std::stod(value(key)); };
        const bool parked = at_park_;
        const Reply refused_parked{{}, 0x408, "The mount is parked"};
        if (member == "connect" || member == "disconnect" || member == "connected") return std::nullopt;
        if (member == "abortslew") {
            if (parked) return refused_parked;
            motion_.reset();
            axis_rate_ = {0.0, 0.0, 0.0};
            set_motion_locked();
            return Reply{};
        }
        if (member == "moveaxis") {
            if (parked) return refused_parked;
            const int axis = std::stoi(value("Axis"));
            const auto can = replies_.find({"canmoveaxis", value("Axis")});
            if (axis < 0 || axis > 2 || can == replies_.end() || can->second.value_json != "true") {
                return Reply{{}, 0x401, "MoveAxis axis " + value("Axis") + " cannot move"};
            }
            const double rate = number_of("Rate");
            if (std::abs(rate) > 4.0) return Reply{{}, 0x401, "MoveAxis rate " + value("Rate") + " is too fast"};
            axis_rate_[axis] = rate;
            set_motion_locked();
            return Reply{};
        }
        if (member == "pulseguide") {
            if (parked) return refused_parked;
            replies_[{"ispulseguiding", ""}] = {"true", 0, {}};
            return Reply{};
        }
        if (member == "slewtocoordinatesasync" || member == "slewtotargetasync" || member == "slewtoaltazasync") {
            if (parked) return refused_parked;
            Motion motion;
            if (member == "slewtocoordinatesasync") {
                set_targets_locked(number_of("RightAscension"), number_of("Declination"));
            }
            if (member != "slewtoaltazasync") {
                if (!target_ra_ || !target_dec_) return Reply{{}, 0x40B, "The target is not set"};
                motion.ra = *target_ra_;
                motion.dec = *target_dec_;
            } else {
                motion.altitude = number_of("Altitude");
                motion.azimuth = number_of("Azimuth");
            }
            start_motion_locked(motion);
            return Reply{};
        }
        if (member == "park") {
            if (!parked) {
                Motion motion;
                motion.park = true;
                start_motion_locked(motion);
            }
            return Reply{};
        }
        if (member == "unpark") {
            at_park_ = false;
            set_motion_locked();
            return Reply{};
        }
        if (member == "findhome") {
            if (parked) return refused_parked;
            Motion motion;
            motion.home = true;
            start_motion_locked(motion);
            return Reply{};
        }
        if (member == "setpark") return Reply{};
        if (member == "synctocoordinates" || member == "synctotarget") {
            if (parked) return refused_parked;
            if (member == "synctocoordinates") {
                set_targets_locked(number_of("RightAscension"), number_of("Declination"));
            }
            if (!target_ra_ || !target_dec_) return Reply{{}, 0x40B, "The target is not set"};
            set_mount_locked(*target_ra_, *target_dec_);
            return Reply{};
        }
        if (member == "synctoaltaz") {
            if (parked) return refused_parked;
            replies_[{"altitude", ""}] = {value("Altitude"), 0, {}};
            replies_[{"azimuth", ""}] = {value("Azimuth"), 0, {}};
            return Reply{};
        }
        if (member == "targetrightascension") {
            target_ra_ = number_of("TargetRightAscension");
            replies_[{member, ""}] = {number(*target_ra_), 0, {}};
            return Reply{};
        }
        if (member == "targetdeclination") {
            target_dec_ = number_of("TargetDeclination");
            replies_[{member, ""}] = {number(*target_dec_), 0, {}};
            return Reply{};
        }
        if (member == "utcdate") {
            replies_[{member, ""}] = {"\"" + value("UTCDate") + "\"", 0, {}};
            return Reply{};
        }
        // A setting: the upstream keeps the value as sent (its parameter is the member's own ASCOM name).
        static const std::map<std::string, std::string> kSettings = {
            {"tracking", "Tracking"},
            {"trackingrate", "TrackingRate"},
            {"rightascensionrate", "RightAscensionRate"},
            {"declinationrate", "DeclinationRate"},
            {"guideraterightascension", "GuideRateRightAscension"},
            {"guideratedeclination", "GuideRateDeclination"},
            {"sideofpier", "SideOfPier"},
            {"sitelatitude", "SiteLatitude"},
            {"sitelongitude", "SiteLongitude"},
            {"siteelevation", "SiteElevation"},
            {"doesrefraction", "DoesRefraction"},
            {"slewsettletime", "SlewSettleTime"},
        };
        if (const auto it = kSettings.find(member); it != kSettings.end()) {
            replies_[{member, ""}] = {lower(value(it->second.c_str())), 0, {}};
            return Reply{};
        }
        return Reply{{}, 0x400, "Method " + member + " is not implemented"};
    }

    struct Motion {
        std::optional<double> ra;
        std::optional<double> dec;
        std::optional<double> altitude;
        std::optional<double> azimuth;
        bool park = false;
        bool home = false;
    };

    void start_motion_locked(const Motion& motion) {
        motion_ = motion;
        replies_[{"athome", ""}] = {"false", 0, {}};
        if (!hold_motion_) {
            land_locked();
            return;
        }
        set_motion_locked();
    }

    void land_locked() {
        if (motion_) {
            if (motion_->ra && motion_->dec) set_mount_locked(*motion_->ra, *motion_->dec);
            if (motion_->altitude) replies_[{"altitude", ""}] = {number(*motion_->altitude), 0, {}};
            if (motion_->azimuth) replies_[{"azimuth", ""}] = {number(*motion_->azimuth), 0, {}};
            if (motion_->park) at_park_ = true;
            if (motion_->home) replies_[{"athome", ""}] = {"true", 0, {}};
        }
        motion_.reset();
        set_motion_locked();
    }

    // Slewing, AtPark and SideOfPier from the running motion, the axes and the park.
    void set_motion_locked() {
        const bool moving = motion_.has_value() || axis_rate_[0] != 0.0 || axis_rate_[1] != 0.0 || axis_rate_[2] != 0.0;
        replies_[{"slewing", ""}] = {moving ? "true" : "false", 0, {}};
        replies_[{"atpark", ""}] = {at_park_ ? "true" : "false", 0, {}};
        if (replies_.find({"sideofpier", ""}) == replies_.end()) replies_[{"sideofpier", ""}] = {"0", 0, {}};
    }

    void set_mount_locked(double ra, double dec) {
        replies_[{"rightascension", ""}] = {number(ra), 0, {}};
        replies_[{"declination", ""}] = {number(dec), 0, {}};
    }

    void set_targets_locked(double ra, double dec) {
        target_ra_ = ra;
        target_dec_ = dec;
        replies_[{"targetrightascension", ""}] = {number(ra), 0, {}};
        replies_[{"targetdeclination", ""}] = {number(dec), 0, {}};
    }
    void set_rotator_locked(double position, double mechanical) {
        position_ = position;
        mechanical_ = mechanical;
        replies_[{"position", ""}] = {number(position), 0, {}};
        replies_[{"mechanicalposition", ""}] = {number(mechanical), 0, {}};
    }

    static double wrap(double degrees) {
        const double wrapped = std::fmod(degrees, 360.0);
        return wrapped < 0.0 ? wrapped + 360.0 : wrapped;
    }

    static std::string lower(std::string text) {
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    // The value of `key` in an a=1&b=2 string, or "".
    static std::string field(const std::string& encoded, const std::string& key) {
        std::size_t at = 0;
        while (at <= encoded.size()) {
            const std::size_t end = encoded.find('&', at);
            const std::string pair = encoded.substr(at, end == std::string::npos ? std::string::npos : end - at);
            if (pair.rfind(key + "=", 0) == 0) return pair.substr(key.size() + 1);
            if (end == std::string::npos) break;
            at = end + 1;
        }
        return "";
    }

    static std::string decode(const std::string& text) {
        std::string out;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '%' && i + 2 < text.size()) {
                out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
                i += 2;
            } else {
                out += text[i] == '+' ? ' ' : text[i];
            }
        }
        return out;
    }

    mutable std::mutex mutex_;
    std::map<std::pair<std::string, std::string>, Reply> replies_;
    std::map<std::string, Reply> put_errors_;
    std::map<std::string, int> calls_;
    bool connected_ = false;
    // A Rotator's angles, its running move's target (none while none runs) and its last move PUT.
    double position_ = 0.0;
    double mechanical_ = 0.0;
    std::optional<double> rotator_target_;
    std::string last_move_;
    std::map<std::string, double> pending_async_;  // a Switch's running asynchronous writes, by Id
    std::string wheel_target_;  // a FilterWheel's running move; empty while none runs
    std::string last_position_put_;
    // A Telescope's motion, axes, park, targets and the PUTs it took.
    bool telescope_ = false;
    bool hold_motion_ = false;
    bool at_park_ = false;
    std::optional<Motion> motion_;
    std::array<double, 3> axis_rate_{0.0, 0.0, 0.0};
    std::optional<double> target_ra_;
    std::optional<double> target_dec_;
    std::vector<std::string> puts_;
    const Kind kind_;
    std::string focuser_target_;  // a Focuser's running move; empty while none runs
};

}  // namespace alpacacore::test
