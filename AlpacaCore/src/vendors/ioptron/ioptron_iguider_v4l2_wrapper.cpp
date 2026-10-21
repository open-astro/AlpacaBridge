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

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/serial_io.h>
#include <alpacacore/vendor/ioptron/ioptron_iguider_v4l2_wrapper.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <string>

namespace alpacacore::vendor::ioptron {

namespace {

constexpr const char* kLog = "iGuider";
constexpr unsigned kBufferCount = 4;
// The highest /dev/videoN enumerate() looks at.
constexpr int kMaxVideoNodes = 64;

int xioctl(int fd, unsigned long request, void* arg) {
    int r = 0;
    do {
        r = ::ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

// These errno values on the fd mean the camera is gone (decision 0009, point 2a).
bool camera_gone(int err) { return err == EIO || err == ENXIO || err == ENODEV || err == EBADF; }

[[noreturn]] void throw_errno(const std::string& what, int err) {
    const std::string message = "iGuider " + what + " failed: " + util::errno_string(err);
    if (camera_gone(err)) throw IGuiderDeviceGone(message, AlpacaError::DriverException);
    throw AlpacaException(message, AlpacaError::DriverException);
}

bool contains_iguider(const char* card) {
    std::string lower(card);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("iguider") != std::string::npos;
}

std::uint32_t device_caps(const v4l2_capability& cap) {
    return (cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0 ? cap.device_caps : cap.capabilities;
}

bool can_capture_and_stream(const v4l2_capability& cap) {
    const std::uint32_t caps = device_caps(cap);
    return (caps & V4L2_CAP_VIDEO_CAPTURE) != 0 && (caps & V4L2_CAP_STREAMING) != 0;
}

std::uint32_t control_id(IGuiderControl control) {
    switch (control) {
        case IGuiderControl::AutoExposure:
            return V4L2_CID_EXPOSURE_AUTO;
        case IGuiderControl::ExposureAbsolute:
            return V4L2_CID_EXPOSURE_ABSOLUTE;
        case IGuiderControl::Gain:
            return V4L2_CID_GAIN;
    }
    return V4L2_CID_GAIN;
}

const char* control_name(IGuiderControl control) {
    switch (control) {
        case IGuiderControl::AutoExposure:
            return "auto_exposure";
        case IGuiderControl::ExposureAbsolute:
            return "exposure_time_absolute";
        case IGuiderControl::Gain:
            return "gain";
    }
    return "control";
}

}  // namespace

struct IGuiderV4L2Wrapper::Device {
    struct Buffer {
        void* start = MAP_FAILED;
        std::size_t length = 0;
    };

    std::mutex mutex;  // serialises this device's calls
    int fd = -1;
    std::string path;
    bool streaming = false;
    int width = 0;
    int height = 0;
    std::vector<Buffer> buffers;

    void release_buffers() {
        for (auto& buffer : buffers) {
            if (buffer.start != MAP_FAILED) ::munmap(buffer.start, buffer.length);
        }
        buffers.clear();
        v4l2_requestbuffers req{};
        req.count = 0;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        static_cast<void>(xioctl(fd, VIDIOC_REQBUFS, &req));
    }

    // Caller holds mutex. Never throws: it runs on the close and failure paths.
    void stop_locked() {
        if (streaming) {
            int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (xioctl(fd, VIDIOC_STREAMOFF, &type) == -1) {
                ALPACA_LOG_DEBUG(kLog, "VIDIOC_STREAMOFF failed on " + path + ": " + util::errno_string(errno));
            }
            streaming = false;
        }
        if (!buffers.empty()) release_buffers();
    }
};

IGuiderV4L2Wrapper& IGuiderV4L2Wrapper::instance() {
    static IGuiderV4L2Wrapper wrapper;
    return wrapper;
}

IGuiderV4L2Wrapper::~IGuiderV4L2Wrapper() {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    for (auto& [fd, device] : devices_) {
        std::lock_guard<std::mutex> device_lock(device->mutex);
        device->stop_locked();
        ::close(fd);
    }
    devices_.clear();
}

std::shared_ptr<IGuiderV4L2Wrapper::Device> IGuiderV4L2Wrapper::find(int handle) const {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    const auto it = devices_.find(handle);
    if (it == devices_.end()) {
        throw AlpacaException("iGuider handle is not open", AlpacaError::DriverException);
    }
    return it->second;
}

std::vector<IGuiderDeviceInfo> IGuiderV4L2Wrapper::enumerate() {
    std::vector<IGuiderDeviceInfo> found;
    for (int n = 0; n < kMaxVideoNodes; ++n) {
        const std::string path = "/dev/video" + std::to_string(n);
        const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        v4l2_capability cap{};
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 && can_capture_and_stream(cap) &&
            contains_iguider(reinterpret_cast<const char*>(cap.card))) {
            found.push_back(
                {path, reinterpret_cast<const char*>(cap.card), reinterpret_cast<const char*>(cap.bus_info)});
        }
        ::close(fd);
    }
    return found;
}

int IGuiderV4L2Wrapper::open(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) throw_errno("open of " + path, errno);
    v4l2_capability cap{};
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == -1) {
        const int err = errno;
        ::close(fd);
        throw_errno("VIDIOC_QUERYCAP on " + path, err);
    }
    if (!can_capture_and_stream(cap)) {
        ::close(fd);
        throw AlpacaException(path + " is not a V4L2 streaming capture device", AlpacaError::DriverException);
    }
    // A pinned node can be re-pointed at another UVC camera by a replug
    // (decision 0009, point 6): refuse it here, by name, not at the first frame.
    if (!contains_iguider(reinterpret_cast<const char*>(cap.card))) {
        const std::string card(reinterpret_cast<const char*>(cap.card));
        ::close(fd);
        throw AlpacaException(path + " is not an iOptron iGuider (V4L2 card \"" + card +
                                  "\"); check the device path in the web UI, or leave it blank to scan",
                              AlpacaError::DriverException);
    }
    auto device = std::make_shared<Device>();
    device->fd = fd;
    device->path = path;
    std::lock_guard<std::mutex> lock(devices_mutex_);
    devices_[fd] = std::move(device);
    return fd;
}

void IGuiderV4L2Wrapper::close(int handle) {
    std::shared_ptr<Device> device;
    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        const auto it = devices_.find(handle);
        if (it == devices_.end()) return;
        device = std::move(it->second);
        devices_.erase(it);
    }
    std::lock_guard<std::mutex> device_lock(device->mutex);
    device->stop_locked();
    ::close(device->fd);
    device->fd = -1;
}

void IGuiderV4L2Wrapper::set_control(int handle, IGuiderControl control, std::int32_t value) {
    const auto device = find(handle);
    std::lock_guard<std::mutex> lock(device->mutex);
    v4l2_control ctrl{};
    ctrl.id = control_id(control);
    ctrl.value = value;
    if (xioctl(device->fd, VIDIOC_S_CTRL, &ctrl) == -1) {
        throw_errno(std::string("setting ") + control_name(control) + "=" + std::to_string(value), errno);
    }
}

void IGuiderV4L2Wrapper::start_stream(int handle, int width, int height) {
    const auto device = find(handle);
    std::lock_guard<std::mutex> lock(device->mutex);
    device->stop_locked();

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = static_cast<std::uint32_t>(width);
    fmt.fmt.pix.height = static_cast<std::uint32_t>(height);
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(device->fd, VIDIOC_S_FMT, &fmt) == -1) throw_errno("VIDIOC_S_FMT", errno);
    // The driver may pick the nearest mode; anything but the exact one would
    // make every frame the wrong size, so refuse it here.
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV || fmt.fmt.pix.width != static_cast<std::uint32_t>(width) ||
        fmt.fmt.pix.height != static_cast<std::uint32_t>(height)) {
        throw AlpacaException("iGuider did not accept YUYV " + std::to_string(width) + "x" + std::to_string(height) +
                                  " (got " + std::to_string(fmt.fmt.pix.width) + "x" +
                                  std::to_string(fmt.fmt.pix.height) + ")",
                              AlpacaError::DriverException);
    }

    v4l2_requestbuffers req{};
    req.count = kBufferCount;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(device->fd, VIDIOC_REQBUFS, &req) == -1) throw_errno("VIDIOC_REQBUFS", errno);
    if (req.count < 2) {
        device->release_buffers();
        throw AlpacaException("iGuider granted too few capture buffers", AlpacaError::DriverException);
    }

    try {
        device->buffers.resize(req.count);
        for (unsigned i = 0; i < req.count; ++i) {
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (xioctl(device->fd, VIDIOC_QUERYBUF, &buf) == -1) throw_errno("VIDIOC_QUERYBUF", errno);
            void* start = ::mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, device->fd, buf.m.offset);
            if (start == MAP_FAILED) throw_errno("mmap of a capture buffer", errno);
            device->buffers[i] = {start, buf.length};
            if (xioctl(device->fd, VIDIOC_QBUF, &buf) == -1) throw_errno("VIDIOC_QBUF", errno);
        }
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(device->fd, VIDIOC_STREAMON, &type) == -1) throw_errno("VIDIOC_STREAMON", errno);
    } catch (...) {
        device->release_buffers();
        throw;
    }
    device->streaming = true;
    device->width = width;
    device->height = height;
}

void IGuiderV4L2Wrapper::stop_stream(int handle) {
    const auto device = find(handle);
    std::lock_guard<std::mutex> lock(device->mutex);
    device->stop_locked();
}

bool IGuiderV4L2Wrapper::read_frame(int handle, std::vector<std::uint8_t>& luma, std::chrono::milliseconds timeout) {
    const auto device = find(handle);
    std::lock_guard<std::mutex> lock(device->mutex);
    if (!device->streaming) {
        throw AlpacaException("iGuider is not streaming", AlpacaError::DriverException);
    }

    pollfd pfd{device->fd, POLLIN, 0};
    int ready = 0;
    do {
        ready = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
    } while (ready == -1 && errno == EINTR);
    if (ready == -1) throw_errno("poll", errno);
    if (ready == 0) return false;
    const bool poll_error = (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 && (pfd.revents & POLLIN) == 0;
    if (poll_error) {
        // uvcvideo reports a removed camera this way; the dequeue below names it.
        ALPACA_LOG_DEBUG(kLog, "poll revents " + std::to_string(pfd.revents) + " on " + device->path);
    }

    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(device->fd, VIDIOC_DQBUF, &buf) == -1) {
        if (errno != EAGAIN) throw_errno("VIDIOC_DQBUF", errno);
        // An error with nothing to dequeue would return at once on every call:
        // fail instead, so the caller latches a fault rather than spinning.
        if (poll_error) {
            throw AlpacaException("iGuider poll reported an error with no frame ready", AlpacaError::DriverException);
        }
        return false;
    }

    const std::size_t expected = static_cast<std::size_t>(device->width) * static_cast<std::size_t>(device->height) * 2;
    bool complete = (buf.flags & V4L2_BUF_FLAG_ERROR) == 0 && buf.index < device->buffers.size() &&
                    buf.bytesused == expected && device->buffers[buf.index].length >= expected;
    if (complete) {
        const auto* yuyv = static_cast<const std::uint8_t*>(device->buffers[buf.index].start);
        luma.resize(expected / 2);
        for (std::size_t i = 0; i < luma.size(); ++i) luma[i] = yuyv[i * 2];
    } else {
        ALPACA_LOG_DEBUG(kLog, "Dropped an incomplete frame (" + std::to_string(buf.bytesused) + " of " +
                                   std::to_string(expected) + " bytes, flags " + std::to_string(buf.flags) + ")");
    }
    if (xioctl(device->fd, VIDIOC_QBUF, &buf) == -1) throw_errno("VIDIOC_QBUF", errno);
    return complete;
}

}  // namespace alpacacore::vendor::ioptron
