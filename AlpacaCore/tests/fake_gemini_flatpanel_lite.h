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

// FakeGeminiFlatPanel answering as the Astro Flat Panel Cover Lite (light only, firmware 206). It is the same fake as
// fake_gemini_flatpanel.h under its own name, because kFakeConnectableRoster and the fake ConformU run list key a row
// on its fake header, so each panel firmware needs a header of its own to have a row.

#include "fake_gemini_flatpanel.h"

namespace alpacacore::test {

class FakeGeminiFlatPanelLite : public FakeGeminiFlatPanel {
public:
    FakeGeminiFlatPanelLite() : FakeGeminiFlatPanel(GeminiPanelFirmware::Lite) {}
};

}  // namespace alpacacore::test
