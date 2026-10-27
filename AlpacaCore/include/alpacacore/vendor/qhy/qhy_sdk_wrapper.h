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

#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace alpacacore::vendor::qhy {

using CoolerOffAction = std::function<void()>;

/**
 * @brief Basic camera info returned by enumeration (no open required).
 *
 * Full chip info (pixel size, dimensions, capabilities) is populated after
 * the camera is opened and initialized via get_chip_info().
 */
struct QHYCameraInfo {
    std::string camera_id;        //!< unique string ID from SDK (e.g., "QHY600-Pro-M-c7b72b")
    std::string model;            //!< camera model name (e.g., "QHY600-Pro-M")
    uint32_t max_width{};         //!< maximum image width in pixels
    uint32_t max_height{};        //!< maximum image height in pixels
    double pixel_size_x_um{};     //!< pixel width in microns
    double pixel_size_y_um{};     //!< pixel height in microns
    uint32_t bpp{};               //!< bits per pixel (native depth)
    bool is_color{};              //!< true if Bayer color sensor
    uint32_t bayer_pattern{};     //!< BAYER_ID from SDK: BAYER_GB=1, BAYER_GR=2, BAYER_BG=3, BAYER_RG=4 (0 = monochrome)
    bool has_cooler{};            //!< true if camera has TEC cooler
    bool has_st4_port{};          //!< true if camera has ST-4 guide port
    bool has_shutter{};           //!< true if camera has mechanical shutter
};

/**
 * @brief Min/max/step range for a camera control parameter.
 */
struct QHYControlRange {
    double min{};
    double max{};
    double step{};
    bool available{};
};

/**
 * @brief QHY CONTROL_ID constants mirrored from qhyccdstruct.h.
 *
 * These allow the camera driver to reference controls without including
 * the QHY SDK headers directly.
 */
namespace control {
    constexpr int BRIGHTNESS        = 0;
    constexpr int CONTRAST          = 1;
    constexpr int GAIN              = 6;
    constexpr int OFFSET            = 7;
    constexpr int EXPOSURE          = 8;   //!< exposure time in microseconds
    constexpr int SPEED             = 9;
    constexpr int TRANSFERBIT       = 10;
    constexpr int CURTEMP           = 14;  //!< current CCD temperature (Celsius)
    constexpr int CURPWM            = 15;  //!< current cooler PWM (0-100)
    constexpr int MANULPWM          = 16;  //!< manual cooler PWM set
    constexpr int COOLER            = 18;  //!< capability: has cooler
    constexpr int ST4PORT           = 19;  //!< capability: has ST-4 port
    constexpr int CAM_COLOR         = 20;  //!< capability: is color camera
    constexpr int BIN1X1            = 21;  //!< capability: 1x1 binning
    constexpr int BIN2X2            = 22;  //!< capability: 2x2 binning
    constexpr int BIN3X3            = 23;  //!< capability: 3x3 binning
    constexpr int BIN4X4            = 24;  //!< capability: 4x4 binning
    constexpr int MECHANICALSHUTTER = 25;  //!< capability: mechanical shutter
    constexpr int CFWPORT = 17;            //!< capability: has color filter wheel port
    constexpr int CHIPTEMP          = 32;  //!< capability: chip temperature sensor
    constexpr int BITS8             = 34;  //!< capability: 8-bit output
    constexpr int BITS16            = 35;  //!< capability: 16-bit output
    constexpr int CFWSLOTSNUM = 44;        //!< query: color filter wheel slot count
    constexpr int IS_EXPOSING_DONE  = 45;  //!< poll: 1 if exposure complete
} // namespace control

/**
 * @brief QHY guide port direction constants (QHY convention).
 *
 * Note: Differs from Alpaca (0=North, 1=South, 2=East, 3=West).
 * QHY guide direction mapping:
 *   0 = EAST  (RA+)
 *   1 = NORTH (Dec+)
 *   2 = SOUTH (Dec-)
 *   3 = WEST  (RA-)
 */
namespace guide_direction {
    constexpr uint32_t EAST  = 0;
    constexpr uint32_t NORTH = 1;
    constexpr uint32_t SOUTH = 2;
    constexpr uint32_t WEST  = 3;
} // namespace guide_direction

/**
 * @brief Abstract interface over the QHYCCD SDK operations the drivers use.
 *
 * This is the fault-injection seam (issue #321): production code talks to the
 * QHYSDKWrapper singleton below; tests substitute a scripted fake
 * (AlpacaCore/tests/fake_qhy_sdk.h) that can throw from any specific call,
 * return canned enumerations/positions, and count opens vs closes — so the
 * connect paths are exercisable without hardware. The QHY SDK is the one
 * vendor blob that cannot run at all on a USB-less host: the first libqhyccd
 * call spawns PnpEventListenerThread, which segfaults in
 * libusb_hotplug_register_callback when libusb_init failed. Any QHY connect
 * or `[stress]` test therefore MUST run against the fake and never touch the
 * singleton — that is the rule for anything added here, not a description of
 * current coverage: the connect tests on this branch follow it, and the
 * `[stress]` registration (issue #321) has to satisfy it before the two QHY
 * entries can come off the allow-list in check_stress_registration.py.
 * Each create_qhy_* factory has an overload taking a QHYSDK&; the
 * default overload passes the singleton.
 *
 * Contract notes for implementors (fakes included):
 * - Methods report failure by THROWING AlpacaException, never by return code
 *   (except the documented bool returns on get_camera_model, get_chip_info,
 *   is_control_available, start_single_frame and get_single_frame).
 * - open_camera/close_camera are reference-counted per camera_id: the physical
 *   open happens on the first opener and the physical close when the last
 *   owner releases, so a camera driver and the CFW driver on the same physical
 *   device (e.g. the miniCam8M's integrated wheel) each get an independent
 *   open/close lifecycle over one shared handle.
 * - A fake MUST NOT block in any method. The camera driver's exposure,
 *   temperature and cooler-off workers join with a bounded timeout and DETACH
 *   on expiry, and its pulse-guide thread is detached by design; a blocking
 *   fake turns those into detached threads still calling into the fake after
 *   the test body has moved on.
 *
 * LIFETIME: drivers hold a plain QHYSDK& — and the detachable workers above
 * capture a QHYSDK* — so the SDK object MUST outlive every driver constructed
 * on it. Declare the fake before the driver (locals destroy in reverse order);
 * never stash a driver beyond the fake's scope.
 */
class QHYSDK {
public:
    // Destructor is protected and NON-virtual (below), not public and virtual:
    // nothing ever owns a QHYSDK*. Drivers hold a QHYSDK&, workers capture a
    // raw QHYSDK*, and every implementation is either a function-local static
    // (QHYSDKWrapper) or a stack object (FakeQHYSDK, LockedQHYSDK). A public
    // virtual destructor here would make `delete static_cast<QHYSDK*>(&...)`
    // compile against the singleton, since access for delete is checked on the
    // static type.

    // ── Enumeration ──────────────────────────────────────────────────────────

    /**
     * @brief Scan and enumerate connected QHY cameras.
     *
     * Returns basic info (camera_id + model) for each camera found.
     * Chip dimensions and capabilities require calling get_chip_info() after
     * open_camera() + init_camera().
     */
    virtual std::vector<QHYCameraInfo> enumerate_cameras() = 0;

    /**
     * @brief Get the model name for a camera ID without opening it.
     */
    virtual bool get_camera_model(const std::string& camera_id, std::string& model) = 0;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /**
     * @brief Open (or share) the physical handle for a camera.
     *
     * Reference-counted by camera_id: a camera driver and an accessory driver
     * on the same physical device (e.g. the miniCam8M's integrated CFW) can
     * both call this for the same id and each get an independent open/close
     * lifecycle -- the underlying OpenQHYCCD fires once, on the first opener,
     * and CloseQHYCCD only when the last owner calls close_camera().
     */
    virtual void open_camera(const std::string& camera_id) = 0;
    virtual void init_camera(const std::string& camera_id) = 0;
    virtual void close_camera(const std::string& camera_id) = 0;

    /**
     * @brief Register the exposure worker's liveness flag for a camera_id.
     *
     * The camera driver calls this once per exposure generation (alongside
     * its own exposure_thread_running_ assignment) so open_camera() below
     * can refuse to open a SECOND physical handle to the same device while
     * a detached zombie worker might still be blocked inside
     * GetQHYCCDSingleFrame on the OLD handle -- a hazard reachable not just
     * through the camera driver's own reconnect, but through the paired CFW
     * driver reconnecting independently (they share one physical handle).
     * Stored outside the handle map so it survives that map entry being
     * erased by close_camera() -- which is exactly the moment it's needed.
     */
    virtual void register_exposure_worker(const std::string& camera_id,
                                          std::shared_ptr<std::atomic<bool>> running_flag) = 0;

    // ── Chip info (requires open + init) ────────────────────────────────────

    /**
     * @brief Populate full QHYCameraInfo after open+init.
     *
     * Calls GetQHYCCDChipInfo and capability checks.
     */
    virtual bool get_chip_info(const std::string& camera_id, QHYCameraInfo& info) = 0;

    // ── Control parameters ───────────────────────────────────────────────────

    virtual bool is_control_available(const std::string& camera_id, int control_id) = 0;
    virtual double get_param(const std::string& camera_id, int control_id) = 0;
    virtual QHYControlRange get_param_range(const std::string& camera_id, int control_id) = 0;
    virtual void set_param(const std::string& camera_id, int control_id, double value) = 0;

    // Retain this physical handle now; the returned action may run after logical close.
    virtual CoolerOffAction prepare_cooler_off(const std::string& camera_id,
                                               std::shared_ptr<std::atomic<bool>> running_flag) = 0;

    // ── Image configuration ──────────────────────────────────────────────────

    virtual void set_resolution(const std::string& camera_id, uint32_t start_x, uint32_t start_y, uint32_t width,
                                uint32_t height) = 0;
    virtual void set_bin_mode(const std::string& camera_id, uint32_t wbin, uint32_t hbin) = 0;
    virtual void set_bits_mode(const std::string& camera_id, uint32_t bits) = 0;
    virtual uint32_t get_mem_length(const std::string& camera_id) = 0;

    // ── Exposure ─────────────────────────────────────────────────────────────

    /**
     * @brief Start a single frame exposure.
     * @return true if SDK returned QHYCCD_READ_DIRECTLY (old cameras — read immediately)
     */
    virtual bool start_single_frame(const std::string& camera_id) = 0;

    /**
     * @brief Blocking call that waits for a single frame and copies it into buffer.
     *
     * buffer must be pre-allocated to at least get_mem_length() bytes.
     * @return true on success, false on failure
     */
    virtual bool get_single_frame(const std::string& camera_id, uint8_t* buffer, uint32_t& width, uint32_t& height,
                                  uint32_t& bpp, uint32_t& channels) = 0;

    /**
     * @brief Cancel the current exposure (and discard the frame).
     *
     * Uses CancelQHYCCDExposingAndReadout — all cameras support this.
     */
    virtual void cancel_exposure(const std::string& camera_id) = 0;

    // ── Guide port ──────────────────────────────────────────────────────────

    /**
     * @brief Issue a pulse guide command.
     *
     * @param qhy_direction  QHY direction: EAST=0, NORTH=1, SOUTH=2, WEST=3
     * @param duration_ms    Duration in milliseconds
     */
    virtual void guide(const std::string& camera_id, uint32_t qhy_direction, uint16_t duration_ms) = 0;

    // ── Temperature ──────────────────────────────────────────────────────────

    /**
     * @brief Drive the TEC toward target_temp_c using the SDK's PID controller.
     *
     * Must be called periodically (approximately every second) while cooling
     * is active.
     */
    virtual void control_temp(const std::string& camera_id, double target_temp_c) = 0;

    // ── Filter wheel (integrated CFW) ────────────────────────────────────────

    /**
     * @brief Move the integrated color filter wheel to a slot.
     * @param position 0-based slot index.
     */
    virtual void move_cfw(const std::string& camera_id, int position) = 0;

    /**
     * @brief Read the CFW's current settled position.
     * @return 0-based slot index, or -1 if the wheel is still moving/unsettled.
     */
    virtual int get_cfw_position(const std::string& camera_id) = 0;

    // ── Readout modes ────────────────────────────────────────────────────────

    virtual uint32_t get_num_readout_modes(const std::string& camera_id) = 0;
    virtual std::string get_readout_mode_name(const std::string& camera_id, uint32_t mode_index) = 0;
    virtual void set_readout_mode(const std::string& camera_id, uint32_t mode_index) = 0;

    // ── Misc ─────────────────────────────────────────────────────────────────

    virtual std::string get_sdk_version() = 0;

protected:
    // See the note at the top of the class: protected + non-virtual, so no
    // caller can delete through a QHYSDK*, while every implementation is still
    // destroyed normally through its own static type.
    ~QHYSDK() = default;
};

/**
 * @brief Singleton wrapper around the QHYCCD SDK.
 *
 * Manages SDK resource lifetime (InitQHYCCDResource / ReleaseQHYCCDResource),
 * camera handle lifecycle, and exposes all per-camera SDK operations.
 * SDK headers are not exposed; all types are standard C++.
 *
 * The SDK resource comes up lazily, on the first call that needs it, rather
 * than in the constructor — see Impl's constructor comment in the .cpp for
 * why (constructing this must never crash a USB-less host).
 */
class QHYSDKWrapper final : public QHYSDK {
public:
    static QHYSDKWrapper& instance();

    std::vector<QHYCameraInfo> enumerate_cameras() override;
    bool get_camera_model(const std::string& camera_id, std::string& model) override;

    void open_camera(const std::string& camera_id) override;
    void init_camera(const std::string& camera_id) override;
    void close_camera(const std::string& camera_id) override;
    void register_exposure_worker(const std::string& camera_id,
                                  std::shared_ptr<std::atomic<bool>> running_flag) override;

    bool get_chip_info(const std::string& camera_id, QHYCameraInfo& info) override;

    bool is_control_available(const std::string& camera_id, int control_id) override;
    double get_param(const std::string& camera_id, int control_id) override;
    QHYControlRange get_param_range(const std::string& camera_id, int control_id) override;
    void set_param(const std::string& camera_id, int control_id, double value) override;
    CoolerOffAction prepare_cooler_off(const std::string& camera_id,
                                       std::shared_ptr<std::atomic<bool>> running_flag) override;

    void set_resolution(const std::string& camera_id, uint32_t start_x, uint32_t start_y, uint32_t width,
                        uint32_t height) override;
    void set_bin_mode(const std::string& camera_id, uint32_t wbin, uint32_t hbin) override;
    void set_bits_mode(const std::string& camera_id, uint32_t bits) override;
    uint32_t get_mem_length(const std::string& camera_id) override;

    bool start_single_frame(const std::string& camera_id) override;
    bool get_single_frame(const std::string& camera_id, uint8_t* buffer, uint32_t& width, uint32_t& height,
                          uint32_t& bpp, uint32_t& channels) override;
    void cancel_exposure(const std::string& camera_id) override;

    void guide(const std::string& camera_id, uint32_t qhy_direction, uint16_t duration_ms) override;

    void control_temp(const std::string& camera_id, double target_temp_c) override;

    void move_cfw(const std::string& camera_id, int position) override;
    int get_cfw_position(const std::string& camera_id) override;

    uint32_t get_num_readout_modes(const std::string& camera_id) override;
    std::string get_readout_mode_name(const std::string& camera_id, uint32_t mode_index) override;
    void set_readout_mode(const std::string& camera_id, uint32_t mode_index) override;

    std::string get_sdk_version() override;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;

    QHYSDKWrapper();
    // No `override`: QHYSDK's destructor is protected and non-virtual, so this
    // does not override anything. It still runs on the function-local static's
    // destruction at exit, which is the only way this object is ever destroyed.
    ~QHYSDKWrapper();

    QHYSDKWrapper(const QHYSDKWrapper&) = delete;
    QHYSDKWrapper& operator=(const QHYSDKWrapper&) = delete;
};

/// QHYCCD_ERROR (0xFFFFFFFF) as QHYSDK::get_param() returns it: GetQHYCCDParam()
/// answers it for an unsupported control or a failed read.
inline constexpr double kParamReadError = 4294967295.0;

/**
 * @brief Convert a QHYSDK::get_param() result to int (issue #510).
 *
 * A plain static_cast<int> of the QHYCCD_ERROR sentinel, or of any double
 * outside int's range, is undefined behaviour. Throws DriverException
 * ("QHY SDK returned no value for <control_name>") when the value is not
 * finite, is the sentinel, or is outside [INT_MIN, INT_MAX]; otherwise
 * returns the value truncated toward zero.
 */
inline int param_to_int(double value, std::string_view control_name) {
    if (!std::isfinite(value) || value == kParamReadError ||
        value < static_cast<double>(std::numeric_limits<int>::min()) ||
        value > static_cast<double>(std::numeric_limits<int>::max())) {
        throw AlpacaException("QHY SDK returned no value for " + std::string(control_name),
                              AlpacaError::DriverException);
    }
    return static_cast<int>(value);
}

} // namespace alpacacore::vendor::qhy
