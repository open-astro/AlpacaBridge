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

// Connect/disconnect/operate concurrency stress for the iOptron iMate
// PowerBox Switch (#271, #1068). The driver drives a libgpiod chip, so a
// connect can only succeed on a host with the iMate's GPIO bank. These cases
// hand the driver a FakeGpio through the create_ioptron_switch(...,
// IoptronGpioBackend&) test seam instead: the connect really succeeds, the
// storm reaches the post-connect paths (boolean writes, the racing disconnect)
// and nothing needs hardware. The iMate has no serial side (the switch driver
// opens no serial port), so there is no pty here.
//
// FakeGpio is harsher than the chip: it refuses a second claim and a write to
// lines nobody holds, so a double-open or a write after release fails the case.
//
// This is a SEPARATE file from test_ioptron_concurrency_stress.cpp because
// the PowerBox Switch driver, unlike the mount/filter-wheel/focuser above,
// is only compiled into alpacacore_ioptron when libgpiod >= 2.0 was found
// (ALPACACORE_IOPTRON_POWERBOX, see AlpacaCore/src/vendors/ioptron/CMakeLists.txt)
// -- putting this in the always-built file would fail to link whenever that
// feature is off. CMakeLists.txt only adds this file to TEST_SOURCES under
// the same guard as test_ioptron_switch.cpp.

#include <alpacacore/switch_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_switch_driver.h>

#include <cerrno>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"

using alpacacore::AlpacaDriver;

namespace {

class FakeGpio final : public alpacacore::vendor::ioptron::IoptronGpioBackend {
public:
    void request(const std::string& chip_path, const std::vector<unsigned int>& offsets,
                 const std::vector<int>& initial_values) override {
        std::lock_guard<std::mutex> lock(m_);
        if (claimed_) {
            throw alpacacore::AlpacaException("FakeGpio: lines already claimed",
                                              alpacacore::AlpacaError::DriverException);
        }
        chip_path_ = chip_path;
        levels_.clear();
        for (std::size_t i = 0; i < offsets.size(); ++i) {
            levels_[offsets[i]] = initial_values[i];
        }
        claimed_ = true;
        ++requests_;
    }

    int set_value(unsigned int offset, int value) override {
        std::lock_guard<std::mutex> lock(m_);
        const auto it = levels_.find(offset);
        if (!claimed_ || it == levels_.end()) {
            errno = EBADF;
            return -1;
        }
        it->second = value;
        ++writes_;
        return 0;
    }

    void release() override {
        std::lock_guard<std::mutex> lock(m_);
        if (claimed_) ++releases_;
        claimed_ = false;
    }

    int requests() const {
        std::lock_guard<std::mutex> lock(m_);
        return requests_;
    }
    int releases() const {
        std::lock_guard<std::mutex> lock(m_);
        return releases_;
    }
    int writes() const {
        std::lock_guard<std::mutex> lock(m_);
        return writes_;
    }
    bool claimed() const {
        std::lock_guard<std::mutex> lock(m_);
        return claimed_;
    }

private:
    mutable std::mutex m_;
    std::string chip_path_;
    std::map<unsigned int, int> levels_;
    bool claimed_ = false;
    int requests_ = 0;
    int releases_ = 0;
    int writes_ = 0;
};

std::unique_ptr<alpacacore::SwitchDriver> make_switch(FakeGpio& gpio) {
    return alpacacore::vendor::ioptron::create_ioptron_switch(
        0, alpacacore::vendor::ioptron::default_imate_powerbox_config(), gpio);
}

}  // namespace

TEST_CASE("iMate PowerBox Switch - concurrent connect/disconnect/operate stress", "[ioptron][switch][stress]") {
    FakeGpio gpio;
    auto driver = make_switch(gpio);

    // The connect really succeeds before the storm (#1068).
    REQUIRE(alpacacore::test::settle_connected(*driver, true));
    REQUIRE(gpio.requests() >= 1);

    // open-astro#326: one guard per call -- before this the callback stopped
    // at the first throw, so only get_max_switch() was ever storm-tested.
    //
    // Only the default {NotConnected} is expected: against FakeGpio a storm saw
    // no other code (guard.report() empty).
    alpacacore::test::StressCallGuard guard{alpacacore::AlpacaError::NotConnected};
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& sw = static_cast<alpacacore::SwitchDriver&>(d);
        guard([&] { static_cast<void>(sw.get_max_switch()); });
        guard([&] { static_cast<void>(sw.get_switch(1)); });
        // switch 1 ("DC1"); switch 0 is read-only pass-through
        guard([&] { sw.set_switch(1, true); });
        guard([&] { static_cast<void>(sw.get_switch_value(1)); });
        guard([&] { sw.set_switch_value(1, 1.0); });
    });

    // open-astro#326: settle_connected() rather than a bare set_connected():
    // right after a storm the last async task may still be in flight, so a
    // single sync disconnect can legitimately no-op against the pending-
    // disconnect machinery and the CHECK below would fail on a correct driver.
    CHECK(alpacacore::test::settle_connected(*driver, false));

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
    // Every claim the storm made was released: nothing is left holding the lines.
    CHECK_FALSE(gpio.claimed());
    CHECK(gpio.releases() == gpio.requests());
}

TEST_CASE("iMate PowerBox Switch - a racing disconnect is never dropped", "[ioptron][switch][stress]") {
    FakeGpio gpio;
    auto driver = make_switch(gpio);

    CHECK_FALSE(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, false));
    CHECK_FALSE(alpacacore::test::connected_then_connect_disconnect_settles_disconnected(*driver, true));
    CHECK_FALSE(gpio.claimed());
}

TEST_CASE("iMate PowerBox Switch - destruction races an in-flight connect", "[ioptron][switch][stress]") {
    FakeGpio gpio;
    alpacacore::test::run_destruction_during_connect_stress([&gpio]() { return make_switch(gpio); });
    // Whatever the destroyed drivers claimed was released with them.
    CHECK_FALSE(gpio.claimed());
}
