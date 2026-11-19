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

// The remotealpaca ObservingConditions factory (decision 0023, issue #1753 slice 3). Each refusal is an InvalidValue
// AlpacaException rather than a schema rule, so both config sources refuse it: an API configuredevice call gets the
// text, and a persisted entry that breaks one is not registered (the WeeWX factory's shape). Every integer is range
// checked before it narrows to int (decision 0007 rule 4). This file (unlike remotealpaca_schema.cpp) is compiled
// only under ALPACACORE_ENABLE_REMOTEALPACA, so the vendor header here is fine.
//
// The factory is a bind hook so it sees the server's listening addresses (BindContext::listen_addresses): an upstream
// that resolves to one of them is refused here, at configure, and the driver keeps the source to refuse it again at
// every Connect (decision 0023 rule 4).

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_observingconditions_driver.h>
#include <alpacacore/vendor/remotealpaca/self_loop.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "../../catalog/builtin_descriptors.h"
#include "remotealpaca_fields.h"

namespace alpacacore::catalog {

namespace {

namespace remote = vendor::remotealpaca;

remote::RemoteObservingConditionsConfig read_config(const DeviceConfig& config) {
    remote::RemoteObservingConditionsConfig out;
    out.host = config.get(kRemoteAlpacaHost);
    const std::int64_t port = config.get(kRemoteAlpacaPort);
    const std::int64_t upstream_number = config.get(kRemoteAlpacaDeviceNumber);
    const std::int64_t poll_seconds = config.get(kRemoteAlpacaPollSeconds);
    if (out.host.empty()) {
        throw AlpacaException("Remote Alpaca observing conditions requires upstreamHost", AlpacaError::InvalidValue);
    }
    if (port < 1 || port > 65535) {
        throw AlpacaException("upstreamPort must be from 1 to 65535", AlpacaError::InvalidValue);
    }
    if (upstream_number < 0 || upstream_number > std::numeric_limits<int>::max()) {
        throw AlpacaException("upstreamDeviceNumber must be from 0 to 2147483647", AlpacaError::InvalidValue);
    }
    if (poll_seconds < remote::kMinPollPeriod.count() || poll_seconds > remote::kMaxPollPeriod.count()) {
        throw AlpacaException("pollPeriodSeconds must be from 1 to 60", AlpacaError::InvalidValue);
    }
    out.port = static_cast<int>(port);
    out.upstream_device_number = static_cast<int>(upstream_number);
    out.poll_period = std::chrono::seconds(poll_seconds);
    return out;
}

remote::ServerAddressSource server_addresses_of(const BindContext& context) {
    if (!context.listen_addresses) return {};
    return [source = context.listen_addresses] {
        std::vector<remote::ServerAddress> out;
        for (const ListenAddress& a : source()) out.push_back({a.host, a.port});
        return out;
    };
}

}  // namespace

void register_remotealpaca_factory(DeviceCatalog& catalog) {
    Factory factory;
    factory.key = DeviceKey{"remotealpaca", DeviceType::ObservingConditions};
    factory.bind = [](const DeviceConfig& config, int device_number, const BindContext& context) {
        const remote::RemoteObservingConditionsConfig remote_config = read_config(config);
        remote::ServerAddressSource self = server_addresses_of(context);
        BindResult result;
        if (self) {
            if (auto refusal = remote::self_loop_refusal(remote_config.host, remote_config.port, self())) {
                result.rejection = std::move(*refusal);
                return result;
            }
        }
        result.driver = remote::create_remotealpaca_observingconditions(device_number, remote_config, std::move(self));
        return result;
    };
    catalog.add(std::move(factory));
}

}  // namespace alpacacore::catalog
