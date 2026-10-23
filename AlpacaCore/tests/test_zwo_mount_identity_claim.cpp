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

#include <alpacacore/vendor/zwo/zwo_mount_identity.h>

#include <barrier>
#include <initializer_list>
#include <string>
#include <thread>

#include "catch2_compat.h"

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::AlpacaException;
using alpacacore::vendor::zwo::MountIdentityClaim;

namespace {
const std::string serial_a = "ABCDEF000001";
const std::string serial_b = "ABCDEF000002";
}  // namespace

TEST_CASE("ZWO mount identity claim refuses duplicates involving Auto", "[zwo][identity][issue1523]") {
    for (bool first_auto : {false, true}) {
        for (bool second_auto : {false, true}) {
            MountIdentityClaim first;
            MountIdentityClaim second;
            first.claim(serial_a, first_auto, 41);
            if (first_auto || second_auto) {
                CHECK_THROWS_AS(second.claim(serial_a, second_auto, 42), AlpacaException);
            } else {
                CHECK_NOTHROW(second.claim(serial_a, second_auto, 42));
                MountIdentityClaim automatic;
                first.reset();
                CHECK_THROWS_AS(automatic.claim(serial_a, true, 43), AlpacaException);
            }
        }
    }
}

TEST_CASE("ZWO mount identity claim allows distinct same-family serials", "[zwo][identity][issue1523]") {
    MountIdentityClaim first;
    MountIdentityClaim second;
    first.claim(serial_a, true, 41);
    CHECK_NOTHROW(second.claim(serial_b, true, 42));
}

TEST_CASE("ZWO mount identity claim reset releases only its owner", "[zwo][identity][issue1523]") {
    MountIdentityClaim first;
    MountIdentityClaim second;
    MountIdentityClaim next;
    first.claim(serial_a, true, 41);
    second.claim(serial_b, true, 42);
    const MountIdentityClaim& const_first = first;
    const_first.reset();
    const_first.reset();
    CHECK_NOTHROW(next.claim(serial_a, true, 43));
    CHECK_THROWS_AS(first.claim(serial_b, true, 41), AlpacaException);
}

TEST_CASE("ZWO mount identity claim destruction releases reservation", "[zwo][identity][issue1523]") {
    MountIdentityClaim next;
    {
        MountIdentityClaim first;
        first.claim(serial_a, true, 41);
    }
    CHECK_NOTHROW(next.claim(serial_a, true, 42));
}

TEST_CASE("ZWO mount identity claim replaces itself atomically", "[zwo][identity][issue1523]") {
    MountIdentityClaim first;
    MountIdentityClaim second;
    first.claim(serial_a, true, 41);
    CHECK_NOTHROW(first.claim(serial_a, true, 41));
    CHECK_NOTHROW(first.claim(serial_b, true, 41));
    CHECK_NOTHROW(second.claim(serial_a, true, 42));
    CHECK_THROWS_AS(first.claim(serial_a, true, 41), AlpacaException);
    MountIdentityClaim third;
    CHECK_THROWS_AS(third.claim(serial_b, true, 43), AlpacaException);
}

TEST_CASE("ZWO mount identity claim refusal names holder without serial", "[zwo][identity][issue1523]") {
    MountIdentityClaim first;
    MountIdentityClaim second;
    first.claim(serial_a, true, 41);
    try {
        second.claim(serial_a, false, 42);
        FAIL("duplicate claim was accepted");
    } catch (const AlpacaException& error) {
        CHECK(error.error_code() == AlpacaError::DriverException);
        const std::string message = error.what();
        CHECK(message.find("ZWO telescope device number 41") != std::string::npos);
        CHECK(message.find("explicit, distinct endpoints") != std::string::npos);
        CHECK(message.find(serial_a) == std::string::npos);
    }
}

TEST_CASE("ZWO mount identity claim concurrent Auto collisions have one winner",
          "[zwo][identity][issue1523][stress-guard]") {
    for (bool first_auto : {false, true}) {
        for (bool second_auto : {false, true}) {
            if (!first_auto && !second_auto) continue;
            for (int iteration = 0; iteration < 16; ++iteration) {
                std::barrier start(2);
                std::barrier attempted(2);
                bool accepted[2] = {false, false};
                bool driver_error[2] = {false, false};
                auto attempt = [&](int index, bool automatic) {
                    MountIdentityClaim claim;
                    start.arrive_and_wait();
                    try {
                        claim.claim(serial_a, automatic, 41 + index);
                        accepted[index] = true;
                    } catch (const AlpacaException& error) {
                        driver_error[index] = error.error_code() == AlpacaError::DriverException;
                    }
                    // Keep the winner held until the other attempt has finished.
                    attempted.arrive_and_wait();
                };
                std::jthread first(attempt, 0, first_auto);
                std::jthread second(attempt, 1, second_auto);
                first.join();
                second.join();
                CHECK(accepted[0] != accepted[1]);
                CHECK(driver_error[0] != driver_error[1]);
            }
        }
    }
}
