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

// Pins the Celestron focus motor form (issue #896): the Celestron vendor is
// offered for Focuser, its port input has a vendor-prefixed name (a bare
// "portPath" collides in FormData with the hidden telescope block), and the
// edit and submit paths read and write that same field.
//
// Run: node --test AlpacaHTTP/tests/web/celestron_focuser.test.js

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const web = path.join(__dirname, '..', '..', 'web');
const html = fs.readFileSync(path.join(web, 'index.html'), 'utf8');
const app = fs.readFileSync(path.join(web, 'app.js'), 'utf8');

test('Celestron is offered for both Telescope and Focuser', () => {
    assert.ok(html.includes('<option value="celestron">Celestron</option>'));
    assert.ok(app.includes('const celestronAllowed = isTelescope || isFocuser;'));
    assert.ok(app.includes("if (!isTelescope && !isFocuser && vendorSelect.value === 'celestron') {"));
});

test('the focus motor port input has a unique vendor-prefixed name', () => {
    assert.match(html, /<input type="text" id="celestron-focuser-port-path" name="celestronFocuserPortPath"/);
    assert.equal((html.match(/name="celestronFocuserPortPath"/g) || []).length, 1);
});

test('the focus motor port is loaded on edit and sent on submit', () => {
    assert.ok(app.includes("setFormValue('celestron-focuser-port-path', config.portPath);"));
    assert.ok(app.includes("deviceData.portPath = (formData.get('celestronFocuserPortPath') || '').trim();"));
});

test('the Celestron sub-section follows Device Type', () => {
    assert.match(app, /function updateCelestronDeviceFields\(\)[\s\S]*?focuserSection\.style\.display = isFocuser \? 'block' : 'none';/);
    assert.match(app, /updateCelestronConnectionFields\(\);\s*updateCelestronDeviceFields\(\);/);
});
