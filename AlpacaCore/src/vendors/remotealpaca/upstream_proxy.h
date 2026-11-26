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

// What every remotealpaca proxied device shares (decision 0023, issue #1753): the upstream client, the self-loop check
// at Connect, the forwarded Connect and Disconnect, the upstream's Name and Description, the polled frame and its link
// health through util::PolledState (decision 0010 rule 7, #2496), the loss of the link (decision 0009), the command
// slot and the pass-through of the upstream's errors. A driver composes one UpstreamProxy<Frame> (decision 0013
// section 5: a member, not a base class), names its frame and how to measure it, and keeps its own AsyncConnectable
// glue, its transition_mutex_ and its get_connected(). Private to the vendor: no header under include/ names it.
//
// Lock order (decision 0016): the driver's transition_mutex_, then the client's call mutex (the I/O lock), then this
// helper's state mutex (the leaf of the driver's own locks; a driver's own fields read under with_connected() or
// with_state() live under it too), then the module's snapshot and health mutexes. No upstream call runs under the
// state mutex.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/device_name.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/http_client.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/polled_state.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/remotealpaca/remote_alpaca_client.h>
#include <alpacacore/vendor/remotealpaca/remote_device_config.h>
#include <alpacacore/vendor/remotealpaca/self_loop.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <utility>

namespace alpacacore::vendor::remotealpaca::detail {

inline constexpr const char* kComponent = "remotealpaca";

// Decision 0009 rule 1: three failed polls in a row latch the link fault, the count every polled driver uses.
inline constexpr int kFailuresToLatch = 3;

// The longest a Connect waits for an upstream's own Platform 7 Connect to finish (Connecting false), and how often it
// asks. The upstream's connect is its device's business; this only bounds how long ours waits for it.
inline constexpr std::chrono::seconds kUpstreamConnectBound{30};
inline constexpr std::chrono::milliseconds kUpstreamConnectingPoll{100};

// One upstream answer: its value, or the upstream's error, kept to be thrown unchanged (decision 0023 rule 5).
template <class T>
struct Answer {
    std::optional<T> value;
    int error_number = 0;
    std::string error_message;

    T get() const {
        if (!value) throw AlpacaException(error_message, error_number);
        return *value;
    }
};

// What the upstream answered; a call with no usable reply (RemoteTransportError) is not an answer and is rethrown.
template <class Fn>
auto answer_of(Fn&& call) -> Answer<decltype(call())> {
    try {
        return {call(), 0, {}};
    } catch (const RemoteTransportError&) {
        throw;
    } catch (const AlpacaException& e) {
        return {std::nullopt, e.error_code(), e.what()};
    }
}

// Decision 0024 rules 1 and 2: the brand, then the upstream's Name as the model, without its " (...)" qualifiers and
// cut at its first comma, so no '(' or ',' reaches Name. An empty result keeps the default Name.
inline constexpr const char* kBrand = "Remote Alpaca";

inline std::string name_from_upstream(const std::string& upstream_name) {
    std::string model = util::without_qualifiers(upstream_name);
    if (const auto comma = model.find(','); comma != std::string::npos) model.resize(comma);
    model.erase(std::find_if(model.rbegin(), model.rend(), [](unsigned char c) { return std::isspace(c) == 0; }).base(),
                model.end());
    model.erase(model.begin(),
                std::find_if(model.begin(), model.end(), [](unsigned char c) { return std::isspace(c) == 0; }));
    if (model.empty() || model.find('(') != std::string::npos) return {};
    return util::branded_name(kBrand, model);
}

inline std::uint32_t new_client_id() {
    std::random_device seed;
    std::uniform_int_distribution<std::uint32_t> pick(1, 0x7FFFFFFF);
    return pick(seed);
}

// Throws AlpacaException(InvalidValue) for a poll period outside 1 to 60 s; the client checks the rest.
inline RemoteDeviceConfig validated(RemoteDeviceConfig config) {
    if (config.poll_period < kMinPollPeriod || config.poll_period > kMaxPollPeriod) {
        throw AlpacaException("Poll period must be from 1 to 60 seconds", AlpacaError::InvalidValue);
    }
    return config;
}

// Decision 0009 point 2(b): the bound is three poll periods (#1753 maintainer decision, 2026-11-16), never shorter than
// util::kLinkStalenessBound. A fault standing that long, or a frame no poll refreshed for it, is a lost link (the
// module's verdict).
inline std::chrono::nanoseconds loss_bound(std::chrono::seconds period) {
    return std::max<std::chrono::nanoseconds>(util::kLinkStalenessBound, 3 * period);
}

template <class Frame>
class UpstreamProxy {
public:
    using State = util::PolledState<Frame>;
    using Token = typename State::Token;

    // What the driver supplies.
    struct Setup {
        std::string device_type;    // the Alpaca path segment: "observingconditions"
        std::string type_name;      // the ASCOM type name, for the default Name and Description: "ObservingConditions"
        int platform7_version = 0;  // the type's first InterfaceVersion with Connect, Disconnect and Connecting
        // One measurement with no lock held (decision 0016 rule 1), on the poll thread and at Connect. Throws
        // RemoteTransportError when a call gets no usable reply; an upstream error is kept in the frame.
        std::function<Frame()> sample;
    };

    UpstreamProxy(Setup setup, const RemoteDeviceConfig& config, std::shared_ptr<util::HttpClient> http,
                  ServerAddressSource self, util::TaskClock& clock)
        : config_(validated(config)),
          self_(std::move(self)),
          clock_(clock),
          client_(std::move(http),
                  UpstreamDevice{config_.host, config_.port, setup.device_type, config_.upstream_device_number},
                  new_client_id()),
          label_(config_.host + ":" + std::to_string(config_.port) + " device " +
                 std::to_string(config_.upstream_device_number)),
          type_name_(std::move(setup.type_name)),
          platform7_version_(setup.platform7_version),
          sample_(std::move(setup.sample)),
          name_(std::string(kBrand) + " " + type_name_),
          description_(default_description()),
          state_(
              clock, poll_generation_,
              {config_.poll_period, kFailuresToLatch, 2 * config_.poll_period, std::string(kComponent) + " " + label_,
               loss_bound(config_.poll_period)},
              [this] { return probe(); }, [this] { on_lost(); }),
          command_op_("remotealpaca command", command_generation_, clock) {}

    UpstreamProxy(const UpstreamProxy&) = delete;
    UpstreamProxy& operator=(const UpstreamProxy&) = delete;

    // The driver calls it in its destructor, after shutdown_connection(): the poll and the command bodies are joined
    // while everything they touch is alive.
    void stop() {
        state_.stop_and_join();
        command_op_.cancel_all_and_join();
    }

    util::TaskClock& clock() const { return clock_; }
    RemoteAlpacaClient& client() { return client_; }
    const RemoteDeviceConfig& config() const { return config_; }

    // For the driver's get_connected(), which reads connected_flag().load() && !link_lost(): an atomic load and the
    // module's atomic, never a lock (decision 0016 rule 3). A loss the module latched (a frame or a fault past the
    // bound) reads false at once, before on_lost() runs.
    const std::atomic<bool>& connected_flag() const { return connected_; }
    bool link_lost() const { return state_.link_lost(); }
    bool connected() const { return connected_.load() && !state_.link_lost(); }

    // Once a Connect has read them: the brand and the upstream's Name (decision 0024), and the upstream's Description.
    std::string name() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return name_;
    }

    std::string description() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return description_;
    }

    // Decision 0009 rule 5: the latched fault, or after a loss the reason it was lost, kept until the next Connect or
    // Disconnect.
    std::string link_fault() const {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string lost = loss_reason_locked();
        return lost.empty() ? state_.fault() : lost;
    }

    // Lost (decision 0009 rule 3), as opposed to never connected, disconnected by the client or connecting: the reason
    // is kept until the next Connect or Disconnect.
    bool lost() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return !loss_reason_locked().empty();
    }

    // NotConnected before any other answer (ASCOM contract precedence); after a loss the text says why.
    void require_connected() const {
        if (!connected()) {
            std::lock_guard<std::mutex> lock(mutex_);
            throw AlpacaException(not_connected_text_locked(), AlpacaError::NotConnected);
        }
    }

    // A getter served from the frame (decisions 0010 and 0016): no upstream call. `fn` takes the module and runs under
    // the state lock with Connected re-checked (a loss drops connected_ under this lock); the module's served()
    // refuses a latched fault with DriverException (decision 0009 rule 1) and serves a stale frame.
    template <class Fn>
    auto with_connected(Fn&& fn) const {
        require_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected()) {
            throw AlpacaException(not_connected_text_locked(), AlpacaError::NotConnected);
        }
        return fn(static_cast<const State&>(state_));
    }

    // The module, and driver state that lives beside the frame (metadata a Connect reads), under the same leaf lock:
    // a probe's or a command's begin_poll() and publish(), a write-through. `fn` makes no upstream call.
    template <class Fn>
    auto with_state(Fn&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        return fn(state_);
    }

    template <class Fn>
    auto with_state(Fn&& fn) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return fn(static_cast<const State&>(state_));
    }

    // Decision 0023 rule 4, then the forwarded Connect, the upstream's identity, `reads` (the driver's own reads at
    // Connect, with no lock held) and the first frame, so the getters answer when Connect returns. `reads` returns
    // what to store, run under the state lock with the identity, or an empty function. Decision 0009 rule 4: this is
    // the only relink of a lost link. Call under the driver's transition_mutex_.
    void connect(const std::function<std::function<void()>()>& reads) {
        state_.stop_and_join();  // a lost link's poll has returned or is returning; join it so a fresh one starts
        // A command body from before (an AveragePeriod write-through) must not land on this Connect's frame.
        command_op_.cancel_all_and_join();
        // A loss a getter latched while the poll waited its tick never ran on_lost(): without this, reset() would clear
        // the latch and the link read connected before this Connect has a frame.
        connected_.store(false);
        state_.reset();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            loss_text_.clear();  // rule 5: the kept text ends at the next Connect
            probe_error_ = nullptr;
        }
        if (self_) {
            if (auto refusal = self_loop_refusal(config_.host, config_.port, self_())) {
                throw AlpacaException(*refusal, AlpacaError::DriverException);
            }
        }
        try {
            const int version = client_.get_int("interfaceversion");
            forward_connect(version);
            if (!client_.get_bool("connected")) {
                throw AlpacaException("Upstream " + label_ + " still reports Connected false after Connect",
                                      AlpacaError::DriverException);
            }
            std::string name = client_.get_string("name");
            std::string description = client_.get_string("description");
            const std::function<void()> store = reads ? reads() : std::function<void()>{};
            {
                std::lock_guard<std::mutex> lock(mutex_);
                // Decision 0024 rule 4: what the brand-and-model Name leaves out of the upstream's Name goes in the
                // Description.
                const std::string branded = name_from_upstream(name);
                if (!branded.empty()) name_ = branded;
                // Rebuilt on each Connect, so a reconnect does not append the upstream Name again.
                description_ = description.empty() ? default_description() : std::move(description);
                if (!name.empty() && branded != util::branded_name(kBrand, name)) {
                    description_ += " (upstream Name: " + name + ")";
                }
                if (store) store();
            }
            // The first frame, then the poll: the getters answer when Connect returns. No frame fails the Connect
            // with the last probe's own error.
            if (!state_.start()) {
                std::exception_ptr error;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    error = std::exchange(probe_error_, nullptr);
                }
                if (error) std::rethrow_exception(error);
                throw AlpacaException("Upstream " + label_ + " state poll did not start", AlpacaError::DriverException);
            }
            connected_.store(true);
        } catch (...) {
            connected_.store(false);
            if (forwarded_connect_) {
                forward_disconnect();  // best effort: leave the upstream as this Connect found it
            }
            throw;
        }
    }

    // Disconnect, forwarded unless the link was lost. Call under the driver's transition_mutex_.
    void disconnect() {
        state_.stop_and_join();
        command_op_.cancel_all_and_join();
        bool lost = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lost = !loss_reason_locked().empty();
            connected_.store(false);
            loss_text_.clear();  // rule 5: the kept text ends at the next Disconnect
        }
        state_.reset();
        if (forwarded_connect_) {
            if (lost) {
                forwarded_connect_ = false;  // the upstream is gone: nothing to tell it, and the call would only wait
            } else {
                forward_disconnect();
            }
        }
    }

    // A command runs as a body in its own slot (decision 0023, decision 0006 rule 1); the caller waits for the body's
    // outcome, which the per-call timeout bounds, and gets its error unchanged. A refused connection is a lost link at
    // once (NotConnected); another call with no reply is a DriverException, or NotConnected when the poll lost the
    // link meanwhile. Both are decided inside the body: a Connect joins the slot first, so no reconnect lands between
    // the failure and its answer (#2456).
    void run_command(std::function<void(util::OperationContext&)> body) {
        struct Outcome {
            std::mutex mutex;
            std::condition_variable done_cv;
            bool done = false;
            std::exception_ptr error;
        };
        auto outcome = std::make_shared<Outcome>();
        command_op_.start([this, outcome, body = std::move(body)](util::OperationContext& ctx) {
            std::exception_ptr error;
            try {
                try {
                    body(ctx);
                } catch (const RemoteConnectRefused& e) {
                    lose_link(e.what());
                    require_connected();
                    throw;
                } catch (const RemoteTransportError&) {
                    require_connected();
                    throw;
                }
            } catch (...) {
                error = std::current_exception();
            }
            {
                std::lock_guard<std::mutex> lock(outcome->mutex);
                outcome->error = std::move(error);  // the caller becomes the error's only owner
                outcome->done = true;
            }
            outcome->done_cv.notify_all();
        });
        {
            // A completion wait, like a join: the body ends after its calls, each bounded by kRemoteCallTimeout.
            std::unique_lock<std::mutex> lock(outcome->mutex);
            outcome->done_cv.wait(lock, [&outcome] { return outcome->done; });
        }
        if (command_done_hook_) command_done_hook_();
        // Taken out of the shared outcome, so the body thread never frees the exception the caller is reading.
        if (auto error = std::move(outcome->error)) std::rethrow_exception(error);
    }

    void set_poll_paused(bool paused) { state_.pause_for_testing(paused); }

    // Set while no command runs; a command's caller calls it after its body has ended, before it answers.
    void set_command_done_hook(std::function<void()> hook) { command_done_hook_ = std::move(hook); }

    std::uint64_t command_generation() const { return command_generation_.current(); }

private:
    std::string default_description() const { return type_name_ + " of the upstream Alpaca server " + label_; }

    std::string not_connected_text_locked() const {
        const std::string lost = loss_reason_locked();
        if (!lost.empty()) {
            return "Upstream " + label_ + " lost (" + lost + "); Connect again when it answers";
        }
        return "Device not connected";
    }

    // Why the link was lost, empty while it is not: the refusal, the latched fault, or the frame's age.
    std::string loss_reason_locked() const {
        if (!loss_text_.empty() || !state_.link_lost()) return loss_text_;
        std::string fault = state_.fault();
        if (!fault.empty()) return fault;
        return "no answer for " +
               std::to_string(
                   std::chrono::duration_cast<std::chrono::seconds>(loss_bound(config_.poll_period)).count()) +
               " s";
    }

    // Platform 7 (the type's platform7_version and later): Connect, then Connecting until false. Before it,
    // Connected=true.
    void forward_connect(int version) {
        forwarded_connect_ = true;
        forwarded_version_ = version;
        if (version < platform7_version_) {
            client_.put("connected", {{"Connected", "True"}});
            return;
        }
        client_.put("connect");
        const auto deadline = clock_.now() + kUpstreamConnectBound;
        while (client_.get_bool("connecting")) {
            if (clock_.now() >= deadline) {
                throw AlpacaException("Upstream " + label_ + " was still connecting after " +
                                          std::to_string(kUpstreamConnectBound.count()) + " s",
                                      AlpacaError::DriverException);
            }
            clock_.sleep_for(kUpstreamConnectingPoll);
        }
    }

    // Best effort: a Disconnect completes here whatever the upstream answers. Called under transition_mutex_.
    void forward_disconnect() {
        forwarded_connect_ = false;
        try {
            if (forwarded_version_ < platform7_version_) {
                client_.put("connected", {{"Connected", "False"}});
            } else {
                client_.put("disconnect");
            }
        } catch (const std::exception& e) {
            // Decision 0007 rule 9: a failure a client can cause is not logged at WARNING or above.
            ALPACA_LOG_INFO(kComponent, "Upstream " + label_ + " did not take the Disconnect: " + e.what());
        }
    }

    // The poll's probe, on the poll thread with no driver lock held (decision 0016 rule 1): one measurement, which the
    // module publishes. A call with no usable reply throws (a failed exchange for the module's latch); a refused
    // connection is a lost link at once. A link a command lost is not called again: the cycle ends the poll.
    std::optional<Frame> probe() {
        if (state_.link_lost()) return std::nullopt;
        try {
            return sample_();
        } catch (const RemoteConnectRefused& e) {
            note_probe_error();
            lose_link(e.what());
            throw;
        } catch (const std::exception&) {
            note_probe_error();
            throw;
        }
    }

    // Kept for the Connect, which answers a first frame that never came with the probe's own error.
    void note_probe_error() {
        std::lock_guard<std::mutex> lock(mutex_);
        probe_error_ = std::current_exception();
    }

    // The module's loss, on the poll thread with no driver lock held (decision 0009 rule 3): Connected false and the
    // reason kept for link_fault() (rule 5); there is no transport to close, each call has its own.
    void on_lost() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_.load()) return;  // a refusal already took it down
        loss_text_ = loss_reason_locked();
        connected_.store(false);
    }

    // Decision 0009 point 2(a), decision 0023: a refused connection says nothing listens there, a lost link at once.
    // Called from the probe or a command body, both of the connection the last reset() opened (a Connect joins both
    // first); while the Connect runs, connected_ is false and the refusal only fails it.
    void lose_link(const std::string& reason) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connected()) return;
            connected_.store(false);
            loss_text_ = reason;
        }
        state_.lose_link();
        ALPACA_LOG_ERROR(kComponent, "Upstream " + label_ + " lost: " + reason +
                                         "; Connected is false until the client connects again");
    }

    const RemoteDeviceConfig config_;
    const ServerAddressSource self_;
    // The poll's wait, the fault stamp and the loss bound (decision 0005).
    util::TaskClock& clock_;
    // The I/O lock is the client's own: one call at a time per upstream device.
    RemoteAlpacaClient client_;
    const std::string label_;  // "<host>:<port> device <n>"
    const std::string type_name_;
    const int platform7_version_;
    const std::function<Frame()> sample_;

    // Decision 0009 rule 3: false on a loss, so every member throws NotConnected and DeviceState is empty. No
    // LinkLostStamp: nothing here moves on a relink (no #521 consumer).
    std::atomic<bool> connected_{false};
    std::function<void()> command_done_hook_;  // test seam: set_command_done_hook()
    // Under the driver's transition_mutex_: the upstream was told Connect and not yet Disconnect, and its
    // InterfaceVersion then.
    bool forwarded_connect_ = false;
    int forwarded_version_ = 0;

    // The state lock, a leaf of the driver's own locks (decision 0016): the snapshot mutex nests under it. Never held
    // across an upstream call.
    mutable std::mutex mutex_;
    std::string loss_text_;           // why the link was lost; empty while it is not
    std::exception_ptr probe_error_;  // the last failed probe's error, for the Connect
    std::string name_;
    std::string description_;

    // Last, so their bodies are joined before the members the bodies touch die. state_ (decision 0010 rule 7, #2496)
    // holds the frame, the poll and the link's fault and loss latches. The command slot comes after it, so the
    // command bodies, which publish to the frame, are joined before state_ dies.
    util::OperationGeneration poll_generation_;
    State state_;
    util::OperationGeneration command_generation_;
    util::AsyncOperation command_op_;
};

}  // namespace alpacacore::vendor::remotealpaca::detail
