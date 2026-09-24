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

// Pins the add-device page's user-visible names for the DSLR / mirrorless
// vendor.
//
// Run: node --test AlpacaHTTP/tests/web/vendor_labels.test.js
//
// The driver is built on libgphoto2, but that is an implementation detail:
// the page names the kind of camera, as INDI's device list does ("Canon
// DSLR", "Nikon DSLR"), and never the library or the gphoto2 command-line
// tool. The option's value stays "gphoto": it is the key the rest of the UI
// and the saved configuration use, and only the visible text is pinned here.

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');

const html = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'index.html'), 'utf8');

const OPTION = '<option value="gphoto">DSLR / Mirrorless</option>';
const HEADING = '<h3>DSLR / Mirrorless Configuration</h3>';

test('add-device option reads "DSLR / Mirrorless" and keeps the gphoto key', () => {
    assert.ok(html.includes(OPTION), 'the gphoto option must read "DSLR / Mirrorless"');
});

test('the gphoto configuration heading uses the same name', () => {
    assert.ok(html.includes(HEADING), 'the gphoto config heading must read "DSLR / Mirrorless Configuration"');
});

test('no visible option, heading, label or help text names gphoto2 or libgphoto2', () => {
    const visible = [...html.matchAll(/<(option|h[1-6]|label|small|p)[^>]*>([^<]*)</g)].map((m) => m[2]);
    const leaks = visible.filter((t) => /gphoto/i.test(t));
    assert.deepStrictEqual(leaks, [], `visible text names the library: ${JSON.stringify(leaks)}`);
});
