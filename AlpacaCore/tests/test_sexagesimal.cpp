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

// Table tests for util/sexagesimal.h (#1757 slice 3), the wire text the OnStep
// and ZWO AM wrappers share.

#include <alpacacore/util/sexagesimal.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using Catch::Approx;
namespace util = alpacacore::util;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

double hms(double h, double m, double s) { return h + m / 60.0 + s / 3600.0; }

struct ParseRow {
    std::string text;
    std::optional<double> expected;  // nullopt: refused
};

void check_parse(const std::optional<double>& got, const ParseRow& row) {
    INFO("input \"" << row.text << "\"");
    if (!row.expected) {
        CHECK_FALSE(got.has_value());
        return;
    }
    REQUIRE(got.has_value());
    CHECK(*got == Approx(*row.expected).epsilon(0).margin(1e-9));
}

}  // namespace

TEST_CASE("Sexagesimal - hours text rounds to the second, carries into the hour and wraps at 24 h",
          "[sexagesimal][unit]") {
    struct Row {
        double hours;
        const char* text;
    };
    const std::vector<Row> rows = {
        {0.0, "00:00:00"},
        {12.5, "12:30:00"},
        {hms(1, 2, 3), "01:02:03"},
        {hms(10, 59, 59.95), "11:00:00"},  // 59.95 s carries into the minutes and the hour
        {hms(10, 59, 59.4), "10:59:59"},
        {hms(23, 59, 59.6), "00:00:00"},  // rounds up to 24 h, which wraps
        {24.0, "00:00:00"},
        {-1e-18, "00:00:00"},  // wraps to exactly 24.0 before the rounding
        {-1.0, "23:00:00"},
        {25.25, "01:15:00"},
        {kNaN, "00:00:00"},
        {kInf, "00:00:00"},
    };
    for (const Row& row : rows) {
        INFO("hours " << row.hours);
        CHECK(util::format_hms(row.hours) == row.text);
    }
}

TEST_CASE("Sexagesimal - degree text keeps the sign below one second and carries into the degrees",
          "[sexagesimal][unit]") {
    struct Row {
        double degrees;
        int width;
        char separator;
        const char* text;
    };
    const std::vector<Row> rows = {
        {hms(45, 30, 15), 2, '*', "+45*30:15"},
        {-hms(0, 0, 30), 2, ':', "-00:00:30"},  // negative zero degrees keeps its sign
        {-hms(0, 0, 30), 2, '*', "-00*00:30"},
        {-hms(0, 0, 0.1), 2, '*', "-00*00:00"},
        {-0.0, 2, '*', "+00*00:00"},
        {hms(89, 59, 59.5), 2, ':', "+90:00:00"},  // the carry reaches the degrees
        {-hms(12, 59, 59.95), 2, '*', "-13*00:00"},
        {74.0, 3, '*', "+074*00:00"},
        {-hms(122, 25, 9), 3, '*', "-122*25:09"},
        {kNaN, 2, '*', "+00*00:00"},
    };
    for (const Row& row : rows) {
        INFO("degrees " << row.degrees << " width " << row.width << " separator " << row.separator);
        CHECK(util::format_dms(row.degrees, row.width, row.separator) == row.text);
    }
}

TEST_CASE("Sexagesimal - degree-minute text rounds to the minute and carries into the degrees", "[sexagesimal][unit]") {
    struct Row {
        double degrees;
        int width;
        const char* text;
    };
    const std::vector<Row> rows = {
        {40.5, 2, "+40*30"},
        {-0.25, 2, "-00*15"},
        {hms(40, 29, 31), 2, "+40*30"},
        {-hms(74, 59, 36), 3, "-075*00"},  // 59.6' carries into the degrees
        {0.0, 3, "+000*00"},
        {kNaN, 2, "+00*00"},
    };
    for (const Row& row : rows) {
        INFO("degrees " << row.degrees << " width " << row.width);
        CHECK(util::format_dm(row.degrees, row.width, '*') == row.text);
    }
}

TEST_CASE("Sexagesimal - the lenient parse reads every OnStep reply form and refuses one with no number",
          "[sexagesimal][unit]") {
    const std::vector<ParseRow> rows = {
        {"12:00:00.0000#", 12.0},            // :GRa#
        {"01:02:03.5000#", hms(1, 2, 3.5)},  // fractional seconds
        {"+45*00:00.000#", 45.0},            // :GDe#
        {"-00*00:30#", -hms(0, 0, 30)},      // negative zero degrees
        {"180*00:00#", 180.0},               // :GZ#
        {"+45*30'15#", hms(45, 30, 15)},     // :GA# with seconds
        {"+40*30#", 40.5},                   // :Gt#
        {"+074*00#", 74.0},                  // :Gg#
        {"-10*#", -10.0},                    // :Gh#
        {"90*#", 90.0},                      // :Go#
        {"+05:00#", 5.0},                    // :GG# (OnStepX)
        {"-05#", -5.0},                      // :GG# (OnStep 4.x)
        {"12:34.5#", hms(12, 34.5, 0)},      // HH:MM.T
        {"10:20:30\r\n", hms(10, 20, 30)},   // CR/LF dropped
        {"10:20:30:40#", hms(10, 20, 30)},   // fields past the third ignored
        {"", std::nullopt},
        {"#", std::nullopt},
        {"+#", std::nullopt},
        {"?#", std::nullopt},
        {".#", std::nullopt},  // a field that is not a number
    };
    for (const ParseRow& row : rows) {
        check_parse(util::parse_sexagesimal(row.text), row);
    }
}

TEST_CASE("Sexagesimal - the strict hours parse takes three in-range fields and nothing else", "[sexagesimal][unit]") {
    const std::vector<ParseRow> rows = {
        {"12:34:56", hms(12, 34, 56)}, {" 01:02:03 ", hms(1, 2, 3)},  {"0:0:0", 0.0},
        {"123:00:00", 123.0},          {"23:59:59", hms(23, 59, 59)}, {"1234:00:00", std::nullopt},
        {"12:60:00", std::nullopt},    {"12:00:60", std::nullopt},    {"12:34", std::nullopt},
        {"12:34:567", std::nullopt},   {"12::56", std::nullopt},      {"12:34:56#", std::nullopt},
        {"+12:00:00", std::nullopt},   {"12:34:56:00", std::nullopt}, {"", std::nullopt},
    };
    for (const ParseRow& row : rows) {
        check_parse(util::parse_hms(row.text), row);
    }
}

TEST_CASE("Sexagesimal - the strict angle parse takes a sign when asked and three in-range fields",
          "[sexagesimal][unit]") {
    struct Row {
        ParseRow parse;
        bool require_sign;
    };
    const std::vector<Row> rows = {
        {{"+45*30:15", hms(45, 30, 15)}, true},
        {{"-00*00:30", -hms(0, 0, 30)}, true},  // negative zero degrees
        {{"-074*00:00", -74.0}, true},
        {{"+40:30:00", 40.5}, true},
        {{"180*00:00", 180.0}, false},
        {{"180*00:00", std::nullopt}, true},  // the sign is required
        {{"+40*30:00.000", 40.5}, true},      // runs past the third ignored
        {{"+45*30:15.12345678901", hms(45, 30, 15)}, true},
        {{"+45*30", std::nullopt}, true},
        {{"+45*60:00", std::nullopt}, true},
        {{"+45*30:60", std::nullopt}, true},
        {{"+1234567890*00:00", std::nullopt}, true},  // refused, not overflowed
        {{"+", std::nullopt}, true},
        {{"", std::nullopt}, false},
    };
    for (const Row& row : rows) {
        INFO("require_sign " << row.require_sign);
        check_parse(util::parse_angle_triplet(row.parse.text, row.require_sign), row.parse);
    }
}
