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

// The remotealpaca SafetyMonitor proxied device (decision 0023, issue #1753 slice 4) over UpstreamProxy
// (upstream_proxy.h), which holds every rule it shares with the ObservingConditions device. IsSafe is served from the
// polled frame (decision 0010); while the link is faulted (latched, not yet lost) it throws DriverException like every
// served getter (decision 0009 rule 1), and while the link is lost it reads false.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/async_connectable.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/http_client.h>
#include <alpacacore/util/polled_state.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/remotealpaca/remote_alpaca_client.h>
#include <alpacacore/vendor/remotealpaca/remotealpaca_safetymonitor_driver.h>
#include <alpacacore/version.h>

#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "upstream_proxy.h"

namespace alpacacore::vendor::remotealpaca {

namespace {

// The polled frame (decision 0010): what the upstream answered for IsSafe, a value or its error.
struct Frame {
    detail::Answer<bool> is_safe;
};

using Proxy = detail::UpstreamProxy<Frame>;

class RemoteAlpacaSafetyMonitorDriver final : public SafetyMonitorDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    RemoteAlpacaSafetyMonitorDriver(int device_number, const RemoteDeviceConfig& config,
                                    std::shared_ptr<util::HttpClient> http, ServerAddressSource self,
                                    util::TaskClock& clock)
        : AsyncConnectable(detail::kComponent),
          device_number_(device_number),
          // ISafetyMonitorV3 is the first SafetyMonitor with Connect; an older upstream gets Connected=true.
          proxy_({"safetymonitor", "SafetyMonitor", 3, [this] { return sample(); }}, config, std::move(http),
                 std::move(self), clock) {}

    ~RemoteAlpacaSafetyMonitorDriver() override {
        // Blocks new connection tasks, then joins the in-flight one: first, before members the task touches die.
        shutdown_connection();
        proxy_.stop();
    }

    int get_device_number() const override { return device_number_; }

    // Once a Connect has read them: the brand and the upstream's Name (decision 0024), and the upstream's Description.
    std::string get_name() const override { return proxy_.name(); }

    std::string get_description() const override { return proxy_.description(); }

    DeviceType get_device_type() const override { return DeviceType::SafetyMonitor; }

    std::string fallback_unique_id() const override { return "REMOTEALPACA_SM_" + std::to_string(device_number_); }

    std::string get_driver_info() const override { return "AlpacaCore Remote Alpaca SafetyMonitor Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    int get_interface_version() const override { return 3; }  // ISafetyMonitorV3 (Platform 7)

    // A loss the module latched (a frame or a fault past the bound) reads false at once, before on_lost() runs.
    bool get_connected() const override { return proxy_.connected_flag().load() && !proxy_.link_lost(); }

    ALPACA_ASYNC_CONNECT_FORWARDERS()

    // Decision 0009 rule 5: the latched fault, or after a loss the reason it was lost, kept until the next Connect or
    // Disconnect.
    std::string get_link_fault() const override { return proxy_.link_fault(); }

    void set_connected(bool connected) override {
        // Issue #528: the connect makes HTTP calls, so a sync disconnect must not slip in between its checks and its
        // store. Guards set_connected() against set_connected() only; no getter takes it.
        std::lock_guard<std::mutex> transition(transition_mutex_);
        if (!connected && record_disconnect_if_connect_in_flight(proxy_.connected_flag().load())) {
            return;
        }
        if (connected && consume_pending_disconnect(proxy_.connected_flag().load())) {
            return;
        }
        if (connected) {
            if (get_connected()) {
                return;  // idempotent: connected, and the poll is running
            }
            proxy_.connect({});  // the Connect reads nothing of its own: IsSafe is the first frame
        } else {
            proxy_.disconnect();
        }
    }

    // The upstream's IsSafe from the frame (a stale frame is served), or its error unchanged. While the link is lost it
    // reads false and does not throw (#1753, decision 0009 rule 3): a dead link never reads safe, and an observatory
    // that polls IsSafe closes rather than stops on an error. While faulted the frame is refused with DriverException,
    // as every served getter is (decision 0009 rule 1). Never connected, connecting, or disconnected by the client:
    // NotConnected.
    bool get_is_safe() const override {
        try {
            return proxy_.with_connected(
                [](const util::PolledState<Frame>& state) { return state.served().value.is_safe.get(); });
        } catch (const AlpacaException& e) {
            if (e.error_code() == AlpacaError::NotConnected && proxy_.lost()) return false;
            throw;
        }
    }

    void set_poll_paused(bool paused) { proxy_.set_poll_paused(paused); }

private:
    // One measurement of the upstream, with no lock held (decision 0016 rule 1). Throws RemoteTransportError when the
    // call gets no usable reply; an upstream error is kept in the frame.
    Frame sample() {
        Frame frame;
        frame.is_safe = detail::answer_of([this] { return proxy_.client().get_bool("issafe"); });
        return frame;
    }

    const int device_number_;
    // Guards set_connected() against a concurrent set_connected() only (#528).
    std::mutex transition_mutex_;
    // Last, so it is destroyed (its poll body joined) before the members the body touches die.
    Proxy proxy_;
};

}  // namespace

std::unique_ptr<SafetyMonitorDriver> create_remotealpaca_safetymonitor(int device_number,
                                                                       const RemoteDeviceConfig& config,
                                                                       ServerAddressSource self) {
    return create_remotealpaca_safetymonitor(device_number, config,
                                             util::make_curl_http_client("AlpacaBridge-remotealpaca/1.0"),
                                             std::move(self), util::default_task_clock());
}

std::unique_ptr<SafetyMonitorDriver> create_remotealpaca_safetymonitor(int device_number,
                                                                       const RemoteDeviceConfig& config,
                                                                       std::shared_ptr<util::HttpClient> http,
                                                                       ServerAddressSource self,
                                                                       util::TaskClock& clock) {
    return std::make_unique<RemoteAlpacaSafetyMonitorDriver>(device_number, config, std::move(http), std::move(self),
                                                             clock);
}

void set_poll_paused_for_testing(SafetyMonitorDriver& driver, bool paused) {
    if (auto* remote = dynamic_cast<RemoteAlpacaSafetyMonitorDriver*>(&driver)) {
        remote->set_poll_paused(paused);
    }
}

}  // namespace alpacacore::vendor::remotealpaca
