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

#include <alpacacore/camera_driver.h>
#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>

// CameraDriver::get_device_state() is defined inline in camera_driver.h so the
// CameraDriver vtable stays weak (no out-of-line key function), which keeps the
// vendor static libraries linkable without a base-library ordering dependency.
