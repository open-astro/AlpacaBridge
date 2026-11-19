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

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace alpacacore::util {

/**
 * @brief A filter wheel's Names and FocusOffsets, and the slot count they are checked against (issue #1991).
 *
 * One rule for every wheel driver: while the slot count is known, names and offsets must have one entry per
 * slot and a lone shorthand token ("LRGB") expands to one name per slot; while it is unknown they are taken as
 * given (the config path runs before connect) and resized at the next publish_slot_count(). Empty names read
 * as "Filter N". A failed call changes nothing.
 *
 * Not thread-safe: the owning driver guards every call with its own short state lock (decision 0016 rule 1).
 * No I/O, no clock, not a base class (decision 0013 section 5).
 */
class FilterWheelSlots {
public:
    // Connect publishes the wheel's count: expands a configured shorthand, resizes names and offsets to the
    // count (one warning per resized field), fills default names, then stores the count. Commits only when
    // every step succeeded. A count <= 0 makes the count unknown and keeps names and offsets.
    void publish_slot_count(int slots, std::string_view log_tag) {
        if (slots <= 0) {
            slot_count_.reset();
            return;
        }
        const auto count = static_cast<std::size_t>(slots);
        std::vector<std::string> names = names_;
        std::vector<int> offsets = offsets_;
        expand_shorthand(names, count);
        if (names.empty()) {
            names.assign(count, std::string());
        } else if (names.size() != count) {
            warn_resize(log_tag, "filterNames", names.size(), count);
            names.resize(count);
        }
        fill_default_names(names);
        if (offsets.empty()) {
            offsets.assign(count, 0);
        } else if (offsets.size() != count) {
            warn_resize(log_tag, "focusOffsets", offsets.size(), count);
            offsets.resize(count, 0);
        }
        names_ = std::move(names);
        offsets_ = std::move(offsets);
        slot_count_ = slots;
    }

    // Disconnect: the count is unknown again; names and offsets stay.
    void clear_slot_count() { slot_count_.reset(); }

    void set_names(std::vector<std::string> names) {
        if (slot_count_) {
            const int count = *slot_count_;
            expand_shorthand(names, static_cast<std::size_t>(count));
            require_length(names.size(), count, "names");
        }
        fill_default_names(names);
        names_ = std::move(names);
    }

    void set_offsets(std::vector<int> offsets) {
        if (slot_count_) {
            require_length(offsets.size(), *slot_count_, "focusOffsets");
        }
        offsets_ = std::move(offsets);
    }

    // Values stored on the wheel fill a field only while it is empty: config, or a client write that landed
    // while the driver read the wheel, wins. The next publish_slot_count() resizes them.
    void seed_names(std::vector<std::string> from_device) {
        if (names_.empty() && !from_device.empty()) {
            names_ = std::move(from_device);
        }
    }

    void seed_offsets(std::vector<int> from_device) {
        if (offsets_.empty() && !from_device.empty()) {
            offsets_ = std::move(from_device);
        }
    }

    const std::vector<std::string>& names() const { return names_; }
    const std::vector<int>& offsets() const { return offsets_; }
    std::optional<int> slot_count() const { return slot_count_; }

private:
    // A lone token of exactly `count` characters with no lowercase letter and no delimiter is a shorthand code;
    // "Clear" or "Ha_NB" stay whole. Mirrors parseFilterNamesInput in AlpacaHTTP/web/app.js.
    static void expand_shorthand(std::vector<std::string>& names, std::size_t count) {
        if (names.size() != 1) {
            return;
        }
        const std::string& token = names[0];
        if (token.size() != count || token.find_first_of("abcdefghijklmnopqrstuvwxyz") != std::string::npos ||
            token.find_first_of(",; \t") != std::string::npos) {
            return;
        }
        std::vector<std::string> expanded;
        expanded.reserve(count);
        for (char ch : token) {
            expanded.emplace_back(1, ch);
        }
        names = std::move(expanded);
    }

    static void fill_default_names(std::vector<std::string>& names) {
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (names[i].empty()) {
                names[i] = "Filter " + std::to_string(i + 1);
            }
        }
    }

    static void warn_resize(std::string_view log_tag, std::string_view field, std::size_t configured,
                            std::size_t count) {
        ALPACA_LOG_WARN(log_tag, "Configured " + std::string(field) + " count (" + std::to_string(configured) +
                                     ") does not match wheel slot count (" + std::to_string(count) +
                                     "); resizing to match the wheel");
    }

    static void require_length(std::size_t size, int count, std::string_view field) {
        if (size != static_cast<std::size_t>(count)) {
            throw AlpacaException("Invalid " + std::string(field) + " length: expected " + std::to_string(count),
                                  AlpacaError::InvalidValue);
        }
    }

    std::vector<std::string> names_;
    std::vector<int> offsets_;
    std::optional<int> slot_count_;
};

}  // namespace alpacacore::util
