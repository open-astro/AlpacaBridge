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
#include <alpacacore/video/ser_writer.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace alpacacore::video {

namespace {

constexpr std::size_t kSerHeaderBytes = 178;
constexpr std::size_t kFrameCountOffset = 38;
constexpr std::size_t kDirectAlignment = 4096;
// .NET ticks (100 ns) between 0001-01-01 and 1970-01-01.
constexpr std::int64_t kUnixEpochTicks = 621355968000000000LL;

void throw_errno(const std::string& context) {
    throw AlpacaException(context + ": " + std::strerror(errno), AlpacaError::DriverException);
}

int open_output(const std::string& path) {
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef __linux__
    flags |= O_DIRECT;
#endif
    int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) {
        throw_errno("SerWriter open '" + path + "'");
    }
#ifdef __APPLE__
    // macOS dev hosts: no O_DIRECT; F_NOCACHE is the equivalent
    // don't-pollute-the-page-cache hint.
    ::fcntl(fd, F_NOCACHE, 1);
#endif
    return fd;
}

// Drop O_DIRECT so unaligned tail writes (trailer, header patch) work.
void clear_direct(int fd) {
#ifdef __linux__
    int flags = ::fcntl(fd, F_GETFL);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags & ~O_DIRECT);
    }
#else
    (void)fd;
#endif
}

void put_i32(std::uint8_t* dst, std::int32_t value) {
    // SER multi-byte fields are little-endian on every real-world writer.
    dst[0] = static_cast<std::uint8_t>(value & 0xFF);
    dst[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    dst[2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    dst[3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
}

void put_i64(std::uint8_t* dst, std::int64_t value) {
    for (int i = 0; i < 8; ++i) {
        dst[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
    }
}

void put_padded_string(std::uint8_t* dst, const std::string& value, std::size_t field_bytes) {
    std::memset(dst, 0, field_bytes);
    std::memcpy(dst, value.data(), std::min(value.size(), field_bytes));
}

std::vector<std::uint8_t> build_header(const SerWriter::Options& options, std::int64_t start_ticks) {
    std::vector<std::uint8_t> header(kSerHeaderBytes, 0);
    std::memcpy(header.data(), "LUCAM-RECORDER", 14);
    put_i32(header.data() + 14, 0);                             // LuID
    put_i32(header.data() + 18, ser_color_id(options.format));  // ColorID
    // The SER "LittleEndian" flag is historically inverted: every mainstream
    // writer (FireCapture, SharpCap) stores 16-bit data little-endian with
    // this field 0, and readers (SER Player, PIPP) treat 0 as little-endian.
    put_i32(header.data() + 22, 0);
    put_i32(header.data() + 26, options.width);
    put_i32(header.data() + 30, options.height);
    put_i32(header.data() + 34, bits_per_plane(options.format));
    put_i32(header.data() + kFrameCountOffset, 0);  // patched at finalize
    put_padded_string(header.data() + 42, options.observer, 40);
    put_padded_string(header.data() + 82, options.instrument, 40);
    put_padded_string(header.data() + 122, options.telescope, 40);
    // DateTime (local) and DateTimeUTC: both stamped with the UTC start
    // time — capture boxes run on UTC and a synthesized local time would be
    // less honest than a repeated UTC value.
    put_i64(header.data() + 162, start_ticks);
    put_i64(header.data() + 170, start_ticks);
    return header;
}

}  // namespace

std::int64_t utc_ticks_from_unix_nanos(std::int64_t unix_nanos) { return kUnixEpochTicks + unix_nanos / 100; }

std::int64_t utc_ticks_now() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return utc_ticks_from_unix_nanos(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

SerWriter::SerWriter(const Options& options) : options_(options) {
    VideoRequest geometry;
    geometry.width = options.width;
    geometry.height = options.height;
    geometry.format = options.format;
    frame_size_ = frame_bytes(geometry);
    if (frame_size_ == 0) {
        throw AlpacaException("SerWriter: zero-sized frame geometry", AlpacaError::InvalidValue);
    }

    staging_capacity_ = std::max(options.staging_bytes, kDirectAlignment * 2);
    staging_capacity_ = (staging_capacity_ / kDirectAlignment) * kDirectAlignment;
    void* raw = nullptr;
    if (::posix_memalign(&raw, kDirectAlignment, staging_capacity_) != 0) {
        throw AlpacaException("SerWriter: staging allocation failed", AlpacaError::DriverException);
    }
    staging_ = static_cast<std::uint8_t*>(raw);

    fd_ = open_output(options.path);

    const auto header = build_header(options_, utc_ticks_now());
    std::memcpy(staging_, header.data(), header.size());
    staging_used_ = header.size();
}

SerWriter::~SerWriter() {
    try {
        finalize();
    } catch (...) {
        // Destructor must not throw; finalize() is the reporting path.
    }
    std::free(staging_);
}

void SerWriter::write_all_direct(const std::uint8_t* data, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        const ssize_t n = ::write(fd_, data + written, size - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_errno("SerWriter write");
        }
        if (n == 0) {
            throw AlpacaException("SerWriter write returned 0", AlpacaError::DriverException);
        }
        written += static_cast<std::size_t>(n);
    }
    bytes_written_ += size;
}

void SerWriter::flush_full_blocks() {
    const std::size_t full = (staging_used_ / kDirectAlignment) * kDirectAlignment;
    if (full == 0) {
        return;
    }
    write_all_direct(staging_, full);
    const std::size_t remainder = staging_used_ - full;
    std::memmove(staging_, staging_ + full, remainder);
    staging_used_ = remainder;
}

void SerWriter::write_frame(const std::uint8_t* data, std::size_t size, std::int64_t timestamp_utc_ticks) {
    if (finalized_) {
        throw AlpacaException("SerWriter: write after finalize", AlpacaError::InvalidOperation);
    }
    if (size != frame_size_) {
        throw AlpacaException("SerWriter: frame size mismatch", AlpacaError::InvalidValue);
    }
    std::size_t copied = 0;
    while (copied < size) {
        const std::size_t chunk = std::min(size - copied, staging_capacity_ - staging_used_);
        std::memcpy(staging_ + staging_used_, data + copied, chunk);
        staging_used_ += chunk;
        copied += chunk;
        if (staging_used_ == staging_capacity_) {
            flush_full_blocks();
        }
    }
    timestamps_.push_back(timestamp_utc_ticks);
    ++frames_written_;
}

void SerWriter::finalize() {
    if (finalized_) {
        return;
    }
    finalized_ = true;
    if (fd_ < 0) {
        return;
    }

    flush_full_blocks();
    clear_direct(fd_);
    if (staging_used_ > 0) {
        write_all_direct(staging_, staging_used_);
        staging_used_ = 0;
    }

    // Timestamp trailer: one little-endian int64 of UTC ticks per frame.
    std::vector<std::uint8_t> trailer(timestamps_.size() * 8);
    for (std::size_t i = 0; i < timestamps_.size(); ++i) {
        put_i64(trailer.data() + i * 8, timestamps_[i]);
    }
    if (!trailer.empty()) {
        write_all_direct(trailer.data(), trailer.size());
    }

    std::uint8_t count[4];
    put_i32(count, static_cast<std::int32_t>(std::min<std::uint64_t>(frames_written_, INT32_MAX)));
    if (::pwrite(fd_, count, sizeof(count), kFrameCountOffset) != sizeof(count)) {
        const int close_errno = errno;
        ::close(fd_);
        fd_ = -1;
        errno = close_errno;
        throw_errno("SerWriter FrameCount patch");
    }

    if (::fsync(fd_) != 0) {
        ::close(fd_);
        fd_ = -1;
        throw_errno("SerWriter fsync");
    }
    if (::close(fd_) != 0) {
        fd_ = -1;
        throw_errno("SerWriter close");
    }
    fd_ = -1;
}

std::uint64_t measure_disk_write_rate(const std::string& directory, std::size_t test_bytes) {
    const std::string path = directory + "/.alpaca_disk_probe.tmp";
    try {
        const std::size_t chunk = 4ULL * 1024 * 1024;
        void* raw = nullptr;
        if (::posix_memalign(&raw, kDirectAlignment, chunk) != 0) {
            return 0;
        }
        std::unique_ptr<void, decltype(&std::free)> guard(raw, &std::free);
        std::memset(raw, 0x5A, chunk);

        int fd = open_output(path);
        const auto start = std::chrono::steady_clock::now();
        std::size_t written = 0;
        while (written < test_bytes) {
            const std::size_t n = std::min(chunk, test_bytes - written);
            // Aligned size: test_bytes callers pass multiples of 4 MiB.
            if (::write(fd, raw, n) != static_cast<ssize_t>(n)) {
                ::close(fd);
                ::unlink(path.c_str());
                return 0;
            }
            written += n;
        }
        ::fsync(fd);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        ::close(fd);
        ::unlink(path.c_str());
        const double seconds = std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();
        if (seconds <= 0) {
            return 0;
        }
        return static_cast<std::uint64_t>(static_cast<double>(written) / seconds);
    } catch (...) {
        ::unlink(path.c_str());
        return 0;
    }
}

}  // namespace alpacacore::video
