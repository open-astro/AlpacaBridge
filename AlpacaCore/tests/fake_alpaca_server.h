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

// A loopback fake Alpaca server for the remotealpaca client (issue #1753 slice 2): 127.0.0.1 on an
// ephemeral port, over LoopbackHttpServer. It answers a member with the Value or the error a test scripted,
// echoes the ClientTransactionID it was sent and records every request. Harsher than a good server by mode:
// Stall (never answers), Drop (closes with no bytes), ServerError (HTTP 500), BadJson (a cut-off body) and
// NoErrorNumber (a reply without ErrorNumber). An unscripted member answers 0x400 (not implemented), as a
// real server does. No network beyond 127.0.0.1.

#include <atomic>
#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "loopback_http_server.h"

namespace alpacacore::test {

class FakeAlpacaServer {
public:
    enum class Mode { Good, Stall, Drop, ServerError, BadJson, NoErrorNumber };

    struct Request {
        std::string method;  // "GET" or "PUT"
        std::string path;    // "/api/v1/observingconditions/0/temperature"
        std::string query;   // after '?', as sent
        std::string body;    // the form body, as sent
    };

    FakeAlpacaServer() {
        server_.set_responder([this](const std::string& request) { return answer(request); });
    }

    int port() const { return server_.port(); }

    // `value_json` is the Value as JSON text: "21.5", "true", "\"text\"", "[\"a\",\"b\"]".
    void set_value(const std::string& member, std::string value_json) {
        std::lock_guard<std::mutex> lock(mutex_);
        replies_[member] = {std::move(value_json), 0, {}};
    }

    void set_error(const std::string& member, int error_number, std::string message) {
        std::lock_guard<std::mutex> lock(mutex_);
        replies_[member] = {{}, error_number, std::move(message)};
    }

    void set_mode(Mode mode) {
        mode_ = mode;
        switch (mode) {
            case Mode::Stall:
                server_.set_mode(LoopbackHttpServer::Mode::Stall);
                break;
            case Mode::Drop:
                server_.set_mode(LoopbackHttpServer::Mode::DropAfterAccept);
                break;
            case Mode::ServerError:
                server_.set_mode(LoopbackHttpServer::Mode::ServerError);
                break;
            default:
                server_.set_mode(LoopbackHttpServer::Mode::Answer);
                break;
        }
    }

    std::vector<Request> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

private:
    struct Scripted {
        std::string value_json;  // empty with an error
        int error_number = 0;
        std::string message;
    };

    // The value of `key` in an a=1&b=2 string, or "0".
    static std::string field(const std::string& encoded, const std::string& key) {
        std::size_t at = 0;
        while (at <= encoded.size()) {
            const std::size_t end = encoded.find('&', at);
            const std::string pair = encoded.substr(at, end == std::string::npos ? std::string::npos : end - at);
            if (pair.rfind(key + "=", 0) == 0) return pair.substr(key.size() + 1);
            if (end == std::string::npos) break;
            at = end + 1;
        }
        return "0";
    }

    std::pair<std::string, std::string> answer(const std::string& raw) {
        Request request;
        const std::size_t line_end = raw.find("\r\n");
        const std::string line = raw.substr(0, line_end);
        const std::size_t space = line.find(' ');
        const std::size_t target_end = line.find(' ', space + 1);
        request.method = line.substr(0, space);
        const std::string target = line.substr(space + 1, target_end - space - 1);
        const std::size_t question = target.find('?');
        request.path = target.substr(0, question);
        request.query = question == std::string::npos ? std::string() : target.substr(question + 1);
        const std::size_t header_end = raw.find("\r\n\r\n");
        request.body = header_end == std::string::npos ? std::string() : raw.substr(header_end + 4);
        const std::string member = request.path.substr(request.path.rfind('/') + 1);
        const std::string transaction =
            field(request.method == "PUT" ? request.body : request.query, "ClientTransactionID");

        Scripted scripted{{}, 0x400, "Method " + member + " is not implemented"};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(request);
            const auto it = replies_.find(member);
            if (it != replies_.end()) scripted = it->second;
        }
        if (mode_ == Mode::BadJson) return {"200 OK", "{\"Value\": 1, \"ErrorNumber\""};
        std::string body = "{";
        if (scripted.error_number == 0 && !scripted.value_json.empty())
            body += "\"Value\":" + scripted.value_json + ",";
        body += "\"ClientTransactionID\":" + transaction + ",\"ServerTransactionID\":" + std::to_string(++server_tx_);
        if (mode_ != Mode::NoErrorNumber) {
            body += ",\"ErrorNumber\":" + std::to_string(scripted.error_number) + ",\"ErrorMessage\":\"" +
                    scripted.message + "\"";
        }
        body += "}";
        return {"200 OK", body};
    }

    std::atomic<Mode> mode_{Mode::Good};
    std::atomic<int> server_tx_{0};
    mutable std::mutex mutex_;
    std::map<std::string, Scripted> replies_;
    std::vector<Request> requests_;
    // Last, so it is destroyed first: its destructor joins the thread that calls answer().
    LoopbackHttpServer server_;
};

}  // namespace alpacacore::test
