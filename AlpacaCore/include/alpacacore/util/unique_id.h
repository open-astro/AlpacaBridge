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

#include <string>
#include <string_view>

namespace alpacacore::util {

// The persisted UniqueID of a configured device (docs/decisions/0018): a random
// version 4 UUID in canonical lowercase form, "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx"
// with y one of 8, 9, a or b. Once stored, an ID is never changed, whatever its form.

// A fresh version 4 UUID from std::random_device.
std::string generate_unique_id();

// True when @p text is a version 4 UUID in the canonical lowercase form above.
bool is_unique_id(std::string_view text);

// True when @p text may stand as a stored UniqueID (decision 0018): a version 4 UUID, or
// 12 to 128 printable ASCII characters that are not a device-number ID
// ("<PREFIX>_<up to 5 decimal digits>", the slot IDs the drivers built before decision 0018). A kept
// value is never replaced, so ZWO_UID_<16 hex> and ZWO_SN_<serial> pass.
bool is_keepable_unique_id(std::string_view text);

}  // namespace alpacacore::util
