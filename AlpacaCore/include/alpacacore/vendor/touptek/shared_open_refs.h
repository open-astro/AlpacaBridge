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

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace alpacacore::vendor::touptek {

/**
 * Reference count for ToupTek devices opened by id, shared by the Toupcam-family
 * wrapper (ToupcamFamilySDK) and the test fake (FakeToupTekSDK) so the fake
 * cannot drift from production. Pure bookkeeping: the caller does the SDK Open
 * and Close and holds its own lock around every call.
 *
 * One camera and its thermal switch (or integrated focuser / filter wheel) share
 * one SDK handle: the first opener physically opens, the last releaser closes.
 */
template <class Handle>
class SharedOpenRefs {
public:
    enum class Release : std::uint8_t {
        Untracked,  // handle not open (a double close): the caller must not Close it
        StillHeld,  // another holder remains: the caller must not Close it
        Last,       // final release: the caller must Close the handle now
    };

    // A device already open: bump its count and return the shared handle.
    std::optional<Handle> acquire(const std::string& id) {
        auto it = by_id_.find(id);
        if (it == by_id_.end()) {
            return std::nullopt;
        }
        ++it->second.count;
        return it->second.handle;
    }

    // Record the first holder of a freshly opened handle (count 1).
    void publish(const std::string& id, Handle handle) {
        by_id_[id] = Entry{handle, 1};
        try {
            id_by_handle_[handle] = id;
        } catch (...) {
            by_id_.erase(id);
            throw;
        }
    }

    // Drop a holder. Only a count above 1 means another driver still holds the
    // device; a lower count releases it (never decrement past the erase).
    Release release(Handle handle) {
        auto hit = id_by_handle_.find(handle);
        if (hit == id_by_handle_.end()) {
            return Release::Untracked;
        }
        auto sit = by_id_.find(hit->second);
        if (sit != by_id_.end() && sit->second.count > 1) {
            --sit->second.count;
            return Release::StillHeld;
        }
        if (sit != by_id_.end()) {
            by_id_.erase(sit);
        }
        id_by_handle_.erase(hit);
        return Release::Last;
    }

    int count(const std::string& id) const {
        auto it = by_id_.find(id);
        return it == by_id_.end() ? 0 : it->second.count;
    }

private:
    struct Entry {
        Handle handle{};
        int count{0};
    };
    std::map<std::string, Entry> by_id_;
    // The single is-open record: a handle is open exactly while it has an entry here.
    std::map<Handle, std::string> id_by_handle_;
};

}  // namespace alpacacore::vendor::touptek
