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

#include <alpacacore/async_connectable.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/last_exposure.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/gphoto/gphoto_camera_driver.h>
#include <alpacacore/vendor/gphoto/gphoto_sdk_wrapper.h>
#include <alpacacore/version.h>
#include <libraw/libraw.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace alpacacore::vendor::gphoto {

namespace {

constexpr const char* kLogTag = "GPhoto";

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

// Parses a libgphoto2 shutter-speed choice string ("1/200", "30", "bulb", or
// the raw PTP fraction form "65535/65535") into seconds. Returns nullopt for
// the bulb sentinel -- callers treat that as "no fixed duration".
std::optional<double> parse_shutter_speed_seconds(const std::string& choice) {
    std::string lower = to_lower(choice);
    if (lower == "bulb" || lower == "65535/65535") {
        return std::nullopt;
    }
    auto slash = choice.find('/');
    try {
        if (slash != std::string::npos) {
            double numerator = std::stod(choice.substr(0, slash));
            double denominator = std::stod(choice.substr(slash + 1));
            if (denominator == 0.0) return std::nullopt;
            return numerator / denominator;
        }
        return std::stod(choice);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// Picks the RAW image-quality choice from a camera's "imageformat"/
// "imagequality" widget: prefers a pure-RAW choice (e.g. "NEF (Raw)") over a
// combined RAW+JPEG one (e.g. "NEF+JPEG Fine"), so only one file per capture
// needs downloading and decoding.
std::optional<std::string> pick_raw_format_choice(const std::vector<std::string>& choices) {
    std::optional<std::string> raw_plus_other;
    for (const auto& choice : choices) {
        std::string lower = to_lower(choice);
        bool has_raw = lower.find("raw") != std::string::npos || lower.find("nef") != std::string::npos ||
                       lower.find("cr2") != std::string::npos || lower.find("cr3") != std::string::npos ||
                       lower.find("arw") != std::string::npos;
        if (!has_raw) continue;
        bool combined = lower.find('+') != std::string::npos || lower.find("jpeg") != std::string::npos ||
                        lower.find("jpg") != std::string::npos;
        if (!combined) {
            return choice;
        }
        if (!raw_plus_other) {
            raw_plus_other = choice;
        }
    }
    return raw_plus_other;
}

// Sensor geometry (width/height/Bayer phase/max ADU) is not knowable from
// libgphoto2 metadata -- it only comes from decoding a real captured RAW
// frame with libraw (see .github/instructions/gphoto.instructions.md). That is identical for
// every camera of the same model, so once it has been learned for a given
// model on this rig it is cached to disk (keyed by the model string, not
// the individual device) and never needs a priming capture again -- not on
// the next Connect, not after a reboot, and not for a second camera of the
// same model. AlpacaCore does not do JSON (see "Core Architecture" in AGENTS.md), so this is a
// deliberately tiny tab-separated flat file rather than pulling in a JSON
// dependency for five integers.
constexpr const char* kSensorCacheRelativePath = "config/gphoto_sensor_cache.tsv";

struct CachedSensorGeometry {
    int width{};
    int height{};
    int bayer_offset_x{};
    int bayer_offset_y{};
    int max_adu{};
};

std::optional<CachedSensorGeometry> load_cached_sensor_geometry(const std::string& model) {
    std::ifstream in(kSensorCacheRelativePath);
    if (!in) return std::nullopt;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        std::string entry_model;
        if (!std::getline(iss, entry_model, '\t') || entry_model != model) {
            continue;
        }
        CachedSensorGeometry g;
        if (iss >> g.width >> g.height >> g.bayer_offset_x >> g.bayer_offset_y >> g.max_adu) {
            return g;
        }
    }
    return std::nullopt;
}

void store_cached_sensor_geometry(const std::string& model, const CachedSensorGeometry& g) {
    if (model.find('\t') != std::string::npos || model.find('\n') != std::string::npos) {
        return;  // Defensive: a model string can't corrupt the flat-file format.
    }
    std::filesystem::path path(kSensorCacheRelativePath);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    // Rewrite the file with every other model's line preserved and this
    // model's entry appended fresh. The file holds one line per distinct
    // camera model this rig has ever primed -- trivially small.
    std::vector<std::string> other_lines;
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream iss(line);
            std::string entry_model;
            if (std::getline(iss, entry_model, '\t') && entry_model != model) {
                other_lines.push_back(line);
            }
        }
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        ALPACA_LOG_WARN(kLogTag, "Unable to persist gphoto sensor geometry cache to " + path.string());
        return;
    }
    for (const auto& line : other_lines) {
        out << line << '\n';
    }
    out << model << '\t' << g.width << '\t' << g.height << '\t' << g.bayer_offset_x << '\t' << g.bayer_offset_y << '\t'
        << g.max_adu << '\n';
}

// Physical pixel pitch (microns) for interchangeable-lens Nikon/Canon bodies
// libgphoto2 recognizes. Unlike sensor width/height/Bayer phase (learned
// from a decoded RAW frame, see prime_sensor_geometry_and_cache above),
// pixel pitch cannot be derived at runtime -- LibRaw exposes crop geometry,
// not photosite spacing, and libgphoto2's own metadata has no field for it
// either. This table is sourced from each model's published sensor
// width/resolution spec, not from anything the camera itself reports, so
// PixelSizeX/Y stay 0.0 (ASCOM "unknown") for any model not listed here --
// every fixed-lens compact/camcorder libgphoto2 also supports, plus any
// interchangeable-lens body released after this table was last updated.
// Canon's Rebel/Kiss/EOS-number triplets are the same physical sensor sold
// under different regional names, so they appear as separate entries here
// with identical values -- libgphoto2 reports whichever name matches the
// camera's actual USB product ID, which varies by region for some bodies.
const std::unordered_map<std::string, double>& known_pixel_size_um_table() {
    static const std::unordered_map<std::string, double> table = {
        {"Canon Digital Rebel XT", 6.4},
        {"Canon EOS 1000D", 5.7},
        {"Canon EOS 100D", 4.3},
        {"Canon EOS 10D", 7.4},
        {"Canon EOS 1100D", 5.2},
        {"Canon EOS 1200D", 4.3},
        {"Canon EOS 1300D", 4.3},
        {"Canon EOS 1500D", 3.72},
        {"Canon EOS 1D C", 6.95},
        {"Canon EOS 1D Mark II", 8.2},
        {"Canon EOS 1D Mark III", 7.2},
        {"Canon EOS 1D Mark IV", 5.7},
        {"Canon EOS 1D X", 6.9},
        {"Canon EOS 1D X MarkII", 6.56},
        {"Canon EOS 1D X MarkIII", 6.56},
        {"Canon EOS 2000D", 3.72},
        {"Canon EOS 200D", 3.72},
        {"Canon EOS 20D", 6.4},
        {"Canon EOS 250D", 3.72},
        {"Canon EOS 300D", 7.4},
        {"Canon EOS 30D", 6.4},
        {"Canon EOS 350D", 6.4},
        {"Canon EOS 4000D", 4.3},
        {"Canon EOS 400D", 5.7},
        {"Canon EOS 40D", 5.7},
        {"Canon EOS 450D", 5.2},
        {"Canon EOS 500D", 4.7},
        {"Canon EOS 50D", 4.7},
        {"Canon EOS 550D", 4.3},
        {"Canon EOS 5D", 8.2},
        {"Canon EOS 5D Mark II", 6.4},
        {"Canon EOS 5D Mark III", 6.25},
        {"Canon EOS 5D Mark IV", 5.36},
        {"Canon EOS 5DS", 4.14},
        {"Canon EOS 5DS R", 4.14},
        {"Canon EOS 600D", 4.3},
        {"Canon EOS 60D", 4.3},
        {"Canon EOS 650D", 4.3},
        {"Canon EOS 6D", 6.55},
        {"Canon EOS 6d Mark II", 5.75},
        {"Canon EOS 700D", 4.3},
        {"Canon EOS 70D", 4.1},
        {"Canon EOS 750D", 3.72},
        {"Canon EOS 760D", 3.72},
        {"Canon EOS 77D", 3.72},
        {"Canon EOS 7D", 4.3},
        {"Canon EOS 7D MarkII", 4.1},
        {"Canon EOS 800D", 3.72},
        {"Canon EOS 80D", 3.72},
        {"Canon EOS 850D", 3.72},
        {"Canon EOS 90D", 3.2},
        {"Canon EOS D30", 10.5},
        {"Canon EOS D60", 7.4},
        {"Canon EOS Digital Rebel", 7.4},
        {"Canon EOS Digital Rebel XTi", 5.7},
        {"Canon EOS Kiss Digital", 7.4},
        {"Canon EOS Kiss Digital N", 6.4},
        {"Canon EOS Kiss Digital X", 5.7},
        {"Canon EOS Kiss X2", 5.2},
        {"Canon EOS Kiss X3", 4.7},
        {"Canon EOS M", 4.3},
        {"Canon EOS M10", 4.3},
        {"Canon EOS M100", 3.72},
        {"Canon EOS M2", 4.3},
        {"Canon EOS M200", 3.72},
        {"Canon EOS M3", 3.72},
        {"Canon EOS M5", 3.72},
        {"Canon EOS M50", 3.72},
        {"Canon EOS M50m2", 3.72},
        {"Canon EOS M6", 3.72},
        {"Canon EOS M6 Mark II", 3.2},
        {"Canon EOS R", 5.34},
        {"Canon EOS R10", 3.72},
        {"Canon EOS R5", 4.39},
        {"Canon EOS R5 C", 4.39},
        {"Canon EOS R6", 6.56},
        {"Canon EOS R6m2", 5.98},
        {"Canon EOS R7", 3.2},
        {"Canon EOS RP", 5.75},
        {"Canon EOS Rebel T1i", 4.7},
        {"Canon EOS Rebel T6", 4.3},
        {"Canon EOS Rebel T7i", 3.72},
        {"Canon EOS Rebel T8i", 3.72},
        {"Canon EOS Rebel XSi", 5.2},
        {"Canon Rebel T2i", 4.3},
        {"Canon Rebel T3", 5.2},
        {"Canon Rebel T3i", 4.3},
        {"Canon Rebel T4i", 4.3},
        {"Nikon D100", 7.8},
        {"Nikon D2H SLR", 9.6},
        {"Nikon D2Hs", 9.6},
        {"Nikon D2X SLR", 5.5},
        {"Nikon D3", 8.45},
        {"Nikon D50", 7.8},
        {"Nikon DSC D100", 7.8},
        {"Nikon DSC D200", 6.1},
        {"Nikon DSC D2Xs", 5.5},
        {"Nikon DSC D300", 5.5},
        {"Nikon DSC D3000", 6.1},
        {"Nikon DSC D300s", 5.5},
        {"Nikon DSC D3100", 5.0},
        {"Nikon DSC D3200", 3.86},
        {"Nikon DSC D3300", 3.92},
        {"Nikon DSC D3400", 3.92},
        {"Nikon DSC D3500", 3.92},
        {"Nikon DSC D3s", 8.45},
        {"Nikon DSC D3x", 5.94},
        {"Nikon DSC D4", 7.3},
        {"Nikon DSC D40", 7.8},
        {"Nikon DSC D40x", 6.1},
        {"Nikon DSC D4s", 7.3},
        {"Nikon DSC D5", 6.45},
        {"Nikon DSC D500", 4.2},
        {"Nikon DSC D5000", 5.5},
        {"Nikon DSC D5100", 4.78},
        {"Nikon DSC D5200", 3.92},
        {"Nikon DSC D5300", 3.91},
        {"Nikon DSC D5500", 3.92},
        {"Nikon DSC D5600", 3.92},
        {"Nikon DSC D6", 6.45},
        {"Nikon DSC D60", 6.1},
        {"Nikon DSC D600", 5.97},
        {"Nikon DSC D610", 5.97},
        {"Nikon DSC D70", 7.8},
        {"Nikon DSC D700", 8.45},
        {"Nikon DSC D7000", 4.78},
        {"Nikon DSC D70s", 7.8},
        {"Nikon DSC D7100", 3.92},
        {"Nikon DSC D7200", 3.92},
        {"Nikon DSC D750", 5.97},
        {"Nikon DSC D7500", 4.2},
        {"Nikon DSC D780", 5.94},
        {"Nikon DSC D80", 6.1},
        {"Nikon DSC D800", 4.88},
        {"Nikon DSC D800E", 4.88},
        {"Nikon DSC D810", 4.88},
        {"Nikon DSC D810A", 4.88},
        {"Nikon DSC D850", 4.35},
        {"Nikon DSC D90", 5.5},
        {"Nikon DSC Df", 7.3},
        {"Nikon J1", 3.4},
        {"Nikon J2", 3.4},
        {"Nikon J3", 2.87},
        {"Nikon J4", 2.52},
        {"Nikon J5", 2.37},
        {"Nikon S1", 3.4},
        {"Nikon S2", 2.87},
        {"Nikon V1", 3.4},
        {"Nikon V2", 2.87},
        {"Nikon V3", 2.52},
        {"Nikon Z30", 4.2},
        {"Nikon Z5", 5.97},
        {"Nikon Z50", 4.2},
        {"Nikon Z6", 5.94},
        {"Nikon Z6_2", 5.94},
        {"Nikon Z7", 4.35},
        {"Nikon Z7_2", 4.35},
        {"Nikon Z8", 4.35},
        {"Nikon Z9", 4.35},
        {"Nikon Zfc", 4.2},
    };
    return table;
}

// A handful of pre-PTP-era Nikon bodies (e.g. the D100) are reachable over
// two different protocols and libgphoto2 names them differently depending
// which one matched -- strip any such suffix so both spellings resolve to
// the same table entry above (whose keys are already suffix-free).
std::string strip_connection_mode_suffix(const std::string& model) {
    static const std::vector<std::string> kSuffixes = {" (PTP mode)",    " (PTP Mode)",    " (PTP)",
                                                       " (normal mode)", " (Normal mode)", " (Sierra Mode)"};
    for (const auto& suffix : kSuffixes) {
        if (model.size() > suffix.size() && model.compare(model.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return model.substr(0, model.size() - suffix.size());
        }
    }
    return model;
}

double lookup_known_pixel_size_um(const std::string& model) {
    const auto& table = known_pixel_size_um_table();
    auto it = table.find(model);
    if (it != table.end()) return it->second;
    it = table.find(strip_connection_mode_suffix(model));
    return it != table.end() ? it->second : 0.0;
}

}  // namespace

class GPhotoCameraDriver : public CameraDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    GPhotoCameraDriver(int device_number, int camera_index, GPhotoSDK& sdk, RawDecoder& decoder,
                       util::TaskClock& clock = util::default_task_clock(), const GPhotoOptions& options = {})
        : AsyncConnectable("GPhoto"),
          device_number_(device_number),
          camera_index_(camera_index),
          sdk_(sdk),
          decoder_(decoder),
          clock_(clock),
          options_(options) {
        preload_camera_info_locked();
    }

    ~GPhotoCameraDriver() override {
        shutdown_connection();
        stop_exposure_thread();
        if (connected_.load()) {
            try {
                set_connected(false);  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(kLogTag, "Error during destruction: " + std::string(e.what()));
            }
        }
    }

    // --- AlpacaDriver ---

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override {
        const_cast<GPhotoCameraDriver*>(this)->refresh_cached_camera_info_if_needed();
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.model : "DSLR / Mirrorless Camera";
    }

    DeviceType get_device_type() const override { return DeviceType::Camera; }

    std::string fallback_unique_id() const override {
        // Review PR #485 round 4: camera_info_.port (the libgphoto2 USB bus
        // path, e.g. "usb:001,005") is not a stable identity -- it changes
        // across an unplug/replug (the bus device number increments) and
        // even across a power-off/power-on within the same session, since a
        // DSLR that was off at Connect-preload time and powered on later
        // re-enumerates with a new port. Unlike the other camera vendors
        // here, libgphoto2/PTP exposes no serial number to key on instead,
        // so device_number_ -- the stable, config-assigned Alpaca device
        // slot -- is the only identifier that doesn't change under the
        // camera's own power state.
        return "GPHOTO_" + std::to_string(device_number_);
    }

    std::string get_description() const override { return "DSLR / Mirrorless Camera Driver"; }
    std::string get_driver_info() const override { return "AlpacaCore GPhoto Camera Driver"; }
    std::string get_driver_version() const override { return alpacacore::kVersion; }

    std::optional<std::string> get_device_sdk_version() const override {
        auto version = sdk_.get_gphoto_version();
        if (version.empty()) return std::nullopt;
        return "libgphoto2 " + version;
    }

    int get_interface_version() const override { return 4; }  // ICameraV4 (Platform 7)

    bool get_connected() const override { return connected_.load(); }
    void connect() override { start_connection_task(true); }
    void disconnect() override { start_connection_task(false); }
    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        std::unique_lock<std::mutex> lifecycle_lock(exposure_lifecycle_mutex_, std::defer_lock);
        if (!connected) {
            lifecycle_lock.lock();
            stop_exposure_thread();
        }
        std::unique_lock<std::mutex> lock(mutex_);
        if (!connected && !connected_.load() && connecting_priming_) {
            // Disconnect requested mid-priming: a sync set_connected(true) on
            // another thread has released mutex_ for the priming capture --
            // conn_task_ (the async task's own state) cannot see this, since
            // set_connected() is also a public sync entry point, and the
            // project's own concurrency stress harness calls it directly.
            // connected_ is still false, so the idempotency check below would
            // silently drop this disconnect (both sides look "disconnected").
            // Record the intent instead; the in-flight connect consumes it
            // once priming finishes and closes the handle rather than
            // publishing connected_.
            record_pending_disconnect();
            return;
        }
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) {
            return;
        }
        if (connected && consume_pending_disconnect(connected_.load())) {
            return;
        }
        if (connected == connected_.load()) {
            return;  // Idempotent (ASCOM)
        }

        auto& sdk = sdk_;

        if (connected) {
            if (connecting_priming_) {
                // Another set_connected(true) is already mid-priming and has
                // released mutex_ for the capture. Every gate above passed
                // (connected_ is still false), so without this we would
                // double-open the camera and leak the first handle when only
                // one close ever fires. The in-flight call publishes
                // handle_/connected_ when it finishes; this call is a no-op.
                return;
            }
            auto cameras = sdk.enumerate_cameras();
            if (camera_index_ < 0 || camera_index_ >= static_cast<int>(cameras.size())) {
                throw AlpacaException(
                    "No DSLR / mirrorless camera found at this camera index (is it plugged in and powered on?)",
                    AlpacaError::NotConnected);
            }
            const auto& info = cameras[static_cast<std::size_t>(camera_index_)];
            int opened_handle = sdk.open_camera(info.model, info.port);
            try {
                configure_after_connect_locked(sdk, opened_handle);
            } catch (const AlpacaException&) {
                sdk.close_camera(opened_handle);
                throw;
            } catch (const std::exception& e) {
                sdk.close_camera(opened_handle);
                throw AlpacaException(std::string("Failed to configure DSLR / mirrorless camera: ") + e.what(),
                                      AlpacaError::DriverException);
            }
            handle_ = opened_handle;
            camera_info_ = info;
            camera_info_valid_ = true;
            pixel_size_um_ = lookup_known_pixel_size_um(info.model);
            reset_exposure_state_locked();

            // Review PR #485: connected_ is published only once geometry is
            // fully resolved (cache hit or priming capture), never before --
            // AsyncConnectable's record_disconnect_if_connect_in_flight() /
            // consume_pending_disconnect() contract (see async_connectable.h)
            // assumes connected_==true means this connect task's real work is
            // done. Publishing it early let a racing set_connected(false) (or
            // start_exposure(), via ensure_connected()) run concurrently with
            // the still-unlocked priming capture on the very same libgphoto2
            // handle -- a genuine use-after-close / concurrent-capture window,
            // not just a theoretical one.
            //
            // Round 2: that fix alone covers only the ASYNC connect()/
            // disconnect() entry points, which is what conn_task_ tracks --
            // set_connected() is also a public sync entry point in its own
            // right (the project's own [stress] harness calls it directly),
            // and conn_task_ stays kConnIdle for a sync call. connecting_priming_
            // (below) closes that second window the same way ToupTek AFW's
            // connecting_homing_ does for its mutex-released homing poll: a
            // racing sync disconnect records itself via record_pending_disconnect()
            // instead of being dropped as falsely idempotent, a racing sync
            // connect no-ops instead of double-opening, and this call consumes
            // the pending flag after re-locking post-priming, closing the
            // handle instead of publishing connected_ if one was recorded.

            // Sensor geometry is the same for every camera of this model
            // (see the cache helpers above) -- a rig that has already primed
            // this exact model, ever, skips straight to it with no capture
            // and no added latency.
            if (auto cached = load_cached_sensor_geometry(info.model)) {
                set_geometry_locked(cached->width, cached->height, cached->bayer_offset_x, cached->bayer_offset_y,
                                    cached->max_adu);
                connected_.store(true);
                return;
            }

            // The priming capture is a gp_camera_capture, which a body with mirror lock-up on
            // answers only after a 60 s timeout. Skip it: geometry is then learned from the first
            // real exposure, exactly as when priming fails. A failed MLU read skips too, since
            // priming is best-effort and the read says nothing about whether a capture would stall.
            bool skip_priming = true;
            try {
                skip_priming = mirror_lockup_on_locked(opened_handle);
            } catch (const std::exception& e) {
                ALPACA_LOG_DEBUG(kLogTag, "Mirror lock-up state unreadable at connect, skipping the priming capture: " +
                                              std::string(e.what()));
            }
            if (skip_priming) {
                ALPACA_LOG_DEBUG(kLogTag,
                                 "Skipping the sensor geometry priming capture (mirror lock-up on or unknown)");
                connected_.store(true);
                return;
            }

            // First time this model has ever connected on this rig: prime it
            // now, off mutex_ (it takes a real capture+download), so ASCOM
            // geometry properties are valid the moment Connected becomes
            // true instead of only after the caller's own first exposure.
            // Best-effort -- see prime_sensor_geometry_and_cache.
            int priming_handle = opened_handle;
            std::string priming_model = info.model;
            connecting_priming_ = true;
            lock.unlock();
            try {
                prime_sensor_geometry_and_cache(priming_handle, priming_model);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(kLogTag,
                                "Sensor geometry priming capture failed (will learn on first real "
                                "exposure instead): " +
                                    std::string(e.what()));
            }
            lock.lock();
            connecting_priming_ = false;
            if (consume_pending_disconnect(connected_.load())) {
                // A sync set_connected(false) landed during the mutex-released
                // priming capture and recorded itself above instead of being
                // dropped. Honor it: close the handle we just opened and stay
                // disconnected rather than publishing connected_. Requested,
                // not a failure -- return cleanly.
                const int abandoned_handle = handle_;
                handle_ = -1;
                camera_info_valid_ = false;
                reset_exposure_state_locked();
                if (abandoned_handle >= 0) {
                    sdk.close_camera(abandoned_handle);
                }
                return;
            }
            connected_.store(true);
            return;
        }

        // Disconnecting: publish disconnected before closing the SDK handle
        // so a racing operational call fails fast at its connection check
        // instead of hitting a just-closed camera (driver-lifecycle.instructions.md disconnect
        // rule). Exposure worker is already joined (lifecycle lock above).
        const int close_handle = handle_;
        handle_ = -1;
        reset_exposure_state_locked();
        connected_.store(false);
        if (close_handle >= 0) {
            sdk.close_camera(close_handle);
        }
    }

    std::vector<std::string> get_supported_actions() const override { return {}; }

    std::string action(std::string_view action_name, std::string_view) override {
        throw AlpacaException("Action not supported: " + std::string(action_name), AlpacaError::ActionNotImplemented);
    }

    bool can_action(std::string_view) const override { return false; }

    std::string command_blind(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }

    bool command_bool(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }

    std::string command_string(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }

    // --- CameraDriver: sensor geometry ---
    //
    // Unlike SDK-enumerated CMOS cameras, libgphoto2 does not report sensor
    // dimensions, pixel pitch, or Bayer phase up front -- those are only
    // knowable once a RAW frame has actually been decoded. set_connected
    // resolves this at Connect time: from the on-disk per-model cache if
    // this exact camera model has connected here before, otherwise via a
    // one-time throwaway priming capture (see prime_sensor_geometry_and_cache
    // and the cache helpers near the top of this file). Every property below
    // that depends on geometry throws InvalidOperation ("not yet known") only
    // in the rare case that both of those failed (e.g. priming capture error)
    // -- it then falls back to the caller's own first real exposure, same as
    // other RAW-decoding ASCOM DSLR drivers (e.g. ASCOM.DSLR). See .github/instructions/gphoto.instructions.md
    // for the history of this tradeoff.

    int get_bayer_offset_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_geometry_known_locked();
        return bayer_offset_x_;
    }

    int get_bayer_offset_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_geometry_known_locked();
        return bayer_offset_y_;
    }

    int get_bin_x() const override { return 1; }
    void set_bin_x(int bin_x) override { reject_non_unity_bin(bin_x); }
    int get_bin_y() const override { return 1; }
    void set_bin_y(int bin_y) override { reject_non_unity_bin(bin_y); }

    CameraState get_camera_state() const override {
        ensure_connected();
        if (exposure_active_.load()) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (expire_exposure_locked()) return CameraState::Idle;
            return CameraState::Exposing;
        }
        return CameraState::Idle;
    }

    int get_camera_x_size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_x_size_;
    }

    int get_camera_y_size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_y_size_;
    }

    bool get_can_abort_exposure() const override { return true; }
    bool get_can_asymmetric_bin() const override { return false; }
    bool get_can_fast_readout() const override { return false; }
    bool get_can_get_cooler_power() const override { return false; }
    bool get_can_pulse_guide() const override { return false; }
    bool get_can_set_ccd_temperature() const override { return false; }
    bool get_can_stop_exposure() const override { return true; }

    double get_ccd_temperature() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!sensor_temperature_.has_value()) {
            // Most DSLR RAW files (and Nikon's specifically) do not embed a
            // sensor-temperature tag; this only ever becomes available if
            // LibRaw finds one (observed mainly on Canon frames).
            throw AlpacaException("Sensor temperature not reported by this camera/frame",
                                  AlpacaError::PropertyNotImplemented);
        }
        return *sensor_temperature_;
    }

    // No gphoto2 camera has a cooler: CanGetCoolerPower and CanSetCCDTemperature are false, so the cooler
    // members raise PropertyNotImplemented (ICameraV4: "if not supported").
    bool get_cooler_on() const override {
        ensure_connected();
        throw AlpacaException("Cooler not supported", AlpacaError::PropertyNotImplemented);
    }
    void set_cooler_on(bool /*cooler_on*/) override {
        ensure_connected();
        throw AlpacaException("Cooler not supported", AlpacaError::PropertyNotImplemented);
    }

    double get_cooler_power() const override {
        ensure_connected();
        throw AlpacaException("Cooler not supported", AlpacaError::PropertyNotImplemented);
    }
    double get_electrons_per_adu() const override {
        ensure_connected();
        // libgphoto2 reports no e-/ADU, so there is no honest value (issue #1277).
        throw AlpacaException("ElectronsPerADU is not available for DSLR / mirrorless cameras",
                              AlpacaError::PropertyNotImplemented);
    }

    double get_exposure_max() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        // No hard ceiling exists once bulb mode is available; report a
        // generous practical cap rather than an unbounded value (ConformU
        // and most Alpaca clients expect a finite ExposureMax). Revisit
        // this constant if hardware validation shows a tighter camera-side
        // limit (e.g. a battery/thermal cutoff during long bulb captures).
        return has_bulb_ ? 3600.0 : max_native_shutter_seconds_;
    }

    double get_exposure_min() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return min_native_shutter_seconds_;
    }

    double get_exposure_resolution() const override { return 0.001; }

    bool get_fast_readout() const override {
        ensure_connected();
        throw AlpacaException("Fast readout not supported", AlpacaError::NotImplemented);
    }
    void set_fast_readout(bool) override {
        ensure_connected();
        throw AlpacaException("Fast readout not supported", AlpacaError::NotImplemented);
    }

    double get_full_well_capacity() const override {
        ensure_connected();
        // libgphoto2 reports no full well, so there is no honest value (issue #1277).
        throw AlpacaException("FullWellCapacity is not available for DSLR / mirrorless cameras",
                              AlpacaError::PropertyNotImplemented);
    }

    int get_gain() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        require_iso_supported_locked();
        return current_iso_index_;
    }

    void set_gain(int gain) override {
        ensure_connected();
        // Review PR #485 round 3: exposure_active_ alone is not a safe "is
        // the SDK busy" gate -- stop_exposure() and the get_camera_state()
        // watchdog both clear it *before* run_exposure()'s thread has
        // actually left libgphoto2 (stop_exposure() clears it, then joins;
        // the watchdog clears it with no join at all). A set_gain() that
        // only checked the flag could slip an SDK call in on the same
        // handle while the exposure thread was still inside
        // gp_camera_capture(), corrupting the shared PTP session.
        //
        // Taking exposure_lifecycle_mutex_ here -- the same mutex
        // start_exposure()/stop_exposure() hold for their setup-or-join
        // duration -- blocks set_gain() until any in-flight
        // exposure_thread_ has actually been joined, matching the lock
        // order used everywhere else in this class (lifecycle mutex,
        // then mutex_). This closes the ordinary StopExposure/SetGain
        // race. It does not (and cannot) close the watchdog's forced-idle
        // case: libgphoto2 has no capture-cancel primitive, so a thread
        // truly wedged inside the SDK past the watchdog deadline is a
        // pre-existing, documented limitation (see the watchdog's own log
        // message in get_camera_state()), not something a mutex can fix.
        std::lock_guard<std::mutex> lifecycle_lock(exposure_lifecycle_mutex_);
        with_handle([&](int handle) {
            require_iso_supported_locked();
            if (gain < 0 || gain >= static_cast<int>(iso_choices_.size())) {
                throw AlpacaException("Gain index out of range", AlpacaError::InvalidValue);
            }
            if (exposure_active_.load()) {
                throw AlpacaException("Cannot change ISO during an exposure", AlpacaError::InvalidOperation);
            }
            const std::string& choice = iso_choices_[static_cast<std::size_t>(gain)];
            sdk_.set_choice_value(handle, "iso", choice);
            current_iso_index_ = gain;
            return 0;
        });
    }

    // ASCOM's Gain interface has three mutually-exclusive modes: "Gain
    // Value" (GainMin/GainMax work, Gains throws), "Gain Index" (Gain/Gains
    // work, GainMin/GainMax throw), or "Gain Not Implemented" (all throw).
    // ISO here is a discrete Gains() list of the camera's real ISO choices,
    // not a continuous register, so this driver is in "Gain Index" mode --
    // unlike the other vendors here, which expose a true min/max range.
    int get_gain_max() const override {
        ensure_connected();
        throw AlpacaException("Gain range not supported; use Gains", AlpacaError::PropertyNotImplemented);
    }

    int get_gain_min() const override {
        ensure_connected();
        throw AlpacaException("Gain range not supported; use Gains", AlpacaError::PropertyNotImplemented);
    }

    std::vector<std::string> get_gains() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        require_iso_supported_locked();
        return iso_choices_;
    }

    bool get_has_shutter() const override { return true; }  // DSLRs have a real mechanical shutter

    double get_heat_sink_temperature() const override { return get_ccd_temperature(); }

    ImageArray get_image_array() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        throw_if_exposure_failed_locked();
        if (!last_exposure_valid_ || !image_ready_ || !image_cached_) {
            throw AlpacaException::asked_too_early("Image not ready");
        }
        return last_image_;
    }

    std::string get_image_array_variant() const override { return "Int32"; }

    bool get_image_ready() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        throw_if_exposure_failed_locked();
        return last_exposure_valid_ && image_ready_ && image_cached_;
    }

    bool get_is_pulse_guiding() const override { return false; }

    double get_last_exposure_duration() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        return published_exposure_.duration();
    }

    std::chrono::system_clock::time_point get_last_exposure_start_time() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected();
        return published_exposure_.start_time();
    }

    int get_max_adu() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return max_adu_;
    }

    int get_max_bin_x() const override { return 1; }
    int get_max_bin_y() const override { return 1; }

    int get_num_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return num_x_;
    }
    void set_num_x(int num_x) override { set_roi_dimension_locked(&num_x_, num_x); }

    int get_num_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return num_y_;
    }
    void set_num_y(int num_y) override { set_roi_dimension_locked(&num_y_, num_y); }

    int get_offset() const override {
        ensure_connected();
        throw AlpacaException("Offset not supported", AlpacaError::NotImplemented);
    }
    void set_offset(int) override {
        ensure_connected();
        throw AlpacaException("Offset not supported", AlpacaError::NotImplemented);
    }
    int get_offset_max() const override {
        ensure_connected();
        throw AlpacaException("Offset not supported", AlpacaError::NotImplemented);
    }
    int get_offset_min() const override {
        ensure_connected();
        throw AlpacaException("Offset not supported", AlpacaError::NotImplemented);
    }
    std::vector<std::string> get_offsets() const override {
        ensure_connected();
        throw AlpacaException("Offset not supported", AlpacaError::PropertyNotImplemented);
    }

    double get_percent_completed() const override {
        ensure_connected();
        if (!exposure_active_.load()) {
            std::lock_guard<std::mutex> lock(mutex_);
            return image_ready_ ? 100.0 : 0.0;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (last_exposure_duration_ <= 0.0 || !pending_progress_start_) return 0.0;
        double elapsed = std::chrono::duration<double>(clock_.now() - *pending_progress_start_).count();
        double percent = (elapsed / last_exposure_duration_) * 100.0;
        return std::clamp(percent, 0.0, 100.0);
    }

    // 0.0 (ASCOM "unknown") unless this exact model is in the static
    // known_pixel_size_um_table above -- libgphoto2 has no protocol-level
    // pixel-pitch query and it can't be derived from a decoded RAW frame
    // either, unlike CameraXSize/YSize.
    double get_pixel_size_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return pixel_size_um_;
    }
    double get_pixel_size_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return pixel_size_um_;
    }

    int get_readout_mode() const override { return 0; }
    void set_readout_mode(int mode) override {
        if (mode != 0) {
            throw AlpacaException("Invalid readout mode", AlpacaError::InvalidValue);
        }
    }
    std::vector<std::string> get_readout_modes() const override { return {"Normal"}; }

    std::string get_sensor_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.model : std::string();  // unknown: "" (ICameraV4 SensorName)
    }

    SensorType get_sensor_type() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return sensor_type_;
    }

    double get_set_ccd_temperature() const override {
        ensure_connected();
        throw AlpacaException("Cooler not supported", AlpacaError::NotImplemented);
    }
    void set_set_ccd_temperature(double) override {
        ensure_connected();
        throw AlpacaException("Cooler not supported", AlpacaError::NotImplemented);
    }

    int get_start_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return start_x_;
    }
    void set_start_x(int start_x) override { set_roi_dimension_locked(&start_x_, start_x); }

    int get_start_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return start_y_;
    }
    void set_start_y(int start_y) override { set_roi_dimension_locked(&start_y_, start_y); }

    double get_sub_exposure_duration() const override {
        ensure_connected();
        throw AlpacaException("Sub-exposure duration not supported", AlpacaError::NotImplemented);
    }
    void set_sub_exposure_duration(double) override {
        ensure_connected();
        throw AlpacaException("Sub-exposure duration not supported", AlpacaError::NotImplemented);
    }

    void abort_exposure() override { stop_exposure(); }

    void pulse_guide(int, int) override {
        ensure_connected();
        throw AlpacaException("Pulse guide not supported (no autoguider port on a DSLR / mirrorless camera)",
                              AlpacaError::NotImplemented);
    }

    void start_exposure(double duration, bool light) override {
        (void)light;  // A DSLR's mechanical shutter always opens; dark frames require capping the lens.

        if (!std::isfinite(duration) || duration < 0.0) {
            throw AlpacaException("Exposure duration must be finite and non-negative", AlpacaError::InvalidValue);
        }
        if (duration > 3600.0) {
            throw AlpacaException("Exposure duration out of range", AlpacaError::InvalidValue);
        }

        std::lock_guard<std::mutex> lifecycle_lock(exposure_lifecycle_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            validate_exposure_locked(duration);
        }
        stop_exposure_thread();

        int active_handle;
        std::string shutter_choice;
        std::string shutter_widget_name;
        double actual_duration;
        bool use_bulb;
        bool use_mlu;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            validate_exposure_locked(duration);
            active_handle = handle_;
            // Follow the camera's own mirror lock-up menu setting; the driver never writes it. With it on, the
            // body answers gp_camera_capture only after a 60 s timeout, so the shutter is fired with two
            // eosremoterelease press/release pairs (mlu_capture_with_abort). A body that cannot do that must
            // fail here, before the worker starts, rather than stall.
            try {
                use_mlu = mirror_lockup_on_locked(active_handle);
            } catch (const GPhotoDeviceGoneException& e) {
                // Same teardown as a worker that finds the camera gone (#1064): report disconnected so the
                // client reconnects, instead of leaving Connected true on a dead handle.
                mark_device_gone_locked(active_handle);
                throw AlpacaException(std::string("Camera is no longer connected: ") + e.what(),
                                      AlpacaError::NotConnected);
            }
            if (use_mlu && !remote_release_pair_.has_value()) {
                throw AlpacaException(
                    "Mirror lock-up is on in the camera, but this camera has no eosremoterelease press/release "
                    "control, so an exposure cannot be fired. Turn mirror lock-up off in the camera menu.",
                    AlpacaError::InvalidOperation);
            }
            // A body in B mode lists only "bulb" for its shutter speed, so there is no native
            // speed to fall back on: every duration takes the bulb path.
            use_bulb = has_bulb_ && ((native_shutter_choices_.empty() && to_lower(bulb_choice_) == "bulb") ||
                                     duration > max_native_shutter_seconds_ + 1e-9);
            actual_duration = duration;
            if (use_bulb) {
                shutter_choice = bulb_choice_;
            } else {
                const auto selected = nearest_shutter_choice_locked(duration);
                shutter_choice = selected.first;
                actual_duration = selected.second;
            }
            // In B mode the body owns the shutter and refuses a shutterspeed write ("I/O in progress");
            // the press/release pair alone drives the exposure.
            if (native_shutter_choices_.empty() && !remote_press_choice_.empty()) {
                shutter_choice.clear();
            }
            shutter_widget_name = shutter_widget_name_;

            last_exposure_duration_ = duration;
            pending_progress_start_.reset();
            last_exposure_valid_ = true;
            image_ready_ = false;
            image_cached_ = false;
            exposure_failed_ = false;
            exposure_error_.clear();
            abort_requested_.store(false);
            // Generous watchdog margin over the requested duration: USB
            // transfer of a 20+MB RAW file plus libgphoto2/PTP overhead. A
            // bulb capture also gets the whole window bulb_capture_with_abort
            // may spend waiting for the frame after the shutter closes, so
            // the watchdog never declares Idle while that wait is still
            // legitimately running. A mirror lock-up exposure waits for its
            // frame the same way (it has no gp_camera_capture to return it) and
            // also spends the settle between its two presses.
            auto duration_margin = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                       std::chrono::duration<double>(duration)) +
                                   std::chrono::seconds(60);
            if (use_bulb || use_mlu) {
                duration_margin += bulb_file_wait_for(duration);
            }
            if (use_mlu) {
                duration_margin +=
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(options_.mirror_lockup_settle);
            }
            exposure_deadline_ = clock_.now() + duration_margin;
            exposure_deadline_valid_ = true;
            exposure_active_.store(true);
        }

        exposure_thread_ = std::thread(
            [this, active_handle, shutter_choice, shutter_widget_name, use_bulb, use_mlu, duration, actual_duration]() {
                run_exposure(active_handle, shutter_choice, shutter_widget_name, use_bulb, use_mlu, duration,
                             actual_duration);
            });
    }

    void stop_exposure() override {
        ensure_connected();
        std::lock_guard<std::mutex> lifecycle_lock(exposure_lifecycle_mutex_);
        // For a bulb capture this closes the shutter early (checked inside
        // the sleep loop in run_exposure). For a native shutter-speed
        // capture, libgphoto2 has no capture-cancel primitive: gp_camera_
        // capture() is a single blocking call, so "abort" here can only wait
        // for it to finish naturally (bounded by the longest native shutter
        // choice, typically <=30s) rather than truly interrupt it. A mirror
        // lock-up exposure has no such call: an abort cuts the settle short and
        // still fires the second press/release pair (the mirror is up), then
        // run_exposure discards the frame.
        stop_exposure_thread();
        std::lock_guard<std::mutex> lock(mutex_);
        image_ready_ = false;
        image_cached_ = false;
        exposure_failed_ = false;
        exposure_error_.clear();
        exposure_deadline_valid_ = false;
    }

private:
    int device_number_;
    int camera_index_;
    int handle_{-1};
    GPhotoSDK& sdk_;
    RawDecoder& decoder_;
    util::TaskClock& clock_;
    const GPhotoOptions options_;

    mutable std::mutex mutex_;
    std::atomic<bool> connected_{false};
    // True while a sync set_connected(true) has released mutex_ for the
    // priming capture (review PR #485 round 2) -- guards the window
    // conn_task_ cannot see, since set_connected() is a public sync entry
    // point in its own right, not just the async task's body. Mirrors
    // ToupTek AFW's connecting_homing_; see async_connectable.h's
    // record_pending_disconnect() doc comment, written for this exact shape.
    bool connecting_priming_{false};

    GPhotoCameraInfo camera_info_{};
    bool camera_info_valid_{false};
    double pixel_size_um_{0.0};  // 0.0 = unknown; see known_pixel_size_um_table above

    // Widget capability caches, populated at connect (configure_after_connect_locked).
    std::vector<std::string> iso_choices_;
    int current_iso_index_{0};
    bool has_bulb_{false};  // long exposure available: a "bulb" toggle, or both eosremoterelease choices
    // Canon bodies have no "bulb" widget; bulb is the shutter-speed "bulb" choice held open by writing
    // eosremoterelease (issue #640). Both are picked by name at connect, never by index; empty without the pair.
    std::string remote_press_choice_;
    std::string remote_release_choice_;
    // The eosremoterelease (press, release) pair, found whenever the widget has one, whatever the mode dial says.
    // Mirror lock-up fires the shutter with it. Unlike remote_press_choice_ it does not mean bulb is available.
    std::optional<std::pair<std::string, std::string>> remote_release_pair_;
    std::string bulb_choice_;  // shutter-speed choice string selecting bulb mode
    std::vector<std::pair<std::string, double>> native_shutter_choices_;  // sorted ascending by seconds
    std::string shutter_widget_name_{"shutterspeed2"};  // whichever of the fallback names was found at connect
    double min_native_shutter_seconds_{0.001};
    double max_native_shutter_seconds_{30.0};
    std::optional<std::string> format_widget_name_;
    std::optional<std::string> raw_format_choice_;

    // Sensor geometry -- unknown until the first successful exposure (see
    // the class-level comment on get_bayer_offset_x above).
    bool geometry_known_{false};
    int camera_x_size_{0};
    int camera_y_size_{0};
    int bayer_offset_x_{0};
    int bayer_offset_y_{0};
    int max_adu_{65535};
    SensorType sensor_type_{SensorType::RGGB};

    int num_x_{0};
    int num_y_{0};
    int start_x_{0};
    int start_y_{0};

    mutable bool image_ready_{false};
    mutable bool image_cached_{false};
    mutable ImageArray last_image_{};
    double last_exposure_duration_{0.0};  // requested; PercentCompleted's denominator
    std::optional<util::TaskClock::clock::time_point> pending_progress_start_;
    bool last_exposure_valid_{false};
    // Last successful image only; in-flight timing stays local to the exposure worker.
    util::LastExposure published_exposure_;
    std::optional<double> sensor_temperature_;

    mutable std::atomic<bool> exposure_active_{false};
    // Set when the exposure worker fails or its deadline expires; cleared by StartExposure, AbortExposure and
    // disconnect. Both guarded by mutex_.
    mutable bool exposure_failed_{false};
    mutable std::string exposure_error_;
    std::atomic<bool> abort_requested_{false};
    std::thread exposure_thread_;
    std::mutex exposure_lifecycle_mutex_;
    mutable std::chrono::steady_clock::time_point exposure_deadline_{};
    mutable bool exposure_deadline_valid_{false};

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
        }
    }

    // Holds mutex_ across the whole SDK call, not just the handle copy --
    // review PR #485: a "copy the handle out, then call the SDK" helper
    // (the previous handle_value()) is a use-after-close trap, since a
    // concurrent set_connected(false) can close the handle in the window
    // between the copy and the unlocked SDK call. Same pattern as
    // ToupTek's with_handle(), the project's reference for this.
    template <typename Fn>
    auto with_handle(Fn&& fn) const -> decltype(fn(0)) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (handle_ < 0) {
            throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
        }
        return fn(handle_);
    }

    void require_geometry_known_locked() const {
        if (!geometry_known_) {
            throw AlpacaException("Sensor geometry not yet known -- take one exposure first",
                                  AlpacaError::InvalidOperation);
        }
    }

    void require_iso_supported_locked() const {
        if (iso_choices_.empty()) {
            throw AlpacaException("ISO control not exposed by this camera", AlpacaError::NotImplemented);
        }
    }

    static void reject_non_unity_bin(int value) {
        if (value != 1) {
            throw AlpacaException("Binning not supported by DSLR cameras", AlpacaError::InvalidValue);
        }
    }

    void reset_exposure_state_locked() {
        image_ready_ = false;
        image_cached_ = false;
        last_exposure_duration_ = 0.0;
        pending_progress_start_.reset();
        last_exposure_valid_ = false;
        published_exposure_.clear();
        exposure_active_.store(false);
        exposure_deadline_valid_ = false;
        exposure_failed_ = false;
        exposure_error_.clear();
        abort_requested_.store(false);
    }

    void stop_exposure_thread() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            abort_requested_.store(true);
            exposure_active_.store(false);
        }
        if (exposure_thread_.joinable()) {
            exposure_thread_.join();
        }
    }

    double get_exposure_max_locked() const { return has_bulb_ ? 3600.0 : max_native_shutter_seconds_; }

    void validate_exposure_locked(double duration) const {
        ensure_connected();
        if (duration > get_exposure_max_locked()) {
            throw AlpacaException("Exposure duration out of range", AlpacaError::InvalidValue);
        }
        if (geometry_known_) {
            if (num_x_ <= 0 || num_y_ <= 0) {
                throw AlpacaException("ROI is not valid for exposure", AlpacaError::InvalidValue);
            }
            if (start_x_ < 0 || start_y_ < 0 || start_x_ >= camera_x_size_ || start_y_ >= camera_y_size_) {
                throw AlpacaException("Start position outside sensor bounds", AlpacaError::InvalidValue);
            }
            if (start_x_ + num_x_ > camera_x_size_ || start_y_ + num_y_ > camera_y_size_) {
                throw AlpacaException("ROI extends beyond sensor bounds", AlpacaError::InvalidValue);
            }
        }
    }

    std::pair<std::string, double> nearest_shutter_choice_locked(double duration) const {
        if (native_shutter_choices_.empty()) {
            throw AlpacaException("No shutter speed control exposed by this camera", AlpacaError::NotImplemented);
        }
        const auto* best = &native_shutter_choices_.front();
        double best_diff = std::abs(best->second - duration);
        for (const auto& entry : native_shutter_choices_) {
            double diff = std::abs(entry.second - duration);
            if (diff < best_diff) {
                best = &entry;
                best_diff = diff;
            }
        }
        return *best;
    }

    void set_roi_dimension_locked(int* field, int value) {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (value < 0) {
            throw AlpacaException("ROI value must be non-negative", AlpacaError::InvalidValue);
        }
        *field = value;
    }

    void preload_camera_info_locked() {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            auto cameras = sdk_.enumerate_cameras();
            if (camera_index_ >= 0 && camera_index_ < static_cast<int>(cameras.size())) {
                camera_info_ = cameras[static_cast<std::size_t>(camera_index_)];
                camera_info_valid_ = true;
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG(kLogTag, "Unable to preload camera info: " + std::string(e.what()));
        }
    }

    void refresh_cached_camera_info_if_needed() {
        if (connected_.load()) return;
        try {
            auto cameras = sdk_.enumerate_cameras();
            std::lock_guard<std::mutex> lock(mutex_);
            if (camera_index_ >= 0 && camera_index_ < static_cast<int>(cameras.size())) {
                camera_info_ = cameras[static_cast<std::size_t>(camera_index_)];
                camera_info_valid_ = true;
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG(kLogTag, "Unable to refresh camera info: " + std::string(e.what()));
        }
    }

    // Reads widget capabilities from the freshly opened camera and caches
    // them for the rest of the session. Requires mutex_ held by the caller
    // (set_connected holds it for the whole connect sequence).
    void configure_after_connect_locked(GPhotoSDK& sdk, int handle) {
        iso_choices_.clear();
        current_iso_index_ = 0;
        if (sdk.has_widget(handle, "iso")) {
            iso_choices_ = sdk.get_choices(handle, "iso");
            std::string current = sdk.get_choice_value(handle, "iso");
            auto it = std::find(iso_choices_.begin(), iso_choices_.end(), current);
            if (it != iso_choices_.end()) {
                current_iso_index_ = static_cast<int>(std::distance(iso_choices_.begin(), it));
            }
        }

        native_shutter_choices_.clear();
        has_bulb_ = false;
        bulb_choice_.clear();
        min_native_shutter_seconds_ = 0.001;
        max_native_shutter_seconds_ = 30.0;
        const char* shutter_widget_names[] = {"shutterspeed2", "shutterspeed", "eos-shutterspeed"};
        for (const char* name : shutter_widget_names) {
            if (!sdk.has_widget(handle, name)) continue;
            shutter_widget_name_ = name;
            for (const auto& choice : sdk.get_choices(handle, name)) {
                auto seconds = parse_shutter_speed_seconds(choice);
                if (!seconds.has_value()) {
                    // A "bulb"/fraction-sentinel entry in the shutter-speed
                    // choice list: remember it so run_exposure can prime the
                    // dial into bulb mode before driving the toggle below.
                    // Prefer a literal "bulb" over other unparseable labels such as
                    // "auto" (Canon shutter value 0), which does not hold the shutter.
                    if (bulb_choice_.empty() || to_lower(choice) == "bulb") bulb_choice_ = choice;
                    continue;
                }
                native_shutter_choices_.emplace_back(choice, *seconds);
            }
            break;
        }
        // Two ways to hold the shutter open (see bulb_capture_with_abort): the
        // standalone "bulb" toggle (Nikon), or on Canon bodies, which have no
        // such widget, the eosremoterelease press/release pair together with
        // the shutter-speed "bulb" choice. Without either, ExposureMax stays
        // capped at the longest native shutter speed. The pair is also kept in
        // remote_release_pair_ whatever the mode dial says, because mirror
        // lock-up fires the shutter with it (mlu_capture_with_abort).
        remote_press_choice_.clear();
        remote_release_choice_.clear();
        remote_release_pair_.reset();
        has_bulb_ = sdk.has_widget(handle, "bulb");
        // The pair is looked up whenever the widget exists, even beside a "bulb" toggle: mirror lock-up needs it
        // to fire the shutter whatever the bulb path is.
        if (sdk.has_widget(handle, "eosremoterelease")) {
            const auto remote_choices = sdk.get_choices(handle, "eosremoterelease");
            const auto has = [&](const char* choice) {
                return std::find(remote_choices.begin(), remote_choices.end(), choice) != remote_choices.end();
            };
            // The pair is chosen as a unit: both are remotereleaseon(3, 1) then remotereleaseoff(3).
            // libgphoto2 2.5.34 renamed the choices; its "Release Full" is remotereleaseoff(2), which
            // leaves the half press held, so it only pairs with the 2.5.31 "Press Full".
            static constexpr std::pair<const char*, const char*> kRemoteReleasePairs[] = {
                {"Press Full MF", "Release"},    // libgphoto2 2.5.34 and later
                {"Press Full", "Release Full"},  // libgphoto2 2.5.33 and earlier
            };
            for (const auto& [press, release] : kRemoteReleasePairs) {
                if (has(press) && has(release)) {
                    remote_release_pair_ = std::make_pair(std::string(press), std::string(release));
                    break;
                }
            }
            // Bulb on a Canon body is the shutter-speed "bulb" choice held open by that pair.
            if (!has_bulb_ && remote_release_pair_.has_value() && to_lower(bulb_choice_) == "bulb") {
                remote_press_choice_ = remote_release_pair_->first;
                remote_release_choice_ = remote_release_pair_->second;
                has_bulb_ = true;
            }
        }
        if (!has_bulb_) {
            bulb_choice_.clear();
        }
        std::sort(native_shutter_choices_.begin(), native_shutter_choices_.end(),
                  [](const auto& a, const auto& b) { return a.second < b.second; });
        if (!native_shutter_choices_.empty()) {
            min_native_shutter_seconds_ = native_shutter_choices_.front().second;
            max_native_shutter_seconds_ = native_shutter_choices_.back().second;
        }

        format_widget_name_.reset();
        raw_format_choice_.reset();
        const char* format_widget_names[] = {"imageformat", "imagequality"};
        for (const char* name : format_widget_names) {
            if (!sdk.has_widget(handle, name)) continue;
            auto choices = sdk.get_choices(handle, name);
            auto raw_choice = pick_raw_format_choice(choices);
            if (raw_choice.has_value()) {
                format_widget_name_ = name;
                raw_format_choice_ = raw_choice;
                sdk.set_choice_value(handle, name, *raw_choice);
                break;
            }
        }
        if (!raw_format_choice_.has_value()) {
            ALPACA_LOG_WARN(kLogTag,
                            "No RAW image-quality choice found on this camera; captures will fail to "
                            "decode unless the camera's current format is already RAW.");
        }

        apply_long_exposure_nr_locked(sdk, handle);

        // Geometry (sensor size, Bayer phase, max ADU) is not knowable until
        // a frame has actually been decoded -- reset here so a reconnect
        // never serves stale dimensions from a previous camera. set_connected
        // repopulates it right after this, from the on-disk cache or (first
        // time this model has ever been seen) a priming capture.
        geometry_known_ = false;
        camera_x_size_ = 0;
        camera_y_size_ = 0;
        num_x_ = 0;
        num_y_ = 0;
        start_x_ = 0;
        start_y_ = 0;
        max_adu_ = 65535;
        sensor_type_ = SensorType::RGGB;
        sensor_temperature_.reset();
    }

    // Called once, without mutex_ held, right after a brand-new camera model
    // (never before cached on this rig) finishes connecting. Takes one
    // throwaway capture purely to learn geometry via libraw, discards the
    // pixel data (never touches last_image_/image_ready_ -- this must not
    // look like a real exposure to the ASCOM client), and persists the
    // result so this cost is never paid again for this model. Best-effort:
    // on any failure, geometry simply stays unknown until the caller's own
    // first real exposure, exactly like before this existed.
    void prime_sensor_geometry_and_cache(int handle, const std::string& model) {
        auto& sdk = sdk_;
        std::string shutter_choice;
        std::string widget_name;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!native_shutter_choices_.empty()) {
                // Fastest native shutter speed: this capture only exists to
                // read the frame's dimensions, not to gather light.
                shutter_choice = native_shutter_choices_.front().first;
                widget_name = shutter_widget_name_;
            }
        }
        if (!shutter_choice.empty()) {
            sdk.set_choice_value(handle, widget_name, shutter_choice);
        }

        GPhotoCaptureResult capture = sdk.capture_and_download(handle);
        DecodedFrame decoded = decoder_.decode(capture.data);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            set_geometry_locked(decoded.width, decoded.height, decoded.bayer_offset_x, decoded.bayer_offset_y,
                                decoded.max_adu, decoded.sensor_type);
        }
        store_cached_sensor_geometry(model, CachedSensorGeometry{decoded.width, decoded.height, decoded.bayer_offset_x,
                                                                 decoded.bayer_offset_y, decoded.max_adu});
    }

    // Requires mutex_ held. A failed exposure is reported by ImageReady and ImageArray while CameraState stays
    // Idle, so the next StartExposure can run: a sticky Error poisoned every later operation on the ZWO driver,
    // which reports a failure the same way (issue #1204).
    void throw_if_exposure_failed_locked() const {
        if (exposure_failed_) {
            throw AlpacaException("Exposure failed: " + exposure_error_, AlpacaError::DriverException);
        }
    }

    // Requires mutex_ held; late frames fail admission even if no client polled the watchdog.
    bool expire_exposure_locked() const {
        if (!exposure_active_.load() || !exposure_deadline_valid_ || clock_.now() < exposure_deadline_) return false;
        exposure_error_ = "Exposure deadline exceeded";
        exposure_failed_ = true;
        image_ready_ = false;
        image_cached_ = false;
        exposure_active_.store(false);
        exposure_deadline_valid_ = false;
        ALPACA_LOG_WARN(kLogTag,
                        "Exposure deadline exceeded; discarding frame. "
                        "Exposure thread may still be blocked inside libgphoto2.");
        return true;
    }

    // Record the failure so ImageReady and ImageArray say why. A camera that left the USB bus
    // (auto power-off) is also reported disconnected, so the client reconnects; reconnecting enumerates the
    // camera again. Runs on the exposure worker; stop_exposure()/set_connected(false) join it before they
    // close handle_, so handle_ only changes under mutex_ (here and in mark_device_gone_locked, which
    // start_exposure also calls from the caller's thread). A stop in progress is not a failure.
    void record_exposure_failure(int handle, const char* what, bool device_gone) {
        if (abort_requested_.load()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!exposure_failed_) {
            exposure_error_ = what;
            exposure_failed_ = true;
        }
        exposure_active_.store(false);
        exposure_deadline_valid_ = false;
        if (!device_gone) return;
        mark_device_gone_locked(handle);
    }

    // Requires mutex_ held. The camera behind `handle` left the USB bus: forget the handle, report
    // disconnected and close it. A handle that is no longer current is left alone.
    void mark_device_gone_locked(int handle) {
        if (handle_ != handle) return;
        handle_ = -1;
        connected_.store(false);
        try {
            sdk_.close_camera(handle);
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
            // The handle belongs to a device that is gone; a close failure adds nothing the caller can act on.
        }
    }

    struct ExposureTiming {
        double duration{0.0};
        std::chrono::system_clock::time_point start;
    };

    void run_exposure(int handle, const std::string& shutter_choice, const std::string& shutter_widget_name,
                      bool use_bulb, bool use_mlu, double duration, double actual_duration) {
        auto& sdk = sdk_;
        try {
            if (!shutter_choice.empty() && !shutter_widget_name.empty()) {
                sdk.set_choice_value(handle, shutter_widget_name, shutter_choice);
            }

            GPhotoCaptureResult capture;
            ExposureTiming timing{actual_duration, {}};
            if (use_mlu) {
                capture = mlu_capture_with_abort(handle, duration, use_bulb, timing);
            } else if (use_bulb) {
                capture = bulb_capture_with_abort(handle, duration, timing);
            } else {
                // Host trigger-call boundary, not a measured photon edge; the camera clock's EXIF time is not UTC.
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    pending_progress_start_ = clock_.now();
                    timing.start = std::chrono::system_clock::now();
                }
                capture = sdk.capture_and_download(handle);
            }

            if (!exposure_active_.load()) {
                // Aborted while the (uncancellable) capture call was still
                // in flight; discard the frame that arrived after the fact.
                return;
            }

            DecodedFrame decoded = decoder_.decode(capture.data);

            std::lock_guard<std::mutex> lock(mutex_);
            if (expire_exposure_locked() || !exposure_active_.load()) {
                // Aborted or expired while the frame was decoding: the image is discarded,
                // so nothing is applied or published.
                return;
            }
            apply_decoded_frame_locked(decoded);
            image_cached_ = true;
            image_ready_ = true;
            published_exposure_.commit(timing.duration, timing.start);
            exposure_active_.store(false);
            exposure_deadline_valid_ = false;
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN(kLogTag, "Exposure failed: " + std::string(e.what()));
            record_exposure_failure(handle, e.what(), dynamic_cast<const GPhotoDeviceGoneException*>(&e) != nullptr);
        }
    }

    // How long to keep polling for the frame after a bulb (or mirror lock-up) shutter closes. A
    // body with long-exposure noise reduction on holds the file for a second
    // exposure-length (the dark frame) before posting it, so the window
    // scales with the exposure rather than being a fixed few seconds; the
    // constant margin covers the RAW write and the USB download on top.
    static std::chrono::steady_clock::duration bulb_file_wait_for(double duration_s) {
        return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                   std::chrono::duration<double>(duration_s)) +
               std::chrono::seconds(30);
    }

    // After an abort the frame is still polled for -- briefly -- so the
    // aborted file is consumed and deleted rather than left queued for the
    // next exposure to mistake for its own (a body posts it a second or two
    // after the close). run_exposure() discards whatever arrives.
    static constexpr auto kAbortedBulbFileWait = std::chrono::seconds(15);

    // Pumps the camera's event queue in short slices for `duration_s` on the TaskClock, or until an abort. The
    // slices (instead of a sleep) keep a Nikon body seeing the host poll for the whole bulb exposure the way the
    // gphoto2 CLI's --wait-event does between bulb=1 and bulb=0 (issue #569), and let stop_exposure()/
    // abort_exposure() end the hold well before the requested time. The hold calls into the SDK, so unlike the
    // sleep it replaced it can throw. A throw that skipped the close would leave the shutter open, the exact
    // wedge issue #569 is about, so `close` runs on every way out of the hold and the original failure is
    // rethrown afterwards.
    void hold_with_events(int handle, std::chrono::steady_clock::duration duration,
                          const std::function<void()>& close) {
        constexpr auto kHoldSlice = std::chrono::milliseconds(100);
        const auto hold_deadline = clock_.now() + duration;
        try {
            while (!abort_requested_.load()) {
                const auto now = clock_.now();
                if (now >= hold_deadline) break;
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(hold_deadline - now);
                sdk_.drain_events(handle, std::min(remaining, std::chrono::milliseconds(kHoldSlice)));
            }
        } catch (...) {
            try {
                close();
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(kLogTag, "Shutter close after a failed hold also failed: " + std::string(e.what()));
            }
            throw;
        }
        close();
    }

    // After the shutter closes, polls for the frame in slices up to bulb_file_wait_for(duration_s), checking the
    // abort flag between slices.
    GPhotoCaptureResult wait_for_bulb_file(int handle, double duration_s, const char* what = "Bulb capture") {
        auto& sdk = sdk_;
        using clock = util::TaskClock::clock;
        constexpr auto kFilePollSlice = std::chrono::seconds(1);

        bool aborted = abort_requested_.load();
        auto file_deadline =
            clock_.now() + (aborted ? clock::duration(kAbortedBulbFileWait) : bulb_file_wait_for(duration_s));
        while (true) {
            const auto now = clock_.now();
            if (now >= file_deadline) break;
            if (!aborted && abort_requested_.load()) {
                aborted = true;
                file_deadline = std::min(file_deadline, now + clock::duration(kAbortedBulbFileWait));
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(file_deadline - now);
            auto frame =
                sdk.poll_bulb_file_and_download(handle, std::min(remaining, std::chrono::milliseconds(kFilePollSlice)));
            if (frame.has_value()) {
                return std::move(*frame);
            }
        }
        throw AlpacaException(
            std::string(what) + ": no file-added event from camera within " +
                std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                   aborted ? clock::duration(kAbortedBulbFileWait) : bulb_file_wait_for(duration_s))
                                   .count()) +
                " s of closing the shutter (is long-exposure noise reduction on?)",
            AlpacaError::DriverException);
    }

    // Bulb capture with early-abort support: open the shutter, hold_with_events() for the exposure, close it,
    // then wait_for_bulb_file().
    GPhotoCaptureResult bulb_capture_with_abort(int handle, double duration_s, ExposureTiming& timing) {
        auto& sdk = sdk_;
        using clock = util::TaskClock::clock;

        // Nikon: the "bulb" toggle. Canon: eosremoterelease Press Full / Release Full.
        const bool use_remote_release = !remote_press_choice_.empty();
        const auto shutter_open = [&] {
            if (use_remote_release) {
                sdk.set_choice_value(handle, "eosremoterelease", remote_press_choice_);
            } else {
                sdk.set_toggle_value(handle, "bulb", true);
            }
        };
        // Elapsed host open/close command boundaries include event-pump overruns, not file wait or decode.
        // Physical shutter latency is camera-dependent and needs rig measurement.
        util::TaskClock::clock::time_point opened_at;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            opened_at = clock_.now();
            timing.start = std::chrono::system_clock::now();
            pending_progress_start_ = opened_at;
        }
        const auto shutter_close = [&] {
            timing.duration = std::chrono::duration<double>(clock_.now() - opened_at).count();
            if (use_remote_release) {
                sdk.set_choice_value(handle, "eosremoterelease", remote_release_choice_);
            } else {
                sdk.set_toggle_value(handle, "bulb", false);
            }
        };

        shutter_open();
        hold_with_events(handle, std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(duration_s)),
                         shutter_close);
        return wait_for_bulb_file(handle, duration_s);
    }

    // The eosremoterelease sequence for a body with mirror lock-up on (never gp_camera_capture, which times out
    // after 60 s): press and release to raise the mirror, settle, then press and release again to take the frame
    // (`hold` keeps the second press down for the exposure, the bulb case). The frame arrives as a file-added
    // event, so it is waited for like a bulb frame.
    //
    // The settle runs on the TaskClock and pumps events like the hold. An abort cuts it short but does not skip
    // the second pair: the mirror is up, and a body left there would fire on the NEXT exposure's first press.
    // The frame it takes is discarded by run_exposure().
    GPhotoCaptureResult mlu_capture_with_abort(int handle, double duration_s, bool hold, ExposureTiming& timing) {
        using clock = util::TaskClock::clock;
        auto& sdk = sdk_;
        const auto press = [&] { sdk.set_choice_value(handle, "eosremoterelease", remote_release_pair_->first); };
        const auto release = [&] { sdk.set_choice_value(handle, "eosremoterelease", remote_release_pair_->second); };

        press();
        release();  // the mirror is up
        hold_with_events(handle, options_.mirror_lockup_settle, [] {});
        util::TaskClock::clock::time_point opened_at;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            opened_at = clock_.now();
            timing.start = std::chrono::system_clock::now();
            pending_progress_start_ = opened_at;
        }
        press();
        if (hold) {
            hold_with_events(
                handle, std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(duration_s)), [&] {
                    timing.duration = std::chrono::duration<double>(clock_.now() - opened_at).count();
                    release();
                });
        } else {
            release();
        }
        return wait_for_bulb_file(handle, duration_s, "Mirror lock-up capture");
    }

    // Requires mutex_ held. True when the camera's own mirror lock-up (MLU) menu item is on. Single-widget
    // lookups only (docs/failures/0009): Canon 70D `mirrorlock` (0/1, writable but never written here), else
    // Canon 250D `mirrorlockstatus` (read-only, 0 off, 1 on, 2 after the first press). Any non-zero number is
    // on, so a leftover 2 never reads as off and sends the body to a stalling gp_camera_capture. A body with
    // neither widget is MLU off, which keeps the single-capture path.
    bool mirror_lockup_on_locked(int handle) {
        for (const char* name : {"mirrorlock", "mirrorlockstatus"}) {
            if (!sdk_.has_widget(handle, name)) continue;
            const std::string value = to_lower(sdk_.get_text_value(handle, name));
            char* end = nullptr;
            const double number = std::strtod(value.c_str(), &end);
            if (end != value.c_str()) return number != 0.0;
            return value == "on" || value == "true" || value == "yes";
        }
        return false;
    }

    // Requires mutex_ held, camera idle (connect). Writes the Nikon longexpnr widget once when the config asks
    // for "on" or "off"; "camera" writes nothing. Never fails the connect: a body without the widget (every
    // Canon tested) or a refused write logs a WARN and the connect carries on. The choice text is matched
    // without regard to case against the camera's own list, because libgphoto2 translates On/Off.
    void apply_long_exposure_nr_locked(GPhotoSDK& sdk, int handle) {
        const std::string& mode = options_.long_exposure_nr;
        if (mode != "on" && mode != "off") return;
        try {
            if (!sdk.has_widget(handle, "longexpnr")) {
                ALPACA_LOG_WARN(kLogTag, "long-exposure NR is not settable on this camera; set it in the camera menu");
                return;
            }
            const auto choices = sdk.get_choices(handle, "longexpnr");
            const auto match = std::find_if(choices.begin(), choices.end(),
                                            [&](const std::string& choice) { return to_lower(choice) == mode; });
            if (match == choices.end()) {
                ALPACA_LOG_WARN(kLogTag, "long-exposure NR has no \"" + mode +
                                             "\" choice on this camera; set it in the camera menu");
                return;
            }
            sdk.set_choice_value(handle, "longexpnr", *match);
        } catch (const GPhotoDeviceGoneException&) {
            throw;
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN(kLogTag, "long-exposure NR could not be set (" + std::string(e.what()) +
                                         "); set it in the camera menu");
        }
    }

    // Requires mutex_ held. Populates the geometry properties (CameraXSize/
    // YSize, BayerOffsetX/Y, MaxADU) shared by both a real decoded exposure
    // and a cache-hit/priming-capture result that never touches last_image_.
    void set_geometry_locked(int width, int height, int bayer_offset_x, int bayer_offset_y, int max_adu,
                             SensorType sensor_type = SensorType::RGGB) {
        bool first_frame = !geometry_known_;
        camera_x_size_ = width;
        camera_y_size_ = height;
        bayer_offset_x_ = bayer_offset_x;
        bayer_offset_y_ = bayer_offset_y;
        max_adu_ = max_adu;
        sensor_type_ = sensor_type;
        geometry_known_ = true;

        if (first_frame || num_x_ <= 0 || num_y_ <= 0) {
            num_x_ = width;
            num_y_ = height;
            start_x_ = 0;
            start_y_ = 0;
        }
    }

    // Requires mutex_ held; applies a freshly decoded frame, cropping to the
    // client-requested software ROI (StartX/StartY/NumX/NumY), clamped to
    // the frame bounds.
    void apply_decoded_frame_locked(const DecodedFrame& frame) {
        set_geometry_locked(frame.width, frame.height, frame.bayer_offset_x, frame.bayer_offset_y, frame.max_adu,
                            frame.sensor_type);
        sensor_temperature_ = frame.sensor_temperature;

        int crop_w = std::clamp(num_x_, 1, frame.width - std::clamp(start_x_, 0, frame.width - 1));
        int crop_h = std::clamp(num_y_, 1, frame.height - std::clamp(start_y_, 0, frame.height - 1));
        int origin_x = std::clamp(start_x_, 0, frame.width - 1);
        int origin_y = std::clamp(start_y_, 0, frame.height - 1);

        if (crop_w == frame.width && crop_h == frame.height && origin_x == 0 && origin_y == 0) {
            last_image_.data = frame.pixels;
        } else {
            last_image_.data.resize(static_cast<std::size_t>(crop_w) * crop_h);
            for (int row = 0; row < crop_h; ++row) {
                const std::int32_t* src_row =
                    frame.pixels.data() + static_cast<std::size_t>(row + origin_y) * frame.width + origin_x;
                std::int32_t* dst_row = last_image_.data.data() + static_cast<std::size_t>(row) * crop_w;
                std::copy(src_row, src_row + crop_w, dst_row);
            }
        }
        last_image_.width = crop_w;
        last_image_.height = crop_h;
        last_image_.rank = 2;
    }
};

DecodedFrame LibRawDecoder::decode(const std::vector<std::uint8_t>& raw_bytes) {
    LibRaw processor;
    int rc = processor.open_buffer(raw_bytes.data(), raw_bytes.size());
    if (rc != LIBRAW_SUCCESS) {
        throw AlpacaException(std::string("libraw failed to open captured frame: ") + libraw_strerror(rc),
                              AlpacaError::DriverException);
    }
    rc = processor.unpack();
    if (rc != LIBRAW_SUCCESS) {
        throw AlpacaException(std::string("libraw failed to unpack captured frame: ") + libraw_strerror(rc),
                              AlpacaError::DriverException);
    }

    const auto& sizes = processor.imgdata.sizes;
    const ushort* raw_image = processor.imgdata.rawdata.raw_image;
    if (raw_image == nullptr || sizes.width == 0 || sizes.height == 0) {
        throw AlpacaException("libraw produced no Bayer data for this frame", AlpacaError::DriverException);
    }

    DecodedFrame frame;
    frame.width = sizes.width;
    frame.height = sizes.height;
    frame.pixels.resize(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height));
    for (int row = 0; row < frame.height; ++row) {
        const ushort* src_row =
            raw_image + static_cast<std::size_t>(row + sizes.top_margin) * sizes.raw_width + sizes.left_margin;
        std::int32_t* dst_row = frame.pixels.data() + static_cast<std::size_t>(row) * frame.width;
        for (int col = 0; col < frame.width; ++col) {
            dst_row[col] = static_cast<std::int32_t>(src_row[col]);
        }
    }

    // Bayer phase of the top-left 2x2 tile: find which cell libraw
    // reports as the red channel.
    const char* cdesc = processor.imgdata.idata.cdesc;
    frame.bayer_offset_x = 0;
    frame.bayer_offset_y = 0;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            int color_index = processor.COLOR(y, x);
            if (color_index >= 0 && color_index < 4 && cdesc[color_index] == 'R') {
                frame.bayer_offset_x = x;
                frame.bayer_offset_y = y;
            }
        }
    }
    frame.sensor_type = SensorType::RGGB;  // Nikon/Canon/Sony DSLR sensors are all standard Bayer RGGB variants

    frame.max_adu = processor.imgdata.color.maximum > 0 ? static_cast<int>(processor.imgdata.color.maximum) : 65535;

    float sensor_temp = processor.imgdata.makernotes.common.SensorTemperature;
    if (sensor_temp > -273.15f) {
        frame.sensor_temperature = static_cast<double>(sensor_temp);
    }

    return frame;
}

std::unique_ptr<CameraDriver> create_gphoto_camera(int device_number, int camera_index, const GPhotoOptions& options) {
    static LibRawDecoder decoder;
    return std::make_unique<GPhotoCameraDriver>(device_number, camera_index, GPhotoSDKWrapper::instance(), decoder,
                                                util::default_task_clock(), options);
}

std::unique_ptr<CameraDriver> create_gphoto_camera(int device_number, int camera_index, GPhotoSDK& sdk,
                                                   RawDecoder& decoder, util::TaskClock& clock,
                                                   const GPhotoOptions& options) {
    return std::make_unique<GPhotoCameraDriver>(device_number, camera_index, sdk, decoder, clock, options);
}

}  // namespace alpacacore::vendor::gphoto
