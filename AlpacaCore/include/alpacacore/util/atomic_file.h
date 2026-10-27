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

#include <filesystem>
#include <string_view>

namespace alpacacore::util {

// Replaces @p path with @p content so a crash or a full disk never leaves a
// partial file: writes "<path>.tmp" beside it, checks the stream after the write
// and the flush, fsyncs the file, renames it over @p path, then fsyncs the
// directory. An existing file's permission bits carry over. On any failure the
// temp file is removed, the previous file is untouched and std::runtime_error is
// thrown. Parent directories are not created.
void write_file_atomic(const std::filesystem::path& path, std::string_view content);

}  // namespace alpacacore::util
