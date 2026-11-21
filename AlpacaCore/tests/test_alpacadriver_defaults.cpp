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

// Issue #1757 slice 1: the six ASCOM common members have their "not implemented" answer as a default in
// AlpacaDriver. A driver with no override of its own answers with these codes and texts, so they are pinned here,
// once, through a driver that overrides only the members AlpacaDriver keeps pure.

#include <alpacacore/alpacadriver.h>
#include <alpacacore/util/error_handling.h>

#include <string>

#include "catch2_compat.h"

namespace {

class MinimalDriver : public alpacacore::AlpacaDriver {
public:
    int get_device_number() const override { return 0; }
    std::string get_name() const override { return "Minimal"; }
    alpacacore::DeviceType get_device_type() const override { return alpacacore::DeviceType::SafetyMonitor; }
    std::string get_description() const override { return "test double"; }
    std::string get_driver_info() const override { return "test double"; }
    std::string get_driver_version() const override { return "0"; }
    int get_interface_version() const override { return 1; }
    bool get_connected() const override { return false; }
    void set_connected(bool) override {}

protected:
    std::string fallback_unique_id() const override { return "minimal"; }
};

template <typename Fn>
void require_refusal(Fn&& fn, int code, const std::string& message) {
    try {
        fn();
        FAIL("the default did not throw");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == code);
        CHECK(std::string(ex.what()) == message);
    }
}

}  // namespace

TEST_CASE("AlpacaDriver defaults - the six common members answer not implemented with their texts",
          "[alpacadriver][unit]") {
    MinimalDriver driver;
    CHECK(driver.get_supported_actions().empty());
    CHECK_FALSE(driver.can_action("Anything"));
    require_refusal([&] { (void)driver.action("Park:Now", "{}"); }, alpacacore::AlpacaError::ActionNotImplemented,
                    "Action not supported: Park:Now");
    require_refusal([&] { (void)driver.command_blind(":Q#", true); }, alpacacore::AlpacaError::MethodNotImplemented,
                    "Command not supported");
    require_refusal([&] { (void)driver.command_bool(":Q#", false); }, alpacacore::AlpacaError::MethodNotImplemented,
                    "Command not supported");
    require_refusal([&] { (void)driver.command_string(":Q#"); }, alpacacore::AlpacaError::MethodNotImplemented,
                    "Command not supported");
}
