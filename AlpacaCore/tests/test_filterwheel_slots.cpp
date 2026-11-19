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

#include <alpacacore/util/filterwheel_slots.h>

#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "catch2_compat.h"

namespace {

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::AlpacaException;
using alpacacore::util::FilterWheelSlots;
namespace logging = alpacacore::logging;
using Names = std::vector<std::string>;
using Offsets = std::vector<int>;

// Captures WARN lines and restores the previous sink and level on every exit path.
struct WarnCapture {
    explicit WarnCapture(bool throw_on_warn = false)
        : previous_sink(logging::get_log_sink()), previous_level(logging::get_log_level()) {
        logging::set_log_level(logging::LogLevel::Info);
        logging::set_log_sink(
            [this, throw_on_warn](logging::LogLevel level, std::string_view component, std::string_view message) {
                if (level != logging::LogLevel::Warn) return;
                if (throw_on_warn) throw std::runtime_error("sink failed");
                lines.push_back(std::string(component) + ": " + std::string(message));
            });
    }
    ~WarnCapture() {
        logging::set_log_sink(previous_sink);
        logging::set_log_level(previous_level);
    }
    WarnCapture(const WarnCapture&) = delete;
    WarnCapture& operator=(const WarnCapture&) = delete;

    logging::LogSink previous_sink;
    logging::LogLevel previous_level;
    std::vector<std::string> lines;
};

int error_code_of(const std::function<void()>& fn, std::string* message = nullptr) {
    try {
        fn();
    } catch (const AlpacaException& e) {
        if (message) *message = e.what();
        return e.error_code();
    }
    return -1;
}

constexpr int kInvalidValue = AlpacaError::InvalidValue;

}  // namespace

// Falsified by: filterwheel_slots.h set_names()/set_offsets() calling require_length() with the count unknown.
TEST_CASE("FilterWheelSlots - unknown count accepts any length and does not expand",
          "[util][filterwheel-slots][unit]") {
    FilterWheelSlots slots;
    slots.set_names({"a", "b"});
    slots.set_offsets({1});
    CHECK(slots.names() == Names{"a", "b"});
    CHECK(slots.offsets() == Offsets{1});
    CHECK_FALSE(slots.slot_count().has_value());
    slots.set_names({"LRGBH"});
    CHECK(slots.names() == Names{"LRGBH"});
}

// Falsified by: filterwheel_slots.h set_names() calling fill_default_names() only inside `if (slot_count_)`.
TEST_CASE("FilterWheelSlots - unknown count still fills default names", "[util][filterwheel-slots][unit]") {
    FilterWheelSlots slots;
    slots.set_names({"L", "", "B"});
    CHECK(slots.names() == Names{"L", "Filter 2", "B"});
}

// Falsified by: filterwheel_slots.h publish_slot_count() dropping the warn_resize() call for focusOffsets.
TEST_CASE("FilterWheelSlots - publish resizes names and offsets with one warning each",
          "[util][filterwheel-slots][unit]") {
    WarnCapture capture;
    FilterWheelSlots slots;
    slots.set_names({"L", "R", ""});
    slots.set_offsets({7, 8});
    slots.publish_slot_count(5, "Tag");
    CHECK(slots.slot_count() == 5);
    CHECK(slots.names() == Names{"L", "R", "Filter 3", "Filter 4", "Filter 5"});
    CHECK(slots.offsets() == Offsets{7, 8, 0, 0, 0});
    REQUIRE(capture.lines.size() == 2);
    CHECK(capture.lines[0] ==
          "Tag: Configured filterNames count (3) does not match wheel slot count (5); resizing to match the wheel");
    CHECK(capture.lines[1] ==
          "Tag: Configured focusOffsets count (2) does not match wheel slot count (5); resizing to match the wheel");
}

// Falsified by: filterwheel_slots.h publish_slot_count() dropping its expand_shorthand() call.
TEST_CASE("FilterWheelSlots - publish expands a configured shorthand and keeps whole names",
          "[util][filterwheel-slots][unit]") {
    WarnCapture capture;
    FilterWheelSlots slots;
    slots.set_names({"LRGBH"});
    slots.publish_slot_count(5, "Tag");
    CHECK(slots.names() == Names{"L", "R", "G", "B", "H"});
    CHECK(slots.offsets() == Offsets{0, 0, 0, 0, 0});
    CHECK(capture.lines.empty());

    FilterWheelSlots clear;
    clear.set_names({"Clear"});
    clear.publish_slot_count(5, "Tag");
    CHECK(clear.names() == Names{"Clear", "Filter 2", "Filter 3", "Filter 4", "Filter 5"});

    FilterWheelSlots narrowband;
    narrowband.set_names({"Ha_NB"});
    narrowband.publish_slot_count(5, "Tag");
    CHECK(narrowband.names() == Names{"Ha_NB", "Filter 2", "Filter 3", "Filter 4", "Filter 5"});
}

// Falsified by: filterwheel_slots.h publish_slot_count() without the slot_count_.reset() in its `slots <= 0` branch
// (the earlier count of 3 stays and a 2-name write is InvalidValue).
TEST_CASE("FilterWheelSlots - publish of zero or a negative count leaves the count unknown and the values as they are",
          "[util][filterwheel-slots][unit]") {
    FilterWheelSlots slots;
    slots.set_names({"L", "R"});
    slots.set_offsets({1});
    slots.publish_slot_count(0, "Tag");
    slots.publish_slot_count(-1, "Tag");
    CHECK_FALSE(slots.slot_count().has_value());
    CHECK(slots.names() == Names{"L", "R"});
    CHECK(slots.offsets() == Offsets{1});

    FilterWheelSlots known;
    known.publish_slot_count(3, "Tag");
    known.publish_slot_count(0, "Tag");
    CHECK_FALSE(known.slot_count().has_value());
    CHECK(known.names() == Names{"Filter 1", "Filter 2", "Filter 3"});
    CHECK(error_code_of([&] { known.set_names({"A", "B"}); }) == -1);
}

// Falsified by: filterwheel_slots.h set_names() assigning names_ before require_length().
TEST_CASE("FilterWheelSlots - known count rejects a wrong length and keeps the stored values",
          "[util][filterwheel-slots][unit]") {
    FilterWheelSlots slots;
    slots.publish_slot_count(3, "Tag");
    slots.set_names({"L", "R", "G"});
    slots.set_offsets({1, 2, 3});
    std::string message;
    CHECK(error_code_of([&] { slots.set_names({"L", "R"}); }, &message) == kInvalidValue);
    CHECK(message == "Invalid names length: expected 3");
    CHECK(error_code_of([&] { slots.set_offsets({1, 2, 3, 4}); }, &message) == kInvalidValue);
    CHECK(message == "Invalid focusOffsets length: expected 3");
    CHECK(slots.names() == Names{"L", "R", "G"});
    CHECK(slots.offsets() == Offsets{1, 2, 3});
}

// Falsified by: filterwheel_slots.h expand_shorthand() without its lowercase check.
TEST_CASE("FilterWheelSlots - known count expands only an uppercase token of the slot count's length",
          "[util][filterwheel-slots][unit]") {
    FilterWheelSlots slots;
    slots.publish_slot_count(5, "Tag");
    slots.set_names({"LRGBH"});
    CHECK(slots.names() == Names{"L", "R", "G", "B", "H"});
    CHECK(error_code_of([&] { slots.set_names({"lrgbh"}); }) == kInvalidValue);
    CHECK(error_code_of([&] { slots.set_names({"LRGB"}); }) == kInvalidValue);
    CHECK(error_code_of([&] { slots.set_names({"LR,GB"}); }) == kInvalidValue);
    CHECK(slots.names() == Names{"L", "R", "G", "B", "H"});
}

// Falsified by: filterwheel_slots.h set_names() calling fill_default_names() only while the count is unknown.
TEST_CASE("FilterWheelSlots - known count fills default names", "[util][filterwheel-slots][unit]") {
    FilterWheelSlots slots;
    slots.publish_slot_count(3, "Tag");
    slots.set_names({"", "R", ""});
    CHECK(slots.names() == Names{"Filter 1", "R", "Filter 3"});
}

// Falsified by: filterwheel_slots.h seed_offsets() without its `offsets_.empty()` check.
TEST_CASE("FilterWheelSlots - seeds fill only empty fields and are resized at publish",
          "[util][filterwheel-slots][unit]") {
    WarnCapture capture;
    FilterWheelSlots slots;
    slots.seed_offsets({4, 5});
    slots.seed_names({"A", "B"});
    CHECK(slots.offsets() == Offsets{4, 5});
    CHECK(slots.names() == Names{"A", "B"});
    slots.seed_offsets({9, 9, 9});
    slots.seed_names({"X"});
    CHECK(slots.offsets() == Offsets{4, 5});
    CHECK(slots.names() == Names{"A", "B"});
    slots.publish_slot_count(3, "Tag");
    CHECK(slots.offsets() == Offsets{4, 5, 0});
    CHECK(slots.names() == Names{"A", "B", "Filter 3"});

    FilterWheelSlots empty_seed;
    empty_seed.seed_offsets({});
    empty_seed.seed_names({});
    CHECK(empty_seed.offsets().empty());
    CHECK(empty_seed.names().empty());

    FilterWheelSlots configured;
    configured.set_offsets({1});
    configured.set_names({"L"});
    configured.seed_offsets({7, 7});
    configured.seed_names({"P", "Q"});
    CHECK(configured.offsets() == Offsets{1});
    CHECK(configured.names() == Names{"L"});
}

// Falsified by: filterwheel_slots.h clear_slot_count() as an empty body.
TEST_CASE("FilterWheelSlots - clearing the count keeps the values and accepts any length again",
          "[util][filterwheel-slots][unit]") {
    FilterWheelSlots slots;
    slots.publish_slot_count(3, "Tag");
    slots.set_names({"L", "R", "G"});
    slots.set_offsets({1, 2, 3});
    slots.clear_slot_count();
    CHECK_FALSE(slots.slot_count().has_value());
    CHECK(slots.names() == Names{"L", "R", "G"});
    CHECK(slots.offsets() == Offsets{1, 2, 3});
    slots.set_names({"A", "B"});
    slots.set_offsets({5});
    CHECK(slots.names() == Names{"A", "B"});
    CHECK(slots.offsets() == Offsets{5});
}

// Falsified by: filterwheel_slots.h publish_slot_count() writing names_ in place instead of on a copy.
TEST_CASE("FilterWheelSlots - a throw during publish leaves the value unchanged", "[util][filterwheel-slots][unit]") {
    // Names are empty (filled without a warning), then the focusOffsets resize warning throws.
    FilterWheelSlots slots;
    slots.set_offsets({1, 2, 3});
    {
        WarnCapture failing(/*throw_on_warn=*/true);
        CHECK_THROWS_AS(slots.publish_slot_count(5, "Tag"), std::runtime_error);
    }
    CHECK_FALSE(slots.slot_count().has_value());
    CHECK(slots.names().empty());
    CHECK(slots.offsets() == Offsets{1, 2, 3});
}
