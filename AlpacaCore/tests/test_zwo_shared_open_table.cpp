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

// SharedOpenTable (issue #1500): the open bookkeeping the ZWO EAF wrapper and its fake share. Several drivers
// may hold one SDK id; each real open gets a link that is never handed out again, every per-device call runs
// under the caller's link, and a replug that puts another unit behind a held id retires the old link, so an
// idle holder cannot drive the unit that took its id. SDK-free: the raw units below stand in for the library,
// so these cases build and run on every host.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/zwo/shared_open_table.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>

#include "catch2_compat.h"

namespace {

using alpacacore::AlpacaException;
using alpacacore::vendor::zwo::SharedOpenTable;
namespace AlpacaError = alpacacore::AlpacaError;

// The library under the table: which unit answers on each SDK id ("" = a unit that reports no serial, as
// EAFGetSerialNumber does with EAF_ERROR_NOT_SUPPORTED), and how often each raw call ran.
struct RawUnits {
    std::map<int, std::string> serial_at;
    int opens{0};
    int closes{0};
    int reads{0};
    int open_failures{0};  // the next N real opens fail
    int read_losses{0};    // the next N serial reads fail with a loss code
    int read_failures{0};  // the next N serial reads fail with a generic error
    bool close_fails{false};

    static bool take(int& next_n) { return next_n > 0 && next_n-- > 0; }

    SharedOpenTable::Ops ops() {
        SharedOpenTable::Ops ops;
        ops.real_open = [this](int) {
            ++opens;
            if (take(open_failures))
                throw AlpacaException("EAFOpen: EAF_ERROR_ERROR_STATE", AlpacaError::DriverException);
        };
        ops.real_close = [this](int) {
            ++closes;
            if (close_fails) throw AlpacaException("EAFClose: EAF_ERROR_REMOVED", AlpacaError::NotConnected);
        };
        ops.read_serial = [this](int id) -> std::string {
            ++reads;
            if (take(read_losses))
                throw AlpacaException("EAFGetSerialNumber: EAF_ERROR_CLOSED", AlpacaError::NotConnected);
            if (take(read_failures))
                throw AlpacaException("EAFGetSerialNumber: EAF_ERROR_GENERAL_ERROR", AlpacaError::DriverException);
            const auto& serial = serial_at.at(id);
            if (serial.empty())
                throw AlpacaException("EAFGetSerialNumber: EAF_ERROR_NOT_SUPPORTED", AlpacaError::NotImplemented);
            return serial;
        };
        return ops;
    }
};

int error_of(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const AlpacaException& e) {
        return e.error_code();
    }
    return 0;
}

// A per-device call as a holder makes it: under its link, checked in the same hold as the call.
bool call_reaches(SharedOpenTable& table, int id, std::uint64_t link) {
    bool reached = false;
    table.with_link(id, link, [&] { table.call(id, [&] { reached = true; }); });
    return reached;
}

}  // namespace

// Falsified by: a real open for every holder, reuse without a fresh serial read, or a real close at count 1.
TEST_CASE("ZWO shared open table - A second open with the same serial shares the real open", "[zwo][link]") {
    RawUnits units;
    units.serial_at = {{3, "EAF-SN-1"}};
    SharedOpenTable table(units.ops());

    const auto y = table.open(3);
    const auto x = table.open(3);
    CHECK(y.link != 0);
    CHECK(x.link == y.link);
    CHECK(y.serial == "EAF-SN-1");
    CHECK(x.serial == "EAF-SN-1");
    CHECK(units.opens == 1);
    CHECK(units.reads == 2);  // the shared open proves the unit again
    CHECK(table.open_count(3) == 2);

    table.close(3, x.link);
    CHECK(units.closes == 0);
    CHECK(call_reaches(table, 3, y.link));
    table.close(3, y.link);
    CHECK(units.closes == 1);
    CHECK(table.open_count(3) == 0);
}

// Falsified by: reusing a held open without comparing the serial read at its real open (B7).
TEST_CASE("ZWO shared open table - A different serial behind a held id retires its link", "[zwo][link]") {
    RawUnits units;
    units.serial_at = {{3, "EAF-SN-1"}};
    SharedOpenTable table(units.ops());
    const auto y = table.open(3);

    units.serial_at[3] = "EAF-SN-2";  // a replug put the other unit on id 3; Y is idle
    const auto x = table.open(3);
    CHECK(x.link != y.link);
    CHECK(x.serial == "EAF-SN-2");
    CHECK(units.closes == 1);  // the dead handle is closed when the entry dies (decision 0009 rule 3)
    CHECK(units.opens == 2);
    CHECK(table.open_count(3) == 1);

    bool reached = false;
    CHECK(error_of([&] { table.with_link(3, y.link, [&] { table.call(3, [&] { reached = true; }); }); }) ==
          AlpacaError::NotConnected);
    CHECK_FALSE(reached);  // refused before the call, under the same hold

    table.close(3, y.link);  // Y's Disconnect leaves X's open alone
    CHECK(table.open_count(3) == 1);
    CHECK(units.closes == 1);
    CHECK(call_reaches(table, 3, x.link));
}

// Falsified by: reusing the held open when the serial read on it fails with a loss code.
TEST_CASE("ZWO shared open table - A lost serial read behind a held id retires its link", "[zwo][link]") {
    RawUnits units;
    units.serial_at = {{3, "EAF-SN-1"}};
    SharedOpenTable table(units.ops());
    const auto y = table.open(3);

    units.read_losses = 1;  // the unit behind id 3 was replugged and answers CLOSED
    const auto x = table.open(3);
    CHECK(x.link != y.link);
    CHECK(x.serial == "EAF-SN-1");
    CHECK(units.opens == 2);
    CHECK(units.closes == 1);
    CHECK(table.open_count(3) == 1);
    CHECK(error_of([&] { (void)call_reaches(table, 3, y.link); }) == AlpacaError::NotConnected);
}

// Falsified by: a generic serial read failure on a held open reusing it, or retiring its link (B7b b).
TEST_CASE("ZWO shared open table - A generic serial read failure fails the open and keeps the held one",
          "[zwo][link]") {
    RawUnits units;
    units.serial_at = {{3, "EAF-SN-1"}};
    SharedOpenTable table(units.ops());
    const auto y = table.open(3);

    units.read_failures = 2;
    CHECK(error_of([&] { (void)table.open(3); }) == AlpacaError::DriverException);
    CHECK(error_of([&] { (void)table.open_candidate(3); }) == AlpacaError::DriverException);
    CHECK(table.open_count(3) == 1);
    CHECK(units.opens == 1);
    CHECK(units.closes == 0);
    CHECK(call_reaches(table, 3, y.link));
}

// Falsified by: a link derived from the id or reused after the entry closed.
TEST_CASE("ZWO shared open table - A link is never handed out twice", "[zwo][link]") {
    RawUnits units;
    units.serial_at = {{3, "EAF-SN-1"}};
    SharedOpenTable table(units.ops());
    const auto first = table.open(3);
    table.close(3, first.link);
    const auto second = table.open(3);

    CHECK(second.link != first.link);
    CHECK(error_of([&] { (void)call_reaches(table, 3, first.link); }) == AlpacaError::NotConnected);
    table.close(3, first.link);  // a close with the old link leaves the new open alone
    CHECK(table.open_count(3) == 1);
    CHECK(call_reaches(table, 3, second.link));
}

// Falsified by: a per-device call outside with_link, or under another id's link, reaching the SDK (B12).
TEST_CASE("ZWO shared open table - A per-device call with no link for its id is refused", "[zwo][link]") {
    RawUnits units;
    units.serial_at = {{3, "EAF-SN-1"}, {4, "EAF-SN-2"}};
    SharedOpenTable table(units.ops());
    const auto y = table.open(3);
    const auto x = table.open(4);

    bool reached = false;
    try {
        table.call(3, [&] { reached = true; });
        FAIL("a call outside with_link ran");
    } catch (const AlpacaException& e) {
        CHECK(e.error_code() == AlpacaError::DriverException);
        CHECK(std::string(e.what()).find("no link for id 3") != std::string::npos);
    }
    CHECK_FALSE(reached);

    CHECK(error_of([&] { table.with_link(4, x.link, [&] { table.call(3, [&] { reached = true; }); }); }) ==
          AlpacaError::DriverException);
    CHECK_FALSE(reached);

    CHECK(table.unchecked([] { return 7; }) == 7);  // enumeration and info need no link
    CHECK(call_reaches(table, 3, y.link));
}

// Falsified by: keeping the dead handle open after a per-device call fails with a loss code (decision 0009
// rule 3), serving the other holder of that entry, or treating a generic error as a loss.
TEST_CASE("ZWO shared open table - A loss code from a call closes the handle at once", "[zwo][link]") {
    RawUnits units;
    units.serial_at = {{3, "EAF-SN-1"}};
    SharedOpenTable table(units.ops());
    const auto y = table.open(3);
    const auto x = table.open(3);
    units.close_fails = true;  // EAFClose on a removed unit fails; the close is best effort

    CHECK(error_of([&] {
              table.with_link(3, y.link, [&] {
                  table.call(
                      3, [] { throw AlpacaException("EAFGetPosition: EAF_ERROR_REMOVED", AlpacaError::NotConnected); });
              });
          }) == AlpacaError::NotConnected);
    CHECK(units.closes == 1);
    CHECK(table.open_count(3) == 0);

    bool reached = false;
    CHECK(error_of([&] { table.with_link(3, x.link, [&] { table.call(3, [&] { reached = true; }); }); }) ==
          AlpacaError::NotConnected);
    CHECK_FALSE(reached);
    table.close(3, y.link);
    table.close(3, x.link);
    CHECK(units.closes == 1);

    units.close_fails = false;
    const auto z = table.open(3);
    CHECK(units.opens == 2);
    CHECK(error_of([&] {
              table.with_link(3, z.link, [&] {
                  table.call(3, [] { throw AlpacaException("EAFGetPosition: EAF_ERROR_GENERAL_ERROR"); });
              });
          }) == AlpacaError::DriverException);
    CHECK(units.closes == 1);
    CHECK(table.open_count(3) == 1);
    CHECK(call_reaches(table, 3, z.link));
}

// Falsified by: with_link not restoring the outer link on throw or return, or holding the table lock while its
// body runs.
TEST_CASE("ZWO shared open table - with_link restores the outer link and holds no lock across its body",
          "[zwo][link]") {
    RawUnits units;
    units.serial_at = {{3, "EAF-SN-1"}, {4, "EAF-SN-2"}, {5, "EAF-SN-3"}};
    SharedOpenTable table(units.ops());
    const auto a = table.open(3);
    const auto b = table.open(4);

    std::thread other;
    std::atomic<bool> other_opened{false};
    bool opened_while_body_ran = false;
    const int value = table.with_link(3, a.link, [&] {
        other = std::thread([&] {
            (void)table.open(5);
            other_opened = true;
        });
        for (int i = 0; i < 200 && !other_opened.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        opened_while_body_ran = other_opened.load();
        if (!opened_while_body_ran) return 0;  // the table lock is held: every call below would deadlock

        CHECK_THROWS(table.with_link(4, b.link, [] { throw std::runtime_error("body failed"); }));
        CHECK(error_of([&] { table.call(4, [] {}); }) == AlpacaError::DriverException);
        CHECK(error_of([&] { table.call(3, [] {}); }) == 0);
        return table.call(3, [] { return 42; });
    });
    other.join();

    CHECK(value == 42);
    CHECK(opened_while_body_ran);
    CHECK(error_of([&] { table.call(3, [] {}); }) == AlpacaError::DriverException);
}

// Falsified by: a candidate open sharing a held open whose unit reported no serial (B7b a): nothing proves
// which unit is behind that id now.
TEST_CASE("ZWO shared open table - A candidate open skips a held id whose unit has no serial", "[zwo][link]") {
    RawUnits units;
    units.serial_at = {{3, ""}, {4, "EAF-SN-2"}};
    SharedOpenTable table(units.ops());

    const auto y = table.open(3);
    CHECK(y.serial.empty());  // a unit with no serial still opens, as a connect accepts it today
    CHECK(table.open_count(3) == 1);

    CHECK_FALSE(table.open_candidate(3).has_value());
    CHECK(table.open_count(3) == 1);
    CHECK(units.opens == 1);

    const auto x = table.open_candidate(4);
    REQUIRE(x.has_value());
    CHECK(x->serial == "EAF-SN-2");
    const auto again = table.open_candidate(4);
    REQUIRE(again.has_value());
    CHECK(again->link == x->link);
    CHECK(table.open_count(4) == 2);
}

// Falsified by: a failed real open, or a unit gone during its first serial read, leaving an entry behind that
// a later open would reuse.
TEST_CASE("ZWO shared open table - A failed open leaves no entry behind", "[zwo][link]") {
    RawUnits units;
    units.serial_at = {{3, "EAF-SN-1"}};
    SharedOpenTable table(units.ops());

    units.open_failures = 1;
    CHECK(error_of([&] { (void)table.open(3); }) == AlpacaError::DriverException);
    CHECK(table.open_count(3) == 0);

    units.read_losses = 1;
    CHECK(error_of([&] { (void)table.open(3); }) == AlpacaError::NotConnected);
    CHECK(table.open_count(3) == 0);
    CHECK(units.closes == 1);  // the handle the vanished unit left is closed

    const auto y = table.open(3);
    CHECK(units.opens == 3);
    CHECK(table.open_count(3) == 1);
    CHECK(call_reaches(table, 3, y.link));
}
