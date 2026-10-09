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

#include <alpacacore/catalog/builtin_catalog.h>

#include "builtin_descriptors.h"

namespace alpacacore::catalog {

void register_builtin_schemas(DeviceCatalog& catalog) {
    register_astroasis_schema(catalog);
    register_skywatcher_schema(catalog);
    register_weewx_schema(catalog);
    register_svbony_schema(catalog);
    register_gphoto_schema(catalog);
    register_playerone_schema(catalog);
    register_bisque_schema(catalog);
    register_onstep_schema(catalog);
    register_celestron_schema(catalog);
    register_synscan_schema(catalog);
}

void register_builtin_factories([[maybe_unused]] DeviceCatalog& catalog) {
#ifdef ALPACACORE_ENABLE_ASTROASIS
    register_astroasis_factory(catalog);
#endif
#ifdef ALPACACORE_ENABLE_SKYWATCHER
    register_skywatcher_factory(catalog);
#endif
#ifdef ALPACACORE_ENABLE_WEEWX
    register_weewx_factory(catalog);
#endif
#ifdef ALPACACORE_ENABLE_SVBONY
    register_svbony_factory(catalog);
#endif
#ifdef ALPACACORE_ENABLE_GPHOTO
    register_gphoto_factory(catalog);
#endif
#ifdef ALPACACORE_ENABLE_PLAYERONE
    register_playerone_factory(catalog);
#endif
#ifdef ALPACACORE_ENABLE_BISQUE
    register_bisque_factory(catalog);
#endif
#ifdef ALPACACORE_ENABLE_ONSTEP
    register_onstep_factory(catalog);
#endif
#ifdef ALPACACORE_ENABLE_CELESTRON
    register_celestron_factory(catalog);
#endif
#ifdef ALPACACORE_ENABLE_SYNSCAN
    register_synscan_factory(catalog);
#endif
}

}  // namespace alpacacore::catalog
