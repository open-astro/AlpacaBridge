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

// Process-wide ownership of serial-port paths (issue #1202).
//
// Every serial driver claims a port before it opens it, and the claim names
// who holds it and why:
//
//   * a Link claim belongs to a connected driver for as long as it holds the
//     port. A second Link claim on the same port is refused, and an
//     auto-detect probe skips the port without opening it.
//   * a Probe claim belongs to an auto-detect probe for the few seconds it
//     has the port open. Another probe or a connect waits for it to finish
//     (bounded), so two devices set to auto that connect at the same time
//     never open the same candidate together.
//
// Many vendors ship the same CH340 adapter, so the probe of one vendor's
// auto-detect matches every other vendor's device. Before this registry
// covered every wrapper, a probe opened a connected device's live port at its
// own baud rate and the connected device stopped answering (#1202, the
// Gemini PDH taken down by a flat panel scan).
//
// Claims are RAII (SerialPortClaim): the claim is released on every exit
// path, including a throw, so a wrapper cannot forget to release it. Wrappers
// do not take claims themselves; util::SerialLink (serial_link.h) takes one
// for every tty it opens. scripts/check_serial_port_ownership.py keeps it
// that way.
//
// Keys are canonical paths (std::filesystem::canonical) where resolvable, so
// a by-id symlink and its /dev/ttyUSBn target compare equal.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace alpacacore::util {

enum class SerialClaimKind : std::uint8_t {
    Probe,  ///< an auto-detect probe has the port open for a few seconds
    Link,   ///< a connected driver holds the port
};

/// How long a probe or a connect waits for another probe that has the port
/// open. The slowest probe in the tree is the QHY CFW3's, which waits up to
/// ~17 s for the wheel's boot byte (`enumerate_cfw3_ports(boot_timeout_ms)`);
/// anything still claimed after this is treated as held. Keep this above that
/// probe's worst case: if it grows past this wait, a second scan that meets it
/// on the same port is refused instead of delayed.
inline constexpr std::chrono::milliseconds kSerialProbeClaimWait{20000};

/// The canonical form of @p path, or @p path itself when it cannot be
/// resolved (a node that vanished, a test path). Never throws.
inline std::string canonical_serial_path(const std::string& path) {
    if (path.empty()) return path;
    std::error_code ec;
    auto resolved = std::filesystem::canonical(path, ec);
    if (ec) return path;
    return resolved.string();
}

/// Who holds a port, as returned by serial_port_holder().
struct SerialPortHolder {
    SerialClaimKind kind = SerialClaimKind::Link;
    std::string owner;  ///< operator-facing name, e.g. "Gemini PDH"
};

namespace detail {
struct SerialRegistry {
    std::mutex mutex;
    std::condition_variable released;
    std::map<std::string, SerialPortHolder> holders;
};
// Never destroyed: a wrapper living in a function-local static singleton (the
// iOptron mount) releases its claim from its own destructor at exit, which can
// run after a destroyed registry's. Leaking one map and mutex avoids the
// static-destruction-order question entirely.
inline SerialRegistry& serial_registry() {
    static auto* r = new SerialRegistry();  // NOLINT(cppcoreguidelines-owning-memory)
    return *r;
}
}  // namespace detail

/// The current holder of @p path (canonicalised), if any.
inline std::optional<SerialPortHolder> serial_port_holder(const std::string& path) {
    const std::string key = canonical_serial_path(path);
    auto& reg = detail::serial_registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    auto it = reg.holders.find(key);
    if (it == reg.holders.end()) return std::nullopt;
    return it->second;
}

/// True if a connected driver or a probe holds @p path.
inline bool is_serial_port_in_use(const std::string& path) { return serial_port_holder(path).has_value(); }

/// How an auto-detect scan treats one candidate port, see serial_candidate_use().
enum class SerialCandidateUse : std::uint8_t {
    Free,           ///< probe it (the probe's claim waits out another scan's probe)
    HeldBySibling,  ///< a connected device of this same scan's kind holds it
    HeldByOther,    ///< a connected device of another kind holds it: skip
};

/**
 * @brief Classify @p path for the auto-detect scan whose devices connect as
 *        @p owner.
 *
 * A scan must never open a port a connected device holds. A port held by a
 * connected device of the SAME kind still counts toward the auto-detect index
 * without being probed: list it in the results at its scan position (with
 * whatever identity the scan can give without I/O), so "the second Gemini
 * focuser" stays the second one while the first is connected. A driver that
 * then picks that entry is refused by its link claim, which names the holder.
 */
inline SerialCandidateUse serial_candidate_use(const std::string& path, const std::string& owner) {
    const auto holder = serial_port_holder(path);
    if (!holder || holder->kind == SerialClaimKind::Probe) return SerialCandidateUse::Free;
    return holder->owner == owner ? SerialCandidateUse::HeldBySibling : SerialCandidateUse::HeldByOther;
}

/**
 * @brief An RAII claim on one serial-port path.
 *
 * Move-only. The destructor releases the claim and wakes anyone waiting for
 * it. An empty claim (acquire() refused) holds nothing.
 */
class SerialPortClaim {
public:
    SerialPortClaim() = default;
    ~SerialPortClaim() { release(); }
    SerialPortClaim(const SerialPortClaim&) = delete;
    SerialPortClaim& operator=(const SerialPortClaim&) = delete;
    SerialPortClaim(SerialPortClaim&& other) noexcept : key_(std::exchange(other.key_, std::string())) {}
    SerialPortClaim& operator=(SerialPortClaim&& other) noexcept {
        if (this != &other) {
            release();
            key_ = std::exchange(other.key_, std::string());
        }
        return *this;
    }

    /**
     * @brief Claim @p path for @p owner.
     *
     * A port held by a Link is refused at once, whatever @p kind asks for.
     * A port held by a Probe is waited for, up to @p probe_wait, and then
     * refused. On refusal the returned claim is empty and @p holder (when
     * given) receives the current holder.
     */
    static SerialPortClaim acquire(const std::string& path, SerialClaimKind kind, const std::string& owner,
                                   std::chrono::milliseconds probe_wait = kSerialProbeClaimWait,
                                   SerialPortHolder* holder = nullptr) {
        SerialPortClaim claim;
        if (path.empty()) return claim;
        const std::string key = canonical_serial_path(path);
        auto& reg = detail::serial_registry();
        std::unique_lock<std::mutex> lock(reg.mutex);
        const auto deadline = std::chrono::steady_clock::now() + probe_wait;
        for (;;) {
            auto it = reg.holders.find(key);
            if (it == reg.holders.end()) {
                reg.holders.emplace(key, SerialPortHolder{kind, owner});
                claim.key_ = key;
                return claim;
            }
            if (it->second.kind == SerialClaimKind::Link ||
                reg.released.wait_until(lock, deadline) == std::cv_status::timeout) {
                // Re-read: the wait may have ended on the deadline just as the
                // probe released the port.
                it = reg.holders.find(key);
                if (it == reg.holders.end()) continue;
                if (holder != nullptr) *holder = it->second;
                return claim;
            }
        }
    }

    bool held() const { return !key_.empty(); }
    explicit operator bool() const { return held(); }

    /// The canonical path this claim holds (empty when it holds nothing).
    const std::string& key() const { return key_; }

    /// Release the claim now. Idempotent.
    void release() noexcept {
        if (key_.empty()) return;
        auto& reg = detail::serial_registry();
        {
            std::lock_guard<std::mutex> lock(reg.mutex);
            reg.holders.erase(key_);
        }
        reg.released.notify_all();
        key_.clear();
    }

private:
    std::string key_;
};

/// The refusal text for a port @p holder holds, written for an operator.
inline std::string serial_port_held_message(const std::string& path, const SerialPortHolder& holder) {
    if (holder.kind == SerialClaimKind::Probe) {
        return "Serial port " + path + " is busy: the " + holder.owner +
               " auto-detect scan is still probing it. Try again in a few seconds.";
    }
    return "Serial port " + path + " is already in use by the connected " + holder.owner +
           ". Each device needs its own port; check the port setting of both devices.";
}

}  // namespace alpacacore::util
