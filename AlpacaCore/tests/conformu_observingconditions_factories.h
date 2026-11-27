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

// Test-only: each ObservingConditions driver over its roster fake, for the fake ConformU server
// (fake_conformu_server.cpp, #2078 slice 6). One factory per kFakeConnectableRoster ObservingConditions row, each
// guarded by the FAKE_CONFORMU_<VENDOR> macro the server's CMake sets when that vendor is built. Each fake is a
// function-local static, built when its row is served, so it outlives every driver the server builds. It lives here,
// not in AlpacaHTTP, so the vendor headers stay out of AlpacaHTTP (scripts/check_layering.py).

#include <alpacacore/observingconditions_driver.h>

#include <memory>

#ifdef FAKE_CONFORMU_REMOTEALPACA
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_observingconditions_driver.h>

#include "alpaca_upstream_script.h"
#include "fake_http_client.h"
#endif

#ifdef FAKE_CONFORMU_WEEWX
#include <alpacacore/vendor/weewx/weewx_observingconditions_driver.h>

#include <ctime>
#include <string>

#include "fake_http_client.h"
#endif

namespace alpacacore::test::conformu {

#ifdef FAKE_CONFORMU_REMOTEALPACA
// The scripted upstream of the remotealpaca tests (alpaca_upstream_script.h): an ObservingConditions 2 device with
// every sensor, answered in process through FakeHttpClient, so no request leaves the process. The poll runs on the
// real task clock at the default period.
inline FakeHttpClient& remotealpaca_upstream() {
    static AlpacaUpstreamScript upstream;  // first, so it outlives the client whose responder calls it
    static FakeHttpClient http;
    static const bool installed = [] {
        upstream.install(http);
        return true;
    }();
    (void)installed;
    return http;
}
inline std::unique_ptr<ObservingConditionsDriver> make_remotealpaca_observingconditions(int device_number) {
    vendor::remotealpaca::RemoteObservingConditionsConfig config;
    config.host = "127.0.0.1";  // never resolved: every request goes to the fake
    config.port = 32323;
    return vendor::remotealpaca::create_remotealpaca_observingconditions(
        device_number, config, remotealpaca_upstream().client(), {}, util::default_task_clock());
}
#endif

#ifdef FAKE_CONFORMU_WEEWX
// The scripted client of the WeeWX tests (fake_http_client.h), answering every fetch with a WeeWX JSON feed that
// carries all seven sensors the driver serves, observed at the time of the fetch. No request leaves the process.
// The poll runs on the real task clock at the default 900 s interval.
inline FakeHttpClient& weewx_feed() {
    static FakeHttpClient http([] {
        const long long now = static_cast<long long>(std::time(nullptr));
        return R"({"lcd_datasheet":{"current":{"outTemp":{"value":50.0},"outHumidity":{"value":40.0},)"
               R"("dewpoint":{"value":26.6},"wind_speed":{"value":10.0},"barometer":{"value":30.0},)"
               R"("sqm":{"value":21.3},"sqmTemp":{"value":41.0},"dateTime":{"value":)" +
               std::to_string(now) + "}}}}";
    });
    return http;
}
inline std::unique_ptr<ObservingConditionsDriver> make_weewx_observingconditions(int device_number) {
    vendor::weewx::WeeWxHttpConfig config;
    config.url = "http://127.0.0.1:32324/weewx.json";  // never fetched: every request goes to the fake
    return vendor::weewx::create_weewx_observingconditions(device_number, config, weewx_feed().client());
}
#endif

}  // namespace alpacacore::test::conformu
