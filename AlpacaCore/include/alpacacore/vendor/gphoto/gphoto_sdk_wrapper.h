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

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace alpacacore::vendor::gphoto {

/**
 * @brief One USB-attached camera as reported by libgphoto2 autodetection.
 */
struct GPhotoCameraInfo {
    std::string model;  // e.g. "Nikon DSC D5300"
    std::string port;   // e.g. "usb:001,004"
};

/**
 * @brief Result of a triggered capture: the downloaded file bytes plus the
 * metadata libgphoto2 reported for it.
 */
struct GPhotoCaptureResult {
    std::vector<std::uint8_t> data;
    std::string filename;
    std::string mime_type;
};

/**
 * @brief A call failed because the camera is no longer on the USB bus (GP_ERROR_IO_USB_FIND), for
 * example after the body's auto power-off.
 *
 * Still an AlpacaException with the DriverException number, so every caller that does not know about it
 * sees the failure it saw before. The exposure worker catches it to report the camera as disconnected.
 */
class GPhotoDeviceGoneException : public AlpacaException {
public:
    using AlpacaException::AlpacaException;
};

/**
 * @brief Abstract interface over the libgphoto2 operations the gphoto driver
 * uses.
 *
 * This is the fault-injection seam (issue #489), mirroring the shape used by
 * the other camera vendors (e.g. ToupTekSDK in
 * AlpacaCore/include/alpacacore/vendor/touptek/touptek_sdk_wrapper.h):
 * production code talks to the GPhotoSDKWrapper singleton below; unit tests
 * substitute a scripted fake (AlpacaCore/tests/fake_gphoto_sdk.h) that can
 * throw from any specific call, script widget choices/values per camera, and
 * count opens vs closes -- so the connect/configure/capture/bulb paths are
 * exercisable without hardware. Every driver factory has an overload taking
 * a GPhotoSDK&; the default overload passes the singleton.
 *
 * Methods report failure by throwing AlpacaException, never by return code.
 */
class GPhotoSDK {
public:
    // Destructor is protected and NON-virtual (below), not public and virtual:
    // nothing ever owns a GPhotoSDK*. Drivers hold a GPhotoSDK&, and every
    // implementation is either a function-local static (GPhotoSDKWrapper) or a
    // stack object (FakeGPhotoSDK, LockedGPhotoSDK). A public virtual
    // destructor here would make `delete static_cast<GPhotoSDK*>(&...)`
    // compile against the singleton, since access for delete is checked on the
    // static type. AGENTS.md calls this the shape to copy; ToupTek's public
    // virtual destructor predates that reasoning.

    /**
     * @brief Enumerate currently attached PTP/MTP cameras via USB autodetect.
     *
     * Index into the returned vector is the Alpaca "cameraIndex" config
     * field, matching the SDK-enumerated convention used by the other camera
     * vendors (ZWO/QHY/SVBONY/PlayerOne/ToupTek).
     */
    virtual std::vector<GPhotoCameraInfo> enumerate_cameras() = 0;

    /**
     * @brief Open a session for the camera at the given model/port pair.
     *
     * @return An opaque handle for use with every other method below.
     * @throws AlpacaException on failure (camera not found, busy, etc).
     */
    virtual int open_camera(const std::string& model, const std::string& port) = 0;

    /// Close a previously opened camera. Safe to call on an unknown handle
    /// (no-op) so driver disconnect paths never need to track validity.
    virtual void close_camera(int handle) = 0;

    virtual std::string get_gphoto_version() = 0;
    virtual std::string get_camera_summary(int handle) = 0;

    virtual bool has_widget(int handle, const std::string& name) = 0;

    /// Radio/menu widgets (e.g. "iso", "shutterspeed2", "imagequality").
    virtual std::vector<std::string> get_choices(int handle, const std::string& name) = 0;
    virtual std::string get_choice_value(int handle, const std::string& name) = 0;
    virtual void set_choice_value(int handle, const std::string& name, const std::string& value) = 0;

    /// Toggle widgets (e.g. the Nikon/Canon PTP "bulb" shutter-open toggle).
    virtual bool get_toggle_value(int handle, const std::string& name) = 0;
    virtual void set_toggle_value(int handle, const std::string& name, bool on) = 0;

    /// Text widgets (rarely needed; kept for completeness/diagnostics).
    virtual std::string get_text_value(int handle, const std::string& name) = 0;

    /// Trigger a normal (non-bulb) capture and download the resulting file.
    virtual GPhotoCaptureResult capture_and_download(int handle) = 0;

    /**
     * @brief Pump the camera's PTP event queue for up to `budget`, discarding
     * everything it delivers, and return once the budget is spent.
     *
     * The bulb hold loop in gphoto_camera_driver.cpp calls this in short
     * slices instead of sleeping: a Nikon body expects the host to keep
     * polling its events while a bulb capture is open, exactly what the
     * gphoto2 CLI's `--wait-event` does between `bulb=1` and `bulb=0`
     * (issue #569). Best-effort for the poll itself: a failed event poll
     * never throws and sleeps out the remaining budget instead. It may still
     * throw for an invalid or closed handle (a disconnect racing the hold),
     * so the caller must send `bulb=0` on the exception path as well; the
     * driver's hold loop does exactly that. A file-added event seen
     * here cannot belong to the exposure in progress (its shutter is still
     * open), so it is a leftover from an earlier one: the file is deleted
     * from the camera and the event dropped, never handed to the next
     * poll_bulb_file_and_download() as a fresh frame.
     */
    virtual void drain_events(int handle, std::chrono::milliseconds budget) = 0;

    /**
     * @brief Wait up to `timeout` for the file-added event a just-closed
     * bulb shutter produces, then download and delete that file.
     *
     * Returns std::nullopt when no file arrived within `timeout` so the caller
     * can keep polling in slices (checking its own abort flag between them)
     * up to a deadline of its own choosing; a real libgphoto2 failure throws.
     * Callers drive the bulb sequence themselves via set_toggle_value(handle,
     * "bulb", true/false) and drain_events() (see gphoto_camera_driver.cpp's
     * abortable hold loop) and call this after closing the shutter.
     */
    virtual std::optional<GPhotoCaptureResult> poll_bulb_file_and_download(int handle,
                                                                           std::chrono::milliseconds timeout) = 0;

protected:
    // See the note at the top of the class: protected + non-virtual, so no
    // caller can delete through a GPhotoSDK*, while every implementation is
    // still destroyed normally through its own static type.
    ~GPhotoSDK() = default;
};

/**
 * @brief Thin RAII wrapper around libgphoto2's Camera/GPContext/CameraWidget
 * API, isolating raw libgphoto2 calls from the Alpaca driver logic (the
 * project's SDK-wrapper pattern -- see AlpacaCore/src/vendors/svbony for the
 * sibling shape used by SDK-based cameras).
 *
 * Unlike the proprietary camera SDKs (ZWO/QHY/SVBONY/...), libgphoto2 has no
 * single global init/shutdown call; the shared state that must outlive every
 * open camera is the GPContext, held once by the singleton. Each opened
 * camera gets its own integer handle (mirroring the "camera_id" shape the
 * other camera wrappers use), so driver code looks the same across vendors.
 */
class GPhotoSDKWrapper final : public GPhotoSDK {
public:
    static GPhotoSDKWrapper& instance();

    std::vector<GPhotoCameraInfo> enumerate_cameras() override;

    int open_camera(const std::string& model, const std::string& port) override;

    void close_camera(int handle) override;

    std::string get_gphoto_version() override;
    std::string get_camera_summary(int handle) override;

    bool has_widget(int handle, const std::string& name) override;

    std::vector<std::string> get_choices(int handle, const std::string& name) override;
    std::string get_choice_value(int handle, const std::string& name) override;
    void set_choice_value(int handle, const std::string& name, const std::string& value) override;

    bool get_toggle_value(int handle, const std::string& name) override;
    void set_toggle_value(int handle, const std::string& name, bool on) override;

    std::string get_text_value(int handle, const std::string& name) override;

    GPhotoCaptureResult capture_and_download(int handle) override;

    void drain_events(int handle, std::chrono::milliseconds budget) override;

    std::optional<GPhotoCaptureResult> poll_bulb_file_and_download(int handle,
                                                                   std::chrono::milliseconds timeout) override;

private:
    GPhotoSDKWrapper();
    ~GPhotoSDKWrapper();
    GPhotoSDKWrapper(const GPhotoSDKWrapper&) = delete;
    GPhotoSDKWrapper& operator=(const GPhotoSDKWrapper&) = delete;

    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

}  // namespace alpacacore::vendor::gphoto
