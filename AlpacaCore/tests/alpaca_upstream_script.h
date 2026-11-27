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

// A scripted upstream ObservingConditions, SafetyMonitor, Rotator, FilterWheel or Switch device for the remotealpaca
// driver tests (issue #1753 slices 3 to 5): the responder a FakeHttpClient answers with, so the driver's poll runs on
// the fake task clock with no socket. The loopback FakeAlpacaServer (fake_alpaca_server.h) covers the client over real
// HTTP; this one answers per member and per SensorName (or, for a Switch, per Id), keeps the upstream's Connected and
// AveragePeriod (a Rotator's moves, Sync and Reverse, a FilterWheel's moves and a Switch's writes) as a real server
// does, and counts calls per member. An unscripted member answers 0x400 (not implemented), as a real server does.

#include <alpacacore/util/http_client.h>

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

    enum class Kind { ObservingConditions, SafetyMonitor, Rotator, FilterWheel, Switch };

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
    explicit AlpacaUpstreamScript(Kind kind = Kind::ObservingConditions) {
        set("connecting", "false");
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

    // A Rotator: ends the running move at its target (Position the target, MechanicalPosition the target less the Sync
    // offset, IsMoving false).
    void finish_move() {
        std::lock_guard<std::mutex> lock(mutex_);
        replies_[{"ismoving", ""}] = {"false", 0, {}};
        if (rotator_target_) set_rotator_locked(*rotator_target_, *rotator_target_ - (position_ - mechanical_));
        rotator_target_.reset();
    }

    // A Rotator: the member and the Position of the last move PUT, as sent ("" before any).
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
        const auto scripted = put_errors_.find(member);
        if (scripted != put_errors_.end()) return scripted->second;
        if (member == "connect") {
            connected_ = true;
        } else if (member == "disconnect") {
            connected_ = false;
        } else if (member == "connected") {
            connected_ = lower(field(params, "Connected")) == "true";
        } else if (member == "averageperiod") {
            replies_[{"averageperiod", ""}] = {decode(field(params, "AveragePeriod")), 0, {}};
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

    // The second key of a reply: the SensorName (lower case), or else a Switch's Id.
    static std::string key_of(const std::string& params) {
        const std::string sensor = lower(decode(field(params, "SensorName")));
        return sensor.empty() ? field(params, "Id") : sensor;
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
};

}  // namespace alpacacore::test
