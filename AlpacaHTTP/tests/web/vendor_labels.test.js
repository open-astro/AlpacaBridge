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

// Pins the add-device page's user-visible names for the libgphoto2 vendor.
//
// Run: node --test AlpacaHTTP/tests/web/vendor_labels.test.js
//
// The vendor's library is libgphoto2. "gphoto2" also names the gPhoto
// software suite and its command-line tool, which this server does not use,
// so the page must not present the device as "(gphoto2)". The option's
// value stays "gphoto": it is the key the rest of the UI and the saved
// configuration use, and only the visible text is being pinned here.

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');

const html = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'index.html'), 'utf8');

test('add-device option shows the libgphoto2 name and keeps the gphoto key', () => {
    assert.ok(
        html.includes('<option value="gphoto">DSLR / Mirrorless (libgphoto2)</option>'),
        'the gphoto option must read "DSLR / Mirrorless (libgphoto2)"'
    );
});

test('the gphoto configuration heading uses the same name', () => {
    assert.ok(
        html.includes('<h3>DSLR / Mirrorless (libgphoto2) Configuration</h3>'),
        'the gphoto config heading must read "DSLR / Mirrorless (libgphoto2) Configuration"'
    );
});

test('no visible text presents the vendor as "(gphoto2)"', () => {
    assert.ok(!html.includes('(gphoto2)'), 'index.html still shows "(gphoto2)"');
});
