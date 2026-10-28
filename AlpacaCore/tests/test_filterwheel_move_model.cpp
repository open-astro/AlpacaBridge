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

// One set of filter wheel move-model cases, run unchanged over the ZWO EFW driver (fake EFW SDK) and the QHY CFW3
// driver (fake CFW3 firmware on a pty). The case bodies are templates over a rig; a rig only builds the driver,
// scripts a slow move and counts device reads, so a divergence between the drivers shows as a failing
// instantiation, not as a different body. The testing instructions record the result and the no-extraction rule.

#include <alpacacore/filterwheel_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/qhy/qhy_cfw3_filterwheel_driver.h>
#include <alpacacore/vendor/qhy/qhy_cfw3_protocol_wrapper.h>
#include <alpacacore/vendor/zwo/zwo_efw_sdk.h>
#include <alpacacore/vendor/zwo/zwo_filterwheel_driver.h>

#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_qhy_cfw3.h"
#include "fake_zwo_efw_sdk.h"

namespace {

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::AlpacaException;
using alpacacore::test::require_alpaca_error;

constexpr int kSlots = 7;

// FakeZWOEFWSDK with a scripted transit: after a set_position() it answers the queued slots first (the slot the
// wheel left, then the ones it passes), then the fake's own answer (-1 for its moving window, then the target).
class TransitZwoSdk final : public alpacacore::vendor::zwo::ZWOEFWSDK {
public:
    alpacacore::test::FakeZWOEFWSDK fake;
    std::vector<int> transit;

    using Info = alpacacore::vendor::zwo::ZWOEFWInfo;
    std::vector<Info> enumerate_wheels() override { return fake.enumerate_wheels(); }
    bool get_wheel_info_by_id(int id, Info& info) override { return fake.get_wheel_info_by_id(id, info); }
    bool get_wheel_info_by_index(int index, Info& info) override { return fake.get_wheel_info_by_index(index, info); }
    void open_wheel(int id) override { fake.open_wheel(id); }
    void close_wheel(int id) override { fake.close_wheel(id); }
    int get_position(int id) override {
        if (next_ < queued_.size()) return queued_[next_++];
        return fake.get_position(id);
    }
    void set_position(int id, int slot) override {
        queued_.clear();
        next_ = 0;
        const int from = fake.position;
        fake.set_position(id, slot);
        if (!transit.empty()) {
            queued_.push_back(from);
            queued_.insert(queued_.end(), transit.begin(), transit.end());
        }
    }
    void set_unidirectional(int id, bool enabled) override { fake.set_unidirectional(id, enabled); }
    bool get_unidirectional(int id) override { return fake.get_unidirectional(id); }
    void calibrate(int id) override { fake.calibrate(id); }
    int get_hardware_error_code(int id) override { return fake.get_hardware_error_code(id); }
    std::string get_serial_number(int id) override { return fake.get_serial_number(id); }
    std::string get_firmware_version(int id) override { return fake.get_firmware_version(id); }
    std::string get_sdk_version() override { return fake.get_sdk_version(); }

private:
    std::vector<int> queued_;
    std::size_t next_{0};
};

struct ZwoRig {
    TransitZwoSdk sdk;
    std::unique_ptr<alpacacore::FilterWheelDriver> driver;

    ZwoRig() {
        sdk.fake.slot_count = kSlots;
        driver = alpacacore::vendor::zwo::create_zwo_efw_filterwheel(0, sdk.fake.id, sdk);
    }
    void connect_at(int slot) {
        sdk.fake.position = slot;
        driver->set_connected(true);
    }
    // The next move reports -1 for a few reads, as the EFW SDK does while the wheel turns.
    void slow_moves() { sdk.fake.moving_reads = 3; }
    // The next move also reports the pre-move slot and slots on the way before the target.
    void transit_through(const std::vector<int>& slots) { sdk.transit = slots; }
    int device_reads() { return sdk.fake.position_calls; }
};

struct QhyRig {
    alpacacore::test::FakeQhyCfw3 fake{kSlots};
    std::unique_ptr<alpacacore::FilterWheelDriver> driver;

    QhyRig() {
        alpacacore::vendor::qhy::Cfw3Settings s;
        s.boot_timeout_ms = 150;
        s.reply_timeout_ms = 300;
        s.move_timeout_ms = 3000;
        driver = alpacacore::vendor::qhy::create_qhy_cfw3_filterwheel(0, fake.slave_path(), s);
    }
    void connect_at(int slot) {
        fake.set_position(slot);
        driver->set_connected(true);
    }
    // The wheel stays silent for 150 ms per slot, then answers the arrival.
    void slow_moves() { fake.set_travel_ms(150); }
    // The firmware answers only on arrival, so the driver never sees a transit slot: nothing to script.
    void transit_through(const std::vector<int>&) { slow_moves(); }
    int device_reads() {
        for (int i = 0; i < 200 && !fake.quiescent(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return fake.count("NOW");
    }
};

// Every value Position reports until it stops reading -1 (or the budget ends); the last entry is the settled one.
std::vector<int> positions_until_settled(alpacacore::FilterWheelDriver& driver) {
    std::vector<int> seen;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        seen.push_back(driver.get_position());
        if (seen.back() != -1 && driver.get_position() == seen.back()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    return seen;
}

template <class Rig>
void position_is_minus_one_while_moving_then_the_target() {
    Rig rig;
    rig.connect_at(0);
    rig.slow_moves();
    rig.driver->set_position(3);
    CHECK(rig.driver->get_position() == -1);
    const auto seen = positions_until_settled(*rig.driver);
    CHECK(seen.back() == 3);
    CHECK(rig.driver->get_position() == 3);
}

template <class Rig>
void a_valid_write_returns_without_waiting_for_the_move() {
    Rig rig;
    rig.connect_at(0);
    rig.slow_moves();
    rig.driver->set_position(5);
    // Returned with the wheel still turning: the write did not wait for the settled slot.
    CHECK(rig.driver->get_position() == -1);
}

template <class Rig>
void an_out_of_range_write_is_invalid_value_and_starts_no_move() {
    Rig rig;
    rig.connect_at(2);
    require_alpaca_error([&] { rig.driver->set_position(-1); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { rig.driver->set_position(kSlots); }, AlpacaError::InvalidValue);
    CHECK(rig.driver->get_position() == 2);
}

template <class Rig>
void invalid_value_precedes_not_connected() {
    Rig rig;
    require_alpaca_error([&] { rig.driver->set_position(-1); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { rig.driver->set_position(0); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { rig.driver->get_position(); }, AlpacaError::NotConnected);
}

template <class Rig>
void a_cache_primed_at_connect_answers_without_a_device_read() {
    Rig rig;
    rig.connect_at(2);
    const int before = rig.device_reads();
    CHECK(rig.driver->get_position() == 2);
    CHECK(rig.device_reads() == before);
}

// docs/failures/0001-qhy-filterwheel-position-cache.md: the device reports the slot it left and slots it passes
// while it turns. Position stays -1 until the commanded target is reported, never a transit slot.
template <class Rig>
void position_ignores_transit_slots_until_the_target_is_reported() {
    Rig rig;
    rig.connect_at(1);
    rig.transit_through({2, 3, 4});
    rig.driver->set_position(5);
    const auto seen = positions_until_settled(*rig.driver);
    REQUIRE(seen.back() == 5);
    for (const int value : seen) CHECK((value == -1 || value == 5));
    CHECK(rig.driver->get_position() == 5);
}

}  // namespace

// One TEST_CASE per driver and case body, named in full so the Falsified by gate can find each. "[!shouldfail]" marks a
// case the ZWO driver diverges on (testing instructions): CI then fails if the divergence goes away without the
// instructions being updated.

TEST_CASE("Filter wheel move model - Position is -1 while moving, then the target (ZWO EFW)",
          "[filterwheel][move-model][zwo][unit]") {
    position_is_minus_one_while_moving_then_the_target<ZwoRig>();
}

TEST_CASE("Filter wheel move model - Position is -1 while moving, then the target (QHY CFW3)",
          "[filterwheel][move-model][qhy][unit]") {
    position_is_minus_one_while_moving_then_the_target<QhyRig>();
}

TEST_CASE("Filter wheel move model - a valid write returns without waiting (ZWO EFW)",
          "[filterwheel][move-model][zwo][unit]") {
    a_valid_write_returns_without_waiting_for_the_move<ZwoRig>();
}

TEST_CASE("Filter wheel move model - a valid write returns without waiting (QHY CFW3)",
          "[filterwheel][move-model][qhy][unit]") {
    a_valid_write_returns_without_waiting_for_the_move<QhyRig>();
}

TEST_CASE("Filter wheel move model - an out-of-range write is InvalidValue (ZWO EFW)",
          "[filterwheel][move-model][zwo][unit]") {
    an_out_of_range_write_is_invalid_value_and_starts_no_move<ZwoRig>();
}

TEST_CASE("Filter wheel move model - an out-of-range write is InvalidValue (QHY CFW3)",
          "[filterwheel][move-model][qhy][unit]") {
    an_out_of_range_write_is_invalid_value_and_starts_no_move<QhyRig>();
}

TEST_CASE("Filter wheel move model - InvalidValue precedes NotConnected (ZWO EFW)",
          "[filterwheel][move-model][zwo][unit]") {
    invalid_value_precedes_not_connected<ZwoRig>();
}

TEST_CASE("Filter wheel move model - InvalidValue precedes NotConnected (QHY CFW3)",
          "[filterwheel][move-model][qhy][unit]") {
    invalid_value_precedes_not_connected<QhyRig>();
}

TEST_CASE("Filter wheel move model - a cache primed at connect needs no read (ZWO EFW)",
          "[filterwheel][move-model][zwo][unit][!shouldfail]") {
    a_cache_primed_at_connect_answers_without_a_device_read<ZwoRig>();
}

TEST_CASE("Filter wheel move model - a cache primed at connect needs no read (QHY CFW3)",
          "[filterwheel][move-model][qhy][unit]") {
    a_cache_primed_at_connect_answers_without_a_device_read<QhyRig>();
}

TEST_CASE("Filter wheel move model - transit slots never reach Position (ZWO EFW)",
          "[filterwheel][move-model][zwo][unit][!shouldfail]") {
    position_ignores_transit_slots_until_the_target_is_reported<ZwoRig>();
}

TEST_CASE("Filter wheel move model - transit slots never reach Position (QHY CFW3)",
          "[filterwheel][move-model][qhy][unit]") {
    position_ignores_transit_slots_until_the_target_is_reported<QhyRig>();
}
