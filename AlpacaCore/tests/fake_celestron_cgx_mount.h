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

#ifndef _WIN32

#include <alpacacore/vendor/celestron/celestron_cgx_protocol.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "fake_pty_write.h"

namespace alpacacore::test {

// KNOWN PARITY GAPS: Opt-in auto-completion remains an instant-motion
// convenience for existing tests. Elapsed-time motion is explicit and uses a
// configured encoder-count rate; it does not claim the controller's real
// velocity-register encoding, acceleration profile, guide-rate scaling, or
// simultaneous-axis behavior. Home uses a fixed simulated physical origin and
// explicit completion; guide expiry still uses real time unless held by a test.
// Guide pulses do not advance encoders; the polarity case applies an explicit
// signed encoder step to check the hardware-verified direction convention.
// Stop-completion delay, label-readback transients and post-rate-off encoder drift are scripted fault hooks,
// not a simulation of the controller's deceleration or settling dynamics.
// Per-read RA drift is a legacy Sync fault hook, not the elapsed-time model.
// Hardware qualification must replace assumptions before they become claims.

/// Small stateful AUX endpoint for direct-USB protocol tests.
class FakeCelestronCgxMount {
public:
    FakeCelestronCgxMount() : pty_("FakeCelestronCgxMount"), worker_([this] { run(); }) {}
    ~FakeCelestronCgxMount() { stop_worker(); }

    FakeCelestronCgxMount(const FakeCelestronCgxMount&) = delete;
    FakeCelestronCgxMount& operator=(const FakeCelestronCgxMount&) = delete;

    bool ok() const { return pty_.master_fd() >= 0; }
    std::string slave_path() const { return pty_.slave_path(); }
    void corrupt_next_reply_checksum() { corrupt_next_checksum_.store(true); }
    void set_auto_complete_motions(bool enabled) { auto_complete_motions_.store(enabled); }
    void set_hold_guide_until_completed(bool hold) { hold_guide_until_completed_.store(hold); }
    void set_muted(bool muted) { muted_.store(muted); }
    void set_echo_requests(bool enabled) { echo_requests_.store(enabled); }
    void fragment_next_reply(std::optional<std::pair<std::uint8_t, std::uint8_t>> filter = std::nullopt) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        reply_fragment_waiting_ = false;
        release_reply_tail_ = false;
        fragment_filter_ = filter;
        fragment_replies_.store(true);
    }
    bool wait_for_reply_fragment(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(state_mutex_);
        return reply_cv_.wait_for(lock, timeout, [this] { return reply_fragment_waiting_; });
    }
    void release_reply_tail() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        release_reply_tail_ = true;
        reply_cv_.notify_all();
    }
    void set_cgx_switches_present(bool present) { cgx_switches_present_.store(present); }
    void set_stop_completion_reads(int reads) { stop_completion_reads_.store(reads); }
    void set_ra_stop_drift_reads(int reads) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ra_stop_drift_reads_ = reads;
    }
    void set_label_read_offsets(std::uint8_t axis, std::vector<std::int32_t> offsets) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        label_read_offsets_[axis_index(axis)] = std::move(offsets);
    }
    void set_motor_identity(std::uint8_t axis, std::vector<std::uint8_t> version, std::vector<std::uint8_t> model) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto index = axis_index(axis);
        motor_versions_[index] = std::move(version);
        motor_models_[index] = std::move(model);
    }
    void set_ra_tracking_drift_per_read(std::uint32_t counts) { ra_tracking_drift_per_read_.store(counts); }
    void set_axis_velocity_counts_per_second(std::uint8_t axis, std::uint32_t counts_per_second) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        axes_[axis_index(axis)].velocity_counts_per_second = counts_per_second;
    }
    void advance_motion(std::chrono::milliseconds elapsed) {
        if (elapsed < std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("CGX fake motion cannot advance by a negative duration");
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto milliseconds = static_cast<std::uint64_t>(elapsed.count());
        for (const AxisState& axis : axes_) {
            if (axis.velocity_counts_per_second != 0 &&
                milliseconds > (std::numeric_limits<std::uint64_t>::max() - axis.motion_remainder) /
                                   axis.velocity_counts_per_second) {
                throw std::invalid_argument("CGX fake motion duration overflows encoder arithmetic");
            }
        }
        for (AxisState& axis : axes_) {
            if (axis.homing || !(axis.slewing || axis.tracking || axis.manual_motion)) continue;
            const std::uint64_t numerator =
                static_cast<std::uint64_t>(axis.velocity_counts_per_second) * milliseconds + axis.motion_remainder;
            const std::uint64_t budget = numerator / 1000;
            axis.motion_remainder = numerator % 1000;
            if (axis.slewing) {
                const std::int32_t delta = signed_delta(axis.target, encoder_position(axis));
                const std::uint64_t distance = static_cast<std::uint64_t>(std::abs(delta));
                if (distance <= budget) {
                    axis.physical_position = (axis.physical_position + delta) & 0x00ffffffU;
                    axis.slewing = false;
                    axis.motion_remainder = 0;
                } else if (budget != 0) {
                    const auto step = static_cast<std::int32_t>(budget);
                    axis.physical_position = (axis.physical_position + (delta < 0 ? -step : step)) & 0x00ffffffU;
                }
            } else {
                const auto counts = static_cast<std::int64_t>(budget & 0x00ffffffU);
                axis.physical_position = static_cast<std::uint32_t>(
                    (static_cast<std::int64_t>(axis.physical_position) + counts * axis.motion_direction) & 0x00ffffff);
            }
        }
    }
    void set_unrelated_noise_duration(std::chrono::milliseconds duration) {
        unrelated_noise_ms_.store(static_cast<int>(duration.count()));
    }
    void drop_next_reply(std::uint8_t axis, std::uint8_t command, bool apply_command) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        reply_faults_.push_back({axis, command, apply_command});
    }

    void sever_link() {
        stop_worker();
        pty_.sever();
    }

    void set_encoder_position(std::uint8_t axis, std::uint32_t position) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        AxisState& state = axes_[axis_index(axis)];
        state.encoder_offset = (position - state.physical_position) & 0x00ffffffU;
    }

    void complete_axis_slew(std::uint8_t axis) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        AxisState& state = axes_[axis_index(axis)];
        state.physical_position =
            (state.physical_position + signed_delta(state.target, encoder_position(state))) & 0x00ffffffU;
        state.slewing = false;
        state.motion_remainder = 0;
    }

    void complete_axis_slew_at(std::uint8_t axis, std::uint32_t position) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        AxisState& state = axes_[axis_index(axis)];
        state.physical_position =
            (state.physical_position + signed_delta(position, encoder_position(state))) & 0x00ffffffU;
        state.slewing = false;
        state.motion_remainder = 0;
    }

    void complete_axis_home(std::uint8_t axis) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        AxisState& state = axes_[axis_index(axis)];
        state.physical_position = 0;
        state.motion_remainder = 0;
        state.homing = false;
        state.slewing = false;
    }

    void complete_axis_guide(std::uint8_t axis) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        axes_[axis_index(axis)].guiding = false;
    }

    bool axis_homing(std::uint8_t axis) const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return axes_[axis_index(axis)].homing;
    }

    bool axis_guiding(std::uint8_t axis) const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return axes_[axis_index(axis)].guiding;
    }

    std::uint32_t axis_position(std::uint8_t axis) const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return encoder_position(axes_[axis_index(axis)]);
    }

    std::uint32_t axis_physical_position(std::uint8_t axis) const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return axes_[axis_index(axis)].physical_position;
    }

    int count_command(std::uint8_t axis, std::uint8_t command) const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return axes_[axis_index(axis)].command_counts[command];
    }

    std::vector<alpacacore::vendor::celestron::AuxFrame> requests() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return {requests_.begin(), requests_.end()};
    }

private:
    struct AxisState {
        std::uint32_t physical_position = 0;
        std::uint32_t encoder_offset = 0;
        std::uint32_t target = 0;
        std::uint32_t velocity_counts_per_second = 100000;
        std::uint64_t motion_remainder = 0;
        int motion_direction = 1;
        bool slewing = false;
        bool homing = false;
        bool tracking = false;
        bool manual_motion = false;
        bool guiding = false;
        int stop_reads_remaining = 0;
        int post_stop_drift_remaining = 0;
        std::deque<std::int32_t> label_read_offsets;
        std::chrono::steady_clock::time_point guide_deadline{};
        std::uint8_t autoguide_rate = 128;
        std::array<int, 256> command_counts{};
    };

    struct ReplyFault {
        std::uint8_t axis;
        std::uint8_t command;
        bool apply_command;
    };

    void stop_worker() {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            stop_.store(true);
        }
        reply_cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    static std::size_t axis_index(std::uint8_t axis) {
        if (axis == 0x10) return 0;
        if (axis == 0x11) return 1;
        throw std::invalid_argument("CGX fake axis must be RA (0x10) or Dec (0x11)");
    }

    static std::uint32_t decode_u24(const std::vector<std::uint8_t>& bytes) {
        return (static_cast<std::uint32_t>(bytes[0]) << 16) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
               static_cast<std::uint32_t>(bytes[2]);
    }

    static std::uint32_t encoder_position(const AxisState& axis) {
        return (axis.physical_position + axis.encoder_offset) & 0x00ffffffU;
    }

    static std::int32_t signed_delta(std::uint32_t target, std::uint32_t current) {
        const std::uint32_t wrapped = (target - current) & 0x00ffffffU;
        return wrapped >= 0x00800000U ? static_cast<std::int32_t>(wrapped) - 0x01000000
                                      : static_cast<std::int32_t>(wrapped);
    }

    static std::vector<std::uint8_t> encode_u24(std::uint32_t value) {
        return {static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 8),
                static_cast<std::uint8_t>(value)};
    }

    void send_unrelated_noise(const alpacacore::vendor::celestron::AuxFrame& request) {
        using alpacacore::vendor::celestron::AuxFrame;
        using alpacacore::vendor::celestron::encode_aux_frame;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(unrelated_noise_ms_.load());
        const AuxFrame noise{.source = request.destination,
                             .destination = request.source,
                             .command = static_cast<std::uint8_t>(request.command ^ 0x01),
                             .payload = {}};
        const auto encoded = encode_aux_frame(noise);
        while (!stop_.load() && std::chrono::steady_clock::now() < deadline) {
            static_cast<void>(pty_write_bounded(pty_.master_fd(), reinterpret_cast<const char*>(encoded.data()),
                                                encoded.size(), stop_));
        }
    }

    void run() {
        while (!stop_.load()) {
            struct pollfd descriptor {};
            descriptor.fd = pty_.master_fd();
            descriptor.events = POLLIN;
            const int ready = ::poll(&descriptor, 1, 20);
            if (ready <= 0 || (descriptor.revents & POLLIN) == 0) {
                continue;
            }

            std::uint8_t bytes[128];
            const ssize_t count = ::read(pty_.master_fd(), bytes, sizeof(bytes));
            if (count <= 0) {
                continue;
            }
            receive_buffer_.insert(receive_buffer_.end(), bytes, bytes + count);
            process_requests();
        }
    }

    void process_requests() {
        using alpacacore::vendor::celestron::AuxFrame;
        using alpacacore::vendor::celestron::decode_aux_frame;
        using alpacacore::vendor::celestron::encode_aux_frame;

        while (!receive_buffer_.empty()) {
            const auto start = std::find(receive_buffer_.begin(), receive_buffer_.end(), 0x3b);
            receive_buffer_.erase(receive_buffer_.begin(), start);
            if (receive_buffer_.size() < 2) {
                return;
            }
            const std::size_t frame_size = static_cast<std::size_t>(receive_buffer_[1]) + 3;
            if (frame_size < 6 || frame_size > 32) {
                receive_buffer_.erase(receive_buffer_.begin());
                continue;
            }
            if (receive_buffer_.size() < frame_size) {
                return;
            }

            AuxFrame request;
            try {
                request = decode_aux_frame(std::span<const std::uint8_t>(receive_buffer_.data(), frame_size));
            } catch (const std::invalid_argument&) {
                receive_buffer_.erase(receive_buffer_.begin());
                continue;
            }
            receive_buffer_.erase(receive_buffer_.begin(),
                                  receive_buffer_.begin() + static_cast<std::ptrdiff_t>(frame_size));

            if (echo_requests_.load()) {
                const auto echo = encode_aux_frame(request);
                static_cast<void>(pty_write_bounded(pty_.master_fd(), reinterpret_cast<const char*>(echo.data()),
                                                    echo.size(), stop_));
            }
            send_unrelated_noise(request);
            const std::optional<AuxFrame> response = respond_to(request);
            if (!response) {
                continue;
            }
            auto encoded = encode_aux_frame(*response);
            if (corrupt_next_checksum_.exchange(false)) {
                encoded.back() ^= 0x01;
            }
            write_reply(*response, encoded);
        }
    }

    void write_reply(const alpacacore::vendor::celestron::AuxFrame& response, const std::vector<std::uint8_t>& bytes) {
        bool fragment = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!fragment_filter_ || *fragment_filter_ == std::pair{response.source, response.command})
                fragment = fragment_replies_.exchange(false);
        }
        if (fragment && bytes.size() > 1) {
            const std::size_t split = bytes.size() / 2;
            static_cast<void>(
                pty_write_bounded(pty_.master_fd(), reinterpret_cast<const char*>(bytes.data()), split, stop_));
            {
                std::unique_lock<std::mutex> lock(state_mutex_);
                reply_fragment_waiting_ = true;
                reply_cv_.notify_all();
                if (!reply_cv_.wait_for(lock, std::chrono::seconds(2),
                                        [this] { return release_reply_tail_ || stop_.load(); }) ||
                    stop_.load())
                    return;
            }
            static_cast<void>(pty_write_bounded(pty_.master_fd(), reinterpret_cast<const char*>(bytes.data() + split),
                                                bytes.size() - split, stop_));
            return;
        }
        static_cast<void>(
            pty_write_bounded(pty_.master_fd(), reinterpret_cast<const char*>(bytes.data()), bytes.size(), stop_));
    }

    std::optional<alpacacore::vendor::celestron::AuxFrame> respond_to(
        const alpacacore::vendor::celestron::AuxFrame& request) {
        using alpacacore::vendor::celestron::AuxFrame;
        if (request.destination != 0x10 && request.destination != 0x11 && request.destination != 0x30 &&
            request.destination != 0x31) {
            return std::nullopt;
        }

        std::lock_guard<std::mutex> lock(state_mutex_);
        if (requests_.size() == 128) requests_.pop_front();
        requests_.push_back(request);
        if (muted_.load()) return std::nullopt;
        if (request.destination == 0x30 || request.destination == 0x31) {
            if (request.command == 0xfe && cgx_switches_present_.load()) {
                return AuxFrame{.source = request.destination,
                                .destination = request.source,
                                .command = request.command,
                                .payload = {1, 0}};
            }
            return std::nullopt;
        }
        AxisState& axis = axes_[axis_index(request.destination)];
        ++axis.command_counts[request.command];
        const auto fault = std::find_if(reply_faults_.begin(), reply_faults_.end(), [&](const ReplyFault& candidate) {
            return candidate.axis == request.destination && candidate.command == request.command;
        });
        const bool drop_reply = fault != reply_faults_.end();
        const bool apply_command = !drop_reply || fault->apply_command;
        if (drop_reply) reply_faults_.erase(fault);
        if (!apply_command) return std::nullopt;
        std::vector<std::uint8_t> payload;
        switch (request.command) {
            case 0xfe:
                payload = motor_versions_[axis_index(request.destination)];
                break;
            case 0x05:
                payload = motor_models_[axis_index(request.destination)];
                break;
            case 0x01:
                if (request.destination == 0x10 && axis.post_stop_drift_remaining > 0) {
                    axis.physical_position = (axis.physical_position + 4) & 0x00ffffffU;
                    --axis.post_stop_drift_remaining;
                }
                if (request.destination == 0x10 && axis.tracking) {
                    axis.physical_position =
                        (axis.physical_position + ra_tracking_drift_per_read_.load()) & 0x00ffffffU;
                }
                if (!axis.label_read_offsets.empty()) {
                    payload = encode_u24((encoder_position(axis) + axis.label_read_offsets.front()) & 0x00ffffffU);
                    axis.label_read_offsets.pop_front();
                } else {
                    payload = encode_u24(encoder_position(axis));
                }
                break;
            case 0x04:
                if (request.payload.size() != 3) return std::nullopt;
                axis.encoder_offset = (decode_u24(request.payload) - axis.physical_position) & 0x00ffffffU;
                axis.label_read_offsets.assign(label_read_offsets_[axis_index(request.destination)].begin(),
                                               label_read_offsets_[axis_index(request.destination)].end());
                break;
            case 0x02:
            case 0x17:
                if (request.payload.size() != 3) return std::nullopt;
                axis.target = decode_u24(request.payload);
                axis.slewing = !auto_complete_motions_.load();
                axis.manual_motion = false;
                axis.tracking = false;
                axis.motion_remainder = 0;
                axis.motion_direction = signed_delta(axis.target, encoder_position(axis)) < 0 ? -1 : 1;
                if (!axis.slewing) {
                    axis.physical_position =
                        (axis.physical_position + signed_delta(axis.target, encoder_position(axis))) & 0x00ffffffU;
                }
                axis.homing = false;
                break;
            case 0x13:
                if (axis.stop_reads_remaining > 0) {
                    --axis.stop_reads_remaining;
                    payload = {0x00};
                    break;
                }
                payload = {static_cast<std::uint8_t>(axis.slewing ? 0x00 : 0xff)};
                break;
            case 0x0b:
                axis.post_stop_drift_remaining = 0;
                axis.homing = !auto_complete_motions_.load();
                axis.slewing = axis.homing;
                axis.manual_motion = false;
                axis.tracking = false;
                axis.motion_remainder = 0;
                if (!axis.homing) axis.physical_position = 0;
                break;
            case 0x12:
                payload = {static_cast<std::uint8_t>(axis.homing ? 0x00 : 0xff)};
                break;
            case 0x24:
            case 0x25:
                if (request.payload.size() != 1) return std::nullopt;
                axis.tracking = false;
                axis.motion_remainder = 0;
                if (request.payload[0] == 0) {
                    axis.stop_reads_remaining = stop_completion_reads_.load();
                    axis.slewing = false;
                    axis.homing = false;
                    axis.manual_motion = false;
                } else {
                    axis.manual_motion = true;
                    axis.slewing = false;
                    axis.homing = false;
                    axis.motion_direction = request.command == 0x25 ? -1 : 1;
                }
                break;
            case 0x26:
                if (request.payload.size() != 2 || request.payload[1] == 0) return std::nullopt;
                axis.guiding = !auto_complete_motions_.load();
                axis.guide_deadline =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(request.payload[1] * 10);
                break;
            case 0x27:
                if (axis.guiding && !hold_guide_until_completed_.load() &&
                    std::chrono::steady_clock::now() >= axis.guide_deadline)
                    axis.guiding = false;
                payload = {static_cast<std::uint8_t>(axis.guiding ? 1 : 0)};
                break;
            case 0x06:
            case 0x07:
                if (request.payload.size() != 2 && request.payload.size() != 3) return std::nullopt;
                if (request.destination == 0x10 && axis.tracking && request.payload[0] == 0 &&
                    request.payload[1] == 0 && (request.payload.size() == 2 || request.payload[2] == 0))
                    axis.post_stop_drift_remaining = ra_stop_drift_reads_;
                axis.tracking = request.payload[0] != 0 || request.payload[1] != 0 ||
                                (request.payload.size() == 3 && request.payload[2] != 0);
                if (axis.tracking) axis.post_stop_drift_remaining = 0;
                axis.manual_motion = false;
                axis.motion_remainder = 0;
                axis.motion_direction = request.command == 0x07 ? -1 : 1;
                break;
            case 0x46:
                if (request.payload.size() != 1) return std::nullopt;
                axis.autoguide_rate = request.payload[0];
                break;
            case 0x47:
                payload = {axis.autoguide_rate};
                break;
            default:
                // Unknown commands are silent; the fake must not turn an
                // unsupported command into a plausible success response.
                return std::nullopt;
        }
        if (drop_reply) return std::nullopt;
        return AuxFrame{.source = request.destination,
                        .destination = request.source,
                        .command = request.command,
                        .payload = std::move(payload)};
    }

    PtyPair pty_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> corrupt_next_checksum_{false};
    std::atomic<bool> auto_complete_motions_{false};
    std::atomic<bool> hold_guide_until_completed_{false};
    std::atomic<bool> muted_{false};
    std::atomic<bool> echo_requests_{false};
    std::atomic<bool> fragment_replies_{false};
    std::atomic<bool> cgx_switches_present_{true};
    std::atomic<int> stop_completion_reads_{0};
    std::atomic<std::uint32_t> ra_tracking_drift_per_read_{0};
    std::atomic<int> unrelated_noise_ms_{0};
    mutable std::mutex state_mutex_;
    std::condition_variable reply_cv_;
    bool reply_fragment_waiting_ = false;
    bool release_reply_tail_ = false;
    std::optional<std::pair<std::uint8_t, std::uint8_t>> fragment_filter_;
    std::array<AxisState, 2> axes_{};
    int ra_stop_drift_reads_ = 0;
    std::array<std::vector<std::int32_t>, 2> label_read_offsets_;
    std::array<std::vector<std::uint8_t>, 2> motor_versions_{{{7, 18}, {7, 18}}};
    std::array<std::vector<std::uint8_t>, 2> motor_models_{{{0x17, 0x88}, {0x17, 0x88}}};
    std::deque<alpacacore::vendor::celestron::AuxFrame> requests_;
    std::vector<ReplyFault> reply_faults_;
    std::vector<std::uint8_t> receive_buffer_;
    std::thread worker_;
};

}  // namespace alpacacore::test

#endif  // !_WIN32
