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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/gphoto/gphoto_sdk_wrapper.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace alpacacore::test {

/**
 * Drop this process's entries from the driver's on-disk sensor-geometry cache
 * (gphoto_camera_driver.cpp's kSensorCacheRelativePath, CWD-relative).
 *
 * Every fake-SDK connect that reaches the priming capture writes this file.
 * Left in place it makes the *next* run of these tests take the cache-hit
 * branch of set_connected(), which publishes Connected without ever calling
 * prime_sensor_geometry_and_cache -- so the priming/decode coverage these
 * tests exist for would evaporate silently on run 2 in a persistent build
 * directory. Call this first in any gphoto test case that connects.
 *
 * The file is real driver state (one TSV line per model), so deleting it
 * wholesale from a test binary run in a deployment working directory would
 * silently discard that rig's primed geometry for every model (review of
 * #546). Every fake model this suite connects carries the unique_test_model()
 * "[pid N]" suffix, so only lines keyed by THIS process's suffix are removed;
 * everything else in the file, including sibling test processes' entries, is
 * rewritten as read.
 */
inline void reset_gphoto_sensor_cache() {
    const std::filesystem::path path("config/gphoto_sensor_cache.tsv");
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return;
    }
    const std::string suffix = " [pid " + std::to_string(::getpid()) + "]";
    std::vector<std::string> kept;
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream iss(line);
            std::string model;
            std::getline(iss, model, '\t');
            const bool mine = model.size() >= suffix.size() &&
                              model.compare(model.size() - suffix.size(), suffix.size(), suffix) == 0;
            if (!mine) {
                kept.push_back(line);
            }
        }
    }
    std::ofstream out(path, std::ios::trunc);
    for (const auto& line : kept) {
        out << line << '\n';
    }
}

/**
 * Suffix a fake camera model so it is unique to this process.
 *
 * The sensor cache is keyed by model string and lives in one CWD-relative
 * file, but catch_discover_tests() gives every TEST_CASE its own process and
 * run_all_tests.sh drives ctest with -j. Without this, a sibling test case
 * priming the same model concurrently could satisfy this one's cache lookup
 * between its reset_gphoto_sensor_cache() and its connect, silently skipping
 * the priming path it means to exercise. (AlpacaHTTP solves the same
 * CWD-sharing problem with per-test WORKING_DIRECTORY; the Catch2 tests here
 * are all one binary, so key separation is the equivalent.)
 */
inline std::string unique_test_model(const std::string& base) {
    return base + " [pid " + std::to_string(::getpid()) + "]";
}

/**
 * Scripted fake for GPhotoSDK (issue #489 fault-injection seam).
 *
 * KNOWN PARITY GAPS (AGENTS.md: a fake must be the harsher of the two; keep
 * this list exhaustive and prefer closing a gap to documenting it):
 *  - `capture_result` defaults to an EMPTY `data` vector, where a real
 *    capture returns 20+ MB of RAW. Harmless only because FakeRawDecoder
 *    ignores the bytes; a test that exercises a real decoder path must fill
 *    `data` itself.
 *  - `has_widget` is inferred from map presence (choices / toggle_value /
 *    text_value), not from a widget tree, so a widget that exists with an
 *    EMPTY choice list cannot be modelled: it reads as absent.
 *  - `close_camera` is the one method `throw_from` cannot target. Deliberate:
 *    it mirrors the real close_session_locked, which never throws, so a
 *    throwing close would test a path the driver can never see.
 *  - `set_choice_value` / `set_toggle_value` on an UNMODELLED widget insert a
 *    new map entry and succeed, where GPhotoSDKWrapper throws
 *    PropertyNotImplemented. Unreachable today (every production write is
 *    gated by has_widget or has_bulb_), but lenient in the direction a fake
 *    must not be; a test that scripts an absent widget must not rely on the
 *    write failing.
 *  - `drain_events` sleeps out its WHOLE budget where the real wrapper
 *    returns as each event lands and polls again; the fake is the slower of
 *    the two, so the hold loop's timing is exercised at its harshest.
 *  - `poll_bulb_file_and_download` answers instantly, where the real wrapper
 *    blocks up to its timeout for the file-added event. A "not ready" poll
 *    (`bulb_file_polls_before_ready`) sleeps at most 10 ms of the timeout so
 *    a multi-poll case stays fast; a test must not read the wall-clock of
 *    that wait as the real one.
 *
 * Callers push one or more FakeCamera entries into `cameras` before
 * connecting (index == the Alpaca "cameraIndex"). Each fake camera carries
 * its own widget state (choices/current value, toggles, text) so tests can
 * script the widget-probing fallback behavior configure_after_connect_locked
 * relies on (e.g. "shutterspeed2" absent but "shutterspeed" present).
 *
 * Failure injection: add a method name to `throw_from` and every call to
 * that method throws AlpacaException regardless of arguments.
 */
class FakeGPhotoSDK : public vendor::gphoto::GPhotoSDK {
public:
    struct FakeCamera {
        std::string model;
        std::string port;
        // widget name -> ordered choice list
        std::unordered_map<std::string, std::vector<std::string>> choices;
        // widget name -> current choice value
        std::unordered_map<std::string, std::string> choice_value;
        // widget name -> toggle state
        std::unordered_map<std::string, bool> toggle_value;
        // widget name -> text value
        std::unordered_map<std::string, std::string> text_value;
    };

    std::vector<FakeCamera> cameras;
    std::set<std::string> throw_from;
    // Persistent, not a one-shot: runs before every call until it is cleared (after the call is counted, before
    // throw_from), so a case can make one named call block. Null in every ordinary test; the contract sweep
    // sets it on the connect call to hold a connect open.
    // Ownership (as fake_qhy_sdk.h): set before the driver exists, never changed while a connect is in flight.
    std::function<void(const std::string&)> before_call;
    std::vector<std::string> call_log;

    int open_count{0};
    int close_count{0};
    std::string gphoto_version{"2.5.31"};

    // Canned result returned by capture_and_download / poll_bulb_file_and_download.
    vendor::gphoto::GPhotoCaptureResult capture_result;

    // Records every set_toggle_value(handle, "bulb", on) call, in order, so
    // bulb-sequence tests can assert true-then-false-then-download ordering.
    std::vector<bool> bulb_toggle_history;

    // drain_events() bookkeeping: how many hold slices the driver pumped and
    // their summed budget, so a test can pin that the bulb hold is spent
    // polling the camera's events rather than sleeping (issue #569).
    int drain_events_calls{0};
    std::chrono::milliseconds drained_budget{0};

    // poll_bulb_file_and_download() bookkeeping. `bulb_file_polls_before_ready`
    // is how many polls answer "no file yet" (std::nullopt) before
    // capture_result is delivered; 0 delivers on the first poll. Every poll
    // is counted and its timeout recorded.
    int bulb_file_polls_before_ready{0};
    int bulb_file_polls{0};
    std::vector<std::chrono::milliseconds> bulb_file_poll_timeouts;

    std::vector<vendor::gphoto::GPhotoCameraInfo> enumerate_cameras() override {
        log_and_maybe_throw("enumerate_cameras");
        std::vector<vendor::gphoto::GPhotoCameraInfo> out;
        for (const auto& cam : cameras) {
            out.push_back({cam.model, cam.port});
        }
        return out;
    }

    int open_camera(const std::string& model, const std::string& port) override {
        log_and_maybe_throw("open_camera");
        for (std::size_t i = 0; i < cameras.size(); ++i) {
            if (cameras[i].model == model && cameras[i].port == port) {
                int handle = static_cast<int>(i);
                ++open_count;
                open_balance_[handle] = true;
                return handle;
            }
        }
        throw AlpacaException("FakeGPhotoSDK: no matching camera for " + model + "/" + port, AlpacaError::NotConnected);
    }

    void close_camera(int handle) override {
        // Never throws (matches the contract: safe on an unknown handle).
        call_log.push_back("close_camera");
        auto it = open_balance_.find(handle);
        if (it != open_balance_.end() && it->second) {
            it->second = false;
            ++close_count;
        }
    }

    std::string get_gphoto_version() override {
        log_and_maybe_throw("get_gphoto_version");
        return gphoto_version;
    }

    std::string get_camera_summary(int handle) override {
        log_and_maybe_throw("get_camera_summary");
        return "Fake camera summary for " + camera_for(handle).model;
    }

    bool has_widget(int handle, const std::string& name) override {
        log_and_maybe_throw("has_widget");
        const auto& cam = camera_for(handle);
        return cam.choices.count(name) != 0 || cam.toggle_value.count(name) != 0 || cam.text_value.count(name) != 0;
    }

    std::vector<std::string> get_choices(int handle, const std::string& name) override {
        log_and_maybe_throw("get_choices");
        const auto& cam = camera_for(handle);
        auto it = cam.choices.find(name);
        return it != cam.choices.end() ? it->second : std::vector<std::string>{};
    }

    std::string get_choice_value(int handle, const std::string& name) override {
        log_and_maybe_throw("get_choice_value");
        auto& cam = camera_for(handle);
        auto it = cam.choice_value.find(name);
        if (it == cam.choice_value.end()) {
            throw AlpacaException("Widget not present: " + name, AlpacaError::PropertyNotImplemented);
        }
        return it->second;
    }

    void set_choice_value(int handle, const std::string& name, const std::string& value) override {
        log_and_maybe_throw("set_choice_value");
        auto& cam = camera_for(handle);
        cam.choice_value[name] = value;
    }

    bool get_toggle_value(int handle, const std::string& name) override {
        log_and_maybe_throw("get_toggle_value");
        auto& cam = camera_for(handle);
        auto it = cam.toggle_value.find(name);
        if (it == cam.toggle_value.end()) {
            throw AlpacaException("Widget not present: " + name, AlpacaError::PropertyNotImplemented);
        }
        return it->second;
    }

    void set_toggle_value(int handle, const std::string& name, bool on) override {
        log_and_maybe_throw("set_toggle_value");
        auto& cam = camera_for(handle);
        cam.toggle_value[name] = on;
        if (name == "bulb") {
            bulb_toggle_history.push_back(on);
        }
    }

    std::string get_text_value(int handle, const std::string& name) override {
        log_and_maybe_throw("get_text_value");
        auto& cam = camera_for(handle);
        auto it = cam.text_value.find(name);
        if (it == cam.text_value.end()) {
            throw AlpacaException("Widget not present: " + name, AlpacaError::PropertyNotImplemented);
        }
        return it->second;
    }

    vendor::gphoto::GPhotoCaptureResult capture_and_download(int handle) override {
        log_and_maybe_throw("capture_and_download");
        camera_for(handle);  // validates handle
        return capture_result;
    }

    void drain_events(int handle, std::chrono::milliseconds budget) override {
        log_and_maybe_throw("drain_events");
        camera_for(handle);
        ++drain_events_calls;
        drained_budget += budget;
        std::this_thread::sleep_for(budget);
    }

    std::optional<vendor::gphoto::GPhotoCaptureResult> poll_bulb_file_and_download(
        int handle, std::chrono::milliseconds timeout) override {
        log_and_maybe_throw("poll_bulb_file_and_download");
        camera_for(handle);
        ++bulb_file_polls;
        bulb_file_poll_timeouts.push_back(timeout);
        if (bulb_file_polls_before_ready > 0) {
            --bulb_file_polls_before_ready;
            std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds(10)));
            return std::nullopt;
        }
        return capture_result;
    }

private:
    std::unordered_map<int, bool> open_balance_;

    FakeCamera& camera_for(int handle) {
        call_log.push_back("camera_for");
        if (handle < 0 || static_cast<std::size_t>(handle) >= cameras.size()) {
            throw AlpacaException("FakeGPhotoSDK: unknown handle", AlpacaError::NotConnected);
        }
        return cameras[static_cast<std::size_t>(handle)];
    }

    void log_and_maybe_throw(const std::string& name) {
        call_log.push_back(name);
        if (before_call) before_call(name);
        if (throw_from.count(name) != 0) {
            throw AlpacaException("FakeGPhotoSDK: injected failure in " + name, AlpacaError::DriverException);
        }
    }
};

}  // namespace alpacacore::test
