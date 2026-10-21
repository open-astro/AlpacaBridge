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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/gphoto/gphoto_sdk_wrapper.h>
#include <gphoto2/gphoto2-camera.h>
#include <gphoto2/gphoto2-context.h>
#include <gphoto2/gphoto2-file.h>
#include <gphoto2/gphoto2-list.h>
#include <gphoto2/gphoto2-version.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace alpacacore::vendor::gphoto {

namespace {

constexpr const char* kLogTag = "GPhoto";

[[noreturn]] void throw_gp_error(const std::string& what, int gp_result) {
    // The only result recorded on a rig when a body powered itself off between exposures (issue #1064).
    if (gp_result == GP_ERROR_IO_USB_FIND) {
        throw GPhotoDeviceGoneException(what + ": " + gp_result_as_string(gp_result), AlpacaError::DriverException);
    }
    throw AlpacaException(what + ": " + gp_result_as_string(gp_result), AlpacaError::DriverException);
}

int clamp_to_timeout_ms(std::chrono::steady_clock::duration remaining) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
    if (ms <= 0) return 0;
    if (ms > std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
    return static_cast<int>(ms);
}

}  // namespace

class GPhotoSDKWrapper::Impl {
public:
    Impl() : context_(gp_context_new()) {
        if (context_ == nullptr) {
            throw AlpacaException("Failed to create libgphoto2 context", AlpacaError::DriverException);
        }
    }

    ~Impl() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, session] : sessions_) {
            (void)id;
            close_session_locked(session);
        }
        sessions_.clear();
        gp_context_unref(context_);
    }

    struct Session {
        Camera* camera{nullptr};
    };

    std::vector<GPhotoCameraInfo> enumerate_cameras() {
        CameraList* list = nullptr;
        int result = gp_list_new(&list);
        if (result != GP_OK) {
            throw_gp_error("gp_list_new failed", result);
        }

        // gp_camera_autodetect returns the number of cameras found (>= 0) on
        // success, not GP_OK -- e.g. exactly one camera makes it return 1,
        // which is not GP_OK. Only a negative result is a real GP_ERROR_*.
        result = gp_camera_autodetect(list, context_);
        if (result < 0) {
            gp_list_free(list);
            throw_gp_error("gp_camera_autodetect failed", result);
        }

        std::vector<GPhotoCameraInfo> cameras;
        int count = gp_list_count(list);
        for (int i = 0; i < count; ++i) {
            const char* name = nullptr;
            const char* port = nullptr;
            gp_list_get_name(list, i, &name);
            gp_list_get_value(list, i, &port);
            cameras.push_back({name ? name : "", port ? port : ""});
        }
        gp_list_free(list);
        return cameras;
    }

    int open_camera(const std::string& model, const std::string& port) {
        Camera* camera = nullptr;
        int result = gp_camera_new(&camera);
        if (result != GP_OK) {
            throw_gp_error("gp_camera_new failed", result);
        }

        // Explicit model+port init (rather than a bare gp_camera_init auto
        // probe) so opening one detected camera never races another
        // USB-attached camera's enumeration -- same rationale INDI's
        // gphoto_driver.cpp documents for its default path.
        CameraAbilitiesList* abilities_list = nullptr;
        GPPortInfoList* port_info_list = nullptr;
        try {
            result = gp_abilities_list_new(&abilities_list);
            if (result != GP_OK) throw_gp_error("gp_abilities_list_new failed", result);
            result = gp_abilities_list_load(abilities_list, context_);
            if (result != GP_OK) throw_gp_error("gp_abilities_list_load failed", result);

            int model_index = gp_abilities_list_lookup_model(abilities_list, model.c_str());
            if (model_index < GP_OK) {
                throw AlpacaException("Camera model not recognized by libgphoto2: " + model, AlpacaError::InvalidValue);
            }
            CameraAbilities abilities;
            result = gp_abilities_list_get_abilities(abilities_list, model_index, &abilities);
            if (result != GP_OK) throw_gp_error("gp_abilities_list_get_abilities failed", result);
            result = gp_camera_set_abilities(camera, abilities);
            if (result != GP_OK) throw_gp_error("gp_camera_set_abilities failed", result);

            result = gp_port_info_list_new(&port_info_list);
            if (result != GP_OK) throw_gp_error("gp_port_info_list_new failed", result);
            result = gp_port_info_list_load(port_info_list);
            if (result != GP_OK) throw_gp_error("gp_port_info_list_load failed", result);

            int port_index = gp_port_info_list_lookup_path(port_info_list, port.c_str());
            if (port_index < GP_OK) {
                throw AlpacaException("Camera port not found: " + port, AlpacaError::NotConnected);
            }
            GPPortInfo port_info;
            result = gp_port_info_list_get_info(port_info_list, port_index, &port_info);
            if (result != GP_OK) throw_gp_error("gp_port_info_list_get_info failed", result);
            result = gp_camera_set_port_info(camera, port_info);
            if (result != GP_OK) throw_gp_error("gp_camera_set_port_info failed", result);

            result = gp_camera_init(camera, context_);
            if (result != GP_OK) {
                throw_gp_error("gp_camera_init failed (camera not connected or busy)", result);
            }
        } catch (...) {
            if (port_info_list) gp_port_info_list_free(port_info_list);
            if (abilities_list) gp_abilities_list_free(abilities_list);
            gp_camera_unref(camera);
            throw;
        }
        gp_port_info_list_free(port_info_list);
        gp_abilities_list_free(abilities_list);

        std::lock_guard<std::mutex> lock(mutex_);
        int handle = next_handle_++;
        sessions_[handle] = Session{camera};
        return handle;
    }

    void close_camera(int handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(handle);
        if (it == sessions_.end()) {
            return;
        }
        close_session_locked(it->second);
        sessions_.erase(it);
    }

    std::string get_gphoto_version() {
        const char** info = gp_library_version(GP_VERSION_SHORT);
        if (info == nullptr || info[0] == nullptr) {
            return "unknown";
        }
        return info[0];
    }

    std::string get_camera_summary(int handle) {
        Camera* camera = camera_for(handle);
        CameraText text;
        int result = gp_camera_get_summary(camera, &text, context_);
        if (result != GP_OK) {
            throw_gp_error("gp_camera_get_summary failed", result);
        }
        return text.text;
    }

    bool has_widget(int handle, const std::string& name) {
        Camera* camera = camera_for(handle);
        CameraWidget* widget = get_single_widget_or_null(camera, name);
        if (widget == nullptr) {
            return false;
        }
        gp_widget_free(widget);
        return true;
    }

    std::vector<std::string> get_choices(int handle, const std::string& name) {
        Camera* camera = camera_for(handle);
        CameraWidget* widget = get_single_widget_or_null(camera, name);
        std::vector<std::string> choices;
        if (widget != nullptr) {
            int count = gp_widget_count_choices(widget);
            for (int i = 0; i < count; ++i) {
                const char* choice = nullptr;
                if (gp_widget_get_choice(widget, i, &choice) == GP_OK && choice != nullptr) {
                    choices.emplace_back(choice);
                }
            }
            gp_widget_free(widget);
        }
        return choices;
    }

    std::string get_choice_value(int handle, const std::string& name) { return get_string_value(handle, name); }

    void set_choice_value(int handle, const std::string& name, const std::string& value) {
        Camera* camera = camera_for(handle);
        CameraWidget* widget = get_single_widget_or_throw(camera, name);
        int result = gp_widget_set_value(widget, value.c_str());
        if (result == GP_OK) {
            result = gp_camera_set_single_config(camera, name.c_str(), widget, context_);
        }
        gp_widget_free(widget);
        if (result != GP_OK) {
            throw_gp_error("Failed to set " + name + " = " + value, result);
        }
    }

    bool get_toggle_value(int handle, const std::string& name) {
        Camera* camera = camera_for(handle);
        CameraWidget* widget = get_single_widget_or_throw(camera, name);
        int toggle_value = 0;
        int result = gp_widget_get_value(widget, &toggle_value);
        gp_widget_free(widget);
        if (result != GP_OK) {
            throw_gp_error("gp_widget_get_value failed for " + name, result);
        }
        return toggle_value != 0;
    }

    void set_toggle_value(int handle, const std::string& name, bool on) {
        Camera* camera = camera_for(handle);
        CameraWidget* widget = get_single_widget_or_throw(camera, name);
        int toggle_value = on ? 1 : 0;
        int result = gp_widget_set_value(widget, &toggle_value);
        if (result == GP_OK) {
            result = gp_camera_set_single_config(camera, name.c_str(), widget, context_);
        }
        gp_widget_free(widget);
        if (result != GP_OK) {
            throw_gp_error(std::string("Failed to set ") + name + (on ? "=1" : "=0"), result);
        }
    }

    std::string get_text_value(int handle, const std::string& name) { return get_string_value(handle, name); }

    GPhotoCaptureResult capture_and_download(int handle) {
        Camera* camera = camera_for(handle);
        CameraFilePath path;
        int result = gp_camera_capture(camera, GP_CAPTURE_IMAGE, &path, context_);
        if (result != GP_OK) {
            throw_gp_error("gp_camera_capture failed", result);
        }
        return download_and_delete(camera, path);
    }

    // libgphoto2 PTP bulb protocol (the ptp2.so camlib exposes it the same
    // way for Canon and Nikon): the caller flips the "bulb" toggle widget on
    // to open the shutter, holds for the requested duration while pumping
    // the event queue through drain_events() below (see
    // gphoto_camera_driver.cpp's abortable hold loop), flips it off to
    // close, and then polls here for the GP_EVENT_FILE_ADDED the camera
    // posts once it has written the frame. Validated on a Nikon D3300 with
    // libgphoto2 2.5.31 (issue #569): 60 s and 300 s bulb frames through
    // this exact sequence.
    void drain_events(int handle, std::chrono::milliseconds budget) {
        Camera* camera = camera_for(handle);
        const auto deadline = std::chrono::steady_clock::now() + budget;
        // Poll at least once even for a zero budget so a caller can flush
        // whatever is already queued without waiting.
        do {
            const auto now = std::chrono::steady_clock::now();
            CameraEventType event_type = GP_EVENT_UNKNOWN;
            void* event_data = nullptr;
            int result = gp_camera_wait_for_event(camera, clamp_to_timeout_ms(deadline - now), &event_type, &event_data,
                                                  context_);
            if (result != GP_OK) {
                // Never throw from the hold loop: the shutter is open and a
                // throw here would leave nobody to close it. Sleep out the
                // rest of the slice so the caller's timing still holds.
                if (event_data != nullptr) free(event_data);
                ALPACA_LOG_DEBUG(kLogTag, std::string("Event poll during bulb hold failed (ignored): ") +
                                              gp_result_as_string(result));
                const auto remaining = deadline - std::chrono::steady_clock::now();
                if (remaining > std::chrono::steady_clock::duration::zero()) {
                    std::this_thread::sleep_for(remaining);
                }
                return;
            }
            if (event_type == GP_EVENT_FILE_ADDED && event_data != nullptr) {
                // The shutter is open, so this exposure cannot have produced
                // a file yet: this is a frame from an earlier exposure whose
                // wait gave up (or that was aborted) before the camera
                // finished writing it. Leaving it queued would hand it to
                // the NEXT poll_bulb_file_and_download() as this exposure's
                // frame, so consume it here and delete it from the camera.
                auto* path = static_cast<CameraFilePath*>(event_data);
                ALPACA_LOG_DEBUG(kLogTag, std::string("Stale file-added event during bulb hold, discarding: ") +
                                              path->folder + "/" + path->name);
                int delete_result = gp_camera_file_delete(camera, path->folder, path->name, context_);
                if (delete_result != GP_OK) {
                    ALPACA_LOG_DEBUG(kLogTag, std::string("Failed to delete stale file from camera: ") +
                                                  gp_result_as_string(delete_result));
                }
            }
            if (event_data != nullptr) {
                free(event_data);
            }
        } while (std::chrono::steady_clock::now() < deadline);
    }

    std::optional<GPhotoCaptureResult> poll_bulb_file_and_download(int handle, std::chrono::milliseconds timeout) {
        Camera* camera = camera_for(handle);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            const auto now = std::chrono::steady_clock::now();
            CameraEventType event_type = GP_EVENT_UNKNOWN;
            void* event_data = nullptr;
            int result = gp_camera_wait_for_event(camera, clamp_to_timeout_ms(deadline - now), &event_type, &event_data,
                                                  context_);
            if (result != GP_OK) {
                if (event_data != nullptr) free(event_data);
                throw_gp_error("gp_camera_wait_for_event failed while waiting for the bulb frame", result);
            }
            if (event_type == GP_EVENT_FILE_ADDED && event_data != nullptr) {
                CameraFilePath local_path = *static_cast<CameraFilePath*>(event_data);
                free(event_data);
                return download_and_delete(camera, local_path);
            }
            if (event_data != nullptr) {
                free(event_data);
            }
        } while (std::chrono::steady_clock::now() < deadline);
        return std::nullopt;
    }

private:
    GPContext* context_;
    std::mutex mutex_;
    std::unordered_map<int, Session> sessions_;
    int next_handle_{0};

    // mutex_ only guards the sessions_ map (open/close/lookup bookkeeping).
    // Concurrent libgphoto2 calls against the SAME handle are not serialized
    // here -- exactly like the other camera SDK wrappers (SVBONY, ZWO, ...),
    // it is the owning driver's own mutex_ that guarantees a single caller
    // per handle at a time.
    Camera* camera_for(int handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(handle);
        if (it == sessions_.end()) {
            throw AlpacaException("Unknown gphoto camera handle", AlpacaError::NotConnected);
        }
        return it->second.camera;
    }

    void close_session_locked(Session& session) {
        if (session.camera == nullptr) {
            return;
        }
        gp_camera_exit(session.camera, context_);
        gp_camera_unref(session.camera);
        session.camera = nullptr;
    }

    // One widget by name through libgphoto2's single-config API
    // (gp_camera_get_single_config / gp_camera_set_single_config, available
    // since libgphoto2 2.5.10). The previous shape fetched the WHOLE config
    // tree with gp_camera_get_config, looked one widget up in it, and for a
    // setter wrote the whole tree back with gp_camera_set_config. On a
    // Nikon D3300 that full walk fails while the body is busy -- mid-bulb,
    // or right after a capture it refused -- so the toggle that closes a
    // bulb shutter reported "Unspecified error", the capture was never
    // terminated and the camera's PTP stack hung until a power cycle, and
    // a session whose priming capture had failed reported "shutterspeed2"
    // absent from a tree the walk had silently truncated. The gphoto2 CLI's
    // --set-config takes this single-widget path and the same bulb sequence
    // succeeded on the same body every time (issue #569,
    // docs/failures/0009-gphoto-nikon-bulb-full-config-walk.md).
    //
    // ptp2 answers GP_ERROR_BAD_PARAMETERS for a name it has no widget for
    // (camlibs/ptp2/config.c, MODE_SINGLE_GET); anything else is a real
    // failure and throws, so a busy camera reads as an error rather than as
    // a missing widget. The returned widget is owned by the caller
    // (gp_widget_free).
    CameraWidget* get_single_widget_or_null(Camera* camera, const std::string& name) {
        CameraWidget* widget = nullptr;
        int result = gp_camera_get_single_config(camera, name.c_str(), &widget, context_);
        if (result == GP_ERROR_BAD_PARAMETERS || result == GP_ERROR_NOT_SUPPORTED) {
            return nullptr;
        }
        if (result != GP_OK) {
            throw_gp_error("gp_camera_get_single_config failed for " + name, result);
        }
        return widget;
    }

    CameraWidget* get_single_widget_or_throw(Camera* camera, const std::string& name) {
        CameraWidget* widget = get_single_widget_or_null(camera, name);
        if (widget == nullptr) {
            throw AlpacaException("Widget not present: " + name, AlpacaError::PropertyNotImplemented);
        }
        return widget;
    }

    // Shared body of get_choice_value / get_text_value: both are string-valued
    // widgets to libgphoto2 (radio/menu and text).
    std::string get_string_value(int handle, const std::string& name) {
        Camera* camera = camera_for(handle);
        CameraWidget* widget = get_single_widget_or_throw(camera, name);
        char* value = nullptr;
        // libgphoto2's C API takes an untyped void* out-param whose pointee
        // type depends on the widget type; for a text/radio/menu widget it
        // writes a `const char*` through it, so a char** here is required,
        // not a mistake.
        int result = gp_widget_get_value(widget, &value);  // NOLINT(bugprone-multi-level-implicit-pointer-conversion)
        std::string out = (result == GP_OK && value != nullptr) ? value : "";
        gp_widget_free(widget);
        if (result != GP_OK) {
            throw_gp_error("gp_widget_get_value failed for " + name, result);
        }
        return out;
    }

    GPhotoCaptureResult download_and_delete(Camera* camera, const CameraFilePath& path) {
        CameraFile* file = nullptr;
        int result = gp_file_new(&file);
        if (result != GP_OK) {
            throw_gp_error("gp_file_new failed", result);
        }

        result = gp_camera_file_get(camera, path.folder, path.name, GP_FILE_TYPE_NORMAL, file, context_);
        if (result != GP_OK) {
            gp_file_unref(file);
            throw_gp_error("gp_camera_file_get failed", result);
        }

        const char* data = nullptr;
        unsigned long size = 0;
        result = gp_file_get_data_and_size(file, &data, &size);
        if (result != GP_OK) {
            gp_file_unref(file);
            throw_gp_error("gp_file_get_data_and_size failed", result);
        }

        GPhotoCaptureResult out;
        out.filename = path.name;
        out.data.assign(data, data + size);
        const char* mime_type = nullptr;
        if (gp_file_get_mime_type(file, &mime_type) == GP_OK && mime_type != nullptr) {
            out.mime_type = mime_type;
        }
        gp_file_unref(file);

        // Best-effort: leaving frames on the card is harmless but fills it
        // over a long session. Never fail the capture over cleanup.
        int delete_result = gp_camera_file_delete(camera, path.folder, path.name, context_);
        if (delete_result != GP_OK) {
            ALPACA_LOG_DEBUG(kLogTag, std::string("Failed to delete captured file from camera: ") +
                                          gp_result_as_string(delete_result));
        }

        return out;
    }
};

GPhotoSDKWrapper::GPhotoSDKWrapper() : pimpl_(std::make_unique<Impl>()) {}
GPhotoSDKWrapper::~GPhotoSDKWrapper() = default;

GPhotoSDKWrapper& GPhotoSDKWrapper::instance() {
    static GPhotoSDKWrapper wrapper;
    return wrapper;
}

std::vector<GPhotoCameraInfo> GPhotoSDKWrapper::enumerate_cameras() { return pimpl_->enumerate_cameras(); }

int GPhotoSDKWrapper::open_camera(const std::string& model, const std::string& port) {
    return pimpl_->open_camera(model, port);
}

void GPhotoSDKWrapper::close_camera(int handle) { pimpl_->close_camera(handle); }

std::string GPhotoSDKWrapper::get_gphoto_version() { return pimpl_->get_gphoto_version(); }

std::string GPhotoSDKWrapper::get_camera_summary(int handle) { return pimpl_->get_camera_summary(handle); }

bool GPhotoSDKWrapper::has_widget(int handle, const std::string& name) { return pimpl_->has_widget(handle, name); }

std::vector<std::string> GPhotoSDKWrapper::get_choices(int handle, const std::string& name) {
    return pimpl_->get_choices(handle, name);
}

std::string GPhotoSDKWrapper::get_choice_value(int handle, const std::string& name) {
    return pimpl_->get_choice_value(handle, name);
}

void GPhotoSDKWrapper::set_choice_value(int handle, const std::string& name, const std::string& value) {
    pimpl_->set_choice_value(handle, name, value);
}

bool GPhotoSDKWrapper::get_toggle_value(int handle, const std::string& name) {
    return pimpl_->get_toggle_value(handle, name);
}

void GPhotoSDKWrapper::set_toggle_value(int handle, const std::string& name, bool on) {
    pimpl_->set_toggle_value(handle, name, on);
}

std::string GPhotoSDKWrapper::get_text_value(int handle, const std::string& name) {
    return pimpl_->get_text_value(handle, name);
}

GPhotoCaptureResult GPhotoSDKWrapper::capture_and_download(int handle) { return pimpl_->capture_and_download(handle); }

void GPhotoSDKWrapper::drain_events(int handle, std::chrono::milliseconds budget) {
    pimpl_->drain_events(handle, budget);
}

std::optional<GPhotoCaptureResult> GPhotoSDKWrapper::poll_bulb_file_and_download(int handle,
                                                                                 std::chrono::milliseconds timeout) {
    return pimpl_->poll_bulb_file_and_download(handle, timeout);
}

}  // namespace alpacacore::vendor::gphoto
