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

// Remembered device location for the Gemini Power & Data Hub and flat panel
// auto-detect paths (issue #1452): after one successful auto-detect the next
// connect tries the port that worked with one handshake, and runs the full
// search only when that fails. Every case drives the real driver over a pty
// fake; the full search is replaced by a counting stub so the cases can say how
// many probes and scans a connect cost.

#include <alpacacore/util/device_location_store.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/vendor/gemini/gemini_flatpanel_driver.h>
#include <alpacacore/vendor/gemini/gemini_flatpanel_protocol_wrapper.h>
#include <alpacacore/vendor/gemini/gemini_pdh_protocol_wrapper.h>
#include <alpacacore/vendor/gemini/gemini_pdh_switch_driver.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "catch2_compat.h"
#include "device_location_test_dir.h"
#include "fake_gemini_flatpanel.h"
#include "fake_gemini_pdh.h"

namespace {

namespace gemini = alpacacore::vendor::gemini;
using alpacacore::util::DeviceLocationStore;
using alpacacore::util::SerialClaimKind;
using alpacacore::util::SerialPortClaim;

constexpr const char* kUnitId = "0b3f5a52-7d2e-4c1a-9a53-2f6a1c0d8e11";
// The store key of the unit at enumeration index 0, and at index 1.
const std::string kKey = alpacacore::util::location_key(kUnitId, 0);
const std::string kKeyAtOne = alpacacore::util::location_key(kUnitId, 1);

// What a connect did, as the stubs saw it.
struct Counts {
    std::atomic<int> probes{0};
    std::atomic<int> scans{0};
};

struct PdhKind {
    using Fake = alpacacore::test::FakeGeminiPdh;
    using Driver = alpacacore::SwitchDriver;
    static std::unique_ptr<Driver> make(int index = 0) { return gemini::create_gemini_pdh_switch_by_index(0, index); }
    static std::unique_ptr<Driver> make_configured(const std::string& port) {
        return gemini::create_gemini_pdh_switch(0, port, 19200);
    }
    static void hook(Counts& c, const std::string& scan_result, const std::string& second_result = {}) {
        gemini::GeminiPdhScanHooks hooks;
        hooks.on_probe = [&c](const std::string&) { ++c.probes; };
        hooks.enumerate = [&c, scan_result, second_result] {
            ++c.scans;
            std::vector<gemini::GeminiPdhPortInfo> found;
            if (!scan_result.empty()) found.push_back({scan_result, ""});
            if (!second_result.empty()) found.push_back({second_result, ""});
            return found;
        };
        gemini::set_gemini_pdh_scan_hooks_for_testing(std::move(hooks));
    }
    static void unhook() { gemini::set_gemini_pdh_scan_hooks_for_testing({}); }
};

template <bool Pro>
struct PanelKind {
    using Fake = alpacacore::test::FakeGeminiFlatPanel;
    using Driver = alpacacore::CoverCalibratorDriver;
    static std::unique_ptr<Driver> make(int index = 0) {
        return Pro ? gemini::create_gemini_flatpanel_pro_by_index(0, index)
                   : gemini::create_gemini_flatpanel_by_index(0, index);
    }
    static std::unique_ptr<Driver> make_configured(const std::string& port) {
        return Pro ? gemini::create_gemini_flatpanel_pro(0, port, 9600)
                   : gemini::create_gemini_flatpanel(0, port, 9600);
    }
    static void hook(Counts& c, const std::string& scan_result, const std::string& second_result = {}) {
        gemini::GeminiFlatPanelScanHooks hooks;
        hooks.on_probe = [&c](const std::string&) { ++c.probes; };
        hooks.enumerate = [&c, scan_result, second_result] {
            ++c.scans;
            std::vector<gemini::GeminiFlatPanelPortInfo> found;
            if (!scan_result.empty()) found.push_back({scan_result, ""});
            if (!second_result.empty()) found.push_back({second_result, ""});
            return found;
        };
        gemini::set_gemini_flatpanel_scan_hooks_for_testing(std::move(hooks));
    }
    static void unhook() { gemini::set_gemini_flatpanel_scan_hooks_for_testing({}); }
};

using LitePanel = PanelKind<false>;
using ProPanel = PanelKind<true>;

// Clears the stubs when a case ends, however it ends.
template <typename Kind>
struct HookGuard {
    ~HookGuard() { Kind::unhook(); }
};

// The second connect: the first connect searched and remembered the port, the second tries it
// first and the search stays idle.
template <typename Kind>
void check_second_connect_probes_once() {
    alpacacore::test::TempLocationDir store;
    typename Kind::Fake fake;
    Counts counts;
    HookGuard<Kind> guard;
    Kind::hook(counts, fake.slave_path());

    auto driver = Kind::make();
    driver->assign_unique_id(kUnitId);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(counts.scans == 1);
    CHECK(counts.probes == 0);  // the stub scan stands in for the probes of a real scan
    REQUIRE(DeviceLocationStore::load(kKey) == fake.slave_path());

    driver->set_connected(false);
    REQUIRE_FALSE(driver->get_connected());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(counts.probes == 1);
    CHECK(counts.scans == 1);  // no second search
    CHECK(DeviceLocationStore::load(kKey) == fake.slave_path());
}

// The remembered port is gone: one failed probe, then the full search finds the device and replaces it.
template <typename Kind>
void check_gone_port_falls_back_to_the_search() {
    alpacacore::test::TempLocationDir store;
    typename Kind::Fake fake;
    Counts counts;
    HookGuard<Kind> guard;
    Kind::hook(counts, fake.slave_path());
    REQUIRE(DeviceLocationStore::save(kKey, "/dev/nonexistent-remembered-port"));

    auto driver = Kind::make();
    driver->assign_unique_id(kUnitId);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(counts.probes == 1);
    CHECK(counts.scans == 1);
    CHECK(DeviceLocationStore::load(kKey) == fake.slave_path());
}

// A port another connected driver holds is never probed, whether remembered or not; the search finds the
// device elsewhere and the held port is left alone.
template <typename Kind>
void check_held_port_is_never_probed() {
    alpacacore::test::TempLocationDir store;
    typename Kind::Fake held_fake;
    typename Kind::Fake real;
    Counts counts;
    HookGuard<Kind> guard;
    Kind::hook(counts, real.slave_path());
    auto other = SerialPortClaim::acquire(held_fake.slave_path(), SerialClaimKind::Link, "Other device");
    REQUIRE(other);
    REQUIRE(DeviceLocationStore::save(kKey, held_fake.slave_path()));

    auto driver = Kind::make();
    driver->assign_unique_id(kUnitId);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(counts.probes == 0);
    CHECK(counts.scans == 1);
    CHECK(held_fake.commands().empty());  // never opened, never written to
    CHECK(DeviceLocationStore::load(kKey) == real.slave_path());
}

// The user renumbers the entry (same id, index 0 to 1): the port found at index 0 must not answer for
// index 1. The new index searches and its result is stored under its own key.
template <typename Kind>
void check_changed_index_ignores_the_remembered_port() {
    alpacacore::test::TempLocationDir store;
    typename Kind::Fake first;
    typename Kind::Fake second;
    Counts counts;
    HookGuard<Kind> guard;
    Kind::hook(counts, first.slave_path(), second.slave_path());
    REQUIRE(DeviceLocationStore::save(kKey, first.slave_path()));

    auto driver = Kind::make(1);
    driver->assign_unique_id(kUnitId);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(counts.probes == 0);
    CHECK(counts.scans == 1);
    CHECK(DeviceLocationStore::load(kKeyAtOne) == second.slave_path());
    CHECK(DeviceLocationStore::load(kKey) == first.slave_path());
}

// A configured port is used as given: the store is neither read nor written, and nothing is probed or searched.
template <typename Kind>
void check_configured_port_never_touches_the_store() {
    alpacacore::test::TempLocationDir store;
    typename Kind::Fake fake;
    Counts counts;
    HookGuard<Kind> guard;
    Kind::hook(counts, "/dev/should-not-be-searched");
    REQUIRE(DeviceLocationStore::save(kKey, "/dev/nonexistent-remembered-port"));

    auto driver = Kind::make_configured(fake.slave_path());
    driver->assign_unique_id(kUnitId);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(counts.probes == 0);
    CHECK(counts.scans == 0);
    CHECK(DeviceLocationStore::load(kKey) == "/dev/nonexistent-remembered-port");  // unchanged

    // A key with no entry stays without one.
    auto second = Kind::make_configured(fake.slave_path());
    second->assign_unique_id("5c1d7e0a-3b94-4f2d-8c66-9a0e41b7d2f3");
    driver->set_connected(false);
    second->set_connected(true);
    REQUIRE(second->get_connected());
    CHECK_FALSE(DeviceLocationStore::load("5c1d7e0a-3b94-4f2d-8c66-9a0e41b7d2f3").has_value());
}

}  // namespace

TEST_CASE("Gemini PDH auto-detect - a second connect probes the remembered port once and skips the search",
          "[gemini][pdh][serial][location][issue1452]") {
    check_second_connect_probes_once<PdhKind>();
}

TEST_CASE("Gemini flat panel Lite auto-detect - a second connect probes the remembered port once and skips the search",
          "[gemini][flatpanel][serial][location][issue1452]") {
    check_second_connect_probes_once<LitePanel>();
}

TEST_CASE("Gemini flat panel Pro auto-detect - a second connect probes the remembered port once and skips the search",
          "[gemini][flatpanel][serial][location][issue1452]") {
    check_second_connect_probes_once<ProPanel>();
}

TEST_CASE("Gemini PDH auto-detect - a gone remembered port falls back to the search and is replaced",
          "[gemini][pdh][serial][location][issue1452]") {
    check_gone_port_falls_back_to_the_search<PdhKind>();
}

TEST_CASE("Gemini flat panel Lite auto-detect - a gone remembered port falls back to the search and is replaced",
          "[gemini][flatpanel][serial][location][issue1452]") {
    check_gone_port_falls_back_to_the_search<LitePanel>();
}

TEST_CASE("Gemini flat panel Pro auto-detect - a gone remembered port falls back to the search and is replaced",
          "[gemini][flatpanel][serial][location][issue1452]") {
    check_gone_port_falls_back_to_the_search<ProPanel>();
}

TEST_CASE("Gemini PDH auto-detect - a remembered port that answers as another device falls back to the search",
          "[gemini][pdh][serial][location][issue1452]") {
    alpacacore::test::TempLocationDir store;
    alpacacore::test::FakeGeminiFlatPanel wrong_device;  // a flat panel now sits where the hub was
    PdhKind::Fake hub;
    Counts counts;
    HookGuard<PdhKind> guard;
    PdhKind::hook(counts, hub.slave_path());
    REQUIRE(DeviceLocationStore::save(kKey, wrong_device.slave_path()));

    auto driver = PdhKind::make();
    driver->assign_unique_id(kUnitId);
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(counts.probes == 1);
    CHECK(counts.scans == 1);
    CHECK(DeviceLocationStore::load(kKey) == hub.slave_path());
}

TEST_CASE("Gemini PDH auto-detect - a port a connected driver holds is never probed",
          "[gemini][pdh][serial][location][issue1452]") {
    check_held_port_is_never_probed<PdhKind>();
}

TEST_CASE("Gemini flat panel Lite auto-detect - a port a connected driver holds is never probed",
          "[gemini][flatpanel][serial][location][issue1452]") {
    check_held_port_is_never_probed<LitePanel>();
}

TEST_CASE("Gemini flat panel Pro auto-detect - a port a connected driver holds is never probed",
          "[gemini][flatpanel][serial][location][issue1452]") {
    check_held_port_is_never_probed<ProPanel>();
}

TEST_CASE("Gemini PDH - a configured port never reads or writes the location store",
          "[gemini][pdh][serial][location][issue1452]") {
    check_configured_port_never_touches_the_store<PdhKind>();
}

TEST_CASE("Gemini flat panel Lite - a configured port never reads or writes the location store",
          "[gemini][flatpanel][serial][location][issue1452]") {
    check_configured_port_never_touches_the_store<LitePanel>();
}

TEST_CASE("Gemini flat panel Pro - a configured port never reads or writes the location store",
          "[gemini][flatpanel][serial][location][issue1452]") {
    check_configured_port_never_touches_the_store<ProPanel>();
}

TEST_CASE("Gemini PDH auto-detect - a changed hub index ignores the port remembered for the old index",
          "[gemini][pdh][serial][location][issue1452]") {
    check_changed_index_ignores_the_remembered_port<PdhKind>();
}

TEST_CASE("Gemini flat panel Lite auto-detect - a changed panel index ignores the port remembered for the old index",
          "[gemini][flatpanel][serial][location][issue1452]") {
    check_changed_index_ignores_the_remembered_port<LitePanel>();
}

TEST_CASE("Gemini flat panel Pro auto-detect - a changed panel index ignores the port remembered for the old index",
          "[gemini][flatpanel][serial][location][issue1452]") {
    check_changed_index_ignores_the_remembered_port<ProPanel>();
}
