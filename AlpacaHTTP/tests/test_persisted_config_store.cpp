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

// The persisted device list on its own (issue #2481): a store over a temp
// directory, no Router, no catalog and no devices.

#include <alpacacore/util/logging.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "http/persisted_config_store.h"
#include "test_assert.h"

namespace {

namespace fs = std::filesystem;
using alpacahttp::PersistedConfigStore;

// A version 4 UUID and a long non-slot ID: both keepable (decision 0018).
const std::string kUuid = "123e4567-e89b-42d3-a456-426614174000";
const std::string kZwoUid = "ZWO_UID_0123456789abcdef";

struct TempDir {
    fs::path path;
    TempDir() {
        static std::atomic<int> counter{0};
        path = fs::temp_directory_path() /
               ("alpacahttp_store_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                "_" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

nlohmann::json identity(const nlohmann::json& entry) { return entry; }

nlohmann::json entry(const std::string& vendor, const std::string& type, int number) {
    return nlohmann::json{{"vendor", vendor}, {"deviceType", type}, {"deviceNumber", number}};
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

nlohmann::json read_json(const fs::path& path) {
    std::ifstream in(path);
    return nlohmann::json::parse(in, nullptr, false);
}

std::vector<fs::path> corrupt_copies(const fs::path& dir) {
    std::vector<fs::path> found;
    for (const auto& item : fs::directory_iterator(dir)) {
        if (item.path().filename().string().rfind("registered_devices.json.corrupt-", 0) == 0) {
            found.push_back(item.path());
        }
    }
    return found;
}

}  // namespace

int main() {
    std::cout << "Testing the persisted config store...\n";
    {
        // case: a store with no file loads once, as empty, and lists nothing
        TempDir dir;
        PersistedConfigStore store(dir.path);
        EXPECT(store.file() == dir.path / "registered_devices.json");
        const auto first = store.load(identity);
        EXPECT(first.first);
        EXPECT(!first.parsed);
        EXPECT(store.snapshot().empty());
        EXPECT(!store.load(identity).first);
        EXPECT(!fs::exists(store.file()));
    }
    {
        // case: load keeps a keepable uniqueId, gives a missing or duplicated one a GUID and saves it back
        TempDir dir;
        nlohmann::json file = nlohmann::json::array();
        file.push_back(entry("zwo", "camera", 0));
        file[0]["uniqueId"] = kZwoUid;
        file.push_back(entry("celestron", "telescope", 0));  // no uniqueId
        file.push_back(entry("zwo", "focuser", 0));
        file[2]["uniqueId"] = kZwoUid;  // duplicate of the first
        write_text(dir.path / "registered_devices.json", file.dump());

        PersistedConfigStore store(dir.path);
        const auto loaded = store.load(identity);
        EXPECT(loaded.first);
        EXPECT(loaded.parsed);
        EXPECT(loaded.entries.size() == 3);
        EXPECT(loaded.entries[0]["uniqueId"] == kZwoUid);
        const std::string minted = loaded.entries[1]["uniqueId"].get<std::string>();
        const std::string replaced = loaded.entries[2]["uniqueId"].get<std::string>();
        EXPECT(!minted.empty() && minted != kZwoUid);
        EXPECT(!replaced.empty() && replaced != kZwoUid && replaced != minted);
        EXPECT(loaded.live_unique_ids == (std::set<std::string>{kZwoUid, minted, replaced}));
        // Only the entry with no stored ID gets its first GUID; the duplicate held one.
        EXPECT(loaded.first_unique_ids == std::set<std::string>{minted});

        const auto on_disk = read_json(store.file());
        EXPECT(on_disk.is_array() && on_disk.size() == 3);
        EXPECT(on_disk[1]["uniqueId"] == minted);
        EXPECT(on_disk[2]["uniqueId"] == replaced);

        // A second store over the same file keeps every ID and gives no first GUID.
        PersistedConfigStore again(dir.path);
        const auto reloaded = again.load(identity);
        EXPECT(reloaded.parsed);
        EXPECT(reloaded.first_unique_ids.empty());
        EXPECT(reloaded.entries[1]["uniqueId"] == minted);
    }
    {
        // case: load keeps what the sanitize step returns, not the raw entry
        TempDir dir;
        nlohmann::json raw = entry("zwo", "camera", 0);
        raw["uniqueId"] = kUuid;
        raw["password"] = "secret";
        write_text(dir.path / "registered_devices.json", nlohmann::json::array({raw}).dump());
        PersistedConfigStore store(dir.path);
        const auto loaded = store.load([](const nlohmann::json& e) {
            nlohmann::json kept = e;
            kept.erase("password");
            return kept;
        });
        EXPECT(loaded.parsed);
        EXPECT(loaded.entries[0].contains("password"));  // the entries handed back are the file's
        const auto snapshot = store.snapshot();
        EXPECT(snapshot.size() == 1 && !snapshot[0].contains("password"));
    }
    {
        // case: a corrupt file is set aside, loads as empty, and the next save does not overwrite it
        TempDir dir;
        write_text(dir.path / "registered_devices.json", "[{\"vendor\": \"zwo\",");
        PersistedConfigStore store(dir.path);
        const auto loaded = store.load(identity);
        EXPECT(loaded.first);
        EXPECT(!loaded.parsed);
        EXPECT(store.snapshot().empty());
        EXPECT(!fs::exists(store.file()));
        const auto aside = corrupt_copies(dir.path);
        EXPECT(aside.size() == 1);
        std::ifstream in(aside.front());
        const std::string kept((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT(kept == "[{\"vendor\": \"zwo\",");

        store.add_or_replace(entry("zwo", "camera", 0));
        EXPECT(store.save());
        EXPECT(corrupt_copies(dir.path).size() == 1);
        EXPECT(read_json(store.file()).size() == 1);
    }
    {
        // case: a file that is JSON but not an array is set aside too
        TempDir dir;
        write_text(dir.path / "registered_devices.json", "{\"vendor\": \"zwo\"}");
        PersistedConfigStore store(dir.path);
        EXPECT(!store.load(identity).parsed);
        EXPECT(corrupt_copies(dir.path).size() == 1);
    }
    {
        // case: save writes the list, a new store reads it back, and add_or_replace replaces by type and number
        TempDir dir;
        PersistedConfigStore store(fs::path(dir.path) / "nested");  // save creates the directory
        (void)store.load(identity);
        nlohmann::json camera = entry("zwo", "camera", 0);
        camera["uniqueId"] = kUuid;
        store.add_or_replace(camera);
        store.add_or_replace(entry("celestron", "telescope", 0));
        nlohmann::json camera_again = entry("zwo", "Camera", 0);
        camera_again["gain"] = 100;
        store.add_or_replace(camera_again);  // same type (any case) and number: replaced
        store.add_or_replace(entry("qhy", "camera", 1));
        store.add_or_replace(nlohmann::json{{"vendor", "zwo"}, {"deviceType", "camera"}});  // no number: ignored
        EXPECT(store.snapshot().size() == 3);
        EXPECT(store.save());

        PersistedConfigStore reader(fs::path(dir.path) / "nested");
        const auto loaded = reader.load(identity);
        EXPECT(loaded.parsed);
        const auto snapshot = reader.snapshot();
        EXPECT(snapshot.size() == 3);
        EXPECT(snapshot[0]["gain"] == 100);
        EXPECT(snapshot[0]["deviceType"] == "Camera");
        EXPECT(snapshot[2]["vendor"] == "qhy" && snapshot[2]["deviceNumber"] == 1);
    }
    {
        // case: one match rule, device type (any case) and number; vendor is data
        const nlohmann::json e = entry("zwo", "Camera", 3);
        EXPECT(PersistedConfigStore::matches(e, "camera", 3));
        EXPECT(PersistedConfigStore::matches(e, "CAMERA", 3));
        EXPECT(!PersistedConfigStore::matches(e, "camera", 4));
        EXPECT(!PersistedConfigStore::matches(e, "focuser", 3));
        // A wrong-typed number has no key and matches nothing (#388); a whole 3.0 is device 3.
        nlohmann::json bad = e;
        bad["deviceNumber"] = "3";
        EXPECT(!PersistedConfigStore::matches(bad, "camera", 3));
        nlohmann::json whole = e;
        whole["deviceNumber"] = 3.0;
        EXPECT(PersistedConfigStore::matches(whole, "camera", 3));
    }
    {
        // case: remove drops the entry at a type and number, whatever its vendor
        TempDir dir;
        PersistedConfigStore store(dir.path);
        (void)store.load(identity);
        store.add_or_replace(entry("zwo", "camera", 0));
        store.add_or_replace(entry("zwo", "camera", 1));
        EXPECT(!store.remove("camera", 5));
        EXPECT(store.remove("Camera", 0));
        EXPECT(store.snapshot().size() == 1);
        EXPECT(!store.remove("", 1));
        EXPECT(!store.remove("camera", -1));
        EXPECT(store.snapshot().size() == 1);
    }
    {
        // case: an edit runs on the first match only and saves only a change
        TempDir dir;
        PersistedConfigStore store(dir.path);
        (void)store.load(identity);
        store.add_or_replace(entry("zwo", "telescope", 0));
        store.add_or_replace(entry("celestron", "telescope", 1));
        int calls = 0;
        auto unchanged = store.edit_by_type_and_number("telescope", 0, [&](nlohmann::json&) {
            ++calls;
            return false;
        });
        EXPECT(calls == 1 && !unchanged.changed && !unchanged.saved);
        EXPECT(!fs::exists(store.file()));
        auto changed = store.edit_by_type_and_number("telescope", 1, [](nlohmann::json& e) {
            e["focalLength"] = 2.0;
            return true;
        });
        EXPECT(changed.changed && changed.saved);
        const auto on_disk = read_json(store.file());
        EXPECT(on_disk.size() == 2 && !on_disk[0].contains("focalLength") && on_disk[1]["focalLength"] == 2.0);
        auto missing = store.edit_by_type_and_number("telescope", 9, [](nlohmann::json&) { return true; });
        EXPECT(!missing.changed && !missing.saved);
    }
    {
        // case: update_fields writes only a changed field, skips an entry accept refuses, and saves only a change
        TempDir dir;
        PersistedConfigStore store(dir.path);
        (void)store.load(identity);
        auto saved = entry("zwo", "telescope", 0);
        saved["focalLength"] = 1;  // an integer: 1.0 is the same value
        store.add_or_replace(saved);
        const auto accept_all = [](const nlohmann::json&) { return true; };
        auto same = store.update_fields("telescope", 0, {{"focalLength", 1.0}}, accept_all);
        EXPECT(!same.changed && !same.saved);
        EXPECT(!fs::exists(store.file()));
        auto refused =
            store.update_fields("telescope", 0, {{"focalLength", 2.0}}, [](const nlohmann::json&) { return false; });
        EXPECT(!refused.changed && !refused.saved);
        EXPECT(store.snapshot()[0]["focalLength"] == 1);
        auto changed = store.update_fields("telescope", 0, {{"focalLength", 1.0}, {"apertureDiameter", 0.2}});
        EXPECT(changed.changed && changed.saved);
        const auto on_disk = read_json(store.file());
        EXPECT(on_disk.size() == 1 && on_disk[0]["focalLength"] == 1 && on_disk[0]["apertureDiameter"] == 0.2);
        auto missing = store.update_fields("telescope", 9, {{"focalLength", 3.0}});
        EXPECT(!missing.changed && !missing.saved);
        EXPECT(store.not_saved_text() ==
               "could NOT be written to " + store.file().generic_string() + " (see the error above)");
    }
    {
        // case: concurrent add, remove, edit and save leave a coherent list and a parsable file
        TempDir dir;
        PersistedConfigStore store(dir.path);
        (void)store.load(identity);
        constexpr int kThreads = 8;
        constexpr int kRounds = 50;
        constexpr std::size_t kEntries = kThreads;
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&store, t] {
                for (int round = 0; round < kRounds; ++round) {
                    store.add_or_replace(entry("zwo", "camera", t));
                    (void)store.save();
                    (void)store.edit_by_type_and_number("camera", t, [round](nlohmann::json& e) {
                        e["round"] = round;
                        return true;
                    });
                    (void)store.snapshot();
                    if (round % 2 == 0) {
                        EXPECT(store.remove("camera", t));
                        (void)store.save();
                    }
                }
            });
        }
        for (auto& thread : threads) thread.join();
        const auto snapshot = store.snapshot();
        EXPECT(snapshot.size() == kEntries);  // the last round of each thread is odd: its entry stays
        std::set<int> numbers;
        for (const auto& e : snapshot) {
            numbers.insert(e["deviceNumber"].get<int>());
            EXPECT(e["round"] == kRounds - 1);
        }
        EXPECT(numbers.size() == kEntries);
        EXPECT(store.save());
        const auto on_disk = read_json(store.file());
        EXPECT(on_disk.is_array() && on_disk.size() == kEntries);
    }
    {
        // case: a second vendor configured at a stored type and number replaces the entry
        TempDir dir;
        PersistedConfigStore store(dir.path);
        (void)store.load(identity);
        // A configure of a second vendor at a type and number leaves one entry, and every lookup finds it.
        store.add_or_replace(entry("zwo", "telescope", 0));
        store.add_or_replace(entry("celestron", "telescope", 0));
        EXPECT(store.snapshot().size() == 1);
        EXPECT(store.snapshot()[0]["vendor"] == "celestron");
        std::vector<std::string> seen;
        (void)store.edit_by_type_and_number("telescope", 0, [&](nlohmann::json& e) {
            seen.push_back(e["vendor"].get<std::string>());
            return false;
        });
        EXPECT(seen == std::vector<std::string>{"celestron"});
        EXPECT(store.remove("telescope", 0));
        EXPECT(store.snapshot().empty());
    }
    {
        // case: load keeps the first of two entries at one type and number, drops the later, warns, and saves
        TempDir dir;
        nlohmann::json file = nlohmann::json::array();
        file.push_back(entry("zwo", "camera", 0));
        file.push_back(entry("qhy", "Camera", 0));  // the same type (any case) and number
        file.push_back(entry("qhy", "camera", 1));
        write_text(dir.path / "registered_devices.json", file.dump());
        std::vector<std::string> warnings;
        std::mutex warnings_mutex;
        struct LoggingRestore {
            alpacacore::logging::LogLevel level = alpacacore::logging::get_log_level();
            alpacacore::logging::LogSink sink = alpacacore::logging::get_log_sink();
            ~LoggingRestore() {
                alpacacore::logging::set_log_sink(sink);
                alpacacore::logging::set_log_level(level);
            }
        } logging_restore;
        alpacacore::logging::set_log_level(alpacacore::logging::LogLevel::Debug);
        alpacacore::logging::set_log_sink(
            [&](alpacacore::logging::LogLevel level, std::string_view, std::string_view message) {
                std::lock_guard<std::mutex> lock(warnings_mutex);
                if (level == alpacacore::logging::LogLevel::Warn) warnings.emplace_back(message);
            });
        PersistedConfigStore store(dir.path);
        const auto loaded = store.load(identity);
        EXPECT(loaded.parsed);
        EXPECT(warnings.size() == 1);
        EXPECT(!warnings.empty() && warnings[0].find("qhy") != std::string::npos &&
               warnings[0].find("zwo") != std::string::npos);
        EXPECT(loaded.entries.size() == 2);
        EXPECT(loaded.entries[0]["vendor"] == "zwo" && loaded.entries[1]["deviceNumber"] == 1);
        EXPECT(loaded.live_unique_ids.size() == 2);
        const auto snapshot = store.snapshot();
        EXPECT(snapshot.size() == 2 && snapshot[0]["vendor"] == "zwo");
        const auto on_disk = read_json(store.file());
        EXPECT(on_disk.is_array() && on_disk.size() == 2 && on_disk[0]["vendor"] == "zwo");
        // Entries with no readable key are not a pair: they stay.
        nlohmann::json odd =
            nlohmann::json::array({nlohmann::json{{"vendor", "zwo"}}, nlohmann::json{{"vendor", "qhy"}}});
        TempDir odd_dir;
        write_text(odd_dir.path / "registered_devices.json", odd.dump());
        PersistedConfigStore odd_store(odd_dir.path);
        EXPECT(odd_store.load(identity).entries.size() == 2);
    }
    {
        // case: a reserved uniqueId is not kept by a second call until the first releases it
        TempDir dir;
        PersistedConfigStore store(dir.path);
        (void)store.load(identity);
        nlohmann::json first = entry("zwo", "camera", 0);
        first["uniqueId"] = kUuid;
        nlohmann::json second = entry("zwo", "camera", 1);
        second["uniqueId"] = kUuid;
        {
            auto held = store.reserve_unique_id(first);
            EXPECT(held.has_value());
            EXPECT(first["uniqueId"] == kUuid);
            auto other = store.reserve_unique_id(second);
            EXPECT(other.has_value());
            EXPECT(second["uniqueId"] != kUuid);
        }
        nlohmann::json third = entry("zwo", "camera", 2);
        third["uniqueId"] = kUuid;
        EXPECT(store.reserve_unique_id(third).has_value());
        EXPECT(third["uniqueId"] == kUuid);  // released: free again

        // A stored entry holds its ID against another number; the entry a call replaces does not.
        store.add_or_replace(first);
        nlohmann::json elsewhere = entry("qhy", "camera", 5);
        elsewhere["uniqueId"] = kUuid;
        EXPECT(store.reserve_unique_id(elsewhere).has_value());
        EXPECT(elsewhere["uniqueId"] != kUuid);
        nlohmann::json same = entry("zwo", "camera", 0);
        same["uniqueId"] = kUuid;
        EXPECT(store.reserve_unique_id(same).has_value());
        EXPECT(same["uniqueId"] == kUuid);

        // A configure that replaces the entry with another vendor counts its uniqueId as free.
        nlohmann::json swapped = entry("qhy", "camera", 0);
        swapped["uniqueId"] = kUuid;
        EXPECT(store.reserve_unique_id(swapped).has_value());
        EXPECT(swapped["uniqueId"] == kUuid);

        // A device-number ID is never kept.
        nlohmann::json slot = entry("zwo", "camera", 7);
        slot["uniqueId"] = "ZWO_7";
        EXPECT(store.reserve_unique_id(slot).has_value());
        EXPECT(slot["uniqueId"] != "ZWO_7");
    }
    {
        // case: the entry cap counts adding calls in flight and never refuses a replacement
        TempDir dir;
        PersistedConfigStore store(dir.path);
        (void)store.load(identity);
        for (int n = 0; n < static_cast<int>(PersistedConfigStore::kMaxEntries) - 1; ++n) {
            store.add_or_replace(entry("zwo", "camera", n));
        }
        nlohmann::json last = entry("zwo", "camera", 1000);
        std::optional<PersistedConfigStore::Reservation> pending = store.reserve_unique_id(last);
        EXPECT(pending.has_value());
        nlohmann::json over = entry("zwo", "camera", 1001);
        EXPECT(!store.reserve_unique_id(over).has_value());
        EXPECT(!over.contains("uniqueId"));
        nlohmann::json replace = entry("zwo", "camera", 0);
        EXPECT(store.reserve_unique_id(replace).has_value());
        pending.reset();  // the adding call failed: its slot is free again
        EXPECT(store.reserve_unique_id(over).has_value());
    }
    std::cout << "All persisted config store tests passed.\n";
    return 0;
}
