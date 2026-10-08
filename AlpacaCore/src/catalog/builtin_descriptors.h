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

// Declares each vendor's schema/factory registration functions
// (open-astro#664). No vendor header: this file is in the layering gate's
// catalog file set (scripts/check_layering.py), which must include none,
// ever. register_astroasis_schema() is defined in astroasis_schema.cpp
// (compiles in every build, also no vendor header); register_astroasis_factory()
// is defined in astroasis_catalog.cpp (vendor header allowed, compiled only
// under ALPACACORE_ENABLE_ASTROASIS). The Sky-Watcher pair (open-astro#744),
// WeeWX (open-astro#731), SVBONY, gphoto, Player One, Bisque, OnStep and Celestron follow the same split, e.g. WeeWX in
// weewx_schema.cpp and weewx_catalog.cpp. All are called only from
// builtin_catalog.cpp (the alpacacore_builtins library, open-astro#710).

#include <alpacacore/catalog/device_catalog.h>

namespace alpacacore::catalog {

void register_astroasis_schema(DeviceCatalog& catalog);
void register_astroasis_factory(DeviceCatalog& catalog);
void register_skywatcher_schema(DeviceCatalog& catalog);
void register_skywatcher_factory(DeviceCatalog& catalog);
void register_weewx_schema(DeviceCatalog& catalog);
void register_weewx_factory(DeviceCatalog& catalog);
void register_svbony_schema(DeviceCatalog& catalog);
void register_svbony_factory(DeviceCatalog& catalog);
void register_gphoto_schema(DeviceCatalog& catalog);
void register_gphoto_factory(DeviceCatalog& catalog);
void register_playerone_schema(DeviceCatalog& catalog);
void register_playerone_factory(DeviceCatalog& catalog);
void register_bisque_schema(DeviceCatalog& catalog);
void register_bisque_factory(DeviceCatalog& catalog);
void register_onstep_schema(DeviceCatalog& catalog);
void register_onstep_factory(DeviceCatalog& catalog);
void register_celestron_schema(DeviceCatalog& catalog);
void register_celestron_factory(DeviceCatalog& catalog);

}  // namespace alpacacore::catalog
