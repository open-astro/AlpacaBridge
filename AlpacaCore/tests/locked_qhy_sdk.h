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

#include <alpacacore/vendor/qhy/qhy_sdk_wrapper.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "fake_qhy_sdk.h"

namespace alpacacore::test {

/**
 * Thread-safe decorator over any QHYSDK — every call forwards to the inner
 * implementation under one mutex, except cancel_exposure(), which has its
 * own (see below).
 *
 * Exists for the concurrency stress harness (issue #101): FakeQHYSDK is
 * deliberately NOT thread-hardened (single-connect-path tests don't need it),
 * but the stress tests hammer one driver from many threads, so unguarded fake
 * state would light up ThreadSanitizer with races in TEST code and drown out
 * the driver races the harness exists to catch. The real wrapper serializes
 * internally (a bookkeeping mutex plus a per-handle call_mutex), so production
 * drivers never need this.
 *
 * The mutex is a leaf: the inner SDK never calls back into this interface.
 *
 * This decorator is only sound because no QHYSDK method blocks in a fake —
 * holding one mutex across a blocking call would serialize the storm into a
 * queue. See rule 1 in FakeQHYSDK's class comment.
 *
 * One production rule this decorator DOES preserve, since open-astro#339:
 * QHYSDKWrapper::cancel_exposure() skips the per-handle call_mutex on purpose
 * (AGENTS.md, "Every SDK call is serialized against its physical handle"),
 * because its whole job is to interrupt a GetQHYCCDSingleFrame blocked on
 * another thread — serializing it the same way as every other call would
 * deadlock it behind the very call it needs to cancel. Here, cancel_exposure()
 * takes cancel_mutex_ instead of mutex_: cancels are serialised against each
 * other but NOT against the other forwards, so a cancel can overtake an
 * in-flight call (e.g. a fake method blocked in before_call to model a
 * timeout) exactly as production lets it. That is sound only because the
 * fake's cancel_exposure body touches no state shared with the call it
 * overtakes; a fake whose cancel starts mutating shared state must go back
 * under mutex_ and give up the overtaking property. slowest_call_ms() is the
 * mechanical check that no OTHER fake method has quietly gained a blocking
 * call.
 */
class LockedQHYSDK : public vendor::qhy::QHYSDK {
public:
    using QHYCameraInfo = vendor::qhy::QHYCameraInfo;
    using QHYControlRange = vendor::qhy::QHYControlRange;

    explicit LockedQHYSDK(QHYSDK& inner) : inner_(inner) {}

    /// The longest any of the 26 forwards has spent INSIDE the inner call,
    /// in milliseconds (open-astro#339); time queued behind another forward
    /// on a mutex is not counted, since the clock starts after the lock is
    /// taken. cancel_exposure() is timed too, by its own Guard, even though it
    /// is the one forward outside locked(). Every fake method is pure bookkeeping,
    /// so this stays at or near zero; a test asserts a generous ceiling on it
    /// to catch a fake method that has gained a blocking call, which would
    /// otherwise show up as a hung [stress] run.
    long long slowest_call_ms() const { return slowest_call_ms_.load(std::memory_order_relaxed); }

    std::vector<QHYCameraInfo> enumerate_cameras() override {
        return locked([&] { return inner_.enumerate_cameras(); });
    }
    bool get_camera_model(const std::string& camera_id, std::string& model) override {
        return locked([&] { return inner_.get_camera_model(camera_id, model); });
    }

    void open_camera(const std::string& camera_id) override {
        locked([&] { inner_.open_camera(camera_id); });
    }
    void init_camera(const std::string& camera_id) override {
        locked([&] { inner_.init_camera(camera_id); });
    }
    void close_camera(const std::string& camera_id) override {
        locked([&] { inner_.close_camera(camera_id); });
    }
    void register_exposure_worker(const std::string& camera_id,
                                  std::shared_ptr<std::atomic<bool>> running_flag) override {
        locked([&] { inner_.register_exposure_worker(camera_id, std::move(running_flag)); });
    }

    bool get_chip_info(const std::string& camera_id, QHYCameraInfo& info) override {
        return locked([&] { return inner_.get_chip_info(camera_id, info); });
    }

    bool is_control_available(const std::string& camera_id, int control_id) override {
        return locked([&] { return inner_.is_control_available(camera_id, control_id); });
    }
    double get_param(const std::string& camera_id, int control_id) override {
        return locked([&] { return inner_.get_param(camera_id, control_id); });
    }
    QHYControlRange get_param_range(const std::string& camera_id, int control_id) override {
        return locked([&] { return inner_.get_param_range(camera_id, control_id); });
    }
    void set_param(const std::string& camera_id, int control_id, double value) override {
        locked([&] { inner_.set_param(camera_id, control_id, value); });
    }

    void set_resolution(const std::string& camera_id, uint32_t start_x, uint32_t start_y, uint32_t width,
                        uint32_t height) override {
        locked([&] { inner_.set_resolution(camera_id, start_x, start_y, width, height); });
    }
    void set_bin_mode(const std::string& camera_id, uint32_t wbin, uint32_t hbin) override {
        locked([&] { inner_.set_bin_mode(camera_id, wbin, hbin); });
    }
    void set_bits_mode(const std::string& camera_id, uint32_t bits) override {
        locked([&] { inner_.set_bits_mode(camera_id, bits); });
    }
    uint32_t get_mem_length(const std::string& camera_id) override {
        return locked([&] { return inner_.get_mem_length(camera_id); });
    }

    bool start_single_frame(const std::string& camera_id) override {
        return locked([&] { return inner_.start_single_frame(camera_id); });
    }
    bool get_single_frame(const std::string& camera_id, uint8_t* buffer, uint32_t& width, uint32_t& height,
                          uint32_t& bpp, uint32_t& channels) override {
        return locked([&] { return inner_.get_single_frame(camera_id, buffer, width, height, bpp, channels); });
    }
    void cancel_exposure(const std::string& camera_id) override {
        // open-astro#339: its OWN mutex, not the shared one.
        //
        // QHYSDKWrapper::cancel_exposure() deliberately skips the per-handle
        // call mutex, because its whole job is to interrupt a
        // GetQHYCCDSingleFrame already blocked on the same handle from another
        // thread. Forwarding it through this decorator's one mutex inverted
        // exactly that invariant: the safety valve would queue behind the very
        // call it exists to interrupt.
        //
        // The issue offered two fixes -- forward unlocked, or give the cancel
        // its own mutex. Unlocked is what production does, but it would make
        // this the one racy forward in the decorator, which is the confusion
        // the decorator was built to prevent (and which check_docs_drift.py
        // rightly refuses). A second mutex keeps every forward serialised
        // while letting a cancel proceed against an in-flight call, which is
        // the property production actually needs. It is sound here because
        // the fake's cancel_exposure body touches no shared state -- it has no
        // in-flight exposure to interrupt -- so it cannot race the call it
        // overtakes.
        std::lock_guard<std::mutex> lock(cancel_mutex_);
        // Timed like every other forward (review round 4 on PR #463): this
        // is the one body outside locked(), so without its own Guard a fake
        // cancel that gained a block was the single method the watchdog
        // could not name.
        const auto started = std::chrono::steady_clock::now();
        Guard guard{started, slowest_call_ms_};
        inner_.cancel_exposure(camera_id);
    }

    void guide(const std::string& camera_id, uint32_t qhy_direction, uint16_t duration_ms) override {
        locked([&] { inner_.guide(camera_id, qhy_direction, duration_ms); });
    }

    void control_temp(const std::string& camera_id, double target_temp_c) override {
        locked([&] { inner_.control_temp(camera_id, target_temp_c); });
    }

    void move_cfw(const std::string& camera_id, int position) override {
        locked([&] { inner_.move_cfw(camera_id, position); });
    }
    int get_cfw_position(const std::string& camera_id) override {
        return locked([&] { return inner_.get_cfw_position(camera_id); });
    }

    uint32_t get_num_readout_modes(const std::string& camera_id) override {
        return locked([&] { return inner_.get_num_readout_modes(camera_id); });
    }
    std::string get_readout_mode_name(const std::string& camera_id, uint32_t mode_index) override {
        return locked([&] { return inner_.get_readout_mode_name(camera_id, mode_index); });
    }
    void set_readout_mode(const std::string& camera_id, uint32_t mode_index) override {
        locked([&] { inner_.set_readout_mode(camera_id, mode_index); });
    }

    std::string get_sdk_version() override {
        return locked([&] { return inner_.get_sdk_version(); });
    }

private:
    template <typename Fn>
    auto locked(Fn&& fn) -> decltype(fn()) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto started = std::chrono::steady_clock::now();
        Guard guard{started, slowest_call_ms_};
        return fn();
    }

    // open-astro#339: the "no QHY fake method may block" rule was prose in two
    // headers, and two things depend on it -- the camera driver's detachable
    // workers (a blocking fake turns a timed-out join into a detached thread
    // still calling into a fake the test body has already destroyed) and the
    // cancel_exposure exemption above. A future fake method gaining a
    // perfectly reasonable-looking sleep_for silently broke both, and the
    // symptom was a HUNG [stress] run rather than a named failure.
    //
    // Recording the slowest forward turns that into something a test can
    // assert on. Every fake method is pure bookkeeping, so the bar is orders
    // of magnitude of headroom rather than a tight timing assertion -- this
    // must not become a flaky test on a loaded CI box.
    struct Guard {
        std::chrono::steady_clock::time_point started;
        std::atomic<long long>& slowest;
        ~Guard() {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                    .count();
            long long previous = slowest.load(std::memory_order_relaxed);
            while (elapsed > previous && !slowest.compare_exchange_weak(previous, elapsed, std::memory_order_relaxed)) {
            }
        }
    };

    QHYSDK& inner_;
    std::mutex mutex_;
    // open-astro#339: cancel-only, so a cancel never queues behind the call
    // it exists to interrupt. See cancel_exposure().
    std::mutex cancel_mutex_;
    std::atomic<long long> slowest_call_ms_{0};
};

/**
 * Owns the fake, the locking decorator and the driver in ONE object, so the
 * lifetime rule cannot be written wrong (open-astro#338).
 *
 * Every QHY seam test must outlive the driver with the fake: the driver holds
 * a QHYSDK& and its detachable workers capture a raw QHYSDK*, so a case that
 * declares them the other way round -- or stashes the driver in a Catch2
 * fixture member, a vector, or anything outliving the fake -- compiles
 * cleanly and is undefined behaviour, most likely a use-after-free inside a
 * detached worker. That is the failure mode hardest to attribute when it
 * surfaces, and the [stress] storms raise the stakes: exposure, temperature,
 * cooler-off, pulse-guide and telemetry workers are all detachable.
 *
 * The rule was stated in three places (the QHYSDK interface comment, the
 * FakeQHYSDK class comment, and AGENTS.md) and enforced by nothing. Member
 * declaration order here gives the right destruction order once, in one
 * place: driver first, then the decorator, then the fake.
 */
template <typename Driver>
class QHYSeamFixture {
public:
    /// `make` receives the decorator and returns the driver, so the driver is
    /// built from a decorator that is already a member of this object rather
    /// than from a caller's local.
    template <typename MakeDriver>
    QHYSeamFixture(FakeQHYSDK fake, MakeDriver&& make) : fake_(std::move(fake)), sdk_(fake_), driver_(make(sdk_)) {}

    FakeQHYSDK& fake() { return fake_; }
    LockedQHYSDK& sdk() { return sdk_; }
    Driver& driver() { return *driver_; }
    Driver* operator->() { return driver_.get(); }

private:
    // DECLARATION ORDER IS THE CONTRACT. Members destroy in reverse, so the
    // driver goes first (joining or detaching its workers), then the
    // decorator, then the fake those workers may still be calling into.
    // Reordering these three lines reintroduces exactly the bug this exists
    // to make unwritable.
    FakeQHYSDK fake_;
    LockedQHYSDK sdk_;
    std::unique_ptr<Driver> driver_;
};

}  // namespace alpacacore::test
