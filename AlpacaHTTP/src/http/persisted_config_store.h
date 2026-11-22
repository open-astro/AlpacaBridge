// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#pragma once

// Internal seam (lives under src/, not include/): the persisted device list,
// config/registered_devices.json (issue #2481). It owns the file, the list in
// memory, its lookups, its lock order and the decision 0018 unique-ID
// reservation. Registering devices, pruning the rotator-offset and power-port
// stores, legacy adoption and the power-on state stay in the Router, which
// asks this store for the entries.

#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace alpacahttp {

// The (vendor, deviceType, deviceNumber) key of a persisted-list entry, read
// through typed guards rather than value() (#388): a hand-edited file can
// carry "deviceNumber": "3", and that entry stays in the list (only its
// registration fails), so every walk over the list must step past it
// instead of throwing type_error on a PUT or POST aimed at a different,
// valid device. Absent or wrong-typed type/number yields no key; vendor is
// "" when missing. deviceType is lowercased, since the web UI and the tests
// post it lowercase while the registry answers "Telescope".
struct PersistedKey {
    std::string vendor{};
    std::string device_type{};  // lowercase
    int device_number = -1;
};

std::optional<PersistedKey> persisted_key(const nlohmann::json& entry);

// Decision 0018: the persisted UniqueID of a configured device. A stored or
// client-sent value is kept when it is a keepable ID (a version 4 UUID or a
// long non-slot string such as ZWO_UID_<hex>) that no other entry holds (the
// web UI edits a device by removing and re-adding it, sending the ID back);
// only an entry with none, a device-number ID or a duplicate gets a fresh GUID.
std::string stored_unique_id(const nlohmann::json& entry);
std::string choose_unique_id(const nlohmann::json& entry, const std::set<std::string>& taken);

class PersistedConfigStore {
public:
    // open-astro#2292 (ADR 0007 rule 10): the most entries configuredevice keeps.
    // A new entry past it is refused; replacing an entry is not. A rig has tens.
    static constexpr std::size_t kMaxEntries = 128;

    // The file is `directory`/registered_devices.json; nothing is read until load().
    explicit PersistedConfigStore(const std::filesystem::path& directory);
    PersistedConfigStore(const PersistedConfigStore&) = delete;
    PersistedConfigStore& operator=(const PersistedConfigStore&) = delete;

    const std::filesystem::path& file() const { return file_; }

    // The one identity rule of a stored device, the same as the live registry's: device
    // type (compared lowercase) and number. The vendor is data: configuring another
    // vendor at a stored type and number replaces the entry.
    static bool matches(const nlohmann::json& entry, const std::string& device_type, int device_number);

    struct LoadResult {
        // This call read the file: false when an earlier call already did.
        bool first = false;
        // The file was read and every entry stored. False when it was absent,
        // unreadable (and set aside) or failed to load; then nothing below is set.
        bool parsed = false;
        // The file's entries as read, each holding the uniqueId the load chose.
        nlohmann::json entries = nlohmann::json::array();
        // Every stored entry's uniqueId.
        std::set<std::string> live_unique_ids;
        // The entries this load gave their first GUID (the upgrade, or a
        // hand-edited file), emptied when the new IDs could not be saved.
        std::set<std::string> first_unique_ids;
    };
    using Sanitize = std::function<nlohmann::json(const nlohmann::json&)>;
    // Once per store: reads the file, sets a corrupt one aside (#1688), keeps the
    // first entry at each type and number and drops a later one with a warning
    // (also from `entries`), keeps `sanitize(entry)` of each entry with a decision
    // 0018 uniqueId, and saves the list back when it assigned an ID or dropped an
    // entry. `sanitize` runs under the list lock and must not call into this store.
    LoadResult load(const Sanitize& sanitize);

    // Writes the list to the file atomically. True when it was written; false
    // is already logged at ERROR.
    bool save() const;

    // A copy of the list.
    std::vector<nlohmann::json> snapshot() const;

    // Replaces the entry matching the config's type and number (whatever its
    // vendor), or appends it. A config without all three is ignored. Does not save.
    void add_or_replace(const nlohmann::json& config);
    // Removes the entry matching the type and number. True when one was
    // removed. Does not save.
    bool remove(const std::string& device_type, int device_number);

    // Runs `edit` on the entry matching the type and number, under the list lock;
    // `edit` returns true when it changed the entry and must not call into this
    // store. A change is saved after the lock is released.
    struct EditResult {
        bool changed = false;
        bool saved = false;
    };
    using Edit = std::function<bool(nlohmann::json&)>;
    EditResult edit_by_type_and_number(const std::string& device_type, int device_number, const Edit& edit);

    // Holds a uniqueId reserve_unique_id() chose, and the entry-cap slot of a
    // call that adds an entry, until it is destroyed.
    class Reservation {
    public:
        Reservation(Reservation&& other) noexcept;
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        Reservation& operator=(Reservation&&) = delete;
        ~Reservation();

    private:
        friend class PersistedConfigStore;
        Reservation(PersistedConfigStore* store, std::string unique_id, bool adds_entry);
        PersistedConfigStore* store_;
        std::string unique_id_;
        bool adds_entry_;
    };
    // Decision 0018: keep a stored ID the client sends back (the web UI's edit
    // removes the entry first, so its own ID is free again) unless another
    // entry holds it, or another configuredevice call chose it and has not
    // stored its entry yet; otherwise mint one, and write it to config's
    // "uniqueId". The entry this call replaces (the one at its type and number,
    // whatever its vendor) is not another entry: its ID is free for the new
    // config. The choice is reserved until the Reservation is destroyed.
    // open-astro#2292: a call that adds an entry counts against kMaxEntries with
    // the adding calls still in flight, so concurrent calls cannot pass it
    // together. std::nullopt, with config unchanged, when the cap is reached.
    std::optional<Reservation> reserve_unique_id(nlohmann::json& config);

private:
    std::filesystem::path file_;
    // Serialises the file write in save(): request workers reach it
    // concurrently (#444), and two truncate-and-write passes interleave into
    // invalid JSON. Lock order: this one FIRST, then mutex_ inside it for the
    // snapshot, so a save can never rename an older snapshot over a newer dump.
    // Only save() takes it, and no member calls save() while holding mutex_.
    mutable std::mutex file_mutex_;
    // Guards everything below. Taken and released inside each member; never
    // held across a return to the caller.
    mutable std::mutex mutex_;
    std::vector<nlohmann::json> entries_;
    bool loaded_ = false;
    // Decision 0018: IDs a configuredevice call has chosen and not stored yet,
    // so two concurrent calls sending one ID cannot both keep it.
    std::set<std::string> reserved_unique_ids_;
    // open-astro#2292: configuredevice calls that add an entry and have not
    // stored it yet, counted against the entry cap.
    std::size_t pending_new_entries_ = 0;
};

}  // namespace alpacahttp
