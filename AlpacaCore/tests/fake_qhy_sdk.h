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
#include <alpacacore/vendor/qhy/qhy_sdk_wrapper.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace alpacacore::test {

/**
 * Scripted fake for the QHYSDK seam (issue #321).
 *
 * QHY is the one vendor whose real SDK cannot run at all on a test runner:
 * the first libqhyccd call spawns PnpEventListenerThread, which segfaults in
 * libusb_hotplug_register_callback when libusb_init failed. Before this fake
 * the QHY drivers had NO automated connect coverage of any kind — neither
 * test_qhy_camera.cpp nor test_qhy_filterwheel.cpp ever reached
 * set_connected(true).
 *
 * Capabilities:
 * - Fault injection: add a method name to `throw_from` and that call throws
 *   AlpacaException(DriverException).
 * - `sdk_resource_available = false` makes the three resource-dependent entry
 *   points (enumerate_cameras, get_camera_model, open_camera) throw the
 *   wrapper's "QHY SDK resource not initialized" DriverException — the path
 *   that on real hardware can only be reached by crashing.
 * - Canned devices: fill `cameras`; enumeration returns them verbatim and
 *   opens resolve ids against them.
 * - Ref-counted opens per camera_id, matching the wrapper's open_count: the
 *   camera driver and the CFW driver share ONE handle for one physical
 *   device. `physical_opens`/`physical_closes` count real transitions;
 *   `underflow_closes` counts closes of a camera that was not open (must stay
 *   0 in a correct driver).
 * - Scripted wheel positions: `cfw_position_script` is consumed one read at a
 *   time (last value repeats), so a homing/transit sequence is testable. -1
 *   means "still moving", matching GetQHYCCDCFWStatus.
 *
 * TWO RULES THIS FAKE MUST KEEP (they are not stylistic):
 *
 * 1. NOTHING HERE MAY BLOCK. The camera driver's exposure, temperature and
 *    cooler-off workers join with a bounded timeout and DETACH on expiry, and
 *    its pulse-guide worker is detached by design. A fake that blocks turns
 *    those into detached threads still calling into it after the test body
 *    has moved on — i.e. a use-after-free of the fake itself.
 * 2. THE FAKE MUST OUTLIVE EVERY DRIVER BUILT ON IT, including those
 *    detachable workers, which reach the SDK through a captured QHYSDK*
 *    rather than through the driver's `sdk_` member. (All but pulse-guide
 *    still capture `this` too and touch it after the SDK call returns, so
 *    they are not safe to outlive the driver either -- the capture narrows
 *    that window, it does not remove it.) Declare the fake before the driver
 *    (locals destroy in reverse order); never stash a driver beyond the
 *    fake's scope.
 *
 * default_camera() reports NO cooler. That is deliberate: has_cooler starts
 * the driver's telemetry thread, whose loop sleeps 1s between polls, so every
 * disconnect then blocks up to ~1s in the join. Use default_cooled_camera()
 * when the thermal paths are what's under test, and keep it out of anything
 * that connects in a loop.
 *
 * KNOWN PARITY GAPS — places this fake is deliberately WEAKER than the real
 * wrapper, so a test passing here would not have caught a regression in the
 * corresponding real guard. Each is tracked, and the [stress] follow-up (#321)
 * will exercise them all. One IS relied on: the two binning cases call
 * set_bin_mode() and assert get_mem_length() does not shrink further, so the
 * #365 residual below is load-bearing for them -- making get_mem_length() read
 * wbin_/hbin_ would fail those cases, which is the protection wanted, but it
 * means closing #365 is not free:
 *
 * - set_bin_mode()'s wbin_/hbin_ are WRITE-ONLY: nothing reads them, so a bin
 *   change on its own never moves get_mem_length(). Under the ROI UNITS
 *   convention below that is correct for how the driver drives this fake --
 *   it always pairs a bin change with a set_resolution() carrying the binned
 *   size -- but a test that calls set_bin_mode() alone and expects the length
 *   to shrink, the way the real GetQHYCCDMemLength() does after
 *   SetQHYCCDBinMode, will not see it. Tracked in issue #365.
 * - open_camera() does not refuse a fresh open while a registered exposure
 *   worker is still live (the real one throws InvalidOperation — this is the
 *   PR #201 finding). `exposure_workers_` is written and never read here.
 *   Reconnect storms are exactly what would go green over a break in it.
 *   Tracked in issue #324.
 * - move_cfw() does not enforce the position > 9 single-digit protocol
 *   ceiling the real one throws InvalidValue for; the wheel tests pass only
 *   because the driver guards it first. Tracked in issue #327.
 * - set_readout_mode() does not re-run init_camera() the way the real one
 *   re-invokes InitQHYCCD, so any init_calls assertion around a mode switch
 *   reads differently here than on hardware. Tracked in issue #335.
 * - get_chip_info() derives has_cooler/is_color/bayer_pattern/has_st4_port/
 *   has_shutter from the canned QHYCameraInfo, where the real one derives all
 *   five from IsQHYCCDControlAvailable. So steering a capability by
 *   adding/removing control::COOLER from `controls_available` has NO effect
 *   here, while the same change on hardware would flip has_cooler. Set the
 *   struct field (or use default_cooled_camera()) instead. Tracked in
 *   issue #337.
 *
 * ROI UNITS — `roi_` is in BINNED pixels, matching the only caller: the driver
 * passes max_width / bin_x to set_resolution() from set_bin_locked(), and
 * ASCOM NumX/NumY (already binned) from start_exposure(). So get_mem_length()
 * shrinks with the binning because the ROI shrinks, NOT by dividing again --
 * doing both would report a quarter of the bytes the SDK owes for the frame it
 * is about to deliver. get_mem_length() and get_single_frame() must agree:
 * hardware cannot deliver an image larger than GetQHYCCDMemLength(), and the
 * "get_single_frame never exceeds get_mem_length" case pins the pair at bin > 1.
 *
 * FIXED since this list was written, kept named so a reader chasing an old
 * comment lands somewhere: get_param() now answers the QHYCCD_ERROR sentinel,
 * not 0.0, for a control missing from `params` (issue #373); and control_temp() now
 * approaches its target by `temp_settle_step_c` per call rather than settling
 * instantly, the way ControlQHYCCDTemp's PID does (issue #390).
 *
 * Not thread-hardened, by design — wrap it in LockedQHYSDK for the [stress]
 * suite so ThreadSanitizer reports point at driver code, not at this file.
 */
class FakeQHYSDK : public vendor::qhy::QHYSDK {
public:
    using QHYCameraInfo = vendor::qhy::QHYCameraInfo;
    using QHYControlRange = vendor::qhy::QHYControlRange;

    // --- scripting knobs ---------------------------------------------------
    std::set<std::string> throw_from;
    bool sdk_resource_available = true;
    // Set to "" to reach the empty-cache branch the driver special-cases in
    // get_driver_info()/get_device_sdk_version(): the real wrapper returns an
    // empty string until the SDK resource comes up, so a fake that always
    // answers non-empty can never exercise it.
    std::string sdk_version = "fake-qhy-1.0";
    std::vector<QHYCameraInfo> cameras;
    // Controls the drivers probe. CFWPORT present by default so the filter
    // wheel connects; add/remove to steer capability branches.
    //
    // ST4PORT earns its place differently: no driver path probes it (the camera
    // reads camera_info_.has_st4_port directly), but default_camera() sets that
    // flag true, and leaving the fake's two capability sources disagreeing in
    // the DEFAULT seeding pre-bakes the trap the get_chip_info() parity gap
    // above warns about. Keep the two in step when adding a capability.
    std::set<int> controls_available{
        vendor::qhy::control::BITS16,
        vendor::qhy::control::BIN1X1,
        vendor::qhy::control::BIN2X2,
        vendor::qhy::control::GAIN,
        vendor::qhy::control::OFFSET,
        vendor::qhy::control::EXPOSURE,
        vendor::qhy::control::CFWPORT,
        vendor::qhy::control::CFWSLOTSNUM,
        vendor::qhy::control::ST4PORT,
        // CURTEMP/CURPWM are seeded in `params` below, and since get_param()
        // now answers the QHYCCD_ERROR sentinel for anything missing from
        // `params` (issue #373), membership there reads as "supported". Listing
        // them here too keeps the fake's two capability sources in step, as the
        // note above requires: the driver gates its telemetry read on
        // is_control_available(CURTEMP) (qhy_camera_driver.cpp), so without this
        // the seeded readings were unreachable and the telemetry path silently
        // untested.
        vendor::qhy::control::CURTEMP,
        vendor::qhy::control::CURPWM,
    };
    std::map<int, double> params{
        {vendor::qhy::control::GAIN, 10.0},         {vendor::qhy::control::OFFSET, 20.0},
        {vendor::qhy::control::EXPOSURE, 100000.0}, {vendor::qhy::control::CURTEMP, -5.0},
        {vendor::qhy::control::CURPWM, 30.0},       {vendor::qhy::control::CFWSLOTSNUM, 5.0},
    };
    std::map<int, QHYControlRange> param_ranges{
        {vendor::qhy::control::GAIN, {0.0, 100.0, 1.0, true}},
        {vendor::qhy::control::OFFSET, {0.0, 255.0, 1.0, true}},
        {vendor::qhy::control::EXPOSURE, {100.0, 3600000000.0, 100.0, true}},
    };
    std::vector<std::string> readout_modes{"Full Resolution", "Linearity HDR"};
    // Consumed by get_cfw_position; -1 = in motion. Last entry repeats.
    std::deque<int> cfw_position_script;
    bool read_directly = false;  // start_single_frame's return
    bool frame_ok = true;        // get_single_frame's return

    // --- observability -----------------------------------------------------
    //
    // Only `calls` is mutex-guarded (read it via call_count()). Every plain
    // counter below is written by whichever thread made the call and read
    // straight from the test body, which is a data race the moment a driver
    // worker is running concurrently with that read.
    //
    // Sound for every case in these files today, though the reason is not
    // simply "no cooled cameras" -- test_qhy_fake_sdk.cpp now has one, the
    // control_temp convergence case, which reads last_temp_target straight
    // from the test body. What makes all of them safe is that NO SECOND THREAD
    // ever runs: that file builds no driver at all, and the camera and wheel
    // files build drivers but never drive one into starting a background
    // worker -- their two start_exposure() calls only assert a throw, and
    // neither file uses a cooled camera, so no telemetry or temperature thread
    // is ever spawned.
    //
    // THE FIRST case that lets a driver worker actually run breaks that -- a
    // connected cooled camera, a real exposure, a pulse guide -- and these
    // reads become TSan findings in test code, exactly the noise LockedQHYSDK
    // exists to keep out of the [stress] suite. Route them through the same
    // lock before adding such a case. Tracked in issue #331.
    //
    // The same issue covers the mirror-image race on the input side: hit()
    // bumps `calls` under calls_mutex but then reads `throw_from` outside it,
    // so a test body that arms or clears fault injection mid-storm races the
    // call path reading it. Sound today for the same reason (no driver, no
    // second thread) and unsound from the same first case, so fix both
    // together rather than one at a time.
    std::map<std::string, int> calls;
    int physical_opens = 0;
    int physical_closes = 0;
    int underflow_closes = 0;
    int init_calls = 0;
    std::string last_opened_id;
    std::string last_guide_id;
    uint32_t last_guide_direction = 0;
    uint16_t last_guide_duration_ms = 0;
    double last_temp_target = 0.0;
    /// How far control_temp() moves CURTEMP toward its target per call. The
    /// real ControlQHYCCDTemp is a PID that converges over many calls
    /// (issue #390); 0.0 restores the old instant settle.
    double temp_settle_step_c = 0.5;
    int last_cfw_target = -1;

    int ref_count(const std::string& id) const {
        auto it = ref_counts_.find(id);
        return it == ref_counts_.end() ? 0 : it->second;
    }

    /// Thread-safe view of `calls` — driver workers hit the fake concurrently
    /// with the test body.
    int call_count(const char* fn) const {
        std::lock_guard<std::mutex> lock(sync_.calls_mutex);
        auto it = calls.find(fn);
        return it == calls.end() ? 0 : it->second;
    }

    /// How many DISTINCT method names have been called at least once.
    ///
    /// Exists so a test never has to touch `calls` directly: reading
    /// `calls.size()` from a test body is the one read that bypasses the
    /// accessor, and it would have to be found again when issue #331 routes
    /// the rest of the observability state through this lock.
    std::size_t distinct_calls() const {
        std::lock_guard<std::mutex> lock(sync_.calls_mutex);
        return calls.size();
    }

    /// A plausible uncooled mono camera, sized small so get_mem_length() and
    /// the driver's frame buffer stay cheap under a storm.
    static QHYCameraInfo default_camera(const std::string& id, const std::string& model) {
        QHYCameraInfo info;
        info.camera_id = id;
        info.model = model;
        info.max_width = 64;
        info.max_height = 48;
        info.pixel_size_x_um = 3.76;
        info.pixel_size_y_um = 3.76;
        info.bpp = 16;
        info.is_color = false;
        info.bayer_pattern = 0;
        info.has_cooler = false;
        info.has_st4_port = true;
        info.has_shutter = false;
        return info;
    }

    /// Same, but with the TEC — starts the driver's telemetry and temp-control
    /// threads. See the class comment before using this in a connect loop.
    static QHYCameraInfo default_cooled_camera(const std::string& id, const std::string& model) {
        QHYCameraInfo info = default_camera(id, model);
        info.has_cooler = true;
        return info;
    }

    /// A fake holding exactly one default_camera() — the setup every QHY seam
    /// test file needs first (issue #342). It lived as a verbatim `make_fake()`
    /// in three of them, so a change to what a default test fake looks like (a
    /// new entry in `controls_available`, a different canned camera) had to be
    /// made in three places with nothing failing if it was made in two.
    static FakeQHYSDK with_one_camera(const std::string& id = "fake-qhy-0", const std::string& model = "FakeQHY600") {
        FakeQHYSDK fake;
        fake.cameras.push_back(default_camera(id, model));
        return fake;
    }

    /// with_one_camera(), but the camera has a TEC. Read default_cooled_camera()'s
    /// warning before using this in a connect loop.
    static FakeQHYSDK with_one_cooled_camera(const std::string& id = "fake-qhy-0",
                                             const std::string& model = "FakeQHY600") {
        FakeQHYSDK fake;
        fake.cameras.push_back(default_cooled_camera(id, model));
        return fake;
    }

    // --- QHYSDK implementation ---------------------------------------------
    std::vector<QHYCameraInfo> enumerate_cameras() override {
        hit("enumerate_cameras");
        require_resource();
        return cameras;
    }

    bool get_camera_model(const std::string& camera_id, std::string& model) override {
        hit("get_camera_model");
        require_resource();
        for (const auto& cam : cameras) {
            if (cam.camera_id == camera_id) {
                model = cam.model;
                return true;
            }
        }
        return false;
    }

    void open_camera(const std::string& camera_id) override {
        hit("open_camera");
        require_resource();
        if (!known_id(camera_id)) {
            // Matches QHYSDKWrapper::open_camera(): OpenQHYCCD returning null
            // for an id it doesn't recognize throws DriverException, not
            // NotConnected -- the real SDK has no concept of "not connected"
            // at this call, only "the open failed".
            throw AlpacaException("fake: unknown QHY camera id '" + camera_id + "'", AlpacaError::DriverException);
        }
        last_opened_id = camera_id;
        auto& count = ref_counts_[camera_id];
        if (count == 0) {
            ++physical_opens;
        }
        ++count;
    }

    void init_camera(const std::string& camera_id) override {
        hit("init_camera");
        require_open(camera_id);
        ++init_calls;
    }

    void close_camera(const std::string& camera_id) override {
        hit("close_camera");
        auto it = ref_counts_.find(camera_id);
        if (it == ref_counts_.end() || it->second <= 0) {
            ++underflow_closes;
            return;
        }
        if (--it->second == 0) {
            ++physical_closes;
            ref_counts_.erase(it);
        }
    }

    void register_exposure_worker(const std::string& camera_id,
                                  std::shared_ptr<std::atomic<bool>> running_flag) override {
        hit("register_exposure_worker");
        exposure_workers_[camera_id] = std::move(running_flag);
    }

    bool get_chip_info(const std::string& camera_id, QHYCameraInfo& info) override {
        hit("get_chip_info");
        require_open(camera_id);
        // QHYSDKWrapper::get_chip_info() never writes info.model at all -- the
        // driver gets the model exclusively from the separate
        // get_camera_model() call, made earlier in the connect sequence.
        // Back-filling it here (an earlier version of this fake did) would
        // hide a regression that dropped that call: the test would still see
        // a correct name under the fake and an empty one on real hardware.
        const std::string model = info.model;
        for (const auto& cam : cameras) {
            if (cam.camera_id == camera_id) {
                info = cam;
                info.model = model;
                return true;
            }
        }
        return false;
    }

    bool is_control_available(const std::string& camera_id, int control_id) override {
        hit("is_control_available");
        require_open(camera_id);
        return controls_available.count(control_id) != 0;
    }

    /// What GetQHYCCDParam() returns for a control the camera does not
    /// support: QHYCCD_ERROR, 0xFFFFFFFF, which is about 4.29e9 once the
    /// SDK's `double` return widens it. Named here so a test can say what it
    /// expects without spelling the constant out (issue #373).
    static constexpr double kUnsupportedControl = 4294967295.0;

    double get_param(const std::string& camera_id, int control_id) override {
        hit("get_param");
        require_open(camera_id);
        auto it = params.find(control_id);
        // Not 0.0: the real wrapper returns GetQHYCCDParam() raw, and the SDK
        // answers the QHYCCD_ERROR sentinel for an unsupported control. A fake
        // answering a plausible zero is the more dangerous of the two, because
        // modelling "unsupported" by dropping an entry from `params` would hand
        // the driver a value it has no reason to reject where hardware hands it
        // one it must (issue #373).
        return it == params.end() ? kUnsupportedControl : it->second;
    }

    QHYControlRange get_param_range(const std::string& camera_id, int control_id) override {
        hit("get_param_range");
        require_open(camera_id);
        auto it = param_ranges.find(control_id);
        if (it == param_ranges.end()) {
            return QHYControlRange{0.0, 0.0, 0.0, false};
        }
        return it->second;
    }

    void set_param(const std::string& camera_id, int control_id, double value) override {
        hit("set_param");
        require_open(camera_id);
        params[control_id] = value;
    }

    void set_resolution(const std::string& camera_id, uint32_t start_x, uint32_t start_y, uint32_t width,
                        uint32_t height) override {
        hit("set_resolution");
        require_open(camera_id);
        roi_ = {start_x, start_y, width, height};
    }

    void set_bin_mode(const std::string& camera_id, uint32_t wbin, uint32_t hbin) override {
        hit("set_bin_mode");
        require_open(camera_id);
        wbin_ = wbin;
        hbin_ = hbin;
    }

    void set_bits_mode(const std::string& camera_id, uint32_t bits) override {
        hit("set_bits_mode");
        require_open(camera_id);
        bits_ = bits;
    }

    uint32_t get_mem_length(const std::string& camera_id) override {
        hit("get_mem_length");
        require_open(camera_id);
        const uint32_t bytes_per_px = (bits_ > 8) ? 2U : 1U;
        // ROI CONVENTION (issue #365, settled in review -- this body is
        // UNCHANGED by that investigation, which is the point): roi_ is in
        // BINNED pixels, because that is what the only caller passes --
        // set_bin_locked() calls set_resolution(0, 0, max_width / bin_x,
        // max_height / bin_y), and start_exposure() passes num_x_/num_y_, which
        // are ASCOM NumX/NumY and therefore binned too. So the length DOES
        // shrink with the binning, via a smaller ROI, and dividing by
        // wbin_/hbin_ here as well would report a quarter of the bytes the SDK
        // must return for the frame it is about to deliver -- a new parity lie
        // in place of the old one, and a heap overflow in any test that sizes
        // its buffer from this and then calls get_single_frame() (proven with
        // ASan: 384-byte buffer, 1536-byte memset). set_bin_mode()'s
        // wbin_/hbin_ therefore stay write-only -- see the parity gap above.
        //
        // This value and what get_single_frame() reports must stay in step:
        // hardware cannot deliver an image larger than GetQHYCCDMemLength().
        // The contract case "get_single_frame never exceeds get_mem_length"
        // pins that pairing at bin > 1.
        return roi_.width * roi_.height * bytes_per_px;
    }

    bool start_single_frame(const std::string& camera_id) override {
        hit("start_single_frame");
        require_open(camera_id);
        return read_directly;
    }

    bool get_single_frame(const std::string& camera_id, uint8_t* buffer, uint32_t& width, uint32_t& height,
                          uint32_t& bpp, uint32_t& channels) override {
        hit("get_single_frame");
        require_open(camera_id);
        // Returns IMMEDIATELY — see rule 1 in the class comment. The real call
        // blocks for the whole exposure; a fake that did would strand the
        // driver's detachable exposure worker.
        width = roi_.width;
        height = roi_.height;
        bpp = bits_;
        channels = 1;
        if (frame_ok && buffer != nullptr) {
            const uint32_t bytes_per_px = (bits_ > 8) ? 2U : 1U;
            std::memset(buffer, 0, static_cast<std::size_t>(width) * height * bytes_per_px);
        }
        return frame_ok;
    }

    void cancel_exposure(const std::string& camera_id) override {
        hit("cancel_exposure");
        // Matches QHYSDKWrapper::cancel_exposure(): a missing/closed handle
        // is a silent no-op, not NotConnected. Deliberate on the real side --
        // this is the SDK's mechanism for interrupting a call already blocked
        // on the SAME handle from another thread, so it can't afford to throw
        // on a handle a racing close() just erased. The body is empty on BOTH
        // paths -- the fake has no in-flight exposure to interrupt -- so there
        // is deliberately no is_open() branch here: adding one would read as
        // a guard while changing nothing.
        static_cast<void>(camera_id);
    }

    void guide(const std::string& camera_id, uint32_t qhy_direction, uint16_t duration_ms) override {
        hit("guide");
        require_open(camera_id);
        // The real call blocks for the full pulse duration; this must not.
        last_guide_id = camera_id;
        last_guide_direction = qhy_direction;
        last_guide_duration_ms = duration_ms;
    }

    void control_temp(const std::string& camera_id, double target_temp_c) override {
        hit("control_temp");
        require_open(camera_id);
        last_temp_target = target_temp_c;
        // ControlQHYCCDTemp runs a PID over many calls -- which is why the
        // driver polls it about once a second -- so the TEC approaches the
        // target rather than arriving at it (issue #390). Writing the target
        // straight into CURTEMP let a thermal test assert an instant settle
        // that hardware can never produce, and a driver that only ever reads
        // back its own setpoint would look correct here.
        //
        // The model is deliberately the simplest thing that is not instant: a
        // fixed step per call toward the target, clamped so it never
        // overshoots. `temp_settle_step_c = 0.0` restores the old instant
        // settle for a case that wants to skip the ramp.
        auto& current = params[vendor::qhy::control::CURTEMP];
        const double delta = target_temp_c - current;
        if (temp_settle_step_c <= 0.0 || std::abs(delta) <= temp_settle_step_c) {
            current = target_temp_c;
        } else {
            current += (delta > 0.0) ? temp_settle_step_c : -temp_settle_step_c;
        }
    }

    void move_cfw(const std::string& camera_id, int position) override {
        hit("move_cfw");
        require_open(camera_id);
        last_cfw_target = position;
        cfw_position_ = position;
        cfw_position_script.clear();
    }

    int get_cfw_position(const std::string& camera_id) override {
        hit("get_cfw_position");
        require_open(camera_id);
        if (cfw_position_script.empty()) {
            return cfw_position_;
        }
        cfw_position_ = cfw_position_script.front();
        if (cfw_position_script.size() > 1) {
            cfw_position_script.pop_front();  // last entry repeats forever
        }
        return cfw_position_;
    }

    uint32_t get_num_readout_modes(const std::string& camera_id) override {
        hit("get_num_readout_modes");
        require_open(camera_id);
        // Matches QHYSDKWrapper::get_num_readout_modes(): floors at 1 ("at
        // least one mode"), never reports zero.
        return effective_readout_mode_count();
    }

    std::string get_readout_mode_name(const std::string& camera_id, uint32_t mode_index) override {
        hit("get_readout_mode_name");
        require_open(camera_id);
        // Matches QHYSDKWrapper::get_readout_mode_name(): an out-of-range
        // index falls back to a synthesized "Mode N" name rather than
        // throwing -- the real SDK call just fails and the wrapper covers
        // for it, it never surfaces InvalidValue here.
        if (mode_index >= readout_modes.size()) {
            return "Mode " + std::to_string(mode_index);
        }
        return readout_modes[mode_index];
    }

    void set_readout_mode(const std::string& camera_id, uint32_t mode_index) override {
        hit("set_readout_mode");
        require_open(camera_id);
        // Matches QHYSDKWrapper::set_readout_mode(): an out-of-range index is
        // an SDK call failure, which check_result() turns into
        // DriverException -- not InvalidValue. Ranged against the SAME
        // floored count get_num_readout_modes() reports, not against
        // readout_modes.size(): otherwise an empty list advertises one mode
        // and then rejects index 0, which no real camera does.
        if (mode_index >= effective_readout_mode_count()) {
            throw AlpacaException("fake: SetQHYCCDReadMode failed (index out of range)", AlpacaError::DriverException);
        }
        readout_mode_ = mode_index;
    }

    std::string get_sdk_version() override {
        hit("get_sdk_version");
        return sdk_version;
    }

private:
    struct ROI {
        uint32_t start_x{};
        uint32_t start_y{};
        uint32_t width{64};
        uint32_t height{48};
    };

    // A std::mutex has no move constructor, so the naive `std::mutex
    // calls_mutex;` member would make FakeQHYSDK non-movable -- and
    // test_qhy_fake_sdk.cpp's own static_assert, plus every helper that
    // builds a fake and returns it by value, depends on movability. A
    // std::unique_ptr<Sync> wrapper would restore movability but leaves the
    // moved-from object's sync_ null, so a stray hit()/call_count() on it
    // (a caller holding a moved-from fake past the move, say) segfaults
    // instead of misbehaving loudly. Sync's own hand-written move
    // constructor sidesteps both: there is nothing meaningful to transfer
    // out of a mutex-only struct, so moving one just re-defaults a fresh
    // mutex in place, and the moved-from object stays fully usable.
    struct Sync {
        mutable std::mutex calls_mutex;

        Sync() = default;
        Sync(Sync&&) noexcept {}
        Sync& operator=(Sync&&) noexcept { return *this; }
        Sync(const Sync&) = delete;
        Sync& operator=(const Sync&) = delete;
    };
    Sync sync_;

    void hit(const char* fn) {
        {
            std::lock_guard<std::mutex> lock(sync_.calls_mutex);
            ++calls[fn];
        }
        if (throw_from.count(fn) != 0) {
            throw AlpacaException(std::string("fake: injected failure in ") + fn, AlpacaError::DriverException);
        }
    }

    void require_resource() const {
        if (!sdk_resource_available) {
            throw AlpacaException("QHY SDK resource not initialized", AlpacaError::DriverException);
        }
    }

    // The mode count every readout-mode method ranges against: the real SDK
    // never reports zero modes, so an empty `readout_modes` still means one.
    // Keeping this in one place is what stops get_num_readout_modes() and
    // set_readout_mode() from disagreeing about whether index 0 is valid.
    uint32_t effective_readout_mode_count() const {
        return std::max<uint32_t>(1, static_cast<uint32_t>(readout_modes.size()));
    }

    bool is_open(const std::string& camera_id) const {
        auto it = ref_counts_.find(camera_id);
        return it != ref_counts_.end() && it->second > 0;
    }

    void require_open(const std::string& camera_id) const {
        if (!is_open(camera_id)) {
            throw AlpacaException("QHY camera not open: " + camera_id, AlpacaError::NotConnected);
        }
    }

    bool known_id(const std::string& id) const {
        for (const auto& cam : cameras) {
            if (cam.camera_id == id) {
                return true;
            }
        }
        return false;
    }

    std::map<std::string, int> ref_counts_;
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> exposure_workers_;
    ROI roi_{};
    uint32_t wbin_ = 1;
    uint32_t hbin_ = 1;
    uint32_t bits_ = 16;
    uint32_t readout_mode_ = 0;
    int cfw_position_ = 0;
};

}  // namespace alpacacore::test
