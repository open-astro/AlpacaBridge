// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, Version 1 (SSPL v1).
// See the LICENSE file in this repository or the official license at:
// https://www.mongodb.com/legal/licensing/server-side-public-license
//
// If you use this program to provide a network-accessible service, appliance,
// or any commercial offering, you must comply
// with all SSPL v1 requirements.

#include <alpacacore/camera_driver.h>
#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/error_handling.h>

// CameraDriver::get_device_state() is defined inline in camera_driver.h so the
// CameraDriver vtable stays weak (no out-of-line key function), which keeps the
// vendor static libraries linkable without a base-library ordering dependency.
