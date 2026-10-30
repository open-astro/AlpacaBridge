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

// The WeeWX ObservingConditions factory (open-astro#731). It refuses what the
// router arm it replaces refused, with the same text, in the same order, and
// then a value out of range (below). Each
// refusal is an InvalidValue AlpacaException rather than a schema rule so that
// both config sources keep the arm's outcome: an API configuredevice call gets
// the text as its error message, and a persisted entry that breaks one is not
// registered. This file (unlike weewx_schema.cpp) is compiled only under
// ALPACACORE_ENABLE_WEEWX and is not in the layering gate's catalog file set,
// so the vendor header here is fine.
//
// The deleted arm read both numbers as int; the schema's Int is 64-bit, so the
// factory refuses a pollIntervalSeconds above INT_MAX to keep that domain.
// Without it 1e10 s overflows the nanoseconds the poll thread's wait_for()
// converts to, and the wait returns at once, so the driver fetches back to
// back. timeoutMs has the tighter cap of 60000 (open-astro#1718).

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/weewx/weewx_observingconditions_driver.h>

#include <chrono>
#include <limits>

#include "../../catalog/builtin_descriptors.h"
#include "weewx_fields.h"

namespace alpacacore::catalog {

void register_weewx_factory(DeviceCatalog& catalog) {
    Factory factory;
    factory.key = DeviceKey{"weewx", DeviceType::ObservingConditions};
    factory.create = [](const DeviceConfig& config, int device_number) {
        vendor::weewx::WeeWxHttpConfig weewx_config;
        weewx_config.url = config.get(kWeeWxUrl);
        const std::int64_t poll_interval = config.get(kWeeWxPollIntervalSeconds);
        const std::int64_t timeout_ms = config.get(kWeeWxTimeoutMs);
        if (weewx_config.url.empty()) {
            throw AlpacaException("WeeWX observing conditions requires weewxUrl", AlpacaError::InvalidValue);
        }
        if (poll_interval <= 0) {
            throw AlpacaException("pollIntervalSeconds must be greater than 0", AlpacaError::InvalidValue);
        }
        if (timeout_ms <= 0) {
            throw AlpacaException("timeoutMs must be greater than 0", AlpacaError::InvalidValue);
        }
        constexpr std::int64_t kIntMax = std::numeric_limits<int>::max();
        if (poll_interval > kIntMax) {
            throw AlpacaException("pollIntervalSeconds must be at most 2147483647", AlpacaError::InvalidValue);
        }
        // One value bounds connect and transfer of a fetch; a minute is far past
        // any LAN station's answer yet keeps a stuck fetch from holding the
        // poll thread and a Connect for days.
        constexpr std::int64_t kMaxTimeoutMs = 60000;
        if (timeout_ms > kMaxTimeoutMs) {
            throw AlpacaException("timeoutMs must be at most 60000", AlpacaError::InvalidValue);
        }
        weewx_config.poll_interval = std::chrono::seconds(poll_interval);
        weewx_config.timeout = std::chrono::milliseconds(timeout_ms);
        return vendor::weewx::create_weewx_observingconditions(device_number, weewx_config);
    };
    catalog.add(std::move(factory));
}

}  // namespace alpacacore::catalog
