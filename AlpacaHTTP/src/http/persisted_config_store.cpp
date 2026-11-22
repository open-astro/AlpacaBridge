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

#include "persisted_config_store.h"

#include <alpacacore/util/atomic_file.h>
#include <alpacacore/util/unique_id.h>
#include <alpacahttp/util/log_text.h>
#include <alpacahttp/util/logging_adapter.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "../core/catalog_json.h"
#include "device_call.h"

namespace alpacahttp {

namespace {

// An exception's text, made safe for a log line (issue #2087), as router.cpp does.
std::string log_what(const std::exception& e) { return util::escape_for_log(e.what(), 4096); }

}  // namespace

std::optional<PersistedKey> persisted_key(const nlohmann::json& entry) {
    if (!entry.is_object()) {
        return std::nullopt;
    }
    const auto type_it = entry.find("deviceType");
    const auto number_it = entry.find("deviceNumber");
    // The numbers catalog_json::get_int() accepts at registration (whole_json_int):
    // a hand-written 3.0 registers as device 3, so the walks over the
    // persisted list must find that same entry or removedevice would drop it
    // from the registry and leave it in the file. A number registration
    // refuses (4294967299, 3.5) has no key, like a wrong-typed one.
    if (type_it == entry.end() || !type_it->is_string() || number_it == entry.end()) {
        return std::nullopt;
    }
    const auto number = catalog_json::whole_json_int(*number_it);
    if (!number) {
        return std::nullopt;
    }
    PersistedKey key;
    key.device_type = to_lower_copy(type_it->get<std::string>());
    key.device_number = *number;
    const auto vendor_it = entry.find("vendor");
    if (vendor_it != entry.end() && vendor_it->is_string()) {
        key.vendor = vendor_it->get<std::string>();
    }
    return key;
}

std::string stored_unique_id(const nlohmann::json& entry) {
    const auto it = entry.find("uniqueId");
    if (it == entry.end() || !it->is_string()) return {};
    const std::string value = it->get<std::string>();
    return alpacacore::util::is_keepable_unique_id(value) ? value : std::string();
}

std::string choose_unique_id(const nlohmann::json& entry, const std::set<std::string>& taken) {
    std::string value = stored_unique_id(entry);
    while (value.empty() || taken.count(value) != 0) {
        value = alpacacore::util::generate_unique_id();
    }
    return value;
}

PersistedConfigStore::PersistedConfigStore(const std::filesystem::path& directory)
    : file_(directory / "registered_devices.json") {}

bool PersistedConfigStore::matches(const nlohmann::json& entry, const std::string& device_type, int device_number) {
    const auto key = persisted_key(entry);
    return key && key->device_type == to_lower_copy(device_type) && key->device_number == device_number;
}

PersistedConfigStore::LoadResult PersistedConfigStore::load(const Sanitize& sanitize) {
    LoadResult result;
    bool assigned_unique_ids = false;
    bool dropped_duplicate = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (loaded_) {
            return result;
        }
        loaded_ = true;
        result.first = true;

        try {
            if (!std::filesystem::exists(file_)) {
                entries_.clear();
                return result;
            }

            std::ifstream in(file_);
            if (!in) {
                throw std::runtime_error("Unable to open " + file_.string() + " for reading");
            }

            try {
                in >> result.entries;
                if (!result.entries.is_array()) {
                    throw std::runtime_error("Persisted device file must contain a JSON array");
                }
            } catch (const std::exception& parse_error) {
                // Set the file aside: the next save would otherwise overwrite the only copy (#1688).
                const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();
                const std::filesystem::path aside = file_.string() + ".corrupt-" + std::to_string(stamp);
                std::error_code rename_ec;
                std::filesystem::rename(file_, aside, rename_ec);
                util::log_error(
                    "Registered devices file is unreadable (" + log_what(parse_error) + "); " +
                    (rename_ec ? "could not set it aside: " + rename_ec.message() : "kept as " + aside.string()));
                entries_.clear();
                return result;
            }

            entries_.clear();
            // One entry per type and number, as the registry holds: keep the first in file order.
            // The router registers `result.entries`, so a dropped one leaves it too.
            {
                std::map<std::pair<std::string, int>, std::string> seen;  // type and number -> the kept vendor
                nlohmann::json kept = nlohmann::json::array();
                for (auto& entry : result.entries) {
                    const auto key = persisted_key(entry);
                    if (key) {
                        const auto first = seen.emplace(std::pair(key->device_type, key->device_number), key->vendor);
                        if (!first.second) {
                            util::log_warning("Dropping a persisted " + util::escape_for_log(key->vendor) + " " +
                                              util::escape_for_log(key->device_type) + " " +
                                              std::to_string(key->device_number) + ": the earlier " +
                                              util::escape_for_log(first.first->second) +
                                              " entry already holds that device type and number");
                            dropped_duplicate = true;
                            continue;
                        }
                    }
                    kept.push_back(std::move(entry));
                }
                result.entries = std::move(kept);
            }
            // Decision 0018: every stored entry without a keepable ID gets a GUID once (the
            // upgrade, or a hand-edited file), and a duplicated one is replaced in the later entry.
            std::set<std::string> taken;
            for (auto& entry : result.entries) {
                nlohmann::json sanitized = sanitize(entry);
                const std::string unique_id = choose_unique_id(sanitized, taken);
                if (stored_unique_id(sanitized).empty()) result.first_unique_ids.insert(unique_id);
                if (stored_unique_id(sanitized) != unique_id) {
                    assigned_unique_ids = true;
                    if (entry.is_object()) entry["uniqueId"] = unique_id;
                    sanitized["uniqueId"] = unique_id;
                }
                taken.insert(unique_id);
                entries_.push_back(std::move(sanitized));
            }
            result.live_unique_ids = std::move(taken);
            result.parsed = true;
        } catch (const std::exception& e) {
            util::log_error("Failed to load registered devices: " + log_what(e));
            return result;
        }
    }
    if (assigned_unique_ids) {
        util::log_info("Assigned a persisted UniqueID to the stored devices that had none (decision 0018)");
    }
    if (assigned_unique_ids || dropped_duplicate) {
        if (!save() && assigned_unique_ids && !result.first_unique_ids.empty()) {
            // The next start would mint other GUIDs and prune whatever moved onto
            // these, so nothing moves until the file can keep them.
            util::log_warning("Could not store the new UniqueIDs; saved rotator offsets stay under their old IDs");
            result.first_unique_ids.clear();
        }
    }
    return result;
}

bool PersistedConfigStore::save() const {
    try {
        if (file_.has_parent_path()) {
            std::filesystem::create_directories(file_.parent_path());
        }

        // One writer at a time, and never in place: site PUTs from two
        // clients run on two workers (#444), and two truncate-and-write
        // passes on the same file end as one dump's head plus the other's
        // tail, which the next start cannot parse and so loads NO devices.
        // Writing a sibling temp file and renaming it over the real one
        // makes each save atomic for a concurrent reader: the file is always
        // a complete dump. write_file_atomic also fsyncs the temp file and the
        // directory, so a crash or a full disk keeps the previous file (#1688).
        //
        // The file lock is taken BEFORE the snapshot (lock order: file, then
        // list, the same everywhere). Snapshotting first let worker A copy
        // the list, lose the CPU, and rename its older copy over the newer
        // dump worker B had just written: memory right, disk one revision
        // stale until the next changed PUT, lost across a restart.
        std::lock_guard<std::mutex> file_lock(file_mutex_);
        nlohmann::json payload;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            payload = entries_;
        }
        alpacacore::util::write_file_atomic(file_, payload.dump(4));
        return true;
    } catch (const std::exception& e) {
        util::log_error("Failed to persist registered devices: " + log_what(e));
        return false;
    }
}

std::vector<nlohmann::json> PersistedConfigStore::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}

void PersistedConfigStore::add_or_replace(const nlohmann::json& config) {
    if (!catalog_json::has_value(config, "deviceType") || !catalog_json::has_value(config, "vendor") ||
        !catalog_json::has_value(config, "deviceNumber")) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const std::string device_type = to_lower_copy(catalog_json::get_string(config, "deviceType"));
    const int device_number = catalog_json::get_int(config, "deviceNumber", -1);
    for (auto& existing : entries_) {
        if (matches(existing, device_type, device_number)) {
            existing = config;
            return;
        }
    }

    entries_.push_back(config);
}

bool PersistedConfigStore::remove(const std::string& device_type, int device_number) {
    if (device_type.empty() || device_number < 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t before = entries_.size();
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
                       [&](const nlohmann::json& entry) { return matches(entry, device_type, device_number); }),
        entries_.end());
    return entries_.size() < before;
}

PersistedConfigStore::EditResult PersistedConfigStore::edit_by_type_and_number(const std::string& device_type,
                                                                               int device_number, const Edit& edit) {
    EditResult result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : entries_) {
            if (matches(entry, device_type, device_number)) {
                result.changed = edit(entry);
                break;
            }
        }
    }
    if (result.changed) {
        result.saved = save();
    }
    return result;
}

PersistedConfigStore::Reservation::Reservation(PersistedConfigStore* store, std::string unique_id, bool adds_entry)
    : store_(store), unique_id_(std::move(unique_id)), adds_entry_(adds_entry) {}

PersistedConfigStore::Reservation::Reservation(Reservation&& other) noexcept
    : store_(std::exchange(other.store_, nullptr)),
      unique_id_(std::move(other.unique_id_)),
      adds_entry_(other.adds_entry_) {}

PersistedConfigStore::Reservation::~Reservation() {
    if (store_ == nullptr) return;
    std::lock_guard<std::mutex> lock(store_->mutex_);
    store_->reserved_unique_ids_.erase(unique_id_);
    if (adds_entry_) --store_->pending_new_entries_;
}

std::optional<PersistedConfigStore::Reservation> PersistedConfigStore::reserve_unique_id(nlohmann::json& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool adds_entry = true;
    std::set<std::string> taken = reserved_unique_ids_;
    const auto replaced = persisted_key(config);
    for (const auto& entry : entries_) {
        if (replaced && matches(entry, replaced->device_type, replaced->device_number)) {
            adds_entry = false;
            continue;
        }
        if (const std::string held = stored_unique_id(entry); !held.empty()) taken.insert(held);
    }
    if (adds_entry && entries_.size() + pending_new_entries_ >= kMaxEntries) {
        return std::nullopt;
    }
    std::string unique_id = choose_unique_id(config, taken);
    config["uniqueId"] = unique_id;
    reserved_unique_ids_.insert(unique_id);
    if (adds_entry) ++pending_new_entries_;
    return Reservation(this, std::move(unique_id), adds_entry);
}

}  // namespace alpacahttp
