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

// The client for one device of an upstream Alpaca server (decision 0023, issue #1753 slice 2): the
// remotealpaca vendor's wire protocol, built on util::HttpClient. A later slice builds the proxied device
// drivers on it; this one has no driver, no catalog entry and no device type.
//
// Rules:
//   1. Every call sends this client's own ClientID and the next ClientTransactionID (1, 2, ...), never a
//      caller's. Parameter names are sent as the caller writes them, which is the canonical Alpaca casing
//      ("Brightness", "SensorName"); ClientID and ClientTransactionID are written that way here.
//   2. A reply whose ErrorNumber is not zero throws AlpacaException with that number unchanged and the
//      upstream's ErrorMessage prefixed with "<host>:<port>: " (decision 0023 rule 5).
//   3. No usable reply throws RemoteTransportError (DriverException): the HTTP call failed or timed out, the
//      status was not 200, the body is not an Alpaca JSON object, or Value is missing or of the wrong type.
//      The driver decides whether that is a lost link (NotConnected) or one failed call.
//   4. One call at a time per client: calls are serialized on the client's own mutex.
//   5. The reply's JSON is parsed inside the wrapper; no JSON type appears in this header
//      (decision 0013 section 4).

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/http_client.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace alpacacore::vendor::remotealpaca {

// The per-call timeout. Fixed, not a config field (#1753 maintainer decision, 2026-11-16).
inline constexpr std::chrono::milliseconds kRemoteCallTimeout{5000};

// One device on an upstream Alpaca server.
struct UpstreamDevice {
    std::string host;         // a name, an IPv4 address or an IPv6 address, without brackets or a scheme
    int port = 0;             // 1 to 65535
    std::string device_type;  // the Alpaca path segment, lower case: "observingconditions"
    int device_number = 0;    // the upstream's own device number, 0 or more
};

// No usable Alpaca reply arrived (rule 3). Always DriverException.
class RemoteTransportError : public AlpacaException {
public:
    explicit RemoteTransportError(const std::string& message)
        : AlpacaException(message, AlpacaError::DriverException) {}
};

// Name and value pairs, in the order they are sent. Names in canonical Alpaca casing.
using RemoteParams = std::vector<std::pair<std::string, std::string>>;

class RemoteAlpacaClient {
public:
    // Throws AlpacaException(InvalidValue) for an empty or malformed host, a port outside 1 to 65535, a
    // device type that is not lower-case letters, or a negative device number. `timeout` is for tests;
    // the vendor passes kRemoteCallTimeout.
    RemoteAlpacaClient(std::shared_ptr<util::HttpClient> http, UpstreamDevice upstream, std::uint32_t client_id,
                       std::chrono::milliseconds timeout = kRemoteCallTimeout);

    RemoteAlpacaClient(const RemoteAlpacaClient&) = delete;
    RemoteAlpacaClient& operator=(const RemoteAlpacaClient&) = delete;

    // GET `member` (lower case, as in the URL) and return its Value as the named type. A JSON number with
    // a fraction, or outside int, is a transport error for get_int().
    bool get_bool(std::string_view member, const RemoteParams& params = {});
    int get_int(std::string_view member, const RemoteParams& params = {});
    double get_double(std::string_view member, const RemoteParams& params = {});
    std::string get_string(std::string_view member, const RemoteParams& params = {});
    std::vector<std::string> get_string_list(std::string_view member, const RemoteParams& params = {});

    // PUT `member` with a form body; a Value in the reply is ignored.
    void put(std::string_view member, const RemoteParams& params = {});

    const UpstreamDevice& upstream() const { return upstream_; }
    std::uint32_t client_id() const { return client_id_; }

    // "http://<host>:<port>/api/v1/<type>/<number>/<member>", the host in brackets when it is IPv6.
    std::string member_url(std::string_view member) const;

private:
    enum class Verb : std::uint8_t { Get, Put };
    struct Reply;
    Reply call(Verb verb, std::string_view member, const RemoteParams& params);

    std::shared_ptr<util::HttpClient> http_;
    UpstreamDevice upstream_;
    std::uint32_t client_id_;
    std::chrono::milliseconds timeout_;
    std::string prefix_;  // "<host>:<port>: "
    std::mutex call_mutex_;
    std::uint32_t next_transaction_id_ = 1;  // under call_mutex_
};

}  // namespace alpacacore::vendor::remotealpaca
