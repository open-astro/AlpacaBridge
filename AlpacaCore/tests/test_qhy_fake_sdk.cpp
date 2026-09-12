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

// Contract tests for the QHY SDK fake itself (issue #321). The driver tests
// assert on this fake's ledger and fault injection, so the fake's own
// behaviour needs to hold up first -- a fake that miscounts opens would turn
// a real driver leak into a green test.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/qhy/qhy_camera_driver.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "catch2_compat.h"
#include "fake_qhy_sdk.h"
#include "locked_qhy_sdk.h"

using alpacacore::AlpacaException;
using alpacacore::test::FakeQHYSDK;
using alpacacore::test::LockedQHYSDK;
namespace AlpacaError = alpacacore::AlpacaError;
namespace control = alpacacore::vendor::qhy::control;

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

// One-camera fake, shared with the other QHY seam test files (issue #342):
// this was three verbatim copies, so a change to what a default test fake
// looks like had to be made in three places with nothing failing if it was
// made in two.
FakeQHYSDK make_fake(const std::string& id = "fake-qhy-0") { return FakeQHYSDK::with_one_camera(id); }

}  // namespace

TEST_CASE("FakeQHYSDK - opens are reference counted per camera id", "[qhy][fake][unit]") {
    // The wrapper shares ONE physical handle between the camera driver and the
    // paired CFW driver; the fake has to model that or the pairing tests lie.
    auto fake = make_fake();

    fake.open_camera("fake-qhy-0");
    CHECK(fake.physical_opens == 1);
    CHECK(fake.ref_count("fake-qhy-0") == 1);

    fake.open_camera("fake-qhy-0");
    CHECK(fake.physical_opens == 1);  // shared, not reopened
    CHECK(fake.ref_count("fake-qhy-0") == 2);

    fake.close_camera("fake-qhy-0");
    CHECK(fake.physical_closes == 0);  // still held by the other owner
    CHECK(fake.ref_count("fake-qhy-0") == 1);

    fake.close_camera("fake-qhy-0");
    CHECK(fake.physical_closes == 1);
    CHECK(fake.ref_count("fake-qhy-0") == 0);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("FakeQHYSDK - closing an unopened camera is counted, not thrown", "[qhy][fake][unit]") {
    // Driver teardown paths close best-effort and must not have their
    // exceptions swallowed into a false pass; the count is the assertion.
    auto fake = make_fake();
    fake.close_camera("fake-qhy-0");
    CHECK(fake.underflow_closes == 1);
    CHECK(fake.physical_closes == 0);
}

TEST_CASE("FakeQHYSDK - per-camera calls require an open handle", "[qhy][fake][unit]") {
    auto fake = make_fake();
    CHECK_THROWS_AS(fake.init_camera("fake-qhy-0"), AlpacaException);
    CHECK_THROWS_AS(fake.get_param("fake-qhy-0", control::GAIN), AlpacaException);
    CHECK_THROWS_AS(fake.get_cfw_position("fake-qhy-0"), AlpacaException);

    try {
        fake.get_param("fake-qhy-0", control::GAIN);
        FAIL("expected a throw");
    } catch (const AlpacaException& ex) {
        CHECK(ex.error_code() == AlpacaError::NotConnected);
    }
}

TEST_CASE("FakeQHYSDK - opening an unknown id throws DriverException", "[qhy][fake][unit]") {
    // Matches QHYSDKWrapper::open_camera(): OpenQHYCCD returning null throws
    // DriverException, not NotConnected -- the real SDK has no "not
    // connected" concept at this call, only "the open failed".
    auto fake = make_fake();
    try {
        fake.open_camera("no-such-camera");
        FAIL("expected a throw");
    } catch (const AlpacaException& ex) {
        CHECK(ex.error_code() == AlpacaError::DriverException);
    }
    CHECK(fake.physical_opens == 0);
}

TEST_CASE("FakeQHYSDK - throw_from injects a failure into one named call", "[qhy][fake][unit]") {
    auto fake = make_fake();
    fake.throw_from.insert("init_camera");

    fake.open_camera("fake-qhy-0");
    try {
        fake.init_camera("fake-qhy-0");
        FAIL("expected a throw");
    } catch (const AlpacaException& ex) {
        CHECK(ex.error_code() == AlpacaError::DriverException);
    }
    // The call is still counted -- injection happens after the tally, so a
    // test can assert the driver reached the call it was meant to fail on.
    CHECK(fake.call_count("init_camera") == 1);
    CHECK(fake.call_count("open_camera") == 1);
}

TEST_CASE("FakeQHYSDK - an unavailable SDK resource fails the three entry points", "[qhy][fake][unit]") {
    // On real hardware this state is only reachable by segfaulting inside
    // libqhyccd's libusb hotplug init (issue #321), so it has never had a test.
    auto fake = make_fake();
    fake.sdk_resource_available = false;

    std::string model;
    CHECK_THROWS_AS(fake.enumerate_cameras(), AlpacaException);
    CHECK_THROWS_AS(fake.get_camera_model("fake-qhy-0", model), AlpacaException);
    CHECK_THROWS_AS(fake.open_camera("fake-qhy-0"), AlpacaException);
    CHECK(fake.physical_opens == 0);
}

TEST_CASE("FakeQHYSDK - the CFW position script drives transit then settles", "[qhy][fake][unit]") {
    auto fake = make_fake();
    fake.open_camera("fake-qhy-0");
    // -1 is GetQHYCCDCFWStatus's "still moving"; the last entry repeats so a
    // polling driver settles instead of running off the end of the script.
    fake.cfw_position_script = {-1, -1, 3};

    CHECK(fake.get_cfw_position("fake-qhy-0") == -1);
    CHECK(fake.get_cfw_position("fake-qhy-0") == -1);
    CHECK(fake.get_cfw_position("fake-qhy-0") == 3);
    CHECK(fake.get_cfw_position("fake-qhy-0") == 3);
    CHECK(fake.get_cfw_position("fake-qhy-0") == 3);
}

TEST_CASE("FakeQHYSDK - a move clears any pending script", "[qhy][fake][unit]") {
    auto fake = make_fake();
    fake.open_camera("fake-qhy-0");
    fake.cfw_position_script = {-1, -1, 0};
    fake.move_cfw("fake-qhy-0", 4);
    CHECK(fake.last_cfw_target == 4);
    CHECK(fake.get_cfw_position("fake-qhy-0") == 4);
}

TEST_CASE("FakeQHYSDK - chip info comes from the canned camera and never touches model", "[qhy][fake][unit]") {
    // Matches QHYSDKWrapper::get_chip_info(), which never writes info.model at
    // all -- the driver's connect sequence always populates it separately
    // (via get_camera_model(), or a carried-over value) BEFORE calling
    // get_chip_info(), so info.model is never empty in practice. A fake that
    // back-filled it from the canned camera when empty would hide a
    // regression that dropped that earlier call.
    auto fake = make_fake();
    fake.open_camera("fake-qhy-0");

    alpacacore::vendor::qhy::QHYCameraInfo info{};
    REQUIRE(fake.get_chip_info("fake-qhy-0", info));
    CHECK(info.max_width == 64);
    CHECK(info.max_height == 48);
    CHECK(info.bpp == 16);
    CHECK(info.model.empty());  // untouched -- caller never set it

    // The camera driver pre-seeds info.model on reconnect and must not lose it.
    alpacacore::vendor::qhy::QHYCameraInfo preset{};
    preset.model = "Remembered";
    REQUIRE(fake.get_chip_info("fake-qhy-0", preset));
    CHECK(preset.model == "Remembered");
    CHECK(preset.max_width == 64);
}

TEST_CASE("FakeQHYSDK - frame length follows the configured ROI and bit depth", "[qhy][fake][unit]") {
    auto fake = make_fake();
    fake.open_camera("fake-qhy-0");

    fake.set_bits_mode("fake-qhy-0", 16);
    fake.set_resolution("fake-qhy-0", 0, 0, 64, 48);
    CHECK(fake.get_mem_length("fake-qhy-0") == 64U * 48U * 2U);

    fake.set_bits_mode("fake-qhy-0", 8);
    CHECK(fake.get_mem_length("fake-qhy-0") == 64U * 48U);
}

TEST_CASE("FakeQHYSDK - get_single_frame fills the buffer and returns immediately", "[qhy][fake][unit]") {
    // "Immediately" is the contract that keeps the driver's DETACHABLE
    // exposure worker from outliving the fake -- see rule 1 on FakeQHYSDK.
    auto fake = make_fake();
    fake.open_camera("fake-qhy-0");
    fake.set_resolution("fake-qhy-0", 0, 0, 4, 2);
    fake.set_bits_mode("fake-qhy-0", 16);

    std::vector<uint8_t> buf(4 * 2 * 2, 0xAB);
    uint32_t w = 0, h = 0, bpp = 0, ch = 0;
    CHECK(fake.get_single_frame("fake-qhy-0", buf.data(), w, h, bpp, ch));
    CHECK(w == 4);
    CHECK(h == 2);
    CHECK(bpp == 16);
    CHECK(ch == 1);
    CHECK(buf[0] == 0);
    CHECK(buf.back() == 0);

    fake.frame_ok = false;
    CHECK_FALSE(fake.get_single_frame("fake-qhy-0", buf.data(), w, h, bpp, ch));
}

TEST_CASE("FakeQHYSDK - default camera reports no cooler", "[qhy][fake][unit]") {
    // has_cooler starts the driver's telemetry thread, whose 1s poll makes
    // every disconnect block on the join. The cooled variant is opt-in.
    CHECK_FALSE(FakeQHYSDK::default_camera("id", "m").has_cooler);
    CHECK(FakeQHYSDK::default_cooled_camera("id", "m").has_cooler);
}

TEST_CASE("LockedQHYSDK - forwards to the inner SDK", "[qhy][fake][unit]") {
    auto fake = make_fake();
    LockedQHYSDK sdk(fake);

    sdk.open_camera("fake-qhy-0");
    sdk.init_camera("fake-qhy-0");
    CHECK(fake.physical_opens == 1);
    CHECK(fake.init_calls == 1);
    CHECK(sdk.get_sdk_version() == "fake-qhy-1.0");
    CHECK(sdk.enumerate_cameras().size() == 1);

    sdk.close_camera("fake-qhy-0");
    CHECK(fake.physical_closes == 1);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("FakeQHYSDK - is movable so helpers can build one and return it", "[qhy][fake][unit]") {
    static_assert(std::is_move_constructible<FakeQHYSDK>::value,
                  "tests build a fake in a helper and return it by value");
    auto fake = make_fake();
    CHECK(fake.cameras.size() == 1);
}

TEST_CASE("FakeQHYSDK - a moved-from instance is still safely usable", "[qhy][fake][unit]") {
    // The sync member holds only a mutex -- nothing meaningful to move out --
    // so a moved-from fake must stay fully functional (its own collections,
    // like `cameras`, are moved away by the ordinary member-wise move; only
    // sync_ is special-cased to survive) rather than segfaulting on its next
    // call, which a std::unique_ptr<Sync> nulled by the move would have done.
    FakeQHYSDK source = make_fake();
    FakeQHYSDK moved_to = std::move(source);

    // get_sdk_version() only touches sync_ (via hit()) and a literal, so it
    // exercises exactly the path that used to null-deref, without depending
    // on `cameras` (moved away, as ordinary vector move semantics dictate).
    REQUIRE_NOTHROW(source.get_sdk_version());
    // >= 1, not == 1: `calls` is a moved-from standard container, so it is
    // "valid but unspecified" rather than guaranteed empty. The guarantee this
    // case actually tests is that sync_ survived the move and hit() ran, which
    // >= 1 states without leaning on a libstdc++/libc++ implementation detail.
    CHECK(source.call_count("get_sdk_version") >= 1);
}

TEST_CASE("FakeQHYSDK - cancel_exposure on a closed handle is a silent no-op", "[qhy][fake][unit]") {
    // Matches QHYSDKWrapper::cancel_exposure(): a missing/closed handle
    // returns silently rather than throwing NotConnected -- this is the
    // SDK's own mechanism for interrupting a call already blocked on the same
    // handle from another thread, so it can't afford to throw on a handle a
    // racing close() just erased.
    auto fake = make_fake();
    REQUIRE_NOTHROW(fake.cancel_exposure("fake-qhy-0"));
    CHECK(fake.call_count("cancel_exposure") == 1);
}

TEST_CASE("FakeQHYSDK - get_num_readout_modes floors at 1", "[qhy][fake][unit]") {
    // Matches QHYSDKWrapper::get_num_readout_modes(): "at least one mode",
    // never zero.
    auto fake = make_fake();
    fake.open_camera("fake-qhy-0");
    fake.readout_modes.clear();
    CHECK(fake.get_num_readout_modes("fake-qhy-0") == 1);
}

TEST_CASE("FakeQHYSDK - an out-of-range readout mode name falls back, set fails", "[qhy][fake][unit]") {
    // Matches QHYSDKWrapper: get_readout_mode_name() synthesizes "Mode N" for
    // an index the SDK call doesn't recognize (it never throws InvalidValue
    // here); set_readout_mode() on the same index is an SDK call failure,
    // which check_result() turns into DriverException.
    auto fake = make_fake();
    fake.open_camera("fake-qhy-0");
    REQUIRE(fake.readout_modes.size() == 2);

    CHECK(fake.get_readout_mode_name("fake-qhy-0", 5) == "Mode 5");
    try {
        fake.set_readout_mode("fake-qhy-0", 5);
        FAIL("expected a throw");
    } catch (const AlpacaException& ex) {
        CHECK(ex.error_code() == AlpacaError::DriverException);
    }
}

TEST_CASE("LockedQHYSDK - every method forwards to its own counterpart", "[qhy][fake][unit]") {
    // The decorator is 26 hand-written one-line forwards, so a transposed
    // body (move_cfw calling get_cfw_position, say) compiles and would only
    // surface as a baffling failure in the [stress] follow-up, which is where
    // LockedQHYSDK is actually load-bearing. Walk all 26 through the decorator
    // and assert the fake's call ledger names exactly them, once each: a
    // mis-wired forward shows up as one name at 2 and another at 0.
    auto fake = make_fake();
    LockedQHYSDK sdk(fake);

    const std::string id = "fake-qhy-0";
    sdk.enumerate_cameras();
    std::string model;
    sdk.get_camera_model(id, model);
    sdk.open_camera(id);
    sdk.init_camera(id);
    sdk.register_exposure_worker(id, std::make_shared<std::atomic<bool>>(false));
    alpacacore::vendor::qhy::QHYCameraInfo info{};
    sdk.get_chip_info(id, info);
    sdk.is_control_available(id, control::GAIN);
    sdk.get_param(id, control::GAIN);
    sdk.get_param_range(id, control::GAIN);
    sdk.set_param(id, control::GAIN, 10.0);
    sdk.set_resolution(id, 0, 0, 8, 8);
    sdk.set_bin_mode(id, 1, 1);
    sdk.set_bits_mode(id, 16);
    sdk.get_mem_length(id);
    sdk.start_single_frame(id);
    std::vector<uint8_t> buffer(1024 * 1024, 0);
    uint32_t w = 0, h = 0, bpp = 0, channels = 0;
    sdk.get_single_frame(id, buffer.data(), w, h, bpp, channels);
    sdk.cancel_exposure(id);
    sdk.guide(id, 0, 10);
    sdk.control_temp(id, -5.0);
    sdk.move_cfw(id, 1);
    sdk.get_cfw_position(id);
    sdk.get_num_readout_modes(id);
    sdk.get_readout_mode_name(id, 0);
    sdk.set_readout_mode(id, 0);
    sdk.get_sdk_version();
    sdk.close_camera(id);

    // open-astro#339, the other half: the sweep above is the one place every
    // forward goes through the decorator, so it is where the no-blocking rule
    // is checked for ALL of them. A 2 s sleep added to any fake method (guide,
    // control_temp, get_single_frame, set_readout_mode, ...) fails here by
    // name instead of hanging the first [stress] run that reaches it.
    INFO("slowest forward: " << sdk.slowest_call_ms() << " ms");
    CHECK(sdk.slowest_call_ms() < 100);

    const std::vector<std::string> methods{
        "enumerate_cameras",
        "get_camera_model",
        "open_camera",
        "init_camera",
        "close_camera",
        "register_exposure_worker",
        "get_chip_info",
        "is_control_available",
        "get_param",
        "get_param_range",
        "set_param",
        "set_resolution",
        "set_bin_mode",
        "set_bits_mode",
        "get_mem_length",
        "start_single_frame",
        "get_single_frame",
        "cancel_exposure",
        "guide",
        "control_temp",
        "move_cfw",
        "get_cfw_position",
        "get_num_readout_modes",
        "get_readout_mode_name",
        "set_readout_mode",
        "get_sdk_version",
    };
    // A literal self-check on the list. It is NOT what catches the interface
    // growing -- if QHYSDK gains a 27th method, LockedQHYSDK must gain a
    // forward to still compile, but this list, the driven calls and
    // fake.calls would all stay at 26 and the case would still pass, having
    // exercised 26 of 27. That hole is closed OUTSIDE this file (issue #394):
    // scripts/check_docs_drift.py compares the interface's pure virtuals,
    // LockedQHYSDK's overrides and this very list, and separately fails on a
    // forward that does not go through locked() -- which the compiler never
    // checks and which is the only reason the decorator exists. Update the
    // number here and the list above in the same change as the interface.
    // What this CASE catches (mutation-verified) is a forward wired to the
    // wrong inner method, or to none.
    CHECK(methods.size() == 26);
    for (const auto& name : methods) {
        INFO("method: " << name);
        CHECK(fake.call_count(name.c_str()) == 1);
    }
    // Nothing OUTSIDE the list was hit, which is the half that catches a
    // forward wired to the wrong inner method.
    CHECK(fake.distinct_calls() == methods.size());
}

TEST_CASE("FakeQHYSDK - an empty readout-mode list still accepts index 0", "[qhy][fake][unit]") {
    // get_num_readout_modes() floors at 1, so the fake advertises one mode
    // even with no canned names. set_readout_mode() must range against that
    // SAME floored count -- otherwise the fake reports a mode it then refuses
    // to select, which no real camera does.
    auto fake = make_fake();
    fake.readout_modes.clear();
    fake.open_camera("fake-qhy-0");

    REQUIRE(fake.get_num_readout_modes("fake-qhy-0") == 1);
    CHECK_NOTHROW(fake.set_readout_mode("fake-qhy-0", 0));
    // One past the advertised count is still a DriverException.
    CHECK_THROWS_AS(fake.set_readout_mode("fake-qhy-0", 1), AlpacaException);
    // The synthesized name is unchanged by the floor.
    CHECK(fake.get_readout_mode_name("fake-qhy-0", 0) == "Mode 0");
}

TEST_CASE("FakeQHYSDK - get_param answers the QHYCCD_ERROR sentinel for an unsupported control", "[qhy][fake][unit]") {
    // Parity with the real wrapper, which returns GetQHYCCDParam() raw
    // (issue #373). The fake used to answer 0.0, and 0.0 is the more dangerous
    // of the two: modelling "this camera does not support that control" by
    // dropping it from `params` handed the driver a plausible reading where
    // hardware hands it a sentinel it must reject. The two answers differ by
    // about four billion on the same input.
    auto fake = make_fake();
    fake.open_camera("fake-qhy-0");

    fake.params[control::GAIN] = 42.0;
    CHECK(fake.get_param("fake-qhy-0", control::GAIN) == 42.0);

    // A control nobody seeded. The constant is exposed so a case can say what
    // it means rather than spelling 0xFFFFFFFF out.
    const int unsupported = 9999;
    REQUIRE(fake.params.count(unsupported) == 0);
    CHECK(fake.get_param("fake-qhy-0", unsupported) == FakeQHYSDK::kUnsupportedControl);
    CHECK(FakeQHYSDK::kUnsupportedControl > 4.0e9);

    // Seeding it explicitly with 0.0 is how a case says "supported, reads
    // zero" -- which is what the old behaviour could not distinguish.
    fake.params[unsupported] = 0.0;
    CHECK(fake.get_param("fake-qhy-0", unsupported) == 0.0);
}

TEST_CASE("FakeQHYSDK - get_mem_length tracks the ROI, which the driver binds", "[qhy][fake][unit]") {
    // GetQHYCCDMemLength() shrinks when the frame does (issue #365), and the
    // fake stored wbin_/hbin_ without reading them. The fix is NOT to divide
    // here: roi_ is already in BINNED pixels, because that is what the driver
    // passes. NOTE the set_bin_mode() calls below are no-ops by construction --
    // nothing reads wbin_/hbin_ (the re-scoped #365 residual) -- so this case
    // covers the ROI path, not the binning path; it is written the way the
    // driver drives the fake, and it would fail if get_mem_length() ever
    // started dividing by the binning as well. The driver -- set_bin_locked() calls set_resolution(0, 0, max_width /
    // bin_x, max_height / bin_y), and start_exposure() passes num_x_/num_y_, which are ASCOM NumX/NumY. Dividing again
    // would report a quarter of the bytes the SDK owes for the frame it is about to deliver.
    auto fake = make_fake();
    fake.open_camera("fake-qhy-0");
    fake.set_bits_mode("fake-qhy-0", 16);

    // Unbinned: the driver would pass the full sensor size.
    fake.set_resolution("fake-qhy-0", 0, 0, 64, 48);
    fake.set_bin_mode("fake-qhy-0", 1, 1);
    const uint32_t unbinned = fake.get_mem_length("fake-qhy-0");
    CHECK(unbinned == 64U * 48U * 2U);

    // 2x2: the driver passes the halved ROI, so the length quarters.
    fake.set_resolution("fake-qhy-0", 0, 0, 32, 24);
    fake.set_bin_mode("fake-qhy-0", 2, 2);
    CHECK(fake.get_mem_length("fake-qhy-0") == unbinned / 4);

    // Asymmetric binning halves one axis only.
    fake.set_resolution("fake-qhy-0", 0, 0, 32, 48);
    fake.set_bin_mode("fake-qhy-0", 2, 1);
    CHECK(fake.get_mem_length("fake-qhy-0") == unbinned / 2);

    // 8-bit readout halves it again, independently of the binning.
    fake.set_bits_mode("fake-qhy-0", 8);
    fake.set_resolution("fake-qhy-0", 0, 0, 32, 24);
    fake.set_bin_mode("fake-qhy-0", 2, 2);
    CHECK(fake.get_mem_length("fake-qhy-0") == 32U * 24U);
}

TEST_CASE("FakeQHYSDK - the two capability sources agree in the default seeding", "[qhy][fake][unit]") {
    // Since get_param() answers the QHYCCD_ERROR sentinel for anything missing
    // from `params` (issue #373), membership there reads as "supported" -- so a
    // control seeded in `params` but absent from `controls_available` makes the
    // fake's two capability sources contradict each other in the DEFAULT
    // scaffolding every QHY case builds on, which is the trap the
    // get_chip_info() parity gap warns about. CURTEMP and CURPWM were exactly
    // that. This case fails if either list drops one of them.
    auto fake = make_fake();
    fake.open_camera("fake-qhy-0");

    for (const int ctrl : {alpacacore::vendor::qhy::control::CURTEMP, alpacacore::vendor::qhy::control::CURPWM}) {
        INFO("control " << ctrl);
        CHECK(fake.is_control_available("fake-qhy-0", ctrl));
        CHECK(fake.get_param("fake-qhy-0", ctrl) != alpacacore::test::FakeQHYSDK::kUnsupportedControl);
    }
}

TEST_CASE("FakeQHYSDK - get_single_frame never exceeds get_mem_length", "[qhy][fake][unit]") {
    // The driver sizes its frame buffer from get_mem_length() and hands that
    // exact buffer to get_single_frame() (qhy_camera_driver.cpp: mem_length ->
    // local_buf(mem_length) -> get_single_frame). Hardware cannot deliver an
    // image larger than GetQHYCCDMemLength(), so the fake must not either.
    // When get_mem_length() divided by the binning and get_single_frame() did
    // not, this pairing was a heap-buffer-overflow inside the fake (ASan: a
    // 384-byte buffer memset with 1536 bytes) -- a crash in test scaffolding
    // that would have read as a driver bug.
    for (const auto bin : {1U, 2U, 4U}) {
        auto fake = make_fake();
        fake.open_camera("fake-qhy-0");
        fake.set_bits_mode("fake-qhy-0", 16);
        fake.set_resolution("fake-qhy-0", 0, 0, 64U / bin, 48U / bin);
        fake.set_bin_mode("fake-qhy-0", bin, bin);

        const uint32_t mem_length = fake.get_mem_length("fake-qhy-0");
        std::vector<uint8_t> buffer(mem_length, 0);
        uint32_t width = 0, height = 0, bpp = 0, channels = 0;
        REQUIRE(fake.get_single_frame("fake-qhy-0", buffer.data(), width, height, bpp, channels));

        const uint32_t delivered = width * height * ((bpp > 8) ? 2U : 1U);
        INFO("bin " << bin << ": mem_length " << mem_length << ", frame " << width << "x" << height);
        CHECK(delivered == mem_length);
    }
}

TEST_CASE("FakeQHYSDK - control_temp converges over calls instead of settling instantly", "[qhy][fake][unit]") {
    // ControlQHYCCDTemp is a PID that converges over many calls, which is why
    // the driver polls it about once a second (issue #390). The fake wrote the
    // target straight into CURTEMP, so a thermal test could assert an instant
    // settle that hardware can never produce -- and a driver that only ever
    // read back its own setpoint would have looked correct.
    auto fake = FakeQHYSDK::with_one_cooled_camera();
    fake.open_camera("fake-qhy-0");
    fake.params[control::CURTEMP] = 20.0;
    fake.temp_settle_step_c = 5.0;

    fake.control_temp("fake-qhy-0", 0.0);
    CHECK(fake.last_temp_target == 0.0);
    CHECK(fake.get_param("fake-qhy-0", control::CURTEMP) == 15.0);

    fake.control_temp("fake-qhy-0", 0.0);
    CHECK(fake.get_param("fake-qhy-0", control::CURTEMP) == 10.0);

    // It never overshoots: the last step lands exactly on the target and
    // further calls are no-ops.
    for (int i = 0; i < 10; ++i) {
        fake.control_temp("fake-qhy-0", 0.0);
    }
    CHECK(fake.get_param("fake-qhy-0", control::CURTEMP) == 0.0);

    // Warming works the same way in the other direction.
    fake.control_temp("fake-qhy-0", 20.0);
    CHECK(fake.get_param("fake-qhy-0", control::CURTEMP) == 5.0);

    // A case that wants the old instant settle opts out explicitly.
    fake.temp_settle_step_c = 0.0;
    fake.control_temp("fake-qhy-0", -30.0);
    CHECK(fake.get_param("fake-qhy-0", control::CURTEMP) == -30.0);
}

TEST_CASE("FakeQHYSDK - with_one_camera is the shared one-camera setup", "[qhy][fake][unit]") {
    // The helper the three QHY seam test files now share (issue #342). Pinned
    // here so a change to what a default test fake looks like is visible in
    // one place instead of drifting between three verbatim copies.
    auto fake = FakeQHYSDK::with_one_camera();
    REQUIRE(fake.cameras.size() == 1);
    CHECK(fake.cameras[0].camera_id == "fake-qhy-0");
    CHECK(fake.cameras[0].model == "FakeQHY600");
    CHECK_FALSE(fake.cameras[0].has_cooler);

    auto named = FakeQHYSDK::with_one_camera("other-id", "FakeQHY268");
    REQUIRE(named.cameras.size() == 1);
    CHECK(named.cameras[0].camera_id == "other-id");
    CHECK(named.cameras[0].model == "FakeQHY268");

    // The cooled variant differs in exactly one field.
    auto cooled = FakeQHYSDK::with_one_cooled_camera();
    REQUIRE(cooled.cameras.size() == 1);
    CHECK(cooled.cameras[0].has_cooler);
    CHECK(cooled.cameras[0].max_width == fake.cameras[0].max_width);
}

TEST_CASE("FakeQHYSDK - get_single_frame never writes past what get_mem_length promised", "[qhy][fake][unit]") {
    // open-astro#328. The driver's exposure worker sizes its buffer once from
    // get_mem_length(), then calls get_single_frame() to fill it. LockedQHYSDK
    // serialises individual calls but NOT that three-step sequence, so a
    // [stress] storm can land a set_resolution() in the window between them.
    //
    // Sized deliberately: the buffer is allocated for the SMALL frame, then
    // the ROI is widened behind the caller's back. Before the fix the memset
    // used the current (larger) ROI and ran off the end of the allocation --
    // a real out-of-bounds write, in the test double rather than in the code
    // under test. ASan reports it; this case makes it a plain assertion too.
    auto fake = FakeQHYSDK::with_one_camera();
    fake.open_camera("fake-qhy-0");
    fake.set_resolution("fake-qhy-0", 0, 0, 8, 8);
    fake.set_bits_mode("fake-qhy-0", 8);

    const uint32_t promised = fake.get_mem_length("fake-qhy-0");
    CHECK(promised == 8U * 8U);

    // The caller allocates for exactly what it was promised, with a guard
    // pattern after it: anything written past `promised` lands in the tail.
    std::vector<uint8_t> buffer(promised + 64, 0xAB);

    // The storm widens the ROI after the allocation.
    fake.set_resolution("fake-qhy-0", 0, 0, 32, 32);

    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t bpp = 0;
    uint32_t channels = 0;
    REQUIRE(fake.get_single_frame("fake-qhy-0", buffer.data(), w, h, bpp, channels));

    // The tail is untouched: the write was clamped to the promise, not to the
    // fake's current state.
    for (std::size_t i = promised; i < buffer.size(); ++i) {
        REQUIRE(buffer[i] == 0xAB);
    }

    // And the normal path still fills the whole frame when nothing raced: a
    // fresh get_mem_length() re-promises the larger size.
    const uint32_t repromised = fake.get_mem_length("fake-qhy-0");
    CHECK(repromised == 32U * 32U);
    std::vector<uint8_t> big(repromised + 64, 0xAB);
    REQUIRE(fake.get_single_frame("fake-qhy-0", big.data(), w, h, bpp, channels));
    CHECK(big[0] == 0x00);
    CHECK(big[repromised - 1] == 0x00);
    for (std::size_t i = repromised; i < big.size(); ++i) {
        REQUIRE(big[i] == 0xAB);
    }

    fake.close_camera("fake-qhy-0");
}

TEST_CASE("FakeQHYSDK - open_camera refuses while a zombie exposure worker is live", "[qhy][fake][unit]") {
    // open-astro#324. QHYSDKWrapper::open_camera() refuses a fresh open while
    // a previously registered exposure worker's flag is still true: that
    // worker may have timed out its join and been DETACHED, leaving it blocked
    // inside GetQHYCCDSingleFrame on the old handle (the PR #201 finding).
    //
    // exposure_workers_ was written and never read, so the fake could not fail
    // a reconnect storm here -- and a reconnect storm is the single most likely
    // [stress] scenario for this driver, i.e. the guard most likely to regress
    // with every test still green.
    auto fake = FakeQHYSDK::with_one_camera();
    auto running = std::make_shared<std::atomic<bool>>(true);

    fake.open_camera("fake-qhy-0");
    fake.register_exposure_worker("fake-qhy-0", running);
    fake.close_camera("fake-qhy-0");
    const int opens_before = fake.physical_opens;

    require_alpaca_error([&] { fake.open_camera("fake-qhy-0"); }, alpacacore::AlpacaError::InvalidOperation);
    // Refused BEFORE any second physical open.
    CHECK(fake.physical_opens == opens_before);

    // Once the worker finishes, the reopen goes through.
    running->store(false);
    fake.open_camera("fake-qhy-0");
    CHECK(fake.physical_opens == opens_before + 1);
    fake.close_camera("fake-qhy-0");
}

TEST_CASE("FakeQHYSDK - a shared open during a live exposure still succeeds", "[qhy][fake][unit]") {
    // The other half of the #324 refusal, from the review on PR #463.
    // QHYSDKWrapper::open_camera() reuses a live handle FIRST (open_count > 0
    // just bumps the count) and consults the zombie-worker flag only when
    // there is no handle to reuse. The camera + CFW pairing AGENTS.md
    // documents relies on that: the wheel connecting while the camera is
    // mid-exposure is an ordinary shared open, not a reopen over a zombie.
    // The fake once checked the flag before the reuse branch, so that
    // pairing threw InvalidOperation here where production returns.
    auto fake = FakeQHYSDK::with_one_camera();
    auto running = std::make_shared<std::atomic<bool>>(true);

    fake.open_camera("fake-qhy-0");                        // camera driver connects
    fake.register_exposure_worker("fake-qhy-0", running);  // exposure in flight
    const int opens_before = fake.physical_opens;

    CHECK_NOTHROW(fake.open_camera("fake-qhy-0"));  // paired CFW driver connects
    CHECK(fake.physical_opens == opens_before);     // reused, not reopened

    fake.close_camera("fake-qhy-0");
    fake.close_camera("fake-qhy-0");
    // With every owner gone the refusal applies again.
    require_alpaca_error([&] { fake.open_camera("fake-qhy-0"); }, alpacacore::AlpacaError::InvalidOperation);
}

TEST_CASE("FakeQHYSDK - move_cfw enforces the single-digit protocol ceiling", "[qhy][fake][unit]") {
    // open-astro#324 (absorbed from #327). The CFW wire protocol is one ASCII
    // digit, so the real wrapper throws InvalidValue above 9. The fake took
    // any int, so the filter-wheel case "A slot above the protocol ceiling
    // issues no move" was passing on the DRIVER's own copy of the guard and
    // would have stayed green if that copy were narrowed.
    auto fake = FakeQHYSDK::with_one_camera();
    fake.open_camera("fake-qhy-0");

    fake.move_cfw("fake-qhy-0", 9);
    CHECK(fake.last_cfw_target == 9);

    require_alpaca_error([&] { fake.move_cfw("fake-qhy-0", 10); }, alpacacore::AlpacaError::InvalidValue);
    // Refused without recording a target.
    CHECK(fake.last_cfw_target == 9);

    fake.close_camera("fake-qhy-0");

    // Same order as QHYSDKWrapper::move_cfw(): the ceiling is checked before
    // the handle lookup, so a CLOSED camera at position 10 answers
    // InvalidValue, not NotConnected (review round 5 on PR #463 -- with
    // require_open() first this reads NotConnected and nothing else notices).
    require_alpaca_error([&] { fake.move_cfw("fake-qhy-0", 10); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&] { fake.move_cfw("fake-qhy-0", 3); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("LockedQHYSDK - records the slowest forward so a blocking fake is nameable", "[qhy][fake][unit]") {
    // open-astro#339. "No QHY fake method may block" was prose in two headers,
    // and two things depend on it: the camera driver's detachable workers (a
    // blocking fake turns a timed-out join into a detached thread still
    // calling into a fake the test body has destroyed) and cancel_exposure's
    // exemption below. A future fake method gaining a reasonable-looking
    // sleep_for broke both, and the symptom was a HUNG [stress] run -- no
    // assertion to fail, nothing pointing at the offending method.
    auto fake = FakeQHYSDK::with_one_camera();
    LockedQHYSDK sdk(fake);

    CHECK(sdk.slowest_call_ms() == 0);

    for (int i = 0; i < 50; ++i) {
        sdk.open_camera("fake-qhy-0");
        static_cast<void>(sdk.get_mem_length("fake-qhy-0"));
        sdk.close_camera("fake-qhy-0");
    }

    // Deliberately generous: every fake method is pure bookkeeping, so this is
    // orders of magnitude of headroom rather than a timing assertion. The
    // point is to catch a method that has gained a real block, not to measure
    // one that has not -- a tight bound here would only buy CI flakes.
    CHECK(sdk.slowest_call_ms() < 100);

    // And it really does measure: a method that blocks is reported, which is
    // what turns "the [stress] run hung" into "this call took 250 ms". Without
    // this half the assertion above could never fail.
    fake.before_call = [](const std::string& name) {
        if (name == "get_mem_length") {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    };
    sdk.open_camera("fake-qhy-0");
    static_cast<void>(sdk.get_mem_length("fake-qhy-0"));
    sdk.close_camera("fake-qhy-0");
    fake.before_call = nullptr;
    CHECK(sdk.slowest_call_ms() >= 200);
}

TEST_CASE("LockedQHYSDK - the cancel forward is timed by its own Guard", "[qhy][fake][unit]") {
    // cancel_exposure() is the one forward outside locked(), so it carries
    // its own Guard (review round 4 on PR #463). Without this case, deleting
    // that Guard left every test green while the watchdog silently stopped
    // covering the one method production most needs to stay non-blocking.
    auto fake = FakeQHYSDK::with_one_camera();
    LockedQHYSDK sdk(fake);
    sdk.open_camera("fake-qhy-0");
    CHECK(sdk.slowest_call_ms() < 100);  // bookkeeping only so far; the >= 200 below is the signal

    fake.before_call = [](const std::string& name) {
        if (name == "cancel_exposure") {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    };
    sdk.cancel_exposure("fake-qhy-0");
    fake.before_call = nullptr;
    CHECK(sdk.slowest_call_ms() >= 200);
    sdk.close_camera("fake-qhy-0");
}

TEST_CASE("LockedQHYSDK - cancel_exposure forwards without taking the decorator's mutex", "[qhy][fake][unit]") {
    // open-astro#339, the half that matters most. QHYSDKWrapper::cancel_exposure()
    // deliberately SKIPS the per-handle call mutex, because its whole job is to
    // interrupt a GetQHYCCDSingleFrame already blocked on the same handle from
    // another thread. The decorator forwarded it through its one mutex, which
    // inverted exactly that invariant: the safety valve would have queued
    // behind the call it exists to interrupt.
    //
    // Driven with the decorator's mutex deliberately held by another thread,
    // which is the shape production has to survive. Before the fix this
    // deadlocks; the bounded wait turns that into a failure rather than a hang.
    auto fake = FakeQHYSDK::with_one_camera();
    LockedQHYSDK sdk(fake);
    sdk.open_camera("fake-qhy-0");

    std::atomic<bool> holder_in{false};
    std::atomic<bool> release{false};
    std::atomic<bool> cancelled{false};

    // Hold the decorator's mutex from inside a forward, the way an in-flight
    // get_single_frame would.
    fake.before_call = [&](const std::string& name) {
        if (name == "get_mem_length") {
            holder_in.store(true);
            while (!release.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    };

    std::thread holder([&] { static_cast<void>(sdk.get_mem_length("fake-qhy-0")); });
    while (!holder_in.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::thread canceller([&] {
        sdk.cancel_exposure("fake-qhy-0");
        cancelled.store(true);
    });

    // The cancel must complete while the other call still holds the mutex.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!cancelled.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(cancelled.load());

    release.store(true);
    holder.join();
    canceller.join();
    fake.before_call = nullptr;
    sdk.close_camera("fake-qhy-0");
}

TEST_CASE("QHYSeamFixture - owns the fake, decorator and driver in destruction order", "[qhy][fake][unit]") {
    // open-astro#338. Every QHY seam test must outlive the driver with the
    // fake: the driver holds a QHYSDK& and its detachable workers capture a
    // raw QHYSDK*. A case that declares them the other way round, or stashes
    // the driver anywhere outliving the fake, compiles cleanly and is
    // undefined behaviour -- most likely a use-after-free inside a detached
    // worker, the failure mode hardest to attribute when it surfaces.
    //
    // The rule was stated in three places and enforced by nothing. This
    // fixture makes it unwritable: member order gives the destruction order
    // once, here, instead of every case getting it right.
    alpacacore::test::QHYSeamFixture<alpacacore::CameraDriver> rig(
        FakeQHYSDK::with_one_camera(), [](alpacacore::test::LockedQHYSDK& sdk) {
            return alpacacore::vendor::qhy::create_qhy_camera(0, "fake-qhy-0", sdk);
        });

    rig->set_connected(true);
    REQUIRE(rig->get_connected());
    CHECK(rig.fake().physical_opens == 1);
    rig->set_connected(false);

    // The destructor runs driver -> decorator -> fake. Under ASan a reversed
    // member order here is a use-after-free at scope exit; the assertion above
    // is what makes an ordinary run notice the fixture works at all.
}
