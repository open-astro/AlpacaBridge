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

// Sexagesimal wire text shared by the LX200-family mount wrappers (OnStep, ZWO AM),
// issue #1757 slice 3: RA in hours, Dec, latitude and longitude in degrees. Pure
// functions: no I/O, no clock, no locale. The wrapper adds its command prefix and
// terminator ("':Sr' + format_hms(ra) + '#'"); pointing arithmetic stays in
// util/pointing.h.

#pragma once

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alpacacore::util {

/// A value split into whole sexagesimal fields. `negative` is the sign of the
/// value before rounding, so -0.1" splits as negative with every field zero.
struct SexagesimalFields {
    bool negative = false;
    long long whole = 0;
    int minutes = 0;
    int seconds = 0;
};

namespace sexagesimal_detail {

/// |value| rounded to the nearest whole `1 / units_per_whole` and split, sign kept apart.
inline SexagesimalFields split(double value, long long units_per_whole, bool with_seconds) {
    SexagesimalFields out;
    if (!std::isfinite(value)) {
        return out;
    }
    out.negative = value < 0.0;
    const long long total = std::llround(std::abs(value) * static_cast<double>(units_per_whole));
    out.whole = total / units_per_whole;
    const long long rest = total % units_per_whole;
    if (with_seconds) {
        out.minutes = static_cast<int>(rest / 60);
        out.seconds = static_cast<int>(rest % 60);
    } else {
        out.minutes = static_cast<int>(rest);
    }
    return out;
}

}  // namespace sexagesimal_detail

/// Hours wrapped into [0, 24) and rounded to the nearest whole second, so
/// 23:59:59.5 and any value that rounds up to 24 h read 00:00:00. A
/// non-finite value reads 00:00:00.
inline SexagesimalFields split_hms(double hours) {
    if (!std::isfinite(hours)) {
        return {};
    }
    double wrapped = std::fmod(hours, 24.0);
    if (wrapped < 0.0) {
        wrapped += 24.0;
    }
    SexagesimalFields out = sexagesimal_detail::split(wrapped, 3600, true);
    out.whole %= 24;
    out.negative = false;
    return out;
}

/// |degrees| rounded to the nearest whole arcsecond; the carry reaches the
/// degrees (89:59:59.5 reads 90:00:00). A non-finite value reads +0.
inline SexagesimalFields split_dms(double degrees) { return sexagesimal_detail::split(degrees, 3600, true); }

/// |degrees| rounded to the nearest whole arcminute; `seconds` is 0.
inline SexagesimalFields split_dm(double degrees) { return sexagesimal_detail::split(degrees, 60, false); }

/// "HH:MM:SS" (split_hms).
inline std::string format_hms(double hours) {
    const SexagesimalFields f = split_hms(hours);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%02lld:%02d:%02d", f.whole, f.minutes, f.seconds);
    return buf;
}

/// "sDD<separator>MM:SS" with the degrees zero padded to `degree_width`
/// digits and the sign always written (split_dms): "+45*30:15", "-00:00:30".
inline std::string format_dms(double degrees, int degree_width, char degree_separator) {
    const SexagesimalFields f = split_dms(degrees);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%c%0*lld%c%02d:%02d", f.negative ? '-' : '+', degree_width, f.whole,
                  degree_separator, f.minutes, f.seconds);
    return buf;
}

/// "sDD<separator>MM" with the degrees zero padded to `degree_width` digits
/// and the sign always written (split_dm): "+40*30", "-074*00".
inline std::string format_dm(double degrees, int degree_width, char degree_separator) {
    const SexagesimalFields f = split_dm(degrees);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%c%0*lld%c%02d", f.negative ? '-' : '+', degree_width, f.whole, degree_separator,
                  f.minutes);
    return buf;
}

/// The lenient LX200 reply parse (OnStep): an optional leading sign, then up
/// to three fields read as whole, minutes and seconds, split on any run of
/// characters that are neither a digit nor '.', so one parser reads "HH:MM:SS",
/// "HH:MM.T", "sDD*MM:SS", "sDD*MM'SS", "sDD*MM", "sDD*#", "sHH:MM" and a
/// fractional last field ("HH:MM:SS.SSSS"). Trailing '#', CR and LF are
/// dropped; fields past the third are ignored. nullopt when there is no
/// field, or a field that is not a number (".").
inline std::optional<double> parse_sexagesimal(std::string_view raw) {
    while (!raw.empty() && (raw.back() == '#' || raw.back() == '\r' || raw.back() == '\n')) {
        raw.remove_suffix(1);
    }
    if (raw.empty()) {
        return std::nullopt;
    }

    bool negative = false;
    if (raw.front() == '+' || raw.front() == '-') {
        negative = raw.front() == '-';
        raw.remove_prefix(1);
    }

    std::vector<double> parts;
    std::string current;
    auto flush_current = [&]() -> bool {
        if (current.empty()) {
            return true;
        }
        try {
            parts.push_back(std::stod(current));
        } catch (const std::exception&) {
            return false;
        }
        current.clear();
        return true;
    };
    for (const char c : raw) {
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            current.push_back(c);
        } else if (!flush_current()) {
            return std::nullopt;
        }
    }
    if (!flush_current() || parts.empty()) {
        return std::nullopt;
    }

    double value = parts[0];
    if (parts.size() > 1) {
        value += parts[1] / 60.0;
    }
    if (parts.size() > 2) {
        value += parts[2] / 3600.0;
    }
    return negative ? -value : value;
}

/// The strict hours parse (ZWO AM): "H:M:S" with 1-3 hour digits and 1-2
/// minute and second digits, minutes and seconds at most 59, whitespace
/// allowed around it and nothing else (no sign, no '#').
inline std::optional<double> parse_hms(std::string_view value) {
    auto is_space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!value.empty() && is_space(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_space(value.back())) {
        value.remove_suffix(1);
    }
    int fields[3] = {0, 0, 0};
    const std::size_t max_digits[3] = {3, 2, 2};
    std::size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
        if (i > 0) {
            if (pos >= value.size() || value[pos] != ':') {
                return std::nullopt;
            }
            ++pos;
        }
        std::size_t digits = 0;
        while (pos < value.size() && std::isdigit(static_cast<unsigned char>(value[pos]))) {
            if (++digits > max_digits[i]) {
                return std::nullopt;
            }
            fields[i] = fields[i] * 10 + (value[pos] - '0');
            ++pos;
        }
        if (digits == 0) {
            return std::nullopt;
        }
    }
    if (pos != value.size() || fields[1] > 59 || fields[2] > 59) {
        return std::nullopt;
    }
    return static_cast<double>(fields[0]) + static_cast<double>(fields[1]) / 60.0 +
           static_cast<double>(fields[2]) / 3600.0;
}

/// The strict angle parse (ZWO AM): an optional sign (required when
/// `require_sign`), then at least three runs of digits split on anything
/// else, read as degrees, minutes and seconds; runs past the third are
/// ignored. Minutes and seconds are at most 59. One of the three runs longer
/// than nine digits is refused rather than overflowed.
inline std::optional<double> parse_angle_triplet(std::string_view value, bool require_sign) {
    if (value.empty()) {
        return std::nullopt;
    }
    bool negative = false;
    if (value.front() == '+' || value.front() == '-') {
        negative = value.front() == '-';
        value.remove_prefix(1);
    } else if (require_sign) {
        return std::nullopt;
    }

    std::vector<int> parts;
    int current = 0;
    std::size_t digits = 0;
    auto end_run = [&]() -> bool {
        if (digits == 0) {
            return true;
        }
        if (digits > 9 && parts.size() < 3) {
            return false;
        }
        parts.push_back(current);
        current = 0;
        digits = 0;
        return true;
    };
    for (const char c : value) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            if (++digits <= 9) {
                current = current * 10 + (c - '0');
            }
        } else if (!end_run()) {
            return std::nullopt;
        }
    }
    if (!end_run()) {
        return std::nullopt;
    }
    if (parts.size() < 3 || parts[1] > 59 || parts[2] > 59) {
        return std::nullopt;
    }
    const double degrees =
        static_cast<double>(parts[0]) + static_cast<double>(parts[1]) / 60.0 + static_cast<double>(parts[2]) / 3600.0;
    return negative ? -degrees : degrees;
}

}  // namespace alpacacore::util
