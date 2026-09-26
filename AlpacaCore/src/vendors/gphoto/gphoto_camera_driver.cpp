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
#include <alpacacore/util/logging.h>
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
#include <filesystem>
#include <fstream>
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
// same model. AlpacaCore does not do JSON (see alpaca_json.h), so this is a
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

    GPhotoCameraDriver(int device_number, int camera_index, GPhotoSDK& sdk, RawDecoder& decoder)
        : AsyncConnectable("GPhoto"),
          device_number_(device_number),
          camera_index_(camera_index),
          sdk_(sdk),
          decoder_(decoder) {
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

    std::string get_unique_id() const override {
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
        // instead of hitting a just-closed camera (AGENTS.md disconnect
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
        if (!connected_.load()) return CameraState::Idle;
        if (exposure_active_.load()) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (exposure_deadline_valid_ && std::chrono::steady_clock::now() >= exposure_deadline_) {
                ALPACA_LOG_WARN(kLogTag,
                                "Exposure deadline exceeded; forcing CameraState=Idle. "
                                "Exposure thread may still be blocked inside libgphoto2.");
                exposure_active_.store(false);
                exposure_deadline_valid_ = false;
                return CameraState::Idle;
            }
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

    bool get_cooler_on() const override { return false; }
    void set_cooler_on(bool cooler_on) override {
        ensure_connected();
        if (cooler_on) {
            throw AlpacaException("Cooler not supported", AlpacaError::NotImplemented);
        }
    }

    double get_cooler_power() const override { return 0.0; }
    double get_electrons_per_adu() const override { return 1.0; }  // unknown; ConformU rejects 0

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
        throw AlpacaException("Fast readout not supported", AlpacaError::NotImplemented);
    }
    void set_fast_readout(bool) override {
        throw AlpacaException("Fast readout not supported", AlpacaError::NotImplemented);
    }

    double get_full_well_capacity() const override { return 0.0; }  // unknown for DSLR sensors

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
        throw AlpacaException("Gain range not supported; use Gains", AlpacaError::PropertyNotImplemented);
    }

    int get_gain_min() const override {
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
        if (!last_exposure_valid_ || !image_ready_ || !image_cached_) {
            throw AlpacaException("Image not ready", AlpacaError::InvalidOperation);
        }
        return last_image_;
    }

    std::string get_image_array_variant() const override { return "Int32"; }

    bool get_image_ready() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return last_exposure_valid_ && image_ready_ && image_cached_;
    }

    bool get_is_pulse_guiding() const override { return false; }

    double get_last_exposure_duration() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!last_exposure_valid_) {
            throw AlpacaException("Last exposure duration not set", AlpacaError::ValueNotSet);
        }
        return last_exposure_duration_;
    }

    std::chrono::system_clock::time_point get_last_exposure_start_time() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!last_exposure_valid_) {
            throw AlpacaException("Last exposure start time not set", AlpacaError::ValueNotSet);
        }
        return last_exposure_start_;
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

    int get_offset() const override { throw AlpacaException("Offset not supported", AlpacaError::NotImplemented); }
    void set_offset(int) override { throw AlpacaException("Offset not supported", AlpacaError::NotImplemented); }
    int get_offset_max() const override { throw AlpacaException("Offset not supported", AlpacaError::NotImplemented); }
    int get_offset_min() const override { throw AlpacaException("Offset not supported", AlpacaError::NotImplemented); }
    std::vector<std::string> get_offsets() const override {
        throw AlpacaException("Offset not supported", AlpacaError::PropertyNotImplemented);
    }

    double get_percent_completed() const override {
        if (!connected_.load()) return 0.0;
        if (!exposure_active_.load()) {
            std::lock_guard<std::mutex> lock(mutex_);
            return image_ready_ ? 100.0 : 0.0;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (last_exposure_duration_ <= 0.0) return 0.0;
        double elapsed = std::chrono::duration<double>(std::chrono::system_clock::now() - last_exposure_start_).count();
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
        return camera_info_valid_ ? camera_info_.model : "DSLR / Mirrorless Sensor";
    }

    SensorType get_sensor_type() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return sensor_type_;
    }

    double get_set_ccd_temperature() const override {
        throw AlpacaException("Cooler not supported", AlpacaError::NotImplemented);
    }
    void set_set_ccd_temperature(double) override {
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
        throw AlpacaException("Sub-exposure duration not supported", AlpacaError::NotImplemented);
    }
    void set_sub_exposure_duration(double) override {
        throw AlpacaException("Sub-exposure duration not supported", AlpacaError::NotImplemented);
    }

    void abort_exposure() override { stop_exposure(); }

    void pulse_guide(int, int) override {
        ensure_connected();
        throw AlpacaException("Pulse guide not supported (no autoguider port on a DSLR / mirrorless camera)",
                              AlpacaError::NotImplemented);
    }

    void start_exposure(double duration, bool light) override {
        ensure_connected();
        (void)light;  // A DSLR's mechanical shutter always opens; dark frames require capping the lens.

        if (duration < 0.0) {
            throw AlpacaException("Exposure duration must be non-negative", AlpacaError::InvalidValue);
        }

        std::lock_guard<std::mutex> lifecycle_lock(exposure_lifecycle_mutex_);
        stop_exposure_thread();

        int active_handle;
        std::string shutter_choice;
        std::string shutter_widget_name;
        bool use_bulb;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (handle_ < 0) {
                throw AlpacaException("Camera handle not set", AlpacaError::NotConnected);
            }
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
            active_handle = handle_;
            use_bulb = has_bulb_ && duration > max_native_shutter_seconds_ + 1e-9;
            shutter_choice = use_bulb ? bulb_choice_ : nearest_shutter_choice_locked(duration);
            shutter_widget_name = shutter_widget_name_;

            last_exposure_duration_ = duration;
            last_exposure_start_ = std::chrono::system_clock::now();
            last_exposure_valid_ = true;
            image_ready_ = false;
            image_cached_ = false;
            abort_requested_.store(false);
            // Generous watchdog margin over the requested duration: USB
            // transfer of a 20+MB RAW file plus libgphoto2/PTP overhead. A
            // bulb capture also gets the whole window bulb_capture_with_abort
            // may spend waiting for the frame after the shutter closes, so
            // the watchdog never declares Idle while that wait is still
            // legitimately running.
            auto duration_margin = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                       std::chrono::duration<double>(duration)) +
                                   std::chrono::seconds(60);
            if (use_bulb) {
                duration_margin += bulb_file_wait_for(duration);
            }
            exposure_deadline_ = std::chrono::steady_clock::now() + duration_margin;
            exposure_deadline_valid_ = true;
            exposure_active_.store(true);
        }

        exposure_thread_ =
            std::thread([this, active_handle, shutter_choice, shutter_widget_name, use_bulb, duration]() {
                run_exposure(active_handle, shutter_choice, shutter_widget_name, use_bulb, duration);
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
        // choice, typically <=30s) rather than truly interrupt it.
        abort_requested_.store(true);
        exposure_active_.store(false);
        if (exposure_thread_.joinable()) {
            exposure_thread_.join();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        image_ready_ = false;
        image_cached_ = false;
        exposure_deadline_valid_ = false;
    }

private:
    int device_number_;
    int camera_index_;
    int handle_{-1};
    GPhotoSDK& sdk_;
    RawDecoder& decoder_;

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
    bool has_bulb_{false};
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
    double last_exposure_duration_{0.0};
    std::chrono::system_clock::time_point last_exposure_start_{};
    bool last_exposure_valid_{false};
    std::optional<double> sensor_temperature_;

    mutable std::atomic<bool> exposure_active_{false};
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
        last_exposure_start_ = std::chrono::system_clock::time_point{};
        last_exposure_valid_ = false;
        exposure_active_.store(false);
        exposure_deadline_valid_ = false;
        abort_requested_.store(false);
    }

    void stop_exposure_thread() {
        abort_requested_.store(true);
        exposure_active_.store(false);
        if (exposure_thread_.joinable()) {
            exposure_thread_.join();
        }
    }

    double get_exposure_max_locked() const { return has_bulb_ ? 3600.0 : max_native_shutter_seconds_; }

    std::string nearest_shutter_choice_locked(double duration) const {
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
        return best->first;
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
                    bulb_choice_ = choice;
                    continue;
                }
                native_shutter_choices_.emplace_back(choice, *seconds);
            }
            break;
        }
        // Bulb mode is only implementable when the standalone "bulb" toggle
        // widget is present -- that is the only mechanism this driver drives
        // (see bulb_capture_with_abort). A shutter-speed "bulb" choice with
        // no toggle widget (the classic Canon eosremoterelease press/release
        // sequence) is not supported yet; ExposureMax stays capped at the
        // longest native shutter speed for such a camera. TODO(gphoto/canon):
        // add the press/release path if/when tested against real hardware.
        has_bulb_ = sdk.has_widget(handle, "bulb");
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

    void run_exposure(int handle, const std::string& shutter_choice, const std::string& shutter_widget_name,
                      bool use_bulb, double duration) {
        auto& sdk = sdk_;
        try {
            if (!shutter_choice.empty() && !shutter_widget_name.empty()) {
                sdk.set_choice_value(handle, shutter_widget_name, shutter_choice);
            }

            GPhotoCaptureResult capture;
            if (use_bulb) {
                capture = bulb_capture_with_abort(handle, duration);
            } else {
                capture = sdk.capture_and_download(handle);
            }

            if (!exposure_active_.load()) {
                // Aborted while the (uncancellable) capture call was still
                // in flight; discard the frame that arrived after the fact.
                return;
            }

            DecodedFrame decoded = decoder_.decode(capture.data);

            std::lock_guard<std::mutex> lock(mutex_);
            apply_decoded_frame_locked(decoded);
            image_cached_ = true;
            image_ready_ = true;
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN(kLogTag, "Exposure failed: " + std::string(e.what()));
        }
        exposure_active_.store(false);
    }

    // How long to keep polling for the frame after a bulb shutter closes. A
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

    // Bulb capture with early-abort support. The hold loop pumps the camera's
    // event queue in short slices instead of sleeping, so a Nikon body sees
    // the host polling for the whole exposure the way the gphoto2 CLI's
    // --wait-event does between bulb=1 and bulb=0 (issue #569), and so
    // stop_exposure()/abort_exposure() can close the shutter well before the
    // full requested duration elapses. After the close, the frame is polled
    // for in slices up to bulb_file_wait_for(duration), checking the abort
    // flag between slices.
    GPhotoCaptureResult bulb_capture_with_abort(int handle, double duration_s) {
        auto& sdk = sdk_;
        using clock = std::chrono::steady_clock;
        constexpr auto kHoldSlice = std::chrono::milliseconds(100);
        constexpr auto kFilePollSlice = std::chrono::seconds(1);

        sdk.set_toggle_value(handle, "bulb", true);
        const auto hold_deadline =
            clock::now() + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(duration_s));
        // The hold calls into the SDK, so unlike the sleep it replaced it can
        // throw. A throw that skipped the close would leave the shutter open,
        // the exact wedge issue #569 is about, so bulb=0 is sent on every way
        // out of the hold and the original failure is rethrown afterwards.
        try {
            while (!abort_requested_.load()) {
                const auto now = clock::now();
                if (now >= hold_deadline) break;
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(hold_deadline - now);
                sdk.drain_events(handle, std::min(remaining, std::chrono::milliseconds(kHoldSlice)));
            }
        } catch (...) {
            try {
                sdk.set_toggle_value(handle, "bulb", false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(kLogTag, "Bulb close after a failed hold also failed: " + std::string(e.what()));
            }
            throw;
        }
        sdk.set_toggle_value(handle, "bulb", false);

        bool aborted = abort_requested_.load();
        auto file_deadline =
            clock::now() + (aborted ? clock::duration(kAbortedBulbFileWait) : bulb_file_wait_for(duration_s));
        while (true) {
            const auto now = clock::now();
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
            "Bulb capture: no file-added event from camera within " +
                std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                   aborted ? clock::duration(kAbortedBulbFileWait) : bulb_file_wait_for(duration_s))
                                   .count()) +
                " s of closing the shutter (is long-exposure noise reduction on?)",
            AlpacaError::DriverException);
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

std::unique_ptr<CameraDriver> create_gphoto_camera(int device_number, int camera_index) {
    static LibRawDecoder decoder;
    return std::make_unique<GPhotoCameraDriver>(device_number, camera_index, GPhotoSDKWrapper::instance(), decoder);
}

std::unique_ptr<CameraDriver> create_gphoto_camera(int device_number, int camera_index, GPhotoSDK& sdk,
                                                   RawDecoder& decoder) {
    return std::make_unique<GPhotoCameraDriver>(device_number, camera_index, sdk, decoder);
}

}  // namespace alpacacore::vendor::gphoto
