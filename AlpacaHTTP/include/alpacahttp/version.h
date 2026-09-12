// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#pragma once

// ALPACAHTTP_VERSION is defined by CMake from the VERSION file
// If this header is included without going through CMake, this will fail to compile
// which is intentional - the version should always come from the VERSION file
#ifndef ALPACAHTTP_VERSION
#error "ALPACAHTTP_VERSION must be defined by CMake. Ensure you're building through CMake."
#endif

#ifndef ALPACAHTTP_GIT_BRANCH
#define ALPACAHTTP_GIT_BRANCH "unknown"
#endif
#ifndef ALPACAHTTP_GIT_COMMIT
#define ALPACAHTTP_GIT_COMMIT "unknown"
#endif
#ifndef ALPACAHTTP_GIT_DIRTY
#define ALPACAHTTP_GIT_DIRTY 0
#endif
#ifndef ALPACAHTTP_GIT_IS_RELEASE
#define ALPACAHTTP_GIT_IS_RELEASE 0
#endif
#ifndef ALPACAHTTP_GIT_REMOTE_URL
#define ALPACAHTTP_GIT_REMOTE_URL ""
#endif

namespace alpacahttp {

inline constexpr const char* kVersion = ALPACAHTTP_VERSION;

// Identifies the actual checkout a build came from, independent of kVersion
// (which is a static release number from the VERSION file and does not
// change on a feature branch). Surfaced via /management/v1/buildinfo and
// shown in the web UI header so a dev build never reads as an official
// release.
inline constexpr const char* kGitBranch = ALPACAHTTP_GIT_BRANCH;
inline constexpr const char* kGitCommit = ALPACAHTTP_GIT_COMMIT;
inline constexpr bool kGitDirty = (ALPACAHTTP_GIT_DIRTY) != 0;
// True when HEAD sits exactly on a vX.Y.Z release tag -- the correct release
// check, since packaging checks out the tag (detached HEAD, not "main").
inline constexpr bool kGitIsRelease = (ALPACAHTTP_GIT_IS_RELEASE) != 0;
inline constexpr const char* kGitRemoteUrl = ALPACAHTTP_GIT_REMOTE_URL;

} // namespace alpacahttp
