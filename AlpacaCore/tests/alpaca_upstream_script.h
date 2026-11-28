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

// A scripted upstream ObservingConditions, SafetyMonitor, Rotator, FilterWheel, Switch, Telescope, Focuser,
// CoverCalibrator, Dome or Camera device for the remotealpaca driver tests (issue #1753 slices 3 to 5): the responder a
// FakeHttpClient answers with, so the driver's poll runs on the fake task clock with no socket. The loopback
// FakeAlpacaServer (fake_alpaca_server.h) covers the client over real HTTP; this one answers per member and per
// SensorName (or, for a Switch, per Id, and for a Telescope, per Axis), keeps the upstream's Connected and
// AveragePeriod (a Rotator's moves, Sync and Reverse, a FilterWheel's moves, a Switch's writes, a Focuser's moves and
// TempComp, a CoverCalibrator's cover and calibrator, a Dome's motions, shutter, Sync and Slaved, and a Camera's
// exposure, settings and image) as a real server does, and counts calls per member. A Telescope keeps its slews, park,
// home, MoveAxis, pulses, syncs and settings as a real mount does. An unscripted member answers 0x400 (not
// implemented), as a real server does.

#include <alpacacore/util/http_client.h>

#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
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

    enum class Kind {
        ObservingConditions,
        SafetyMonitor,
        Rotator,
        FilterWheel,
        Switch,
        Telescope,
        Focuser,
        CoverCalibrator,
        Dome,
        Camera
    };

    // The Camera's sensor: 8 x 6 pixels, so an image is small and its x-major wire order shows.
    static constexpr int kCameraWidth = 8;
    static constexpr int kCameraHeight = 6;

    // The value of the Camera's image at (x, y) of the frame read out: unique per pixel, and not symmetric in x and
    // y, so a transposed decode reads wrong.
    static int pixel(int x, int y) { return 100 * x + y + 1; }

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
    // Camera: a Camera 4 upstream (Platform 7) named "Alpaca Simulator Camera": an 8 x 6 monochrome sensor
    // ("IMX571"), MaxBin 2 with no asymmetric binning (a BinX write sets BinY too), ExposureMin 0.001 s, ExposureMax
    // 3600 s, Gain 10 in GainMin 0 to GainMax 100 (no Gains list), Offset 5 in 0 to 50, readout modes Normal and
    // Fast, a cooler (CCDTemperature 20, CoolerPower 0, SetCCDTemperature -10, CoolerOn false), no HeatSinkTemperature,
    // no FastReadout and no SubExposureDuration. ElectronsPerADU is 3 less a hundredth of the gain. An exposure runs
    // until finish_exposure() (or StopExposure) ends it with an image, or AbortExposure ends it without one; the image
    // is NumX x NumY of pixel(x, y), served as ImageBytes (UInt16 on the wire) to a request that accepts them, else as
    // JSON. A setter or a StartExposure while exposing is InvalidOperation, a frame off the sensor InvalidValue.
    // Dome: a Dome 3 upstream (Platform 7) named "Alpaca Simulator Dome" with every capability, at Azimuth 100 and
    // Altitude 0, not slewing, not at home or park, its shutter Closed, not slaved; home is Azimuth 0 and park 180. A
    // slew, FindHome or Park runs until finish_move() (AbortSlew stops it and ends Slaved), and the shutter moves until
    // finish_shutter(), so a test sees Slewing true or ShutterStatus Opening on the upstream for as long as it needs.
    // CoverCalibrator: a CoverCalibrator 2 upstream (Platform 7) named "Alpaca Simulator Cover Calibrator", its cover
    // Closed and its calibrator Off at Brightness 0 of MaxBrightness 100. OpenCover and CloseCover run until
    // finish_cover() (or a HaltCover, which leaves the cover Unknown), CalibratorOn and CalibratorOff until
    // finish_calibrator(), so a test sees CoverMoving and CalibratorChanging true on the upstream for as long as it
    // needs.
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
        if (kind == Kind::Camera) {
            set_camera_defaults();
            return;
        }
        if (kind == Kind::Dome) {
            set("interfaceversion", "3");
            set("name", "\"Alpaca Simulator Dome\"");
            set("description", "\"ASCOM Alpaca Simulator Dome\"");
            for (const char* can : {"canfindhome", "canpark", "cansetaltitude", "cansetazimuth", "cansetpark",
                                    "cansetshutter", "canslave", "cansyncazimuth"}) {
                set(can, "true");
            }
            set("slewing", "false");
            set("azimuth", number(100.0));
            set("altitude", number(0.0));
            set("athome", "false");
            set("atpark", "false");
            set("shutterstatus", "1");
            set("slaved", "false");
            return;
        }
        if (kind == Kind::CoverCalibrator) {
            set("interfaceversion", "2");
            set("name", "\"Alpaca Simulator Cover Calibrator\"");
            set("description", "\"ASCOM Alpaca Simulator CoverCalibrator\"");
            set("coverstate", "1");  // Closed
            set("covermoving", "false");
            set("calibratorstate", "1");  // Off
            set("calibratorchanging", "false");
            set("brightness", "0");
            set("maxbrightness", "100");
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

    // Called before each call is answered, on the calling thread with no script lock held: a test advances the fake
    // task clock in it to give every upstream call a duration (a per-call delay, never a real sleep). Empty clears it.
    void set_call_hook(std::function<void()> hook) {
        std::lock_guard<std::mutex> lock(mutex_);
        call_hook_ = std::move(hook);
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
    // target less the Sync offset. A Focuser: Position the target. A Dome: ends the running slew, FindHome or Park
    // there (Slewing false; AtHome or AtPark true for those two).
    void finish_move() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (dome_motion_) {
            const DomeMotion motion = *dome_motion_;
            dome_motion_.reset();
            replies_[{"slewing", ""}] = {"false", 0, {}};
            if (motion.member == "slewtoaltitude") {
                replies_[{"altitude", ""}] = {number(motion.target), 0, {}};
            } else {
                replies_[{"azimuth", ""}] = {number(motion.target), 0, {}};
            }
            if (motion.member == "findhome") replies_[{"athome", ""}] = {"true", 0, {}};
            if (motion.member == "park") replies_[{"atpark", ""}] = {"true", 0, {}};
            return;
        }
        replies_[{"ismoving", ""}] = {"false", 0, {}};
        if (kind_ == Kind::Focuser) {
            if (!focuser_target_.empty()) replies_[{"position", ""}] = {focuser_target_, 0, {}};
            focuser_target_.clear();
            return;
        }
        if (rotator_target_) set_rotator_locked(*rotator_target_, *rotator_target_ - (position_ - mechanical_));
        rotator_target_.reset();
    }

    // A CoverCalibrator: ends the running OpenCover or CloseCover (CoverState Open or Closed, CoverMoving false).
    void finish_cover() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cover_target_) replies_[{"coverstate", ""}] = {std::to_string(*cover_target_), 0, {}};
        replies_[{"covermoving", ""}] = {"false", 0, {}};
        cover_target_.reset();
    }

    // A CoverCalibrator: ends the running CalibratorOn or CalibratorOff (CalibratorState Ready or Off,
    // CalibratorChanging false).
    void finish_calibrator() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (calibrator_target_) replies_[{"calibratorstate", ""}] = {std::to_string(*calibrator_target_), 0, {}};
        replies_[{"calibratorchanging", ""}] = {"false", 0, {}};
        calibrator_target_.reset();
    }

    // A Dome: ends the shutter's transition (Opening to Open, Closing to Closed).
    void finish_shutter() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& status = replies_[{"shutterstatus", ""}];
        if (status.value_json == "2") status = {"0", 0, {}};
        if (status.value_json == "3") status = {"1", 0, {}};
    }

    // A Rotator or a Focuser: the member and the Position of the last move PUT, as sent ("" before any). A Dome: the
    // member of the last slew, FindHome or Park, and the angle a slew sent.
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

    // A Camera: ends the running exposure with an image (CameraState Idle, ImageReady true, LastExposure* set).
    void finish_exposure() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (exposing_) finish_exposure_locked();
    }

    // A Camera: answer imagearray as JSON even to a request that accepts ImageBytes (an upstream without them).
    void set_image_json(bool json) {
        std::lock_guard<std::mutex> lock(mutex_);
        image_json_ = json;
    }

    // A Camera: the Duration and Light of the last StartExposure, as sent ("" before any).
    std::string last_start() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_start_;
    }

    static std::string number(double value) { return std::to_string(value); }

    // A Telescope: while true, each RightAscension read moves RA 0.01 h on while axis 0 runs a MoveAxis, and each
    // Declination read moves Dec 0.1 deg on while axis 1 does (false, the default: the position stays).
    void move_positions(bool move) {
        std::lock_guard<std::mutex> lock(mutex_);
        move_positions_ = move;
    }

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
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hook = call_hook_;
        }
        if (hook) hook();

        Reply reply{{}, 0x400, "Method " + member + " is not implemented"};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++calls_[member];
            if (request.method == "GET" && member == "imagearray" && camera_) {
                return image_locked(request.accept == "application/imagebytes" && !image_json_, transaction);
            }
            if (request.method == "PUT") {
                reply = put_locked(member, params);
            } else if (member == "connected") {
                reply = {connected_ ? "true" : "false", 0, {}};
            } else {
                if (telescope_ && move_positions_ && request.method == "GET") {
                    if (member == "rightascension" && axis_rate_[0] != 0.0)
                        set_mount_locked(mount_ra_ + 0.01, mount_dec_);
                    if (member == "declination" && axis_rate_[1] != 0.0) set_mount_locked(mount_ra_, mount_dec_ + 0.1);
                }
                const auto it = replies_.find({member, key_of(params)});
                if (it != replies_.end()) reply = it->second;
            }
        }
        std::string body = "{";
        if (reply.error_number == 0 && !reply.value_json.empty()) body += "\"Value\":" + reply.value_json + ",";
        body += "\"ClientTransactionID\":" + (transaction.empty() ? std::string("0") : transaction) +
                ",\"ServerTransactionID\":1,\"ErrorNumber\":" + std::to_string(reply.error_number) +
                ",\"ErrorMessage\":\"" + reply.message + "\"}";
        return util::HttpResponse{200, body, {}};
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
        if (camera_ && member != "connect" && member != "disconnect" && member != "connected") {
            return camera_put_locked(member, params);
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
        } else if (member == "slewtoazimuth" || member == "slewtoaltitude" || member == "findhome" ||
                   member == "park") {
            // The motion runs until finish_move() or an AbortSlew; it leaves home and park at once, as a real dome.
            const std::string sent = decode(field(params, member == "slewtoaltitude" ? "Altitude" : "Azimuth"));
            last_move_ = sent.empty() ? member : member + " " + sent;
            const double target = member == "findhome" ? 0.0 : member == "park" ? dome_park_ : std::stod(sent);
            dome_motion_ = DomeMotion{member, target};
            replies_[{"slewing", ""}] = {"true", 0, {}};
            replies_[{"athome", ""}] = {"false", 0, {}};
            replies_[{"atpark", ""}] = {"false", 0, {}};
        } else if (member == "abortslew") {
            dome_motion_.reset();
            replies_[{"slewing", ""}] = {"false", 0, {}};
            replies_[{"slaved", ""}] = {"false", 0, {}};
        } else if (member == "openshutter") {
            replies_[{"shutterstatus", ""}] = {"2", 0, {}};
        } else if (member == "closeshutter") {
            replies_[{"shutterstatus", ""}] = {"3", 0, {}};
        } else if (member == "setpark") {
            const auto it = replies_.find({"azimuth", ""});
            if (it != replies_.end() && !it->second.value_json.empty()) dome_park_ = std::stod(it->second.value_json);
        } else if (member == "synctoazimuth") {
            replies_[{"azimuth", ""}] = {number(wrap(std::stod(decode(field(params, "Azimuth"))))), 0, {}};
        } else if (member == "slaved") {
            replies_[{"slaved", ""}] = {lower(field(params, "Slaved")), 0, {}};
        } else if (member == "opencover" || member == "closecover") {
            cover_target_ = member == "opencover" ? 3 : 1;  // Open, Closed
            replies_[{"coverstate", ""}] = {"2", 0, {}};    // Moving
            replies_[{"covermoving", ""}] = {"true", 0, {}};
        } else if (member == "haltcover") {
            cover_target_.reset();
            replies_[{"coverstate", ""}] = {"4", 0, {}};  // Unknown: stopped between the two
            replies_[{"covermoving", ""}] = {"false", 0, {}};
        } else if (member == "calibratoron" || member == "calibratoroff") {
            const bool on = member == "calibratoron";
            calibrator_target_ = on ? 3 : 1;                   // Ready, Off
            replies_[{"calibratorstate", ""}] = {"2", 0, {}};  // NotReady
            replies_[{"calibratorchanging", ""}] = {"true", 0, {}};
            replies_[{"brightness", ""}] = {on ? decode(field(params, "Brightness")) : "0", 0, {}};
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
        mount_ra_ = ra;
        mount_dec_ = dec;
        replies_[{"rightascension", ""}] = {number(ra), 0, {}};
        replies_[{"declination", ""}] = {number(dec), 0, {}};
    }

    void set_targets_locked(double ra, double dec) {
        target_ra_ = ra;
        target_dec_ = dec;
        replies_[{"targetrightascension", ""}] = {number(ra), 0, {}};
        replies_[{"targetdeclination", ""}] = {number(dec), 0, {}};
    }
    // --- The Camera ---

    // In the constructor: set() takes the mutex.
    void set_camera_defaults() {
        camera_ = true;
        set("interfaceversion", "4");
        set("name", "\"Alpaca Simulator Camera\"");
        set("description", "\"ASCOM Alpaca Simulator Camera\"");
        set("cameraxsize", std::to_string(kCameraWidth));
        set("cameraysize", std::to_string(kCameraHeight));
        set_error("bayeroffsetx", 0x400, "BayerOffsetX is not implemented: a monochrome sensor");
        set_error("bayeroffsety", 0x400, "BayerOffsetY is not implemented: a monochrome sensor");
        set("canabortexposure", "true");
        set("canasymmetricbin", "false");
        set("canfastreadout", "false");
        set("cangetcoolerpower", "true");
        set("canpulseguide", "true");
        set("cansetccdtemperature", "true");
        set("canstopexposure", "true");
        set("electronsperadu", number(2.9));
        set("fullwellcapacity", number(50000.0));
        set("exposuremin", "0.001");
        set("exposuremax", "3600");
        set("exposureresolution", "0.001");
        set("gainmin", "0");
        set("gainmax", "100");
        set_error("gains", 0x400, "Gains is not implemented: the gain is a value from GainMin to GainMax");
        set("hasshutter", "false");
        set("maxadu", "65535");
        set("maxbinx", "2");
        set("maxbiny", "2");
        set("offsetmin", "0");
        set("offsetmax", "50");
        set_error("offsets", 0x400, "Offsets is not implemented: the offset is a value from OffsetMin to OffsetMax");
        set("pixelsizex", "3.76");
        set("pixelsizey", "3.76");
        set("readoutmodes", "[\"Normal\",\"Fast\"]");
        set("sensorname", "\"IMX571\"");
        set("sensortype", "0");
        set("binx", "1");
        set("biny", "1");
        set("numx", std::to_string(kCameraWidth));
        set("numy", std::to_string(kCameraHeight));
        set("startx", "0");
        set("starty", "0");
        set("gain", "10");
        set("offset", "5");
        set("readoutmode", "0");
        set_error("fastreadout", 0x400, "FastReadout is not implemented: CanFastReadout is false");
        set("cooleron", "false");
        set("setccdtemperature", "-10");
        set_error("subexposureduration", 0x400, "SubExposureDuration is not implemented");
        set("ccdtemperature", "20");
        set("coolerpower", "0");
        set_error("heatsinktemperature", 0x400, "HeatSinkTemperature is not implemented");
        set("ispulseguiding", "false");
        set("camerastate", "0");
        set("imageready", "false");
        set_error("percentcompleted", 0x40B, "PercentCompleted is only valid while exposing");
        set_error("lastexposureduration", 0x40B, "No image has been taken");
        set_error("lastexposurestarttime", 0x40B, "No image has been taken");
    }

    // A held integer member's value (the setter replies keep them as JSON text).
    int int_locked(const std::string& member) const {
        const auto it = replies_.find({member, ""});
        return it == replies_.end() || it->second.value_json.empty() ? 0 : std::stoi(it->second.value_json);
    }

    Reply camera_put_locked(const std::string& member, const std::string& params) {
        static const char* const kSettings[] = {
            "binx", "biny",   "numx",        "numy",     "startx",           "starty",
            "gain", "offset", "readoutmode", "cooleron", "setccdtemperature"};
        if (member == "startexposure") {
            if (exposing_) return {{}, 0x40B, "An exposure is already running"};
            const std::string duration = decode(field(params, "Duration"));
            if (std::stod(duration) > 3600.0) return {{}, 0x401, "Duration is above ExposureMax"};
            const int bin_x = int_locked("binx");
            const int bin_y = int_locked("biny");
            const int width = kCameraWidth / bin_x;
            const int height = kCameraHeight / bin_y;
            if (int_locked("startx") + int_locked("numx") > width ||
                int_locked("starty") + int_locked("numy") > height) {
                return {{}, 0x401, "The frame is off the sensor"};
            }
            exposing_ = true;
            exposure_duration_ = duration;
            last_start_ = duration + " " + lower(field(params, "Light"));
            image_.clear();
            replies_[{"camerastate", ""}] = {"2", 0, {}};
            replies_[{"imageready", ""}] = {"false", 0, {}};
            replies_[{"percentcompleted", ""}] = {"0", 0, {}};
            return {{}, 0, {}};
        }
        if (member == "abortexposure") {
            end_exposure_locked();
            return {{}, 0, {}};
        }
        if (member == "stopexposure") {
            if (exposing_) finish_exposure_locked();
            return {{}, 0, {}};
        }
        if (member == "pulseguide") {
            replies_[{"ispulseguiding", ""}] = {"true", 0, {}};
            return {{}, 0, {}};
        }
        for (const char* setting : kSettings) {
            if (member != setting) continue;
            if (exposing_) return {{}, 0x40B, "The camera is exposing"};
            const std::size_t eq = params.find('=');
            std::string value = decode(params.substr(eq + 1, params.find('&') - eq - 1));
            if (member == "cooleron") value = lower(value);
            if ((member == "binx" || member == "biny") && std::stoi(value) > 2) {
                return {{}, 0x401, "Bin " + value + " is above MaxBin"};
            }
            replies_[{member, ""}] = {value, 0, {}};
            // No asymmetric binning: one bin sets both.
            if (member == "binx") replies_[{"biny", ""}] = {value, 0, {}};
            if (member == "biny") replies_[{"binx", ""}] = {value, 0, {}};
            if (member == "gain") replies_[{"electronsperadu", ""}] = {number(3.0 - std::stoi(value) * 0.01), 0, {}};
            return {{}, 0, {}};
        }
        return {{}, 0x400, "Method " + member + " is not implemented"};
    }

    void end_exposure_locked() {
        exposing_ = false;
        replies_[{"camerastate", ""}] = {"0", 0, {}};
        replies_[{"percentcompleted", ""}] = {{}, 0x40B, "PercentCompleted is only valid while exposing"};
    }

    void finish_exposure_locked() {
        end_exposure_locked();
        replies_[{"imageready", ""}] = {"true", 0, {}};
        replies_[{"lastexposureduration", ""}] = {exposure_duration_, 0, {}};
        replies_[{"lastexposurestarttime", ""}] = {"\"2026-11-26T12:00:00.5\"", 0, {}};
        image_width_ = int_locked("numx");
        image_height_ = int_locked("numy");
        image_.assign(static_cast<std::size_t>(image_width_ * image_height_), 0);
        for (int x = 0; x < image_width_; ++x) {
            for (int y = 0; y < image_height_; ++y)
                image_[static_cast<std::size_t>(x * image_height_ + y)] = pixel(x, y);
        }
    }

    static void put_le32(std::string& out, std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) out += static_cast<char>((value >> shift) & 0xFF);
    }

    // imagearray: the image held (x-major), or InvalidOperation before one; ImageBytes or JSON.
    util::HttpResponse image_locked(bool image_bytes, const std::string& transaction) const {
        const std::string message = "No image is ready";
        if (image_bytes) {
            std::string body;
            const std::uint32_t error = image_.empty() ? 0x40B : 0;
            put_le32(body, 1);
            put_le32(body, error);
            put_le32(body, static_cast<std::uint32_t>(transaction.empty() ? 0 : std::stoul(transaction)));
            put_le32(body, 1);
            put_le32(body, 44);
            put_le32(body, 2);  // ImageElementType Int32
            put_le32(body, 8);  // TransmissionElementType UInt16
            put_le32(body, 2);
            put_le32(body, static_cast<std::uint32_t>(error ? 0 : image_width_));
            put_le32(body, static_cast<std::uint32_t>(error ? 0 : image_height_));
            put_le32(body, 0);
            if (error) {
                body += message;
            } else {
                for (const int value : image_) {
                    body += static_cast<char>(value & 0xFF);
                    body += static_cast<char>((value >> 8) & 0xFF);
                }
            }
            return util::HttpResponse{200, body, "application/imagebytes"};
        }
        std::string body = "{";
        if (!image_.empty()) {
            body += "\"Type\":2,\"Rank\":2,\"Value\":[";
            for (int x = 0; x < image_width_; ++x) {
                body += x == 0 ? "[" : ",[";
                for (int y = 0; y < image_height_; ++y) {
                    body +=
                        (y == 0 ? "" : ",") + std::to_string(image_[static_cast<std::size_t>(x * image_height_ + y)]);
                }
                body += "]";
            }
            body += "],";
        }
        body += "\"ClientTransactionID\":" + (transaction.empty() ? std::string("0") : transaction) +
                ",\"ServerTransactionID\":1,\"ErrorNumber\":" + std::to_string(image_.empty() ? 0x40B : 0) +
                ",\"ErrorMessage\":\"" + (image_.empty() ? message : std::string()) + "\"}";
        return util::HttpResponse{200, body, "application/json"};
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
    std::function<void()> call_hook_;  // set_call_hook()
    bool connected_ = false;
    // A Rotator's angles, its running move's target (none while none runs) and its last move PUT.
    double position_ = 0.0;
    double mechanical_ = 0.0;
    std::optional<double> rotator_target_;
    std::string last_move_;
    std::map<std::string, double> pending_async_;  // a Switch's running asynchronous writes, by Id
    std::string wheel_target_;                     // a FilterWheel's running move; empty while none runs
    std::string last_position_put_;
    // A Telescope's motion, axes, park, targets and the PUTs it took.
    bool telescope_ = false;
    bool hold_motion_ = false;
    bool move_positions_ = false;
    double mount_ra_ = 0.0;
    double mount_dec_ = 0.0;
    bool at_park_ = false;
    std::optional<Motion> motion_;
    std::array<double, 3> axis_rate_{0.0, 0.0, 0.0};
    std::optional<double> target_ra_;
    std::optional<double> target_dec_;
    std::vector<std::string> puts_;
    const Kind kind_;
    std::string focuser_target_;  // a Focuser's running move; empty while none runs
    // A Camera's exposure, its last StartExposure and its image (x-major, NumX x NumY when it finished).
    bool camera_ = false;
    bool exposing_ = false;
    bool image_json_ = false;
    std::string exposure_duration_;
    std::string last_start_;
    std::vector<int> image_;
    int image_width_ = 0;
    int image_height_ = 0;
    // A Dome's running motion (none while none runs) and its park azimuth.
    struct DomeMotion {
        std::string member;
        double target = 0.0;
    };
    std::optional<DomeMotion> dome_motion_;
    double dome_park_ = 180.0;
    // A CoverCalibrator's running cover and calibrator commands: the state each ends in (none while none runs).
    std::optional<int> cover_target_;
    std::optional<int> calibrator_target_;
};

}  // namespace alpacacore::test
